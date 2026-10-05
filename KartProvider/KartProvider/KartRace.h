#ifndef KARTPROVIDER_KARTRACE_H__
#define KARTPROVIDER_KARTRACE_H__

#include "../../../core_defs.h"
#include "../../../ontology.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

/*
 * ── KartRace: karts on a track, one per player, between runtimes ─────────
 *
 * THE RECORD IS THE RACE, as it is the game in chess (ChessGame's header
 * note). Every runtime holds its own copy of the world -- the same course
 * script built it -- and steps it from the record alone:
 *
 *   join               the author takes the next free kart, on the grid
 *   drive <t> <s>      the author's pedal and wheel: t, s in -1 0 1
 *   tick <n>           every kart driven, the world stepped, up to tick n
 *   leave <who>        the host's: that kart back in the pit
 *
 * Lines apply in the record's order, and a tick is a line, so where an input
 * falls between two ticks is the record's to say and the same everywhere.
 * The world is Scene3D marked driven (SetDriven): no picture steps it, so the
 * wall clock and each runtime's own eye change nothing; the rows are the
 * fixed-point Causal rows, so two runtimes fed the same lines are the same
 * race (etcs_causal_constraints.md §2). Lockstep, with the record as the
 * lock: nobody negotiates anything.
 *
 * ONE RACE JUDGES (the host's). A guest's keys go to the host's proposals
 * ledger (`race.Emit() -> proposals.Take()`, authored by its link); the judge
 * applies each line it takes and appends it to the record. The host's own
 * clock is the race's: its frame edge steps the world in real time and puts
 * `tick` lines into the record in pairs, and always before any other line,
 * so a line lands at the tick the host applied it at. A guest follows the
 * record (`record.Follow(0) -> race.Absorb()`): it sees the race one trip
 * behind the host, and its own keys a round trip late. Alone (Practice) the
 * race is its own judge with no record.
 *
 * A RACE THAT DIFFERS is reported, not repaired (ChessShare's rule): every
 * kSnapEvery ticks each runtime hashes the race -- the karts' rows and the
 * race's own state -- and advertises it with its presence; a guest whose hash
 * at a tick is not the host's at that tick says so.
 *
 * THE KART IS A BICYCLE. W and S are the pedal: speed along the heading
 * grows to a top speed, brakes, reverses. A and D turn the WHEELS, which ease
 * to a lock and back; the heading turns by speed x wheel angle / wheelbase,
 * so a kart standing still does not turn and one reversing turns the other
 * way. What is left of the velocity across the heading is the tyres' to take
 * away (grip). The pedal's work is an Impulse (energy in); what the brakes,
 * the drag and the grip take is SetVelocity's (heat). All of it in Fixed, on
 * the rows, between two steps: the same arithmetic on every runtime.
 *
 * A KART IS SOLID -- a sphere to everything (etcs_causal_constraints.md §13),
 * drawn as a body on four wheels -- so karts bump each other and the walls
 * with the family's own contact. The wheels are drawn things this race puts
 * at the body's corners after every step, the front two turned by the wheel
 * angle (wheelsLocked). The world has no field: the track is flat and the
 * karts stay at their height.
 */
