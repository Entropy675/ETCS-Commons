#ifndef KARTPROVIDER_KARTBATTLE_H__
#define KARTPROVIDER_KARTBATTLE_H__

#include "../../../core_defs.h"
#include "../../../ontology.h"
#include "KartArms.h"
#include "KartArena.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

/*
 * ── KartBattle: karts in an arena, one per player, between runtimes ───────
 *
 * THE RECORD IS THE BATTLE, as it is the game in chess (ChessGame's header
 * note). Every runtime holds its own copy of the world -- the same arena
 * script built it -- and steps it from the record alone:
 *
 *   join                     the author takes the next free kart, at a spawn
 *   drive <t> <s> <f>        the author's pedal, wheel and trigger: -1 0 1, 0 1
 *   tick <n>                 every kart driven, every shot flown, the world
 *                            stepped, up to tick n
 *   leave <who>              (the host's) that kart back in the pit
 *   config <map> <secs> [<layout>]   (the host's, in the lobby only) the next
 *                            round's map and length; a map made from a seed
 *                            carries its seed, so it is the same everywhere
 *   round <seed>             (the host's, in the lobby) the round starts:
 *                            scores to nothing, everyone respawned, the dice
 *                            seeded
 *   abandon                  (the host's, in a round) back to the lobby, the
 *                            round's scores discarded
 *   give <who> <weapon>      (the host's) a weapon handed over outright
 *
 * Lines apply in the record's order, and a tick is a line, so where an input
 * falls between two ticks is the record's to say and the same everywhere.
 * The world is Scene3D marked driven (SetDriven): no picture steps it. The
 * karts' rows, the ground under them, the shots, the damage and the dice are
 * all in the rows' own integers (Fixed, KartArms.h, KartArena.h), so two
 * runtimes fed the same lines are the same battle (etcs_causal_constraints.md
 * §2). Lockstep, with the record as the lock: nobody negotiates anything.
 *
 * ONE BATTLE JUDGES (the host's). A guest's keys go to the host's proposals
 * ledger (`battle.Emit() -> proposals.Take()`, authored by its link); the
 * judge applies each line it takes and appends it to the record. The host's
 * clock is the battle's: its frame edge steps the world in real time and puts
 * `tick` lines into the record in pairs, and always before any other line,
 * so a line lands at the tick the host applied it at. A guest follows the
 * record (`record.Follow(0) -> battle.Absorb()`): it sees the arena one trip
 * behind the host, and its own keys a round trip late. Alone (Practice) the
 * battle is its own judge with no record.
 *
 * A BATTLE THAT DIFFERS is reported, not repaired (ChessShare's rule): every
 * kSnapEvery ticks each runtime hashes it and advertises the hash with its
 * presence; a guest whose hash is not the host's at that tick says so.
 *
 * THE GAME. Between rounds is the LOBBY: the karts wait parked at their
 * spawns, and the host sets the next round on the menu (the map, the length,
 * start) -- every runtime shows the menu, only the host's moves it. Once a
 * round starts its settings are locked until it is completed or the host
 * abandons it (Esc), which discards its scores. A kart has 100 points (10.0);
 * a power-up gives a random weapon (KartArms.h), replacing one not yet used
 * up. Space fires it. A kart brought to nothing is eliminated -- one to the
 * attacker, one against the kart -- and comes back at the spawn furthest from
 * everyone after three seconds, whole and unarmed. Tab holds up the scores.
 *
 * THE KART IS A BICYCLE ON THE GROUND. W and S are the pedal, A and D turn
 * the WHEELS, which ease to a lock and back; the heading turns by speed x
 * wheel angle / wheelbase, so a kart standing still does not turn. The ground
 * is the map's (KartArena.h): a kart rides on it, slows going up and gains
 * going down, leaves it where it falls away faster than gravity follows (a
 * ramp's lip) and lands where it meets it again; ground rising more than a
 * kart can climb in a step is a wall. A kart is SOLID -- a sphere to
 * everything (etcs_causal_constraints.md §13) -- and drawn as a body on four
 * wheels, tilted to the ground, which this battle places after every step (a
 * node's children do not turn with it). What is drawn for one runtime alone
 * -- its camera, its card, the menu and the board, the HP bars turned to face
 * its eye -- is set on the frame edge and is not a line.
 */
