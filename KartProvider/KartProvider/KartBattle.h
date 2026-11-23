#ifndef KARTPROVIDER_KARTBATTLE_H__
#define KARTPROVIDER_KARTBATTLE_H__

#include "../../../core_defs.h"
#include "../../../ontology.h"
#include "../../../ontology/Noise.h"
#include "../../../ontology/Epoch.h"
#include "KartArms.h"
#include "KartArena.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
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
 *   look <colour|=> [<name>|=]   (in the lobby) the author's colour -- a
 *                            place in the palette, -1 its kart's own -- and
 *                            the name shown for it (none: its label); "="
 *                            keeps what it has
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
    // The lobby, drawn: a RenderProvider::Menu over the eye (kart_play.etcs).
    void BindMenu(ETCS::RID r)   { std::lock_guard<std::mutex> g(m_mu); m_menu_rid = r; m_menu_sent.clear(); m_menu_lines = 0; m_menu_focus.clear(); m_menu_shown = -1; }

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
        s.made_body = extentOf(body, s.made_volume);
        Fixed unused;
        s.made_tyre = extentOf(fr, unused);
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
    // A vehicle by name, the kart's numbers to begin with; Tune and
    // VehicleLook are of the latest (kart_vehicle_*.etcs). A name taken is
    // refused, and so is the kart's -- and then they are refused too.
    bool AddVehicle(const std::string& name)
    {
        std::lock_guard<std::mutex> g(m_mu);
        m_tuning = false;
        if (name.empty() || name.find(' ') != std::string::npos || vehicleNamedLocked(name) >= 0) return false;
        Vehicle v; v.name = name;
        m_vehicles.push_back(v);
        m_tuning = true;
        return true;
    }
    // "<what> <values...>" of the latest vehicle: top, boost <top> <push>,
    // push <start> <launch> <pull>, brake, reverse <acc> <top>, coast,
    // steer <lock> <jump> <ease> <back>, wheelbase, grip <rolling> <braking>,
    // slide <free> <least>, tyres <2|4> <side> <ahead>, body <w> <h> <d>,
    // tyre <diameter> <width> <drop>, reach, lean, ram <hp>.
    // Decimals read exactly (no float), so every runtime tunes the same.
    bool Tune(const std::string& line)
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (!m_tuning) return false;
        Vehicle& v = m_vehicles.back();
        std::istringstream in(line);
        std::string what;
        in >> what;
        std::vector<Fixed> n;
        std::vector<std::string> said;
        for (std::string w; in >> w; ) { Fixed f; if (!decimal(w, f)) return false; n.push_back(f); said.push_back(w); }
        auto need = [&](size_t k) { return n.size() == k; };
        auto positive = [&](size_t k) { for (size_t i = 0; i < k; ++i) if (!n[i].IsPositive()) return false; return true; };
        if (what == "top" && need(1) && positive(1)) { v.top = n[0]; return true; }
        if (what == "boost" && need(2) && positive(2)) { v.boost_top = n[0]; v.boost_push = n[1]; return true; }
        if (what == "push" && need(3) && positive(3)) { v.start = n[0]; v.launch = n[1]; v.pull = n[2]; return true; }
        if (what == "brake" && need(1) && positive(1)) { v.brake = n[0]; return true; }
        if (what == "reverse" && need(2) && positive(2)) { v.rev_acc = n[0]; v.rev_top = n[1]; return true; }
        if (what == "coast" && need(1) && n[0].raw >= 0) { v.coast = n[0]; return true; }
        if (what == "steer" && need(4) && positive(4)) { v.lock = n[0]; v.jump = n[1]; v.ease = n[2]; v.back = n[3]; return true; }
        if (what == "wheelbase" && need(1) && positive(1)) { v.base = n[0]; return true; }
        if (what == "grip" && need(2) && positive(2)) { v.grip = n[0]; v.brake_grip = n[1]; return true; }
        if (what == "slide" && need(2) && positive(2) && n[0] < Fixed::One() && n[1] <= Fixed::One()) { v.slide_free = n[0]; v.slide_least = n[1]; return true; }
        if (what == "tyres" && need(3) && (n[0] == F(2) || n[0] == F(4)) && n[1].raw >= 0 && n[2].IsPositive())
        {
            v.tyres = n[0] == F(2) ? 2 : 4; v.side = n[1]; v.ahead = n[2];
            return true;
        }
        if (what == "body" && need(3) && positive(3))
        {
            v.body = said[0] + ", " + said[1] + ", " + said[2]; v.volume = n[0] * n[1] * n[2];
            return true;
        }
        // A tyre stands on the ground: the body rides its radius and the drop over it.
        if (what == "tyre" && need(3) && positive(2) && (n[0] / F(2) + n[2]).IsPositive())
        {
            v.tyre = said[0] + ", " + said[1] + ", " + said[0]; v.drop = n[2]; v.ride = n[0] / F(2) + n[2];
            return true;
        }
        if (what == "reach" && need(1) && positive(1)) { v.reach = n[0]; return true; }
        if (what == "lean" && need(1) && n[0].raw >= 0) { v.lean = n[0]; return true; }
        if (what == "ram" && need(1) && n[0].raw >= 0) { v.ram = static_cast<int>(n[0].ToInt()); return true; }
        return false;
    }
    // The latest vehicle's body: a Mesh under the world (0: the body's own box).
    bool VehicleLook(ETCS::RID mesh) { std::lock_guard<std::mutex> g(m_mu); if (m_tuning) m_vehicles.back().look = mesh; return m_tuning; }
    // The vehicles, one a line, the kart first.
    std::string Vehicles()
    {
        std::lock_guard<std::mutex> g(m_mu);
        std::string out = kart().name + "\n";
        for (const Vehicle& v : m_vehicles) out += v.name + "\n";
        return out;
    }
    // This runtime's vehicle, in the lobby: a `ride` line.
    bool Ride(const std::string& name)
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (slotOf(m_me) < 0 || m_phase != Lobby || vehicleNamedLocked(name) < 0) return false;
        proposeLocked("ride " + name);
        return true;
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
    // The next of the fire's meshes, empty: the trees are baked into it (meshFireLocked).
    void AddFireMesh(ETCS::RID r)   { std::lock_guard<std::mutex> g(m_mu); m_fire_meshes.push_back(r); meshFireLocked(m_fire_meshes.size() - 1); }
    // The next shell's own mesh, empty: a burst's tree, bent to its ground, is copied into it (bentCopyLocked).
    void AddBurstMesh(ETCS::RID r)  { std::lock_guard<std::mutex> g(m_mu); m_burst_meshes.push_back(r); }
    // "<tree> <turn degrees> <bends> <steps> <cut> <cut unbent>": the latest burst's tree, for a test.
    std::string LastBurst()         { std::lock_guard<std::mutex> g(m_mu); return m_last_burst.empty() ? std::string("-") : m_last_burst; }
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
    void RestartConcrete() override { resetLocked(); }
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
     * runtime's alone and no line of the trace. In the lobby the keys move
     * the menu instead (menuKeyLocked): up/down choose, left/right change,
     * Enter starts or edits -- the host's the round's settings, everyone's
     * their own colour and name; each change is a line.
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
        if (down && key == kSettingsKey && m_menu_rid) { m_settings = !m_settings; m_editing = false; return; }
        if (down && m_settings && settingsKeyLocked(key)) return;
        if (down && m_ready && !m_defs.empty() && !m_me.empty())
        {
            if (m_phase == Lobby && menuKeyLocked(key)) return;
            if (m_authority && m_phase == Playing && key == 256) { takeLocked(m_me, "abandon"); return; }
        }
        int bit = key == 0 ? 5 : -1;   // no key: the held ones read again (SetInvert)
        if (key == 'W' || key == 265) bit = 0;
        else if (key == 'S' || key == 264) bit = 1;
        else if (key == 'A' || key == 263) bit = 2;
        else if (key == 'D' || key == 262) bit = 3;
        else if (key == ' ') bit = 4;
        if (bit < 0) return;
        if (bit < 5) { if (down) m_keys |= (1u << bit); else m_keys &= ~(1u << bit); }
        sendKeysLocked();
    }
    // The keys held, said as a `drive` line when what they ask has changed.
    void sendKeysLocked()
    {
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
        std::lock_guard<std::mutex> g(m_mu);
        if (m_invert == on) return;
        m_invert = on;
        sendKeysLocked();
    }
    bool Inverted() { std::lock_guard<std::mutex> g(m_mu); return m_invert; }

    /*
     * THE SETTINGS: this runtime's own, and none of them the battle's -- no
     * line, nothing hashed: which way the wheel's keys turn, how much road a
     * tyre's mark keeps (drawn only), how fast the look turns. O opens them
     * over the arena, in a round or in the lobby (whose menu has a button
     * for them too), drawn on the lobby's menu; Setting and Settings are the
     * same for a page, which keeps them for the tab.
     */
    bool Setting(const std::string& name, const std::string& value)
    {
        std::lock_guard<std::mutex> g(m_mu);
        return settingLocked(name, value);
    }
    // "invert <0|1> marks <units> look <scale>"
    std::string Settings()
    {
        std::lock_guard<std::mutex> g(m_mu);
        return "invert " + std::string(m_invert ? "1" : "0") + " marks " + std::to_string(m_mark_len.ToInt())
             + " look " + fmt3(m_look_speed);
    }
    void ShowSettings(bool on) { std::lock_guard<std::mutex> g(m_mu); m_settings = on && m_menu_rid; m_editing = false; }
    // Whether the drawn menu (the lobby's, or the settings) is over the arena: a page hides its pads under it.
    bool MenuUp() { std::lock_guard<std::mutex> g(m_mu); return menuShownLocked(); }

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
        const bool lobby = menuShownLocked();
        switch (ev.action)
        {
            case INPUT_BUTTON_DOWN:
                if (lobby && ev.key == 0) { menuPressLocked(ev.x, ev.y); break; }
                if (ev.key == kLookButton) { m_look_drag = true; m_look_x = ev.x; m_look_y = ev.y; }
                break;
            case INPUT_MOTION:
                if (lobby) verb(m_menu_rid, "Hover", std::to_string(ev.x) + " " + std::to_string(ev.y));
                if (!m_look_drag) break;
                // Let go outside the window: the platform says it is up.
                if (input_button_state(ev, kLookButton) == 0) { m_look_drag = false; break; }
                turnLookLocked((ev.x - m_look_x) * kLookTurn * m_look_speed, (ev.y - m_look_y) * kLookTurn * m_look_speed);
                m_look_x = ev.x; m_look_y = ev.y;
                break;
            case INPUT_BUTTON_UP:
                if (ev.key == kLookButton) m_look_drag = false;
                break;
            default: break;
        }
    }
    // A press on the lobby's panel, as a verb (window pixels): what a click does.
    void MenuPress(float x, float y)
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (menuShownLocked()) menuPressLocked(static_cast<int>(x), static_cast<int>(y));
    }
    // This runtime's name in the battle, from the page: its `look`, in the lobby.
    bool Rename(const std::string& name)
    {
        std::lock_guard<std::mutex> g(m_mu);
        const int i = slotOf(m_me);
        // No name is the seat's label again.
        if (i < 0 || m_phase != Lobby || (!name.empty() && (!validName(name) || nameTakenLocked(i, name)))) return false;
        proposeLocked("look =" + (name.empty() ? std::string() : " " + name));
        return true;
    }
    // "<colour> <name> <palette>", or "-": this runtime's look, for a page.
    std::string Livery()
    {
        std::lock_guard<std::mutex> g(m_mu);
        const int i = slotOf(m_me);
        if (i < 0) return "-";
        const Slot& s = m_slots[static_cast<size_t>(i)];
        std::string pal;
        for (int c = 0; c < kPaletteN; ++c) pal += (c ? "," : "") + hexOf(kPalette[c]);
        return std::to_string(s.colour >= 0 ? s.colour : i % static_cast<int>(kMaxKarts)) + " " + nameOfLocked(static_cast<size_t>(i)) + " " + pal;
    }
    // A colour of the palette (-1: the kart's own), for this runtime's kart, in the lobby.
    bool Recolour(int colour)
    {
        std::lock_guard<std::mutex> g(m_mu);
        const int i = slotOf(m_me);
        if (i < 0 || m_phase != Lobby || colour < -1 || colour >= kPaletteN) return false;
        proposeLocked("look " + std::to_string(colour) + " =");
        return true;
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
            out += std::to_string(i) + " " + nameOfLocked(i) + " " + std::to_string(elimsOf(s)) + " " + std::to_string(deathsOf(s))
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
        // Every map's, what is wrong before what is only bare: the answer holds a few lines (the log has all).
        std::vector<std::string> all;
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
                ETCS_LOG("KartBattle", "fault: " << m.name << ": " << line);
                all.push_back(m.name + ": " + line + "\n");
            }
        }
        std::stable_partition(all.begin(), all.end(), [](const std::string& l) { return l.find(": bare ") == std::string::npos; });
        std::string out;
        int more = 0;
        for (const std::string& line : all) { if (more == 0 && out.size() + line.size() <= 240) out += line; else ++more; }
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
             + " " + std::to_string(s.dagger) + " " + vehicleOf(s).name + " " + fmt3(s.lean.ToFloat());
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
        menuLocked();
        fpsLocked(dt_ms);
    }

