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
 * the WHEELS -- slowly at first, gathering pace while held, toward a lock
 * that is smaller the faster the kart goes -- and back when let go; the
 * heading turns by speed x wheel angle / wheelbase, so a kart standing still
 * does not turn, and one at speed carves an arc rather than spinning. THE
 * TYRES GRIP only so hard: across the heading they pull the kart's way round
 * with it, up to grip() (half that with the brake on), and a turn that asks
 * more -- too fast, too much lock -- leaves the rest a slide: the kart
 * drifts, keeping most of its speed, and steering against it brings it back.
 * What the brake and the slide take from the motion is heat, and the tyres --
 * the body's members, anchored in its frame -- take it, each its share, and
 * shed it into the body; what a tyre sheds per unit of road is its MARK, a
 * rope of fixed length drawn behind it, dark where it braked or slid, wiped
 * from the far end as it goes on. The ground is the map's (KartArena.h): a
 * kart rides on it, slows going up and gains going down, leaves it where it
 * falls away faster than gravity follows (a ramp's lip) and lands where it
 * meets it again; ground rising more than a kart can climb in a step is a
 * wall. The ground has levels -- a pit's floor below the arena's, a deck in
 * the air that a kart is on from above, under from below and through going up
 * -- so which ground is a kart's follows from how high it is; a spring throws
 * it up, a pad pushes it along. A kart is SOLID -- a sphere to everything
 * (etcs_causal_constraints.md §13) -- and drawn as a body on its four tyres,
 * tilted to the ground, which this battle turns with it after every step
 * (frames are translations). What is drawn for one runtime alone -- its
 * camera, its card, the menu and the board, the HP bars turned to face its
 * eye -- is set on the frame edge and is not a line.
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

    // A kart of the pool, its four wheels (front right, front left, rear
    // right, rear left: +x first, the picture's right) and its HP bar (the back and the fill): coloured by its
    // slot, waiting in the pit until someone joins.
    void AddKart(ETCS::RID body, ETCS::RID fr, ETCS::RID fl, ETCS::RID rr, ETCS::RID rl,
                 ETCS::RID bar_back, ETCS::RID bar_fill)
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (!body || m_slots.size() >= kMaxKarts) return;
        Slot s;
        s.body = body;
        s.wheels[0] = fr; s.wheels[1] = fl; s.wheels[2] = rr; s.wheels[3] = rl;
        s.bar_back = bar_back; s.bar_fill = bar_fill;
        const float* c = kColors[m_slots.size() % kMaxKarts];
        verb(body, "SetColor", f4(c[0], c[1], c[2], 1.0f));
        if (Causal_* k = causal(body))
        {
            std::lock_guard<std::recursive_mutex> lk(k->TreeMutex());
            s.pristine = k->RowsUnder();   // what "as made" means, for every restart
        }
        for (int t = 0; t < 4; ++t)
            if (Causal_* c = causal(s.wheels[t]))
            {
                std::lock_guard<std::recursive_mutex> lk(c->TreeMutex());
                s.tyre_pristine[t] = c->RowsUnder();
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
        if (m_defs.empty() || width <= 0 || up < 0 || flat < 0 || down < 0 || up + flat + down <= 0 || height <= 0) return;
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
    // A pad on the level ground of the map being described, pushing toward
    // `yaw` (a ramp's convention).
    void AddPad(float x, float z, float yaw)
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (m_defs.empty()) return;
        KartArena::Pad p;
        p.x = Fixed::From(x); p.z = Fixed::From(z); p.yaw = static_cast<int>(std::lround(yaw));
        m_defs.back().pads.push_back(p);
    }
    // A spring on the ground of the map being described; a deck, its top
    // `height` up, `width` along x and `depth` along z; a pit, its floor
    // `deep` down (what is put in it stands on that floor).
    void AddSpring(float x, float z)
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (!m_defs.empty()) m_defs.back().springs.push_back({ Fixed::From(x), Fixed::From(z), Fixed::Zero() });
    }
    void AddDeck(float x, float z, float width, float depth, float height)
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (m_defs.empty() || width <= 0 || depth <= 0 || height <= 0) return;
        m_defs.back().decks.push_back({ Fixed::From(x), Fixed::From(z), Fixed::From(width), Fixed::From(depth), Fixed::From(height) });
    }
    void AddPit(float x, float z, float width, float depth, float deep)
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (m_defs.empty() || width <= 0 || depth <= 0 || deep <= 0) return;
        m_defs.back().pits.push_back({ Fixed::From(x), Fixed::From(z), Fixed::From(width), Fixed::From(depth), Fixed::From(deep) });
    }
    void AddSpawn(float x, float z)  { std::lock_guard<std::mutex> g(m_mu); if (!m_defs.empty()) m_defs.back().spawns.push_back({ Fixed::From(x), Fixed::From(z) }); }
    void AddPickup(float x, float z) { std::lock_guard<std::mutex> g(m_mu); if (!m_defs.empty()) m_defs.back().pickups.push_back({ Fixed::From(x), Fixed::From(z) }); }
    // A box on top of the deck over the spot: AddPickup's is on the ground under it.
    void AddPickupUp(float x, float z) { std::lock_guard<std::mutex> g(m_mu); if (!m_defs.empty()) m_defs.back().pickups.push_back({ Fixed::From(x), Fixed::From(z), true }); }

    // The pools the map in play is drawn with: solid blocks, the ground's
    // slabs (a ramp's slope or top) and domes (a hill), power-up boxes, shots,
    // bursts, and the rows of the menu and the board on the camera.
    void AddBlockNode(ETCS::RID r)  { std::lock_guard<std::mutex> g(m_mu); m_block_nodes.push_back(r); parkNode(r); }
    void AddSlabNode(ETCS::RID r)   { std::lock_guard<std::mutex> g(m_mu); m_slab_nodes.push_back(r); groundColour(r, KartArena::OnRamp); parkNode(r); }
    // A wedge of the pool: a ramp's slope, solid down to the ground (its mesh
    // rises toward its own +z: kart_wedge_shape.etcs).
    void AddWedgeNode(ETCS::RID r)  { std::lock_guard<std::mutex> g(m_mu); m_wedge_nodes.push_back(r); groundColour(r, KartArena::OnRamp); parkNode(r); }
    void AddDomeNode(ETCS::RID r)   { std::lock_guard<std::mutex> g(m_mu); m_dome_nodes.push_back(r); groundColour(r, KartArena::OnHill); parkNode(r); }
    // The arena's whole floor: coloured here, as the slabs and domes are, so a
    // mark fades into the ground it is on; put away when the map has a pit,
    // for a floor of slabs laid round it.
    void AddGroundNode(ETCS::RID r) { std::lock_guard<std::mutex> g(m_mu); m_floor = r; groundColour(r, KartArena::Floor); }
    // A spring of the pool: its round plate and the four arrows that close on
    // its middle, sized and coloured here.
    void AddSpringNode(ETCS::RID plate, ETCS::RID a, ETCS::RID b, ETCS::RID c, ETCS::RID d)
    {
        std::lock_guard<std::mutex> g(m_mu);
        m_spring_nodes.push_back({ plate, { a, b, c, d } });
        const float wide = KartArena::SpringRadius().ToFloat() * 2.0f;
        verb(plate, "Create", fmt3(wide) + ", 0.012, " + fmt3(wide));
        verb(plate, "SetColor", f4(kSpring[0], kSpring[1], kSpring[2], 1.0f));
        parkNode(plate);
        for (ETCS::RID r : { a, b, c, d })
        {
            verb(r, "Create", "0.9, 0.7, 0.02");
            verb(r, "SetColor", f4(kSpringArrow[0], kSpringArrow[1], kSpringArrow[2], 1.0f));
            parkNode(r);
        }
    }
    // A pad of the pool: its plate, coloured and sized here (the size is the
    // arena's), and the three arrows that run along it.
    void AddPadNode(ETCS::RID plate, ETCS::RID a, ETCS::RID b, ETCS::RID c)
    {
        std::lock_guard<std::mutex> g(m_mu);
        m_pad_nodes.push_back({ plate, { a, b, c } });
        groundColour(plate, KartArena::OnPad);
        verb(plate, "Create", fmt3(KartArena::PadWidth().ToFloat()) + ", 0.01, " + fmt3(KartArena::PadLength().ToFloat()));
        parkNode(plate);
        // An arrow lies flat: its mesh's length (+y) along the pad, its depth up.
        for (ETCS::RID r : { a, b, c }) { verb(r, "Create", "1.7, " + fmt3(padArrow().ToFloat()) + ", 0.02"); parkNode(r); }
    }
    // A segment of a tyre's mark: a thin strip on the ground, sized when it
    // is laid; twenty-four to a tyre, ninety-six to a kart, in the order the
    // karts are made.
    void AddMarkNode(ETCS::RID r)
    {
        std::lock_guard<std::mutex> g(m_mu);
        m_mark_nodes.push_back(r);
        verb(r, "Create", "0.18, 0.01, 1.0");
        verb(r, "SetAnchored", "1");
        parkNode(r);
    }
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
     * any is a `drive` line -- and, for the host, Esc abandons the round. A
     * turns the kart to the picture's left and D to its right, which is the
     * wheel's -1 and +1 (the camera's right is up x forward: +x for a kart
     * heading +z); SetInvert swaps the two keys for this runtime alone. Tab
     * holds up the board and Q and E look round the kart (lookLocked), this
     * runtime's alone and no line of the record. In the lobby the host's keys
     * move the menu instead: up/down choose, left/right change, Enter
     * changes or starts; each change is a line.
     */
    void Key(uint16_t key, bool down)
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (key == 258) { m_board_held = down; return; }
        if (key == 'Q' || key == 'E')
        {
            const unsigned bit = key == 'Q' ? 1u : 2u;
            if (down) m_look_keys |= bit; else m_look_keys &= ~bit;
            return;
        }
        if (down && m_judge && !m_defs.empty())
        {
            if (m_phase == Lobby && menuKeyLocked(key)) return;
            if (m_phase == Playing && key == 256) { takeLocked(m_me, "abandon"); return; }
        }
        int bit = key == 0 ? 5 : -1;   // no key: the held ones read again (SetInvert)
        if (key == 'W' || key == 265) bit = 0;
        else if (key == 'S' || key == 264) bit = 1;
        else if (key == 'A' || key == 263) bit = 2;
        else if (key == 'D' || key == 262) bit = 3;
        else if (key == ' ') bit = 4;
        if (bit < 0) return;
        if (bit < 5) { if (down) m_keys |= (1u << bit); else m_keys &= ~(1u << bit); }
        const int t = ((m_keys & 1) ? 1 : 0) - ((m_keys & 2) ? 1 : 0);
        const int right = ((m_keys & 8) ? 1 : 0) - ((m_keys & 4) ? 1 : 0);
        const int s = m_invert ? -right : right;
        const int f = (m_keys & 16) ? 1 : 0;
        if (t == m_sent_t && s == m_sent_s && f == m_sent_f) return;
        m_sent_t = t; m_sent_s = s; m_sent_f = f;
        if (m_me.empty()) return;
        const std::string line = driveLine(t, s, f);
        if (m_judge) takeLocked(m_me, line);
        else m_outbox.push_back(line);
    }

    // The wheel's keys swapped, for this runtime alone: the keys held are
    // read again, so a change mid-turn is a `drive` line like any other.
    void SetInvert(bool on)
    {
        { std::lock_guard<std::mutex> g(m_mu); if (m_invert == on) return; m_invert = on; }
        Key(0, false);
    }
    bool Inverted() { std::lock_guard<std::mutex> g(m_mu); return m_invert; }

    /*
     * LOOKING ROUND THE KART. The eye rides behind the kart; a drag with the
     * right button, or Q and E held, swings it round the kart (and a drag up
     * or down, over it) -- to see beside or behind, or out of a hole the
     * view behind is no good in. Let alone it comes back behind the kart,
     * slowly (lookLocked). This runtime's alone: where a driver looks is no
     * part of the battle, so no line and nothing hashed.
     */
    void Pointer(const InputEvent& ev)
    {
        std::lock_guard<std::mutex> g(m_mu);
        switch (ev.action)
        {
            case INPUT_BUTTON_DOWN:
                if (ev.key == kLookButton) { m_look_drag = true; m_look_x = ev.x; m_look_y = ev.y; }
                break;
            case INPUT_MOTION:
                if (!m_look_drag) break;
                // Let go outside the window: the platform says it is up.
                if (input_button_state(ev, kLookButton) == 0) { m_look_drag = false; break; }
                turnLookLocked((ev.x - m_look_x) * kLookTurn, (ev.y - m_look_y) * kLookTurn);
                m_look_x = ev.x; m_look_y = ev.y;
                break;
            case INPUT_BUTTON_UP:
                if (ev.key == kLookButton) m_look_drag = false;
                break;
            default: break;
        }
    }
    // The look as a verb: this far round the kart and over it, in degrees
    // (to the picture's right, and up), as a drag would leave it.
    void Look(float yaw_deg, float pitch_deg)
    {
        std::lock_guard<std::mutex> g(m_mu);
        m_look_yaw = m_look_pitch = 0.0f;
        turnLookLocked(yaw_deg * kPi / 180.0f, pitch_deg * kPi / 180.0f);
    }
    // "<eye x> <y> <z> <at x> <y> <z> <look yaw> <pitch>": where this
    // runtime's eye is headed (the frame edge eases it there) and the look, in
    // degrees.
    std::string Eye()
    {
        std::lock_guard<std::mutex> g(m_mu);
        float eye[3], at[3];
        viewLocked(eye, at);
        return fmt3(eye[0]) + " " + fmt3(eye[1]) + " " + fmt3(eye[2]) + " " + fmt3(at[0]) + " " + fmt3(at[1]) + " " + fmt3(at[2]) + " "
             + fmt3(m_look_yaw * 180.0f / kPi) + " " + fmt3(m_look_pitch * 180.0f / kPi);
    }
    // The frame edge by hand, for a script with no window: what a frame
    // `ms` long does (the host's clock runs with it, as a window's does).
    void Frame(float ms) { AdvanceConcrete(static_cast<double>(ms)); }

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
    // The map in play: "<name> <layout> <blocks> <ramps> <hills> <spawns> <pickups> <pads>
    // <springs> <decks> <pits> <digest>", the digest over every number on it.
    std::string Layout()
    {
        std::lock_guard<std::mutex> g(m_mu);
        std::string st;
        auto put = [&st](const Fixed& f) { st += std::to_string(f.raw) + " "; };
        for (const auto& b : m_live.blocks) { put(b.x); put(b.z); put(b.w); put(b.d); put(b.h); st += std::to_string(b.yaw) + "|"; }
        for (const auto& r : m_live.ramps)  { put(r.x); put(r.z); put(r.width); put(r.up); put(r.flat); put(r.down); put(r.height); st += std::to_string(r.yaw) + "|"; }
        for (const auto& k : m_live.hills)  { put(k.x); put(k.z); put(k.radius); put(k.height); st += "|"; }
        for (const auto& p : m_live.pads)   { put(p.x); put(p.z); st += std::to_string(p.yaw) + "|"; }
        for (const auto& p : m_live.springs) { put(p.x); put(p.z); st += "|"; }
        for (const auto& d : m_live.decks)  { put(d.x); put(d.z); put(d.w); put(d.d); put(d.h); st += "|"; }
        for (const auto& p : m_live.pits)   { put(p.x); put(p.z); put(p.w); put(p.d); put(p.depth); st += "|"; }
        for (const auto& p : m_live.spawns) { put(p.x); put(p.z); }
        for (const auto& p : m_live.pickups) { put(p.x); put(p.z); if (p.up) st += "u"; }
        char d[17];
        std::snprintf(d, sizeof d, "%016llx", static_cast<unsigned long long>(XXH3_64bits(st.data(), st.size())));
        return m_live.name + " " + std::to_string(m_layout) + " " + std::to_string(m_live.blocks.size()) + " "
             + std::to_string(m_live.ramps.size()) + " " + std::to_string(m_live.hills.size()) + " "
             + std::to_string(m_live.spawns.size()) + " " + std::to_string(m_live.pickups.size()) + " "
             + std::to_string(m_live.pads.size()) + " " + std::to_string(m_live.springs.size()) + " "
             + std::to_string(m_live.decks.size()) + " " + std::to_string(m_live.pits.size()) + " " + d;
    }
    // What is wrong with the maps, "<map>: <fault>" a line (KartArena::Faults,
    // and what a map has more of than the pools draw), or "none": the
    // hand-made ones as given, one from a seed as it is in
    // play. A verb's answer is 255 characters: past the lines that fit,
    // "+<n> more".
    std::string Faults()
    {
        std::lock_guard<std::mutex> g(m_mu);
        std::string out;
        int more = 0;
        for (size_t i = 0; i < m_defs.size(); ++i)
        {
            KartArena::Map m = m_defs[i];
            if (!m.generated) KartArena::Prepare(m);
            else if (static_cast<int>(i) == m_map) m = m_live;
            else continue;
            std::istringstream lines(KartArena::Faults(m) + "\n" + shortLocked(m));
            for (std::string line; std::getline(lines, line);)
            {
                if (line.empty()) continue;
                line = m.name + ": " + line + "\n";
                if (more == 0 && out.size() + line.size() <= 240) out += line; else ++more;
            }
        }
        if (out.empty()) return "none";
        if (more) return out + "+" + std::to_string(more) + " more";
        out.pop_back();
        return out;
    }
    std::string Hash()     { std::lock_guard<std::mutex> g(m_mu); return std::to_string(m_tick) + " " + hashLocked(); }
    std::string Snapshot() { std::lock_guard<std::mutex> g(m_mu); return std::to_string(m_snap_tick) + " " + m_snap_hash; }
    std::string Note()     { std::lock_guard<std::mutex> g(m_mu); return m_note; }
    // "<slot> <x> <z> <heading x> <heading z> <speed> <hp> <weapon> <ammo> <y> <air|ground> <wheel> <slide>",
    // or "-"; the wheel and the slide (the way across the heading) are + to
    // the picture's right.
    std::string Kart(const std::string& who)
    {
        std::lock_guard<std::mutex> g(m_mu);
        const int i = slotOf(who);
        Causal_* k = i < 0 ? nullptr : causal(m_slots[i].body);
        if (!k) return "-";
        const Slot& s = m_slots[i];
        float x, y, z, slide;
        {
            std::lock_guard<std::recursive_mutex> lk(k->TreeMutex());
            const OrderVector& r = k->Order4();
            x = r.x.ToFloat(); y = r.y.ToFloat(); z = r.z.ToFloat();
            Fixed vx, vy, vz;
            r.Velocity(k->MassUnder(), vx, vy, vz);
            slide = (vx * s.hz - vz * s.hx).ToFloat();   // along the heading's across, (hz, -hx)
        }
        return std::to_string(i) + " " + fmt3(x) + " " + fmt3(z) + " " + fmt3(s.hx.ToFloat()) + " " + fmt3(s.hz.ToFloat()) + " "
             + fmt1(speedOf(static_cast<size_t>(i))) + " " + std::to_string(s.hp) + " " + KartArms::Name(s.weapon)
             + " " + std::to_string(s.ammo) + " " + fmt3(y) + " " + (s.air ? "air" : "ground") + " " + fmt3(s.wheel.ToFloat())
             + " " + fmt3(slide);
    }
    // Tyres <name> -- per tyre, fr fl rr rl, "<heat> <rope> <shown> <darkest>": its heat (J), how
    // long its mark's rope is laid behind it, how many segments are drawn,
    // and the darkest (0..1). "-" for nobody.
    std::string Tyres(const std::string& who)
    {
        std::lock_guard<std::mutex> g(m_mu);
        const int i = slotOf(who);
        if (i < 0) return "-";
        const Slot& s = m_slots[i];
        std::string out;
        for (int t = 0; t < 4; ++t)
        {
            const Trail& tr = s.trail[t];
            Fixed rope = tr.run_d;
            int shown = 0, darkest = 0;
            for (const MarkSeg& g : tr.segs)
            {
                rope += g.len;
                if (g.node >= 0) { ++shown; darkest = std::max(darkest, g.level); }
            }
            Causal_* c = causal(s.wheels[t]);
            const float heat = c ? c->Order4().Heat().ToFloat() : 0.0f;
            out += (t ? " " : "") + fmt3(heat) + " " + fmt3(rope.ToFloat()) + " " + std::to_string(shown) + " "
                 + fmt3(static_cast<float>(darkest) / 8.0f);
        }
        return out;
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
    static constexpr int kMarkNodes = 24;   // a tyre's drawn segments at most
    /*
     * A TYRE'S MARK: the road it ran, as segments laid where it ran them and
     * left there, newest first, each as dark as what the tyre shed over it
     * (a level, 0..8; 0 is laid but not drawn). A segment is laid when the
     * way turns, the darkness changes, the tyre leaves or meets the ground,
     * or it is long enough -- so a reversal is a segment's end, and nothing
     * already laid moves. A rope of fixed length: past markLength() of road
     * the oldest goes, so going on, either way, wipes it from the far end.
     * Derived from the rows (the tyres' heat), so the same everywhere, but
     * drawn only: not in the hash.
     */
    struct MarkSeg { Fixed x0, y0, z0, x1, y1, z1, len; int level = 0; int node = -1; };
    struct Trail
    {
        std::deque<MarkSeg> segs;
        Fixed    sx, sy, sz;                       // where the open run starts: the last segment's end
        Fixed    hx, hy, hz;                       // where the tyre touched last tick
        Fixed    run_e, run_d;                     // shed and road in the open run
        Fixed    dx, dz;                           // the open run's way, as its first step went
        int      run_level = 0;
        bool     touched = false, air = false;
        uint32_t seq = 0;                          // segments laid: which lift a new one takes
        uint32_t used = 0;                         // the tyre's drawn segments' nodes in use, a bit each
    };
    struct Slot
    {
        ETCS::RID   body = 0;
        ETCS::RID   wheels[4] = { 0, 0, 0, 0 };   // fr fl rr rl: drawn, placed by the battle
        ETCS::RID   bar_back = 0, bar_fill = 0;   // the HP bar: this runtime's to turn
        OrderVector pristine;                      // the rows as made: every restart starts here
        std::string driver;
        int         thr = 0, steer = 0, fire = 0;
        int         press = 0;                     // a press of the trigger the tick has not yet seen
        Fixed       wheel;                         // radians, + turns toward (hz, -hx): the picture's right
        Fixed       wheel_ease;                    // how far past the lock a held wheel has eased: 0 at the press, 1 at the far lock
        int         steer_held = 0;                // the way the wheel was being turned last tick
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
        // The tyres: Causal members of the body (kart_car.etcs), anchored in
        // its frame; their rows as made, every restart starts there.
        OrderVector tyre_pristine[4];
        Fixed       tyre_heat[4];                  // each tyre's heat as the drive left it: the tick emits from it
        Trail       trail[4];                      // each tyre's mark: drawn, not state
    };
    struct Pickup { Fixed x, y, z; int64_t back = 0; ETCS::RID node = 0; };
    struct Shot
    {
        uint8_t kind = 0; int owner = -1;
        Fixed x, y, z, vx, vy, vz;
        int life = 0, armed = 0, node = -1;
    };
    struct BoardRow { ETCS::RID rid; std::string text; bool shown; };
    struct PadNodes { ETCS::RID plate = 0; ETCS::RID arrow[3] = { 0, 0, 0 }; };
    struct SpringNodes { ETCS::RID plate = 0; ETCS::RID arrow[4] = { 0, 0, 0, 0 }; };
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
    // The eye's boom behind and over the kart, and the look that swings it.
    static constexpr float    kPi         = 3.14159265f;
    static constexpr float    kBoom       = 8.32f;     // its length: 7.5 behind and 3.6 over
    static constexpr float    kBoomRise   = 0.4475f;   // and that angle over the ground
    static constexpr int      kBoomSteps  = 16;        // the points along it tried against the ground
    static constexpr float    kBoomGap    = 0.25f;     // the least it keeps over the ground, or off a deck
    static constexpr float    kBoomLeast  = 0.2f;      // of its length, however little is clear
    static constexpr uint16_t kLookButton = 1;         // the right button
    static constexpr float    kLookTurn   = 0.006f;    // radians a pixel dragged (golf's orbit)
    static constexpr double   kLookKeyRate = 2.0;      // radians a second, Q or E held
    static constexpr float    kLookDown   = -0.35f, kLookUp = 0.95f;   // the boom from nearly level to nearly overhead
    static constexpr double   kLookHoldMs = 700.0, kLookBackMs = 1500.0;
    // The ground's colours, the battle's to give (AddGroundNode, AddSlabNode,
    // AddDomeNode): a mark is drawn between the ground's own and the tyre's.
    static constexpr float kGround[6][3] = { { 0.30f, 0.31f, 0.33f }, { 0.72f, 0.52f, 0.30f }, { 0.33f, 0.52f, 0.27f },
                                             { 0.13f, 0.20f, 0.36f }, { 0.50f, 0.55f, 0.66f }, { 0.22f, 0.21f, 0.24f } };
                                             // floor, ramp, hill, pad, deck, a pit's floor
    static constexpr float kPitWall[3] = { 0.15f, 0.15f, 0.17f };
    static constexpr float kSpring[3] = { 0.42f, 0.12f, 0.34f }, kSpringArrow[3] = { 1.0f, 0.82f, 0.25f };
    static constexpr float kTyre[3] = { 0.06f, 0.06f, 0.07f };
    static constexpr float kColors[kMaxKarts][3] = {
        { 0.86f, 0.18f, 0.16f }, { 0.18f, 0.42f, 0.88f }, { 0.95f, 0.80f, 0.15f }, { 0.20f, 0.70f, 0.30f },
        { 0.95f, 0.50f, 0.10f }, { 0.60f, 0.30f, 0.80f }, { 0.20f, 0.80f, 0.85f }, { 0.92f, 0.92f, 0.90f } };

    static Fixed F(int64_t n, int64_t d = 1) { return Fixed::FromInt(n) / Fixed::FromInt(d); }
    static Fixed dt()        { return F(1, kHz); }
    /*
     * THE PEDAL: quick off the line, then less the nearer the top -- the
     * launch's push falling with speed, never below pull() -- so the top is
     * reached over about 80 units, more than a wall to the other. Speed is
     * worth keeping: a corner taken in a drift keeps more of it than one
     * braked for. A boost pushes hard (boostPush()) to its own top.
     */
    static Fixed launch()    { return F(10); }
    static Fixed pull()      { return F(2); }
    static Fixed topSpeed()  { return F(22); }
    static Fixed boostSpeed(){ return F(30); }
    static Fixed boostPush() { return F(18); }
    static Fixed pushAt(Fixed v, Fixed top)
    {
        const Fixed p = launch() * (Fixed::One() - v / top);
        return p > pull() ? p : pull();
    }
    static Fixed brake()     { return F(20); }
    static Fixed reverseAcc(){ return F(5); }
    static Fixed reverseTop(){ return F(6); }
    static Fixed coast()     { return F(6, 10); }
    static Fixed wheelLock() { return F(45, 100); }   // the lock standing still: where a press puts the wheel at once
    static Fixed steerEase() { return F(3, 2); }      // s: a hold eases the wheel on from the lock to twice it in this long
    static Fixed steerBack() { return F(3); }         // rad/s: back toward the middle, let go
    // The lock at a speed: the full lock standing still, less the faster the
    // kart goes, so the turn a press gives at speed (speed x lock /
    // wheelbase) stays near 1.7 to 1.9 rad/s from 16 u/s up instead of
    // growing with it. A long hold doubles it.
    static Fixed lockAt(Fixed speed) { return wheelLock() / (Fixed::One() + speed.Abs() / F(10)); }
    static Fixed wheelbase() { return F(16, 10); }
    // The tyres across: the most they pull a slide out (u/s/s), and less
    // with the brake on -- past it the kart slides. A full lock at 16 u/s
    // asks ~28, at the top ~42, so it drifts; at 12 u/s it still bites.
    static Fixed grip()      { return F(20); }
    static Fixed brakeGrip() { return F(10); }
    // What a slide costs, of the speed the tyres' pull would take with it:
    // a quarter while the way is up to ~35 degrees off the heading (`off`
    // is that angle's sine), rising to all of it full sideways -- so a drift
    // keeps its speed and a kart knocked sideways still stops.
    static Fixed slideCost(Fixed off)
    {
        const Fixed free = F(574, 1000), least = F(1, 4);
        if (off <= free) return least;
        return Fixed::Min(Fixed::One(), least + (Fixed::One() - least) * (off - free) / (Fixed::One() - free));
    }
    // A PAD pushes the way it points while a kart's middle is over it:
    // padPush() (u/s/s) whatever the kart is doing, and padGain() of its
    // speed that way a second on top, so one taken at speed gives more --
    // until it is going padTop() that way, the fastest anything goes. Its
    // arrows run its length in padBeat() ticks, one behind the other.
    static Fixed padPush()   { return F(120); }
    static Fixed padGain()   { return F(1); }
    static Fixed padTop()    { return F(40); }
    // A SPRING throws a kart up as hard as a pad pushes one along in an
    // eighth of a second -- 15 u/s, five and a half units high -- and again
    // when it comes down on it. Its four arrows close on its middle.
    static Fixed springKick(){ return padPush() / F(8); }
    static Fixed padArrow()  { return F(9, 10); }    // an arrow's length
    static constexpr int padBeat() { return 40; }
    // The tyres at the body's corners, in its frame: across, ahead.
    static Fixed tyreSide()  { return F(70, 100); }
    static Fixed tyreAhead() { return F(64, 100); }
    // A MARK, from the heat a tyre sheds per unit of road (a force: its
    // share of the friction against its speed) over its quarter of the
    // kart's mass -- a deceleration. Nothing below markFrom(); full black
    // at markFull(): the brake on (20) is full, a drift's slide (what it
    // costs: a quarter of the grip times how far the way is off the heading)
    // part way, deepening as it opens.
    static Fixed markFrom()  { return F(1, 5); }
    static Fixed markFull()  { return F(2); }
    static Fixed markLength(){ return F(40); }      // the rope: this much road, and the segment over
    static Fixed markSegMax(){ return F(5, 2); }    // a dark segment at most; an unmarked run...
    static Fixed markRunMax(){ return F(4); }       // ...at most this
    static Fixed markTurn()  { return F(99, 100); } // a way turned past ~8 degrees ends a segment...
    static Fixed markMin()   { return F(1, 4); }    // ...this long at least
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
        const Fixed y = KartArena::Solid(m_live, x, z, &gx, &gz) + kartY();   // on the ground, under any deck
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
        s.thr = s.steer = s.fire = s.press = 0; s.wheel = s.wheel_ease = Fixed::Zero(); s.steer_held = 0;
        s.hp = kFullHp; s.weapon = KartArms::None; s.ammo = 0; s.cooldown = s.boost = 0;
        s.dead_until = -1; s.ram_safe = 0;
        verb(s.body, "SetVisible", "0");
        verb(s.body, "SetAnchored", "1");
        for (ETCS::RID w : s.wheels) verb(w, "SetVisible", "0");
        tyresAfreshLocked(s);
        clearMarksLocked(i);
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
        s.thr = s.steer = 0; s.press = 0; s.wheel = s.wheel_ease = Fixed::Zero(); s.steer_held = 0;
        s.hp = kFullHp; s.weapon = KartArms::None; s.ammo = 0; s.cooldown = s.boost = 0;
        s.dead_until = -1;
        Fixed dx = -bx, dz = -bz;
        if (dx.IsZero() && dz.IsZero()) dz = Fixed::One();
        placeLocked(i, bx, bz, dx, dz);
        tyresAfreshLocked(s);
        clearMarksLocked(i);
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
     * and placed, each ramp's slopes a pooled wedge and its top a slab, each hill a
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
                Pickup p; p.x = sp.x; p.z = sp.z; p.y = (sp.up ? topAt(sp.x, sp.z) : KartArena::Solid(m_live, sp.x, sp.z)) + F(7, 10); p.back = 0; p.node = m_pickup_nodes[n++];
                m_pickups.push_back(p);
                placeNode(p.node, p.x, p.y, p.z);
                verb(p.node, "SetVisible", "1");
            }
        for (; n < m_pickup_nodes.size(); ++n) hide(m_pickup_nodes[n]);
    }
    // The arena's floor round a map's pits: every piece each pit cuts into
    // leaves what is left of it, in up to four.
    struct Rect { Fixed x0, z0, x1, z1; };
    static std::vector<Rect> floorPieces(const KartArena::Map& m)
    {
        const Fixed half = Fixed::Half();
        std::vector<Rect> floor{ { F(-36), F(-36), F(36), F(36) } };
        for (const KartArena::Pit& p : m.pits)
        {
            const Fixed x0 = p.x - p.w * half, x1 = p.x + p.w * half, z0 = p.z - p.d * half, z1 = p.z + p.d * half;
            std::vector<Rect> next;
            for (const Rect& r : floor)
            {
                if (r.x1 <= x0 || r.x0 >= x1 || r.z1 <= z0 || r.z0 >= z1) { next.push_back(r); continue; }
                if (r.x0 < x0) next.push_back({ r.x0, r.z0, x0, r.z1 });
                if (r.x1 > x1) next.push_back({ x1, r.z0, r.x1, r.z1 });
                const Fixed a = Fixed::Max(r.x0, x0), b = Fixed::Min(r.x1, x1);
                if (r.z0 < z0) next.push_back({ a, r.z0, b, z0 });
                if (r.z1 > z1) next.push_back({ a, z1, b, r.z1 });
            }
            floor.swap(next);
        }
        return floor;
    }
    // What a map needs of each pool to be drawn whole, "" when it has it: a
    // thing past a pool's end is not drawn, and a block not drawn is not there.
    std::string shortLocked(const KartArena::Map& m) const
    {
        size_t wedges = 0, slabs = m.decks.size() + m.pits.size() * 5 + (m.pits.empty() ? 0 : floorPieces(m).size());
        for (const KartArena::Ramp& r : m.ramps) { wedges += r.up.IsPositive() + r.down.IsPositive(); slabs += r.flat.IsPositive(); }
        std::string out;
        auto need = [&out](const char* what, size_t n, size_t pool) {
            if (pool != 0 && n > pool) out += std::to_string(n) + " " + what + ", the pool has " + std::to_string(pool) + "\n";
        };
        need("blocks", m.blocks.size(), m_block_nodes.size());
        need("wedges", wedges, m_wedge_nodes.size());
        need("slabs", slabs, m_slab_nodes.size());
        need("domes", m.hills.size(), m_dome_nodes.size());
        need("pads", m.pads.size(), m_pad_nodes.size());
        need("springs", m.springs.size(), m_spring_nodes.size());
        need("boxes", m.pickups.size(), m_pickup_nodes.size());
        return out;
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
            poseNode(node, b.x, b.base + b.h * Fixed::Half(), b.z, b.yaw, Fixed::Zero(), Fixed::One());
            verb(node, "SetVisible", "1");
            m_live_blocks.push_back(node);
        }
        // A ramp, solid to the ground it stands on (a pit's floor, for one in
        // a pit): its slope up a wedge, its top a box, its slope down a wedge
        // turned about; a jump has no slope down -- its lip is a wall's top.
        // Nothing is hollow, so nothing is under one.
        size_t nw = 0;
        for (const KartArena::Ramp& r : m_live.ramps)
        {
            const Fixed len = r.length();
            auto part = [&](std::vector<ETCS::RID>& pool, size_t& n, Fixed from, Fixed run, int turn) {
                if (n >= pool.size() || !run.IsPositive()) return;
                const ETCS::RID node = pool[n++];
                const Fixed mid = from + run * Fixed::Half() - len * Fixed::Half();
                verb(node, "Create", fmt3(r.width.ToFloat()) + ", " + fmt3(r.height.ToFloat()) + ", " + fmt3(run.ToFloat()));
                poseNode(node, r.x + r.sx * mid, r.base + r.height * Fixed::Half(), r.z + r.cz * mid, r.yaw + turn, Fixed::Zero(), Fixed::One());
                groundColour(node, KartArena::OnRamp);
                verb(node, "SetVisible", "1");
            };
            part(m_wedge_nodes, nw, Fixed::Zero(), r.up, 0);
            part(m_slab_nodes, ns, r.up, r.flat, 0);
            part(m_wedge_nodes, nw, r.up + r.flat, r.down, 180);
        }
        for (; nw < m_wedge_nodes.size(); ++nw) parkNode(m_wedge_nodes[nw]);
        // The levels, of slabs: a deck a plate in the air; a pit its floor and
        // four walls down to it -- and the arena's floor then laid round the
        // pits in pieces, the whole one put away.
        auto slab = [&](Fixed x, Fixed y, Fixed z, Fixed w, Fixed h, Fixed d, const float* c) {
            if (ns >= m_slab_nodes.size() || !w.IsPositive() || !d.IsPositive()) return;
            const ETCS::RID node = m_slab_nodes[ns++];
            verb(node, "Create", fmt3(w.ToFloat()) + ", " + fmt3(h.ToFloat()) + ", " + fmt3(d.ToFloat()));
            poseNode(node, x, y, z, 0, Fixed::Zero(), Fixed::One());
            verb(node, "SetColor", f4(c[0], c[1], c[2], 1.0f));
            verb(node, "SetVisible", "1");
        };
        const Fixed half = Fixed::Half(), thick = KartArena::DeckThick();
        for (const KartArena::Deck& d : m_live.decks)
            slab(d.x, d.h - thick * half, d.z, d.w, thick, d.d, kGround[KartArena::OnDeck]);
        for (const KartArena::Pit& p : m_live.pits)
        {
            const Fixed x0 = p.x - p.w * half, x1 = p.x + p.w * half, z0 = p.z - p.d * half, z1 = p.z + p.d * half;
            const Fixed mid = -p.depth * half - F(1, 100), wall = F(3, 10);
            slab(p.x, -p.depth - F(5, 100), p.z, p.w, F(1, 10), p.d, kGround[KartArena::InPit]);
            slab(p.x, mid, z1 + wall * half, p.w + wall * F(2), p.depth, wall, kPitWall);
            slab(p.x, mid, z0 - wall * half, p.w + wall * F(2), p.depth, wall, kPitWall);
            slab(x1 + wall * half, mid, p.z, wall, p.depth, p.d, kPitWall);
            slab(x0 - wall * half, mid, p.z, wall, p.depth, p.d, kPitWall);
        }
        const std::vector<Rect> floor = floorPieces(m_live);
        if (m_live.pits.empty()) { if (m_floor) verb(m_floor, "SetVisible", "1"); }
        else
        {
            if (m_floor) hide(m_floor);
            for (const Rect& r : floor)
                slab((r.x0 + r.x1) * half, F(-5, 100), (r.z0 + r.z1) * half, r.x1 - r.x0, F(1, 10), r.z1 - r.z0, kGround[KartArena::Floor]);
        }
        for (const KartArena::Hill& k : m_live.hills)
        {
            if (nd >= m_dome_nodes.size()) break;
            const ETCS::RID node = m_dome_nodes[nd++];
            const std::string a = fmt3((k.a * F(2)).ToFloat()), b = fmt3((k.b * F(2)).ToFloat());
            verb(node, "Create", a + ", " + b + ", " + a);
            poseNode(node, k.x, k.base + k.y0, k.z, 0, Fixed::Zero(), Fixed::One());
            verb(node, "SetVisible", "1");
        }
        for (; nb < m_block_nodes.size(); ++nb) parkNode(m_block_nodes[nb]);
        for (; ns < m_slab_nodes.size(); ++ns) parkNode(m_slab_nodes[ns]);
        for (; nd < m_dome_nodes.size(); ++nd) parkNode(m_dome_nodes[nd]);
        // The pads: each plate on the floor, under any tyre's mark; the
        // arrows are put on it every tick (arrowsLocked).
        for (size_t n = 0; n < m_pad_nodes.size(); ++n)
        {
            const PadNodes& pn = m_pad_nodes[n];
            if (n < m_live.pads.size())
            {
                const KartArena::Pad& p = m_live.pads[n];
                poseNode(pn.plate, p.x, p.y + F(3, 1000), p.z, p.yaw, Fixed::Zero(), Fixed::One());
                verb(pn.plate, "SetVisible", "1");
                for (ETCS::RID a : pn.arrow) verb(a, "SetVisible", "1");
            }
            else { parkNode(pn.plate); for (ETCS::RID a : pn.arrow) parkNode(a); }
        }
        for (size_t n = 0; n < m_spring_nodes.size(); ++n)
        {
            const SpringNodes& sn = m_spring_nodes[n];
            if (n < m_live.springs.size())
            {
                const KartArena::Spring& p = m_live.springs[n];
                poseNode(sn.plate, p.x, p.y + F(4, 1000), p.z, 0, Fixed::Zero(), Fixed::One());
                verb(sn.plate, "SetVisible", "1");
                for (ETCS::RID a : sn.arrow) verb(a, "SetVisible", "1");
            }
            else { parkNode(sn.plate); for (ETCS::RID a : sn.arrow) parkNode(a); }
        }
        arrowsLocked();
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
                        heatsLocked(s);
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
                if (alive(s)) { wheelsLocked(s); marksLocked(i); }
            }
            if (m_phase != Lobby) pickupsLocked();
            for (size_t b = 0; b < m_blast_nodes.size(); ++b)
                if (m_blast_until[b] && static_cast<int64_t>(m_tick) >= m_blast_until[b]) { m_blast_until[b] = 0; hide(m_blast_nodes[b]); }
            arrowsLocked();
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
        const Fixed hx = s.hx, hz = s.hz;
        Fixed vx, vy, vz;
        r.Velocity(m, vx, vy, vz);
        // A pad under it pushes the way the pad points, whichever way the
        // kart does: along its heading that is speed, across it a slide for
        // the tyres. The work is the Impulse below, with the pedal's.
        if (!s.air)
            if (const KartArena::Pad* pad = KartArena::PadAt(m_live, r.x, r.z))
                if ((groundFor(s, r, r.x, r.z) - pad->y).Abs() < F(5, 100))
                {
                    const Fixed along = vx * pad->sx + vz * pad->cz;
                    if (along < padTop())
                    {
                        const Fixed push = (padPush() + (along.IsPositive() ? along * padGain() : Fixed::Zero())) * step;
                        const Fixed add = Fixed::Min(push, padTop() - along);
                        vx += pad->sx * add; vz += pad->cz * add;
                    }
                }
        Fixed speed = vx * hx + vz * hz;
        wheelTurnLocked(s, steer, speed, step);
        if (s.boost > 0) --s.boost;
        if (s.air) return;
        Fixed lx = vx - hx * speed, lz = vz - hz * speed;
        const Fixed rolled = speed;
        const bool braking = (thr > 0 && speed.raw < 0) || (thr < 0 && speed.raw > 0);
        const Fixed top = s.boost > 0 ? boostSpeed() : topSpeed();
        Fixed push = pushAt(speed, top);
        if (s.boost > 0 && push < boostPush()) push = boostPush();
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
        // What the brake took is the tyres' work against the road.
        const Fixed braked = braking ? Fixed::Half() * m * (rolled * rolled - speed * speed) : Fixed::Zero();
        // The slope: gravity's share along the heading (the sine of the rise).
        Fixed gx, gz;
        groundFor(s, r, r.x, r.z, &gx, &gz);
        const Fixed along = gx * hx + gz * hz;
        if (!along.IsZero()) speed -= gravity() * along / (Fixed::One() + along * along).Sqrt() * step;
        // Past the kart's own top (a boost run out, a pad taken, a hill run
        // down) it eases back on the level; nothing goes past a pad's top.
        if (speed > top && along.raw >= 0) speed = Fixed::Max(top, speed - brake() * Fixed::Half() * step);
        if (speed > padTop()) speed = padTop();
        // The heading turns by speed x wheel / wheelbase, about +y: a turn of
        // +a takes +z toward +x, which with +z ahead is to the picture's right.
        const Fixed yaw = speed * s.wheel / wheelbase() * step;
        if (!yaw.IsZero())
        {
            Fixed sn, cs;
            yaw.SinCos(sn, cs);
            const Fixed nx = hx * cs + hz * sn, nz = hz * cs - hx * sn;
            const Fixed l = Fixed::Length(nx, Fixed::Zero(), nz);
            s.hx = nx / l; s.hz = nz / l;
        }
        // The tyres: what the kart's way is across its new heading they pull
        // out, as hard as they grip -- a turn within that bends the way with
        // the heading; past it (too fast, too much lock, the brake on) the
        // rest stays a slide across, and the kart drifts. The pull turns the
        // way; of the speed it would take with it, a slide keeps most
        // (slideCost): a drift carries its speed round a corner.
        const Fixed wx = hx * speed + lx, wz = hz * speed + lz;
        const Fixed on = wx * s.hx + wz * s.hz;
        Fixed ax = wx - s.hx * on, az = wz - s.hz * on;
        const Fixed across = Fixed::Length(ax, Fixed::Zero(), az);
        const Fixed most = (braking ? brakeGrip() : grip()) * step;
        if (across > most) { const Fixed keep = (across - most) / across; ax *= keep; az *= keep; }
        else               { ax = az = Fixed::Zero(); }
        Fixed nx = s.hx * on + ax, nz = s.hz * on + az;
        const Fixed was = Fixed::Length(wx, Fixed::Zero(), wz), pulled = Fixed::Length(nx, Fixed::Zero(), nz);
        Fixed now = pulled;
        if (pulled.IsPositive() && was > pulled)
        {
            now = was - slideCost(across / was) * (was - pulled);
            nx = nx * now / pulled; nz = nz * now / pulled;
        }
        const Fixed scrubbed = Fixed::Half() * m * (was * was - now * now);
        // THE LEDGER: the pedal's work is what the motion gained plus what
        // the tyres took (an Impulse, from outside); what the motion lost
        // beyond that is heat in the body (SetVelocity), and the tyres' part
        // of it -- the brake, the scrub -- crosses into the tyres that did it.
        const Fixed after = Fixed::Half() * m * (nx * nx + nz * nz);
        const Fixed before = r.KineticEnergy();
        const Fixed friction = braked + scrubbed;
        if (after + friction > before) k->Impulse(nx, Fixed::Zero(), nz, after + friction - before);
        r.SetVelocity(m, nx, Fixed::Zero(), nz);
        tyresTakeLocked(s, r, braked, scrubbed, wx, wz, yaw / step);
    }

    /*
     * THE TYRES TAKE THE FRICTION, as heat out of the body: the brake's part
     * a quarter each, the scrub's by how fast each tyre's patch of road
     * slides across the way that tyre rolls (the body's turn adds to its
     * way; the front two roll where the wheel points). Each tyre sheds it
     * again at its emissivity (kart_wheel.etcs), up into the body -- and what
     * it sheds per unit of road is its mark (marksLocked).
     */
    void tyresTakeLocked(Slot& s, OrderVector& r, Fixed braked, Fixed scrubbed, Fixed wx, Fixed wz, Fixed spin)
    {
        Fixed total = braked + scrubbed;
        if (!total.IsPositive()) return;
        if (total > r.Heat()) { const Fixed f = r.Heat() / total; braked *= f; scrubbed *= f; total = r.Heat(); }
        Fixed slide[4], sum = Fixed::Zero();
        Fixed dsn, dcs;
        s.wheel.SinCos(dsn, dcs);
        for (int t = 0; t < 4; ++t)
        {
            // The tyre's place off the body's middle, level: across (hz, -hx), ahead (hx, hz).
            const Fixed sx = (t % 2) ? -tyreSide() : tyreSide(), sz = (t < 2) ? tyreAhead() : -tyreAhead();
            const Fixed ox = s.hz * sx + s.hx * sz, oz = -s.hx * sx + s.hz * sz;
            // Its patch's way: the body's, and the turn's (+spin takes +x toward -z).
            const Fixed px = wx + spin * oz, pz = wz - spin * ox;
            Fixed rx = s.hx, rz = s.hz;
            if (t < 2) { rx = s.hx * dcs + s.hz * dsn; rz = s.hz * dcs - s.hx * dsn; }
            slide[t] = (px * rz - pz * rx).Abs();
            sum += slide[t];
        }
        r.energy -= total;
        for (int t = 0; t < 4; ++t)
        {
            const Fixed share = braked / F(4) + (sum.IsPositive() ? scrubbed * slide[t] / sum : scrubbed / F(4));
            if (!share.IsPositive()) continue;
            Causal_* c = causal(s.wheels[t]);
            if (!c) { r.energy += share; continue; }   // no tyre: the body keeps it
            OrderVector heat;
            heat.energy = share;
            c->AbsorbUnder(heat);
        }
    }

    /*
     * THE WHEEL, turned by A and D. A press puts it at the lock for the
     * kart's speed (lockAt) at once -- the wheel answers the key, not a
     * moment after it -- and held, it eases on from there to twice the lock
     * over steerEase: a tap is a firm turn, a long hold a hard one. Turned
     * the other way it is at that side's lock at once and eases again from
     * there. Let go, it goes back to the middle at steerBack; and past where
     * it should be (the kart speeding up with the wheel turned, so the lock
     * shrinking) it comes back at that pace too.
     */
    static void wheelTurnLocked(Slot& s, int steer, Fixed speed, Fixed step)
    {
        if (steer != s.steer_held) s.wheel_ease = Fixed::Zero();
        s.steer_held = steer;
        Fixed target = Fixed::Zero();
        if (steer != 0)
        {
            s.wheel_ease = Fixed::Min(s.wheel_ease + step / steerEase(), Fixed::One());
            target = lockAt(speed) * (Fixed::One() + s.wheel_ease) * F(steer);
            // Short of it: there at once (the press's jump, and the ease after it).
            if (((target - s.wheel) * F(steer)).IsPositive()) { s.wheel = target; return; }
        }
        const Fixed turn = steerBack() * step;
        Fixed d = target - s.wheel;
        if (d > turn) d = turn; else if (d < -turn) d = -turn;
        s.wheel += d;
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
        Fixed g = groundFor(s, r, r.x, r.z, &gx, &gz) + kartY();
        const Fixed sp = Fixed::Length(vx, Fixed::Zero(), vz);
        bool wall = g - r.y > climb();
        if (!wall && sp.IsPositive() && !s.air)
        {
            const Fixed nx = r.x + vx / sp * F(9, 10), nz = r.z + vz / sp * F(9, 10);
            wall = groundFor(s, r, nx, nz) + kartY() - r.y > noseRise();
        }
        if (wall)
        {
            // On the ground it is stopped; in the air it only does not go on
            // (a kart thrown up beside a pit's wall clears it as it rises).
            r.PlaceAt(s.px, r.y, s.pz);
            if (!s.air) r.SetVelocity(m, Fixed::Zero(), Fixed::Zero(), Fixed::Zero());
            g = groundFor(s, r, s.px, s.pz, &gx, &gz) + kartY();
        }
        Fixed y = r.y;
        const Fixed step = dt();
        // A spring under it throws it up, whatever way it was going.
        if (!s.air)
            if (const KartArena::Spring* up = KartArena::SpringAt(m_live, r.x, r.z))
                if ((g - kartY() - up->y).Abs() < F(5, 100)) { s.air = true; s.vy = springKick(); }
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
    // The ground under a kart at (x, z): the solid ground, or a deck it is on
    // or over -- rising it goes through one, falling it lands on it
    // (KartArena::Height).
    Fixed groundFor(const Slot& s, const OrderVector& r, Fixed x, Fixed z, Fixed* gx = nullptr, Fixed* gz = nullptr) const
    {
        const Fixed reach = s.air && s.vy.IsPositive() ? Fixed::Zero() : climb();
        return KartArena::Height(m_live, x, z, r.y - kartY(), reach, gx, gz);
    }
    // ...under a shot at height y, and the top of everything at a point.
    Fixed groundAt(Fixed x, Fixed z, Fixed y) const { return KartArena::Height(m_live, x, z, y, Fixed::Zero()); }
    Fixed topAt(Fixed x, Fixed z, Fixed* gx = nullptr, Fixed* gz = nullptr) const { return KartArena::Top(m_live, x, z, gx, gz); }

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
                Shot& m = shootLocked(MineShot, i, mx, groundAt(mx, mz, y - kartY() + climb()) + F(1, 10), mz, Fixed::Zero(), Fixed::Zero(), Fixed::Zero(), 30 * kHz);
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
            if (solidAt(sx, y, sz, F(1, 10)) || groundAt(sx, sz, y) > y - F(15, 100)) { reach = t; hit = -1; break; }
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
                    const bool ground = s.y < groundAt(s.x, s.z, s.y) + (s.kind == BombShot ? F(3, 10) : F(5, 100));
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

    // The tyres at the body's corners, turned with it (tilt and all), the
    // front two turned by the wheel. They are its members, so where they are
    // is stated in its frame -- which goes where the body goes; but frames
    // are translations, so the body's turn and tilt are stated here.
    void wheelsLocked(const Slot& s)
    {
        Causal_* k = causal(s.body);
        if (!k) return;
        std::lock_guard<std::recursive_mutex> lk(k->TreeMutex());
        const OrderVector& r = k->Order4();
        const Fixed down = F(3, 100);
        for (int i = 0; i < 4; ++i)
        {
            Causal_* w = causal(s.wheels[i]);
            if (!w) continue;
            OrderVector& q = w->RowsUnder();
            // In the body's own frame: +z ahead, the front right wheel at +x
            // (the heading's across, (hz, -hx), on the level).
            Fixed ox = (i % 2) ? -tyreSide() : tyreSide(), oy = -down, oz = (i < 2) ? tyreAhead() : -tyreAhead();
            r.RotateVector(ox, oy, oz);
            q.PlaceAt(ox, oy, oz);
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

    // ── the pads' arrows ────────────────────────────────────────────────

    // Three arrows a pad, running its length one behind the other, like a
    // belt; four a spring, closing on its middle. Where each is follows from
    // the tick, so the same everywhere and no state. Flat on the plate, above
    // any tyre's mark. The rows are written under the picture, which a still
    // scene does not take again: the world is told it moved (by nothing), so
    // they run in the lobby and with every kart at rest.
    void arrowsLocked()
    {
        const size_t pads = std::min(m_pad_nodes.size(), m_live.pads.size());
        const size_t springs = std::min(m_spring_nodes.size(), m_live.springs.size());
        if (pads + springs) verb(m_world, "Move", "0, 0, 0");
        const Fixed run = KartArena::PadLength() - padArrow();   // an arrow's middle goes this far
        const Fixed beat = F(static_cast<int64_t>(m_tick % static_cast<uint64_t>(padBeat())), padBeat());
        for (size_t n = 0; n < pads; ++n)
        {
            const KartArena::Pad& p = m_live.pads[n];
            for (int k = 0; k < 3; ++k)
            {
                Fixed at = (F(k) + beat) / F(3);
                at = (at - Fixed::Half()) * run;
                poseNode(m_pad_nodes[n].arrow[k], p.x + p.sx * at, p.y + F(6, 100), p.z + p.cz * at, p.yaw, Fixed::One(), Fixed::Zero());
            }
        }
        // From the rim in: each on its own side (+x, +z, -x, -z), pointing at the middle.
        static const int side[4][3] = { { 1, 0, 270 }, { 0, 1, 180 }, { -1, 0, 90 }, { 0, -1, 0 } };
        const Fixed out = F(115, 100) - F(65, 100) * beat;
        for (size_t n = 0; n < springs; ++n)
        {
            const KartArena::Spring& p = m_live.springs[n];
            for (int k = 0; k < 4; ++k)
                poseNode(m_spring_nodes[n].arrow[k], p.x + out * F(side[k][0]), p.y + F(6, 100), p.z + out * F(side[k][1]),
                         side[k][2], Fixed::One(), Fixed::Zero());
        }
    }

    // ── the tyres' marks ────────────────────────────────────────────────

    // Each tyre's heat before the tick's interaction, which sheds from it.
    void heatsLocked(Slot& s)
    {
        for (int t = 0; t < 4; ++t)
            if (Causal_* c = causal(s.wheels[t])) s.tyre_heat[t] = c->Order4().Heat();
    }
    // A tyre's place in the world: the body's, and the tyre's own in its frame.
    bool tyreAt(const Slot& s, int t, Fixed& x, Fixed& y, Fixed& z)
    {
        Causal_* k = causal(s.body);
        Causal_* c = causal(s.wheels[t]);
        if (!k || !c) return false;
        const OrderVector& r = k->Order4();
        const OrderVector& q = c->Order4();
        x = r.x + q.x; y = r.y + q.y; z = r.z + q.z;
        return true;
    }

    /*
     * THE MARKS, after the tick: each tyre's step since the last, and what
     * it shed over it (its heat before the tick, less its heat now), added to
     * the open run -- or, when the step turns from the run's way, is of
     * another darkness, or the tyre left or met the ground, the run is laid
     * as a segment first and a new one starts. A run long enough is laid as
     * it stands; a kart come to a stop lays a dark run's end at its tyres.
     */
    void marksLocked(size_t i)
    {
        Slot& s = m_slots[i];
        Causal_* k = causal(s.body);
        if (!k) return;
        const Fixed quarter = k->MassUnder() / F(4);
        for (int t = 0; t < 4; ++t)
        {
            Trail& tr = s.trail[t];
            Fixed x, y, z;
            if (!tyreAt(s, t, x, y, z)) continue;
            Fixed shed = Fixed::Zero();
            if (Causal_* c = causal(s.wheels[t])) shed = s.tyre_heat[t] - c->Order4().Heat();
            if (shed.raw < 0 || s.air) shed = Fixed::Zero();
            const Fixed gy = groundFor(s, k->Order4(), x, z);
            if (!tr.touched)
            {
                tr.touched = true; tr.air = s.air;
                tr.sx = tr.hx = x; tr.sy = tr.hy = gy; tr.sz = tr.hz = z;
                continue;
            }
            const Fixed stx = x - tr.hx, stz = z - tr.hz;
            const Fixed step = Fixed::Length(stx, Fixed::Zero(), stz);
            if (step < F(1, 100))
            {
                if (tr.run_level > 0 && tr.run_d >= F(1, 10)) layLocked(i, t, tr.hx, tr.hy, tr.hz);
                continue;
            }
            const int level = s.air ? 0 : levelOf(markOf(shed, step, quarter));
            // A run shorter than markMin() is not ended by a turn or a shade:
            // a kart held at a wall, shoved back and forth, lays nothing new.
            if (tr.run_d.IsPositive() && Fixed::Length(tr.hx - tr.sx, Fixed::Zero(), tr.hz - tr.sz) >= markMin())
            {
                const bool turned = stx * tr.dx + stz * tr.dz < markTurn() * step;
                const bool shade = (level > 0) != (tr.run_level > 0) || std::abs(level - tr.run_level) >= 3;
                if (turned || shade || s.air != tr.air) layLocked(i, t, tr.hx, tr.hy, tr.hz);
            }
            if (!tr.run_d.IsPositive()) { tr.dx = stx / step; tr.dz = stz / step; tr.air = s.air; }
            tr.run_e += shed;
            tr.run_d += step;
            tr.run_level = tr.air ? 0 : levelOf(markOf(tr.run_e, tr.run_d, quarter));
            tr.hx = x; tr.hy = gy; tr.hz = z;
            const Fixed chord = Fixed::Length(x - tr.sx, Fixed::Zero(), z - tr.sz);
            if (chord >= (tr.run_level > 0 ? markSegMax() : markRunMax())) layLocked(i, t, x, gy, z);
            trimLocked(i, t);
        }
    }
    // Darkness from what was shed over a run of road, 0..1, and as a level of 8.
    static Fixed markOf(Fixed shed, Fixed run, Fixed quarter)
    {
        if (!run.IsPositive() || !quarter.IsPositive()) return Fixed::Zero();
        const Fixed a = (shed / run / quarter - markFrom()) / (markFull() - markFrom());
        return a.raw < 0 ? Fixed::Zero() : Fixed::Min(a, Fixed::One());
    }
    static int levelOf(Fixed a) { return static_cast<int>((a * F(8) + Fixed::Half()).ToFloat()); }
    /*
     * The open run laid, from its start to (x, y, z): a segment, drawn when
     * dark -- sized to its length, laid along it, tinted between the ground's
     * colour and the tyre's -- and left where it is. Lifted a hair by tyre
     * and by how many came before, so two that overlap (a rear tyre in a
     * front one's track, a run back over its own) are never at one height.
     */
    void layLocked(size_t i, int t, Fixed x, Fixed y, Fixed z)
    {
        Trail& tr = m_slots[i].trail[t];
        MarkSeg g;
        g.x0 = tr.sx; g.y0 = tr.sy; g.z0 = tr.sz; g.x1 = x; g.y1 = y; g.z1 = z;
        g.len = Fixed::Length(x - tr.sx, y - tr.sy, z - tr.sz);
        g.level = tr.run_level;
        const size_t base = (i * 4 + static_cast<size_t>(t)) * kMarkNodes;
        if (g.level > 0 && g.len.IsPositive() && base + kMarkNodes <= m_mark_nodes.size())
        {
            // A node of the tyre's own: a free one, or the oldest drawn one's.
            while (tr.used == (1u << kMarkNodes) - 1u)
            {
                for (auto it = tr.segs.rbegin(); it != tr.segs.rend(); ++it)
                    if (it->node >= 0) { unmarkLocked(base, tr, *it); break; }
            }
            int n = 0;
            while (tr.used & (1u << n)) ++n;
            tr.used |= 1u << n;
            g.node = n;
            const ETCS::RID node = m_mark_nodes[base + static_cast<size_t>(n)];
            const Fixed lift = F(8, 1000) + F(1, 1000) * F(static_cast<int64_t>(t * 8 + static_cast<int>(tr.seq % 8)));
            KartArena::Surface on = KartArena::Floor;
            const Fixed mx = (g.x0 + g.x1) * Fixed::Half(), mz = (g.z0 + g.z1) * Fixed::Half();
            KartArena::Height(m_live, mx, mz, (g.y0 + g.y1) * Fixed::Half(), climb(), nullptr, nullptr, &on);
            verb(node, "Create", "0.18, 0.01, " + fmt3(g.len.ToFloat() + 0.02f));
            poseAlong(node, mx, (g.y0 + g.y1) * Fixed::Half() + lift, mz, g.x1 - g.x0, g.y1 - g.y0, g.z1 - g.z0);
            const float a = static_cast<float>(g.level) / 8.0f;
            const float* c = kGround[on];
            verb(node, "SetColor", f4(c[0] + (kTyre[0] - c[0]) * a, c[1] + (kTyre[1] - c[1]) * a, c[2] + (kTyre[2] - c[2]) * a, 1.0f));
            verb(node, "SetVisible", "1");
        }
        if (g.len.IsPositive()) { tr.segs.push_front(g); ++tr.seq; }
        tr.sx = x; tr.sy = y; tr.sz = z;
        tr.run_e = tr.run_d = Fixed::Zero();
        tr.run_level = 0;
    }
    // The rope's far end: the oldest segments go while what is left, the
    // open run with it, is still markLength() of road.
    void trimLocked(size_t i, int t)
    {
        Trail& tr = m_slots[i].trail[t];
        const size_t base = (i * 4 + static_cast<size_t>(t)) * kMarkNodes;
        Fixed rope = tr.run_d;
        for (const MarkSeg& m : tr.segs) rope += m.len;
        while (!tr.segs.empty() && rope - tr.segs.back().len >= markLength())
        {
            rope -= tr.segs.back().len;
            unmarkLocked(base, tr, tr.segs.back());
            tr.segs.pop_back();
        }
    }
    // A segment no longer drawn: its node parked and free.
    void unmarkLocked(size_t base, Trail& tr, MarkSeg& g)
    {
        if (g.node < 0) return;
        if (base + static_cast<size_t>(g.node) < m_mark_nodes.size()) parkNode(m_mark_nodes[base + static_cast<size_t>(g.node)]);
        tr.used &= ~(1u << g.node);
        g.node = -1;
    }
    // A tyre's mark gone: the trail forgotten, its segments parked.
    void clearMarksLocked(size_t i)
    {
        Slot& s = m_slots[i];
        for (int t = 0; t < 4; ++t)
        {
            Trail& tr = s.trail[t];
            const size_t base = (i * 4 + static_cast<size_t>(t)) * kMarkNodes;
            for (MarkSeg& g : tr.segs) unmarkLocked(base, tr, g);
            tr = Trail();
        }
    }
    // The tyres' rows as made: cold, and nothing shed to come.
    void tyresAfreshLocked(Slot& s)
    {
        for (int t = 0; t < 4; ++t)
            if (Causal_* c = causal(s.wheels[t]))
            {
                std::lock_guard<std::recursive_mutex> lk(c->TreeMutex());
                OrderVector& q = c->RowsUnder();
                const uint64_t id = q.id;
                q = s.tyre_pristine[t];
                q.id = id;
                s.tyre_heat[t] = Fixed::Zero();
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
    // A pooled node laid along (dx, dy, dz) from its middle: its long side
    // (+z) turned onto the way and pitched to its rise.
    void poseAlong(ETCS::RID node, Fixed x, Fixed y, Fixed z, Fixed dx, Fixed dy, Fixed dz)
    {
        Causal_* c = causal(node);
        if (!c) return;
        Fixed yw, yy;
        halfTurn(dx, dz, yw, yy);
        const Fixed level = Fixed::Length(dx, Fixed::Zero(), dz), len = Fixed::Length(dx, dy, dz);
        Fixed pc = Fixed::One(), ps = Fixed::Zero();
        if (len.IsPositive())
        {
            const Fixed cs = level / len;   // pitched up by the rise: about its own across, -dy/len
            pc = ((Fixed::One() + cs) * Fixed::Half()).Sqrt();
            ps = ((Fixed::One() - cs) * Fixed::Half()).Sqrt();
            if (dy.IsPositive()) ps = -ps;
        }
        std::lock_guard<std::recursive_mutex> lk(c->TreeMutex());
        OrderVector& r = c->RowsUnder();
        r.PlaceAt(x, y, z);
        r.qw = yw * pc; r.qx = yw * ps; r.qy = yy * pc; r.qz = -(yy * ps);
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
    static void groundColour(ETCS::RID r, KartArena::Surface on)
    {
        verb(r, "SetColor", f4(kGround[on][0], kGround[on][1], kGround[on][2], 1.0f));
    }

    // ── the battle as one number ────────────────────────────────────────

    std::string hashLocked()
    {
        std::string st = std::to_string(m_tick) + " " + std::to_string(m_phase) + " " + std::to_string(m_end) + " "
                       + std::to_string(m_map) + " " + std::to_string(m_layout) + " " + std::to_string(m_round_secs)
                       + " " + std::to_string(m_rng);
        for (const Slot& s : m_slots)
        {
            st += "|" + s.driver + " " + std::to_string(s.thr) + std::to_string(s.steer) + std::to_string(s.fire)
                + std::to_string(s.press) + " " + std::to_string(s.wheel.raw) + " " + std::to_string(s.wheel_ease.raw) + " "
                + std::to_string(s.steer_held) + " " + std::to_string(s.hx.raw) + " "
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
            for (ETCS::RID t : s.wheels)
                if (Causal_* c = causal(t)) st += " t" + std::to_string(c->Order4().energy.raw);
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
    void turnLookLocked(float yaw, float pitch)
    {
        m_look_yaw += yaw;
        while (m_look_yaw > kPi)  m_look_yaw -= 2.0f * kPi;
        while (m_look_yaw < -kPi) m_look_yaw += 2.0f * kPi;
        m_look_pitch = std::max(kLookDown, std::min(kLookUp, m_look_pitch + pitch));
        m_look_idle_ms = 0.0;
    }
    // Q and E turn the look while held; left alone for kLookHoldMs it goes
    // back to behind the kart, to about a third of what is left every kLookBackMs.
    void lookLocked(double dt_ms)
    {
        const int keys = ((m_look_keys & 2u) ? 1 : 0) - ((m_look_keys & 1u) ? 1 : 0);
        if (keys != 0) turnLookLocked(static_cast<float>(keys * kLookKeyRate * dt_ms / 1000.0), 0.0f);
        if (keys != 0 || m_look_drag) return;
        m_look_idle_ms += dt_ms;
        if (m_look_idle_ms < kLookHoldMs || (m_look_yaw == 0.0f && m_look_pitch == 0.0f)) return;
        const float keep = static_cast<float>(std::exp(-dt_ms / kLookBackMs));
        m_look_yaw *= keep; m_look_pitch *= keep;
        if (std::fabs(m_look_yaw) < 1e-3f && std::fabs(m_look_pitch) < 1e-3f) m_look_yaw = m_look_pitch = 0.0f;
    }
    /*
     * WHERE THE EYE SHOULD BE: over the arena in the lobby; in a round on a
     * boom behind this runtime's kart and over it, swung by the look, looking
     * past the kart the way the boom points. The boom is shortened to what
     * it can see along: where the ground comes up through it (a pit's wall,
     * a ramp, a hill), a deck lies between the kart and the eye, or it would
     * pass out through the arena's wall below the wall's top, the eye comes
     * in to this side of it -- into the pit with the kart, under the deck
     * with it, inside the wall with it -- instead of looking at the kart
     * through the ground, or at the back of a wall.
     */
    void viewLocked(float eye[3], float at[3])
    {
        eye[0] = 0.0f; eye[1] = 62.0f; eye[2] = -46.0f;
        at[0] = at[1] = at[2] = 0.0f;
        const int i = slotOf(m_me);
        if (m_phase == Lobby || i < 0 || !alive(m_slots[i])) return;
        Causal_* k = causal(m_slots[i].body);
        if (!k) return;
        float px, py, pz;
        {
            std::lock_guard<std::recursive_mutex> lk(k->TreeMutex());
            const OrderVector& r = k->Order4();
            px = r.x.ToFloat(); py = r.y.ToFloat(); pz = r.z.ToFloat();
        }
        const float fx = m_slots[i].hx.ToFloat(), fz = m_slots[i].hz.ToFloat();
        const float cy = std::cos(m_look_yaw), sy = std::sin(m_look_yaw);
        const float dx = fx * cy + fz * sy, dz = fz * cy - fx * sy;   // the heading, turned to the picture's right
        const float rise = kBoomRise + m_look_pitch;
        const float back = kBoom * std::cos(rise), up = kBoom * std::sin(rise);
        at[0] = px + dx * 4.0f; at[1] = py + 0.6f; at[2] = pz + dz * 4.0f;
        // The arena's wall: how far out its inside is, and its top.
        const float inside = bound().ToFloat() + 1.0f;
        float wall_top = 0.0f;
        for (ETCS::RID w : m_walls)
            if (Causal_* c = causal(w))
            {
                std::lock_guard<std::recursive_mutex> lk(c->TreeMutex());
                wall_top = std::max(wall_top, c->Order4().y.ToFloat() + c->Solid().hy.ToFloat());
            }
        // From the kart's top out along the boom, as far as it is clear.
        const float top = py + 0.9f;
        const float ex = px - dx * back, ey = py + up, ez = pz - dz * back;
        float clear = 1.0f;
        for (int n = 1; n <= kBoomSteps; ++n)
        {
            const float t = static_cast<float>(n) / kBoomSteps;
            const float qx = px + (ex - px) * t, qy = top + (ey - top) * t, qz = pz + (ez - pz) * t;
            const Fixed x = Fixed::From(qx), z = Fixed::From(qz);
            bool blocked = KartArena::Solid(m_live, x, z).ToFloat() > qy - kBoomGap
                        || ((std::fabs(qx) > inside || std::fabs(qz) > inside) && qy < wall_top + kBoomGap);
            for (const KartArena::Deck& d : m_live.decks)
            {
                if (blocked || !KartArena::Over(x, z, d.x, d.z, d.w, d.d)) continue;
                const float plate = d.h.ToFloat(), under = plate - KartArena::DeckThick().ToFloat();
                blocked = top < under ? qy > under - kBoomGap : (top > plate && qy < plate + kBoomGap);
            }
            if (blocked) { clear = std::max(kBoomLeast, static_cast<float>(n - 1) / kBoomSteps); break; }
        }
        eye[0] = px + (ex - px) * clear; eye[1] = top + (ey - top) * clear; eye[2] = pz + (ez - pz) * clear;
    }
    void cameraLocked(double dt_ms)
    {
        lookLocked(dt_ms);
        if (!m_camera) return;
        ETCS::Held<Camera_> cam = ETCS::resolve_held<Camera_>("Camera", m_camera);
        if (!cam) return;
        float eye[3], at[3];
        viewLocked(eye, at);
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
    std::vector<ETCS::RID> m_block_nodes, m_slab_nodes, m_wedge_nodes, m_dome_nodes;
    std::vector<ETCS::RID> m_pickup_nodes, m_shot_nodes, m_blast_nodes, m_mark_nodes;
    std::vector<PadNodes> m_pad_nodes;
    std::vector<SpringNodes> m_spring_nodes;
    ETCS::RID m_floor = 0;
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
    bool     m_invert = false;   // this runtime's wheel keys swapped (SetInvert)
    int      m_sent_t = 0, m_sent_s = 0, m_sent_f = 0;
    int      m_menu = MenuStart;
    bool     m_board_held = false;
    float    m_eye[3] = { 0.0f, 0.0f, 0.0f }, m_at[3] = { 0.0f, 0.0f, 0.0f };
    bool     m_eye_set = false;
    // The look round the kart (lookLocked): how far the boom is swung from
    // behind the kart and raised, by what, and how long it has been let alone.
    float    m_look_yaw = 0.0f, m_look_pitch = 0.0f, m_look_x = 0.0f, m_look_y = 0.0f;
    double   m_look_idle_ms = 0.0;
    unsigned m_look_keys = 0;      // Q is bit 0, E bit 1
    bool     m_look_drag = false;
    double   m_card_ms = 0.0;
    std::string m_card_text;
};

#endif // KARTPROVIDER_KARTBATTLE_H__
