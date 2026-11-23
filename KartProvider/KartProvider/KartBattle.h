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
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

/*
 * ── KartBattle: karts in an arena, one per player, between runtimes ───────
 *
 * THE TRACE IS THE BATTLE, as the record is the game in chess (ChessGame's
 * header note). Every runtime holds its own copy of the world -- the same
 * arena script built it -- and steps it from the host's trace alone:
 *
 *   join                     the author takes the next free kart, at a spawn
 *                            (a seat's first line, taken as it sits)
 *   leave                    the author's kart back in the pit (its seat's
 *                            stream ended, however it did)
 *   drive <t> <s> <f>        the author's pedal, wheel and trigger: -1 0 1, 0 1
 *   tick <n>                 every kart driven, every shot flown, the world
 *                            stepped, up to tick n
 *   config <map> <secs> [<layout>]   (the host's, in the lobby only) the next
 *                            round's map and length; a map made from a seed
 *                            carries its seed, so it is the same everywhere
 *   round <seed>             (the host's, in the lobby) the round starts:
 *                            scores to nothing, everyone respawned, the dice
 *                            seeded
 *   abandon                  (the host's, in a round) back to the lobby, the
 *                            round's scores discarded
 *   give <who> <weapon> [spare|pickup]   (the host's) a weapon in hand
 *                            outright; as the spare; or as a power-up hands
 *                            one over (KartArms.h, two are carried)
 *
 * An AUTHOR is a seat: over a link, the key its link proved (shown by the
 * label it gave, ontology/Trace.h); here, the name this runtime gave.
 *
 * Lines apply in the trace's order, and a tick is a line, so where an input
 * falls between two ticks is the trace's to say and the same everywhere.
 * The world is Scene3D marked driven (SetDriven): no picture steps it. The
 * karts' rows, the ground under them, the shots, the damage and the dice are
 * all in the rows' own integers (Fixed, KartArms.h, KartArena.h), so two
 * runtimes fed the same lines are the same battle (etcs_causal_constraints.md
 * §2). Lockstep, with the trace as the lock: nobody negotiates anything.
 *
 * THE HOST'S BATTLE IS THE AUTHORITY. A guest's keys go to it on the guest's
 * own seat (`battle.Emit() -> host.Take()`, a stream into a surface of the
 * host's battle): the stream is who said it, and the order the host reads
 * the seats in is the order. The host's clock is the battle's: its frame edge
 * steps the world in real time and keeps `tick` lines in pairs, and always
 * before any other line, so a line lands at the tick the host applied it at.
 * A guest's battle mirrors the host's (`host.Trace(0) -> battle.Follow()`): it
 * sees the arena one trip behind the host, and its own keys a round trip
 * late. Alone (Practice) the battle is the authority of a trace nobody
 * follows.
 *
 * A BATTLE THAT DIFFERS is reported, not repaired (ChessShare's rule): every
 * kSnapEvery ticks the host keeps its hash as a line, and a guest reaching
 * that line whose own hash is not the host's says so.
 *
 * THE SESSION IS THE FAMILIES': the trace and its mirrors (Trace), time as
 * `tick` lines (Lockstep), and the streams' edges and the hall (Present) --
 * ontology/Trace.h, Lockstep.h, Present.h. What is written here is what a
 * battle is.
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
 * it up, a pad pushes it along, both harder the faster it comes. A kart that
 * comes down in lava is out, and so is one that goes over the arena's wall
 * and falls outside it. A kart is SOLID -- a sphere to everything
 * (etcs_causal_constraints.md §13) -- and drawn as a body on its four tyres,
 * tilted to the ground, which this battle turns with it after every step
 * (frames are translations). What is drawn for one runtime alone -- its
 * camera, its card, the menu and the board, the HP bars turned to face its
 * eye -- is set on the frame edge and is not a line.
 */