class KartRace : public AnimatedBase<KartRace>,
                 public DeletableBase<KartRace>
{
public:
    WIRE_TYPE_IDENTITY(KartRace);

    KartRace() = default;
    bool DeleteConcrete() override { Leave(); return true; }

    bool Create() { this->addTag("active"); return true; }

    void BindWorld(ETCS::RID r)  { std::lock_guard<std::mutex> g(m_mu); m_world = r; }
    void BindCamera(ETCS::RID r) { std::lock_guard<std::mutex> g(m_mu); m_camera = r; m_eye_set = false; }
    void BindCard(ETCS::RID r)   { std::lock_guard<std::mutex> g(m_mu); m_card = r; m_card_text.clear(); }

    // A kart for the pool, in the order the course makes them, with its four
    // wheels (front left, front right, rear left, rear right): its colour is
    // the slot's, and it waits in the pit until someone joins.
    void AddKart(ETCS::RID body, ETCS::RID fl, ETCS::RID fr, ETCS::RID rl, ETCS::RID rr)
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (!body || m_slots.size() >= kMaxKarts) return;
        Slot s;
        s.body = body;
        s.wheels[0] = fl; s.wheels[1] = fr; s.wheels[2] = rl; s.wheels[3] = rr;
        const float* c = kColors[m_slots.size() % kMaxKarts];
        verb(body, "SetColor", f4(c[0], c[1], c[2], 1.0f));
        if (Causal_* k = causal(body))
        {
            std::lock_guard<std::recursive_mutex> lk(k->TreeMutex());
            s.pristine = k->RowsUnder();   // what "as made" means, for every restart
        }
        m_slots.push_back(s);
        pitLocked(m_slots.size() - 1);
        // Practice asked for before the course had made the karts (a page is
        // quicker than its boot): the driver takes the first one made.
        if (!m_me.empty() && !m_session && slotOf(m_me) < 0) takeLocked(m_me, "join");
    }

    // ── sessions ──────────────────────────────────────────────────────────

    // Alone: this race is its own judge, nothing recorded.
    void Practice(const std::string& name)
    {
        Leave();
        std::lock_guard<std::mutex> g(m_mu);
        restartLocked();
        m_me = name; m_owner = name; m_judge = true;
        takeLocked(m_me, "join");
    }
    // Host: the record is here and this race judges it; the hall (a Lobby)
    // lists the race as `kart <id>` for as long as it lasts.
    bool Host(ETCS::RID record, ETCS::RID proposals, ETCS::RID presence, const std::string& name,
              ETCS::RID hall, const std::string& id)
    {
        if (name.empty() || !record) return false;
        Leave();
        {
            std::lock_guard<std::mutex> g(m_mu);
            restartLocked();
            m_me = name; m_owner = name; m_judge = true; m_session = true;
            m_record = record; m_proposals = proposals; m_presence = presence; m_hall = hall; m_id = id;
            takeLocked(m_me, "join");
        }
        ETCS_LOG("KartRace", "hosting a race as " << name);
        Tick();
        return true;
    }
    // Join: this race follows the host's record from its first line, and
    // asks for a kart.
    bool Join(ETCS::RID presence, const std::string& name, const std::string& owner)
    {
        if (name.empty()) return false;
        Leave();
        std::lock_guard<std::mutex> g(m_mu);
        restartLocked();
        m_me = name; m_owner = owner; m_judge = false; m_session = true; m_presence = presence;
        m_outbox.push_back("join");
        ETCS_LOG("KartRace", "joined " << owner << "'s race as " << name);
        return true;
    }
    void Leave()
    {
        ETCS::RID hall = 0;
        std::string me;
        {
            std::lock_guard<std::mutex> g(m_mu);
            if (m_me.empty()) return;
            if (m_session && m_judge) { hall = m_hall; me = m_me; }
            m_me.clear(); m_owner.clear(); m_judge = false; m_session = false;
            m_record = m_proposals = m_presence = m_hall = 0; m_id.clear();
            m_outbox.clear(); m_here.clear(); m_sent.clear(); m_advertised.clear(); m_note.clear();
        }
        if (hall) { ETCS::Buffer a; a.writeString(me.c_str()); callOn(hall, "Lobby.Withdraw", a); }
    }

    /*
     * PRESENCE AND THE HALL, each when it changed: "<name> kart <tick> <hash>"
     * -- this race's latest snapshot -- and, on the host, "<name> kart <id>
     * <drivers> <karts>" in the hall. Calls go out with no lock held: a
     * guest's presence is a surface of the host's, so an advert is a trip.
     */
    void Tick()
    {
        std::string line, ad;
        ETCS::RID presence = 0, hall = 0;
        {
            std::lock_guard<std::mutex> g(m_mu);
            if (!m_session || m_me.empty()) return;
            presence = m_presence;
            const std::string mine = m_me + " kart " + std::to_string(m_snap_tick) + " " + m_snap_hash;
            if (mine != m_sent) { m_sent = mine; line = mine; }
            if (m_judge && m_hall)
            {
                const std::string a = m_me + " kart " + (m_id.empty() ? "-" : m_id) + " "
                                    + std::to_string(driversLocked()) + " " + std::to_string(m_slots.size());
                if (a != m_advertised) { m_advertised = a; ad = a; hall = m_hall; }
            }
        }
        if (!line.empty() && presence) { ETCS::Buffer a; a.writeString(line.c_str()); callOn(presence, "Lobby.Advertise", a); }
        if (!ad.empty() && hall)       { ETCS::Buffer a; a.writeString(ad.c_str());   callOn(hall, "Lobby.Advertise", a); }
    }

    // ── the streams ──────────────────────────────────────────────────────

    // Emit's queue: a follower's next proposal.
    bool nextEmit(std::string& out)
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (m_outbox.empty()) return false;
        out = std::move(m_outbox.front());
        m_outbox.pop_front();
        return true;
    }
    // A line of the record, followed: "<seq> <author> <line>".
    void Absorb(const std::string& msg)
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (m_judge || msg.compare(0, 2, "~ ") == 0) return;
        uint64_t seq = 0; std::string author, line;
        if (!parseLine(msg, seq, author, line)) return;
        if (!applyLocked(author, line) && line.compare(0, 4, "tick") != 0)
            ETCS_LOG("KartRace", "line " << seq << " (" << author << " " << line << ") refused here -- this race is not the record's.");
        m_seq = seq + 1;
    }
    // The record ended under a guest still in the race: the host closed it
    // (or its link went). The world stays where the last line left it.
    void RecordEnded()
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (m_session && !m_judge) noteLocked("the race is over -- " + m_owner + " closed it");
    }
    // A proposal, judged: applied as its author and recorded if taken. The
    // proposals ledger is checkpointed behind the judge.
    void JudgeLine(const std::string& msg)
    {
        ETCS::RID proposals = 0; uint64_t seq = 0;
        {
            std::lock_guard<std::mutex> g(m_mu);
            if (!m_judge || !m_session || msg.compare(0, 2, "~ ") == 0) return;
            std::string author, line;
            if (!parseLine(msg, seq, author, line) || author.empty()) return;
            // A guest proposes its own seat and its own wheel, nothing else.
            if (line == "join" || line.compare(0, 6, "drive ") == 0) takeLocked(author, line);
            proposals = m_proposals;
        }
        if (Record_* p = recordOf(proposals)) p->Checkpoint(seq + 1);
    }
    // The presence listing, whole, on every change: arrivals and departures,
    // a departed driver's kart back to the pit (the host's line), and this
    // race's snapshots against the host's.
    void Roster(const std::string& listing)
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (!m_session) return;
        std::map<std::string, std::pair<uint64_t, std::string>> now;
        std::istringstream in(listing);
        std::string row;
        while (std::getline(in, row))
        {
            std::istringstream ls(row);
            std::string who, kind, hash; uint64_t tick = 0;
            if (!(ls >> who >> kind >> tick >> hash) || kind != "kart") continue;
            now[who] = { tick, hash };
        }
        for (auto& [who, at] : now) if (!m_here.count(who) && who != m_me) noteLocked(who + " is here");
        for (auto& [who, at] : m_here)
            if (!now.count(who))
            {
                noteLocked(who + " left");
                if (m_judge && slotOf(who) >= 0) takeLocked(m_me, "leave " + who);
            }
        m_here.clear();
        for (auto& [who, at] : now) m_here[who] = at;
        // The host's snapshot against this race's own at the same tick.
        if (!m_judge && now.count(m_owner))
        {
            const auto& host = now[m_owner];
            for (auto& [t, h] : m_snaps)
                if (t == host.first && host.first && h != host.second && m_differed_at != t)
                {
                    m_differed_at = t;
                    ETCS_LOG("KartRace", "this race (" << h << ") is not the host's (" << host.second << ") at tick " << t);
                    noteLocked("the race differs from the host's at tick " + std::to_string(t));
                }
        }
    }

    // A key from the window (GLFW codes): W/S or up/down the pedal, A/D or
    // left/right the wheel. A change of either is a line.
    void Key(uint16_t key, bool down)
    {
        int bit = -1;
        if (key == 'W' || key == 265) bit = 0;
        else if (key == 'S' || key == 264) bit = 1;
        else if (key == 'A' || key == 263) bit = 2;
        else if (key == 'D' || key == 262) bit = 3;
        if (bit < 0) return;
        std::lock_guard<std::mutex> g(m_mu);
        if (down) m_keys |= (1u << bit); else m_keys &= ~(1u << bit);
        const int t = ((m_keys & 1) ? 1 : 0) - ((m_keys & 2) ? 1 : 0);
        const int s = ((m_keys & 4) ? 1 : 0) - ((m_keys & 8) ? 1 : 0);
        if (t == m_sent_t && s == m_sent_s) return;
        m_sent_t = t; m_sent_s = s;
        const std::string line = "drive " + std::to_string(t) + " " + std::to_string(s);
        if (m_me.empty()) return;
        if (m_judge) takeLocked(m_me, line);
        else m_outbox.push_back(line);
    }

    // ── reads ────────────────────────────────────────────────────────────

    // "<tick> <seq> <mode> <me> <slot>": mode host | guest | practice | idle.
    std::string Status()
    {
        std::lock_guard<std::mutex> g(m_mu);
        const char* mode = m_me.empty() ? "idle" : !m_session ? "practice" : m_judge ? "host" : "guest";
        return std::to_string(m_tick) + " " + std::to_string(m_seq) + " " + mode + " "
             + (m_me.empty() ? "-" : m_me) + " " + std::to_string(slotOf(m_me));
    }
    // One line per kart with a driver: "<slot> <driver> <laps> <last s> <best s> <speed>".
    std::string Standings()
    {
        std::lock_guard<std::mutex> g(m_mu);
        std::string out;
        for (size_t i = 0; i < m_slots.size(); ++i)
        {
            const Slot& s = m_slots[i];
            if (s.driver.empty()) continue;
            out += std::to_string(i) + " " + s.driver + " " + std::to_string(s.laps) + " "
                 + secs(s.last) + " " + secs(s.best) + " " + fmt1(speedOf(i)) + "\n";
        }
        return out;
    }
    // "<tick> <hash>": the race now (Hash) -- what a test compares.
    std::string Hash()
    {
        std::lock_guard<std::mutex> g(m_mu);
        return std::to_string(m_tick) + " " + hashLocked();
    }
    // The latest snapshot: "<tick> <hash>".
    std::string Snapshot()
    {
        std::lock_guard<std::mutex> g(m_mu);
        return std::to_string(m_snap_tick) + " " + m_snap_hash;
    }
    // What the race said last (a join, a departure, a difference).
    std::string Note() { std::lock_guard<std::mutex> g(m_mu); return m_note; }
    // Where a driver's kart is: "<slot> <x> <z> <heading x> <heading z> <speed>",
    // or "-" for nobody driving under that name.
    std::string Kart(const std::string& who)
    {
        std::lock_guard<std::mutex> g(m_mu);
        const int i = slotOf(who);
        Causal_* k = i < 0 ? nullptr : causal(m_slots[i].body);
        if (!k) return "-";
        float x, z, hxf, hzf;
        {
            std::lock_guard<std::recursive_mutex> lk(k->TreeMutex());
            const OrderVector& r = k->Order4();
            Fixed hx, hz;
            heading(r, hx, hz);
            x = r.x.ToFloat(); z = r.z.ToFloat(); hxf = hx.ToFloat(); hzf = hz.ToFloat();
        }
        return std::to_string(i) + " " + fmt3(x) + " " + fmt3(z) + " " + fmt3(hxf) + " " + fmt3(hzf) + " "
             + fmt1(speedOf(static_cast<size_t>(i)));
    }
    /*
     * THE JUDGE'S CLOCK BY HAND: `ticks` ticks stepped and recorded, as the
     * frame edge does in real time -- what a headless run or a test drives a
     * race with (Scene3D::Run's counterpart). A follower has no clock of its
     * own: its ticks are the record's.
     */
    void Run(uint32_t ticks)
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (!m_judge) return;
        for (uint32_t i = 0; i < ticks; ++i)
        {
            stepLocked();
            if (m_tick >= m_recorded + kBatch) flushTicksLocked();
        }
        flushTicksLocked();
    }

    // ── Animated: the frame edge ────────────────────────────────────────

    bool AnimatingConcrete() override { return m_world != 0; }
    void AdvanceConcrete(double dt_ms) override
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (m_judge)
        {
            // The host's clock is the race's: whole ticks of wall time, at
            // most a few at once (a stalled frame is time lost, not a lurch).
            m_wall_ms += dt_ms;
            int due = static_cast<int>(m_wall_ms / kTickMs);
            m_wall_ms -= due * kTickMs;
            if (due > kMaxCatchUp) due = kMaxCatchUp;
            for (int i = 0; i < due; ++i) stepLocked();
            if (m_tick >= m_recorded + kBatch) flushTicksLocked();
        }
        cameraLocked(dt_ms);
        cardLocked(dt_ms);
    }