private:
    static constexpr int kMarkNodes = 32;   // a tyre's drawn segments at most: bits of a 64-bit mask
    static_assert(kMarkNodes > 0 && kMarkNodes < 64, "a tyre's nodes are bits of Trail::used");
    // Every node of a tyre in use. Not (1u << kMarkNodes) - 1: at 32 that shift
    // is past a 32-bit word, undefined, and a compiler may drop the code it guards
    // (the web's did: no mark was ever drawn).
    static constexpr uint64_t kMarkAll = (uint64_t(1) << kMarkNodes) - 1u;
    /*
     * A TYRE'S MARK: the road it ran, as segments laid where it ran them and
     * left there, newest first, each as dark as what the tyre shed over it
     * (a level, 0..8; 0 is laid but not drawn). A segment is laid when the
     * way turns, the darkness changes, the tyre leaves or meets the ground,
     * or it is long enough -- so a reversal is a segment's end, and nothing
     * already laid moves. A rope of fixed length: past the settings' length of road
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
        uint64_t used = 0;                         // the tyre's drawn segments' nodes in use, a bit each (kMarkNodes < 64)
    };
    struct Slot
    {
        ETCS::RID   body = 0;
        ETCS::RID   wheels[4] = { 0, 0, 0, 0 };   // fr fl rr rl: drawn, placed by the battle
        ETCS::RID   bar_back = 0, bar_fill = 0;   // the HP bar: this runtime's to turn
        OrderVector pristine;                      // the rows as made: every restart starts here
        std::string driver;
        int         colour = -1;                   // its `look`: a place in the palette, -1 its own
        std::string name;                          // its `look`: shown for it; empty, its label
        int         ride = 0;                      // its vehicle: a place in m_vehicles
        Fixed       lean;                          // its body's roll into the turn, rad (+ to the picture's right)
        std::string made_body, made_tyre;          // the pool's extents, "w, h, d": the kart's
        Fixed       made_volume;                   // the pool's body's: a vehicle's mass is its volume over this
        int         thr = 0, steer = 0, fire = 0;
        int         press = 0;                     // a press of the trigger the tick has not yet seen
        Fixed       wheel;                         // radians, + turns toward (hz, -hx): the picture's right
        Fixed       wheel_ease;                    // how far a held wheel has eased: 0 at the press, 1 at twice the lock
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
        int         out_ticks = 0;                 // ticks beyond the walls, on end
        int         dagger = 0;                    // ticks of the smoke's dagger left
        int         dagger_fx = -1;                // its blade: drawn, not state
        int         drips = 0, drip_in = 0;        // oil still to drip behind it, and the ticks to the next drop
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
    static constexpr int kSmokePuffs = 10;         // a smoke bomb's ring
    struct Shot
    {
        uint8_t kind = 0; int owner = -1;
        Fixed x, y, z, vx, vy, vz;
        int life = 0, armed = 0, node = -1;
        int fx = -1, ring = -1;                    // a slick's node, a shockwave's: drawn, not state
        Fixed reach;                               // a slick's: how far out it is oil
        int cloud[kSmokePuffs];                    // a smoke's ring of puffs: drawn, not state
        Shot() { for (int& c : cloud) c = -1; }
    };
    struct BoardRow { ETCS::RID rid; std::string text; bool shown; };
    struct PadNodes { ETCS::RID plate = 0; ETCS::RID arrow[3] = { 0, 0, 0 }; };
    struct SpringNodes { ETCS::RID plate = 0; ETCS::RID arrow[4] = { 0, 0, 0, 0 }; };
    enum Phase { Lobby, Playing, Results };
    static constexpr int kPaletteN = 10;
    // The palette a `look` picks from: the karts' own eight, then two more.
    static constexpr float kPalette[kPaletteN][3] = {
        { 0.86f, 0.18f, 0.16f }, { 0.18f, 0.42f, 0.88f }, { 0.95f, 0.80f, 0.15f }, { 0.20f, 0.70f, 0.30f },
        { 0.95f, 0.50f, 0.10f }, { 0.60f, 0.30f, 0.80f }, { 0.20f, 0.80f, 0.85f }, { 0.92f, 0.92f, 0.90f },
        { 0.93f, 0.40f, 0.66f }, { 0.20f, 0.20f, 0.23f } };

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
    static constexpr uint16_t kSettingsKey = 'O';      // opens and shuts the settings
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
    struct Vehicle;   // below: what the numbers that follow are the kart's of
    /*
     * THE PEDAL: slow off the line, then as it always was -- quick, and less
     * the nearer the top, the launch's push falling with speed, never below
     * pull(). Over the first 30% of the top the push eases (smoothstep) from
     * startPush up to that curve, so the first few units of speed take about
     * a second; from there on it is the old pedal, a little over half a
     * second later: the slide speed of a full lock (16 or so) at about 3.5 s,
     * the top at about 6.4 s and 83 units -- reached, and drifted at.
     */
    static Fixed launch()    { return F(10); }
    static Fixed pull()      { return F(2); }
    static Fixed startPush() { return F(5, 2); }
    static Fixed topSpeed()  { return F(22); }
    static Fixed boostSpeed(){ return F(30); }
    static Fixed boostPush() { return F(18); }
    static Fixed pushAt(const Vehicle& c, Fixed v, Fixed top)
    {
        const Fixed u = v.IsPositive() ? v / top : Fixed::Zero();
        const Fixed p = c.launch * (Fixed::One() - u);
        const Fixed full = p > c.pull ? p : c.pull;
        if (!(u < F(3, 10))) return full;
        const Fixed e = u / F(3, 10), ease = e * e * (F(3) - F(2) * e);
        return c.start + (full - c.start) * ease;
    }
    static Fixed brake()     { return F(20); }
    static Fixed reverseAcc(){ return F(5); }
    static Fixed reverseTop(){ return F(6); }
    static Fixed coast()     { return F(6, 10); }
    static Fixed wheelLock() { return F(45, 100); }   // the lock standing still: where a press puts the wheel at once
    static Fixed steerJump() { return F(1, 2); }      // of the lock: where a press puts the wheel at once
    static Fixed steerEase() { return F(3, 2); }      // s: a hold eases the wheel on from there to twice the lock in this long
    static Fixed steerBack() { return F(3); }         // rad/s: back toward the middle, let go
    // The lock at a speed: the full lock standing still, less the faster the
    // kart goes, so the turn a press gives at speed (speed x lock /
    // wheelbase) stays near 1.7 to 1.9 rad/s from 16 u/s up instead of
    // growing with it. A long hold doubles it.
    static Fixed lockAt(const Vehicle& c, Fixed speed) { return c.lock / (Fixed::One() + speed.Abs() / F(10)); }
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
    static Fixed slideCost(const Vehicle& c, Fixed off)
    {
        const Fixed free = c.slide_free, least = c.slide_least;
        if (off <= free) return least;
        return Fixed::Min(Fixed::One(), least + (Fixed::One() - least) * (off - free) / (Fixed::One() - free));
    }
    /*
     * A VEHICLE: every number the drive reads. The first is the kart, the
     * numbers above as they always were; a vehicle script adds another and
     * tunes it (kart_vehicle_*.etcs: AddVehicle, Tune, VehicleLook), and a
     * driver picks one in the lobby -- a `ride` line, so every runtime drives
     * the same. Its look is presentation: the body keeps its extent, so it
     * meets the world as any kart does.
     */
    struct Vehicle
    {
        std::string name = "kart";
        Fixed start = startPush(), launch = KartBattle::launch(), pull = KartBattle::pull(), top = topSpeed();
        Fixed boost_top = boostSpeed(), boost_push = boostPush();
        Fixed brake = KartBattle::brake(), rev_acc = reverseAcc(), rev_top = reverseTop(), coast = KartBattle::coast();
        Fixed lock = wheelLock(), jump = steerJump(), ease = steerEase(), back = steerBack();
        Fixed base = wheelbase();
        Fixed grip = KartBattle::grip(), brake_grip = brakeGrip();
        Fixed slide_free = F(574, 1000), slide_least = F(1, 4);
        int   tyres = 4;                       // 2: in line, the front pair and the rear pair one each
        Fixed side = tyreSide(), ahead = tyreAhead();
        std::string body, tyre;                // "w, h, d" Created on the body and each tyre; empty: as the pool made them
        Fixed volume;                          // the body's; it weighs its share of the pool's (0: the pool's own)
        Fixed drop = F(3, 100);                // the tyres' centres below the body's
        Fixed ride = kartY();                  // the body's centre over the ground: on its tyres
        Fixed reach = kartR();                 // how far from its centre a shot meets it
        Fixed lean;                            // of the turn's lean, 0 none: a rolled body (poseLocked)
        int   ram = 20;                        // a ram's damage, boosting at speed
        ETCS::RID look = 0;                    // the body's mesh; 0 its own box
    };
    // The most a body leans, and how quickly it gets there (rad, per s).
    static Fixed leanMost() { return F(85, 100); }
    static Fixed leanPace() { return F(8); }

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
    // eighth of a second, standing (7.5 u/s, a hop of 2.8), and springGain()
    // of the kart's speed over the ground on top -- 15 at 15 u/s, which is
    // eleven high and onto a third storey; 18.5 at the top speed, seventeen
    // high: over the arena's wall, or onto anything a map has. Again when it
    // comes down on it. Its four arrows close on its middle.
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
    // The rope -- this much road, and the segment over -- is the settings'
    // (marks): kMarkLength unless this runtime says otherwise.
    static constexpr int kMarkLength = 80;
    static Fixed markSegMax(){ return F(5, 2); }    // a dark segment at most; an unmarked run...
    static Fixed markRunMax(){ return F(4); }       // ...at most this
    static Fixed markTurn()  { return F(99, 100); } // a way turned past ~8 degrees ends a segment...
    static Fixed markMin()   { return F(1, 4); }    // ...this long at least
    static Fixed kartY()     { return F(25, 100); }   // the kart's ride height: its body's centre over the ground
    static Fixed kartR()     { return F(1); }          // the kart's reach a shot meets
    static Fixed gravity()   { return F(10); }        // light: half of what it was -- air is where the fun is
    static Fixed climb()     { return F(35, 100); }   // more ground than this in one step is a wall
    static Fixed noseRise()  { return F(7, 10); }     // ...and this much under the nose (a slope of ~0.75)
    static Fixed bound()     { return KartArena::Half() - F(1); }   // the arena's inside, less a kart's reach
    // OVER THE WALL. The walls stop a kart whose feet are below wallRide():
    // their top (KartArena::WallTop) less what of the kart is under its feet.
    // One higher goes over -- off a spring, a jump, a storey's edge -- and
    // comes down on THE RIM: mountains round the arena (kart_rim.etcs draws
    // them), ground that climbs from the wall's top to rimRise() over it,
    // rimDeep() out. Nothing stays up there: it is scree, with no grip, and
    // a kart on it slides back down and in off the wall. Only one that
    // clears the whole rim has no ground under it, and at lostAt() is out, as
    // one that comes down in lava is -- and one that is somehow still beyond
    // the walls after kOutTicks is out too, so nobody waits out a round there.
    static Fixed wallRide(const Vehicle& c) { return KartArena::WallTop() + c.reach - c.ride; }
    static Fixed rimDeep()   { return F(8); }
    static Fixed rimRise()   { return F(12); }
    static Fixed wallOut()   { return KartArena::Half() + rimDeep(); }   // past the rim's far side
    static constexpr int kOutTicks = 3 * kHz;
    // The long arms reach across the arena, however big it is: the sniper
    // corner to corner at once, and a rocket or a round of the minigun lives
    // until it has flown as far (crossLife), so a shot from one corner can
    // still land in the other -- a long shot is a tactic, not a waste.
    static Fixed sniperReach() { return KartArena::Half() * F(283, 100); }
    static Fixed homingSpeed() { return F(22); }
    static Fixed homingSight() { return F(70); }      // how far a homing rocket sees a kart
    // Oil: one big slick where it is dropped, then kDrips drops behind the
    // kart as it goes on, each smaller than the last (dripReach), kDripTicks apart.
    static Fixed oilReach()    { return F(9, 2); }
    static constexpr int kDrips = 3, kDripTicks = kHz / 3;
    static Fixed dripReach(int k) { return k == 0 ? F(5, 2) : k == 1 ? F(7, 4) : F(6, 5); }
    // A mine's burst: well past a bomb's (4.5), so one left lying, or driven
    // onto, clears round it -- and as red as a bomb's.
    static Fixed mineReach()   { return F(7); }
    static Fixed waveReach()   { return F(9); }
    static Fixed waveThrow()   { return F(18); }
    static Fixed daggerReach() { return F(26, 10); }   // up against: a kart's length, middle to middle
    // A smoke bomb's ring: thick five seconds, dissipating fifteen, dressed
    // kSmokeSteps times over its life (each a verb a puff); its dagger, eight.
    static constexpr int kSmokeThick = 5 * kHz, kSmokeTicks = 20 * kHz, kSmokeSteps = 30, kDaggerTicks = 8 * kHz;
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
        if (verb == "look")
        {
            // The lobby's only, as the round's settings are: a kart does not
            // change colour under fire.
            // `look <colour|=> [<name>|=]`: each part said, or kept ("=") -- a
            // colour and a name changed one after the other are two lines that
            // do not undo each other. No name at all is the label again.
            const int i = slotOf(author);
            std::string colour, name;
            if (i < 0 || m_phase != Lobby || !(in >> colour)) return false;
            Slot& s = m_slots[static_cast<size_t>(i)];
            int c = s.colour;
            if (colour != "=") { char* end = nullptr; c = static_cast<int>(std::strtol(colour.c_str(), &end, 10)); if (!end || *end || c < -1 || c >= kPaletteN) return false; }
            in >> name;
            if (name == "=") name = s.name;
            // A name is one seat's: not another's, nor another's label.
            else if (!name.empty() && (!validName(name) || nameTakenLocked(i, name))) return false;
            s.colour = c; s.name = name;
            paintKartLocked(static_cast<size_t>(i));
            return true;
        }
        if (verb == "ride")
        {
            // `ride <vehicle>`: the lobby's only, as a look is.
            const int i = slotOf(author);
            std::string name;
            if (i < 0 || m_phase != Lobby || !(in >> name)) return false;
            const int v = vehicleNamedLocked(name);
            if (v < 0) return false;
            m_slots[static_cast<size_t>(i)].ride = v;
            rideLookLocked(static_cast<size_t>(i));
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
            bakeFiresLocked(seed ^ ETCS::EpochSeed());   // fresh fire for the round
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
    void resetLocked()
    {
        for (size_t i = 0; i < m_slots.size(); ++i) pitLocked(i);
        for (Shot& s : m_shots) dropShotLocked(s);
        m_shots.clear();
        clearWispsLocked();
        for (Burst& b : m_bursts) endBurstLocked(b);
        m_keys = 0; m_sent_t = m_sent_s = m_sent_f = 0;
        m_eye_set = false; m_menu = 0; m_editing = false;
        m_phase = Lobby; m_end = 0; m_round_secs = 180;
        m_rng = 0x9E3779B97F4A7C15ull;
        bakeFiresLocked(ETCS::EpochSeed() ^ 0xF17E5EEDull);   // the lobby's: the epoch's own
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
        const Fixed y = KartArena::Solid(m_live, x, z, &gx, &gz) + vehicleOf(s).ride;   // on the ground, under any deck
        s.sgx = gx; s.sgz = gz; s.vy = Fixed::Zero(); s.air = false;
        std::lock_guard<std::recursive_mutex> lk(k->TreeMutex());
        OrderVector& r = k->RowsUnder();
        const uint64_t id = r.id;
        const Fixed reach = r.radius;   // its extent's, which a ride may have changed
        r = s.pristine;
        r.id = id; r.radius = reach;
        r.PlaceAt(x, y, z);
        poseLocked(s, r);
    }
    // The pit: hidden, held, out of everyone's way.
    void pitLocked(size_t i)
    {
        m_slots[i].driver.clear();
        m_slots[i].colour = -1; m_slots[i].name.clear();
        m_slots[i].ride = 0; m_slots[i].lean = Fixed::Zero();
        verb(m_slots[i].body, "SetMesh", "0");
        verb(m_slots[i].body, "Create", m_slots[i].made_body);
        verb(m_slots[i].body, "SetMass", "1");
        for (ETCS::RID w : m_slots[i].wheels) verb(w, "Create", m_slots[i].made_tyre);
        paintKartLocked(i);
        parkLocked(i);
    }
    void parkLocked(size_t i)
    {
        Slot& s = m_slots[i];
        s.thr = s.steer = s.fire = s.press = 0; s.wheel = s.wheel_ease = Fixed::Zero(); s.steer_held = 0;
        s.hp = kFullHp; s.weapon = s.spare = KartArms::None; s.ammo = s.spare_ammo = 0; s.cooldown = s.boost = s.slip = s.out_ticks = s.dagger = s.drips = s.drip_in = 0;
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
        s.hp = kFullHp; s.weapon = s.spare = KartArms::None; s.ammo = s.spare_ammo = 0; s.cooldown = s.boost = s.slip = s.out_ticks = s.dagger = s.drips = s.drip_in = 0;
        s.kx = s.kz = Fixed::Zero(); s.lean = Fixed::Zero();
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
        for (int t = 0; t < 4; ++t) verb(s.wheels[t], "SetVisible", tyreShown(vehicleOf(s), t) ? "1" : "0");
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
                for (size_t i = 0; i < m_slots.size(); ++i) if (alive(m_slots[i])) { daggerLocked(i); dripLocked(i); }
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
            noteLocked(rank.empty() ? "the round is over" : clear ? "the round is over -- " + nameOfLocked(rank[0]) + " wins"
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
        const Vehicle& c = vehicleOf(s);
        wheelTurnLocked(c, s, steer, speed, step);
        if (s.boost > 0) --s.boost;
        if (s.slip > 0) --s.slip;
        if (s.air) return;
        Fixed lx = vx - hx * speed, lz = vz - hz * speed;
        const Fixed rolled = speed;
        const bool braking = (thr > 0 && speed.raw < 0) || (thr < 0 && speed.raw > 0);
        const Fixed top = s.boost > 0 ? c.boost_top : c.top;
        Fixed push = pushAt(c, speed, top);
        if (s.boost > 0 && push < c.boost_push) push = c.boost_push;
        if (s.slip > 0) push = push / F(3);   // oil: the tyres spin
        if (thr > 0)
        {
            if (speed.raw < 0) speed = Fixed::Min(speed + c.brake * step, Fixed::Zero());
            else if (speed < top) speed = Fixed::Min(speed + push * step, top);
        }
        else if (thr < 0)
        {
            if (speed.raw > 0) speed = Fixed::Max(speed - c.brake * step, Fixed::Zero());
            else               speed = Fixed::Max(speed - c.rev_acc * step, -c.rev_top);
        }
        else
        {
            speed -= speed * c.coast * step;
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
        if (speed > top && along.raw >= 0) speed = Fixed::Max(top, speed - c.brake * Fixed::Half() * step);
        if (speed > padTop()) speed = padTop();
        // The heading turns by speed x wheel / wheelbase, about +y: a turn of
        // +a takes +z toward +x, which with +z ahead is to the picture's right.
        const Fixed yaw = speed * s.wheel / c.base * step;
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
        Fixed most = (braking ? c.brake_grip : c.grip) * step;
        if (s.slip > 0) most = most / F(6);   // ...and hold a sixth of what they did
        const Fixed right = ax * s.hz - az * s.hx;   // the way's across, to the heading's right (hz, -hx)
        // The lean: toward the pull the tyres give (the turn's inside) by it
        // over gravity, eased there -- a slide leans as far as the grip holds.
        // In the air nothing pulls and it holds. The kart's is none.
        if (!c.lean.IsZero())
        {
            const Fixed pull = across.IsPositive() ? -right / across * Fixed::Min(across, most) / step : Fixed::Zero();
            Fixed want = c.lean * pull / gravity();
            if (want > leanMost()) want = leanMost(); else if (want < -leanMost()) want = -leanMost();
            const Fixed pace = leanPace() * step;
            Fixed d = want - s.lean;
            if (d > pace) d = pace; else if (d < -pace) d = -pace;
            s.lean += d;
        }
        if (across > most) { const Fixed keep = (across - most) / across; ax *= keep; az *= keep; }
        else               { ax = az = Fixed::Zero(); }
        Fixed nx = s.hx * on + ax, nz = s.hz * on + az;
        const Fixed was = Fixed::Length(wx, Fixed::Zero(), wz), pulled = Fixed::Length(nx, Fixed::Zero(), nz);
        Fixed now = pulled;
        if (pulled.IsPositive() && was > pulled)
        {
            now = was - slideCost(c, across / was) * (was - pulled);
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
        // Shared among the tyres it has (tyreShown); a hidden one takes none.
        const Vehicle& c = vehicleOf(s);
        const Fixed tyres = F(c.tyres);
        Fixed slide[4], sum = Fixed::Zero();
        Fixed dsn, dcs;
        s.wheel.SinCos(dsn, dcs);
        for (int t = 0; t < 4; ++t)
        {
            slide[t] = Fixed::Zero();
            if (!tyreShown(c, t)) continue;
            // The tyre's place off the body's middle, level: across (hz, -hx), ahead (hx, hz).
            const Fixed sx = (t % 2) ? -c.side : c.side, sz = (t < 2) ? c.ahead : -c.ahead;
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
            if (!tyreShown(c, t)) continue;
            const Fixed share = braked / tyres + (sum.IsPositive() ? scrubbed * slide[t] / sum : scrubbed / tyres);
            if (!share.IsPositive()) continue;
            Causal_* w = causal(s.wheels[t]);
            if (!w) { r.energy += share; continue; }   // no tyre: the body keeps it
            OrderVector heat;
            heat.energy = share;
            w->AbsorbUnder(heat);
        }
    }

    /*
     * THE WHEEL, turned by A and D. A press puts it at steerJump of the lock
     * for the kart's speed (lockAt) at once -- the wheel answers the key, not
     * a moment after it -- and held, it eases on from there to twice the
     * lock over steerEase: a tap is a gentle turn, and most of the turn is
     * in the hold, a long one a hard turn. Turned the other way it is at
     * that side's jump at once and eases again from there. Let go, it goes back to the middle at steerBack; and past where
     * it should be (the kart speeding up with the wheel turned, so the lock
     * shrinking) it comes back at that pace too.
     */
    static void wheelTurnLocked(const Vehicle& c, Slot& s, int steer, Fixed speed, Fixed step)
    {
        if (steer != s.steer_held) s.wheel_ease = Fixed::Zero();
        s.steer_held = steer;
        Fixed target = Fixed::Zero();
        if (steer != 0)
        {
            s.wheel_ease = Fixed::Min(s.wheel_ease + step / c.ease, Fixed::One());
            target = lockAt(c, speed) * (c.jump + (F(2) - c.jump) * s.wheel_ease) * F(steer);
            // Short of it: there at once (the press's jump, and the ease after it).
            if (((target - s.wheel) * F(steer)).IsPositive()) { s.wheel = target; return; }
        }
        const Fixed turn = c.back * step;
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
        Fixed g = groundFor(s, r, r.x, r.z, &gx, &gz) + vehicleOf(s).ride;
        const Fixed sp = Fixed::Length(vx, Fixed::Zero(), vz);
        // Past the walls' inside: over them (high enough, or out already) or stopped by them.
        const Fixed far = Fixed::Max(r.x.Abs(), r.z.Abs());
        const bool over = far > bound()
                       && (r.y - vehicleOf(s).ride >= wallRide(vehicleOf(s)) - climb() || Fixed::Max(s.px.Abs(), s.pz.Abs()) > bound() + F(1, 100));
        if (over)
        {
            // On the wall's top -- or a deck laid to it, if that is higher -- or past it all.
            const bool on_wall = far < wallOut() && r.y - vehicleOf(s).ride >= wallRide(vehicleOf(s)) - climb();
            const bool deck = on_wall && far <= KartArena::Half() && g > wallRide(vehicleOf(s)) + vehicleOf(s).ride;
            if (!deck)
            {
                // The rim: up from the wall's top the further out, and the
                // way down it is in. Scree: no grip (as oil), and a pull
                // down the slope whichever way the kart points.
                const Fixed out = Fixed::Max(Fixed::Zero(), far - KartArena::Half());
                const Fixed slope = rimRise() / rimDeep();
                g = (on_wall ? wallRide(vehicleOf(s)) + slope * out : voidAt()) + vehicleOf(s).ride;
                gx = gz = Fixed::Zero();
                if (on_wall)
                {
                    const bool along_x = r.x.Abs() > r.z.Abs();
                    const Fixed way = F((along_x ? r.x : r.z).raw > 0 ? 1 : -1);
                    (along_x ? gx : gz) = slope * way;
                    if (!s.air && !(r.y > g + F(5, 100)))
                    {
                        s.slip = std::max(s.slip, 3);
                        (along_x ? s.kx : s.kz) -= way * gravity() * F(2) * slope * dt();
                    }
                }
            }
        }
        // Beyond the walls too long, however it got there or stays: out.
        s.out_ticks = far > KartArena::Half() ? s.out_ticks + 1 : 0;
        bool wall = !over && g - r.y > climb();
        if (!wall && !over && sp.IsPositive() && !s.air)
        {
            const Fixed nx = r.x + vx / sp * F(9, 10), nz = r.z + vz / sp * F(9, 10);
            wall = groundFor(s, r, nx, nz) + vehicleOf(s).ride - r.y > noseRise();
        }
        if (wall)
        {
            // On the ground it is stopped; in the air it only does not go on
            // (a kart thrown up beside a pit's wall clears it as it rises).
            r.PlaceAt(s.px, r.y, s.pz);
            if (!s.air) r.SetVelocity(m, Fixed::Zero(), Fixed::Zero(), Fixed::Zero());
            g = groundFor(s, r, s.px, s.pz, &gx, &gz) + vehicleOf(s).ride;
        }
        Fixed y = r.y;
        const Fixed step = dt();
        // A spring under it throws it up, whatever way it was going.
        if (!s.air)
            if (const KartArena::Spring* up = KartArena::SpringAt(m_live, r.x, r.z))
                if ((g - vehicleOf(s).ride - up->y).Abs() < F(5, 100)) { s.air = true; s.vy = springKick(sp); }
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
        const bool molten = pit && pit->lava && (y - vehicleOf(s).ride + pit->depth).Abs() < F(5, 100);
        if (molten || (over && y - vehicleOf(s).ride < lostAt()) || s.out_ticks > kOutTicks)
            damageLocked(static_cast<size_t>(&s - m_slots.data()), -1, kFullHp, molten ? "the lava" : s.out_ticks > kOutTicks ? "the mountains" : "the drop");
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
    // stands on (or last stood on, in the air), then rolled into its lean.
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
        if (s.lean.IsZero()) return;
        // x roll about its own +z by -lean: its top toward +x, the turn's inside.
        Fixed sn, cs;
        (-s.lean / F(2)).SinCos(sn, cs);
        const Fixed w = r.qw, x = r.qx, y = r.qy, z = r.qz;
        r.qw = w * cs - z * sn;
        r.qx = x * cs + y * sn;
        r.qy = y * cs - x * sn;
        r.qz = z * cs + w * sn;
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
        return KartArena::Height(m_live, x, z, r.y - vehicleOf(s).ride, reach, gx, gz);
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
                    damageLocked(j, static_cast<int>(i), vehicleOf(s).ram);
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
                Shot& m = shootLocked(MineShot, i, mx, groundAt(mx, mz, y - vehicleOf(s).ride + climb()) + F(1, 10), mz, Fixed::Zero(), Fixed::Zero(), Fixed::Zero(), kMineTicks);
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
                // The big slick behind it now; the drops follow as it goes on (dripLocked).
                slickLocked(i, x - hx * F(4), y, z - hz * F(4), oilReach());
                s.drips = kDrips; s.drip_in = kDripTicks;
                break;
            case Smoke:
            {
                Shot& c = shootLocked(SmokeShot, i, x, groundAt(x, z, y - vehicleOf(s).ride + climb()), z, Fixed::Zero(), Fixed::Zero(), Fixed::Zero(), kSmokeTicks);
                s.dagger = kDaggerTicks;
                for (int k = 0; k < kSmokePuffs; ++k) c.cloud[k] = takeFx();
                smokeRingLocked(c);
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
            if (off > vehicleOf(m_slots[j]).reach || !(along < best)) continue;
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
        if (hit >= 0) damageLocked(static_cast<size_t>(hit), static_cast<int>(i), 100 - static_cast<int>((F(50) * miss / vehicleOf(m_slots[static_cast<size_t>(hit)]).reach).raw >> 32));
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
                        else if (s.kind == BombShot) burstLocked(s.x, s.y, s.z, F(45, 10), 40, s.owner, 1);
                        else if (s.kind == HomingShot) burstLocked(s.x, s.y, s.z, F(3), 30, s.owner, 2);
                        else if (hit >= 0)           damageLocked(static_cast<size_t>(hit), s.owner, s.kind == Pellet ? 10 : 4);
                    }
                    break;
                }
                case MineShot:
                    if (s.armed > 0) --s.armed;
                    else if (kartAt(s.x, s.y, s.z, s.owner, F(6, 10)) >= 0) { gone = true; burstLocked(s.x, s.y, s.z, mineReach(), 30, s.owner, 1); }
                    // Left alone it does not lie there for good: its last two
                    // seconds it blinks, and then it bursts by itself.
                    if (!gone && s.life <= 0) { gone = true; burstLocked(s.x, s.y, s.z, mineReach(), 30, s.owner, 1); }
                    else if (!gone && s.life < 2 * kHz && s.life % 10 == 0 && s.node >= 0)
                        verb(m_shot_nodes[s.node], "SetColor", (s.life / 10) % 2 ? "1.0, 0.9, 0.2, 1.0" : "0.55, 0.08, 0.08, 1.0");
                    break;
                case OilShot:
                    // On the tyres of whoever is on it, but the kart that dropped it.
                    for (size_t j = 0; j < m_slots.size(); ++j)
                    {
                        Fixed jx, jy, jz;
                        if (static_cast<int>(j) == s.owner || !alive(m_slots[j]) || m_slots[j].air || !where(j, jx, jy, jz)) continue;
                        if (Fixed::Length(jx - s.x, Fixed::Zero(), jz - s.z) <= s.reach && (jy - s.y).Abs() < F(1)) m_slots[j].slip = kSlipTicks;
                    }
                    if (s.life <= 0) gone = true;
                    else if ((s.life == 20 || s.life == 10) && s.fx >= 0)   // drying up
                        dressLocked(m_fx_nodes[s.fx], slickSize(s.reach.ToFloat() * s.life / 30.0f), kOilColour, 0.0f, 0.0f);
                    break;
                case SmokeShot:
                    if (s.life <= 0) gone = true;
                    else smokeRingLocked(s);
                    break;
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
                        if (d > front + vehicleOf(m_slots[j]).reach || (jy - s.y).Abs() > F(5, 2)) continue;
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
                && Fixed::Length(x - s.x, Fixed::Zero(), z - s.z) < smokeRingR(kSmokeTicks - s.life) + F(3, 2)) return true;
        return false;
    }
    /*
     * THE DAGGER: every fifth of a second while it lasts, 8 to each kart
     * whose middle is within a kart's length of this one's -- up against
     * it, side by side, nose to tail. Its blade is drawn out of the nose: a
     * spear, longer than it cuts, so it reads as one.
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
            dressLocked(n, "0.1, 0.1, 4.2", "0.95, 0.95, 1.0, 1.0", 0.9f, 0.0f);   // a spear: out of the nose, three times the old blade
            if (Causal_* c = causal(n))
            {
                std::lock_guard<std::recursive_mutex> lk(c->TreeMutex());
                OrderVector& r = c->RowsUnder();
                r.PlaceAt(x + s.hx * F(27, 10), y, z + s.hz * F(27, 10));
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
    // Freed, an effect is set square again: a slick tilted to a ramp, or a
    // spear turned to a heading, would otherwise lend its turn to whatever takes it next.
    void freeFx(int n)
    {
        if (n < 0 || n >= static_cast<int>(m_fx_busy.size())) return;
        m_fx_busy[n] = false;
        undressLocked(m_fx_nodes[n]);
        if (Causal_* c = causal(m_fx_nodes[n]))
        {
            std::lock_guard<std::recursive_mutex> lk(c->TreeMutex());
            OrderVector& r = c->RowsUnder();
            r.qw = Fixed::One(); r.qx = r.qy = r.qz = Fixed::Zero();
        }
    }
    void freeRing(int n) { if (n >= 0 && n < static_cast<int>(m_ring_busy.size())) { m_ring_busy[n] = false; undressLocked(m_ring_nodes[n]); } }
    static std::string slickSize(float reach) { const std::string d = fmt1(2.0f * reach); return d + ", 0.08, " + d; }
    static constexpr const char* kOilColour = "0.05, 0.05, 0.08, 1.0";
    // A slick of `reach` on the ground under (x, z), dropped by kart i, laid
    // on its slope (a ramp's, a hill's); `lift` over the ground, so a drop on
    // the big slick is seen on it.
    void slickLocked(size_t i, Fixed x, Fixed y, Fixed z, Fixed reach, Fixed lift = Fixed::Zero())
    {
        Fixed gx = Fixed::Zero(), gz = Fixed::Zero();
        const Fixed h = KartArena::Height(m_live, x, z, y - vehicleOf(m_slots[i]).ride + climb(), Fixed::Zero(), &gx, &gz);
        Shot& o = shootLocked(KartArms::OilShot, i, x, h + F(4, 100) + lift, z, Fixed::Zero(), Fixed::Zero(), Fixed::Zero(), kOilTicks);
        o.reach = reach;
        o.fx = takeFx();
        if (o.fx >= 0)
        {
            poseNode(m_fx_nodes[o.fx], o.x, o.y, o.z, 0, Fixed::Zero(), Fixed::One(), gx, gz);
            dressLocked(m_fx_nodes[o.fx], slickSize(reach.ToFloat()), kOilColour, 0.0f, 0.0f);
        }
    }
    // The oil after the big slick: a drop behind the kart every kDripTicks
    // as it goes on, each smaller than the last.
    void dripLocked(size_t i)
    {
        Slot& s = m_slots[i];
        if (s.drips <= 0 || --s.drip_in > 0) return;
        Fixed x, y, z;
        if (!where(i, x, y, z)) { s.drips = 0; return; }
        const int k = kDrips - s.drips;
        slickLocked(i, x - s.hx * F(2), y, z - s.hz * F(2), dripReach(k), F(k + 1, 200));
        --s.drips; s.drip_in = kDripTicks;
    }
    /*
     * A SMOKE BOMB IS A RING: kSmokePuffs puffs round where it went off,
     * spreading from 1.5 out to 8 over its thick five seconds -- fast, then
     * slowing -- and on to 10 as it thins over fifteen, so it walls a kart
     * in and anyone outside out. Placed every tick (rows, not lines), turning
     * slowly; dressed kSmokeSteps times over its life (a verb a puff each):
     * swelling all along, dense while thick, thinning to nothing after.
     */
    static Fixed smokeRingR(int age)
    {
        if (age < kSmokeThick) { const Fixed v = Fixed::One() - F(age, kSmokeThick); return F(3, 2) + F(13, 2) * (Fixed::One() - v * v); }
        return F(8) + F(2) * F(std::min(age, kSmokeTicks) - kSmokeThick, kSmokeTicks - kSmokeThick);
    }
    void smokeRingLocked(const Shot& c)
    {
        const int age = kSmokeTicks - c.life;
        const Fixed r = smokeRingR(age);
        for (int k = 0; k < kSmokePuffs; ++k)
        {
            if (c.cloud[k] < 0) continue;
            Fixed sn, cs;
            (Fixed::TwoPi() * F(k, kSmokePuffs) + F(age, 6 * kHz * 10)).SinCos(sn, cs);
            placeNode(m_fx_nodes[static_cast<size_t>(c.cloud[k])], c.x + sn * r, c.y + F(6, 5) + F(k % 2, 2), c.z + cs * r);
        }
        const int step = age * kSmokeSteps / kSmokeTicks;
        if (age > 0 && step == (age - 1) * kSmokeSteps / kSmokeTicks) return;
        const int at = step * kSmokeTicks / kSmokeSteps;   // the age this step stands for
        const float g = static_cast<float>(step) / kSmokeSteps;
        const float thin = at < kSmokeThick ? 0.0f : static_cast<float>(at - kSmokeThick) / (kSmokeTicks - kSmokeThick);
        const std::string w = fmt1(4.5f + 4.0f * g), h = fmt1(3.2f + 2.3f * g);
        for (int k = 0; k < kSmokePuffs; ++k)
            if (c.cloud[k] >= 0)
                dressLocked(m_fx_nodes[static_cast<size_t>(c.cloud[k])], w + ", " + h + ", " + w, "0.5, 0.5, 0.53, 1.0", 0.0f, 0.95f * (1.0f - thin) + 0.03f);
    }
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
     * A BURST IS A TREE OF FIRE, and the tree is what hurts -- and the trees
     * are BAKED. kFires of them are grown at a reach of one -- a dozen
     * branches out round a point, a little up to well up, in steps of a
     * fifth of the reach, turning a little at every step and forking now and
     * then (less the further out) -- when a session opens, from the epoch
     * (ontology/Epoch.h: every runtime that can join shares it), and again as
     * each round starts, from the round's seed and the epoch. A burst is one of them, turned about +y and sized to its
     * reach: which one, and how far turned, are the burst's own dice (the
     * tick, the point, the bursts so far). So the fire looks different from
     * one lobby and one round to the next, and costs nothing to grow.
     *
     * IT IS WARPED, THE GROUND BENDS IT, THEN WHAT IS DENSE CUTS IT. The
     * tree, set down, is curved all through by a field of noise of its own
     * (the warp), pushed toward the open by what is round the burst
     * (bendsLocked: round an edge, over a wall), and a step that is still in
     * a wall, a block, the ground or a
     * deck's slab is not taken, nor anything that grew from it: the fire
     * stops there. Of kTurns turns the dice offer, a burst takes the one
     * that loses least -- so fire set off against a block pours round it and
     * away, and what is behind the block is reached only the long way.
     *
     * A kart is hurt by the most energy any branch has where it passes
     * within its reach (or the whole of it, at the burst's own point):
     * `max` times that over the reach. In the open that is the straight
     * falloff; behind a block, less, or nothing. Every number is the rows'
     * (Fixed), so the same tree does the same damage on every runtime.
     *
     * DRAWN AS IT WAS BAKED. Each tree is baked into kShells meshes, its steps
     * by what they carry -- white-hot near the point, orange, red at the ends
     * -- each step a glowing PUFF about its part of the way: nearly round, a
     * little longer than it is wide and overlapping the next, so a branch
     * reads as a string of small bursts rather than a line (fire is too
     * dissipative to draw thin lines). A burst is kShells nodes given those
     * meshes, turned as its tree is, swelling from a third of their size to
     * the whole in kGrowTicks and fading over kFadeTicks, round a core that
     * swells and fades with them. The meshes are made when the trees are, not
     * a burst; a burst's shells are COPIES of them bent as its tree was (the
     * renderer bends them: Mesh.CopyBent), so a burst says a few dozen things
     * to the scene and is drawn where its fire went. What a cut step was is
     * still drawn: fire licks the face of a block it did not get past.
     */
    static constexpr int kFires = 8, kShells = 3, kTurns = 6, kFireBranches = 12, kFireSteps = 64;
    struct Spark { Fixed x0, y0, z0, x1, y1, z1, e0, e1, d0; };
    // A tree as baked: its steps at a reach of one about (0, 0, 0), heading +z, each with the step it grew from.
    struct FireStep { Fixed x0, y0, z0, x1, y1, z1, e0, e1, d0; int from = -1; };
    struct Fire { std::vector<FireStep> steps; std::vector<Fixed> az; };
    // A bend of a burst's tree by the ground round it (bendsLocked).
    struct Bend { Fixed cx, cy, cz, dx, dy, dz, r; };
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
        std::vector<Spark> sparks;                // its tree's steps as set down, less what was cut
        std::vector<float> az;                    // the first branches' bearings, for the gaps between
        std::vector<Puff>  puffs;
        int64_t            done = 0;              // the age the last puff is gone at
        int                fire = -1;             // which tree, and its turn (sine, cosine)
        Fixed              ts, tc;
        std::vector<Bend>  bends;                 // the ground's bends round its point
        uint32_t           noise = 0;             // its warp's seed
        int                cut = 0;               // its tree's steps the dense cut
        int                tint = 0;              // the weapon's fire: 0 a rocket's, 1 a bomb's or a mine's (redder), 2 a homing rocket's (purple)
        bool               shown = false;         // its shells up: drawn, not state
    };
    static constexpr int kBursts = 3, kGrowTicks = 8, kFadeTicks = 30, kFadeSteps = 5;
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
    /*
     * THE TREES, grown from `seed` at a reach of one: kFireBranches first
     * branches round the point, then one step of every live branch in turn
     * (so they grow together) until kFireSteps -- each step a fifth of the
     * reach, turned a little, costing what it goes (air: a unit a unit), and
     * now and then a fork off its way with most of what it has.
     */
    void bakeFiresLocked(uint64_t seed)
    {
        m_fires.assign(kFires, Fire());
        uint64_t r = seed | 1;
        auto unit = [&r]() { return F(static_cast<int64_t>(KartArms::Next(r) % 2001) - 1000, 1000); };   // -1..1
        for (Fire& fire : m_fires)
        {
            struct Tip { Fixed x, y, z, dx, dy, dz, e, d; int depth, from; };
            std::deque<Tip> tips;
            const Fixed step = F(1, 5);
            for (int k = 0; k < kFireBranches; ++k)
            {
                Fixed sn, cs;
                const Fixed bearing = Fixed::TwoPi() * (F(k) + unit() * F(3, 10)) / F(kFireBranches);
                bearing.SinCos(sn, cs);
                fire.az.push_back(bearing);
                Tip t{ Fixed::Zero(), Fixed::Zero(), Fixed::Zero(), sn, F(1, 10) + (unit() + Fixed::One()) * F(3, 10), cs, Fixed::One(), Fixed::Zero(), 0, -1 };
                unitize(t.dx, t.dy, t.dz);
                tips.push_back(t);
            }
            while (!tips.empty() && static_cast<int>(fire.steps.size()) < kFireSteps)
            {
                Tip t = tips.front(); tips.pop_front();
                Fixed dx = t.dx + unit() * F(35, 100), dy = t.dy + unit() * F(25, 100), dz = t.dz + unit() * F(35, 100);
                unitize(dx, dy, dz);
                const Fixed go = Fixed::Min(step, t.e);
                FireStep st;
                st.x0 = t.x; st.y0 = t.y; st.z0 = t.z;
                t.x += dx * go; t.y += dy * go; t.z += dz * go;
                st.x1 = t.x; st.y1 = t.y; st.z1 = t.z;
                st.e0 = t.e; st.e1 = t.e - go; st.d0 = t.d; st.from = t.from;
                fire.steps.push_back(st);
                t.e -= go; t.d += go; t.dx = dx; t.dy = dy; t.dz = dz; t.from = static_cast<int>(fire.steps.size()) - 1;
                if (!t.e.IsPositive()) continue;
                if (t.depth < 3 && t.e > F(1, 4) && static_cast<int>(KartArms::Next(r) % 100) < 30 - t.depth * 8)
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
        for (size_t m = 0; m < m_fire_meshes.size(); ++m) meshFireLocked(m);
    }
    /*
     * THE WARP: the tree curved all through. Perlin's noise in each of the
     * three axes (ontology/Noise.h: FireWarp, a Warp with Perlin in its
     * slot), a field warpFreq cells to the reach whatever the reach, seeded
     * by the burst's dice -- its push at a point warpAmp times the point's
     * distance from the burst's: nothing at the middle, most at the ends. So the warp is stretched over the whole of
     * the tree, and each branch bends along all its length, smoothly, rather
     * than kinking somewhere. The bends below ride on it.
     *
     * THE BENDS: what the ground round a burst does to its tree. A burst
     * looks out from its point -- kLooks bearings, a little up and well up,
     * a quarter of its reach at a time -- and where a look meets something
     * dense it tries the ways beside it: turned to either side, up, then
     * further round. A BEND is made there, at the point met: a smooth push
     * toward the first of those ways that is open, at the same distance, so
     * a branch heading into a block is carried round its edge, or over it --
     * or, where nothing beside is open, a push back the way it came. A bend
     * fades to nothing over its reach ((1 - d^2/r^2)^2 at d), and is held off
     * the burst's own point. The nearest kBends are kept. All of it in units
     * of the reach, about the point, the arena's own axes, in the rows'
     * integers: the same bends on every runtime.
     *
     * So the baked tree, made once with no idea where it would go off, is
     * bent to where it did: the same tree pours round a corner here and
     * over a wall there. The steps are cut by what is dense only after the
     * bending, and the damage reads the bent tree; the picture is a copy of
     * the tree's meshes put through the same bends by the renderer
     * (Mesh.Bend, Mesh.CopyBent) -- so what is seen is what hurts.
     */
    static constexpr int kLooks = 12, kBends = 8;
    static Fixed holdR() { return F(1, 4); }   // within this of its point, a bend is scaled down to nothing there
    std::vector<Bend> bendsLocked(const Burst& b)
    {
        struct Met { Bend bend; Fixed t; };
        std::vector<Met> met;
        const Fixed lifts[2] = { F(1, 10), F(1, 2) };
        auto way = [](Fixed a, Fixed lift, Fixed& x, Fixed& y, Fixed& z) {
            Fixed sn, cs; a.SinCos(sn, cs);
            x = sn; y = lift; z = cs; unitize(x, y, z);
        };
        auto open = [&](Fixed x, Fixed y, Fixed z, Fixed t) {
            return densityAt(b.x + x * t * b.reach, b.y + y * t * b.reach, b.z + z * t * b.reach) == Fixed::One()
                && densityAt(b.x + x * t * Fixed::Half() * b.reach, b.y + y * t * Fixed::Half() * b.reach, b.z + z * t * Fixed::Half() * b.reach) == Fixed::One();
        };
        for (const Fixed& lift : lifts)
            for (int k = 0; k < kLooks; ++k)
            {
                const Fixed a = Fixed::TwoPi() * F(k, kLooks);
                Fixed x, y, z;
                way(a, lift, x, y, z);
                Fixed t = Fixed::Zero();
                for (int q = 1; q <= 4 && t.IsZero(); ++q)
                    if (!open(x, y, z, F(q, 4))) t = F(q, 4);
                if (t.IsZero()) continue;
                // Beside it: either side, up, further round.
                const Fixed turns[5][2] = { { F(6, 10), lift }, { F(-6, 10), lift }, { Fixed::Zero(), lift + F(7, 10) },
                                            { F(12, 10), lift }, { F(-12, 10), lift } };
                Bend bd;
                bd.cx = x * t; bd.cy = y * t; bd.cz = z * t;
                bd.dx = -x * t * Fixed::Half(); bd.dy = -y * t * Fixed::Half(); bd.dz = -z * t * Fixed::Half();
                for (const auto& tn : turns)
                {
                    Fixed ox, oy, oz;
                    way(a + tn[0], tn[1], ox, oy, oz);
                    if (!open(ox, oy, oz, t)) continue;
                    bd.dx = (ox - x) * t * F(9, 10); bd.dy = (oy - y) * t * F(9, 10); bd.dz = (oz - z) * t * F(9, 10);
                    break;
                }
                bd.r = F(45, 100) + t * F(25, 100);
                met.push_back({ bd, t });
            }
        std::stable_sort(met.begin(), met.end(), [](const Met& p, const Met& q) { return p.t < q.t; });
        std::vector<Bend> out;
        for (size_t k = 0; k < met.size() && out.size() < static_cast<size_t>(kBends); ++k) out.push_back(met[k].bend);
        return out;
    }
    // The warp's noise is Perlin's; the renderer redraws it with the same one (bentCopyLocked: Mesh.Warp).
    using FireWarp = ETCS::Noise::Warp<ETCS::Noise::Perlin, Fixed>;
    static Fixed warpAmp()  { return F(9, 10); }
    static Fixed warpFreq() { return F(3, 2); }
    // How far apart a step is looked along for what is dense: under the
    // thinnest thing fire must not pass (a pit's wall, 0.3), so a stretched step does not hop it.
    static Fixed kSampleGap() { return F(1, 4); }
    // A point about the burst's point (units of its reach, the arena's axes), put through the warp and the bends.
    static void bendPoint(const Burst& b, Fixed& x, Fixed& y, Fixed& z)
    {
        const Fixed out = Fixed::Length(x, y, z);
        const Fixed hold = Fixed::Min(Fixed::One(), out / holdR());
        Fixed px = Fixed::Zero(), py = Fixed::Zero(), pz = Fixed::Zero();
        FireWarp{ b.noise, warpFreq(), warpAmp() }.Push(x, y, z, px, py, pz);
        for (const Bend& bd : b.bends)
        {
            const Fixed ex = x - bd.cx, ey = y - bd.cy, ez = z - bd.cz;
            const Fixed q = (ex * ex + ey * ey + ez * ez) / (bd.r * bd.r);
            if (!(q < Fixed::One())) continue;
            const Fixed w = (Fixed::One() - q) * (Fixed::One() - q) * hold;
            px += bd.dx * w; py += bd.dy * w; pz += bd.dz * w;
        }
        x += px; y += py; z += pz;
    }
    // A burst's tree: one of the baked, turned, bent to the ground round it,
    // and cut by what is dense still -- at the turn of kTurns that loses least.
    void growLocked(Burst& b, uint64_t seed)
    {
        b.sparks.clear(); b.az.clear();
        if (m_fires.empty()) bakeFiresLocked(1);
        uint64_t r = seed | 1;
        b.fire = static_cast<int>(KartArms::Next(r) % kFires);
        b.bends = bendsLocked(b);
        b.noise = static_cast<uint32_t>(KartArms::Next(r) >> 32);
        const Fire& fire = m_fires[static_cast<size_t>(b.fire)];
        const size_t n = fire.steps.size();
        /*
         * A STEP IS A CURVE: its two ends and its middle, each turned and put
         * through the warp and the bends (bendPoint), and the quadratic
         * through the three -- the way the renderer bends the step's puff
         * (Mesh.Parts: a bent copy bends each part along the curve through
         * the same three points), so what is drawn is what was cut and what hurts.
         */
        using P3 = std::array<Fixed, 3>;
        auto set = [&](Fixed ts, Fixed tc, Fixed x, Fixed y, Fixed z) {
            P3 o{ x * tc + z * ts, y, z * tc - x * ts };
            bendPoint(b, o[0], o[1], o[2]);
            return o;
        };
        auto curve = [&](Fixed ts, Fixed tc, const FireStep& st, P3& c0, P3& cm, P3& c1) {
            c0 = set(ts, tc, st.x0, st.y0, st.z0);
            cm = set(ts, tc, (st.x0 + st.x1) * Fixed::Half(), (st.y0 + st.y1) * Fixed::Half(), (st.z0 + st.z1) * Fixed::Half());
            c1 = set(ts, tc, st.x1, st.y1, st.z1);
        };
        // The curve at t (0 at c0, a half at cm, 1 at c1), about the point.
        auto along = [](const P3& c0, const P3& cm, const P3& c1, Fixed t) {
            const Fixed a = (t * t * F(2)) - t * F(3) + Fixed::One(), m = t * F(4) - t * t * F(4), e = t * t * F(2) - t;
            return P3{ c0[0] * a + cm[0] * m + c1[0] * e, c0[1] * a + cm[1] * m + c1[1] * e, c0[2] * a + cm[2] * m + c1[2] * e };
        };
        auto at = [&](const P3& p) { return P3{ b.x + p[0] * b.reach, b.y + p[1] * b.reach, b.z + p[2] * b.reach }; };
        // Whether a step's curve passes through anything dense: looked at
        // every kSampleGap of its way, not only at its middle and end -- a
        // warped, bent step can be long, and a block is one thick.
        auto blocked = [&](const P3& c0, const P3& cm, const P3& c1) {
            const Fixed len = (Fixed::Length(cm[0] - c0[0], cm[1] - c0[1], cm[2] - c0[2]) + Fixed::Length(c1[0] - cm[0], c1[1] - cm[1], c1[2] - cm[2])) * b.reach;
            const int64_t looks = std::min<int64_t>(16, std::max<int64_t>(2, (len / kSampleGap()).ToInt() + 1));
            for (int64_t k = 1; k <= looks; ++k)
            {
                const P3 p = at(along(c0, cm, c1, F(k) / F(looks)));
                if (densityAt(p[0], p[1], p[2]) > Fixed::One()) return true;
            }
            return false;
        };
        std::vector<char> cut(n), best_cut;
        Fixed best_lost = F(1000000), best_s = Fixed::Zero(), best_c = Fixed::One();
        for (int k = 0; k < kTurns; ++k)
        {
            Fixed ts, tc;
            (Fixed::TwoPi() * F(static_cast<int64_t>(KartArms::Next(r) % 360), 360)).SinCos(ts, tc);
            Fixed lost = Fixed::Zero();
            for (size_t i = 0; i < n; ++i)
            {
                const FireStep& st = fire.steps[i];
                cut[i] = st.from >= 0 && cut[static_cast<size_t>(st.from)];
                if (!cut[i])
                {
                    P3 c0, cm, c1;
                    curve(ts, tc, st, c0, cm, c1);
                    cut[i] = blocked(c0, cm, c1);
                }
                if (cut[i]) lost += st.e0;
            }
            if (lost < best_lost) { best_lost = lost; best_cut = cut; best_s = ts; best_c = tc; }
        }
        b.ts = best_s; b.tc = best_c;
        b.cut = 0;
        for (size_t i = 0; i < n; ++i)
        {
            if (best_cut[i]) { ++b.cut; continue; }
            const FireStep& st = fire.steps[i];
            P3 c0, cm, c1;
            curve(b.ts, b.tc, st, c0, cm, c1);
            // Two sparks a step, end to middle to end: the curve's chords, for the damage to read.
            const P3 q[3] = { at(c0), at(cm), at(c1) };
            const Fixed e[3] = { st.e0, (st.e0 + st.e1) * Fixed::Half(), st.e1 };
            for (int h = 0; h < 2; ++h)
            {
                Spark sp;
                sp.x0 = q[h][0]; sp.y0 = q[h][1]; sp.z0 = q[h][2]; sp.x1 = q[h + 1][0]; sp.y1 = q[h + 1][1]; sp.z1 = q[h + 1][2];
                sp.e0 = e[h] * b.reach; sp.e1 = e[h + 1] * b.reach; sp.d0 = st.d0 * b.reach;
                b.sparks.push_back(sp);
            }
        }
        const float turn = std::atan2(b.ts.ToFloat(), b.tc.ToFloat());
        for (const Fixed& a : fire.az) b.az.push_back(a.ToFloat() + turn);
        // What the same turn would have lost unbent: for a test to weigh the bending by.
        int straight = 0;
        for (size_t i = 0; i < n; ++i)
        {
            const FireStep& st = fire.steps[i];
            cut[i] = st.from >= 0 && cut[static_cast<size_t>(st.from)];
            if (!cut[i])
            {
                const P3 c0{ st.x0 * b.tc + st.z0 * b.ts, st.y0, st.z0 * b.tc - st.x0 * b.ts }, c1{ st.x1 * b.tc + st.z1 * b.ts, st.y1, st.z1 * b.tc - st.x1 * b.ts };
                cut[i] = blocked(c0, P3{ (c0[0] + c1[0]) * Fixed::Half(), (c0[1] + c1[1]) * Fixed::Half(), (c0[2] + c1[2]) * Fixed::Half() }, c1);
            }
            straight += cut[i] ? 1 : 0;
        }
        m_last_burst = std::to_string(b.fire) + " " + std::to_string(static_cast<int>(std::lround(turn * 180.0f / kPi))) + " "
                     + std::to_string(b.bends.size()) + " " + std::to_string(n) + " " + std::to_string(b.cut) + " " + std::to_string(straight);
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
    void burstLocked(Fixed x, Fixed y, Fixed z, Fixed radius, int max, int owner, int tint = 0)
    {
        Burst& b = blastLocked(x, y, z, radius);
        b.tint = tint;
        for (size_t j = 0; j < m_slots.size(); ++j)
        {
            if (static_cast<int>(j) == owner || !alive(m_slots[j])) continue;
            Fixed jx, jy, jz;
            if (!where(j, jx, jy, jz)) continue;
            const Fixed e = reachedBy(b, jx, jy, jz, vehicleOf(m_slots[j]).reach);
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
        b.live = true; b.x = x; b.y = y; b.z = z; b.reach = radius; b.born = static_cast<int64_t>(m_tick); b.fade = -1; b.tint = 0;
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
            p.r0 = step * 1.0f; p.r1 = step * 2.7f; p.rise = 1.2f; p.dense = 0.85f; p.grey = 0.07f;
            p.delay = kGrowTicks / 2 + i; p.life = 160 + 8 * i;
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
                p.r0 = reach * 0.3f; p.r1 = reach * 0.8f; p.rise = 0.8f; p.dense = 0.7f; p.grey = 0.14f;
                p.delay = kGrowTicks; p.life = 230;
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
            // Eight densities, each a verb: at least the first while it thickens,
            // and put away only once it has thinned to nothing.
            const int step = std::max(t < 0.15f ? 1 : 0, static_cast<int>(p.dense * thick * 8.0f + 0.5f));
            if (pa > p.life || step <= 0) { parkNode(node); p.shown = 2; continue; }
            const float grow = 1.0f - (1.0f - t) * (1.0f - t);
            const float r = p.r0 + (p.r1 - p.r0) * grow;
            placeNode(node, Fixed::From(p.x), Fixed::From(p.y + p.rise * static_cast<float>(pa) / kHz), Fixed::From(p.z));
            if (pa % 6 == 0 || !p.shown) verb(node, "Create", fmt3(r * 2.0f) + ", " + fmt3(r * 1.8f) + ", " + fmt3(r * 2.0f));
            if (step != p.said && step > 0) { verb(node, "SetSmoke", fmt3(static_cast<float>(step) / 8.0f)); p.said = step; }
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
        if (b.shown)
            for (size_t n = 0; n < static_cast<size_t>(kShells); ++n)
                if (k * kShells + n < m_spark_nodes.size()) undressLocked(m_spark_nodes[k * kShells + n]);
        b.shown = false;
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
            if (age > kGrowTicks + kFadeTicks) { if (b.shown || !b.sparks.empty()) fireOutLocked(b); continue; }
            const Fixed front = b.reach * F(std::min<int64_t>(age + 1, kGrowTicks), kGrowTicks);
            const ETCS::RID core = static_cast<size_t>(k) < m_blast_nodes.size() ? m_blast_nodes[k] : 0;
            if (age <= kGrowTicks && core)
            {
                const std::string d = fmt3((b.reach * (F(3, 10) + F(4, 10) * front / b.reach)).ToFloat());
                verb(core, "Create", d + ", " + d + ", " + d);
                if (age == 0) { placeNode(core, b.x, b.y, b.z); verb(core, "SetVisible", "1"); }
            }
            // The glow, in kFadeSteps down once it is grown.
            const int fade = age <= kGrowTicks ? 0 : std::min(kFadeSteps - 1, static_cast<int>((age - kGrowTicks) * kFadeSteps / kFadeTicks));
            const float glow = 1.6f * (1.0f - static_cast<float>(fade) / kFadeSteps);
            // Its shells: given their tree's meshes and turned as it is when it
            // goes up, sized as it swells, dimmed as it fades -- each said when it changes.
            // A rocket's fire white-hot to orange; a bomb's and a mine's redder
            // through and through; a homing rocket's purple, as it is magic.
            static const char* const hues[3][kShells] = { { "1.0, 0.86, 0.55, 1.0", "1.0, 0.55, 0.14, 1.0", "0.92, 0.25, 0.07, 1.0" },
                                                          { "1.0, 0.38, 0.16, 1.0", "0.88, 0.1, 0.03, 1.0", "0.5, 0.02, 0.02, 1.0" },
                                                          { "0.95, 0.75, 1.0, 1.0", "0.68, 0.28, 1.0, 1.0", "0.4, 0.08, 0.7, 1.0" } };
            const char* const* hue = hues[b.tint >= 0 && b.tint < 3 ? b.tint : 0];
            const float swell = 0.35f + 0.65f * static_cast<float>(std::min<int64_t>(age + 1, kGrowTicks)) / kGrowTicks;
            const std::string size = fmt3(kFireBox * b.reach.ToFloat() * swell);
            for (int n = 0; n < kShells && b.fire >= 0; ++n)
            {
                const size_t at = static_cast<size_t>(k) * kShells + static_cast<size_t>(n);
                const size_t mesh = static_cast<size_t>(b.fire) * kShells + static_cast<size_t>(n);
                if (at >= m_spark_nodes.size() || mesh >= m_fire_meshes.size()) continue;
                const ETCS::RID node = m_spark_nodes[at];
                if (!b.shown)
                {
                    // The tree's mesh bent to this burst's ground: a copy of it, into this shell's own.
                    const ETCS::RID shape = at < m_burst_meshes.size() ? bentCopyLocked(m_burst_meshes[at], m_fire_meshes[mesh], b) : m_fire_meshes[mesh];
                    Dress& d = m_dress[node];
                    if (d.mesh != shape) { verb(node, "SetMesh", std::to_string(shape)); d.mesh = shape; }
                    poseFireLocked(node, b);
                }
                dressLocked(node, size + ", " + size + ", " + size, hue[n], glow * (n == 0 ? 1.0f : n == 1 ? 0.85f : 0.7f), 0.0f);
            }
            b.shown = true;
            if (fade != b.fade)
            {
                b.fade = fade;
                if (core) verb(core, "SetGlow", fmt3(0.8f * (1.0f - static_cast<float>(fade) / kFadeSteps)));
            }
        }
    }
    // `into` made a copy of `from` put through the burst's warp and bends: the
    // warp as its seed and the turn into its frame, the bends said in the
    // mesh's own frame -- the tree's, turned back, over kFireBox.
    ETCS::RID bentCopyLocked(ETCS::RID into, ETCS::RID from, const Burst& b)
    {
        verb(into, "Warp", std::to_string(b.noise) + ", " + fmt3(warpFreq().ToFloat()) + ", " + fmt3(warpAmp().ToFloat()) + ", "
                         + fmt3(kFireBox) + ", " + std::to_string(b.ts.ToFloat()) + ", " + std::to_string(b.tc.ToFloat()));
        for (const Bend& bd : b.bends)
        {
            // The arena's axes back to the tree's (the inverse of the turn).
            auto back = [&](Fixed x, Fixed z, float& ox, float& oz) {
                ox = (x * b.tc - z * b.ts).ToFloat() / kFireBox; oz = (z * b.tc + x * b.ts).ToFloat() / kFireBox;
            };
            float cx, cz, dx, dz;
            back(bd.cx, bd.cz, cx, cz);
            back(bd.dx, bd.dz, dx, dz);
            verb(into, "Bend", fmt3(cx) + ", " + fmt3(bd.cy.ToFloat() / kFireBox) + ", " + fmt3(cz) + ", " + fmt3(dx) + ", "
                             + fmt3(bd.dy.ToFloat() / kFireBox) + ", " + fmt3(dz) + ", " + fmt3(bd.r.ToFloat() / kFireBox) + ", "
                             + fmt3(holdR().ToFloat() / kFireBox));
        }
        verb(into, "CopyBent", std::to_string(from));
        return into;
    }
    // A burst's shell where the burst is, turned as its tree was.
    void poseFireLocked(ETCS::RID node, const Burst& b)
    {
        Causal_* c = causal(node);
        if (!c) return;
        std::lock_guard<std::recursive_mutex> lk(c->TreeMutex());
        OrderVector& r = c->RowsUnder();
        r.PlaceAt(b.x, b.y, b.z);
        Fixed w, y;
        halfTurn(b.ts, b.tc, w, y);
        r.qw = w; r.qx = r.qz = Fixed::Zero(); r.qy = y;
    }
    /*
     * THE TREES' MESHES, baked into the empty ones the script made, kShells
     * to a tree in order: a tree's steps that carry more than three fifths
     * of its reach, more than three tenths, the rest. A step is a puff about
     * its part of the way -- an ellipsoid 1.15 of the step long and 0.6 to
     * 0.9 of it across by what it carries, so each overlaps the next -- in a
     * box kFireBox reaches across with the tree's point at its middle, which
     * is what a burst's node is sized to. Remade when the trees are.
     */
    static constexpr float kFireBox = 2.4f;
    void meshFireLocked(size_t at)
    {
        const ETCS::RID mesh = at < m_fire_meshes.size() ? m_fire_meshes[at] : 0;
        if (!mesh || m_fires.empty() || at >= static_cast<size_t>(kFires * kShells)) return;
        const Fire& fire = m_fires[at / kShells];
        const int shell = static_cast<int>(at % kShells);
        const int around = 6, rings = 3;   // a puff of 14 corners: round enough when it glows
        verb(mesh, "Clear", "");
        // Each puff a part, its poles first: a bent copy bends it along the
        // curve through its step's two ends and middle -- the poles stand
        // 1.15 times as far out as the ends, so the puffs overlap (growLocked: a step is a curve).
        verb(mesh, "Parts", std::to_string(2 + (rings - 1) * around) + ", " + fmt3(1.0f / 1.15f));
        int base = 0;
        for (const FireStep& st : fire.steps)
        {
            const float hot = st.e0.ToFloat();
            if ((hot > 0.6f ? 0 : hot > 0.3f ? 1 : 2) != shell) continue;
            const float p0[3] = { st.x0.ToFloat(), st.y0.ToFloat(), st.z0.ToFloat() };
            const float p1[3] = { st.x1.ToFloat(), st.y1.ToFloat(), st.z1.ToFloat() };
            float a[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] };
            const float al = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
            if (!(al > 1e-4f)) continue;
            for (float& v : a) v /= al;
            float u[3] = { a[2], 0.0f, -a[0] };                         // across, level: a x up
            const float ul = std::sqrt(u[0] * u[0] + u[2] * u[2]);
            if (ul > 0.001f) { u[0] /= ul; u[2] /= ul; } else { u[0] = 1.0f; u[2] = 0.0f; }
            const float w[3] = { a[1] * u[2] - a[2] * u[1], a[2] * u[0] - a[0] * u[2], a[0] * u[1] - a[1] * u[0] };
            const float along = 0.5f * 1.15f * al, across = 0.5f * al * (0.6f + 0.3f * hot);
            auto vertex = [&](float su, float sw, float sa) {
                float pos[3], nrm[3];
                for (int i = 0; i < 3; ++i)
                {
                    pos[i] = ((p0[i] + p1[i]) * 0.5f + u[i] * su * across + w[i] * sw * across + a[i] * sa * along) / kFireBox;
                    nrm[i] = u[i] * su / across + w[i] * sw / across + a[i] * sa / along;
                }
                const float nl = std::sqrt(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]);
                verb(mesh, "AddVertex", fmt3(pos[0]) + ", " + fmt3(pos[1]) + ", " + fmt3(pos[2]) + ", "
                                      + fmt3(nrm[0] / nl) + ", " + fmt3(nrm[1] / nl) + ", " + fmt3(nrm[2] / nl));
            };
            vertex(0.0f, 0.0f, -1.0f);
            vertex(0.0f, 0.0f, 1.0f);
            for (int g = 1; g < rings; ++g)
                for (int i = 0; i < around; ++i)
                {
                    const float lat = kPi * g / rings, lon = 2.0f * kPi * i / around;
                    vertex(std::sin(lat) * std::cos(lon), std::sin(lat) * std::sin(lon), -std::cos(lat));
                }
            auto ring = [&](int g, int i) { return std::to_string(base + 2 + (g - 1) * around + (i % around)); };
            auto tri = [&](const std::string& p, const std::string& q, const std::string& t) { verb(mesh, "AddTriangle", p + ", " + q + ", " + t); };
            for (int i = 0; i < around; ++i)
            {
                tri(std::to_string(base), ring(1, i + 1), ring(1, i));
                tri(std::to_string(base + 1), ring(rings - 1, i), ring(rings - 1, i + 1));
                for (int g = 1; g + 1 < rings; ++g)
                {
                    tri(ring(g, i), ring(g, i + 1), ring(g + 1, i + 1));
                    tri(ring(g, i), ring(g + 1, i + 1), ring(g + 1, i));
                }
            }
            base += 2 + (rings - 1) * around;
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
        for (size_t j = 0; j < m_slots.size(); ++j)
        {
            if (static_cast<int>(j) == except || !alive(m_slots[j])) continue;
            Fixed jx, jy, jz;
            if (!where(j, jx, jy, jz)) continue;
            const Fixed reach = vehicleOf(m_slots[j]).reach + r;
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
        const Vehicle& c = vehicleOf(s);
        for (int i = 0; i < 4; ++i)
        {
            Causal_* w = causal(s.wheels[i]);
            if (!w) continue;
            OrderVector& q = w->RowsUnder();
            // In the body's own frame: +z ahead, the front right wheel at +x
            // (the heading's across, (hz, -hx), on the level).
            Fixed ox = (i % 2) ? -c.side : c.side, oy = -c.drop, oz = (i < 2) ? c.ahead : -c.ahead;
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
            while (tr.used == kMarkAll)
            {
                for (auto it = tr.segs.rbegin(); it != tr.segs.rend(); ++it)
                    if (it->node >= 0) { unmarkLocked(base, tr, *it); break; }
            }
            int n = 0;
            while (tr.used & (uint64_t(1) << n)) ++n;
            tr.used |= uint64_t(1) << n;
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
        while (!tr.segs.empty() && rope - tr.segs.back().len >= m_mark_len)
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
        tr.used &= ~(uint64_t(1) << g.node);
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
                const Fixed reach = q.radius;   // its extent's, which a ride may have changed
                q = s.tyre_pristine[t];
                q.id = id; q.radius = reach;
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
                + std::to_string(s.spare_ammo) + " " + std::to_string(s.slip) + " " + std::to_string(s.out_ticks) + " " + std::to_string(s.dagger) + " " + std::to_string(s.drips) + " " + std::to_string(s.drip_in) + " " + std::to_string(s.kx.raw) + " "
                + std::to_string(s.kz.raw) + " " + std::to_string(s.elims) + " "
                + std::to_string(s.deaths) + " " + std::to_string(s.last_elims) + " " + std::to_string(s.last_deaths) + " "
                + std::to_string(s.dead_until) + " " + std::to_string(s.ram_safe) + " " + std::to_string(s.colour) + " " + s.name
                + " " + std::to_string(s.ride) + " " + std::to_string(s.lean.raw);
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
                + std::to_string(s.y.raw) + " " + std::to_string(s.z.raw) + " " + std::to_string(s.life) + " " + std::to_string(s.armed) + " " + std::to_string(s.reach.raw);
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
        if (keys != 0) turnLookLocked(static_cast<float>(keys * kLookKeyRate * m_look_speed * dt_ms / 1000.0), 0.0f);
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

    /*
     * THE LOBBY, DRAWN (RenderProvider::Menu). What is on it is said as lines
     * each frame and only the lines that changed are sent: the next round's
     * map and length (the host's to move), your kart's colour and your name
     * (yours), start, a new layout for a map made from a seed, the map from
     * above, and the drivers with their last round. The keys and the pointer
     * both move it; every change it makes is a line.
     */
    std::vector<std::string> menuItemsLocked() const
    {
        std::vector<std::string> items;
        if (m_settings) return { "invert", "marks", "look", "close" };
        const bool seated = slotOf(m_me) >= 0;
        if (m_authority) { items.push_back("map"); items.push_back("length"); }
        if (seated) { items.push_back("vehicle"); items.push_back("colour"); items.push_back("name"); }
        if (m_authority)
        {
            items.push_back("start");
            if (!m_defs.empty() && m_defs[m_map].generated) items.push_back("reroll");
        }
        items.push_back("settings");
        return items;
    }
    bool menuShownLocked() const { return m_menu_rid && (m_settings || (m_phase == Lobby && !m_me.empty() && m_ready)); }

    // The settings' choices, in their order: the marks' road and the look's pace.
    static constexpr int kMarkChoices[5] = { 0, 20, 40, 80, 160 };
    static constexpr float kLookChoices[3] = { 0.5f, 1.0f, 2.0f };
    bool settingLocked(const std::string& name, const std::string& value)
    {
        if (name == "invert")
        {
            if (value != "0" && value != "1") return false;
            const bool on = value == "1";
            if (on != m_invert) { m_invert = on; sendKeysLocked(); }
            return true;
        }
        if (name == "marks")
        {
            const int v = std::atoi(value.c_str());
            if (v < 0 || v > 400 || (v == 0 && value != "0")) return false;
            m_mark_len = F(v);
            for (size_t i = 0; i < m_slots.size(); ++i) for (int t = 0; t < 4; ++t) trimLocked(i, t);
            return true;
        }
        if (name == "look")
        {
            const float v = static_cast<float>(std::atof(value.c_str()));
            if (!(v >= 0.1f && v <= 5.0f)) return false;
            m_look_speed = v;
            return true;
        }
        return false;
    }
    // A step of a setting through its choices, -1 or +1.
    void settingStepLocked(const std::string& id, int dir)
    {
        if (id == "invert") { settingLocked("invert", m_invert ? "0" : "1"); return; }
        if (id == "marks")
        {
            int at = 0;
            for (int k = 0; k < 5; ++k) if (F(kMarkChoices[k]) <= m_mark_len) at = k;
            settingLocked("marks", std::to_string(kMarkChoices[(at + dir + 5) % 5]));
            return;
        }
        if (id == "look")
        {
            int at = 0;
            for (int k = 0; k < 3; ++k) if (kLookChoices[k] <= m_look_speed + 1e-3f) at = k;
            m_look_speed = kLookChoices[(at + dir + 3) % 3];
        }
    }
    // A key with the settings open: true when it was theirs (all but O, which shuts them, above).
    bool settingsKeyLocked(uint16_t key)
    {
        const std::vector<std::string> items = menuItemsLocked();
        const int n = static_cast<int>(items.size());
        const bool up = key == 265 || key == 'W', down = key == 264 || key == 'S';
        const bool left = key == 263 || key == 'A', right = key == 262 || key == 'D';
        const bool enter = key == 257 || key == 335 || key == ' ';
        m_set_focus = ((m_set_focus % n) + n) % n;
        if (key == 256) { m_settings = false; return true; }
        if (up)   { m_set_focus = (m_set_focus + n - 1) % n; return true; }
        if (down) { m_set_focus = (m_set_focus + 1) % n; return true; }
        if (!left && !right && !enter) return false;
        const std::string& at = items[static_cast<size_t>(m_set_focus)];
        if (at == "close") { if (enter) m_settings = false; return true; }
        settingStepLocked(at, left ? -1 : 1);
        return true;
    }

    // A key in the lobby: true when it was the menu's.
    bool menuKeyLocked(uint16_t key)
    {
        if (m_editing)
        {
            if (key == 257 || key == 335) { m_editing = false; if (validName(m_edit)) restyleLocked(-2, m_edit); return true; }
            if (key == 256) { m_editing = false; return true; }
            if (key == 259) { if (!m_edit.empty()) m_edit.pop_back(); return true; }
            char c = 0;
            if (key >= 'A' && key <= 'Z') c = static_cast<char>(key - 'A' + 'a');
            else if ((key >= '0' && key <= '9') || key == '-') c = static_cast<char>(key);
            if (c && m_edit.size() < kNameMax) m_edit += c;
            return true;   // typing drives nothing
        }
        const std::vector<std::string> items = menuItemsLocked();
        if (items.empty()) return false;
        const bool up = key == 265 || key == 'W', down = key == 264 || key == 'S';
        const bool left = key == 263 || key == 'A', right = key == 262 || key == 'D';
        const bool enter = key == 257 || key == 335;
        const int n = static_cast<int>(items.size());
        m_menu = ((m_menu % n) + n) % n;
        if (up)   { m_menu = (m_menu + n - 1) % n; return true; }
        if (down) { m_menu = (m_menu + 1) % n; return true; }
        if (!left && !right && !enter) return false;
        const std::string& at = items[static_cast<size_t>(m_menu)];
        if (at == "start" || at == "reroll" || at == "name" || at == "settings") { if (enter) menuActLocked(at, "press", 0); return true; }
        if (at == "colour") { menuActLocked(at, left ? "prev" : "next", 0); return true; }
        // Enter on a map made from a seed: the same map, a new seed.
        if (at == "map" && enter && m_defs[m_map].generated) { menuActLocked("reroll", "press", 0); return true; }
        menuActLocked(at, left ? "prev" : "next", 0);
        return true;
    }
    // A press at (x, y) of the window: what the menu says it is on.
    void menuPressLocked(int x, int y)
    {
        ETCS::Buffer a("Menu.Point"), d((std::to_string(x) + " " + std::to_string(y)).c_str());
        try { if (ETCS::Entity* e = ETCS::etcs_resolve_rid_anywhere(&ETCS::getLoader(), m_menu_rid)) e->call(a, d); }
        catch (...) { return; }
        std::istringstream in(d.toString());
        std::string id, part;
        int n = 0;
        in >> id >> part >> n;
        if (m_editing && id != "name") { m_editing = false; if (validName(m_edit)) restyleLocked(-2, m_edit); }
        if (id.empty() || id == "-" || id == "+") return;   // off the panel, or on nothing of it
        const std::vector<std::string> items = menuItemsLocked();
        for (size_t k = 0; k < items.size(); ++k) if (items[k] == id) (m_settings ? m_set_focus : m_menu) = static_cast<int>(k);
        if (m_settings) { if (id == "close") m_settings = false; else settingStepLocked(id, part == "prev" ? -1 : 1); return; }
        menuActLocked(id, part, n);
    }
    void menuActLocked(const std::string& id, const std::string& part, int n)
    {
        const int dir = part == "prev" ? -1 : 1;
        if (id == "colour")
        {
            const int i = slotOf(m_me);
            if (i < 0) return;
            int c = m_slots[static_cast<size_t>(i)].colour;
            if (c < 0) c = i % kMaxKarts;
            c = part == "swatch" ? n : ((c + dir) % kPaletteN + kPaletteN) % kPaletteN;
            restyleLocked(c, std::string());
            return;
        }
        if (id == "name") { const int i = slotOf(m_me); if (i >= 0) { m_editing = true; m_edit = nameOfLocked(static_cast<size_t>(i)); } return; }
        if (id == "vehicle")
        {
            const int i = slotOf(m_me);
            if (i < 0) return;
            const int count = static_cast<int>(m_vehicles.size()) + 1;
            const int v = ((m_slots[static_cast<size_t>(i)].ride + dir) % count + count) % count;
            proposeLocked("ride " + (v == 0 ? kart().name : m_vehicles[static_cast<size_t>(v - 1)].name));
            return;
        }
        if (id == "settings") { m_settings = true; m_set_focus = 0; m_editing = false; return; }
        if (!m_authority) return;
        if (id == "start") { roundLocked(); return; }
        if (id == "reroll") { if (m_defs[m_map].generated) configLocked(m_map, m_round_secs, true); return; }
        if (id == "map")
        {
            const int count = static_cast<int>(m_defs.size());
            configLocked(((m_map + dir) % count + count) % count, m_round_secs, true);
            return;
        }
        if (id == "length")
        {
            static const uint32_t lengths[] = { 60, 120, 180, 300 };
            int at = 0;
            for (int l = 0; l < 4; ++l) if (lengths[l] <= m_round_secs) at = l;
            at = (at + dir + 4) % 4;
            configLocked(m_map, lengths[at], false);
        }
    }
    // This runtime's `look`: -2 keeps the colour it has, an empty name the name.
    void restyleLocked(int colour, const std::string& name)
    {
        if (slotOf(m_me) < 0) return;
        proposeLocked("look " + (colour == -2 ? std::string("=") : std::to_string(colour)) + " " + (name.empty() ? std::string("=") : name));
    }

    void menuLocked()
    {
        if (!m_menu_rid) return;
        const int show = menuShownLocked() ? 1 : 0;
        if (show != m_menu_shown) { verb(m_menu_rid, "SetHidden", show ? "0" : "1"); m_menu_shown = show; if (!show) m_editing = false; }
        if (!show) return;
        std::vector<std::string> L;
        if (m_settings) settingsLinesLocked(L);
        else lobbyLinesLocked(L);
        // Only what changed goes over.
        if (m_menu_sent.size() > L.size()) m_menu_sent.resize(L.size());
        for (size_t k = 0; k < L.size(); ++k)
        {
            if (k < m_menu_sent.size() && m_menu_sent[k] == L[k]) continue;
            verb(m_menu_rid, "SetLine", std::to_string(k) + " " + L[k]);
            if (k < m_menu_sent.size()) m_menu_sent[k] = L[k]; else m_menu_sent.push_back(L[k]);
        }
        if (m_menu_lines != L.size()) { verb(m_menu_rid, "SetLines", std::to_string(L.size())); m_menu_lines = L.size(); }
        const std::vector<std::string> items = menuItemsLocked();
        const int at = m_settings ? m_set_focus : m_menu;
        const std::string focus = items.empty() ? "-" : items[static_cast<size_t>(((at % static_cast<int>(items.size())) + static_cast<int>(items.size())) % static_cast<int>(items.size()))];
        if (focus != m_menu_focus) { verb(m_menu_rid, "Focus", focus); m_menu_focus = focus; }
    }
    void settingsLinesLocked(std::vector<std::string>& L)
    {
        L.push_back("title|SETTINGS");
        L.push_back("sub|this tab's own -- none of it is the battle's");
        L.push_back(std::string("choice|invert|wheel|") + (m_invert ? "swapped: A right, D left" : "A left, D right") + "|1");
        L.push_back("choice|marks|marks|" + (m_mark_len.IsZero() ? std::string("off") : std::to_string(m_mark_len.ToInt()) + " units of road") + "|1");
        L.push_back("choice|look|look|" + std::string(m_look_speed < 0.75f ? "slow" : m_look_speed > 1.5f ? "fast" : "normal") + "|1");
        L.push_back("button|close|done|1|1");
        L.push_back("hint|arrows or click change them  O shuts");
    }
    void lobbyLinesLocked(std::vector<std::string>& L)
    {
        const bool host = m_authority;
        const int me = slotOf(m_me);
        L.push_back("title|LOBBY");
        L.push_back("sub|" + (m_session ? labelLocked(m_owner) + "'s battle" : std::string("practice")) + " -- next round");
        L.push_back("choice|map|map|" + mapName(m_map) + (m_layout ? "  #" + std::to_string(m_layout % 10000) : std::string()) + "|" + (host ? "1" : "0"));
        L.push_back("choice|length|length|" + clock(static_cast<int64_t>(m_round_secs) * kHz) + "|" + (host ? "1" : "0"));
        if (me >= 0)
        {
            const Slot& s = m_slots[static_cast<size_t>(me)];
            std::string sw;
            for (int c = 0; c < kPaletteN; ++c) sw += (c ? "," : "") + hexOf(kPalette[c]);
            L.push_back("choice|vehicle|vehicle|" + vehicleOf(s).name + "|1");
            L.push_back("swatches|colour|colour|" + std::to_string(s.colour >= 0 ? s.colour : me % kMaxKarts) + "|" + sw + "|1");
            L.push_back("field|name|name|" + (m_editing ? m_edit : nameOfLocked(static_cast<size_t>(me))) + "|1|" + (m_editing ? "1" : "0"));
        }
        L.push_back(host ? std::string("button|start|start the round|1|1") : "button|start|" + labelLocked(m_owner) + " starts the round|0|0");
        if (host && !m_defs.empty() && m_defs[m_map].generated) L.push_back("button|reroll|new layout|1|0");
        L.push_back("button|settings|settings (O)|1|0");
        // The table is twenty letters wide beside the map: K for eliminations, D for deaths.
        L.push_back("head|driver        K   D");
        for (size_t i : rankLocked())
        {
            const Slot& s = m_slots[i];
            char line[96];
            std::snprintf(line, sizeof line, "%-11s %3d %3d", nameOfLocked(i).substr(0, 11).c_str(), s.last_elims, s.last_deaths);
            L.push_back("row|" + hexOf(colourOfLocked(i)) + "|" + line);
        }
        L.push_back(std::string("hint|") + (m_editing ? "type a name  Enter keeps it  Esc drops it"
                                            : host ? "arrows move it  Enter starts  or click"
                                                   : "arrows: your vehicle, colour and name  or click"));
        previewLocked(L);
    }
    // The map in play from above: its ground, what stands on it, its pads,
    // springs and boxes, and where karts start.
    void previewLocked(std::vector<std::string>& L) const
    {
        const KartArena::Map& m = m_live;
        const float h = KartArena::Half().ToFloat();
        auto f1 = [](float v) { char b[16]; std::snprintf(b, sizeof b, "%.1f", v); return std::string(b); };
        auto rect = [&](float x, float z, float w, float d, const char* c) { L.push_back("rect|" + f1(x) + "|" + f1(z) + "|" + f1(w) + "|" + f1(d) + "|" + c); };
        auto disc = [&](float x, float z, float r, const char* c) { L.push_back("disc|" + f1(x) + "|" + f1(z) + "|" + f1(r) + "|" + c); };
        L.push_back("preview|" + f1(-h) + "|" + f1(-h) + "|" + f1(h) + "|" + f1(h));
        rect(0, 0, 2 * h, 2 * h, "3a4a35");
        for (const auto& p : m.pits) rect(p.x.ToFloat(), p.z.ToFloat(), p.w.ToFloat(), p.d.ToFloat(), p.lava ? "f25c14" : "22281f");
        for (const auto& k : m.hills) disc(k.x.ToFloat(), k.z.ToFloat(), k.radius.ToFloat(), "56684a");
        for (const auto& r : m.ramps)
        {
            const bool across = (((r.yaw % 180) + 180) % 180) == 90;
            const float len = r.length().ToFloat(), wid = r.width.ToFloat();
            rect(r.x.ToFloat(), r.z.ToFloat(), across ? len : wid, across ? wid : len, "8a8f6a");
        }
        for (const auto& d : m.decks) rect(d.x.ToFloat(), d.z.ToFloat(), d.w.ToFloat(), d.d.ToFloat(), "6e7f99");
        for (const auto& b : m.blocks)
        {
            const bool across = (((b.yaw % 180) + 180) % 180) == 90;
            rect(b.x.ToFloat(), b.z.ToFloat(), (across ? b.d : b.w).ToFloat(), (across ? b.w : b.d).ToFloat(), "b4ad97");
        }
        for (const auto& p : m.pads) disc(p.x.ToFloat(), p.z.ToFloat(), 2.0f, "2f8fd0");
        for (const auto& p : m.springs) disc(p.x.ToFloat(), p.z.ToFloat(), 2.0f, "c04fa0");
        for (const auto& p : m.pickups) disc(p.x.ToFloat(), p.z.ToFloat(), 1.6f, "f2cc26");
        for (const auto& p : m.spawns) disc(p.x.ToFloat(), p.z.ToFloat(), 1.8f, "f0e3c7");
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
                    std::snprintf(line, sizeof line, "%d. %-18s %6d %7d%s", place++, nameOfLocked(i).substr(0, 18).c_str(),
                                  s.last_elims, s.last_deaths, s.driver == m_me ? "  <" : "");
                else
                    std::snprintf(line, sizeof line, "%d. %-18s %6d %7d %5.1f%s", place++, nameOfLocked(i).substr(0, 18).c_str(),
                                  s.elims, s.deaths, s.hp / 10.0, s.driver == m_me ? "  <" : "");
                rows.push_back(line);
            }
        };
        if (m_phase == Lobby) {}   // the lobby is the menu's (menuLocked)
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
    // What a driver is called: its `look`'s name, or its seat's label.
    std::string nameOfLocked(size_t i) const
    {
        const Slot& s = m_slots[i];
        return s.name.empty() ? labelLocked(s.driver) : s.name;
    }
    const float* colourOfLocked(size_t i) const
    {
        const Slot& s = m_slots[i];
        return s.colour >= 0 && s.colour < kPaletteN ? kPalette[s.colour] : kColors[i % kMaxKarts];
    }
    // The kart, and the vehicles added after it: 0 is the kart.
    static const Vehicle& kart() { static const Vehicle k; return k; }
    const Vehicle& vehicleOf(const Slot& s) const
    {
        return s.ride > 0 && s.ride <= static_cast<int>(m_vehicles.size()) ? m_vehicles[static_cast<size_t>(s.ride - 1)] : kart();
    }
    int vehicleNamedLocked(const std::string& name) const
    {
        if (name == kart().name) return 0;
        for (size_t k = 0; k < m_vehicles.size(); ++k) if (m_vehicles[k].name == name) return static_cast<int>(k + 1);
        return -1;
    }
    // A decimal read exactly: [-]digits[.digits], as a ratio of integers.
    static bool decimal(const std::string& w, Fixed& out)
    {
        size_t i = 0;
        const bool neg = !w.empty() && w[0] == '-';
        if (neg) ++i;
        int64_t num = 0, den = 1;
        bool digits = false, point = false;
        for (; i < w.size(); ++i)
        {
            if (w[i] == '.' && !point) { point = true; continue; }
            if (w[i] < '0' || w[i] > '9' || num > 100000000) return false;
            num = num * 10 + (w[i] - '0');
            if (point) den *= 10;
            digits = true;
        }
        if (!digits) return false;
        out = F(neg ? -num : num, den);
        return true;
    }
    // A slot's vehicle as drawn: its body's mesh, and the tyres it rides on.
    void rideLookLocked(size_t i)
    {
        Slot& s = m_slots[i];
        const Vehicle& c = vehicleOf(s);
        verb(s.body, "Create", c.body.empty() ? s.made_body : c.body);
        // At the kart's density: what it weighs is its volume's share of the kart's.
        const Fixed mass = c.volume.IsPositive() && s.made_volume.IsPositive() ? c.volume / s.made_volume : Fixed::One();
        verb(s.body, "SetMass", exact(mass.ToFloat()));
        for (ETCS::RID w : s.wheels) verb(w, "Create", c.tyre.empty() ? s.made_tyre : c.tyre);
        verb(s.body, "SetMesh", std::to_string(c.look));
        const bool shown = alive(s);
        for (int t = 0; t < 4; ++t) verb(s.wheels[t], "SetVisible", shown && tyreShown(c, t) ? "1" : "0");
        s.lean = Fixed::Zero();
        Fixed x, y, z;
        if (shown && where(i, x, y, z)) placeLocked(i, x, z, s.hx, s.hz);   // at its own ride height
        wheelsLocked(s);
    }
    // Two in line: the right of each pair is drawn, the pair being one place.
    static bool tyreShown(const Vehicle& c, int t) { return c.tyres == 4 || t % 2 == 0; }
    // A float as text that reads back as the same float.
    static std::string exact(float f) { char b[32]; std::snprintf(b, sizeof b, "%.9g", f); return b; }
    // A node's extent as it was Created, "w, h, d", and its volume. A half
    // extent's words are a script's float halved, so each reads back exactly
    // and a Create with them makes the same extent again.
    static std::string extentOf(ETCS::RID rid, Fixed& volume)
    {
        ETCS::Entity* e = ETCS::etcs_resolve_rid_anywhere(&ETCS::getLoader(), rid);
        std::string v;
        volume = Fixed::Zero();
        if (!e || !e->valueOf(ETCS::Buffer("extent"), v) || v.size() != 24) return std::string();
        size_t at = 0;
        int64_t w[3];
        for (int64_t& x : w) ETCS::Entity::getWord(v, at, x);
        volume = Fixed::FromRaw(w[0]) * Fixed::FromRaw(w[1]) * Fixed::FromRaw(w[2]) * F(8);
        return exact(2.0f * Fixed::FromRaw(w[0]).ToFloat()) + ", " + exact(2.0f * Fixed::FromRaw(w[1]).ToFloat()) + ", "
             + exact(2.0f * Fixed::FromRaw(w[2]).ToFloat());
    }

    void paintKartLocked(size_t i)
    {
        const float* c = colourOfLocked(i);
        verb(m_slots[i].body, "SetColor", f4(c[0], c[1], c[2], 1.0f));
    }
    static std::string hexOf(const float* c)
    {
        char b[8];
        auto by = [](float v) { return static_cast<int>(std::lround(std::min(1.0f, std::max(0.0f, v)) * 255.0f)); };
        std::snprintf(b, sizeof b, "%02x%02x%02x", by(c[0]), by(c[1]), by(c[2]));
        return b;
    }
    static constexpr size_t kNameMax = 16;
    // A name a `look` may give: letters, digits, - and _, at most kNameMax.
    static bool validName(const std::string& n)
    {
        if (n.empty() || n.size() > kNameMax) return false;
        for (char c : n)
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
        return true;
    }
    bool nameTakenLocked(int mine, const std::string& n) const
    {
        for (size_t k = 0; k < m_slots.size(); ++k)
            if (static_cast<int>(k) != mine && !m_slots[k].driver.empty() && (m_slots[k].name == n || labelLocked(m_slots[k].driver) == n)) return true;
        return false;
    }
    // A driver as a person names one: its seat, or else its label.
    int slotNamedLocked(const std::string& who) const
    {
        const int i = slotOf(who);
        if (i >= 0 || who.empty()) return i;
        for (size_t k = 0; k < m_slots.size(); ++k)
            if (!m_slots[k].driver.empty() && (labelLocked(m_slots[k].driver) == who || m_slots[k].name == who)) return static_cast<int>(k);
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
    std::vector<Vehicle> m_vehicles;   // after the kart: what vehicle scripts added, in their order
    bool m_tuning = false;             // the latest AddVehicle took: Tune and VehicleLook are its
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
    std::vector<ETCS::RID> m_spark_nodes;   // the bursts' shells: kBursts x kShells, glowing
    std::vector<ETCS::RID> m_fire_meshes;   // the trees' meshes: kFires x kShells, baked here
    std::vector<Fire>      m_fires;         // the trees: from the epoch, then each round's seed and the epoch
    std::vector<ETCS::RID> m_burst_meshes;  // each shell's own bent copy: kBursts x kShells
    std::string            m_last_burst;    // "<tree> <turn> <bends> <steps> <cut> <cut unbent>", for a test
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
    // The settings (Setting): this runtime's, none of them the battle's.
    bool     m_settings = false;                // open over the arena
    int      m_set_focus = 0;
    Fixed    m_mark_len = Fixed::FromInt(kMarkLength);
    float    m_look_speed = 1.0f;
    int      m_sent_t = 0, m_sent_s = 0, m_sent_f = 0;
    int      m_menu = 0;                        // the menu's focus: a place in menuItemsLocked()
    bool     m_editing = false;                 // the name field has the keys
    std::string m_edit;                         // what is typed into it, before Enter
    ETCS::RID m_menu_rid = 0;
    std::vector<std::string> m_menu_sent;       // the lines the menu has, as last sent
    size_t   m_menu_lines = 0;
    std::string m_menu_focus;
    int      m_menu_shown = -1;
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