class KartBattle : public LockstepBase<KartBattle>,
                   public PresentBase<KartBattle>,
                   public AnimatedBase<KartBattle>,
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
    // The frame counter: a label this runtime shows its pictures a second on
    // (fpsLocked). This runtime's alone, never a line.
    void BindFps(ETCS::RID r)    { std::lock_guard<std::mutex> g(m_mu); m_fps = r; m_fps_ms = 0.0; m_fps_seen = -1; }

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
    // A pit of lava: a kart that comes down on its floor is out.
    void AddLava(float x, float z, float width, float depth, float deep)
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (m_defs.empty() || width <= 0 || depth <= 0 || deep <= 0) return;
        m_defs.back().pits.push_back({ Fixed::From(x), Fixed::From(z), Fixed::From(width), Fixed::From(depth), Fixed::From(deep), true });
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
    void AddPickupNode(ETCS::RID r) { std::lock_guard<std::mutex> g(m_mu); m_pickup_nodes.push_back(r); parkNode(r); }
    void AddShotNode(ETCS::RID r)   { std::lock_guard<std::mutex> g(m_mu); m_shot_nodes.push_back(r); m_shot_busy.push_back(false); parkNode(r); }
    void AddBlastNode(ETCS::RID r)  { std::lock_guard<std::mutex> g(m_mu); m_blast_nodes.push_back(r); parkNode(r); }
    void AddSparkNode(ETCS::RID r)  { std::lock_guard<std::mutex> g(m_mu); m_spark_nodes.push_back(r); parkNode(r); }
    void AddSmokeNode(ETCS::RID r)  { std::lock_guard<std::mutex> g(m_mu); m_smoke_nodes.push_back(r); parkNode(r); }
    // The weapons' effects (fxLocked's comment): a ball each, and a ring with the mesh this battle bakes for it.
    void AddFxNode(ETCS::RID r)     { std::lock_guard<std::mutex> g(m_mu); m_fx_nodes.push_back(r); m_fx_busy.push_back(false); parkNode(r); }
    void AddRingNode(ETCS::RID r, ETCS::RID mesh)
    {
        std::lock_guard<std::mutex> g(m_mu);
        bakeRingLocked(mesh);
        m_ring_nodes.push_back(r); m_ring_busy.push_back(false); parkNode(r);
    }
    void AddBoardRow(ETCS::RID r)
    {
        std::lock_guard<std::mutex> g(m_mu);
        verb(r, "SetPosition", std::to_string(80) + ", " + std::to_string(96 + 26 * static_cast<int>(m_board.size())));
        verb(r, "SetHidden", "1");
        m_board.push_back({ r, std::string(), false });
    }
    // The arena is made: the first map out, the lobby open. Until then the
    // battle has no maps to offer (Maps) and takes no settings: a page that
    // lists them the moment the first is added would let a round start on an
    // arena half made, and this would then put the first map back under it.
    void Ready()
    {
        std::lock_guard<std::mutex> g(m_mu);
        m_ready = true;
        mapLocked(0, firstLayout(0));
        respawnAllLocked();
    }

    // ── sessions ──────────────────────────────────────────────────────────
    //
    // The session is the families' (ontology/Trace.h, Present.h): these say
    // only what a battle adds -- its hall line, and its note.

    // Alone: the authority of a trace nobody follows.
    void Practice(const std::string& name)
    {
        Absent();
        LockstepBase<KartBattle>::Practice(name);
        std::lock_guard<std::mutex> g(m_mu); m_note.clear();
    }
    // Host: the authority; the hall (a Lobby) lists it as `kart <id>` for as
    // long as it lasts.
    bool Host(const std::string& name, ETCS::RID hall, const std::string& id)
    {
        Absent();
        if (!LockstepBase<KartBattle>::Host(name)) return false;
        { std::lock_guard<std::mutex> g(m_mu); m_note.clear(); }
        Attend(hall, name, id);
        ETCS_LOG("KartBattle", "hosting a battle as " << name);
        Announce();
        return true;
    }
    // Join: this battle mirrors the host's from its first line, and its seat
    // asks for a kart.
    bool Join(const std::string& name, const std::string& owner)
    {
        Absent();
        if (!LockstepBase<KartBattle>::Join(name, owner)) return false;
        { std::lock_guard<std::mutex> g(m_mu); m_note.clear(); }
        ETCS_LOG("KartBattle", "joined " << owner << "'s battle as " << name);
        return true;
    }
    void Leave()
    {
        LockstepBase<KartBattle>::Leave();
        Absent();
        std::lock_guard<std::mutex> g(m_mu); m_note.clear();
    }
    // The beat: on the host, its hall line, when it changed (Present::Announce).
    void Tick() { Announce(); }

    // ── what the families ask of a battle ─────────────────────────────────

    // Trace: a line, as its author (applyLocked), what "as made" is, the hash;
    // a seat opens with `join` and its stream's end is its `leave`.
    bool ApplyConcrete(const std::string& author, const std::string& line) override { return applyLocked(author, line); }
    void RestartConcrete() override { restartLocked(); }
    std::string HashConcrete() override { return hashLocked(); }
    void EndedConcrete() override { noteLocked("the battle is over -- " + m_owner + " closed it"); }
    std::string OpeningConcrete() override { return "join"; }
    std::string LeavingConcrete() override { return "leave"; }
    void DiffersConcrete(uint64_t at, const std::string& mine, const std::string& theirs) override
    {
        ETCS_LOG("KartBattle", "this battle (" << mine << ") is not the host's (" << theirs << ") at tick " << at);
        noteLocked("the battle differs from the host's at tick " + std::to_string(at));
    }
    // Lockstep: one tick, at kHz; ticks kept in pairs; a hash every kSnapEvery.
    void StepConcrete() override { stepLocked(); }
    Rate RateConcrete() const override { return { kHz, kMaxCatchUp, kBatch, kSnapEvery }; }
    // Present: listed as `kart`, its drivers and karts in the hall; a seat's
    // stream opening and ending.
    std::string KindConcrete() const override { return "kart"; }
    std::string HallConcrete() override
    {
        std::lock_guard<std::mutex> g(m_mu);
        return std::to_string(driversLocked()) + " " + std::to_string(m_slots.size());
    }
    void ArrivedConcrete(const std::string& who) override { std::lock_guard<std::mutex> g(m_mu); noteLocked(labelLocked(who) + " is here"); }
    void DepartedConcrete(const std::string& who) override
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (who != m_owner) noteLocked(labelLocked(who) + " left");
    }

    // ── the host's lobby and round, as lines ─────────────────────────────

    // The next round's map (by name) and length in seconds -- in the lobby
    // only. Choosing a map made from a seed gives it a new seed; the same map
    // again keeps the one it has (Reroll gives a new one).
    bool Configure(const std::string& map, uint32_t secs)
    {
        std::lock_guard<std::mutex> g(m_mu);
        const int m = mapIndex(map);
        if (!m_authority || !m_ready || m < 0) return false;
        return configLocked(m, secs, m != m_map);
    }
    bool Reroll()
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (!m_authority || !m_ready || m_defs.empty() || !m_defs[m_map].generated) return false;
        return configLocked(m_map, m_round_secs, true);
    }
    bool StartRound()
    {
        std::lock_guard<std::mutex> g(m_mu);
        return m_authority && m_ready && roundLocked();
    }
    bool Abandon()
    {
        std::lock_guard<std::mutex> g(m_mu);
        return m_authority && takeLocked(m_me, "abandon");
    }
    bool Give(const std::string& who, const std::string& weapon, const std::string& where)
    {
        std::lock_guard<std::mutex> g(m_mu);
        // Named as people name them; the line names the seat.
        const int i = slotNamedLocked(who);
        if (!m_authority || i < 0) return false;
        return takeLocked(m_me, "give " + m_slots[i].driver + " " + weapon + (where.empty() ? "" : " " + where));
    }

    /*
     * A KEY from the window (GLFW codes). In a round: W/S (or up/down) the
     * pedal, A/D (or left/right) the wheel, Space the trigger -- a change of
     * any is a `drive` line -- and, for the host, Esc abandons the round. A
     * turns the kart to the picture's left and D to its right, which is the
     * wheel's -1 and +1 (the camera's right is up x forward: +x for a kart
     * heading +z); SetInvert swaps the two keys for this runtime alone. Tab
     * holds up the board and Q and E look round the kart (lookLocked), this
     * runtime's alone and no line of the trace. In the lobby the host's keys
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
        if (down && m_authority && m_ready && !m_defs.empty())
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
        proposeLocked(line);
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

    // "<tick> <lines> <mode> <me> <slot> <phase> <secs left> <map> <round secs> <layout>"
    // mode: host | guest | practice | idle; phase: lobby | playing | results;
    // layout: the seed of a map made from one, 0 for a hand-made map.
    std::string Status()
    {
        std::lock_guard<std::mutex> g(m_mu);
        const char* mode = m_me.empty() ? "idle" : !m_session ? "practice" : m_authority ? "host" : "guest";
        return std::to_string(m_tick) + " " + std::to_string(m_lines.size()) + " " + mode + " "
             + (m_me.empty() ? "-" : labelLocked(m_me)) + " " + std::to_string(slotOf(m_me)) + " " + phaseName() + " "
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
            out += std::to_string(i) + " " + labelLocked(s.driver) + " " + std::to_string(elimsOf(s)) + " " + std::to_string(deathsOf(s))
                 + " " + std::to_string(s.hp) + " " + KartArms::Name(s.weapon) + " " + std::to_string(s.ammo)
                 + " " + (s.dead_until < 0 ? "alive" : "out") + "\n";
        }
        return out;
    }
    // The maps, one name a line, in their order: none until the arena is made (Ready).
    std::string Maps()
    {
        std::lock_guard<std::mutex> g(m_mu);
        std::string out;
        if (m_ready) for (const KartArena::Map& m : m_defs) out += m.name + "\n";
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
    std::string Note()     { std::lock_guard<std::mutex> g(m_mu); return m_note; }
    // "<slot> <x> <z> <heading x> <heading z> <speed> <hp> <weapon> <ammo> <y> <air|ground> <wheel> <slide>",
    // or "-"; the wheel and the slide (the way across the heading) are + to
    // the picture's right.
    std::string Kart(const std::string& who)
    {
        std::lock_guard<std::mutex> g(m_mu);
        const int i = slotNamedLocked(who);
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
             + " " + fmt3(slide) + " " + KartArms::Name(s.spare) + " " + std::to_string(s.spare_ammo) + " " + std::to_string(s.slip)
             + " " + std::to_string(s.dagger);
    }
    // Tyres <name> -- per tyre, fr fl rr rl, "<heat> <rope> <shown> <darkest>": its heat (J), how
    // long its mark's rope is laid behind it, how many segments are drawn,
    // and the darkest (0..1). "-" for nobody.
    std::string Tyres(const std::string& who)
    {
        std::lock_guard<std::mutex> g(m_mu);
        const int i = slotNamedLocked(who);
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
    // ── Animated: the frame edge ────────────────────────────────────────

    bool AnimatingConcrete() override { return m_world != 0; }
    void AdvanceConcrete(double dt_ms) override
    {
        std::lock_guard<std::mutex> g(m_mu);
        // The host's clock is the battle's (Lockstep): on a guest, nothing.
        clockLocked(dt_ms);
        cameraLocked(dt_ms);
        barsLocked();
        cardLocked(dt_ms);
        boardLocked();
        fpsLocked(dt_ms);
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
        uint8_t     spare = KartArms::None, spare_ammo = 0;   // the second one carried (KartArms.h)
        int         cooldown = 0, boost = 0;
        int         slip = 0;                      // ticks of oil still on the tyres
        int         dagger = 0;                    // ticks of the smoke's dagger left
        int         dagger_fx = -1;                // its blade: drawn, not state
        Fixed       kx, kz;                        // a shockwave's throw the drive has not yet taken
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
        int fx = -1, ring = -1;                    // a slick's node, a shockwave's: drawn, not state
        int cloud[4] = { -1, -1, -1, -1 };         // a smoke's puffs: drawn, not state
    };
    struct BoardRow { ETCS::RID rid; std::string text; bool shown; };
    struct PadNodes { ETCS::RID plate = 0; ETCS::RID arrow[3] = { 0, 0, 0 }; };
    struct SpringNodes { ETCS::RID plate = 0; ETCS::RID arrow[4] = { 0, 0, 0, 0 }; };
    enum Phase { Lobby, Playing, Results };
    enum MenuItem { MenuMap, MenuLength, MenuStart, MenuItems };

    static constexpr size_t   kMaxKarts   = 8;
    static constexpr int      kHz         = 50;
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
    static constexpr float kLava[3]    = { 0.95f, 0.36f, 0.08f };
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
    // padPush() (u/s/s) whatever the kart is doing -- a nudge -- and
    // padGain() times its speed that way a second on top, so one taken at
    // speed gives a lot: 60 standing, 140 at 20 u/s -- until the kart is
    // going padTop() that way, the fastest anything goes. Its arrows run its
    // length in padBeat() ticks, one behind the other.
    static Fixed padPush()   { return F(60); }
    static Fixed padGain()   { return F(4); }
    static Fixed padTop()    { return F(40); }
    // A SPRING throws a kart up: as hard as a pad pushes one along in an
    // eighth of a second, standing (7.5 u/s, a hop of 1.4), and springGain()
    // of the kart's speed over the ground on top -- 15 at 15 u/s, which is
    // five and a half high and onto a storey; 18.5 at the top speed, eight
    // and a half high and over the arena's wall. Again when it comes down on
    // it. Its four arrows close on its middle.
    static Fixed springKick(Fixed speed) { return padPush() / F(8) + speed * springGain(); }
    static Fixed springGain(){ return Fixed::Half(); }
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
    static Fixed bound()     { return KartArena::Half() - F(1); }   // the arena's inside, less a kart's reach
    // OVER THE WALL. The walls stop a kart whose feet are below wallRide():
    // their top (KartArena::WallTop) less what of the kart is under its feet.
    // One higher goes over -- off a spring, a jump, a storey's edge -- rides
    // the wall's top while it is over it, and past it has no ground: at
    // lostAt() it is out, as one that comes down in lava is.
    static Fixed wallRide()  { return KartArena::WallTop() + kartR() - kartY(); }
    static Fixed wallOut()   { return KartArena::Half() + F(1) + kartR(); }   // past the wall's far side
    // The long arms reach across the arena, however big it is: the sniper
    // corner to corner at once, and a rocket or a round of the minigun lives
    // until it has flown as far (crossLife), so a shot from one corner can
    // still land in the other -- a long shot is a tactic, not a waste.
    static Fixed sniperReach() { return KartArena::Half() * F(283, 100); }
    static Fixed homingSpeed() { return F(22); }
    static Fixed homingSight() { return F(70); }      // how far a homing rocket sees a kart
    static Fixed oilReach()    { return F(5, 2); }
    static Fixed waveReach()   { return F(9); }
    static Fixed waveThrow()   { return F(18); }
    static Fixed smokeReach()  { return F(5); }
    static Fixed daggerReach() { return F(26, 10); }   // up against: a kart's length, middle to middle
    // A smoke bomb's cloud: thick five seconds, dissipating fifteen; its dagger, eight.
    static constexpr int kSmokeThick = 5 * kHz, kSmokeTicks = 20 * kHz, kSmokeSteps = 10, kDaggerTicks = 8 * kHz;
    static constexpr int kOilTicks = 12 * kHz, kSlipTicks = kHz, kWaveTicks = 16, kWispTicks = 36, kMineTicks = 20 * kHz;
    static int crossLife(Fixed speed) { return static_cast<int>((sniperReach() / speed * F(kHz)).ToInt()) + 1; }
    static Fixed voidAt()    { return F(-40); }        // where a fall outside ends, between rounds
    static Fixed lostAt()    { return F(-6); }

    // ── the trace ───────────────────────────────────────────────────────

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
    // A seed: rolled here, on the host, and traced -- every runtime plays it.
    uint64_t rollLocked()
    {
        std::random_device rd;
        return ((static_cast<uint64_t>(rd()) << 32) ^ rd() ^ m_tick) | 1;
    }

    // One line, as its author: what every runtime does with the trace (a
    // `tick` is Lockstep's, before it gets here).
    bool applyLocked(const std::string& author, const std::string& line)
    {
        std::istringstream in(line);
        std::string verb;
        in >> verb;
        const bool host = author == m_owner;
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
                    noteLocked(labelLocked(author) + " drives kart " + std::to_string(i + 1));
                    if (author == m_me && !m_authority && (m_sent_t || m_sent_s || m_sent_f))
                        proposeLocked(driveLine(m_sent_t, m_sent_s, m_sent_f));
                    return true;
                }
            return false;   // a full arena: watching
        }
        if (verb == "leave")
        {
            const int i = slotOf(author);
            if (i < 0) return false;
            pitLocked(static_cast<size_t>(i));
            return true;
        }
        if (!host) return false;   // the rest are the host's
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
            noteLocked(labelLocked(author) + " abandoned the round");
            return true;
        }
        if (verb == "give")
        {
            // In hand outright, whatever was there -- or, said so, as the
            // spare, or as a power-up hands one over (armLocked).
            std::string who, w, how;
            in >> who >> w >> how;
            const int i = slotNamedLocked(who);
            const uint8_t weapon = KartArms::ByName(w);
            if (i < 0 || weapon == KartArms::None || (!how.empty() && how != "spare" && how != "pickup")) return false;
            if (how == "pickup") armLocked(m_slots[i], weapon);
            else if (how == "spare") { m_slots[i].spare = weapon; m_slots[i].spare_ammo = KartArms::Ammo(weapon); }
            else { m_slots[i].weapon = weapon; m_slots[i].ammo = KartArms::Ammo(weapon); m_slots[i].cooldown = 0; }
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
        for (Shot& s : m_shots) dropShotLocked(s);
        m_shots.clear();
        clearWispsLocked();
        for (Burst& b : m_bursts) endBurstLocked(b);
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
        for (Shot& s : m_shots) dropShotLocked(s);
        m_shots.clear();
        clearWispsLocked();
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
        s.hp = kFullHp; s.weapon = s.spare = KartArms::None; s.ammo = s.spare_ammo = 0; s.cooldown = s.boost = s.slip = s.dagger = 0;
        s.kx = s.kz = Fixed::Zero();
        freeFx(s.dagger_fx); s.dagger_fx = -1;
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
        s.hp = kFullHp; s.weapon = s.spare = KartArms::None; s.ammo = s.spare_ammo = 0; s.cooldown = s.boost = s.slip = s.dagger = 0;
        s.kx = s.kz = Fixed::Zero();
        freeFx(s.dagger_fx); s.dagger_fx = -1;
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
        for (Shot& s : m_shots) dropShotLocked(s);
        m_shots.clear();
        clearWispsLocked();
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
        for (; n < m_pickup_nodes.size(); ++n) parkNode(m_pickup_nodes[n]);
    }
    // The arena's floor round a map's pits: every piece each pit cuts into
    // leaves what is left of it, in up to four.
    struct Rect { Fixed x0, z0, x1, z1; };
    static std::vector<Rect> floorPieces(const KartArena::Map& m)
    {
        const Fixed half = Fixed::Half();
        const Fixed edge = KartArena::Half() + F(4);   // the floor runs on a little past the walls
        std::vector<Rect> floor{ { -edge, -edge, edge, edge } };
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
            slab(p.x, -p.depth - F(1, 4), p.z, p.w, F(1, 2), p.d, p.lava ? kLava : kGround[KartArena::InPit]);
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
                slab((r.x0 + r.x1) * half, F(-1, 4), (r.z0 + r.z1) * half, r.x1 - r.x0, F(1, 2), r.z1 - r.z0, kGround[KartArena::Floor]);
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
        // The pads: each plate on its ground -- tilted to a bank's slope --
        // under any tyre's mark; the arrows are put on it every tick
        // (arrowsLocked).
        for (size_t n = 0; n < m_pad_nodes.size(); ++n)
        {
            const PadNodes& pn = m_pad_nodes[n];
            if (n < m_live.pads.size())
            {
                const KartArena::Pad& p = m_live.pads[n];
                poseNode(pn.plate, p.x, p.y + F(3, 1000), p.z, p.yaw, Fixed::Zero(), Fixed::One(), p.gx, p.gz);
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
        if (!w) return;
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
            for (Slot& s : m_slots) if (alive(s)) groundLocked(s);
            if (m_phase != Lobby)
            {
                for (size_t i = 0; i < m_slots.size(); ++i) if (alive(m_slots[i])) touchLocked(i);
                for (size_t i = 0; i < m_slots.size(); ++i) if (alive(m_slots[i])) daggerLocked(i);
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
            burstsLocked();
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
            noteLocked(rank.empty() ? "the round is over" : clear ? "the round is over -- " + labelLocked(m_slots[rank[0]].driver) + " wins"
                                                                 : std::string("the round is over -- a draw"));
        }
        else if (m_phase == Results && static_cast<int64_t>(m_tick) >= m_end) lobbyLocked();
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
        // A shockwave's throw, taken on the ground like a pad's push: across
        // the heading it is a slide for the tyres. In the air it passes under.
        if (!s.air) { vx += s.kx; vz += s.kz; }
        s.kx = s.kz = Fixed::Zero();
        // A pad under it pushes the way the pad points, whichever way the
        // kart does: along its heading that is speed, across it a slide for
        // the tyres. The work is the Impulse below, with the pedal's.
        if (!s.air)
            if (const KartArena::Pad* pad = KartArena::PadAt(m_live, r.x, r.z))
                if ((groundFor(s, r, r.x, r.z) - KartArena::Solid(m_live, r.x, r.z)).Abs() < F(5, 100))   // on its ground, not on a deck over it
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
        if (s.slip > 0) --s.slip;
        if (s.air) return;
        Fixed lx = vx - hx * speed, lz = vz - hz * speed;
        const Fixed rolled = speed;
        const bool braking = (thr > 0 && speed.raw < 0) || (thr < 0 && speed.raw > 0);
        const Fixed top = s.boost > 0 ? boostSpeed() : topSpeed();
        Fixed push = pushAt(speed, top);
        if (s.boost > 0 && push < boostPush()) push = boostPush();
        if (s.slip > 0) push = push / F(3);   // oil: the tyres spin
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
        Fixed most = (braking ? brakeGrip() : grip()) * step;
        if (s.slip > 0) most = most / F(6);   // ...and hold a sixth of what they did
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
     * back where the step found it, stopped. The arena's walls bound it all
     * -- for a kart below their top; one over it has left (wallRide).
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
        // Past the walls' inside: over them (high enough, or out already) or stopped by them.
        const Fixed far = Fixed::Max(r.x.Abs(), r.z.Abs());
        const bool over = far > bound()
                       && (r.y - kartY() >= wallRide() - climb() || Fixed::Max(s.px.Abs(), s.pz.Abs()) > bound() + F(1, 100));
        if (over)
        {
            // On the wall's top -- or a deck laid to it, if that is higher -- or past it all.
            const bool on_wall = far < wallOut() && r.y - kartY() >= wallRide() - climb();
            const bool deck = on_wall && far <= KartArena::Half() && g > wallRide() + kartY();
            if (!deck) { g = (on_wall ? wallRide() : voidAt()) + kartY(); gx = gz = Fixed::Zero(); }
        }
        bool wall = !over && g - r.y > climb();
        if (!wall && !over && sp.IsPositive() && !s.air)
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
                if ((g - kartY() - up->y).Abs() < F(5, 100)) { s.air = true; s.vy = springKick(sp); }
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
        const bool cx = !over && x.Abs() > bound(), cz = !over && z.Abs() > bound();
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
        // Out: down on a lava pit's floor, or far enough down outside the walls.
        const KartArena::Pit* pit = s.air || over ? nullptr : KartArena::PitAt(m_live, x, z);
        const bool molten = pit && pit->lava && (y - kartY() + pit->depth).Abs() < F(5, 100);
        if (molten || (over && y - kartY() < lostAt()))
            damageLocked(static_cast<size_t>(&s - m_slots.data()), -1, kFullHp, molten ? "the lava" : "the drop");
    }
    // Up turned onto the normal of ground that slopes (gx, gz): the half-way
    // spinor (w, x, 0, z).
    static void tiltTo(Fixed gx, Fixed gz, Fixed& tw, Fixed& tx, Fixed& tz)
    {
        const Fixed nl = Fixed::Length(-gx, Fixed::One(), -gz);
        const Fixed nx = -gx / nl, ny = Fixed::One() / nl, nz = -gz / nl;
        tw = Fixed::One() + ny; tx = nz; tz = -nx;
        const Fixed tl = Fixed::Length(tw, tx, tz);
        tw = tw / tl; tx = tx / tl; tz = tz / tl;
    }
    // The body's turn: level to the heading, then tilted to the slope it
    // stands on (or last stood on, in the air).
    static void poseLocked(const Slot& s, OrderVector& r)
    {
        Fixed yw, yy;   // the heading's turn about +y: +z onto (hx, hz)
        halfTurn(s.hx, s.hz, yw, yy);
        Fixed tw, tx, tz;
        tiltTo(s.sgx, s.sgz, tw, tx, tz);
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
            case Rocket:  shootLocked(RocketShot, i, fx, fy, fz, hx * F(30), Fixed::Zero(), hz * F(30), crossLife(F(30))); break;
            case Mine:
            {
                const Fixed mx = x - hx * F(2), mz = z - hz * F(2);
                Shot& m = shootLocked(MineShot, i, mx, groundAt(mx, mz, y - kartY() + climb()) + F(1, 10), mz, Fixed::Zero(), Fixed::Zero(), Fixed::Zero(), kMineTicks);
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
            case Minigun: shootLocked(Bullet, i, fx, fy, fz, hx * F(45), Fixed::Zero(), hz * F(45), crossLife(F(45))); s.cooldown = 5; break;
            case Fuse:    shootLocked(FuseShot, i, x, y + F(12, 10), z, Fixed::Zero(), Fixed::Zero(), Fixed::Zero(), 125); break;
            case Sniper:  sniperLocked(i, x, fy, z, hx, hz); break;
            case Homing:  shootLocked(HomingShot, i, fx, fy, fz, hx * homingSpeed(), Fixed::Zero(), hz * homingSpeed(), crossLife(homingSpeed())); break;
            case Oil:
            {
                const Fixed ox = x - hx * F(3), oz = z - hz * F(3);
                Shot& o = shootLocked(OilShot, i, ox, groundAt(ox, oz, y - kartY() + climb()) + F(4, 100), oz, Fixed::Zero(), Fixed::Zero(), Fixed::Zero(), kOilTicks);
                o.fx = takeFx();
                if (o.fx >= 0) { placeNode(m_fx_nodes[o.fx], o.x, o.y, o.z); dressLocked(m_fx_nodes[o.fx], slickSize(1.0f), "0.05, 0.05, 0.08, 1.0", 0.0f, 0.0f); }
                break;
            }
            case Smoke:
            {
                Shot& c = shootLocked(SmokeShot, i, x, groundAt(x, z, y - kartY() + climb()), z, Fixed::Zero(), Fixed::Zero(), Fixed::Zero(), kSmokeTicks);
                s.dagger = kDaggerTicks;
                for (int k = 0; k < 4; ++k)
                {
                    c.cloud[k] = takeFx();
                    if (c.cloud[k] < 0) continue;
                    Fixed sn, cs;
                    (Fixed::TwoPi() * F(k, 4) + F(1, 3)).SinCos(sn, cs);
                    const ETCS::RID n = m_fx_nodes[static_cast<size_t>(c.cloud[k])];
                    placeNode(n, c.x + sn * F(2), c.y + F(1) + F(k % 2, 2), c.z + cs * F(2));
                    dressLocked(n, "3.0, 2.0, 3.0", "0.55, 0.55, 0.58, 1.0", 0.0f, 0.5f);
                }
                break;
            }
            case Shockwave:
            {
                Shot& w = shootLocked(WaveShot, i, x, y, z, Fixed::Zero(), Fixed::Zero(), Fixed::Zero(), kWaveTicks);
                w.ring = takeRing();
                if (w.ring >= 0) placeNode(m_ring_nodes[w.ring], x, y, z);
                break;
            }
            default: break;
        }
        // Spent: the spare comes to hand.
        if (s.ammo > 0 && --s.ammo == 0) { s.weapon = s.spare; s.ammo = s.spare_ammo; s.spare = None; s.spare_ammo = 0; }
    }

    Shot& shootLocked(uint8_t kind, size_t owner, Fixed x, Fixed y, Fixed z, Fixed vx, Fixed vy, Fixed vz, int life)
    {
        Shot s; s.kind = kind; s.owner = static_cast<int>(owner);
        s.x = x; s.y = y; s.z = z; s.vx = vx; s.vy = vy; s.vz = vz; s.life = life;
        // A slick and a shockwave are drawn by their own effects (fx, ring).
        s.node = (kind == KartArms::OilShot || kind == KartArms::WaveShot || kind == KartArms::SmokeShot) ? -1 : takeNode();
        if (s.node >= 0)
        {
            using namespace KartArms;
            const ETCS::RID n = m_shot_nodes[s.node];
            const char* size = kind == HomingShot ? "0.3, 0.3, 0.8" : kind == RocketShot ? "0.35, 0.35, 0.9" : kind == Pellet ? "0.22, 0.22, 0.22"
                             : kind == Bullet ? "0.16, 0.16, 0.4" : kind == BombShot ? "0.55, 0.55, 0.55"
                             : kind == MineShot ? "0.9, 0.18, 0.9" : kind == FuseShot ? "0.5, 0.5, 0.5" : "0.1, 0.1, 1.0";
            const char* colour = kind == HomingShot ? "0.75, 0.3, 0.95, 1.0" : kind == RocketShot ? "0.95, 0.35, 0.10, 1.0" : kind == Pellet ? "1.0, 0.85, 0.3, 1.0"
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
        int hit = -1; Fixed best = sniperReach() + F(1), miss;
        for (size_t j = 0; j < m_slots.size(); ++j)
        {
            if (j == i || !alive(m_slots[j])) continue;
            Fixed jx, jy, jz;
            if (!where(j, jx, jy, jz)) continue;
            const Fixed along = (jx - x) * hx + (jz - z) * hz;
            if (along.raw <= 0 || along > sniperReach()) continue;
            const Fixed px = jx - (x + hx * along), pz = jz - (z + hz * along);
            const Fixed off = Fixed::Length(px, jy - y, pz);
            if (off > kartR() || !(along < best)) continue;
            best = along; hit = static_cast<int>(j); miss = off;
        }
        Fixed reach = hit >= 0 ? best : sniperReach();
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
                case RocketShot: case Pellet: case Bullet: case BombShot: case HomingShot:
                {
                    if (s.kind == BombShot) s.vy -= gravity() * dt();
                    if (s.kind == HomingShot) { homeLocked(s); if (s.life % 4 == 0) wispLocked(s.x, s.y, s.z); }
                    const Fixed was_x = s.x, was_y = s.y, was_z = s.z;
                    s.x += s.vx * dt(); s.y += s.vy * dt(); s.z += s.vz * dt();
                    const int hit = kartAt(s.x, s.y, s.z, s.owner, F(25, 100));
                    const bool wall = solidAt(s.x, s.y, s.z, F(2, 10));
                    const bool ground = s.y < groundAt(s.x, s.z, s.y) + (s.kind == BombShot ? F(3, 10) : F(5, 100));
                    if (hit >= 0 || wall || ground || s.life <= 0)
                    {
                        gone = true;
                        // Met a wall or the ground: it bursts on the side it came from, not inside.
                        if (hit < 0 && (wall || ground)) { s.x = was_x; s.y = was_y; s.z = was_z; }
                        if (s.kind == RocketShot)    burstLocked(s.x, s.y, s.z, F(4), 40, s.owner);
                        else if (s.kind == BombShot) burstLocked(s.x, s.y, s.z, F(45, 10), 40, s.owner);
                        else if (s.kind == HomingShot) burstLocked(s.x, s.y, s.z, F(3), 30, s.owner);
                        else if (hit >= 0)           damageLocked(static_cast<size_t>(hit), s.owner, s.kind == Pellet ? 10 : 4);
                    }
                    break;
                }
                case MineShot:
                    if (s.armed > 0) --s.armed;
                    else if (kartAt(s.x, s.y, s.z, s.owner, F(6, 10)) >= 0) { gone = true; burstLocked(s.x, s.y, s.z, F(25, 10), 30, s.owner); }
                    // Left alone it does not lie there for good: its last two
                    // seconds it blinks, and then it bursts by itself.
                    if (!gone && s.life <= 0) { gone = true; burstLocked(s.x, s.y, s.z, F(25, 10), 30, s.owner); }
                    else if (!gone && s.life < 2 * kHz && s.life % 10 == 0 && s.node >= 0)
                        verb(m_shot_nodes[s.node], "SetColor", (s.life / 10) % 2 ? "1.0, 0.9, 0.2, 1.0" : "0.55, 0.08, 0.08, 1.0");
                    break;
                case OilShot:
                    // On the tyres of whoever is on it, but the kart that dropped it.
                    for (size_t j = 0; j < m_slots.size(); ++j)
                    {
                        Fixed jx, jy, jz;
                        if (static_cast<int>(j) == s.owner || !alive(m_slots[j]) || m_slots[j].air || !where(j, jx, jy, jz)) continue;
                        if (Fixed::Length(jx - s.x, Fixed::Zero(), jz - s.z) <= oilReach() && (jy - s.y).Abs() < F(1)) m_slots[j].slip = kSlipTicks;
                    }
                    if (s.life <= 0) gone = true;
                    else if ((s.life == 20 || s.life == 10) && s.fx >= 0)   // drying up
                        dressLocked(m_fx_nodes[s.fx], slickSize(s.life / 30.0f), "0.05, 0.05, 0.08, 1.0", 0.0f, 0.0f);
                    break;
                case SmokeShot:
                {
                    // Thick for its first five seconds, then dissipating over
                    // fifteen: swelling as it thins, in steps (each a verb).
                    const int age = kSmokeTicks - s.life, thin = kSmokeTicks - kSmokeThick;
                    const int step = age < kSmokeThick ? 0 : 1 + (age - kSmokeThick) * kSmokeSteps / thin;
                    if (age == kHz / 2 || (age >= kSmokeThick && (age - kSmokeThick) % (thin / kSmokeSteps) == 0))
                    {
                        const float f = static_cast<float>(step) / kSmokeSteps;
                        const std::string w = fmt1(4.2f + 2.6f * f), h = fmt1(2.8f + 1.6f * f);
                        for (int c : s.cloud)
                            if (c >= 0) dressLocked(m_fx_nodes[static_cast<size_t>(c)], w + ", " + h + ", " + w, "0.55, 0.55, 0.58, 1.0", 0.0f, 0.55f * (1.0f - f) + 0.02f);
                    }
                    if (s.life <= 0) gone = true;
                    break;
                }
                case WaveShot:
                {
                    // The ring's front, out from where it was set off: every
                    // kart it passes, once (`armed` keeps who), hurt and thrown.
                    const Fixed front = waveReach() * F(kWaveTicks - s.life, kWaveTicks);
                    for (size_t j = 0; j < m_slots.size(); ++j)
                    {
                        Fixed jx, jy, jz;
                        if (static_cast<int>(j) == s.owner || (s.armed >> j & 1) || !alive(m_slots[j]) || !where(j, jx, jy, jz)) continue;
                        const Fixed dx = jx - s.x, dz = jz - s.z, d = Fixed::Length(dx, Fixed::Zero(), dz);
                        if (d > front + kartR() || (jy - s.y).Abs() > F(5, 2)) continue;
                        s.armed |= 1 << j;
                        if (d.IsPositive()) { m_slots[j].kx += dx / d * waveThrow(); m_slots[j].kz += dz / d * waveThrow(); }
                        damageLocked(j, s.owner, 15);
                    }
                    if (s.ring >= 0)
                    {
                        const float across = 2.0f * (front.ToFloat() + 0.6f), t = static_cast<float>(kWaveTicks - s.life) / kWaveTicks;
                        dressLocked(m_ring_nodes[s.ring], fmt1(across) + ", 0.6, " + fmt1(across), "0.45, 0.85, 1.0, 1.0", t < 0.5f ? 1.4f : t < 0.75f ? 0.9f : 0.45f, 0.0f);
                    }
                    if (s.life <= 0) gone = true;
                    break;
                }
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
            if (gone) dropShotLocked(s);
            else { nodeAtLocked(s); keep.push_back(s); }
        }
        m_shots.swap(keep);
        wispsLocked();
    }
    void dropShotLocked(Shot& s)
    {
        freeNode(s.node); freeFx(s.fx); freeRing(s.ring); s.node = s.fx = s.ring = -1;
        for (int& c : s.cloud) { freeFx(c); c = -1; }
    }
    // Is (x, z) in a cloud of smoke?
    bool smokedAt(Fixed x, Fixed z) const
    {
        for (const Shot& s : m_shots)
            if (s.kind == KartArms::SmokeShot && s.life > kSmokeTicks - kSmokeThick - (kSmokeTicks - kSmokeThick) * 2 / 3   // while it is thick enough to hide in
                && Fixed::Length(x - s.x, Fixed::Zero(), z - s.z) < smokeReach()) return true;
        return false;
    }
    /*
     * THE DAGGER: every fifth of a second while it lasts, 8 to each kart
     * whose middle is within a kart's length of this one's -- up against
     * it, side by side, nose to tail. Its blade is drawn out of the nose.
     */
    void daggerLocked(size_t i)
    {
        Slot& s = m_slots[i];
        if (s.dagger <= 0) return;
        --s.dagger;
        Fixed x, y, z;
        if (!where(i, x, y, z)) return;
        if (s.dagger % 10 == 0)
            for (size_t j = 0; j < m_slots.size(); ++j)
            {
                Fixed jx, jy, jz;
                if (j == i || !alive(m_slots[j]) || !where(j, jx, jy, jz)) continue;
                if (Fixed::Length(jx - x, jy - y, jz - z) < daggerReach()) damageLocked(j, static_cast<int>(i), 8, "a dagger");
            }
        if (s.dagger_fx < 0 && s.dagger > 0) s.dagger_fx = takeFx();
        if (s.dagger_fx >= 0)
        {
            if (s.dagger <= 0) { freeFx(s.dagger_fx); s.dagger_fx = -1; return; }
            const ETCS::RID n = m_fx_nodes[static_cast<size_t>(s.dagger_fx)];
            dressLocked(n, "0.12, 0.12, 1.4", "0.95, 0.95, 1.0, 1.0", 0.9f, 0.0f);
            if (Causal_* c = causal(n))
            {
                std::lock_guard<std::recursive_mutex> lk(c->TreeMutex());
                OrderVector& r = c->RowsUnder();
                r.PlaceAt(x + s.hx * F(13, 10), y, z + s.hz * F(13, 10));
                Fixed w, yy;
                halfTurn(s.hx, s.hz, w, yy);
                r.qw = w; r.qx = r.qz = Fixed::Zero(); r.qy = yy;
            }
        }
    }

    // A homing rocket's turn: toward the nearest kart ahead of it, an eighth
    // of the way round a tick, and toward its height -- at its own speed.
    void homeLocked(Shot& s)
    {
        const Fixed speed = Fixed::Length(s.vx, Fixed::Zero(), s.vz);
        if (!speed.IsPositive()) return;
        const Fixed ux = s.vx / speed, uz = s.vz / speed;
        int at = -1; Fixed best = homingSight(), bx, by, bz;
        for (size_t j = 0; j < m_slots.size(); ++j)
        {
            Fixed jx, jy, jz;
            if (static_cast<int>(j) == s.owner || !alive(m_slots[j]) || !where(j, jx, jy, jz) || smokedAt(jx, jz)) continue;   // smoke hides it
            const Fixed dx = jx - s.x, dz = jz - s.z, d = Fixed::Length(dx, Fixed::Zero(), dz);
            if (!(d < best) || !(dx * ux + dz * uz).IsPositive()) continue;
            best = d; at = static_cast<int>(j); bx = dx; by = jy - s.y; bz = dz;
        }
        if (at < 0 || !best.IsPositive()) { s.vy = Fixed::Zero(); return; }
        Fixed nx = ux + (bx / best - ux) / F(8), nz = uz + (bz / best - uz) / F(8);
        const Fixed l = Fixed::Length(nx, Fixed::Zero(), nz);
        if (!l.IsPositive()) return;
        s.vx = nx / l * homingSpeed(); s.vz = nz / l * homingSpeed();
        s.vy = Fixed::Max(F(-6), Fixed::Min(F(6), by * F(2)));
    }

    /*
     * THE WEAPONS' EFFECTS, drawn only and never the trace's: a homing
     * rocket's WISPS of smoke behind it, an oil SLICK flat on the ground, a
     * shockwave's RING. Each a pooled node (kart_fx.etcs: a ball; kart_ring
     * .etcs: the ring this battle baked), DRESSED when it changes and not
     * otherwise -- a verb on a node is a line of the scene's record, so an
     * effect says a handful in its life, not one a tick: a wisp swells in
     * three steps, a slick dries in three, a ring is sized a tick (it is one
     * node, where beads round it would be a dozen).
     */
    struct Dress { std::string size, colour; float glow = 0.0f, smoke = 0.0f; bool up = false; ETCS::RID mesh = 0; };
    void dressLocked(ETCS::RID n, const std::string& size, const std::string& colour, float glow, float smoke)
    {
        Dress& d = m_dress[n];
        if (d.size != size)     { verb(n, "Create", size); d.size = size; }
        if (d.colour != colour) { verb(n, "SetColor", colour); d.colour = colour; }
        if (d.glow != glow)     { verb(n, "SetGlow", fmt3(glow)); d.glow = glow; }
        if (d.smoke != smoke)   { verb(n, "SetSmoke", fmt3(smoke)); d.smoke = smoke; }
        if (!d.up)              { verb(n, "SetVisible", "1"); d.up = true; }
    }
    void undressLocked(ETCS::RID n)
    {
        Dress& d = m_dress[n];
        if (!d.up) return;
        parkNode(n);          // away, hidden, and small enough for its berth
        d.up = false; d.size.clear();
    }
    static int takeOf(std::vector<bool>& busy)
    {
        for (size_t n = 0; n < busy.size(); ++n) if (!busy[n]) { busy[n] = true; return static_cast<int>(n); }
        return -1;
    }
    int  takeFx()        { return takeOf(m_fx_busy); }
    int  takeRing()      { return takeOf(m_ring_busy); }
    void freeFx(int n)   { if (n >= 0 && n < static_cast<int>(m_fx_busy.size()))   { m_fx_busy[n] = false;   undressLocked(m_fx_nodes[n]); } }
    void freeRing(int n) { if (n >= 0 && n < static_cast<int>(m_ring_busy.size())) { m_ring_busy[n] = false; undressLocked(m_ring_nodes[n]); } }
    static std::string slickSize(float f) { const std::string d = fmt1(2.0f * 2.5f * f); return d + ", 0.08, " + d; }
    struct Wisp { int node; int64_t born; };
    void wispLocked(Fixed x, Fixed y, Fixed z)
    {
        // A quarter of the pool is kept back for the slicks.
        size_t idle = 0;
        for (bool b : m_fx_busy) if (!b) ++idle;
        if (idle * 4 <= m_fx_busy.size()) return;
        const int n = takeFx();
        if (n < 0) return;
        placeNode(m_fx_nodes[n], x, y, z);
        dressLocked(m_fx_nodes[n], "0.5, 0.5, 0.5", "0.62, 0.6, 0.58, 1.0", 0.0f, 0.6f);
        m_wisps.push_back({ n, static_cast<int64_t>(m_tick) });
    }
    void wispsLocked()
    {
        std::vector<Wisp> keep;
        for (const Wisp& w : m_wisps)
        {
            const int64_t age = static_cast<int64_t>(m_tick) - w.born;
            if (age >= kWispTicks) { freeFx(w.node); continue; }
            if (age == kWispTicks / 3)          dressLocked(m_fx_nodes[w.node], "0.9, 0.9, 0.9", "0.62, 0.6, 0.58, 1.0", 0.0f, 0.4f);
            else if (age == 2 * kWispTicks / 3) dressLocked(m_fx_nodes[w.node], "1.3, 1.3, 1.3", "0.62, 0.6, 0.58, 1.0", 0.0f, 0.2f);
            keep.push_back(w);
        }
        m_wisps.swap(keep);
    }
    void clearWispsLocked() { for (const Wisp& w : m_wisps) freeFx(w.node); m_wisps.clear(); }
    // The ring a shockwave is drawn as, baked into the mesh the script made
    // for it: round the unit box's middle, level, its tube the box's height.
    void bakeRingLocked(ETCS::RID mesh)
    {
        if (!mesh || !m_baked.insert(mesh).second) return;
        const int around = 24, tube = 6;
        const float R = 0.4f, r = 0.1f;
        verb(mesh, "Clear", "");
        for (int i = 0; i < around; ++i)
            for (int j = 0; j < tube; ++j)
            {
                const float a = 2.0f * kPi * i / around, b = 2.0f * kPi * j / tube;
                const float ca = std::cos(a), sa = std::sin(a), cb = std::cos(b), sb = std::sin(b);
                verb(mesh, "AddVertex", fmt3((R + r * cb) * ca) + ", " + fmt3(0.5f * sb) + ", " + fmt3((R + r * cb) * sa) + ", "
                                      + fmt3(cb * ca) + ", " + fmt3(sb) + ", " + fmt3(cb * sa));
            }
        auto at = [&](int i, int j) { return std::to_string(((i % around) * tube) + (j % tube)); };
        for (int i = 0; i < around; ++i)
            for (int j = 0; j < tube; ++j)
            {
                verb(mesh, "AddTriangle", at(i, j) + ", " + at(i, j + 1) + ", " + at(i + 1, j + 1));
                verb(mesh, "AddTriangle", at(i, j) + ", " + at(i + 1, j + 1) + ", " + at(i + 1, j));
            }
    }

    /*
     * A BURST IS A TREE OF FIRE, and the tree is what hurts. From the burst's
     * point a dozen branches go out -- round it, a little up to well up --
     * each in steps of a fifth of the burst's reach, turning a little at
     * every step, forking now and then (less the further out), and carrying
     * an energy that starts at the reach and is spent by what it goes
     * through: air costs a unit of energy a unit of way; the ground and a
     * deck's slab cost 8, a wall or a block 12. Before a step into anything
     * denser than air a branch tries to go round it -- turned to either side,
     * up, down -- and only goes in when nothing round it is open, so fire
     * pours round a block and over a ramp's lip rather than through them.
     *
     * A kart is hurt by the most energy any branch has where it passes
     * within its reach (or the whole of it, at the burst's own point):
     * `max` times that over the reach. In the open that is the old straight
     * falloff; behind a block, the long way round, it is less, or nothing.
     * Every number is the rows' (Fixed), and the tree's dice are made from
     * the tick, the point and the bursts so far -- the same tree, the same
     * damage, on every runtime.
     *
     * Drawn as it grows: the sparks a front reaches, the front out along the
     * branches to the reach in kGrowTicks, then all of it fading over
     * kFadeTicks -- each spark a glowing PUFF (Scene3D::SetGlow) about its
     * step of the branch: nearly round, a little longer than it is wide and
     * overlapping the next, so a branch reads as a string of small bursts
     * rather than a line -- fire is too dissipative to draw thin lines. White-
     * hot and fuller where its branch is strong, red and leaner where it is
     * spent, round a core that swells and fades with it.
     */
    struct Spark { Fixed x0, y0, z0, x1, y1, z1, e0, e1, d0; int shown = 0; };
    /*
     * AND THEN THE SMOKE, from the fire: small dark puffs where branches
     * burned, and big pale clouds in the gaps between the first branches,
     * each swelling as it rises and thinning to nothing (Scene3D::SetSmoke)
     * -- long after the fire is out. Drawn only, from the tree; not the
     * trace's, as the sparks are not.
     */
    struct Puff
    {
        float x = 0, y = 0, z = 0, r0 = 0, r1 = 0, rise = 0, dense = 0, grey = 0;
        int   delay = 0, life = 0;
        int   shown = 0;                          // 0 not yet, 1 up, 2 gone
        int   said = -1;                          // the density's step last said
    };
    struct Burst
    {
        bool               live = false;
        Fixed              x, y, z, reach;
        int64_t            born = 0;
        int                fade = -1;             // the glow's step last said
        std::vector<Spark> sparks;
        std::vector<float> az;                    // the first branches' bearings, for the gaps between
        std::vector<Puff>  puffs;
        int64_t            done = 0;              // the age the last puff is gone at
    };
    static constexpr int kBursts = 3, kSparks = 64, kGrowTicks = 8, kFadeTicks = 30;
    static constexpr int kBranchPuffs = 8, kCloudPuffs = 6, kPuffs = kBranchPuffs + kCloudPuffs;

    // How hard the medium at a point is to go through, a unit of way: air 1.
    Fixed densityAt(Fixed x, Fixed y, Fixed z)
    {
        if (solidAt(x, y, z, F(1, 10))) return F(12);                 // a wall, a block
        if (KartArena::Solid(m_live, x, z) > y) return F(8);          // the ground
        for (const KartArena::Deck& d : m_live.decks)                 // a deck's slab
            if (!(y > d.h) && !(y < d.h - KartArena::DeckThick()) && KartArena::Over(x, z, d.x, d.z, d.w, d.d)) return F(8);
        return F(1);
    }
    static void unitize(Fixed& x, Fixed& y, Fixed& z)
    {
        const Fixed l = Fixed::Length(x, y, z);
        if (!l.IsPositive()) { x = Fixed::Zero(); y = Fixed::One(); z = Fixed::Zero(); return; }
        x = x / l; y = y / l; z = z / l;
    }
    void growLocked(Burst& b, uint64_t seed)
    {
        b.sparks.clear();
        struct Tip { Fixed x, y, z, dx, dy, dz, e, d; int depth; };
        std::deque<Tip> tips;
        uint64_t r = seed | 1;
        auto unit = [&r]() { return F(static_cast<int64_t>(KartArms::Next(r) % 2001) - 1000, 1000); };   // -1..1
        const Fixed full = b.reach, step = b.reach / F(5);
        const int first = 12;
        b.az.clear();
        for (int k = 0; k < first; ++k)
        {
            Fixed sn, cs;
            const Fixed bearing = Fixed::TwoPi() * (F(k) + unit() * F(3, 10)) / F(first);
            bearing.SinCos(sn, cs);
            b.az.push_back(bearing.ToFloat());
            Tip t{ b.x, b.y, b.z, sn, F(1, 10) + (unit() + Fixed::One()) * F(3, 10), cs, full, Fixed::Zero(), 0 };
            unitize(t.dx, t.dy, t.dz);
            tips.push_back(t);
        }
        // One step of every live branch in turn, so they grow together.
        while (!tips.empty() && static_cast<int>(b.sparks.size()) < kSparks)
        {
            Tip t = tips.front(); tips.pop_front();
            Fixed dx = t.dx + unit() * F(35, 100), dy = t.dy + unit() * F(25, 100), dz = t.dz + unit() * F(35, 100);
            unitize(dx, dy, dz);
            // The medium a step goes through: at its middle and its end, so a
            // thin wall is not stepped over.
            auto through = [&](Fixed ax, Fixed ay, Fixed az) {
                return Fixed::Max(densityAt(t.x + ax * step * Fixed::Half(), t.y + ay * step * Fixed::Half(), t.z + az * step * Fixed::Half()),
                                  densityAt(t.x + ax * step, t.y + ay * step, t.z + az * step));
            };
            Fixed rho = through(dx, dy, dz);
            if (rho > Fixed::One())
            {
                // Round it: to one side, the other, up, then the far turns and down.
                const int side = (KartArms::Next(r) & 1) ? 1 : -1;
                const Fixed turns[5][2] = { { F(8, 10) * F(side), Fixed::Zero() }, { F(-8, 10) * F(side), Fixed::Zero() },
                                            { Fixed::Zero(), F(1) }, { F(16, 10) * F(side), Fixed::Zero() }, { Fixed::Zero(), F(-1) } };
                for (const auto& tn : turns)
                {
                    Fixed sn, cs;
                    tn[0].SinCos(sn, cs);
                    Fixed ax = dx * cs + dz * sn, ay = dy + tn[1], az = dz * cs - dx * sn;
                    unitize(ax, ay, az);
                    if (through(ax, ay, az) == Fixed::One())
                    { dx = ax; dy = ay; dz = az; rho = Fixed::One(); break; }
                }
            }
            Fixed go = step, cost = rho * step;
            if (cost > t.e) { go = t.e / rho; cost = t.e; }
            Spark sp;
            sp.x0 = t.x; sp.y0 = t.y; sp.z0 = t.z;
            t.x += dx * go; t.y += dy * go; t.z += dz * go;
            sp.x1 = t.x; sp.y1 = t.y; sp.z1 = t.z;
            sp.e0 = t.e; sp.e1 = t.e - cost; sp.d0 = t.d;
            b.sparks.push_back(sp);
            t.e -= cost; t.d += go; t.dx = dx; t.dy = dy; t.dz = dz;
            if (!t.e.IsPositive()) continue;
            // A fork now and then: off the branch's way, with most of what it has.
            if (t.depth < 3 && t.e > full / F(4) && static_cast<int>(KartArms::Next(r) % 100) < 30 - t.depth * 8)
            {
                Tip c = t;
                c.depth = t.depth + 1;
                c.e = t.e * F(7, 10);
                c.dx = t.dx + unit() * F(9, 10); c.dy = t.dy + unit() * F(6, 10); c.dz = t.dz + unit() * F(9, 10);
                unitize(c.dx, c.dy, c.dz);
                tips.push_back(c);
            }
            tips.push_back(t);
        }
    }
    // The most energy a branch has within a kart's reach of (x, y, z).
    static Fixed reachedBy(const Burst& b, Fixed x, Fixed y, Fixed z, Fixed reach)
    {
        if (Fixed::Length(x - b.x, y - b.y, z - b.z) <= reach) return b.reach;
        Fixed best = Fixed::Zero();
        for (const Spark& sp : b.sparks)
        {
            const Fixed sx = sp.x1 - sp.x0, sy = sp.y1 - sp.y0, sz = sp.z1 - sp.z0;
            const Fixed ll = sx * sx + sy * sy + sz * sz;
            Fixed t = Fixed::Zero();
            if (ll.IsPositive()) t = Fixed::Min(Fixed::One(), Fixed::Max(Fixed::Zero(), ((x - sp.x0) * sx + (y - sp.y0) * sy + (z - sp.z0) * sz) / ll));
            if (Fixed::Length(x - (sp.x0 + sx * t), y - (sp.y0 + sy * t), z - (sp.z0 + sz * t)) > reach) continue;
            best = Fixed::Max(best, sp.e0 + (sp.e1 - sp.e0) * t);
        }
        return best;
    }
    // A burst: its tree, everyone it reaches but the one who set it off, and the fire.
    void burstLocked(Fixed x, Fixed y, Fixed z, Fixed radius, int max, int owner)
    {
        Burst& b = blastLocked(x, y, z, radius);
        for (size_t j = 0; j < m_slots.size(); ++j)
        {
            if (static_cast<int>(j) == owner || !alive(m_slots[j])) continue;
            Fixed jx, jy, jz;
            if (!where(j, jx, jy, jz)) continue;
            const Fixed e = reachedBy(b, jx, jy, jz, kartR());
            const int dmg = static_cast<int>((F(max) * e / b.reach).raw >> 32);
            if (dmg > 0) damageLocked(j, owner, dmg);
        }
    }
    // The tree and its fire, in the oldest burst's place if all are burning.
    Burst& blastLocked(Fixed x, Fixed y, Fixed z, Fixed radius)
    {
        int at = 0;
        for (int k = 0; k < kBursts; ++k)
        {
            if (!m_bursts[k].live) { at = k; break; }
            if (m_bursts[k].born < m_bursts[at].born) at = k;
        }
        Burst& b = m_bursts[at];
        endBurstLocked(b);
        b.live = true; b.x = x; b.y = y; b.z = z; b.reach = radius; b.born = static_cast<int64_t>(m_tick); b.fade = -1;
        uint64_t seed = 0x9e3779b97f4a7c15ull ^ (m_tick * 0x100000001b3ull) ^ ++m_burst_seq;
        seed ^= static_cast<uint64_t>(x.raw) * 31 + static_cast<uint64_t>(y.raw) * 131 + static_cast<uint64_t>(z.raw) * 1031;
        growLocked(b, seed);
        smokeFromLocked(b);
        return b;
    }
    // The smoke a burst's tree leaves: puffs along its branches, spread over
    // its sparks, and clouds in every second gap between its first branches,
    // a little up -- where the medium there is air.
    void smokeFromLocked(Burst& b)
    {
        b.puffs.clear();
        const float reach = b.reach.ToFloat(), step = reach / 5.0f;
        const size_t n = b.sparks.size();
        for (int i = 0; i < kBranchPuffs && n; ++i)
        {
            const Spark& sp = b.sparks[(static_cast<size_t>(i) * 2 + 1) * n / (kBranchPuffs * 2)];
            Puff p;
            p.x = sp.x1.ToFloat(); p.y = sp.y1.ToFloat(); p.z = sp.z1.ToFloat();
            p.r0 = step * 0.9f; p.r1 = step * 2.4f; p.rise = 1.2f; p.dense = 0.75f; p.grey = 0.14f;
            p.delay = kGrowTicks / 2 + i; p.life = 100 + 6 * i;
            b.puffs.push_back(p);
        }
        for (size_t j = 0; j + 1 < b.az.size() && static_cast<int>(b.puffs.size()) < kPuffs; j += 2)
        {
            const float a = (b.az[j] + b.az[j + 1]) * 0.5f;
            for (const float out : { 0.55f, 0.3f })
            {
                const float x = b.x.ToFloat() + std::sin(a) * reach * out, z = b.z.ToFloat() + std::cos(a) * reach * out;
                const float y = b.y.ToFloat() + reach * 0.25f;
                if (densityAt(Fixed::From(x), Fixed::From(y), Fixed::From(z)) != Fixed::One()) continue;
                Puff p;
                p.x = x; p.y = y; p.z = z;
                p.r0 = reach * 0.3f; p.r1 = reach * 0.75f; p.rise = 0.8f; p.dense = 0.55f; p.grey = 0.3f;
                p.delay = kGrowTicks; p.life = 150;
                b.puffs.push_back(p);
                break;
            }
        }
        b.done = kGrowTicks + kFadeTicks;
        for (const Puff& p : b.puffs) b.done = std::max<int64_t>(b.done, p.delay + p.life);
    }
    // The smoke a tick on: each puff up once its time comes, swelling as it
    // rises (fast, then slower), thickening for its first part and thinning
    // after; put away when it has thinned to nothing.
    void puffsLocked(int k, Burst& b, int64_t age)
    {
        for (size_t i = 0; i < b.puffs.size(); ++i)
        {
            Puff& p = b.puffs[i];
            const size_t at = static_cast<size_t>(k) * kPuffs + i;
            if (p.shown == 2 || at >= m_smoke_nodes.size()) continue;
            const ETCS::RID node = m_smoke_nodes[at];
            const int64_t pa = age - p.delay;
            if (pa < 0) continue;
            const float t = static_cast<float>(pa) / static_cast<float>(p.life);
            const float thick = t < 0.15f ? t / 0.15f : 1.0f - (t - 0.15f) / 0.85f;
            const int step = static_cast<int>(p.dense * thick * 20.0f + 0.5f);
            if (pa > p.life || (step <= 0 && pa > 0)) { parkNode(node); p.shown = 2; continue; }
            const float grow = 1.0f - (1.0f - t) * (1.0f - t);
            const float r = p.r0 + (p.r1 - p.r0) * grow;
            placeNode(node, Fixed::From(p.x), Fixed::From(p.y + p.rise * static_cast<float>(pa) / kHz), Fixed::From(p.z));
            if (pa % 3 == 0 || !p.shown) verb(node, "Create", fmt3(r * 2.0f) + ", " + fmt3(r * 1.8f) + ", " + fmt3(r * 2.0f));
            if (step != p.said && step > 0) { verb(node, "SetSmoke", fmt3(static_cast<float>(step) / 20.0f)); p.said = step; }
            if (!p.shown)
            {
                verb(node, "SetColor", f4(p.grey, p.grey * 0.96f, p.grey * 0.92f, 1.0f));
                verb(node, "SetVisible", "1");
                p.shown = 1;
            }
        }
    }
    // The fire out: its core and sparks put away (the smoke goes on).
    void fireOutLocked(Burst& b)
    {
        const size_t k = static_cast<size_t>(&b - m_bursts);
        if (k < m_blast_nodes.size()) parkNode(m_blast_nodes[k]);
        for (size_t n = 0; n < b.sparks.size(); ++n)
            if (b.sparks[n].shown && k * kSparks + n < m_spark_nodes.size()) parkNode(m_spark_nodes[k * kSparks + n]);
        b.sparks.clear();
    }
    void endBurstLocked(Burst& b)
    {
        const size_t k = static_cast<size_t>(&b - m_bursts);
        fireOutLocked(b);
        for (size_t n = 0; n < b.puffs.size(); ++n)
            if (b.puffs[n].shown == 1 && k * kPuffs + n < m_smoke_nodes.size()) parkNode(m_smoke_nodes[k * kPuffs + n]);
        b.puffs.clear();
        b.live = false;
    }
    // The fire, a tick on: the sparks the front has reached shown, the core
    // swelling with it, and all of it fading once grown. Drawn only.
    void burstsLocked()
    {
        for (int k = 0; k < kBursts; ++k)
        {
            Burst& b = m_bursts[k];
            if (!b.live) continue;
            const int64_t age = static_cast<int64_t>(m_tick) - b.born;
            puffsLocked(k, b, age);
            if (age > b.done) { endBurstLocked(b); continue; }
            if (age > kGrowTicks + kFadeTicks) { if (!b.sparks.empty()) fireOutLocked(b); continue; }
            const Fixed front = b.reach * F(std::min<int64_t>(age + 1, kGrowTicks), kGrowTicks);
            const ETCS::RID core = static_cast<size_t>(k) < m_blast_nodes.size() ? m_blast_nodes[k] : 0;
            if (age <= kGrowTicks && core)
            {
                const std::string d = fmt3((b.reach * (F(3, 10) + F(4, 10) * front / b.reach)).ToFloat());
                verb(core, "Create", d + ", " + d + ", " + d);
                if (age == 0) { placeNode(core, b.x, b.y, b.z); verb(core, "SetVisible", "1"); }
            }
            for (size_t n = 0; n < b.sparks.size(); ++n)
            {
                Spark& sp = b.sparks[n];
                const size_t at = static_cast<size_t>(k) * kSparks + n;
                if (sp.shown || sp.d0 > front || at >= m_spark_nodes.size()) continue;
                const ETCS::RID node = m_spark_nodes[at];
                const Fixed sx = sp.x1 - sp.x0, sy = sp.y1 - sp.y0, sz = sp.z1 - sp.z0;
                const float len = Fixed::Length(sx, sy, sz).ToFloat();
                if (!(len > 0.01f)) continue;
                const float hot = (sp.e0 / b.reach).ToFloat();       // 1 at the burst, 0 spent
                // Round, not flattened: at most about 1.9 long to 1 across.
                const float along = len * 1.15f, across = len * (0.6f + 0.3f * hot);
                verb(node, "Create", fmt3(across) + ", " + fmt3(across) + ", " + fmt3(along));
                verb(node, "SetColor", f4(1.0f, 0.25f + 0.65f * hot, 0.05f + 0.55f * hot * hot, 1.0f));
                poseAlong(node, (sp.x0 + sp.x1) * Fixed::Half(), (sp.y0 + sp.y1) * Fixed::Half(), (sp.z0 + sp.z1) * Fixed::Half(), sx, sy, sz);
                verb(node, "SetVisible", "1");
                sp.shown = 1;
            }
            // The glow, in ten steps down once it is grown.
            const int step = age <= kGrowTicks ? 0 : static_cast<int>((age - kGrowTicks) * 10 / kFadeTicks);
            if (step != b.fade)
            {
                b.fade = step;
                const std::string glow = fmt3(1.6f * (1.0f - static_cast<float>(step) / 10.0f));
                if (core) verb(core, "SetGlow", fmt3(0.8f * (1.0f - static_cast<float>(step) / 10.0f)));
                for (size_t n = 0; n < b.sparks.size(); ++n)
                    if (static_cast<size_t>(k) * kSparks + n < m_spark_nodes.size())
                        verb(m_spark_nodes[static_cast<size_t>(k) * kSparks + n], "SetGlow", glow);
            }
        }
    }

    // Damage, and what it comes to: at nothing, one to whoever did it, one
    // against the kart, and three seconds out. Only in a round.
    void damageLocked(size_t victim, int attacker, int amount, const char* what = "the arena")
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
        const std::string by = attacker >= 0 && attacker != static_cast<int>(victim) ? m_slots[attacker].driver : std::string(what);
        noteLocked(by + " took out " + v.driver);
        const int64_t back = static_cast<int64_t>(m_tick) + kRespawn;
        parkLocked(victim);
        v.hp = 0;   // out: nothing, until the spawn makes it whole
        v.dead_until = back;
    }

    // A weapon to a kart, of the two it carries (KartArms.h): more of one it
    // has, else in hand, else the spare -- in place of what the spare was.
    // What it came to, for the card's note.
    const char* armLocked(Slot& s, uint8_t w)
    {
        const int more = KartArms::Ammo(w), most = 3 * more;
        if (s.weapon == w) { s.ammo = static_cast<uint8_t>(std::min(most, s.ammo + more)); return "more "; }
        if (s.weapon == KartArms::None) { s.weapon = w; s.ammo = static_cast<uint8_t>(more); s.cooldown = 0; return "you have a "; }
        if (s.spare == w) { s.spare_ammo = static_cast<uint8_t>(std::min(most, s.spare_ammo + more)); return "more spare "; }
        s.spare = w; s.spare_ammo = static_cast<uint8_t>(more);
        return "your spare is a ";
    }
    // The power-ups: a kart that drives over one gets a weapon off the dice
    // (armLocked); the box is back eight seconds later.
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
                const uint8_t got = static_cast<uint8_t>(1 + KartArms::Next(m_rng) % (KartArms::Count - 1));
                const char* said = armLocked(s, got);
                if (s.driver == m_me) noteLocked(std::string(said) + KartArms::Name(got));
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
                poseNode(m_pad_nodes[n].arrow[k], p.x + p.sx * at, p.y + (p.gx * p.sx + p.gz * p.cz) * at + F(6, 100), p.z + p.cz * at,
                         p.yaw, Fixed::One(), Fixed::Zero(), p.gx, p.gz);
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
    // about its own across by the angle whose sine and cosine are given --
    // and the whole then tilted onto ground that slopes (gx, gz).
    void poseNode(ETCS::RID node, Fixed x, Fixed y, Fixed z, int yaw, Fixed psin, Fixed pcos,
                  Fixed gx = Fixed::Zero(), Fixed gz = Fixed::Zero())
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
        const Fixed w = yc * pc, qx = yc * ps, qy = ys * pc, qz = -(ys * ps);
        r.qw = w; r.qx = qx; r.qy = qy; r.qz = qz;
        if (gx.IsZero() && gz.IsZero()) return;
        Fixed tw, tx, tz;
        tiltTo(gx, gz, tw, tx, tz);
        // tilt x that: (tw, tx, 0, tz) x (w, qx, qy, qz)
        r.qw = tw * w - tx * qx - tz * qz;
        r.qx = tw * qx + tx * w - tz * qy;
        r.qy = tw * qy - tx * qz + tz * qx;
        r.qz = tw * qz + tx * qy + tz * w;
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
    /*
     * A pooled node not in use: hidden, far below, IN A BERTH OF ITS OWN. The
     * pools are members of the world like anything drawn, and members whose
     * reaches meet are a pair every interaction (etcs_causal_constraints.md
     * §13): a thousand parked at one point were half a million pairs a tick
     * -- the battle's whole frame. Each takes the next berth the first time
     * it is parked (the scripts make the pools in one order, so a berth is
     * the same on every runtime): a grid kBerth apart, inside the world's
     * space. One that reaches further than half a berth is made small first:
     * every pool that large -- blocks, slabs, wedges, domes, marks -- is
     * made to size again when it is next drawn.
     */
    void parkNode(ETCS::RID node)
    {
        auto at = m_berths.find(node);
        if (at == m_berths.end()) at = m_berths.emplace(node, static_cast<uint32_t>(m_berths.size())).first;
        const int64_t k = at->second, row = 2 * kBerthsOut + 1;
        if (Causal_* c = causal(node))
        {
            Fixed reach;
            { std::lock_guard<std::recursive_mutex> lk(c->TreeMutex()); reach = c->Order4().radius; }
            if (reach > F(kBerth, 2) - F(1, 2)) verb(node, "Create", "0.1, 0.1, 0.1");
        }
        placeNode(node, F((k % row - kBerthsOut) * kBerth), F(-250), F((k / row % row - kBerthsOut) * kBerth));
        hide(node);
    }
    static constexpr int64_t kBerth = 10, kBerthsOut = 25;   // 51 x 51 berths, the farthest 433 out
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
        parkNode(m_shot_nodes[n]);
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
                + std::to_string(s.cooldown) + " " + std::to_string(s.boost) + " " + std::to_string(s.spare) + " "
                + std::to_string(s.spare_ammo) + " " + std::to_string(s.slip) + " " + std::to_string(s.dagger) + " " + std::to_string(s.kx.raw) + " "
                + std::to_string(s.kz.raw) + " " + std::to_string(s.elims) + " "
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
        // The lobby's: over the whole arena, from the south.
        const float half = KartArena::Half().ToFloat();
        eye[0] = 0.0f; eye[1] = half * 1.94f; eye[2] = -half * 1.44f;
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
    // The frame counter: the camera's own count of the pictures it made
    // (Camera3D.Renders), over each second of frames -- what was drawn, not
    // how often the frame edge came round.
    void fpsLocked(double dt_ms)
    {
        if (!m_fps || !m_camera) return;
        m_fps_ms += dt_ms;
        if (m_fps_ms < 1000.0 && m_fps_seen >= 0) return;
        const std::string said = answer(m_camera, "Renders");
        const int64_t now = said.empty() ? -1 : std::atoll(said.c_str());
        if (now >= 0 && m_fps_seen >= 0 && m_fps_ms > 0.0)
        {
            const int fps = static_cast<int>(static_cast<double>(now - m_fps_seen) * 1000.0 / m_fps_ms + 0.5);
            verb(m_fps, "SetText", std::to_string(fps) + " fps");
        }
        m_fps_seen = now;
        m_fps_ms = 0.0;
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
              + (m_me.empty() ? std::string() : m_authority ? std::string("   yours to start") : "   " + m_owner + " starts it");
        else if (i < 0) t = m_me.empty() ? "kart" : "watching -- the arena is full";
        else
        {
            const Slot& s = m_slots[i];
            if (s.dead_until >= 0)
                t = "out -- back in " + std::to_string((s.dead_until - static_cast<int64_t>(m_tick) + kHz - 1) / kHz);
            else
            {
                char hp[16]; std::snprintf(hp, sizeof hp, "%.1f", s.hp / 10.0);
                t = std::string("HP ") + hp + "   " + (s.weapon ? std::string(KartArms::Name(s.weapon)) + " x" + std::to_string(s.ammo) : std::string("no weapon"))
                  + (s.spare ? std::string(" (then ") + KartArms::Name(s.spare) + " x" + std::to_string(s.spare_ammo) + ")" : std::string());
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
                    std::snprintf(line, sizeof line, "%d. %-18s %6d %7d%s", place++, labelLocked(s.driver).substr(0, 18).c_str(),
                                  s.last_elims, s.last_deaths, s.driver == m_me ? "  <" : "");
                else
                    std::snprintf(line, sizeof line, "%d. %-18s %6d %7d %5.1f%s", place++, labelLocked(s.driver).substr(0, 18).c_str(),
                                  s.elims, s.deaths, s.hp / 10.0, s.driver == m_me ? "  <" : "");
                rows.push_back(line);
            }
        };
        if (m_phase == Lobby && !m_me.empty())
        {
            auto item = [&](int at, const std::string& text) { return (m_authority && m_menu == at ? "> " : "  ") + text; };
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
            rows.push_back(m_authority ? "[up/down] choose  [left/right] change  [Enter] start" : m_owner + " sets the next round");
        }
        else if (m_phase != Lobby && (m_board_held || m_phase == Results))
        {
            rows.push_back(m_phase == Playing ? "ROUND  " + clock(m_end - static_cast<int64_t>(m_tick)) + " left  --  " + mapName(m_map)
                                              : "ROUND OVER  --  " + mapName(m_map));
            char head[96];
            std::snprintf(head, sizeof head, "   %-18s %6s %7s %5s", "driver", "elims", "deaths", "hp");
            rows.push_back(head);
            ranked(false);
            if (m_authority && m_phase == Playing) { rows.push_back(" "); rows.push_back("[Esc] abandon the round"); }
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
    // A driver as a person names one: its seat, or else its label.
    int slotNamedLocked(const std::string& who) const
    {
        const int i = slotOf(who);
        if (i >= 0 || who.empty()) return i;
        for (size_t k = 0; k < m_slots.size(); ++k)
            if (!m_slots[k].driver.empty() && labelLocked(m_slots[k].driver) == who) return static_cast<int>(k);
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
    static Causal_* causal(ETCS::RID r) { return r ? ETCS::resolve_in_family<Causal_>("Causal", r) : nullptr; }
    // A verb's answer, "" when there is none.
    static std::string answer(ETCS::RID node, const char* name)
    {
        if (!node) return std::string();
        ETCS::Entity* e = ETCS::etcs_resolve_rid_anywhere(&ETCS::getLoader(), node);
        if (!e) return std::string();
        ETCS::Buffer action((e->getSourceTag().toString() + "." + name).c_str());
        ETCS::Buffer payload("");
        try { e->call(action, payload); } catch (...) { return std::string(); }
        return payload.c_str();
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

    ETCS::RID  m_world = 0, m_camera = 0, m_card = 0;
    std::vector<Slot> m_slots;

    // The arena: its walls, its maps, the map in play, and the pools.
    std::vector<ETCS::RID> m_walls;
    std::vector<KartArena::Map> m_defs;
    KartArena::Map m_live;
    std::vector<ETCS::RID> m_live_blocks;
    std::vector<ETCS::RID> m_block_nodes, m_slab_nodes, m_wedge_nodes, m_dome_nodes;
    std::vector<ETCS::RID> m_pickup_nodes, m_shot_nodes, m_blast_nodes, m_mark_nodes;
    std::map<ETCS::RID, uint32_t> m_berths;   // a pooled node's parking place (parkNode)
    std::vector<PadNodes> m_pad_nodes;
    std::vector<SpringNodes> m_spring_nodes;
    ETCS::RID m_floor = 0;
    std::vector<bool> m_shot_busy;
    std::vector<ETCS::RID> m_fx_nodes, m_ring_nodes;   // the weapons' effects: drawn, not state
    std::vector<bool> m_fx_busy, m_ring_busy;
    std::vector<Wisp> m_wisps;
    std::map<ETCS::RID, Dress> m_dress;
    std::set<ETCS::RID> m_baked;                        // the meshes this battle has baked
    std::vector<ETCS::RID> m_spark_nodes;   // the bursts' sparks: kBursts x kSparks, glowing
    std::vector<ETCS::RID> m_smoke_nodes;   // ...and their smoke: kBursts x kPuffs
    Burst      m_bursts[kBursts];
    uint64_t   m_burst_seq = 0;             // bursts so far: each one's tree is its own
    std::vector<BoardRow> m_board;

    // The session (who, whose trace, the clock, the snapshots) is the
    // families'; the battle's word to its page is its own.
    std::string m_note;

    // The battle: its round, its arena, its dice.
    Phase    m_phase = Lobby;
    int64_t  m_end = 0;
    uint32_t m_round_secs = 180;
    int      m_map = 0;
    uint64_t m_layout = 0;
    uint64_t m_rng = 0x9E3779B97F4A7C15ull;
    std::vector<Shot> m_shots;
    std::vector<Pickup> m_pickups;

    // This runtime's own: its keys, its eye, its card, its menu, its board.
    unsigned m_keys = 0;
    bool     m_invert = false;   // this runtime's wheel keys swapped (SetInvert)
    int      m_sent_t = 0, m_sent_s = 0, m_sent_f = 0;
    int      m_menu = MenuStart;
    bool     m_board_held = false;
    float    m_eye[3] = { 0.0f, 0.0f, 0.0f }, m_at[3] = { 0.0f, 0.0f, 0.0f };
    bool     m_eye_set = false;
    bool     m_ready = false;      // the arena is made (Ready): the lobby is open
    // The look round the kart (lookLocked): how far the boom is swung from
    // behind the kart and raised, by what, and how long it has been let alone.
    float    m_look_yaw = 0.0f, m_look_pitch = 0.0f, m_look_x = 0.0f, m_look_y = 0.0f;
    double   m_look_idle_ms = 0.0;
    unsigned m_look_keys = 0;      // Q is bit 0, E bit 1
    bool     m_look_drag = false;
    double   m_card_ms = 0.0;
    std::string m_card_text;
    ETCS::RID   m_fps = 0;          // BindFps: the frame counter's label
    double      m_fps_ms = 0.0;     // ...the frames' time since it was last said
    int64_t     m_fps_seen = -1;    // ...the camera's pictures then
};

#endif // KARTPROVIDER_KARTBATTLE_H__