private:
    struct Slot
    {
        ETCS::RID   body = 0;
        ETCS::RID   wheels[4] = { 0, 0, 0, 0 };   // fl fr rl rr: drawn, placed by the race
        OrderVector pristine;          // the rows as made: every restart starts here
        std::string driver;
        int         thr = 0, steer = 0;
        Fixed       wheel;             // radians, + is left
        Fixed       prev_x;
        uint32_t    laps = 0;
        int64_t     from = -1, last = -1, best = -1;   // ticks; -1 none yet
        bool        half = false;      // past the far gate since the last lap
    };

    static constexpr size_t   kMaxKarts   = 8;
    static constexpr int      kHz         = 50;           // ticks a second
    static constexpr double   kTickMs     = 1000.0 / kHz;
    static constexpr int      kMaxCatchUp = 5;
    static constexpr uint64_t kBatch      = 2;            // ticks per tick line (25 a second)
    static constexpr uint64_t kSnapEvery  = 250;          // a snapshot each 5 s of race
    static constexpr float kColors[kMaxKarts][3] = {
        { 0.86f, 0.18f, 0.16f }, { 0.18f, 0.42f, 0.88f }, { 0.95f, 0.80f, 0.15f }, { 0.20f, 0.70f, 0.30f },
        { 0.95f, 0.50f, 0.10f }, { 0.60f, 0.30f, 0.80f }, { 0.20f, 0.80f, 0.85f }, { 0.92f, 0.92f, 0.90f } };

    // The kart, in Fixed, from whole numbers: the same constants everywhere.
    static Fixed F(int64_t n, int64_t d = 1) { return Fixed::FromInt(n) / Fixed::FromInt(d); }
    static Fixed dt()        { return F(1, kHz); }
    static Fixed accel()     { return F(9); }        // units/s^2 with the pedal down
    static Fixed topSpeed()  { return F(16); }
    static Fixed brake()     { return F(20); }
    static Fixed reverseAcc(){ return F(5); }
    static Fixed reverseTop(){ return F(6); }
    static Fixed coast()     { return F(6, 10); }    // per second, pedal up
    static Fixed wheelLock() { return F(45, 100); }  // radians
    static Fixed wheelRate() { return F(3); }        // radians/s at the wheel
    static Fixed wheelbase() { return F(16, 10); }
    static Fixed grip()      { return F(9); }        // per second, across the heading
    static Fixed kartY()     { return F(25, 100); }  // the kart's centre over the track

    // ── the record ──────────────────────────────────────────────────────

    // The judge: applied here, then into the record. Ticks the host has
    // stepped but not recorded go in first, so the line lands where it was
    // applied. False when refused.
    bool takeLocked(const std::string& author, const std::string& line)
    {
        if (line.compare(0, 5, "tick ") != 0) flushTicksLocked();
        if (!applyLocked(author, line)) return false;
        recordLocked(author, line);
        return true;
    }
    void flushTicksLocked()
    {
        if (m_tick <= m_recorded) return;
        m_recorded = m_tick;
        recordLocked(m_me, "tick " + std::to_string(m_tick));
    }
    void recordLocked(const std::string& author, const std::string& line)
    {
        if (!m_session) return;
        Record_* rec = recordOf(m_record);
        if (!rec) return;
        const uint64_t seq = rec->Append(author, line);
        if (seq != UINT64_MAX) m_seq = seq + 1;
    }

    // One line, as its author: what every runtime does with the record.
    bool applyLocked(const std::string& author, const std::string& line)
    {
        std::istringstream in(line);
        std::string verb;
        in >> verb;
        if (verb == "tick")
        {
            uint64_t n = 0;
            in >> n;
            while (m_tick < n) stepLocked();
            return true;
        }
        if (verb == "drive")
        {
            const int i = slotOf(author);
            int t = 0, s = 0;
            if (i < 0 || !(in >> t >> s)) return false;
            m_slots[i].thr   = t < 0 ? -1 : (t > 0 ? 1 : 0);
            m_slots[i].steer = s < 0 ? -1 : (s > 0 ? 1 : 0);
            return true;
        }
        if (verb == "join")
        {
            if (slotOf(author) >= 0) return false;
            for (size_t i = 0; i < m_slots.size(); ++i)
                if (m_slots[i].driver.empty())
                {
                    m_slots[i].driver = author;
                    gridLocked(i);
                    noteLocked(author + " drives kart " + std::to_string(i + 1));
                    // Keys held while the seat was on its way: said again now.
                    if (author == m_me && !m_judge && (m_sent_t || m_sent_s))
                        m_outbox.push_back("drive " + std::to_string(m_sent_t) + " " + std::to_string(m_sent_s));
                    return true;
                }
            return false;   // a full grid: watching
        }
        if (verb == "leave")
        {
            std::string who;
            in >> who;
            const int i = slotOf(who);
            if (i < 0) return false;
            pitLocked(static_cast<size_t>(i));
            return true;
        }
        return false;
    }

    // ── the world ───────────────────────────────────────────────────────

    // Back to the start: every kart in the pit as made, tick 0. What a
    // session begins from on every runtime, so two of them start the same.
    void restartLocked()
    {
        for (size_t i = 0; i < m_slots.size(); ++i) pitLocked(i);
        m_tick = m_recorded = m_seq = 0;
        m_wall_ms = 0.0;
        m_snaps.clear(); m_snap_tick = 0; m_snap_hash = "-"; m_differed_at = 0;
        m_keys = 0; m_sent_t = m_sent_s = 0;
        m_eye_set = false;
    }
    // A kart's rows as made, then placed and faced.
    void placeLocked(size_t i, Fixed x, Fixed y, Fixed z, Fixed yaw)
    {
        Causal_* k = causal(m_slots[i].body);
        if (!k) return;
        std::lock_guard<std::recursive_mutex> lk(k->TreeMutex());
        OrderVector& r = k->RowsUnder();
        const uint64_t id = r.id;
        r = m_slots[i].pristine;
        r.id = id;
        r.PlaceAt(x, y, z);
        r.Orient(Fixed::Zero(), Fixed::One(), Fixed::Zero(), yaw);
    }
    // The pit: hidden, held, out of everyone's way.
    void pitLocked(size_t i)
    {
        Slot& s = m_slots[i];
        s.driver.clear(); s.thr = s.steer = 0; s.wheel = Fixed::Zero();
        s.laps = 0; s.from = s.last = s.best = -1; s.half = false;
        verb(s.body, "SetVisible", "0");
        verb(s.body, "SetAnchored", "1");
        for (ETCS::RID w : s.wheels) verb(w, "SetVisible", "0");
        placeLocked(i, F(200) + F(4) * F(static_cast<int64_t>(i)), kartY(), Fixed::Zero(), Fixed::Zero());
        wheelsLocked(s);
        s.prev_x = F(200);
    }
    // The grid: two columns behind the line, facing +x along the near straight.
    void gridLocked(size_t i)
    {
        Slot& s = m_slots[i];
        s.thr = s.steer = 0; s.wheel = Fixed::Zero();
        s.laps = 0; s.from = s.last = s.best = -1; s.half = false;
        const Fixed x = -(F(3) + F(35, 10) * F(static_cast<int64_t>(i / 2)));
        const Fixed z = (i % 2) ? -F(115, 10) : -F(165, 10);
        placeLocked(i, x, kartY(), z, Fixed::TwoPi() / F(4));
        s.prev_x = x;
        wheelsLocked(s);
        verb(s.body, "SetAnchored", "0");
        verb(s.body, "SetVisible", "1");
        for (ETCS::RID w : s.wheels) verb(w, "SetVisible", "1");
    }

    // One tick: every kart driven, the world stepped, the laps read.
    void stepLocked()
    {
        Causal_* w = causal(m_world);
        if (!w) { ++m_tick; return; }
        {
            std::lock_guard<std::recursive_mutex> lk(w->TreeMutex());
            for (Slot& s : m_slots)
                if (!s.driver.empty())
                    if (Causal_* k = causal(s.body)) driveLocked(s, k);
            w->Interact(dt());
            ++m_tick;
            for (Slot& s : m_slots)
                if (!s.driver.empty())
                {
                    if (Causal_* k = causal(s.body)) lapLocked(s, k->RowsUnder());
                    wheelsLocked(s);
                }
        }
        if (m_tick % kSnapEvery == 0)
        {
            m_snap_tick = m_tick;
            m_snap_hash = hashLocked();
            m_snaps.push_back({ m_snap_tick, m_snap_hash });
            if (m_snaps.size() > 16) m_snaps.erase(m_snaps.begin());
        }
    }

    // The bicycle, on the rows, under the tree's lock (KartRace's header note).
    void driveLocked(Slot& s, Causal_* k)
    {
        OrderVector& r = k->RowsUnder();
        const Fixed m = k->MassUnder();
        const Fixed step = dt();
        Fixed hx, hz;
        heading(r, hx, hz);
        Fixed vx, vy, vz;
        r.Velocity(m, vx, vy, vz);
        Fixed speed = vx * hx + vz * hz;                   // along the heading, signed
        Fixed lx = vx - hx * speed, lz = vz - hz * speed;  // across it

        // The pedal.
        if (s.thr > 0)
        {
            if (speed.raw < 0) speed = Fixed::Min(speed + brake() * step, Fixed::Zero());
            else               speed = Fixed::Min(speed + accel() * step, topSpeed());
        }
        else if (s.thr < 0)
        {
            if (speed.raw > 0) speed = Fixed::Max(speed - brake() * step, Fixed::Zero());
            else               speed = Fixed::Max(speed - reverseAcc() * step, -reverseTop());
        }
        else
        {
            speed -= speed * coast() * step;
            if (speed.Abs() < F(1, 100)) speed = Fixed::Zero();
        }
        // The wheels, toward the lock the key asks for.
        const Fixed target = wheelLock() * F(s.steer);
        const Fixed turn = wheelRate() * step;
        Fixed d = target - s.wheel;
        if (d > turn) d = turn; else if (d < -turn) d = -turn;
        s.wheel += d;
        // The heading, by speed x wheel / wheelbase.
        const Fixed yaw = speed * s.wheel / wheelbase() * step;
        if (!yaw.IsZero()) r.RotateBy(Fixed::Zero(), Fixed::One(), Fixed::Zero(), yaw);
        heading(r, hx, hz);
        // The tyres take what is across the heading.
        Fixed keep = Fixed::One() - grip() * step;
        if (keep.raw < 0) keep = Fixed::Zero();
        lx *= keep; lz *= keep;
        const Fixed nx = hx * speed + lx, nz = hz * speed + lz;
        // Work in is the pedal's Impulse; work out is heat (SetVelocity).
        const Fixed after = Fixed::Half() * m * (nx * nx + nz * nz);
        const Fixed before = r.KineticEnergy();
        if (after > before) k->Impulse(nx, Fixed::Zero(), nz, after - before);
        r.SetVelocity(m, nx, Fixed::Zero(), nz);
        r.y = kartY();   // a flat track: the height is the track's
    }
    /*
     * THE WHEELS, where the kart is: four cylinders on their sides at its
     * corners, the front two turned by the wheel angle -- what A and D are
     * seen to do. A node's children do not turn with it (frames are
     * translations), so the wheels are the world's members, placed here
     * after every step from the kart's rows: anchored, not solid, nothing
     * meets them. The same on every runtime, being of the rows.
     */
    void wheelsLocked(const Slot& s)
    {
        Causal_* k = causal(s.body);
        if (!k) return;
        std::lock_guard<std::recursive_mutex> lk(k->TreeMutex());
        const OrderVector& r = k->Order4();
        Fixed hx, hz;
        heading(r, hx, hz);
        const Fixed lx = hz, lz = -hx;                              // left of the heading
        const Fixed side = F(70, 100), ahead = F(64, 100), y = r.y - F(3, 100);
        for (int i = 0; i < 4; ++i)
        {
            Causal_* w = causal(s.wheels[i]);
            if (!w) continue;
            OrderVector& q = w->RowsUnder();
            const Fixed lat = (i % 2) ? -side : side;               // fl, rl on the left
            const Fixed lon = (i < 2) ? ahead : -ahead;
            q.PlaceAt(r.x + hx * lon + lx * lat, y, r.z + hz * lon + lz * lat);
            // On its side (the axle along x), turned by the wheel if a front
            // one, then by the kart's own facing: R = kart * steer * roll.
            q.Orient(Fixed::Zero(), Fixed::Zero(), Fixed::One(), Fixed::TwoPi() / F(4));
            if (i < 2 && !s.wheel.IsZero()) q.RotateBy(Fixed::Zero(), Fixed::One(), Fixed::Zero(), s.wheel);
            const Fixed aw = r.qw, ax = r.qx, ay = r.qy, az = r.qz;
            const Fixed bw = q.qw, bx = q.qx, by = q.qy, bz = q.qz;
            q.qw = aw * bw - ax * bx - ay * by - az * bz;
            q.qx = aw * bx + ax * bw + ay * bz - az * by;
            q.qy = aw * by - ax * bz + ay * bw + az * bx;
            q.qz = aw * bz + ax * by - ay * bx + az * bw;
        }
    }
    static void heading(const OrderVector& r, Fixed& hx, Fixed& hz)
    {
        Fixed x = Fixed::Zero(), y = Fixed::Zero(), z = Fixed::One();
        r.RotateVector(x, y, z);
        const Fixed l = Fixed::Length(x, Fixed::Zero(), z);
        if (!l.IsPositive()) { hx = Fixed::Zero(); hz = Fixed::One(); return; }
        hx = x / l; hz = z / l;
    }

    /*
     * THE LAPS, by two gates across the track at x = 0 (the course puts the
     * line there): the near one (z < -8) crossed going +x is the line, the
     * far one (z > 8) crossed going -x is the half-way mark. The first
     * crossing of the line starts the clock; each one after it, with the
     * half-way mark passed since, is a lap.
     */
    void lapLocked(Slot& s, const OrderVector& r)
    {
        const Fixed x = r.x, z = r.z;
        if (s.prev_x.raw < 0 && x.raw >= 0 && z < -F(8))
        {
            if (s.from < 0) s.from = static_cast<int64_t>(m_tick);
            else if (s.half)
            {
                ++s.laps;
                s.last = static_cast<int64_t>(m_tick) - s.from;
                if (s.best < 0 || s.last < s.best) s.best = s.last;
                s.from = static_cast<int64_t>(m_tick);
                s.half = false;
            }
        }
        if (s.prev_x.raw > 0 && x.raw <= 0 && z > F(8)) s.half = true;
        s.prev_x = x;
    }

    // The race as one number: every kart's rows and the race's own state.
    std::string hashLocked()
    {
        std::string st = std::to_string(m_tick);
        for (const Slot& s : m_slots)
        {
            st += "|" + s.driver + " " + std::to_string(s.thr) + " " + std::to_string(s.steer) + " "
                + std::to_string(s.wheel.raw) + " " + std::to_string(s.laps) + " " + std::to_string(s.from)
                + " " + std::to_string(s.best) + " " + (s.half ? "h" : "-");
            if (Causal_* k = causal(s.body))
            {
                std::lock_guard<std::recursive_mutex> lk(k->TreeMutex());
                const OrderVector& r = k->Order4();
                const Fixed v[] = { r.x, r.y, r.z, r.ox, r.oy, r.oz, r.energy, r.qx, r.qy, r.qz, r.qw };
                for (const Fixed& f : v) st += " " + std::to_string(f.raw);
            }
        }
        char buf[17];
        std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(XXH3_64bits(st.data(), st.size())));
        return buf;
    }

    // ── the picture ─────────────────────────────────────────────────────

    // Behind this runtime's own kart, eased; over the whole track for a
    // runtime with none. The camera is this runtime's: nothing here is a line.
    void cameraLocked(double dt_ms)
    {
        if (!m_camera) return;
        ETCS::Held<Camera_> cam = ETCS::resolve_held<Camera_>("Camera", m_camera);
        if (!cam) return;
        float eye[3] = { 0.0f, 58.0f, -42.0f }, at[3] = { 0.0f, 0.0f, 0.0f };
        const int i = slotOf(m_me);
        if (i >= 0)
            if (Causal_* k = causal(m_slots[i].body))
            {
                std::lock_guard<std::recursive_mutex> lk(k->TreeMutex());
                const OrderVector& r = k->Order4();
                Fixed hx, hz;
                heading(r, hx, hz);
                const float px = r.x.ToFloat(), py = r.y.ToFloat(), pz = r.z.ToFloat();
                const float fx = hx.ToFloat(), fz = hz.ToFloat();
                eye[0] = px - fx * 7.0f; eye[1] = py + 3.2f; eye[2] = pz - fz * 7.0f;
                at[0]  = px + fx * 4.0f; at[1]  = py + 0.6f; at[2]  = pz + fz * 4.0f;
            }
        const float a = m_eye_set ? static_cast<float>(1.0 - std::exp(-dt_ms / 120.0)) : 1.0f;
        float moved = 0.0f;
        for (int c = 0; c < 3; ++c)
        {
            const float de = (eye[c] - m_eye[c]) * a, da = (at[c] - m_at[c]) * a;
            m_eye[c] += de; m_at[c] += da;
            moved += std::fabs(de) + std::fabs(da);
        }
        // A still eye is not set again: setting it is a new picture to draw.
        if (m_eye_set && moved < 1e-4f) return;
        m_eye_set = true;
        ViewFrustum v = cam->GetView();
        v.position = Point3D{ m_eye[0], m_eye[1], m_eye[2] };
        v.look_at  = Point3D{ m_at[0], m_at[1], m_at[2] };
        v.up       = Point3D{ 0.0f, 1.0f, 0.0f };
        cam->SetView(v);
    }
    // The card on the camera: this runtime's lap, times and speed.
    void cardLocked(double dt_ms)
    {
        if (!m_card) return;
        m_card_ms += dt_ms;
        if (m_card_ms < 150.0) return;
        m_card_ms = 0.0;
        std::string t;
        const int i = slotOf(m_me);
        if (i < 0) t = m_me.empty() ? "kart: not racing" : "watching -- the grid is full";
        else
        {
            const Slot& s = m_slots[i];
            t = "lap " + std::to_string(s.laps + 1) + "   last " + secs(s.last) + "   best " + secs(s.best)
              + "   " + fmt1(speedOf(static_cast<size_t>(i))) + " u/s";
        }
        if (!m_note.empty()) t += "   " + m_note;
        if (t == m_card_text) return;
        m_card_text = t;
        verb(m_card, "SetText", t);
    }

    // ── small things ────────────────────────────────────────────────────

    int slotOf(const std::string& who) const
    {
        if (who.empty()) return -1;
        for (size_t i = 0; i < m_slots.size(); ++i) if (m_slots[i].driver == who) return static_cast<int>(i);
        return -1;
    }
    size_t driversLocked() const
    {
        size_t n = 0;
        for (const Slot& s : m_slots) if (!s.driver.empty()) ++n;
        return n;
    }
    float speedOf(size_t i)
    {
        Causal_* k = causal(m_slots[i].body);
        if (!k) return 0.0f;
        std::lock_guard<std::recursive_mutex> lk(k->TreeMutex());
        Fixed vx, vy, vz;
        k->Order4().Velocity(k->MassUnder(), vx, vy, vz);
        return Fixed::Length(vx, vy, vz).ToFloat();
    }
    void noteLocked(const std::string& n) { m_note = n; ETCS_LOG("KartRace", n); }
    static std::string secs(int64_t ticks)
    {
        if (ticks < 0) return "-";
        char b[32];
        std::snprintf(b, sizeof b, "%.2f", static_cast<double>(ticks) / kHz);
        return b;
    }
    static std::string fmt1(float v) { char b[32]; std::snprintf(b, sizeof b, "%.1f", v); return b; }
    static std::string fmt3(float v) { char b[32]; std::snprintf(b, sizeof b, "%.3f", v); return b; }
    static std::string f4(float a, float b, float c, float d)
    {
        return std::to_string(a) + ", " + std::to_string(b) + ", " + std::to_string(c) + ", " + std::to_string(d);
    }
    static bool parseLine(const std::string& msg, uint64_t& seq, std::string& author, std::string& line)
    {
        const size_t a = msg.find(' ');
        const size_t b = a == std::string::npos ? a : msg.find(' ', a + 1);
        if (b == std::string::npos) return false;
        seq = std::strtoull(msg.c_str(), nullptr, 10);
        author = msg.substr(a + 1, b - a - 1);
        line = msg.substr(b + 1);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        return !line.empty();
    }
    static Causal_* causal(ETCS::RID r) { return r ? ETCS::resolve_in_family<Causal_>("Causal", r) : nullptr; }
    static Record_* recordOf(ETCS::RID rid)
    {
        ETCS::Entity* e = rid ? ETCS::etcs_resolve_rid_anywhere(&ETCS::getLoader(), rid) : nullptr;
        return e ? static_cast<Record_*>(e->getInterfacePointer(ETCS::Buffer("Record"))) : nullptr;
    }
    static void callOn(ETCS::RID rid, const char* v, ETCS::Buffer& arg)
    {
        ETCS::Entity* e = rid ? ETCS::etcs_resolve_rid_anywhere(&ETCS::getLoader(), rid) : nullptr;
        if (e) e->call(ETCS::Buffer(v), arg);
    }
    // A verb on a node by its own tag: what this module knows of the module
    // that made it is the verb's name (GolfGame's way).
    static bool verb(ETCS::RID node, const char* name, const std::string& args)
    {
        if (!node) return false;
        ETCS::Entity* e = ETCS::etcs_resolve_rid_anywhere(&ETCS::getLoader(), node);
        if (!e) return false;
        const std::string tag = e->getSourceTag().toString();
        ETCS::Buffer action((tag + "." + name).c_str());
        ETCS::Buffer payload(args.c_str());
        try { e->call(action, payload); } catch (...) { return false; }
        return true;
    }

    std::mutex m_mu;
    ETCS::RID  m_world = 0, m_camera = 0, m_card = 0;
    std::vector<Slot> m_slots;

    // The session.
    std::string m_me, m_owner, m_id;
    bool        m_judge = false, m_session = false;
    ETCS::RID   m_record = 0, m_proposals = 0, m_presence = 0, m_hall = 0;
    std::deque<std::string> m_outbox;
    std::map<std::string, std::pair<uint64_t, std::string>> m_here;   // presence: name -> snapshot
    std::string m_sent, m_advertised, m_note;

    // The race's clock and its record position.
    uint64_t m_tick = 0, m_recorded = 0, m_seq = 0;
    double   m_wall_ms = 0.0;
    uint64_t m_snap_tick = 0, m_differed_at = 0;
    std::string m_snap_hash = "-";
    std::vector<std::pair<uint64_t, std::string>> m_snaps;

    // This runtime's own: its keys, its eye, its card.
    unsigned m_keys = 0;
    int      m_sent_t = 0, m_sent_s = 0;
    float    m_eye[3] = { 0.0f, 0.0f, 0.0f }, m_at[3] = { 0.0f, 0.0f, 0.0f };
    bool     m_eye_set = false;
    double   m_card_ms = 0.0;
    std::string m_card_text;
};

#endif // KARTPROVIDER_KARTRACE_H__