class KartBattle : public AnimatedBase<KartBattle>,
                   public DeletableBase<KartBattle>
{
public:
    WIRE_TYPE_IDENTITY(KartBattle);

    KartBattle() = default;
    bool DeleteConcrete() override { Leave(); return true; }

    bool Create() { this->addTag("active"); return true; }

    void BindWorld(ETCS::RID r)  { std::lock_guard<std::mutex> g(m_mu); m_world = r; }
    void BindCamera(ETCS::RID r) { std::lock_guard<std::mutex> g(m_mu); m_camera = r; m_eye_set = false; }
    void BindCard(ETCS::RID r)   { std::lock_guard<std::mutex> g(m_mu); m_card = r; m_card_text.clear(); }

    // ── the arena: what the scripts hand over ────────────────────────────

    // A kart of the pool, its four wheels (front left, front right, rear left,
    // rear right) and its HP bar (the back and the fill): coloured by its
    // slot, waiting in the pit until someone joins.
    void AddKart(ETCS::RID body, ETCS::RID fl, ETCS::RID fr, ETCS::RID rl, ETCS::RID rr,
                 ETCS::RID bar_back, ETCS::RID bar_fill)
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (!body || m_slots.size() >= kMaxKarts) return;
        Slot s;
        s.body = body;
        s.wheels[0] = fl; s.wheels[1] = fr; s.wheels[2] = rl; s.wheels[3] = rr;
        s.bar_back = bar_back; s.bar_fill = bar_fill;
        const float* c = kColors[m_slots.size() % kMaxKarts];
        verb(body, "SetColor", f4(c[0], c[1], c[2], 1.0f));
        if (Causal_* k = causal(body))
        {
            std::lock_guard<std::recursive_mutex> lk(k->TreeMutex());
            s.pristine = k->RowsUnder();   // what "as made" means, for every restart
        }
        m_slots.push_back(s);
        pitLocked(m_slots.size() - 1);
        // Practice asked for before the arena had made the karts (a page is
        // quicker than its boot): the driver takes the first one made.
        if (!m_me.empty() && !m_session && slotOf(m_me) < 0) takeLocked(m_me, "join");
    }
    // A wall of the arena: solid on every map, met by shots as by karts.
    void AddWall(ETCS::RID part) { std::lock_guard<std::mutex> g(m_mu); if (part) m_walls.push_back(part); }

    // A map by name, then what is on it (KartArena.h), each of the latest
    // map: blocks, ramps, hills, spawns and power-up spots.
    void AddMap(const std::string& name)
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (name.empty()) return;
        KartArena::Map m; m.name = name;
        m_defs.push_back(m);
    }
    // A map made from a seed each time it is chosen (KartArena::Generate).
    void AddRandomMap(const std::string& name)
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (name.empty()) return;
        KartArena::Map m; m.name = name; m.generated = true;
        m_defs.push_back(m);
    }
    void AddBlock(float x, float z, float w, float d, float h, float yaw)
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (m_defs.empty() || w <= 0 || d <= 0 || h <= 0) return;
        KartArena::Block b;
        b.x = Fixed::From(x); b.z = Fixed::From(z); b.w = Fixed::From(w); b.d = Fixed::From(d); b.h = Fixed::From(h);
        b.yaw = static_cast<int>(std::lround(yaw));
        m_defs.back().blocks.push_back(b);
    }
    void AddRamp(float x, float z, float yaw, float width, float up, float flat, float down, float height)
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (m_defs.empty() || width <= 0 || up <= 0 || flat < 0 || down < 0 || height <= 0) return;
        KartArena::Ramp r;
        r.x = Fixed::From(x); r.z = Fixed::From(z); r.yaw = static_cast<int>(std::lround(yaw));
        r.width = Fixed::From(width); r.up = Fixed::From(up); r.flat = Fixed::From(flat);
        r.down = Fixed::From(down); r.height = Fixed::From(height);
        m_defs.back().ramps.push_back(r);
    }
    void AddHill(float x, float z, float radius, float height)
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (m_defs.empty() || radius <= 0 || height <= 0) return;
        KartArena::Hill k;
        k.x = Fixed::From(x); k.z = Fixed::From(z); k.radius = Fixed::From(radius); k.height = Fixed::From(height);
        m_defs.back().hills.push_back(k);
    }
    void AddSpawn(float x, float z)  { std::lock_guard<std::mutex> g(m_mu); if (!m_defs.empty()) m_defs.back().spawns.push_back({ Fixed::From(x), Fixed::From(z) }); }
    void AddPickup(float x, float z) { std::lock_guard<std::mutex> g(m_mu); if (!m_defs.empty()) m_defs.back().pickups.push_back({ Fixed::From(x), Fixed::From(z) }); }

    // The pools the map in play is drawn with: solid blocks, the ground's
    // slabs (a ramp's slope or top) and domes (a hill), power-up boxes, shots,
    // bursts, and the rows of the menu and the board on the camera.
    void AddBlockNode(ETCS::RID r)  { std::lock_guard<std::mutex> g(m_mu); m_block_nodes.push_back(r); parkNode(r); }
    void AddSlabNode(ETCS::RID r)   { std::lock_guard<std::mutex> g(m_mu); m_slab_nodes.push_back(r); parkNode(r); }
    void AddDomeNode(ETCS::RID r)   { std::lock_guard<std::mutex> g(m_mu); m_dome_nodes.push_back(r); parkNode(r); }
    void AddPickupNode(ETCS::RID r) { std::lock_guard<std::mutex> g(m_mu); m_pickup_nodes.push_back(r); hide(r); }
    void AddShotNode(ETCS::RID r)   { std::lock_guard<std::mutex> g(m_mu); m_shot_nodes.push_back(r); m_shot_busy.push_back(false); hide(r); }
    void AddBlastNode(ETCS::RID r)  { std::lock_guard<std::mutex> g(m_mu); m_blast_nodes.push_back(r); m_blast_until.push_back(0); hide(r); }
    void AddBoardRow(ETCS::RID r)
    {
        std::lock_guard<std::mutex> g(m_mu);
        verb(r, "SetPosition", std::to_string(80) + ", " + std::to_string(96 + 26 * static_cast<int>(m_board.size())));
        verb(r, "SetHidden", "1");
        m_board.push_back({ r, std::string(), false });
    }
    // The arena is made: the first map out, the lobby open.
    void Ready()
    {
        std::lock_guard<std::mutex> g(m_mu);
        mapLocked(0, firstLayout(0));
        respawnAllLocked();
    }

    // ── sessions ──────────────────────────────────────────────────────────

    // Alone: this battle is its own judge, nothing recorded.
    void Practice(const std::string& name)
    {
        Leave();
        std::lock_guard<std::mutex> g(m_mu);
        restartLocked();
        m_me = name; m_owner = name; m_judge = true;
        takeLocked(m_me, "join");
    }
    // Host: the record is here and this battle judges it; the hall (a Lobby)
    // lists it as `kart <id>` for as long as it lasts.
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
        ETCS_LOG("KartBattle", "hosting a battle as " << name);
        Tick();
        return true;
    }
    // Join: this battle follows the host's record from its first line, and
    // asks for a kart.
    bool Join(ETCS::RID presence, const std::string& name, const std::string& owner)
    {
        if (name.empty()) return false;
        Leave();
        std::lock_guard<std::mutex> g(m_mu);
        restartLocked();
        m_me = name; m_owner = owner; m_judge = false; m_session = true; m_presence = presence;
        m_outbox.push_back("join");
        ETCS_LOG("KartBattle", "joined " << owner << "'s battle as " << name);
        return true;
    }
    void Leave()
    {
        ETCS::RID hall = 0;
        std::string me;
        {
            std::lock_guard<std::mutex> g(m_mu);
            ++m_gen;   // the streams of the session going end with it (below)
            if (m_me.empty()) return;
            if (m_session && m_judge) { hall = m_hall; me = m_me; }
            m_me.clear(); m_owner.clear(); m_judge = false; m_session = false;
            m_record = m_proposals = m_presence = m_hall = 0; m_id.clear();
            m_outbox.clear(); m_here.clear(); m_sent.clear(); m_advertised.clear(); m_note.clear();
        }
        if (hall) { ETCS::Buffer a; a.writeString(me.c_str()); callOn(hall, "Lobby.Withdraw", a); }
    }

    // ── the host's lobby and round, as lines ─────────────────────────────

    // The next round's map (by name) and length in seconds -- in the lobby
    // only. Choosing a map made from a seed gives it a new seed; the same map
    // again keeps the one it has (Reroll gives a new one).
    bool Configure(const std::string& map, uint32_t secs)
    {
        std::lock_guard<std::mutex> g(m_mu);
        const int m = mapIndex(map);
        if (!m_judge || m < 0) return false;
        return configLocked(m, secs, m != m_map);
    }
    bool Reroll()
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (!m_judge || m_defs.empty() || !m_defs[m_map].generated) return false;
        return configLocked(m_map, m_round_secs, true);
    }
    bool StartRound()
    {
        std::lock_guard<std::mutex> g(m_mu);
        return m_judge && roundLocked();
    }
    bool Abandon()
    {
        std::lock_guard<std::mutex> g(m_mu);
        return m_judge && takeLocked(m_me, "abandon");
    }
    bool Give(const std::string& who, const std::string& weapon)
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (!m_judge) return false;
        return takeLocked(m_me, "give " + who + " " + weapon);
    }

    /*
     * PRESENCE AND THE HALL, each when it changed: "<name> kart <tick> <hash>"
     * -- this battle's latest snapshot -- and, on the host, "<name> kart <id>
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
    //
    // EACH STREAM IS ITS SESSION'S. A stream takes the session's number when
    // it starts (a template detaches it after Host or Join), and is stale once
    // the battle is in another: a guest's emitter left over from a link that
    // went would otherwise take the next session's first line -- its `join`
    // -- and write it to the closed link, and a follower would feed an old
    // record into the new battle. A stale stream is told so, and ends.

    uint64_t Generation() { std::lock_guard<std::mutex> g(m_mu); return m_gen; }

    // A line for the host, or nothing yet; false (and `stale`) once the
    // session is another's.
    bool nextEmit(uint64_t gen, std::string& out, bool& stale)
    {
        std::lock_guard<std::mutex> g(m_mu);
        stale = gen != m_gen;
        if (stale || m_outbox.empty()) return false;
        out = std::move(m_outbox.front());
        m_outbox.pop_front();
        return true;
    }
    // A line of the record, followed: "<seq> <author> <line>". False once
    // the session is another's.
    bool Absorb(uint64_t gen, const std::string& msg)
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (gen != m_gen) return false;
        if (m_judge || msg.compare(0, 2, "~ ") == 0) return true;
        uint64_t seq = 0; std::string author, line;
        if (!parseLine(msg, seq, author, line)) return true;
        if (!applyLocked(author, line) && line.compare(0, 4, "tick") != 0)
            ETCS_LOG("KartBattle", "line " << seq << " (" << author << " " << line << ") refused here -- this battle is not the record's.");
        m_seq = seq + 1;
        return true;
    }
    // The record ended under a guest still in the battle: the host closed it
    // (or its link went). The world stays where the last line left it.
    void RecordEnded(uint64_t gen)
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (gen == m_gen && m_session && !m_judge) noteLocked("the battle is over -- " + m_owner + " closed it");
    }
    // A proposal, judged: applied as its author and recorded if taken. A
    // guest proposes its own seat and its own controls, nothing else.
    bool JudgeLine(uint64_t gen, const std::string& msg)
    {
        ETCS::RID proposals = 0; uint64_t seq = 0;
        {
            std::lock_guard<std::mutex> g(m_mu);
            if (gen != m_gen) return false;
            if (!m_judge || !m_session || msg.compare(0, 2, "~ ") == 0) return true;
            std::string author, line;
            if (!parseLine(msg, seq, author, line) || author.empty()) return true;
            if (line == "join" || line.compare(0, 6, "drive ") == 0) takeLocked(author, line);
            proposals = m_proposals;
        }
        if (Record_* p = recordOf(proposals)) p->Checkpoint(seq + 1);
        return true;
    }
    // The presence listing, whole, on every change: arrivals and departures,
    // a departed driver's kart back to the pit (the host's line), and this
    // battle's snapshots against the host's.
    bool Roster(uint64_t gen, const std::string& listing)
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (gen != m_gen) return false;
        if (!m_session) return true;
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
        if (!m_judge && now.count(m_owner))
        {
            const auto& host = now[m_owner];
            for (auto& [t, h] : m_snaps)
                if (t == host.first && host.first && h != host.second && m_differed_at != t)
                {
                    m_differed_at = t;
                    ETCS_LOG("KartBattle", "this battle (" << h << ") is not the host's (" << host.second << ") at tick " << t);
                    noteLocked("the battle differs from the host's at tick " + std::to_string(t));
                }
        }
        return true;
    }

    /*
     * A KEY from the window (GLFW codes). In a round: W/S (or up/down) the
     * pedal, A/D (or left/right) the wheel, Space the trigger -- a change of
     * any is a `drive` line -- and, for the host, Esc abandons the round. Tab
     * holds up the board, this runtime's alone. In the lobby the host's keys
     * move the menu instead: up/down choose, left/right change, Enter
     * changes or starts; each change is a line.
     */
    void Key(uint16_t key, bool down)
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (key == 258) { m_board_held = down; return; }
        if (down && m_judge && !m_defs.empty())
        {
            if (m_phase == Lobby && menuKeyLocked(key)) return;
            if (m_phase == Playing && key == 256) { takeLocked(m_me, "abandon"); return; }
        }
        int bit = -1;
        if (key == 'W' || key == 265) bit = 0;
        else if (key == 'S' || key == 264) bit = 1;
        else if (key == 'A' || key == 263) bit = 2;
        else if (key == 'D' || key == 262) bit = 3;
        else if (key == ' ') bit = 4;
        if (bit < 0) return;
        if (down) m_keys |= (1u << bit); else m_keys &= ~(1u << bit);
        const int t = ((m_keys & 1) ? 1 : 0) - ((m_keys & 2) ? 1 : 0);
        const int s = ((m_keys & 4) ? 1 : 0) - ((m_keys & 8) ? 1 : 0);
        const int f = (m_keys & 16) ? 1 : 0;
        if (t == m_sent_t && s == m_sent_s && f == m_sent_f) return;
        m_sent_t = t; m_sent_s = s; m_sent_f = f;
        if (m_me.empty()) return;
        const std::string line = driveLine(t, s, f);
        if (m_judge) takeLocked(m_me, line);
        else m_outbox.push_back(line);
    }

    // ── reads ────────────────────────────────────────────────────────────

    // "<tick> <seq> <mode> <me> <slot> <phase> <secs left> <map> <round secs> <layout>"
    // mode: host | guest | practice | idle; phase: lobby | playing | results;
    // layout: the seed of a map made from one, 0 for a hand-made map.
    std::string Status()
    {
        std::lock_guard<std::mutex> g(m_mu);
        const char* mode = m_me.empty() ? "idle" : !m_session ? "practice" : m_judge ? "host" : "guest";
        return std::to_string(m_tick) + " " + std::to_string(m_seq) + " " + mode + " "
             + (m_me.empty() ? "-" : m_me) + " " + std::to_string(slotOf(m_me)) + " " + phaseName() + " "
             + std::to_string(secsLeft()) + " " + mapName(m_map) + " " + std::to_string(m_round_secs) + " "
             + std::to_string(m_layout);
    }
    // One line per kart with a driver, best first:
    // "<slot> <driver> <elims> <deaths> <hp> <weapon> <ammo> <alive|out>" --
    // the round's, or in the lobby the last round completed.
    std::string Scores()
    {
        std::lock_guard<std::mutex> g(m_mu);
        std::string out;
        for (size_t i : rankLocked())
        {
            const Slot& s = m_slots[i];
            out += std::to_string(i) + " " + s.driver + " " + std::to_string(elimsOf(s)) + " " + std::to_string(deathsOf(s))
                 + " " + std::to_string(s.hp) + " " + KartArms::Name(s.weapon) + " " + std::to_string(s.ammo)
                 + " " + (s.dead_until < 0 ? "alive" : "out") + "\n";
        }
        return out;
    }
    // The maps, one name a line, in their order.
    std::string Maps()
    {
        std::lock_guard<std::mutex> g(m_mu);
        std::string out;
        for (const KartArena::Map& m : m_defs) out += m.name + "\n";
        return out;
    }
    // The map in play: "<name> <layout> <blocks> <ramps> <hills> <spawns> <pickups> <digest>",
    // the digest over every number on it.
    std::string Layout()
    {
        std::lock_guard<std::mutex> g(m_mu);
        std::string st;
        auto put = [&st](const Fixed& f) { st += std::to_string(f.raw) + " "; };
        for (const auto& b : m_live.blocks) { put(b.x); put(b.z); put(b.w); put(b.d); put(b.h); st += std::to_string(b.yaw) + "|"; }
        for (const auto& r : m_live.ramps)  { put(r.x); put(r.z); put(r.width); put(r.up); put(r.flat); put(r.down); put(r.height); st += std::to_string(r.yaw) + "|"; }
        for (const auto& k : m_live.hills)  { put(k.x); put(k.z); put(k.radius); put(k.height); st += "|"; }
        for (const auto& p : m_live.spawns) { put(p.x); put(p.z); }
        for (const auto& p : m_live.pickups) { put(p.x); put(p.z); }
        char d[17];
        std::snprintf(d, sizeof d, "%016llx", static_cast<unsigned long long>(XXH3_64bits(st.data(), st.size())));
        return m_live.name + " " + std::to_string(m_layout) + " " + std::to_string(m_live.blocks.size()) + " "
             + std::to_string(m_live.ramps.size()) + " " + std::to_string(m_live.hills.size()) + " "
             + std::to_string(m_live.spawns.size()) + " " + std::to_string(m_live.pickups.size()) + " " + d;
    }
    std::string Hash()     { std::lock_guard<std::mutex> g(m_mu); return std::to_string(m_tick) + " " + hashLocked(); }
    std::string Snapshot() { std::lock_guard<std::mutex> g(m_mu); return std::to_string(m_snap_tick) + " " + m_snap_hash; }
    std::string Note()     { std::lock_guard<std::mutex> g(m_mu); return m_note; }
    // "<slot> <x> <z> <heading x> <heading z> <speed> <hp> <weapon> <ammo> <y> <air|ground>", or "-".
    std::string Kart(const std::string& who)
    {
        std::lock_guard<std::mutex> g(m_mu);
        const int i = slotOf(who);
        Causal_* k = i < 0 ? nullptr : causal(m_slots[i].body);
        if (!k) return "-";
        float x, y, z;
        {
            std::lock_guard<std::recursive_mutex> lk(k->TreeMutex());
            const OrderVector& r = k->Order4();
            x = r.x.ToFloat(); y = r.y.ToFloat(); z = r.z.ToFloat();
        }
        const Slot& s = m_slots[i];
        return std::to_string(i) + " " + fmt3(x) + " " + fmt3(z) + " " + fmt3(s.hx.ToFloat()) + " " + fmt3(s.hz.ToFloat()) + " "
             + fmt1(speedOf(static_cast<size_t>(i))) + " " + std::to_string(s.hp) + " " + KartArms::Name(s.weapon)
             + " " + std::to_string(s.ammo) + " " + fmt3(y) + " " + (s.air ? "air" : "ground");
    }
    /*
     * THE JUDGE'S CLOCK BY HAND: `ticks` ticks stepped and recorded, as the
     * frame edge does in real time -- what a headless run or a test drives a
     * battle with (Scene3D::Run's counterpart).
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
            // The host's clock is the battle's: whole ticks of wall time, at
            // most a few at once (a stalled frame is time lost, not a lurch).
            m_wall_ms += dt_ms;
            int due = static_cast<int>(m_wall_ms / kTickMs);
            m_wall_ms -= due * kTickMs;
            if (due > kMaxCatchUp) due = kMaxCatchUp;
            for (int i = 0; i < due; ++i) stepLocked();
            if (m_tick >= m_recorded + kBatch) flushTicksLocked();
        }
        cameraLocked(dt_ms);
        barsLocked();
        cardLocked(dt_ms);
        boardLocked();
    }

private:
    struct Slot
    {
        ETCS::RID   body = 0;
        ETCS::RID   wheels[4] = { 0, 0, 0, 0 };   // fl fr rl rr: drawn, placed by the battle
        ETCS::RID   bar_back = 0, bar_fill = 0;   // the HP bar: this runtime's to turn
        OrderVector pristine;                      // the rows as made: every restart starts here
        std::string driver;
        int         thr = 0, steer = 0, fire = 0;
        int         press = 0;                     // a press of the trigger the tick has not yet seen
        Fixed       wheel;                         // radians, + is left
        Fixed       hx, hz;                        // the heading, level (the body is tilted to the ground)
        Fixed       vy;                            // up: the ground's pace under it, or a flight's
        bool        air = false;                   // off the ground
        Fixed       sgx, sgz;                      // the slope it last stood on: its tilt in the air
        Fixed       px, pz;                        // where the step found it: a wall of ground puts it back
        int         hp = kFullHp;
        uint8_t     weapon = KartArms::None, ammo = 0;
        int         cooldown = 0, boost = 0;
        int         elims = 0, deaths = 0;         // this round's
        int         last_elims = 0, last_deaths = 0;   // the last round completed's: the lobby's
        int64_t     dead_until = -1;               // the tick it comes back; -1 alive
        int64_t     ram_safe = 0;                  // rammed: not again before this tick
        int         killer = -1;                   // who took it out last
        int         shown_hp = -1, shown_band = -1; bool bar_shown = false;   // drawn, not state
    };
    struct Pickup { Fixed x, y, z; int64_t back = 0; ETCS::RID node = 0; };
    struct Shot
    {
        uint8_t kind = 0; int owner = -1;
        Fixed x, y, z, vx, vy, vz;
        int life = 0, armed = 0, node = -1;
    };
    struct BoardRow { ETCS::RID rid; std::string text; bool shown; };
    enum Phase { Lobby, Playing, Results };
    enum MenuItem { MenuMap, MenuLength, MenuStart, MenuItems };

    static constexpr size_t   kMaxKarts   = 8;
    static constexpr int      kHz         = 50;
    static constexpr double   kTickMs     = 1000.0 / kHz;
    static constexpr int      kMaxCatchUp = 5;
    static constexpr uint64_t kBatch      = 2;
    static constexpr uint64_t kSnapEvery  = 250;
    static constexpr int      kFullHp     = 100;
    static constexpr int      kRespawn    = 3 * kHz;
    static constexpr int      kPickupBack = 8 * kHz;
    static constexpr int      kResults    = 6 * kHz;
    static constexpr float kColors[kMaxKarts][3] = {
        { 0.86f, 0.18f, 0.16f }, { 0.18f, 0.42f, 0.88f }, { 0.95f, 0.80f, 0.15f }, { 0.20f, 0.70f, 0.30f },
        { 0.95f, 0.50f, 0.10f }, { 0.60f, 0.30f, 0.80f }, { 0.20f, 0.80f, 0.85f }, { 0.92f, 0.92f, 0.90f } };

    static Fixed F(int64_t n, int64_t d = 1) { return Fixed::FromInt(n) / Fixed::FromInt(d); }
    static Fixed dt()        { return F(1, kHz); }
    static Fixed accel()     { return F(9); }
    static Fixed topSpeed()  { return F(16); }
    static Fixed boostSpeed(){ return F(26); }
    static Fixed brake()     { return F(20); }
    static Fixed reverseAcc(){ return F(5); }
    static Fixed reverseTop(){ return F(6); }
    static Fixed coast()     { return F(6, 10); }
    static Fixed wheelLock() { return F(45, 100); }
    static Fixed wheelRate() { return F(3); }
    static Fixed wheelbase() { return F(16, 10); }
    static Fixed grip()      { return F(9); }
    static Fixed kartY()     { return F(25, 100); }   // the ride height: the body's centre over the ground
    static Fixed kartR()     { return F(1); }          // the reach a shot meets
    static Fixed gravity()   { return F(20); }
    static Fixed climb()     { return F(35, 100); }   // more ground than this in one step is a wall
    static Fixed noseRise()  { return F(7, 10); }     // ...and this much under the nose (a slope of ~0.75)
    static Fixed bound()     { return F(31); }         // the arena's inside, less a kart's reach

    // ── the record ──────────────────────────────────────────────────────

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
    // The host's own lines for the lobby: a config -- a map made from a seed
    // carrying a new seed when `reroll`, or when it was not the map out, and
    // its own otherwise -- and a round, with its dice.
    bool configLocked(int m, uint32_t secs, bool reroll)
    {
        std::string line = "config " + std::to_string(m) + " " + std::to_string(secs);
        if (m_defs[m].generated)
            line += " " + std::to_string(reroll || m != m_map || !m_layout ? rollLocked() : m_layout);
        return takeLocked(m_me, line);
    }
    bool roundLocked() { return takeLocked(m_me, "round " + std::to_string(rollLocked())); }
    // A seed: rolled here, on the host, and recorded -- every runtime plays it.
    uint64_t rollLocked()
    {
        std::random_device rd;
        return ((static_cast<uint64_t>(rd()) << 32) ^ rd() ^ m_tick) | 1;
    }

    // One line, as its author: what every runtime does with the record.
    bool applyLocked(const std::string& author, const std::string& line)
    {
        std::istringstream in(line);
        std::string verb;
        in >> verb;
        const bool host = author == m_owner;
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
            int t = 0, s = 0, f = 0;
            if (i < 0 || !(in >> t >> s)) return false;
            in >> f;
            m_slots[i].thr   = t < 0 ? -1 : (t > 0 ? 1 : 0);
            m_slots[i].steer = s < 0 ? -1 : (s > 0 ? 1 : 0);
            // A press is kept until a tick sees it: a tap whose down and up
            // both land between two ticks (a busy host's frame) still fires.
            if (f > 0 && !m_slots[i].fire) m_slots[i].press = 1;
            m_slots[i].fire  = f > 0 ? 1 : 0;
            return true;
        }
        if (verb == "join")
        {
            if (slotOf(author) >= 0) return false;
            for (size_t i = 0; i < m_slots.size(); ++i)
                if (m_slots[i].driver.empty())
                {
                    Slot& s = m_slots[i];
                    s.driver = author;
                    s.elims = s.deaths = s.last_elims = s.last_deaths = 0;
                    spawnLocked(i);
                    noteLocked(author + " drives kart " + std::to_string(i + 1));
                    if (author == m_me && !m_judge && (m_sent_t || m_sent_s || m_sent_f))
                        m_outbox.push_back(driveLine(m_sent_t, m_sent_s, m_sent_f));
                    return true;
                }
            return false;   // a full arena: watching
        }
        if (!host) return false;   // the rest are the host's
        if (verb == "leave")
        {
            std::string who;
            in >> who;
            const int i = slotOf(who);
            if (i < 0) return false;
            pitLocked(static_cast<size_t>(i));
            return true;
        }
        if (verb == "config")
        {
            // The lobby's only: a round's settings are locked until it ends.
            int m = -1; uint32_t secs = 0; uint64_t layout = 0;
            if (m_phase != Lobby || !(in >> m >> secs) || m < 0 || m >= static_cast<int>(m_defs.size())
                || secs < 30 || secs > 900) return false;
            in >> layout;
            if (m_defs[m].generated != (layout != 0)) return false;
            m_round_secs = secs;
            mapLocked(m, layout);
            respawnAllLocked();
            noteLocked("next round: " + mapName(m_map) + ", " + clock(static_cast<int64_t>(m_round_secs) * kHz));
            return true;
        }
        if (verb == "round")
        {
            uint64_t seed = 0;
            if (m_phase != Lobby || !(in >> seed) || !seed) return false;
            m_rng = seed;
            mapLocked(m_map, m_layout);   // the boxes out afresh, the shots gone
            for (Slot& s : m_slots) s.elims = s.deaths = 0;
            m_phase = Playing;
            m_end = static_cast<int64_t>(m_tick) + static_cast<int64_t>(m_round_secs) * kHz;
            respawnAllLocked();
            noteLocked("the round is on: " + mapName(m_map) + ", " + clock(static_cast<int64_t>(m_round_secs) * kHz));
            return true;
        }
        if (verb == "abandon")
        {
            if (m_phase != Playing) return false;
            for (Slot& s : m_slots) s.elims = s.deaths = 0;   // discarded: the lobby keeps the last round completed
            lobbyLocked();
            noteLocked(author + " abandoned the round");
            return true;
        }
        if (verb == "give")
        {
            std::string who, w;
            in >> who >> w;
            const int i = slotOf(who);
            const uint8_t weapon = KartArms::ByName(w);
            if (i < 0 || weapon == KartArms::None) return false;
            m_slots[i].weapon = weapon; m_slots[i].ammo = KartArms::Ammo(weapon); m_slots[i].cooldown = 0;
            return true;
        }
        return false;
    }

    // ── the world ───────────────────────────────────────────────────────

    // Back to the start: every kart in the pit as made, the shots gone, the
    // lobby on the first map, the dice as made. What a session begins from on
    // every runtime, so two of them start the same.
    void restartLocked()
    {
        for (size_t i = 0; i < m_slots.size(); ++i) pitLocked(i);
        for (Shot& s : m_shots) freeNode(s.node);
        m_shots.clear();
        for (size_t b = 0; b < m_blast_nodes.size(); ++b) { m_blast_until[b] = 0; hide(m_blast_nodes[b]); }
        m_tick = m_recorded = m_seq = 0;
        m_wall_ms = 0.0;
        m_snaps.clear(); m_snap_tick = 0; m_snap_hash = "-"; m_differed_at = 0;
        m_keys = 0; m_sent_t = m_sent_s = m_sent_f = 0;
        m_eye_set = false; m_menu = MenuStart;
        m_phase = Lobby; m_end = 0; m_round_secs = 180;
        m_rng = 0x9E3779B97F4A7C15ull;
        if (!m_defs.empty()) mapLocked(0, firstLayout(0));
    }
    // A map made from a seed, out before anyone chose a seed: one from its place.
    uint64_t firstLayout(int m) const { return m < static_cast<int>(m_defs.size()) && m_defs[m].generated ? 0x5EEDull + static_cast<uint64_t>(m) : 0; }

    void lobbyLocked()
    {
        m_phase = Lobby;
        m_end = 0;
        for (Shot& s : m_shots) freeNode(s.node);
        m_shots.clear();
        respawnAllLocked();
    }
    void placeLocked(size_t i, Fixed x, Fixed z, Fixed dx, Fixed dz)
    {
        Slot& s = m_slots[i];
        Causal_* k = causal(s.body);
        if (!k) return;
        const Fixed l = Fixed::Length(dx, Fixed::Zero(), dz);
        if (l.IsPositive()) { s.hx = dx / l; s.hz = dz / l; } else { s.hx = Fixed::Zero(); s.hz = Fixed::One(); }
        Fixed gx, gz;
        const Fixed y = groundAt(x, z, &gx, &gz) + kartY();
        s.sgx = gx; s.sgz = gz; s.vy = Fixed::Zero(); s.air = false;
        std::lock_guard<std::recursive_mutex> lk(k->TreeMutex());
        OrderVector& r = k->RowsUnder();
        const uint64_t id = r.id;
        r = s.pristine;
        r.id = id;
        r.PlaceAt(x, y, z);
        poseLocked(s, r);
    }
    // The pit: hidden, held, out of everyone's way.
    void pitLocked(size_t i)
    {
        m_slots[i].driver.clear();
        parkLocked(i);
    }
    void parkLocked(size_t i)
    {
        Slot& s = m_slots[i];
        s.thr = s.steer = s.fire = s.press = 0; s.wheel = Fixed::Zero();
        s.hp = kFullHp; s.weapon = KartArms::None; s.ammo = 0; s.cooldown = s.boost = 0;
        s.dead_until = -1; s.ram_safe = 0;
        verb(s.body, "SetVisible", "0");
        verb(s.body, "SetAnchored", "1");
        for (ETCS::RID w : s.wheels) verb(w, "SetVisible", "0");
        placeLocked(i, F(200) + F(4) * F(static_cast<int64_t>(i)), Fixed::Zero(), Fixed::Zero(), Fixed::One());
        wheelsLocked(s);
    }
    /*
     * A SPAWN: of the map's spawns, the one furthest from every other kart in
     * play (the largest nearest distance; the first on a tie), facing the
     * arena's middle, on the ground there. Whole, unarmed -- and in the lobby
     * held where it stands until the round starts.
     */
    void spawnLocked(size_t i)
    {
        Slot& s = m_slots[i];
        Fixed bx = Fixed::Zero(), bz = Fixed::Zero(), best = F(-1);
        for (const KartArena::Spot& p : m_live.spawns)
        {
            Fixed nearest = F(1000000);
            for (size_t j = 0; j < m_slots.size(); ++j)
            {
                if (j == i || m_slots[j].driver.empty() || m_slots[j].dead_until >= 0) continue;
                Fixed jx, jy, jz;
                if (!where(j, jx, jy, jz)) continue;
                const Fixed d = (jx - p.x) * (jx - p.x) + (jz - p.z) * (jz - p.z);
                if (d < nearest) nearest = d;
            }
            if (nearest > best) { best = nearest; bx = p.x; bz = p.z; }
        }
        s.thr = s.steer = 0; s.press = 0; s.wheel = Fixed::Zero();
        s.hp = kFullHp; s.weapon = KartArms::None; s.ammo = 0; s.cooldown = s.boost = 0;
        s.dead_until = -1;
        Fixed dx = -bx, dz = -bz;
        if (dx.IsZero() && dz.IsZero()) dz = Fixed::One();
        placeLocked(i, bx, bz, dx, dz);
        wheelsLocked(s);
        verb(s.body, "SetAnchored", m_phase == Lobby ? "1" : "0");
        verb(s.body, "SetVisible", "1");
        for (ETCS::RID w : s.wheels) verb(w, "SetVisible", "1");
    }
    // Everyone afresh (a new map, a round, the lobby): out of play first,
    // then back in slot order, each at the spawn furthest from those already
    // back -- not from where the others stood before.
    void respawnAllLocked()
    {
        for (Slot& s : m_slots) if (!s.driver.empty()) s.dead_until = 0;
        for (size_t i = 0; i < m_slots.size(); ++i) if (!m_slots[i].driver.empty()) spawnLocked(i);
    }

    /*
     * A MAP OUT, drawn with the pools: each block a pooled solid box sized
     * and placed, each ramp's slopes and top a pooled slab, each hill a
     * pooled dome; what a pool has left over waits far below, hidden. The
     * boxes are set out on the ground, and the shots go.
     */
    void mapLocked(int m, uint64_t layout)
    {
        if (m_defs.empty()) return;
        if (m < 0 || m >= static_cast<int>(m_defs.size())) m = 0;
        m_map = m;
        m_live = m_defs[m];
        m_layout = m_live.generated ? layout : 0;
        if (m_live.generated) KartArena::Generate(m_layout, m_live);
        else KartArena::Prepare(m_live);
        drawMapLocked();
        for (Shot& s : m_shots) freeNode(s.node);
        m_shots.clear();
        m_pickups.clear();
        size_t n = 0;
        for (const KartArena::Spot& sp : m_live.pickups)
            if (n < m_pickup_nodes.size())
            {
                Pickup p; p.x = sp.x; p.z = sp.z; p.y = groundAt(sp.x, sp.z) + F(7, 10); p.back = 0; p.node = m_pickup_nodes[n++];
                m_pickups.push_back(p);
                placeNode(p.node, p.x, p.y, p.z);
                verb(p.node, "SetVisible", "1");
            }
        for (; n < m_pickup_nodes.size(); ++n) hide(m_pickup_nodes[n]);
    }
    void drawMapLocked()
    {
        m_live_blocks.clear();
        size_t nb = 0, ns = 0, nd = 0;
        for (const KartArena::Block& b : m_live.blocks)
        {
            if (nb >= m_block_nodes.size()) break;
            const ETCS::RID node = m_block_nodes[nb++];
            verb(node, "Create", fmt3(b.w.ToFloat()) + ", " + fmt3(b.h.ToFloat()) + ", " + fmt3(b.d.ToFloat()));
            poseNode(node, b.x, b.h * Fixed::Half(), b.z, b.yaw, Fixed::Zero(), Fixed::One());
            verb(node, "SetVisible", "1");
            m_live_blocks.push_back(node);
        }
        // A ramp: its slope up (a slab tilted by the rise), its top (a box
        // down to the ground), its slope down; a jump has no slope down -- its
        // lip stands over the drop.
        const Fixed T = F(3, 10);
        for (const KartArena::Ramp& r : m_live.ramps)
        {
            const Fixed len = r.length();
            auto slab = [&](Fixed from, Fixed run, bool rising) {
                if (ns >= m_slab_nodes.size() || !run.IsPositive()) return;
                const ETCS::RID node = m_slab_nodes[ns++];
                const Fixed slope = Fixed::Length(run, r.height, Fixed::Zero());
                const Fixed sn = r.height / slope, cs = run / slope;
                // The top face's middle, lowered half the slab along its normal.
                const Fixed mid = from + run * Fixed::Half() - len * Fixed::Half();
                const Fixed off = T * Fixed::Half() * sn;
                const Fixed along = rising ? mid + off : mid - off;
                const Fixed y = r.height * Fixed::Half() - T * Fixed::Half() * cs;
                verb(node, "Create", fmt3(r.width.ToFloat()) + ", " + fmt3(T.ToFloat()) + ", " + fmt3(slope.ToFloat()));
                // Pitched about its own across: its far end up when rising.
                poseNode(node, r.x + r.sx * along, y, r.z + r.cz * along, r.yaw, rising ? -sn : sn, cs);
                verb(node, "SetVisible", "1");
            };
            slab(Fixed::Zero(), r.up, true);
            if (r.flat.IsPositive() && ns < m_slab_nodes.size())
            {
                const ETCS::RID node = m_slab_nodes[ns++];
                const Fixed mid = r.up + r.flat * Fixed::Half() - len * Fixed::Half();
                verb(node, "Create", fmt3(r.width.ToFloat()) + ", " + fmt3(r.height.ToFloat()) + ", " + fmt3(r.flat.ToFloat()));
                poseNode(node, r.x + r.sx * mid, r.height * Fixed::Half(), r.z + r.cz * mid, r.yaw, Fixed::Zero(), Fixed::One());
                verb(node, "SetVisible", "1");
            }
            slab(r.up + r.flat, r.down, false);
        }
        for (const KartArena::Hill& k : m_live.hills)
        {
            if (nd >= m_dome_nodes.size()) break;
            const ETCS::RID node = m_dome_nodes[nd++];
            const std::string a = fmt3((k.a * F(2)).ToFloat()), b = fmt3((k.b * F(2)).ToFloat());
            verb(node, "Create", a + ", " + b + ", " + a);
            poseNode(node, k.x, k.y0, k.z, 0, Fixed::Zero(), Fixed::One());
            verb(node, "SetVisible", "1");
        }
        for (; nb < m_block_nodes.size(); ++nb) parkNode(m_block_nodes[nb]);
        for (; ns < m_slab_nodes.size(); ++ns) parkNode(m_slab_nodes[ns]);
        for (; nd < m_dome_nodes.size(); ++nd) parkNode(m_dome_nodes[nd]);
    }

    // ── one tick ────────────────────────────────────────────────────────

    void stepLocked()
    {
        Causal_* w = causal(m_world);
        if (!w) { ++m_tick; return; }
        {
            std::lock_guard<std::recursive_mutex> lk(w->TreeMutex());
            for (Slot& s : m_slots)
                if (alive(s))
                    if (Causal_* k = causal(s.body))
                    {
                        s.px = k->Order4().x; s.pz = k->Order4().z;
                        if (m_phase != Lobby) driveLocked(s, k);
                    }
            w->Interact(dt());
            ++m_tick;
            for (Slot& s : m_slots) if (alive(s)) groundLocked(s);
            if (m_phase != Lobby)
            {
                for (size_t i = 0; i < m_slots.size(); ++i) if (alive(m_slots[i])) touchLocked(i);
                for (size_t i = 0; i < m_slots.size(); ++i) if (alive(m_slots[i])) fireLocked(i);
            }
            shotsLocked();
            for (size_t i = 0; i < m_slots.size(); ++i)
            {
                Slot& s = m_slots[i];
                if (s.driver.empty()) continue;
                if (s.dead_until >= 0 && static_cast<int64_t>(m_tick) >= s.dead_until) spawnLocked(i);
                if (alive(s)) wheelsLocked(s);
            }
            if (m_phase != Lobby) pickupsLocked();
            for (size_t b = 0; b < m_blast_nodes.size(); ++b)
                if (m_blast_until[b] && static_cast<int64_t>(m_tick) >= m_blast_until[b]) { m_blast_until[b] = 0; hide(m_blast_nodes[b]); }
        }
        if (m_phase == Playing && static_cast<int64_t>(m_tick) >= m_end)
        {
            m_phase = Results;
            m_end = static_cast<int64_t>(m_tick) + kResults;
            for (Slot& s : m_slots) { s.last_elims = s.elims; s.last_deaths = s.deaths; }
            // A winner is one ahead of everyone; level at the top is a draw.
            const std::vector<size_t> rank = rankLocked();
            const bool clear = !rank.empty() && (rank.size() == 1
                || m_slots[rank[0]].elims != m_slots[rank[1]].elims || m_slots[rank[0]].deaths != m_slots[rank[1]].deaths);
            noteLocked(rank.empty() ? "the round is over" : clear ? "the round is over -- " + m_slots[rank[0]].driver + " wins"
                                                                 : std::string("the round is over -- a draw"));
        }
        else if (m_phase == Results && static_cast<int64_t>(m_tick) >= m_end) lobbyLocked();
        if (m_tick % kSnapEvery == 0)
        {
            m_snap_tick = m_tick;
            m_snap_hash = hashLocked();
            m_snaps.push_back({ m_snap_tick, m_snap_hash });
            if (m_snaps.size() > 16) m_snaps.erase(m_snaps.begin());
        }
    }

    // The bicycle, on the rows, under the tree's lock; a boost lifts the top
    // speed and doubles the pedal, and the ground's slope pulls along the
    // heading. In the air there is nothing to push on; between rounds'
    // results nobody drives.
    void driveLocked(Slot& s, Causal_* k)
    {
        OrderVector& r = k->RowsUnder();
        const Fixed m = k->MassUnder();
        const Fixed step = dt();
        const int thr = m_phase == Results ? 0 : s.thr, steer = m_phase == Results ? 0 : s.steer;
        const Fixed target = wheelLock() * F(steer);
        const Fixed turn = wheelRate() * step;
        Fixed d = target - s.wheel;
        if (d > turn) d = turn; else if (d < -turn) d = -turn;
        s.wheel += d;
        if (s.boost > 0) --s.boost;
        if (s.air) return;
        const Fixed hx = s.hx, hz = s.hz;
        Fixed vx, vy, vz;
        r.Velocity(m, vx, vy, vz);
        Fixed speed = vx * hx + vz * hz;
        Fixed lx = vx - hx * speed, lz = vz - hz * speed;
        const Fixed top = s.boost > 0 ? boostSpeed() : topSpeed();
        const Fixed push = s.boost > 0 ? accel() * F(2) : accel();
        if (thr > 0)
        {
            if (speed.raw < 0) speed = Fixed::Min(speed + brake() * step, Fixed::Zero());
            else if (speed < top) speed = Fixed::Min(speed + push * step, top);
        }
        else if (thr < 0)
        {
            if (speed.raw > 0) speed = Fixed::Max(speed - brake() * step, Fixed::Zero());
            else               speed = Fixed::Max(speed - reverseAcc() * step, -reverseTop());
        }
        else
        {
            speed -= speed * coast() * step;
            if (speed.Abs() < F(1, 100)) speed = Fixed::Zero();
        }
        // The slope: gravity's share along the heading (the sine of the rise).
        Fixed gx, gz;
        groundAt(r.x, r.z, &gx, &gz);
        const Fixed along = gx * hx + gz * hz;
        if (!along.IsZero()) speed -= gravity() * along / (Fixed::One() + along * along).Sqrt() * step;
        // Past the kart's own top (a boost run out, a hill run down) it eases
        // back on the level; nothing goes past a boost's top.
        if (speed > top && along.raw >= 0) speed = Fixed::Max(top, speed - brake() * Fixed::Half() * step);
        if (speed > boostSpeed()) speed = boostSpeed();
        // The heading turns by speed x wheel / wheelbase, about +y: a turn of
        // +a takes +x toward -z, which with +z ahead is to the left.
        const Fixed yaw = speed * s.wheel / wheelbase() * step;
        if (!yaw.IsZero())
        {
            Fixed sn, cs;
            yaw.SinCos(sn, cs);
            const Fixed nx = hx * cs + hz * sn, nz = hz * cs - hx * sn;
            const Fixed l = Fixed::Length(nx, Fixed::Zero(), nz);
            s.hx = nx / l; s.hz = nz / l;
        }
        Fixed keep = Fixed::One() - grip() * step;
        if (keep.raw < 0) keep = Fixed::Zero();
        lx *= keep; lz *= keep;
        const Fixed nx = s.hx * speed + lx, nz = s.hz * speed + lz;
        const Fixed after = Fixed::Half() * m * (nx * nx + nz * nz);
        const Fixed before = r.KineticEnergy();
        if (after > before) k->Impulse(nx, Fixed::Zero(), nz, after - before);
        r.SetVelocity(m, nx, Fixed::Zero(), nz);
    }

    /*
     * THE GROUND UNDER A KART, after the step moved it: on the ground it
     * rides at the ground's height and keeps its pace up and down; where the
     * ground falls away faster than gravity can follow (a lip) it flies, and
     * lands where it meets the ground again. Ground rising more than a step
     * can climb, at the kart or under its nose, is a wall: the kart is put
     * back where the step found it, stopped. The arena's walls bound it all.
     */
    void groundLocked(Slot& s)
    {
        Causal_* k = causal(s.body);
        if (!k) return;
        OrderVector& r = k->RowsUnder();
        const Fixed m = k->MassUnder();
        Fixed vx, vy, vz;
        r.Velocity(m, vx, vy, vz);
        Fixed gx, gz;
        Fixed g = groundAt(r.x, r.z, &gx, &gz) + kartY();
        const Fixed sp = Fixed::Length(vx, Fixed::Zero(), vz);
        bool wall = g - r.y > climb();
        if (!wall && sp.IsPositive() && !s.air)
        {
            const Fixed nx = r.x + vx / sp * F(9, 10), nz = r.z + vz / sp * F(9, 10);
            wall = groundAt(nx, nz) + kartY() - r.y > noseRise();
        }
        if (wall)
        {
            r.PlaceAt(s.px, r.y, s.pz);
            r.SetVelocity(m, Fixed::Zero(), Fixed::Zero(), Fixed::Zero());
            g = groundAt(s.px, s.pz, &gx, &gz) + kartY();
        }
        Fixed y = r.y;
        const Fixed step = dt();
        if (!s.air)
        {
            const Fixed flying = y + s.vy * step - gravity() * step * step * Fixed::Half();
            if (g < flying - F(2, 100)) { s.air = true; y = flying; s.vy -= gravity() * step; }
            else { s.vy = (g - y) / step; y = g; s.sgx = gx; s.sgz = gz; }
        }
        else
        {
            s.vy -= gravity() * step;
            y += s.vy * step;
            if (!(y > g)) { y = g; s.air = false; s.vy = Fixed::Zero(); s.sgx = gx; s.sgz = gz; }
        }
        Fixed x = r.x, z = r.z;
        const bool cx = x.Abs() > bound(), cz = z.Abs() > bound();
        if (cx) x = x.raw > 0 ? bound() : -bound();
        if (cz) z = z.raw > 0 ? bound() : -bound();
        if (cx || cz)
        {
            Fixed ux, uy, uz;
            r.Velocity(m, ux, uy, uz);
            r.SetVelocity(m, cx ? Fixed::Zero() : ux, Fixed::Zero(), cz ? Fixed::Zero() : uz);
        }
        r.PlaceAt(x, y, z);
        poseLocked(s, r);
    }
    // The body's turn: level to the heading, then tilted to the slope it
    // stands on (or last stood on, in the air).
    static void poseLocked(const Slot& s, OrderVector& r)
    {
        Fixed yw, yy;   // the heading's turn about +y: +z onto (hx, hz)
        halfTurn(s.hx, s.hz, yw, yy);
        // Up onto the ground's normal (-gx, 1, -gz): the half-way spinor.
        const Fixed nl = Fixed::Length(-s.sgx, Fixed::One(), -s.sgz);
        const Fixed nx = -s.sgx / nl, ny = Fixed::One() / nl, nz = -s.sgz / nl;
        Fixed tw = Fixed::One() + ny, tx = nz, tz = -nx;
        const Fixed tl = Fixed::Length(tw, tx, tz);
        tw = tw / tl; tx = tx / tl; tz = tz / tl;
        // tilt x yaw: (tw, tx, 0, tz) x (yw, 0, yy, 0).
        r.qw = tw * yw;
        r.qx = tx * yw - tz * yy;
        r.qy = tw * yy;
        r.qz = tz * yw + tx * yy;
    }
    // +z turned about +y onto (dx, dz), as a half-angle pair (w, y); straight
    // back is a half turn.
    static void halfTurn(Fixed dx, Fixed dz, Fixed& w, Fixed& y)
    {
        const Fixed l = Fixed::Length(dx, Fixed::Zero(), dz);
        if (!l.IsPositive()) { w = Fixed::One(); y = Fixed::Zero(); return; }
        const Fixed ux = dx / l, uz = dz / l;
        const Fixed a = Fixed::One() + uz;
        if (a.raw <= (Fixed::ONE >> 20)) { w = Fixed::Zero(); y = Fixed::One(); return; }
        const Fixed n = Fixed::Length(a, ux, Fixed::Zero());
        w = a / n; y = ux / n;
    }
    Fixed groundAt(Fixed x, Fixed z, Fixed* gx = nullptr, Fixed* gz = nullptr) const { return KartArena::Height(m_live, x, z, gx, gz); }

    // What a kart touches after the step: a kart it rams.
    void touchLocked(size_t i)
    {
        Slot& s = m_slots[i];
        Fixed x, y, z;
        if (!where(i, x, y, z)) return;
        // A ram: boosting, at speed, into a kart not rammed a moment ago.
        if (s.boost > 0 && speedFixed(i) > F(14))
            for (size_t j = 0; j < m_slots.size(); ++j)
            {
                if (j == i || !alive(m_slots[j]) || m_slots[j].ram_safe > static_cast<int64_t>(m_tick)) continue;
                Fixed jx, jy, jz;
                if (!where(j, jx, jy, jz)) continue;
                if ((jx - x) * (jx - x) + (jy - y) * (jy - y) + (jz - z) * (jz - z) < F(22, 10) * F(22, 10))
                {
                    m_slots[j].ram_safe = static_cast<int64_t>(m_tick) + 25;
                    damageLocked(j, static_cast<int>(i), 20);
                }
            }
    }

    // ── the weapons ─────────────────────────────────────────────────────

    // The trigger: on its press, or for as long as it is held (the minigun).
    void fireLocked(size_t i)
    {
        Slot& s = m_slots[i];
        const bool pressed = s.press != 0;
        s.press = 0;
        if (s.cooldown > 0) --s.cooldown;
        if (s.weapon == KartArms::None || m_phase == Results) return;
        const bool held = s.weapon == KartArms::Minigun ? ((s.fire || pressed) && s.cooldown == 0) : pressed;
        if (!held) return;
        Fixed x, y, z;
        if (!where(i, x, y, z)) return;
        const Fixed hx = s.hx, hz = s.hz;
        // Shots leave at the kart's own height, level: a kart on the same
        // ground is met at its middle.
        const Fixed fx = x + hx * F(15, 10), fz = z + hz * F(15, 10), fy = y;
        using namespace KartArms;
        switch (s.weapon)
        {
            case Rocket:  shootLocked(RocketShot, i, fx, fy, fz, hx * F(30), Fixed::Zero(), hz * F(30), 100); break;
            case Mine:
            {
                const Fixed mx = x - hx * F(2), mz = z - hz * F(2);
                Shot& m = shootLocked(MineShot, i, mx, groundAt(mx, mz) + F(1, 10), mz, Fixed::Zero(), Fixed::Zero(), Fixed::Zero(), 30 * kHz);
                m.armed = 25;
                break;
            }
            case Shotgun:
                for (int p = -2; p <= 2; ++p)
                {
                    Fixed sn, cs;
                    (F(12, 100) * F(p)).SinCos(sn, cs);
                    const Fixed dx = hx * cs + hz * sn, dz = hz * cs - hx * sn;
                    shootLocked(Pellet, i, fx, fy, fz, dx * F(40), Fixed::Zero(), dz * F(40), 15);
                }
                break;
            case Boost:   s.boost = 2 * kHz; break;
            case Bomb:    shootLocked(BombShot, i, x + hx * F(12, 10), y + F(55, 100), z + hz * F(12, 10), hx * F(14), F(9), hz * F(14), 3 * kHz); break;
            case Minigun: shootLocked(Bullet, i, fx, fy, fz, hx * F(45), Fixed::Zero(), hz * F(45), 25); s.cooldown = 5; break;
            case Fuse:    shootLocked(FuseShot, i, x, y + F(12, 10), z, Fixed::Zero(), Fixed::Zero(), Fixed::Zero(), 125); break;
            case Sniper:  sniperLocked(i, x, fy, z, hx, hz); break;
            default: break;
        }
        if (s.ammo > 0 && --s.ammo == 0) s.weapon = None;
    }

    Shot& shootLocked(uint8_t kind, size_t owner, Fixed x, Fixed y, Fixed z, Fixed vx, Fixed vy, Fixed vz, int life)
    {
        Shot s; s.kind = kind; s.owner = static_cast<int>(owner);
        s.x = x; s.y = y; s.z = z; s.vx = vx; s.vy = vy; s.vz = vz; s.life = life;
        s.node = takeNode();
        if (s.node >= 0)
        {
            using namespace KartArms;
            const ETCS::RID n = m_shot_nodes[s.node];
            const char* size = kind == RocketShot ? "0.35, 0.35, 0.9" : kind == Pellet ? "0.22, 0.22, 0.22"
                             : kind == Bullet ? "0.16, 0.16, 0.4" : kind == BombShot ? "0.55, 0.55, 0.55"
                             : kind == MineShot ? "0.9, 0.18, 0.9" : kind == FuseShot ? "0.5, 0.5, 0.5" : "0.1, 0.1, 1.0";
            const char* colour = kind == RocketShot ? "0.95, 0.35, 0.10, 1.0" : kind == Pellet ? "1.0, 0.85, 0.3, 1.0"
                               : kind == Bullet ? "1.0, 0.95, 0.6, 1.0" : kind == BombShot ? "0.12, 0.12, 0.14, 1.0"
                               : kind == MineShot ? "0.55, 0.08, 0.08, 1.0" : kind == FuseShot ? "1.0, 0.15, 0.1, 1.0" : "0.4, 0.95, 1.0, 1.0";
            verb(n, "Create", size);
            verb(n, "SetColor", colour);
            nodeAtLocked(s);
            verb(n, "SetVisible", "1");
        }
        m_shots.push_back(s);
        return m_shots.back();
    }

    // The sniper: an instant line, level from the kart's middle, to the first
    // kart it meets, stopped by a wall, a block or ground higher than the
    // shooter's own. 100 dead on, 50 at the very edge of the kart.
    void sniperLocked(size_t i, Fixed x, Fixed y, Fixed z, Fixed hx, Fixed hz)
    {
        int hit = -1; Fixed best = F(91), miss;
        for (size_t j = 0; j < m_slots.size(); ++j)
        {
            if (j == i || !alive(m_slots[j])) continue;
            Fixed jx, jy, jz;
            if (!where(j, jx, jy, jz)) continue;
            const Fixed along = (jx - x) * hx + (jz - z) * hz;
            if (along.raw <= 0 || along > F(90)) continue;
            const Fixed px = jx - (x + hx * along), pz = jz - (z + hz * along);
            const Fixed off = Fixed::Length(px, jy - y, pz);
            if (off > kartR() || !(along < best)) continue;
            best = along; hit = static_cast<int>(j); miss = off;
        }
        Fixed reach = hit >= 0 ? best : F(90);
        for (Fixed t = F(1); t < reach; t += F(1, 2))
        {
            const Fixed sx = x + hx * t, sz = z + hz * t;
            if (solidAt(sx, y, sz, F(1, 10)) || groundAt(sx, sz) > y - F(15, 100)) { reach = t; hit = -1; break; }
        }
        Shot& tr = shootLocked(KartArms::Tracer, i, x + hx * reach * Fixed::Half(), y, z + hz * reach * Fixed::Half(),
                               hx, Fixed::Zero(), hz, 8);
        if (tr.node >= 0) verb(m_shot_nodes[tr.node], "Create", "0.1, 0.1, " + fmt1(reach.ToFloat()));
        if (hit >= 0) damageLocked(static_cast<size_t>(hit), static_cast<int>(i), 100 - static_cast<int>((F(50) * miss / kartR()).raw >> 32));
    }

    // Every shot one tick on: flown, met, burst or spent.
    void shotsLocked()
    {
        using namespace KartArms;
        std::vector<Shot> keep;
        keep.reserve(m_shots.size());
        for (Shot s : m_shots)
        {
            bool gone = false;
            --s.life;
            switch (s.kind)
            {
                case RocketShot: case Pellet: case Bullet: case BombShot:
                {
                    if (s.kind == BombShot) s.vy -= gravity() * dt();
                    s.x += s.vx * dt(); s.y += s.vy * dt(); s.z += s.vz * dt();
                    const int hit = kartAt(s.x, s.y, s.z, s.owner, F(25, 100));
                    const bool wall = solidAt(s.x, s.y, s.z, F(2, 10));
                    const bool ground = s.y < groundAt(s.x, s.z) + (s.kind == BombShot ? F(3, 10) : F(5, 100));
                    if (hit >= 0 || wall || ground || s.life <= 0)
                    {
                        gone = true;
                        if (s.kind == RocketShot)    burstLocked(s.x, s.y, s.z, F(4), 40, s.owner);
                        else if (s.kind == BombShot) burstLocked(s.x, s.y, s.z, F(45, 10), 40, s.owner);
                        else if (hit >= 0)           damageLocked(static_cast<size_t>(hit), s.owner, s.kind == Pellet ? 10 : 4);
                    }
                    break;
                }
                case MineShot:
                    if (s.armed > 0) --s.armed;
                    else if (kartAt(s.x, s.y, s.z, s.owner, F(6, 10)) >= 0) { gone = true; burstLocked(s.x, s.y, s.z, F(25, 10), 30, s.owner); }
                    if (s.life <= 0) gone = true;
                    break;
                case FuseShot:
                {
                    Fixed ox, oy, oz;
                    if (s.owner < 0 || !alive(m_slots[s.owner]) || !where(static_cast<size_t>(s.owner), ox, oy, oz)) { gone = true; break; }
                    s.x = ox; s.y = oy + F(12, 10); s.z = oz;
                    if (s.life <= 0) { gone = true; burstLocked(ox, oy, oz, F(6), 50, s.owner); }
                    break;
                }
                default: if (s.life <= 0) gone = true; break;   // a tracer fades
            }
            if (gone) freeNode(s.node);
            else { nodeAtLocked(s); keep.push_back(s); }
        }
        m_shots.swap(keep);
    }

    // A burst: everyone in reach but the one who set it off, by how near.
    void burstLocked(Fixed x, Fixed y, Fixed z, Fixed radius, int max, int owner)
    {
        for (size_t j = 0; j < m_slots.size(); ++j)
        {
            if (static_cast<int>(j) == owner || !alive(m_slots[j])) continue;
            Fixed jx, jy, jz;
            if (!where(j, jx, jy, jz)) continue;
            const Fixed d = Fixed::Length(jx - x, jy - y, jz - z) - kartR();
            const int dmg = KartArms::Falloff(d, radius, max);
            if (dmg > 0) damageLocked(j, owner, dmg);
        }
        blastLocked(x, y, z, radius);
    }
    void blastLocked(Fixed x, Fixed y, Fixed z, Fixed radius)
    {
        for (size_t b = 0; b < m_blast_nodes.size(); ++b)
        {
            if (m_blast_until[b]) continue;
            m_blast_until[b] = static_cast<int64_t>(m_tick) + 12;
            const std::string d = fmt1(radius.ToFloat() * 2.0f);
            verb(m_blast_nodes[b], "Create", d + ", " + d + ", " + d);
            placeNode(m_blast_nodes[b], x, y, z);
            verb(m_blast_nodes[b], "SetVisible", "1");
            return;
        }
    }

    // Damage, and what it comes to: at nothing, one to whoever did it, one
    // against the kart, and three seconds out. Only in a round.
    void damageLocked(size_t victim, int attacker, int amount)
    {
        Slot& v = m_slots[victim];
        if (!alive(v) || amount <= 0 || m_phase != Playing) return;
        v.hp -= amount;
        if (v.hp > 0) return;
        v.hp = 0;
        v.killer = attacker;
        ++v.deaths;
        if (attacker >= 0 && attacker != static_cast<int>(victim)) ++m_slots[attacker].elims;
        Fixed x, y, z;
        if (where(victim, x, y, z)) blastLocked(x, y, z, F(2));
        const std::string by = attacker >= 0 && attacker != static_cast<int>(victim) ? m_slots[attacker].driver : std::string("the arena");
        noteLocked(by + " took out " + v.driver);
        const int64_t back = static_cast<int64_t>(m_tick) + kRespawn;
        parkLocked(victim);
        v.hp = 0;   // out: nothing, until the spawn makes it whole
        v.dead_until = back;
    }

    // The power-ups: a kart that drives over one gets a weapon off the dice,
    // in place of what it had; the box is back eight seconds later.
    void pickupsLocked()
    {
        for (Pickup& p : m_pickups)
        {
            if (p.back > static_cast<int64_t>(m_tick))
            {
                if (p.back == static_cast<int64_t>(m_tick) + 1) { placeNode(p.node, p.x, p.y, p.z); verb(p.node, "SetVisible", "1"); }
                continue;
            }
            const int j = kartAt(p.x, p.y, p.z, -1, F(8, 10));
            if (j >= 0)
            {
                Slot& s = m_slots[j];
                s.weapon = static_cast<uint8_t>(1 + KartArms::Next(m_rng) % (KartArms::Count - 1));
                s.ammo = KartArms::Ammo(s.weapon);
                s.cooldown = 0;
                if (s.driver == m_me) noteLocked(std::string("you have a ") + KartArms::Name(s.weapon));
                p.back = static_cast<int64_t>(m_tick) + kPickupBack;
                verb(p.node, "SetVisible", "0");
                continue;
            }
            // Turning in place, as power-ups do.
            if (Causal_* c = causal(p.node))
            {
                std::lock_guard<std::recursive_mutex> lk(c->TreeMutex());
                c->RowsUnder().RotateBy(Fixed::Zero(), Fixed::One(), Fixed::Zero(), F(6, 100));
            }
        }
    }

    // ── where things are ────────────────────────────────────────────────

    bool where(size_t i, Fixed& x, Fixed& y, Fixed& z)
    {
        Causal_* k = causal(m_slots[i].body);
        if (!k) return false;
        std::lock_guard<std::recursive_mutex> lk(k->TreeMutex());
        x = k->Order4().x; y = k->Order4().y; z = k->Order4().z;
        return true;
    }
    Fixed speedFixed(size_t i)
    {
        Causal_* k = causal(m_slots[i].body);
        if (!k) return Fixed::Zero();
        std::lock_guard<std::recursive_mutex> lk(k->TreeMutex());
        Fixed vx, vy, vz;
        k->Order4().Velocity(k->MassUnder(), vx, vy, vz);
        return Fixed::Length(vx, vy, vz);
    }
    // The first kart in play (not `except`) within its reach plus `r` of a point.
    int kartAt(Fixed x, Fixed y, Fixed z, int except, Fixed r)
    {
        const Fixed reach = kartR() + r;
        for (size_t j = 0; j < m_slots.size(); ++j)
        {
            if (static_cast<int>(j) == except || !alive(m_slots[j])) continue;
            Fixed jx, jy, jz;
            if (!where(j, jx, jy, jz)) continue;
            if ((jx - x) * (jx - x) + (jy - y) * (jy - y) + (jz - z) * (jz - z) < reach * reach) return static_cast<int>(j);
        }
        return -1;
    }
    // Is a point (a sphere of r) inside a wall or a block of the map in play?
    // Asked of each one's planes in its own frame (Planes::SphereBox).
    bool solidAt(Fixed x, Fixed y, Fixed z, Fixed r)
    {
        auto inside = [&](ETCS::RID rid) {
            Causal_* c = causal(rid);
            if (!c) return false;
            const CausalSolid sol = c->Solid();
            if (sol.shape != CausalSolid::Box) return false;
            std::lock_guard<std::recursive_mutex> lk(c->TreeMutex());
            const OrderVector& o = c->Order4();
            Fixed lx = x - o.x, ly = y - o.y, lz = z - o.z;
            o.UnrotateVector(lx, ly, lz);
            return Planes::SphereBox(sol.hx, sol.hy, sol.hz, lx, ly, lz, r, Fixed::Zero()).touching;
        };
        for (ETCS::RID w : m_walls) if (inside(w)) return true;
        for (ETCS::RID b : m_live_blocks) if (inside(b)) return true;
        return false;
    }
    bool alive(const Slot& s) const { return !s.driver.empty() && s.dead_until < 0; }

    // The wheels at the body's corners, turned with it (tilt and all), the
    // front two turned by the wheel.
    void wheelsLocked(const Slot& s)
    {
        Causal_* k = causal(s.body);
        if (!k) return;
        std::lock_guard<std::recursive_mutex> lk(k->TreeMutex());
        const OrderVector& r = k->Order4();
        const Fixed side = F(70, 100), ahead = F(64, 100), down = F(3, 100);
        for (int i = 0; i < 4; ++i)
        {
            Causal_* w = causal(s.wheels[i]);
            if (!w) continue;
            OrderVector& q = w->RowsUnder();
            // In the body's own frame: +z ahead, the front left wheel at +x
            // (the heading's across, (hz, -hx), on the level).
            Fixed ox = (i % 2) ? -side : side, oy = -down, oz = (i < 2) ? ahead : -ahead;
            r.RotateVector(ox, oy, oz);
            q.PlaceAt(r.x + ox, r.y + oy, r.z + oz);
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

    // ── the drawn things the battle places ──────────────────────────────

    void placeNode(ETCS::RID node, Fixed x, Fixed y, Fixed z)
    {
        Causal_* c = causal(node);
        if (!c) return;
        std::lock_guard<std::recursive_mutex> lk(c->TreeMutex());
        c->RowsUnder().PlaceAt(x, y, z);
    }
    // A pooled node placed and turned: `yaw` degrees about +y, then pitched
    // about its own across by the angle whose sine and cosine are given.
    void poseNode(ETCS::RID node, Fixed x, Fixed y, Fixed z, int yaw, Fixed psin, Fixed pcos)
    {
        Causal_* c = causal(node);
        if (!c) return;
        Fixed ys, yc;
        (Fixed::TwoPi() * F(yaw, 720)).SinCos(ys, yc);   // half the yaw
        // Half the pitch from its sine and cosine: cos/2 = sqrt((1+c)/2), sin/2 by the sign.
        const Fixed pc = ((Fixed::One() + pcos) * Fixed::Half()).Sqrt();
        Fixed ps = ((Fixed::One() - pcos) * Fixed::Half()).Sqrt();
        if (psin.raw < 0) ps = -ps;
        std::lock_guard<std::recursive_mutex> lk(c->TreeMutex());
        OrderVector& r = c->RowsUnder();
        r.PlaceAt(x, y, z);
        // yaw x pitch: (yc, 0, ys, 0) x (pc, ps, 0, 0)
        r.qw = yc * pc; r.qx = yc * ps; r.qy = ys * pc; r.qz = -(ys * ps);
    }
    // A pooled node not in use: far below, hidden.
    void parkNode(ETCS::RID node)
    {
        placeNode(node, F(0), F(-300), F(0));
        hide(node);
    }
    void nodeAtLocked(const Shot& s)
    {
        if (s.node < 0) return;
        Causal_* c = causal(m_shot_nodes[s.node]);
        if (!c) return;
        std::lock_guard<std::recursive_mutex> lk(c->TreeMutex());
        OrderVector& r = c->RowsUnder();
        r.PlaceAt(s.x, s.y, s.z);
        if (!s.vx.IsZero() || !s.vz.IsZero())
        {
            Fixed w, y;
            halfTurn(s.vx, s.vz, w, y);
            r.qw = w; r.qx = r.qz = Fixed::Zero(); r.qy = y;
        }
    }
    int takeNode()
    {
        for (size_t n = 0; n < m_shot_busy.size(); ++n) if (!m_shot_busy[n]) { m_shot_busy[n] = true; return static_cast<int>(n); }
        return -1;   // more in the air than there are to draw: flown, not drawn
    }
    void freeNode(int n)
    {
        if (n < 0 || n >= static_cast<int>(m_shot_busy.size())) return;
        m_shot_busy[n] = false;
        hide(m_shot_nodes[n]);
    }
    static void hide(ETCS::RID r) { verb(r, "SetVisible", "0"); }

    // ── the battle as one number ────────────────────────────────────────

    std::string hashLocked()
    {
        std::string st = std::to_string(m_tick) + " " + std::to_string(m_phase) + " " + std::to_string(m_end) + " "
                       + std::to_string(m_map) + " " + std::to_string(m_layout) + " " + std::to_string(m_round_secs)
                       + " " + std::to_string(m_rng);
        for (const Slot& s : m_slots)
        {
            st += "|" + s.driver + " " + std::to_string(s.thr) + std::to_string(s.steer) + std::to_string(s.fire)
                + std::to_string(s.press) + " " + std::to_string(s.wheel.raw) + " " + std::to_string(s.hx.raw) + " "
                + std::to_string(s.hz.raw) + " " + std::to_string(s.vy.raw) + " " + (s.air ? "a" : "g") + " "
                + std::to_string(s.hp) + " " + std::to_string(s.weapon) + " " + std::to_string(s.ammo) + " "
                + std::to_string(s.cooldown) + " " + std::to_string(s.boost) + " " + std::to_string(s.elims) + " "
                + std::to_string(s.deaths) + " " + std::to_string(s.last_elims) + " " + std::to_string(s.last_deaths) + " "
                + std::to_string(s.dead_until) + " " + std::to_string(s.ram_safe);
            if (Causal_* k = causal(s.body))
            {
                std::lock_guard<std::recursive_mutex> lk(k->TreeMutex());
                const OrderVector& r = k->Order4();
                const Fixed v[] = { r.x, r.y, r.z, r.ox, r.oy, r.oz, r.energy, r.qx, r.qy, r.qz, r.qw };
                for (const Fixed& f : v) st += " " + std::to_string(f.raw);
            }
        }
        for (const Shot& s : m_shots)
            st += "|s" + std::to_string(s.kind) + " " + std::to_string(s.owner) + " " + std::to_string(s.x.raw) + " "
                + std::to_string(s.y.raw) + " " + std::to_string(s.z.raw) + " " + std::to_string(s.life) + " " + std::to_string(s.armed);
        for (const Pickup& p : m_pickups) st += "|p" + std::to_string(p.back);
        char buf[17];
        std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(XXH3_64bits(st.data(), st.size())));
        return buf;
    }

    // ── what this runtime alone draws ───────────────────────────────────

    // Behind this runtime's own kart, eased; over the arena in the lobby and
    // for one with no kart in play.
    void cameraLocked(double dt_ms)
    {
        if (!m_camera) return;
        ETCS::Held<Camera_> cam = ETCS::resolve_held<Camera_>("Camera", m_camera);
        if (!cam) return;
        float eye[3] = { 0.0f, 62.0f, -46.0f }, at[3] = { 0.0f, 0.0f, 0.0f };
        const int i = slotOf(m_me);
        if (m_phase != Lobby && i >= 0 && alive(m_slots[i]))
            if (Causal_* k = causal(m_slots[i].body))
            {
                std::lock_guard<std::recursive_mutex> lk(k->TreeMutex());
                const OrderVector& r = k->Order4();
                const float px = r.x.ToFloat(), py = r.y.ToFloat(), pz = r.z.ToFloat();
                const float fx = m_slots[i].hx.ToFloat(), fz = m_slots[i].hz.ToFloat();
                eye[0] = px - fx * 7.5f; eye[1] = py + 3.6f; eye[2] = pz - fz * 7.5f;
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
        if (m_eye_set && moved < 1e-4f) return;
        m_eye_set = true;
        ViewFrustum v = cam->GetView();
        v.position = Point3D{ m_eye[0], m_eye[1], m_eye[2] };
        v.look_at  = Point3D{ m_at[0], m_at[1], m_at[2] };
        v.up       = Point3D{ 0.0f, 1.0f, 0.0f };
        cam->SetView(v);
    }
    /*
     * THE HP BARS, over every kart in play, turned to face this runtime's eye:
     * a dark back and a fill as long as the health, green to yellow to red.
     * This runtime's alone (its eye is), so placed here on the frame edge and
     * never hashed; nothing meets them (anchored, not solid).
     */
    void barsLocked()
    {
        for (Slot& s : m_slots)
        {
            const bool show = alive(s);
            if (show != s.bar_shown)
            {
                verb(s.bar_back, "SetVisible", show ? "1" : "0");
                verb(s.bar_fill, "SetVisible", show && s.hp > 0 ? "1" : "0");
                s.bar_shown = show;
                s.shown_hp = -1;
            }
            if (!show) continue;
            Causal_* k = causal(s.body);
            Causal_* back = causal(s.bar_back);
            Causal_* fill = causal(s.bar_fill);
            if (!k || !back || !fill) continue;
            if (s.hp != s.shown_hp)
            {
                const float frac = static_cast<float>(s.hp) / kFullHp;
                verb(s.bar_fill, "Create", fmt3(1.7f * frac + 0.001f) + ", 0.2, 0.1");
                const int band = s.hp > 60 ? 2 : s.hp > 30 ? 1 : 0;
                if (band != s.shown_band)
                {
                    verb(s.bar_fill, "SetColor", band == 2 ? "0.25, 0.85, 0.3, 1.0" : band == 1 ? "0.95, 0.8, 0.2, 1.0" : "0.95, 0.2, 0.15, 1.0");
                    s.shown_band = band;
                }
                s.shown_hp = s.hp;
            }
            float px, py, pz;
            {
                std::lock_guard<std::recursive_mutex> lk(k->TreeMutex());
                px = k->Order4().x.ToFloat(); py = k->Order4().y.ToFloat(); pz = k->Order4().z.ToFloat();
            }
            // Square to the line from the eye: about +y, +x goes to (cos, -sin),
            // so the bar's length lies along (fz, -fx), across the line. The
            // fill keeps to the end at -x, the left as the picture shows it.
            float fx = px - m_eye[0], fz = pz - m_eye[2];
            const float fl = std::sqrt(fx * fx + fz * fz);
            if (fl > 1e-4f) { fx /= fl; fz /= fl; } else { fx = 0.0f; fz = 1.0f; }
            const float yaw = std::atan2(fx, fz);
            const float frac = static_cast<float>(s.hp) / kFullHp;
            const float shift = -0.85f * (1.0f - frac);
            const float rx = fz, rz = -fx;
            {
                std::lock_guard<std::recursive_mutex> lk(back->TreeMutex());
                OrderVector& b = back->RowsUnder();
                b.PlaceAt(Fixed::From(px), Fixed::From(py + 1.35f), Fixed::From(pz));
                b.Orient(Fixed::Zero(), Fixed::One(), Fixed::Zero(), Fixed::From(yaw));
                OrderVector& f = fill->RowsUnder();
                f.PlaceAt(Fixed::From(px + rx * shift), Fixed::From(py + 1.35f), Fixed::From(pz + rz * shift));
                f.Orient(Fixed::Zero(), Fixed::One(), Fixed::Zero(), Fixed::From(yaw));
            }
        }
    }
    // The card: this runtime's health, weapon, eliminations and the clock --
    // or, in the lobby, what the next round is and who starts it.
    void cardLocked(double dt_ms)
    {
        if (!m_card) return;
        m_card_ms += dt_ms;
        if (m_card_ms < 150.0) return;
        m_card_ms = 0.0;
        std::string t;
        const int i = slotOf(m_me);
        if (m_phase == Lobby)
            t = "LOBBY   next: " + mapName(m_map) + ", " + clock(static_cast<int64_t>(m_round_secs) * kHz)
              + (m_me.empty() ? std::string() : m_judge ? std::string("   yours to start") : "   " + m_owner + " starts it");
        else if (i < 0) t = m_me.empty() ? "kart" : "watching -- the arena is full";
        else
        {
            const Slot& s = m_slots[i];
            if (s.dead_until >= 0)
                t = "out -- back in " + std::to_string((s.dead_until - static_cast<int64_t>(m_tick) + kHz - 1) / kHz);
            else
            {
                char hp[16]; std::snprintf(hp, sizeof hp, "%.1f", s.hp / 10.0);
                t = std::string("HP ") + hp + "   " + (s.weapon ? std::string(KartArms::Name(s.weapon)) + " x" + std::to_string(s.ammo) : std::string("no weapon"));
            }
            t += "   " + std::to_string(s.elims) + (s.elims == 1 ? " elim" : " elims");
            t += m_phase == Playing ? "   " + clock(m_end - static_cast<int64_t>(m_tick)) + " left" : std::string("   round over");
        }
        // The note, unless the lobby's own line already says it.
        if (!m_note.empty() && !(m_phase == Lobby && m_note.compare(0, 11, "next round:") == 0)) t += "   " + m_note;
        if (t == m_card_text) return;
        m_card_text = t;
        verb(m_card, "SetText", t);
    }

    // The lobby menu, under the host's keys: up/down choose, left/right
    // change, Enter changes or starts. True when the key was the menu's.
    bool menuKeyLocked(uint16_t key)
    {
        const bool up = key == 265 || key == 'W', down = key == 264 || key == 'S';
        const bool left = key == 263 || key == 'A', right = key == 262 || key == 'D';
        const bool enter = key == 257 || key == 335;
        if (up)   { m_menu = (m_menu + MenuItems - 1) % MenuItems; return true; }
        if (down) { m_menu = (m_menu + 1) % MenuItems; return true; }
        if (!left && !right && !enter) return false;
        const int dir = left ? -1 : 1;
        if (m_menu == MenuStart) { if (enter) roundLocked(); return true; }
        if (m_menu == MenuMap)
        {
            // Enter on a map made from a seed: the same map, a new seed.
            const int n = static_cast<int>(m_defs.size());
            const int next = enter && m_defs[m_map].generated ? m_map : ((m_map + dir) % n + n) % n;
            configLocked(next, m_round_secs, true);
            return true;
        }
        static const uint32_t lengths[] = { 60, 120, 180, 300 };
        int at = 0;
        for (int l = 0; l < 4; ++l) if (lengths[l] <= m_round_secs) at = l;
        at = (at + dir + 4) % 4;
        configLocked(m_map, lengths[at], false);
        return true;
    }
    /*
     * THE MENU AND THE BOARD on the camera's rows. In the lobby, the menu --
     * every runtime's, only the host's moved -- with the drivers and the last
     * round completed; in a round, the scores while Tab is held; after one,
     * the results until the lobby.
     */
    void boardLocked()
    {
        if (m_board.empty()) return;
        // The panel's lines, top down; a blank one is " " (a line of the
        // panel), and the rows past the last line are hidden.
        std::vector<std::string> rows;
        auto ranked = [&](bool last_round) {
            int place = 1;
            for (size_t i : rankLocked())
            {
                if (rows.size() + 2 >= m_board.size()) break;
                const Slot& s = m_slots[i];
                char line[128];
                if (last_round)
                    std::snprintf(line, sizeof line, "%d. %-18s %6d %7d%s", place++, s.driver.substr(0, 18).c_str(),
                                  s.last_elims, s.last_deaths, s.driver == m_me ? "  <" : "");
                else
                    std::snprintf(line, sizeof line, "%d. %-18s %6d %7d %5.1f%s", place++, s.driver.substr(0, 18).c_str(),
                                  s.elims, s.deaths, s.hp / 10.0, s.driver == m_me ? "  <" : "");
                rows.push_back(line);
            }
        };
        if (m_phase == Lobby && !m_me.empty())
        {
            auto item = [&](int at, const std::string& text) { return (m_judge && m_menu == at ? "> " : "  ") + text; };
            rows.push_back("LOBBY  --  " + (m_session ? m_owner + "'s battle" : std::string("practice")));
            rows.push_back(item(MenuMap, "map      <  " + mapName(m_map) + (m_layout ? "  #" + std::to_string(m_layout % 10000) : std::string()) + "  >"));
            rows.push_back(item(MenuLength, "length   <  " + clock(static_cast<int64_t>(m_round_secs) * kHz) + "  >"));
            rows.push_back(item(MenuStart, "[ start the round ]"));
            rows.push_back(" ");
            char head[96];
            std::snprintf(head, sizeof head, "   %-18s %6s %7s   %s", "driver", "elims", "deaths", "(last round)");
            rows.push_back(head);
            ranked(true);
            rows.push_back(" ");
            rows.push_back(m_judge ? "[up/down] choose  [left/right] change  [Enter] start" : m_owner + " sets the next round");
        }
        else if (m_phase != Lobby && (m_board_held || m_phase == Results))
        {
            rows.push_back(m_phase == Playing ? "ROUND  " + clock(m_end - static_cast<int64_t>(m_tick)) + " left  --  " + mapName(m_map)
                                              : "ROUND OVER  --  " + mapName(m_map));
            char head[96];
            std::snprintf(head, sizeof head, "   %-18s %6s %7s %5s", "driver", "elims", "deaths", "hp");
            rows.push_back(head);
            ranked(false);
            if (m_judge && m_phase == Playing) { rows.push_back(" "); rows.push_back("[Esc] abandon the round"); }
        }
        // One width for every line (the font is a fixed cell), so the rows'
        // backgrounds make one panel.
        size_t wide = 0;
        for (const std::string& t : rows) wide = std::max(wide, t.size());
        for (std::string& t : rows) t.resize(wide, ' ');
        rows.resize(m_board.size());
        for (size_t r = 0; r < m_board.size(); ++r)
        {
            BoardRow& b = m_board[r];
            const bool show = !rows[r].empty();
            if (show && rows[r] != b.text) { verb(b.rid, "SetText", rows[r]); b.text = rows[r]; }
            if (show != b.shown) { verb(b.rid, "SetHidden", show ? "0" : "1"); b.shown = show; }
        }
    }

    // ── small things ────────────────────────────────────────────────────

    int elimsOf(const Slot& s) const  { return m_phase == Lobby ? s.last_elims : s.elims; }
    int deathsOf(const Slot& s) const { return m_phase == Lobby ? s.last_deaths : s.deaths; }
    std::vector<size_t> rankLocked() const
    {
        std::vector<size_t> out;
        for (size_t i = 0; i < m_slots.size(); ++i) if (!m_slots[i].driver.empty()) out.push_back(i);
        std::stable_sort(out.begin(), out.end(), [this](size_t a, size_t b) {
            const Slot& x = m_slots[a]; const Slot& y = m_slots[b];
            if (elimsOf(x) != elimsOf(y)) return elimsOf(x) > elimsOf(y);
            return deathsOf(x) < deathsOf(y);
        });
        return out;
    }
    int slotOf(const std::string& who) const
    {
        if (who.empty()) return -1;
        for (size_t i = 0; i < m_slots.size(); ++i) if (m_slots[i].driver == who) return static_cast<int>(i);
        return -1;
    }
    int mapIndex(const std::string& name) const
    {
        for (size_t m = 0; m < m_defs.size(); ++m) if (m_defs[m].name == name) return static_cast<int>(m);
        return -1;
    }
    std::string mapName(int m) const { return m >= 0 && m < static_cast<int>(m_defs.size()) ? m_defs[m].name : std::string("-"); }
    const char* phaseName() const { return m_phase == Playing ? "playing" : m_phase == Results ? "results" : "lobby"; }
    int64_t secsLeft() const { return m_phase == Playing ? (m_end - static_cast<int64_t>(m_tick) + kHz - 1) / kHz : 0; }
    size_t driversLocked() const
    {
        size_t n = 0;
        for (const Slot& s : m_slots) if (!s.driver.empty()) ++n;
        return n;
    }
    float speedOf(size_t i) { return speedFixed(i).ToFloat(); }
    void noteLocked(const std::string& n) { m_note = n; ETCS_LOG("KartBattle", n); }
    static std::string driveLine(int t, int s, int f) { return "drive " + std::to_string(t) + " " + std::to_string(s) + " " + std::to_string(f); }
    static std::string clock(int64_t ticks)
    {
        if (ticks < 0) ticks = 0;
        const int64_t secs = (ticks + kHz - 1) / kHz;
        char b[48];
        std::snprintf(b, sizeof b, "%lld:%02lld", static_cast<long long>(secs / 60), static_cast<long long>(secs % 60));
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

    // The arena: its walls, its maps, the map in play, and the pools.
    std::vector<ETCS::RID> m_walls;
    std::vector<KartArena::Map> m_defs;
    KartArena::Map m_live;
    std::vector<ETCS::RID> m_live_blocks;
    std::vector<ETCS::RID> m_block_nodes, m_slab_nodes, m_dome_nodes;
    std::vector<ETCS::RID> m_pickup_nodes, m_shot_nodes, m_blast_nodes;
    std::vector<bool> m_shot_busy;
    std::vector<int64_t> m_blast_until;
    std::vector<BoardRow> m_board;

    // The session, and its number (the streams'; Leave moves it on).
    uint64_t    m_gen = 0;
    std::string m_me, m_owner, m_id;
    bool        m_judge = false, m_session = false;
    ETCS::RID   m_record = 0, m_proposals = 0, m_presence = 0, m_hall = 0;
    std::deque<std::string> m_outbox;
    std::map<std::string, std::pair<uint64_t, std::string>> m_here;
    std::string m_sent, m_advertised, m_note;

    // The battle: its clock, its round, its arena, its dice.
    uint64_t m_tick = 0, m_recorded = 0, m_seq = 0;
    double   m_wall_ms = 0.0;
    Phase    m_phase = Lobby;
    int64_t  m_end = 0;
    uint32_t m_round_secs = 180;
    int      m_map = 0;
    uint64_t m_layout = 0;
    uint64_t m_rng = 0x9E3779B97F4A7C15ull;
    std::vector<Shot> m_shots;
    std::vector<Pickup> m_pickups;
    uint64_t m_snap_tick = 0, m_differed_at = 0;
    std::string m_snap_hash = "-";
    std::vector<std::pair<uint64_t, std::string>> m_snaps;

    // This runtime's own: its keys, its eye, its card, its menu, its board.
    unsigned m_keys = 0;
    int      m_sent_t = 0, m_sent_s = 0, m_sent_f = 0;
    int      m_menu = MenuStart;
    bool     m_board_held = false;
    float    m_eye[3] = { 0.0f, 0.0f, 0.0f }, m_at[3] = { 0.0f, 0.0f, 0.0f };
    bool     m_eye_set = false;
    double   m_card_ms = 0.0;
    std::string m_card_text;
};

#endif // KARTPROVIDER_KARTBATTLE_H__
