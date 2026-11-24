#ifndef GOLFPROVIDER_GOLFMATCH_H__
#define GOLFPROVIDER_GOLFMATCH_H__

#include "../../../core_defs.h"
#include "../../../ontology.h"
#include "GolfCourse.h"
#include "GolfPowers.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <vector>

/*
 * ── GolfMatch: a round of golf, everyone's ball on the hole at once ───────
 *
 * THE TRACE IS THE MATCH, as it is the battle in kart (KartBattle.h). Every
 * runtime holds its own copy of the course -- the same scripts made it -- and
 * steps it from the host's trace alone (Lockstep, ontology/Lockstep.h): `tick`
 * lines step the world, and between them
 *
 *   join                      the author takes the next free ball, on the tee
 *                             (a seat's first line, taken as it sits)
 *   leave                     the author's ball off the course (its seat's
 *                             stream ended, however it did)
 *   shot <x> <y> <z>          the author's pull, in thousandths: its ball
 *                             struck along it, as hard as it is long -- only
 *                             a ball at rest, not yet holed out
 *   use <x> <y> <z> <yaw>     the author's power put down at that point
 *                             (thousandths), the way it looked (degrees)
 *   config <course> <secs> [<layout>]   (the host's, in the lobby) the next
 *                             round's course and each hole's time; a course
 *                             made from a seed carries its seed
 *   round <seed>              (the host's, in the lobby) the round starts on
 *                             the first hole: cards clear, the dice seeded
 *   abandon                   (the host's, in a round) back to the lobby
 *   give <who> <power>        (the host's) a power handed over outright
 *
 * An AUTHOR is a seat: over a link, the key its link proved (shown by the
 * label it gave, ontology/Trace.h); here, the name this runtime gave.
 *
 * The world is Scene3D marked driven (SetDriven): no picture steps it. The
 * balls are Causal solids on solid greens (etcs_causal_constraints.md §12-
 * §13): gravity is the world's space's, the bounce and the roll are the
 * contact's, and the cup is a container a ball that drops in is taken into by
 * fit -- which is what holed means. What this match adds on the rows is the
 * course's own things -- pads, springs, sand -- and the powers', all in
 * Fixed. So two runtimes fed the same lines are the same match; a copy that
 * is not is reported (Trace's `hash` line), not repaired.
 *
 * THE GAME. Between rounds is the LOBBY: the first hole of the next course is
 * out and anyone may knock a ball about on it, uncounted. A round is the
 * course's holes in order, every ball on the hole at once (and solid to each
 * other). A hole is over for a ball when it drops in, or when it comes to rest
 * after kCap strokes (scored kCap + 1); for everyone when every ball is
 * over, or when the hole's time is up (whoever is still out scored kCap + 1).
 * The lowest total over the course wins. A ball that falls off goes back to
 * where it was struck from, a stroke added. Rolling through a gold spot gives
 * a ball with no power a power (GolfPowers.h); E arms it, and a left press
 * and release puts it down where the pointer is.
 *
 * THE HAND IS THIS RUNTIME'S, and no line until it lets go: a drag that
 * starts on your own ball at rest pulls a shot back on the plane facing the
 * camera (the camera's pitch is the loft); a drag anywhere else, or with the
 * right button, turns the camera about your ball; letting go of the RIGHT
 * button while the left holds a pull or a placement puts it down -- the left's
 * release then finds nothing in hand. The wheel brings the camera in or out.
 * What is drawn for one runtime alone -- the camera, the arrow, the shadows,
 * the card, the board -- is set on the frame edge and is not a line.
 *
 * THE SESSION IS THE FAMILIES' (Trace -> Lockstep, and Present beside): a
 * guest's lines reach the host on its own seat's stream, and its copy mirrors
 * the host's trace; what is written here is what a match is.
 */
class GolfMatch : public LockstepBase<GolfMatch>,
                  public PresentBase<GolfMatch>,
                  public AnimatedBase<GolfMatch>,
                  public DeletableBase<GolfMatch>
{
public:
    WIRE_TYPE_IDENTITY(GolfMatch);

    GolfMatch() = default;
    bool DeleteConcrete() override { Leave(); return true; }

    bool Create() { this->addTag("active"); return true; }

    // ── this runtime's: the world, the eye, the card ─────────────────────

    void BindWorld(ETCS::RID r)  { std::lock_guard<std::mutex> g(m_mu); m_world = r; }
    /*
     * THE CUP IS DRAWN ROUND. What a ball drops through is a square gap cut
     * in the green (GolfCourse::CutCup: the ground is boxes), and what is
     * seen is a round hole: a RIM at the green's top, in the turf's colour,
     * over the gap's four corners -- a square with a circle out of it -- and
     * a dark WELL down from it, a tube, in front of the gap's square sides.
     * Both are drawn only and baked here into the two empty meshes the cup's
     * script made (golf_cup.etcs). The corners stay open under the rim: a
     * ball over one is over the gap, and its own width hides it.
     */
    void BakeCup(ETCS::RID rim, ETCS::RID rim_mesh, ETCS::RID well_mesh)
    {
        std::lock_guard<std::mutex> g(m_mu);
        m_cup_rim = rim; m_rim_said.clear();
        const int around = 32;
        auto v6 = [](float x, float y, float z, float nx, float ny, float nz) {
            return fmt3(x) + ", " + fmt3(y) + ", " + fmt3(z) + ", " + fmt3(nx) + ", " + fmt3(ny) + ", " + fmt3(nz);
        };
        auto tri = [](ETCS::RID m, int a, int b, int c) { verb(m, "AddTriangle", std::to_string(a) + ", " + std::to_string(b) + ", " + std::to_string(c)); };
        verb(rim_mesh, "Clear", "");
        verb(well_mesh, "Clear", "");
        for (int i = 0; i < around; ++i)
        {
            const float a = 2.0f * kPi * i / around, ca = std::cos(a), sa = std::sin(a);
            const float out = 0.5f / std::max(std::fabs(ca), std::fabs(sa));   // to the square's edge that way
            verb(rim_mesh, "AddVertex", v6(0.5f * ca, 0.0f, 0.5f * sa, 0.0f, 1.0f, 0.0f));
            verb(rim_mesh, "AddVertex", v6(out * ca, 0.0f, out * sa, 0.0f, 1.0f, 0.0f));
            verb(well_mesh, "AddVertex", v6(0.5f * ca, 0.5f, 0.5f * sa, -ca, 0.0f, -sa));
            verb(well_mesh, "AddVertex", v6(0.5f * ca, -0.5f, 0.5f * sa, -ca, 0.0f, -sa));
        }
        for (int i = 0; i < around; ++i)
        {
            const int p = 2 * i, q = 2 * ((i + 1) % around);
            // The rim's faces look up; the well's look in.
            tri(rim_mesh, p, q + 1, p + 1); tri(rim_mesh, p, q, q + 1);
            tri(well_mesh, p, p + 1, q + 1); tri(well_mesh, p, q + 1, q);
        }
    }
    void BindCamera(ETCS::RID r) { std::lock_guard<std::mutex> g(m_mu); m_camera = r; m_follow = true; m_sky_said.clear(); skyLocked(); }
    // The lobby, drawn: a RenderProvider::Menu over the eye (menuLocked).
    void BindMenu(ETCS::RID r)   { std::lock_guard<std::mutex> g(m_mu); m_menu_rid = r; m_menu_sent.clear(); m_menu_lines = 0; m_menu_focus.clear(); m_menu_shown = -1; }
    void BindCard(ETCS::RID r)   { std::lock_guard<std::mutex> g(m_mu); m_card = r; m_card_text.clear(); }
    void BindFps(ETCS::RID r)    { std::lock_guard<std::mutex> g(m_mu); m_fps = r; m_fps_ms = 0.0; m_fps_seen = -1; }
    // The arrow a pull draws, and its shadow on the ground under it.
    void BindArrow(ETCS::RID arrow, ETCS::RID shadow) { std::lock_guard<std::mutex> g(m_mu); m_arrow = arrow; m_arrow_shadow = shadow; }
    // Where a power will go while the left holds it: a ring, and an arrow for the way.
    void BindGhost(ETCS::RID ring, ETCS::RID arrow) { std::lock_guard<std::mutex> g(m_mu); m_ghost = ring; m_ghost_arrow = arrow; hide(ring); hide(arrow); }
    void AddBoardRow(ETCS::RID r)
    {
        std::lock_guard<std::mutex> g(m_mu);
        verb(r, "SetPosition", std::to_string(724) + ", " + std::to_string(84 + 22 * static_cast<int>(m_board.size())));
        verb(r, "SetHidden", "1");
        m_board.push_back({ r, std::string(), false });
    }
    // The pointer's longest pull (scene units) and the speed it gives (units/s).
    void SetPower(float max_pull, float max_speed)
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (max_pull > 0.0f)  m_max_pull = Fixed::From(max_pull);
        if (max_speed > 0.0f) m_max_speed = Fixed::From(max_speed);
    }

    // ── the pools: what the scripts make once ────────────────────────────

    // A ball of the pool and its shadow: coloured by its slot, off the course
    // until someone takes it.
    void AddBall(ETCS::RID ball, ETCS::RID shadow)
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (!ball || m_slots.size() >= kMaxBalls) return;
        Slot s; s.ball = ball; s.shadow = shadow;
        const float* c = kColors[m_slots.size() % kMaxBalls];
        verb(ball, "SetColor", f4(c[0], c[1], c[2], 1.0f));
        if (Causal_* k = causal(ball)) { std::lock_guard<std::recursive_mutex> lk(k->TreeMutex()); s.pristine = k->RowsUnder(); }
        m_slots.push_back(s);
        hide(shadow);
        parkBallLocked(m_slots.size() - 1);
        if (!m_me.empty() && !m_session && slotOf(m_me) < 0) takeLocked(m_me, "join");
    }
    void AddSlabNode(ETCS::RID r)  { std::lock_guard<std::mutex> g(m_mu); m_slab_nodes.push_back(r); parkNode(r); }
    void AddCupNode(ETCS::RID r)   { std::lock_guard<std::mutex> g(m_mu); m_cup = r; parkNode(r); }
    void AddTeeNode(ETCS::RID r)   { std::lock_guard<std::mutex> g(m_mu); m_tee = r; parkNode(r); }
    void AddSpotNode(ETCS::RID r)  { std::lock_guard<std::mutex> g(m_mu); m_spot_nodes.push_back(r); parkNode(r); }
    void AddPuffNode(ETCS::RID r)  { std::lock_guard<std::mutex> g(m_mu); m_puff_nodes.push_back(r); m_puff_until.push_back(-1); m_puff_size.emplace_back(); parkNode(r); }
    void AddPadNode(ETCS::RID plate, ETCS::RID a, ETCS::RID b, ETCS::RID c)
    {
        std::lock_guard<std::mutex> g(m_mu);
        PadNodes p; p.plate = plate; p.arrow[0] = a; p.arrow[1] = b; p.arrow[2] = c;
        verb(plate, "Create", f3(GolfCourse::PadWidth().ToFloat(), 0.04f, GolfCourse::PadLength().ToFloat()));
        verb(plate, "SetColor", "0.10, 0.45, 0.60, 1.0");
        m_pad_nodes.push_back(p);
        parkNode(plate); for (ETCS::RID r : p.arrow) parkNode(r);
    }
    void AddSpringNode(ETCS::RID plate, ETCS::RID a, ETCS::RID b, ETCS::RID c, ETCS::RID d)
    {
        std::lock_guard<std::mutex> g(m_mu);
        SpringNodes p; p.plate = plate; p.arrow[0] = a; p.arrow[1] = b; p.arrow[2] = c; p.arrow[3] = d;
        const float r = GolfCourse::SpringRadius().ToFloat() * 2.0f;
        verb(plate, "Create", f3(r, 0.05f, r));
        verb(plate, "SetColor", "0.75, 0.30, 0.55, 1.0");
        m_spring_nodes.push_back(p);
        parkNode(plate); for (ETCS::RID r2 : p.arrow) parkNode(r2);
    }

    // ── the courses: data (GolfCourse.h), each of the latest course/hole ──

    void AddCourse(const std::string& name)
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (name.empty()) return;
        GolfCourse::Course c; c.name = name;
        m_defs.push_back(c);
    }
    // The latest course's look: its turf, walls, sand and sky (rgb each).
    void SetTheme(const float c[12])
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (m_defs.empty()) return;
        GolfCourse::Theme& t = m_defs.back().theme;
        for (int k = 0; k < 3; ++k) { t.turf[k] = c[k]; t.wall[k] = c[3 + k]; t.sand[k] = c[6 + k]; t.sky[k] = c[9 + k]; }
    }
    // A course made from a seed each time it is chosen (GolfCourse::Generate).
    void AddRandomCourse(const std::string& name)
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (name.empty()) return;
        GolfCourse::Course c; c.name = name; c.generated = true;
        m_defs.push_back(c);
    }
    void AddHole(uint32_t par)
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (m_defs.empty() || m_defs.back().generated) return;
        settleLastLocked();
        GolfCourse::Hole h; h.par = par ? static_cast<int>(par) : 3;
        m_defs.back().holes.push_back(h);
    }
    // A slab of the latest hole: its middle, its size, its tilt about its own
    // x and its yaw about +y (degrees) -- a green, a wall, a patch of sand.
    void AddGreen(float x, float y, float z, float w, float h, float d, float tilt, float yaw) { addSlab(x, y, z, w, h, d, tilt, yaw, GolfCourse::Green); }
    void AddWall(float x, float y, float z, float w, float h, float d, float tilt, float yaw)  { addSlab(x, y, z, w, h, d, tilt, yaw, GolfCourse::Wall); }
    void AddSand(float x, float y, float z, float w, float h, float d, float tilt, float yaw)  { addSlab(x, y, z, w, h, d, tilt, yaw, GolfCourse::Sand); }
    // The cup in the level green there (which is cut round it), and the tee.
    void AddCup(float x, float z)
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (GolfCourse::Hole* h = lastHoleLocked()) { h->cup = true; h->cup_x = Fixed::From(x); h->cup_z = Fixed::From(z); }
    }
    void AddTee(float x, float z)
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (GolfCourse::Hole* h = lastHoleLocked()) { h->tee_x = Fixed::From(x); h->tee_z = Fixed::From(z); }
    }
    // A pad pushing the way `yaw` says (degrees: 0 toward +z, 90 toward +x), a
    // spring, a spot for a power: each on whatever is highest there.
    void AddPad(float x, float z, float yaw)
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (GolfCourse::Hole* h = lastHoleLocked())
        {
            GolfCourse::Pad p; p.x = Fixed::From(x); p.z = Fixed::From(z); p.yaw = static_cast<int>(std::lround(yaw));
            h->pads.push_back(p);
        }
    }
    void AddSpring(float x, float z)
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (GolfCourse::Hole* h = lastHoleLocked()) h->springs.push_back({ Fixed::From(x), Fixed::From(z), Fixed::Zero() });
    }
    void AddSpot(float x, float z)
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (GolfCourse::Hole* h = lastHoleLocked()) h->pickups.push_back({ Fixed::From(x), Fixed::From(z), Fixed::Zero() });
    }

    // The courses are made: the first out, the lobby open (KartBattle::Ready's reason).
    void Ready()
    {
        std::lock_guard<std::mutex> g(m_mu);
        settleLastLocked();
        m_ready = true;
        courseLocked(0, firstLayout(0));
        holeLocked(0);
        readyLocked();   // a guest's lines that came before the courses were made (Trace)
    }

    // ── sessions (the families'); a match adds its hall line and its note ──

    void Practice(const std::string& name)
    {
        Absent();
        LockstepBase<GolfMatch>::Practice(name);
        std::lock_guard<std::mutex> g(m_mu); m_note.clear();
    }
    bool Host(const std::string& name, ETCS::RID hall, const std::string& id)
    {
        Absent();
        if (!LockstepBase<GolfMatch>::Host(name)) return false;
        { std::lock_guard<std::mutex> g(m_mu); m_note.clear(); }
        Attend(hall, name, id);
        ETCS_LOG("GolfMatch", "hosting a match as " << name);
        Announce();
        return true;
    }
    bool Join(const std::string& name, const std::string& owner)
    {
        Absent();
        if (!LockstepBase<GolfMatch>::Join(name, owner)) return false;
        { std::lock_guard<std::mutex> g(m_mu); m_note.clear(); }
        ETCS_LOG("GolfMatch", "joined " << owner << "'s match as " << name);
        return true;
    }
    void Leave()
    {
        LockstepBase<GolfMatch>::Leave();
        Absent();
        std::lock_guard<std::mutex> g(m_mu); m_note.clear();
    }
    void Tick() { Announce(); }

    // ── the host's lobby and round, as lines ─────────────────────────────

    bool Configure(const std::string& course, uint32_t secs)
    {
        std::lock_guard<std::mutex> g(m_mu);
        const int c = courseIndex(course);
        if (!m_authority || !m_ready || c < 0) return false;
        return configLocked(c, secs, c != m_course);
    }
    bool Reroll()
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (!m_authority || !m_ready || m_defs.empty() || !m_defs[m_course].generated) return false;
        return configLocked(m_course, m_hole_secs, true);
    }
    bool StartRound()
    {
        std::lock_guard<std::mutex> g(m_mu);
        return m_authority && m_ready && takeLocked(m_me, "round " + std::to_string(rollLocked()));
    }
    bool Abandon()
    {
        std::lock_guard<std::mutex> g(m_mu);
        return m_authority && takeLocked(m_me, "abandon");
    }
    bool Give(const std::string& who, const std::string& power, const std::string& how)
    {
        std::lock_guard<std::mutex> g(m_mu);
        // Named as people name them; the line names the seat.
        const int i = slotNamedLocked(who);
        if (!m_authority || i < 0) return false;
        return takeLocked(m_me, "give " + m_slots[i].player + " " + power + (how.empty() ? "" : " " + how));
    }

    // ── what the families ask of a match ──────────────────────────────────

    bool ApplyConcrete(const std::string& author, const std::string& line) override { return applyLocked(author, line); }
    void RestartConcrete() override { resetLocked(replayingLocked()); }
    std::string HashConcrete() override { return hashLocked(); }
    bool ReadyConcrete() override { return m_ready; }   // no line before the courses are made (KartBattle's reason)
    void ResyncedConcrete(uint64_t at) override
    {
        ETCS_LOG("GolfMatch", "this match differed from the host's at tick " << at << " -- replayed, and the same again");
        noteLocked("the match differed from the host's -- put right");
    }
    void EndedConcrete() override { noteLocked("the match is over -- " + m_owner + " closed it"); }
    std::string OpeningConcrete() override { return "join"; }
    std::string LeavingConcrete() override { return "leave"; }
    void DiffersConcrete(uint64_t at, const std::string& mine, const std::string& theirs) override
    {
        ETCS_LOG("GolfMatch", "this match (" << mine << ") is not the host's (" << theirs << ") at tick " << at);
        noteLocked("the match differs from the host's at tick " + std::to_string(at));
    }
    void StepConcrete() override { stepLocked(); }
    Rate RateConcrete() const override { return { kHz, kMaxCatchUp, kBatch, kSnapEvery }; }
    std::string KindConcrete() const override { return "golf"; }
    std::string HallConcrete() override
    {
        std::lock_guard<std::mutex> g(m_mu);
        return std::to_string(playersLocked()) + " " + std::to_string(m_slots.size());
    }
    // A seat's stream opening and ending (Present); the owner's is the trace
    // this mirror follows, whose end EndedConcrete says.
    void ArrivedConcrete(const std::string& who) override { std::lock_guard<std::mutex> g(m_mu); noteLocked(labelLocked(who) + " is here"); }
    void DepartedConcrete(const std::string& who) override
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (who != m_owner) noteLocked(labelLocked(who) + " left");
    }

    // ── the hand: keys and the pointer (this runtime's, until it lets go) ──

    /*
     * A KEY (GLFW codes): E arms the power held (or puts it away), Tab holds
     * up the board, and the host's Enter starts a round from the lobby and
     * Esc abandons one -- each of those two a line.
     */
    void Key(uint16_t key, bool down)
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (key == 258) { m_board_held = down; return; }
        if (!down) return;
        // The lobby's menu has the keys while it is up (and a name being
        // typed has them all); M folds it away to knock a ball about.
        if (key == kSettingsKey && m_menu_rid) { m_settings = !m_settings; m_editing = false; return; }
        if (m_settings && settingsKeyLocked(key)) return;
        if (m_phase == Lobby && m_menu_rid && !m_me.empty() && m_ready && menuKeyLocked(key)) return;
        if (key == 'E' || key == 'e')
        {
            const int i = slotOf(m_me);
            if (i >= 0 && m_slots[i].power != GolfPowers::None) m_armed = !m_armed;
            else m_armed = false;
            if (!m_armed) hideGhostLocked();
            m_card_text.clear();
            return;
        }
        if (m_authority && m_ready)
        {
            if (key == 257 && m_phase == Lobby) takeLocked(m_me, "round " + std::to_string(rollLocked()));
            else if (key == 256 && m_phase != Lobby) takeLocked(m_me, "abandon");
        }
    }
    void Pointer(const InputEvent& ev)
    {
        std::lock_guard<std::mutex> g(m_mu);
        const float x = ev.x, y = ev.y;
        switch (ev.action)
        {
            case INPUT_BUTTON_DOWN:
                // The right button looks round, with the left held or not.
                if (ev.key == kLookButton) { m_right = true; m_last_x = x; m_last_y = y; break; }
                if (ev.key != 0 || m_mode != Idle) break;
                // A press on the menu is the menu's: nothing behind it is pulled or turned.
                if (menuShownLocked() && menuPressLocked(static_cast<int>(x), static_cast<int>(y))) { m_mode = Down; break; }
                m_last_x = x; m_last_y = y;
                m_pull_len = Fixed::Zero();
                if (m_armed && heldPowerLocked() != GolfPowers::None) { m_mode = Placing; ghostLocked(x, y); }
                else if (onMyBallLocked(x, y) && canShootLocked(slotOf(m_me))) { m_mode = Pulling; m_pull_pitch = m_pitch; }
                else m_mode = Orbit;
                break;
            case INPUT_MOTION:
            {
                // A release seen by someone else (let go outside the window):
                // the platform says the button is up. A pull or a placement
                // ended unseen is put down, not used -- where is not known.
                if (m_right && input_button_state(ev, kLookButton) == 0) { m_right = false; putDownLocked(); }
                if (m_mode != Idle && input_button_state(ev, 0) == 0) { if (m_mode != Down) putDownLocked(); m_mode = Idle; }
                if (m_right || m_mode == Orbit)
                {
                    m_yaw  += (x - m_last_x) * kTurn * m_look_speed;
                    m_pitch = clampf(m_pitch + (y - m_last_y) * kTurn * m_look_speed, kMinPitch, kMaxPitch);
                    m_follow = true;
                    placeCameraLocked();
                }
                if (m_mode == Pulling) pullLocked(x, y);
                else if (m_mode == Placing) ghostLocked(x, y);
                else if (m_mode == Idle && menuShownLocked()) verb(m_menu_rid, "Hover", std::to_string(static_cast<int>(x)) + ", " + std::to_string(static_cast<int>(y)));
                m_last_x = x; m_last_y = y;
                break;
            }
            case INPUT_BUTTON_UP:
                if (ev.key == kLookButton)
                {
                    // Letting the right go puts down what the left holds.
                    m_right = false;
                    if (m_mode == Pulling || m_mode == Placing) { putDownLocked(); m_mode = Down; }
                    break;
                }
                if (ev.key != 0) break;
                if (m_mode == Pulling) shootLocked();
                else if (m_mode == Placing) placeLocked();
                m_mode = Idle;
                break;
            case INPUT_SCROLL:
                m_dist = clampf(m_dist * std::exp(-0.1f * m_zoom_speed * static_cast<float>(ev.y)), kMinDist, kMaxDist);
                m_follow = true;
                placeCameraLocked();
                break;
            default: break;
        }
    }
    // The pointer's events as verbs (window pixels): a stroke played from a
    // script or a test, as GolfGame's were.
    void Press(float x, float y)        { Pointer(event(INPUT_BUTTON_DOWN, 0, x, y)); }
    void Drag(float x, float y)         { Pointer(event(INPUT_MOTION, 0, x, y)); }
    void Release(float x, float y)      { Pointer(event(INPUT_BUTTON_UP, 0, x, y)); }
    void RightPress(float x, float y)   { Pointer(event(INPUT_BUTTON_DOWN, kLookButton, x, y)); }
    // A drag with both held: the look turned while the left holds a pull (its loft).
    void RightDrag(float x, float y)    { InputEvent e = event(INPUT_MOTION, 0, x, y); e.buttons |= input_button_bit(kLookButton); Pointer(e); }
    void RightRelease(float x, float y) { Pointer(event(INPUT_BUTTON_UP, kLookButton, x, y)); }
    void Wheel(float notches)           { Pointer(event(INPUT_SCROLL, 0, 0.0f, notches)); }
    // The lobby from anywhere: a press on the drawn menu (window pixels), or
    // this runtime's name and colour said outright (a page's controls) --
    // each a `look` line or a setting, the same state whichever way it came.
    void MenuPress(float x, float y)
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (menuShownLocked()) menuPressLocked(static_cast<int>(x), static_cast<int>(y));
    }
    bool Rename(const std::string& name)
    {
        std::lock_guard<std::mutex> g(m_mu);
        const int i = slotOf(m_me);
        // No name is the seat's label again.
        if (i < 0 || m_phase != Lobby || (!name.empty() && (!validName(name) || nameTakenLocked(i, name)))) return false;
        proposeLocked("look =" + (name.empty() ? std::string() : " " + name));
        return true;
    }
    bool Recolour(int colour)
    {
        std::lock_guard<std::mutex> g(m_mu);
        const int i = slotOf(m_me);
        if (i < 0 || m_phase != Lobby || colour < -1 || colour >= kPaletteN) return false;
        proposeLocked("look " + std::to_string(colour) + " =");
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
        return std::to_string(s.colour >= 0 ? s.colour : i % static_cast<int>(kMaxBalls)) + " " + nameOfLocked(static_cast<size_t>(i)) + " " + pal;
    }
    // A shot or a power stated outright, as this runtime's line: what the
    // pointer's release says, for a test or a script.
    void Shoot(float x, float y, float z)
    {
        std::lock_guard<std::mutex> g(m_mu);
        proposeLocked("shot " + milli(x) + " " + milli(y) + " " + milli(z));
    }
    void Use(float x, float y, float z, float yaw)
    {
        std::lock_guard<std::mutex> g(m_mu);
        proposeLocked("use " + milli(x) + " " + milli(y) + " " + milli(z) + " " + std::to_string(static_cast<int>(std::lround(yaw))));
    }
    // The camera's distance from the ball and its bearing and pitch, in degrees.
    void SetOrbit(float distance, float yaw_deg, float pitch_deg)
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (distance > 0.0f) m_dist = clampf(distance, kMinDist, kMaxDist);
        m_yaw   = yaw_deg * kPi / 180.0f;
        m_pitch = clampf(pitch_deg * kPi / 180.0f, kMinPitch, kMaxPitch);
        m_follow = true;
    }
    void Frame(float ms) { AdvanceConcrete(static_cast<double>(ms)); }

    // ── reads ────────────────────────────────────────────────────────────

    // "<tick> <lines> <mode> <me> <slot> <phase> <secs left> <course> <hole> <holes> <hole secs> <layout>"
    // mode: host | guest | practice | idle; phase: lobby | playing | between | results.
    std::string Status()
    {
        std::lock_guard<std::mutex> g(m_mu);
        const char* mode = m_me.empty() ? "idle" : !m_session ? "practice" : m_authority ? "host" : "guest";
        return std::to_string(m_tick) + " " + std::to_string(m_lines.size()) + " " + mode + " " + (m_me.empty() ? "-" : labelLocked(m_me)) + " "
             + std::to_string(slotOf(m_me)) + " " + phaseName() + " " + std::to_string(secsLeft()) + " " + courseName(m_course) + " "
             + std::to_string(m_hole + 1) + " " + std::to_string(m_live.holes.size()) + " " + std::to_string(m_hole_secs) + " "
             + std::to_string(m_layout);
    }
    // One line per player, best first: "<slot> <name> <total> <on this hole> <holed|out|playing> <power>".
    std::string Scores()
    {
        std::lock_guard<std::mutex> g(m_mu);
        std::string out;
        for (size_t i : rankLocked())
        {
            const Slot& s = m_slots[i];
            out += std::to_string(i) + " " + nameOfLocked(i) + " " + std::to_string(s.total) + " " + std::to_string(s.strokes) + " "
                 + (s.holed ? "holed" : s.done ? "out" : "playing") + " " + GolfPowers::Name(s.power) + "\n";
        }
        return out;
    }
    std::string Maps()
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (!m_ready) return std::string();
        std::string out;
        for (const auto& c : m_defs) out += c.name + "\n";
        return out;
    }
    // "<course> <layout> <hole> <slabs> <pads> <springs> <spots> <digest>" -- the hole out.
    std::string Layout()
    {
        std::lock_guard<std::mutex> g(m_mu);
        const GolfCourse::Hole& h = m_play;
        std::string st;
        for (const auto& b : h.slabs) st += std::to_string(b.x.raw) + "," + std::to_string(b.y.raw) + "," + std::to_string(b.z.raw) + "," + std::to_string(b.kind) + ";";
        char d[17];
        std::snprintf(d, sizeof d, "%016llx", static_cast<unsigned long long>(XXH3_64bits(st.data(), st.size())));
        return courseName(m_course) + " " + std::to_string(m_layout) + " " + std::to_string(m_hole + 1) + " " + std::to_string(h.slabs.size()) + " "
             + std::to_string(h.pads.size()) + " " + std::to_string(h.springs.size()) + " " + std::to_string(h.pickups.size()) + " " + d;
    }
    // Every hand-made course's faults and the course out's, "none" when nothing.
    std::string Faults()
    {
        std::lock_guard<std::mutex> g(m_mu);
        std::string out;
        for (const auto& c : m_defs)
            if (!c.generated) { const std::string f = GolfCourse::Faults(c); if (f != "none") out += (out.empty() ? "" : "; ") + f; }
        if (!m_defs.empty() && m_defs[m_course].generated) { const std::string f = GolfCourse::Faults(m_live); if (f != "none") out += (out.empty() ? "" : "; ") + f; }
        if (m_slab_nodes.size() < maxSlabsLocked()) out += std::string(out.empty() ? "" : "; ") + "slab pool short: " + std::to_string(m_slab_nodes.size()) + " for " + std::to_string(maxSlabsLocked());
        return out.empty() ? "none" : out;
    }
    std::string Hash() { std::lock_guard<std::mutex> g(m_mu); return std::to_string(m_tick) + " " + hashLocked(); }
    std::string Note() { std::lock_guard<std::mutex> g(m_mu); return m_note; }
    // "<slot> <x> <y> <z> <speed> <strokes> <total> <holed|out|playing> <power> <resting|moving> <spare>", or "-".
    std::string Ball(const std::string& who)
    {
        std::lock_guard<std::mutex> g(m_mu);
        const int i = slotNamedLocked(who.empty() ? m_me : who);
        if (i < 0) return "-";
        const Slot& s = m_slots[i];
        Fixed x, y, z, v;
        ballAt(s, x, y, z, &v);
        return std::to_string(i) + " " + fmt3(x.ToFloat()) + " " + fmt3(y.ToFloat()) + " " + fmt3(z.ToFloat()) + " " + fmt3(v.ToFloat()) + " "
             + std::to_string(s.strokes) + " " + std::to_string(s.total) + " " + (s.holed ? "holed" : s.done ? "out" : "playing") + " "
             + GolfPowers::Name(s.power) + " " + (restingLocked(i) ? "resting" : "moving") + " " + GolfPowers::Name(s.spare);
    }
    // "x y z length mode": the pull as it stands (mode idle | orbiting | pulling | placing | down).
    std::string Pull()
    {
        std::lock_guard<std::mutex> g(m_mu);
        static const char* modes[] = { "idle", "orbiting", "pulling", "placing", "down" };
        return fmt3(m_pull[0].ToFloat()) + " " + fmt3(m_pull[1].ToFloat()) + " " + fmt3(m_pull[2].ToFloat()) + " " + fmt3(m_pull_len.ToFloat()) + " "
             + modes[m_mode] + (m_armed ? " armed" : "");
    }

    // ── Animated: the frame edge ────────────────────────────────────────

    bool AnimatingConcrete() override { return m_world != 0; }
    void AdvanceConcrete(double dt_ms) override
    {
        std::lock_guard<std::mutex> g(m_mu);
        // The host's clock is the match's (Lockstep): on a guest, nothing.
        clockLocked(dt_ms);
        cameraLocked();
        shadowsLocked();
        cardLocked(dt_ms);
        boardLocked();
        menuLocked();
        fpsLocked(dt_ms);
    }

private:
    enum Phase { Lobby, Playing, Between, Results };
    enum Mode  { Idle, Orbit, Pulling, Placing, Down };   // Down: put down by the right's release, until the left lets go

    struct Slot
    {
        ETCS::RID   ball = 0, shadow = 0;
        OrderVector pristine;                 // the rows as made: a ball put anywhere starts from these
        std::string player;
        int         colour = -1;              // its `look`: a place in the palette, -1 its own
        std::string name;                     // its `look`: shown for it; empty, its label
        int         strokes = 0, total = 0;   // on this hole; over the holes done
        int         last_total = -1;          // the last round completed's, for the lobby's board
        bool        holed = false, done = false, out = true;   // out: off the course (parked)
        int64_t     park_at = -1;             // the tick a holed or finished ball leaves the course
        Fixed       fx, fy, fz;               // where its last shot was struck from: a lost ball goes back there
        uint8_t     power = GolfPowers::None, spare = GolfPowers::None;   // the one in hand, and the next (GolfPowers.h)
        bool        shadow_shown = false;     // drawn, not state
    };
    struct PadNodes    { ETCS::RID plate = 0; ETCS::RID arrow[3] = { 0, 0, 0 }; };
    struct SpringNodes { ETCS::RID plate = 0; ETCS::RID arrow[4] = { 0, 0, 0, 0 }; };
    struct BoardRow    { ETCS::RID rid; std::string text; bool shown; };
    static constexpr int kEffectPuffs = 10;
    // A power at work: a black hole, a wind, a tornado (and what each draws).
    struct Effect
    {
        uint8_t kind = 0; int owner = -1;
        Fixed   x, y, z, sx, cz, vx, vz;
        int64_t until = 0, next = 0;
        int     puffs[kEffectPuffs];       // drawn: a smoke's ring is all of them, the rest use six
        int     said = -1;                 // drawn: the step of its look last said
        Effect() { for (int& p : puffs) p = -1; }
    };
    // A lightning strike's picture: a bolt, a flash and its smoke (drawn only).
    struct Strike { Fixed x, y, z; int64_t at = 0; int puffs[4] = { -1, -1, -1, -1 }; };

    static constexpr size_t   kMaxBalls   = 8;
    static constexpr int      kHz         = 50;
    static constexpr int      kMaxCatchUp = 5;
    static constexpr uint64_t kBatch      = 2;
    static constexpr uint64_t kSnapEvery  = 250;
    static constexpr int      kCap        = 8;          // strokes on a hole before a ball at rest is out (scored kCap + 1)
    static constexpr int      kBetween    = 4 * kHz;    // a hole's scores up before the next
    static constexpr int      kResults    = 8 * kHz;
    static constexpr int      kParkAfter  = 1 * kHz;    // a holed ball stays in the cup this long
    static constexpr int      kSpotBack   = 8 * kHz;
    static constexpr uint16_t kLookButton = 1;          // the right button
    static constexpr float    kPi       = 3.14159265358979f;
    static constexpr float    kTurn     = 0.006f;       // radians a pixel of a look
    static constexpr uint16_t kSettingsKey = 'O';        // opens and shuts the settings
    static constexpr float    kMinPitch = 0.05f, kMaxPitch = 1.45f;
    static constexpr float    kMinDist  = 2.0f,  kMaxDist  = 60.0f;
    static constexpr float    kShadowLift = 0.035f;     // a shadow's middle over its surface
    static constexpr const float kColors[kMaxBalls][3] = {
        { 0.97f, 0.97f, 0.95f }, { 0.95f, 0.30f, 0.25f }, { 0.25f, 0.55f, 0.95f }, { 0.98f, 0.82f, 0.20f },
        { 0.65f, 0.35f, 0.90f }, { 0.25f, 0.85f, 0.55f }, { 0.98f, 0.55f, 0.15f }, { 0.20f, 0.20f, 0.22f } };
    // The palette a `look` picks from: the balls' own eight, then two more.
    static constexpr int kPaletteN = 10;
    static constexpr const float kPalette[kPaletteN][3] = {
        { 0.97f, 0.97f, 0.95f }, { 0.95f, 0.30f, 0.25f }, { 0.25f, 0.55f, 0.95f }, { 0.98f, 0.82f, 0.20f },
        { 0.65f, 0.35f, 0.90f }, { 0.25f, 0.85f, 0.55f }, { 0.98f, 0.55f, 0.15f }, { 0.20f, 0.20f, 0.22f },
        { 0.96f, 0.52f, 0.72f }, { 0.15f, 0.75f, 0.78f } };

    static Fixed F(int64_t n, int64_t d = 1) { return Fixed::FromInt(n) / Fixed::FromInt(d); }
    static Fixed dt()          { return F(1, kHz); }
    static Fixed ballR()       { return F(15, 100); }
    static Fixed minPull()     { return F(5, 100); }
    static Fixed restSpeed()   { return F(5, 100); }
    static Fixed touching()    { return F(4, 100); }   // a ball this close over its surface is on it
    // A pad: padPush() (u/s/s) whatever the ball is doing and padGain() times
    // its speed that way on top, to padTop() -- the kart's booster, sized for a
    // ball whose hardest shot is 12 (the course's light is 16).
    static Fixed padPush()     { return F(20); }
    static Fixed padGain()     { return F(2); }
    static Fixed padTop()      { return F(14); }
    static Fixed springKick(Fixed speed) { return F(6) + speed * Fixed::Half(); }
    static Fixed sandDrag()    { return F(5); }        // of the kinetic share, a second
    static constexpr int padBeat() { return 40; }
    static Fixed placeReach()  { return F(30); }      // a power goes down at most this far from its user's ball
    static Fixed teleportReach() { return F(10); }
    static Fixed holeR()       { return F(8); }       // a black hole's reach, and its pull at the middle
    static Fixed holePull()    { return F(28); }
    static Fixed windPush()    { return F(5); }
    static Fixed twisterR()    { return F(4); }
    static Fixed twisterWander() { return F(3, 2); }
    static Fixed strikeEnergy() { return F(2400); }   // a lightning burst's, spread over the sphere it reaches
    static Fixed strikeReach() { return F(10); }
    static Fixed strikeMost()  { return F(40); }      // the most a ball takes of one
    static Fixed strikeAround() { return F(6); }      // how far from the tornado it strikes
    static Fixed magnetR()     { return F(10); }      // how far the cup's pull reaches
    static Fixed magnetPull()  { return F(5); }
    static Fixed magnetDrag()  { return F(4); }
    static Fixed cannonReach() { return F(20); }      // 14 a second at 45 degrees: under the world's light
    static Fixed freezeDrag()  { return F(2); }       // of the kinetic share, a second: a shot carries about a third as far

    static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
    static InputEvent event(uint8_t action, uint16_t key, float x, float y)
    {
        InputEvent e{};
        e.action = action; e.key = key;
        e.x = static_cast<int16_t>(x); e.y = static_cast<int16_t>(y);
        e.buttons = INPUT_BUTTONS_STATED | (action == INPUT_BUTTON_UP ? 0 : input_button_bit(key));
        return e;
    }

    // ── the trace ───────────────────────────────────────────────────────

    bool configLocked(int c, uint32_t secs, bool reroll)
    {
        std::string line = "config " + std::to_string(c) + " " + std::to_string(secs);
        if (m_defs[c].generated) line += " " + std::to_string(reroll || c != m_course || !m_layout ? rollLocked() : m_layout);
        return takeLocked(m_me, line);
    }
    uint64_t rollLocked()
    {
        std::random_device rd;
        return ((static_cast<uint64_t>(rd()) << 32) ^ rd() ^ m_tick) | 1;
    }
    uint64_t diceLocked()
    {
        m_rng += 0x9E3779B97F4A7C15ull;
        uint64_t z = m_rng;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    static Fixed fromMilli(int64_t v) { return Fixed::FromInt(v) / Fixed::FromInt(1000); }
    static std::string milli(float v) { return std::to_string(static_cast<long long>(std::llround(static_cast<double>(v) * 1000.0))); }

    // One line, as its author (a `tick` is Lockstep's, before it gets here).
    bool applyLocked(const std::string& author, const std::string& line)
    {
        std::istringstream in(line);
        std::string verb;
        in >> verb;
        const bool host = author == m_owner;
        if (verb == "join")
        {
            if (slotOf(author) >= 0) return false;
            for (size_t i = 0; i < m_slots.size(); ++i)
                if (m_slots[i].player.empty())
                {
                    Slot& s = m_slots[i];
                    s.player = author;
                    s.strokes = 0; s.power = s.spare = GolfPowers::None; s.last_total = -1;
                    // In a round already: from the next hole's tee at the
                    // worst total so far, so a late start is not a lead.
                    s.total = m_phase == Lobby ? 0 : worstTotalLocked(i);
                    s.holed = s.done = false;
                    if (m_phase == Lobby || m_phase == Playing) teeBallLocked(i);
                    else { s.done = true; parkBallLocked(i); }
                    noteLocked(labelLocked(author) + " plays ball " + std::to_string(i + 1));
                    return true;
                }
            return false;   // every ball taken: watching
        }
        if (verb == "shot")
        {
            const int i = slotOf(author);
            int64_t x = 0, y = 0, z = 0;
            if (i < 0 || !(in >> x >> y >> z) || !canShootLocked(i)) return false;
            return shootBallLocked(static_cast<size_t>(i), fromMilli(x), fromMilli(y), fromMilli(z));
        }
        if (verb == "use")
        {
            const int i = slotOf(author);
            int64_t x = 0, y = 0, z = 0; int yaw = 0;
            if (i < 0 || !(in >> x >> y >> z >> yaw)) return false;
            return usePowerLocked(static_cast<size_t>(i), fromMilli(x), fromMilli(y), fromMilli(z), yaw);
        }
        if (verb == "leave")
        {
            const int i = slotOf(author);
            if (i < 0) return false;
            Slot& s = m_slots[i];
            s.player.clear(); s.strokes = s.total = 0; s.last_total = -1; s.power = s.spare = GolfPowers::None;
            s.holed = s.done = false;
            s.colour = -1; s.name.clear();
            paintBallLocked(static_cast<size_t>(i));
            parkBallLocked(static_cast<size_t>(i));
            return true;
        }
        if (verb == "look")
        {
            // Each player's own, and the lobby's only, as the round's
            // settings are: `look <colour|=> [<name>|=]`, a colour of the
            // palette (-1 the ball's own) and a name no other player has --
            // each said, or kept ("="), so a colour and a name changed one
            // after the other do not undo each other. No name is the label.
            const int i = slotOf(author);
            std::string colour, name;
            if (i < 0 || m_phase != Lobby || !(in >> colour)) return false;
            Slot& s = m_slots[static_cast<size_t>(i)];
            int c = s.colour;
            if (colour != "=") { char* end = nullptr; c = static_cast<int>(std::strtol(colour.c_str(), &end, 10)); if (!end || *end || c < -1 || c >= kPaletteN) return false; }
            in >> name;
            if (name == "=") name = s.name;
            else if (!name.empty() && (!validName(name) || nameTakenLocked(i, name))) return false;
            s.colour = c; s.name = name;
            paintBallLocked(static_cast<size_t>(i));
            return true;
        }
        if (!host) return false;   // the rest are the host's
        if (verb == "config")
        {
            int c = -1; uint32_t secs = 0; uint64_t layout = 0;
            if (m_phase != Lobby || !(in >> c >> secs) || c < 0 || c >= static_cast<int>(m_defs.size()) || secs < 30 || secs > 600) return false;
            in >> layout;
            if (m_defs[c].generated != (layout != 0)) return false;
            m_hole_secs = secs;
            courseLocked(c, layout);
            holeLocked(0);
            noteLocked("next round: " + courseName(m_course) + ", " + std::to_string(m_live.holes.size()) + " holes, " + clock(static_cast<int64_t>(secs) * kHz) + " a hole");
            return true;
        }
        if (verb == "round")
        {
            uint64_t seed = 0;
            if (m_phase != Lobby || !(in >> seed) || !seed || m_live.holes.empty()) return false;
            m_rng = seed;
            for (Slot& s : m_slots) { s.total = 0; s.strokes = 0; s.power = s.spare = GolfPowers::None; }
            m_phase = Playing;
            holeLocked(0);
            noteLocked("the round is on: " + courseName(m_course) + ", " + std::to_string(m_live.holes.size()) + " holes");
            return true;
        }
        if (verb == "abandon")
        {
            if (m_phase == Lobby) return false;
            lobbyLocked();
            noteLocked(labelLocked(author) + " abandoned the round");
            return true;
        }
        if (verb == "give")
        {
            // In hand outright -- or, said so, as the spare, or as a spot hands one over.
            std::string who, p, how;
            in >> who >> p >> how;
            const int i = slotNamedLocked(who);
            const uint8_t power = GolfPowers::ByName(p);
            if (i < 0 || power == GolfPowers::None || (!how.empty() && how != "spare" && how != "pickup")) return false;
            if (how == "pickup") armLocked(m_slots[i], power);
            else (how == "spare" ? m_slots[i].spare : m_slots[i].power) = power;
            return true;
        }
        return false;
    }

    // ── the course out, and the hole in play ────────────────────────────

    // Back to the start: every ball off the course, the lobby on the first
    // course's first hole, the dice as made -- what every session begins from.
    // `replay`: the trace is replayed next (Trace): this runtime's aim and menu are kept.
    void resetLocked(bool replay = false)
    {
        for (size_t i = 0; i < m_slots.size(); ++i)
        {
            Slot& s = m_slots[i];
            s.player.clear(); s.strokes = s.total = 0; s.last_total = -1; s.power = s.spare = GolfPowers::None; s.holed = s.done = false;
            s.colour = -1; s.name.clear();
            paintBallLocked(i);
            parkBallLocked(i);
        }
        m_phase = Lobby; m_end = 0; m_hole_secs = 120;
        m_rng = 0x9E3779B97F4A7C15ull;
        if (!replay)
        {
            m_armed = false; m_mode = Idle; m_right = false;
            m_menu = 0; m_editing = false; m_menu_open = true;
            hideArrowLocked(); hideGhostLocked();
        }
        if (!m_defs.empty() && m_ready) { courseLocked(0, firstLayout(0)); holeLocked(0); }
    }
    uint64_t firstLayout(int c) const { return c < static_cast<int>(m_defs.size()) && m_defs[c].generated ? 0x5EEDull + static_cast<uint64_t>(c) : 0; }

    void lobbyLocked()
    {
        m_phase = Lobby;
        m_end = 0;
        m_menu_open = true;
        holeLocked(0);
    }
    void courseLocked(int c, uint64_t layout)
    {
        if (c < 0 || c >= static_cast<int>(m_defs.size())) return;
        m_course = c;
        m_layout = m_defs[c].generated ? layout : 0;
        m_live = m_defs[c].generated ? GolfCourse::Generate(m_layout, m_defs[c].name) : m_defs[c];
    }
    // Hole n out: its pieces laid, its spots full, every player's ball on its
    // tee with nothing struck on it, the powers at work gone.
    void holeLocked(size_t n)
    {
        if (n >= m_live.holes.size()) return;
        m_hole = n;
        m_play = m_live.holes[n];
        m_course_pads = m_play.pads.size(); m_course_springs = m_play.springs.size(); m_course_slabs = m_play.slabs.size();
        m_spot_back.assign(m_play.pickups.size(), 0);
        endEffectsLocked();
        drawHoleLocked();
        m_end = m_phase == Playing ? static_cast<int64_t>(m_tick) + static_cast<int64_t>(m_hole_secs) * kHz : 0;
        for (size_t i = 0; i < m_slots.size(); ++i)
        {
            Slot& s = m_slots[i];
            s.strokes = 0; s.holed = false;
            if (s.player.empty()) { s.done = false; parkBallLocked(i); continue; }
            s.done = false;
            teeBallLocked(i);
        }
        m_follow = true;
    }
    // Every hole stated so far settled (GolfCourse::Settle): cut, and its
    // things on their surfaces. A hole is stated by the verbs after its
    // AddHole, so it is done when the next begins, or at Ready.
    void settleLastLocked()
    {
        for (GolfCourse::Course& c : m_defs)
            for (GolfCourse::Hole& h : c.holes)
                if (!h.settled) GolfCourse::Settle(h);
    }
    GolfCourse::Hole* lastHoleLocked()
    {
        if (m_defs.empty() || m_defs.back().generated || m_defs.back().holes.empty()) return nullptr;
        return &m_defs.back().holes.back();
    }
    void addSlab(float x, float y, float z, float w, float h, float d, float tilt, float yaw, int kind)
    {
        std::lock_guard<std::mutex> g(m_mu);
        GolfCourse::Hole* hole = lastHoleLocked();
        if (!hole || w <= 0 || h <= 0 || d <= 0) return;
        GolfCourse::Slab b = GolfCourse::Box(Fixed::From(x), Fixed::From(y), Fixed::From(z), Fixed::From(w), Fixed::From(h), Fixed::From(d), kind);
        GolfCourse::Degrees(Fixed::From(tilt), b.ts, b.tc);
        GolfCourse::Degrees(Fixed::From(yaw), b.ys, b.yc);
        if (tilt == 0.0f) { b.ts = Fixed::Zero(); b.tc = Fixed::One(); }
        if (yaw == 0.0f)  { b.ys = Fixed::Zero(); b.yc = Fixed::One(); }
        hole->slabs.push_back(b);
    }
    size_t maxSlabsLocked() const
    {
        size_t most = 0;
        for (const auto& c : m_defs) for (const auto& h : c.holes) most = std::max(most, h.slabs.size());
        return most;
    }

    // ── the balls ───────────────────────────────────────────────────────

    // A ball onto the course at (x, y, z), still, out of whatever held it.
    void putBallLocked(size_t i, Fixed x, Fixed y, Fixed z)
    {
        Slot& s = m_slots[i];
        Causal_* k = causal(s.ball);
        if (!k) return;
        // Out of the cup (or wherever fit put it) and back into the world: a
        // move, which emits, so by bare pointers used at once.
        if (Causal_* w = causal(m_world))
        {
            ETCS::Entity* p = static_cast<ETCS::Entity*>(k)->getParent();
            if (!p || p->getRID() != m_world) w->Contain(k);
        }
        if (s.out) { verb(s.ball, "SetAnchored", "0"); verb(s.ball, "SetVisible", "1"); s.out = false; }
        {
            std::lock_guard<std::recursive_mutex> lk(k->TreeMutex());
            OrderVector& r = k->RowsUnder();
            const uint64_t id = r.id;
            r = s.pristine;
            r.id = id;
            r.PlaceAt(x, y, z);
        }
        k->RowsWritten();
        s.fx = x; s.fy = y; s.fz = z;
        s.park_at = -1;
    }
    // On the hole's tee, side by side by slot, so no two start in each other.
    void teeBallLocked(size_t i)
    {
        const Fixed across = (F(static_cast<int64_t>(i)) - F(7, 2)) * F(2, 5);
        putBallLocked(i, m_play.tee_x + across, m_play.tee_y + ballR() + F(1, 200), m_play.tee_z);
    }
    // Off the course: held in a berth of its own, hidden.
    void parkBallLocked(size_t i)
    {
        Slot& s = m_slots[i];
        if (!s.out)
        {
            if (Causal_* k = causal(s.ball))
                if (Causal_* w = causal(m_world))
                {
                    ETCS::Entity* p = static_cast<ETCS::Entity*>(k)->getParent();
                    if (!p || p->getRID() != m_world) w->Contain(k);
                }
            verb(s.ball, "SetAnchored", "1");
            s.out = true;
        }
        parkNode(s.ball);
        hide(s.shadow); s.shadow_shown = false;
        s.park_at = -1;
    }
    bool ballAt(const Slot& s, Fixed& x, Fixed& y, Fixed& z, Fixed* speed = nullptr)
    {
        Causal_* k = causal(s.ball);
        if (!k) return false;
        std::lock_guard<std::recursive_mutex> lk(k->TreeMutex());
        const OrderVector& r = k->Order4();
        x = r.x; y = r.y; z = r.z;
        // In the cup: its frame's, as the world sees it.
        if (Causal_* in = k->Environment())
            if (in != causal(m_world)) { const OrderVector& c = in->Order4(); x += c.x; y += c.y; z += c.z; }
        if (speed) { Fixed vx, vy, vz; r.Velocity(k->MassUnder(), vx, vy, vz); *speed = Fixed::Length(vx, vy, vz); }
        return true;
    }
    bool restingLocked(int i)
    {
        if (i < 0 || m_slots[i].out) return false;
        Fixed x, y, z, v;
        return ballAt(m_slots[i], x, y, z, &v) && v < restSpeed();
    }
    // A shot is a ball at rest, on the course, not yet over its hole -- in a
    // round's hole or knocking about in the lobby.
    bool canShootLocked(int i)
    {
        if (i < 0 || (m_phase != Lobby && m_phase != Playing)) return false;
        const Slot& s = m_slots[i];
        return !s.player.empty() && !s.out && !s.done && !s.holed && restingLocked(i);
    }
    bool shootBallLocked(size_t i, Fixed px, Fixed py, Fixed pz)
    {
        Fixed len = Fixed::Length(px, py, pz);
        if (len < minPull()) return false;
        if (len > m_max_pull) { px = px * m_max_pull / len; py = py * m_max_pull / len; pz = pz * m_max_pull / len; len = m_max_pull; }
        Slot& s = m_slots[i];
        Causal_* k = causal(s.ball);
        if (!k) return false;
        Fixed x, y, z;
        ballAt(s, x, y, z);
        s.fx = x; s.fy = y; s.fz = z;
        {
            std::lock_guard<std::recursive_mutex> lk(k->TreeMutex());
            // Speed proportional to the pull; the impulse is its kinetic energy.
            const Fixed speed = m_max_speed * len / m_max_pull;
            k->RowsUnder().Impulse(px, py, pz, Fixed::Half() * k->MassUnder() * speed * speed);
        }
        k->RowsWritten();
        if (m_phase == Playing) ++s.strokes;
        return true;
    }
    int worstTotalLocked(size_t except) const
    {
        int worst = 0;
        for (size_t i = 0; i < m_slots.size(); ++i) if (i != except && !m_slots[i].player.empty()) worst = std::max(worst, m_slots[i].total);
        return worst;
    }
    // The hole over for ball i: scored, and off the course a moment later.
    void finishLocked(size_t i, int strokes, bool holed)
    {
        Slot& s = m_slots[i];
        s.done = true; s.holed = holed;
        if (m_phase == Playing) { s.strokes = strokes; s.total += strokes; }
        s.park_at = static_cast<int64_t>(m_tick) + kParkAfter;
        if (m_phase == Playing)
            noteLocked(nameOfLocked(i) + (holed ? " holed in " + std::to_string(strokes) : " is out: " + std::to_string(strokes)));
    }

    // ── one tick ────────────────────────────────────────────────────────

    void stepLocked()
    {
        Causal_* w = causal(m_world);
        if (!w) return;
        {
            std::lock_guard<std::recursive_mutex> lk(w->TreeMutex());
            for (size_t i = 0; i < m_slots.size(); ++i)
                if (!m_slots[i].out) { forcesLocked(i); if (Causal_* k = causal(m_slots[i].ball)) k->RowsWritten(); }
            w->Interact(dt());
            for (size_t i = 0; i < m_slots.size(); ++i)
                if (!m_slots[i].out && !m_slots[i].player.empty()) afterLocked(i);
            effectsLocked();
            arrowsLocked();
            puffsLocked();
        }
        phaseLocked();
    }

    // What the course and the powers do to a ball before the world steps it:
    // pads, springs and sand on the ground under it, and the powers at work.
    void forcesLocked(size_t i)
    {
        Causal_* k = causal(m_slots[i].ball);
        if (!k || k->Environment() != causal(m_world)) return;   // in the cup: nothing
        std::lock_guard<std::recursive_mutex> lk(k->TreeMutex());
        OrderVector& r = k->RowsUnder();
        const Fixed m = k->MassUnder();
        Fixed vx, vy, vz;
        r.Velocity(m, vx, vy, vz);
        Fixed ax = Fixed::Zero(), ay = Fixed::Zero(), az = Fixed::Zero();
        bool held = false;
        Fixed drag = Fixed::Zero();   // a power's, of the kinetic share a second
        Fixed top;
        const GolfCourse::Slab* on = GolfCourse::TopAt(m_play, r.x, r.z, top, r.y);
        const bool ground = on && r.y - ballR() - top < touching();
        if (ground)
        {
            for (const GolfCourse::Pad& p : m_play.pads)
            {
                const Fixed dx = r.x - p.x, dz = r.z - p.z;
                const Fixed u = dx * p.sx + dz * p.cz, v = dx * p.cz - dz * p.sx;
                if (u.Abs() > GolfCourse::PadLength() * Fixed::Half() || v.Abs() > GolfCourse::PadWidth() * Fixed::Half() || (top - p.y).Abs() > F(5, 100)) continue;
                const Fixed along = vx * p.sx + vz * p.cz;
                if (along >= padTop()) continue;
                const Fixed push = padPush() + (along.IsPositive() ? along * padGain() : Fixed::Zero());
                const Fixed most = (padTop() - along) / dt();
                const Fixed a = Fixed::Min(push, most);
                ax += p.sx * a; az += p.cz * a;
            }
            for (const GolfCourse::Spot& p : m_play.springs)
            {
                if (Fixed::Length(r.x - p.x, Fixed::Zero(), r.z - p.z) > GolfCourse::SpringRadius() || (top - p.y).Abs() > F(5, 100) || vy > Fixed::Half()) continue;
                const Fixed kick = springKick(Fixed::Length(vx, Fixed::Zero(), vz));
                ay += (kick - vy) / dt();
            }
        }
        for (Effect& e : m_effects)
        {
            const Fixed dx = e.x - r.x, dz = e.z - r.z, dy = e.y - r.y;
            if (e.kind == GolfPowers::BlackHole)
            {
                const Fixed d = Fixed::Length(dx, dy, dz);
                if (d >= holeR() || !d.IsPositive()) continue;
                const Fixed a = holePull() * (Fixed::One() - d / holeR());
                ax += dx / d * a; ay += dy / d * a; az += dz / d * a;
            }
            else if (e.kind == GolfPowers::Wind) { ax += e.sx * windPush(); az += e.cz * windPush(); }
            else if (e.kind == GolfPowers::Tornado)
            {
                const Fixed d = Fixed::Length(dx, Fixed::Zero(), dz);
                if (d >= twisterR() || !d.IsPositive()) continue;
                // Round it (anticlockwise from above), in toward it, and up near its middle.
                ax += (-dz / d) * F(9) + dx / d * F(5);
                az += ( dx / d) * F(9) + dz / d * F(5);
                if (d < F(3, 2)) ay += F(14);
            }
            else if (e.kind == GolfPowers::Magnet)
            {
                // The cup's pull on its user's ball alone, along the ground --
                // and a drag with it, and less of both close in: a ball
                // drawn to the cup comes to it slowly enough to drop.
                const Fixed d = Fixed::Length(dx, Fixed::Zero(), dz);
                if (static_cast<int>(i) != e.owner || d >= magnetR() || d < F(1, 10)) continue;
                const Fixed a = magnetPull() * Fixed::Max(F(2, 5), Fixed::Min(Fixed::One(), d / F(4)));
                ax += dx / d * a; az += dz / d * a;
                drag = magnetDrag();
            }
            else if (e.kind == GolfPowers::Cannon && static_cast<int>(i) == e.owner)
            {
                // Its ball comes down dead: on the first ground it touches
                // after the shot, most of its way is gone (and the power's work with it).
                const int64_t flown = static_cast<int64_t>(m_tick) - (e.until - GolfPowers::kCannonTicks);
                if (flown > 10 && ground) { drag = F(45); e.until = static_cast<int64_t>(m_tick) + 1; }
            }
            else if (e.kind == GolfPowers::Freeze && static_cast<int>(i) != e.owner) held = true;
        }
        if (!ax.IsZero() || !ay.IsZero() || !az.IsZero()) r.Accelerate(ax, ay, az, dt(), m);
        if (ground && on->kind == GolfCourse::Sand) r.Dissipate(Fixed::One() - sandDrag() * dt());
        if (held) r.Dissipate(Fixed::One() - freezeDrag() * dt());   // in the cold: everything it does is slowed, not stopped
        if (drag.IsPositive()) r.Dissipate(Fixed::One() - drag * dt());
    }

    // A power to a ball, of the two it carries (GolfPowers.h): in hand if
    // that is empty, else the spare -- in place of what the spare was.
    static void armLocked(Slot& s, uint8_t power)
    {
        (s.power == GolfPowers::None ? s.power : s.spare) = power;
    }
    // After the step: holed, lost, out of strokes, or through a spot.
    void afterLocked(size_t i)
    {
        Slot& s = m_slots[i];
        if (s.park_at >= 0)
        {
            if (static_cast<int64_t>(m_tick) < s.park_at) return;
            // In the lobby a holed ball goes back to the tee: knock it about again.
            if (m_phase == Lobby) { s.holed = s.done = false; teeBallLocked(i); }
            else parkBallLocked(i);
            return;
        }
        if (s.done) return;
        Causal_* k = causal(s.ball);
        if (!k) return;
        // Holed: fit took the ball into the cup's space.
        if (m_cup && k->Environment() == causal(m_cup)) { finishLocked(i, s.strokes, true); return; }
        Fixed x, y, z;
        ballAt(s, x, y, z);
        if (y < m_play.low)
        {
            // Lost: back where it was struck from, a stroke added.
            const Fixed fx = s.fx, fy = s.fy, fz = s.fz;
            putBallLocked(i, fx, fy, fz);
            if (m_phase == Playing) ++s.strokes;
            noteLocked(nameOfLocked(i) + "'s ball is lost: +1");
            return;
        }
        if (m_phase == Playing && s.strokes >= kCap && restingLocked(static_cast<int>(i))) { finishLocked(i, kCap + 1, false); return; }
        // A spot: a power -- in hand, or the spare (armLocked).
        for (size_t n = 0; n < m_play.pickups.size(); ++n)
        {
            const GolfCourse::Spot& p = m_play.pickups[n];
            if (m_spot_back[n] > static_cast<int64_t>(m_tick)) continue;
            if (Fixed::Length(x - p.x, Fixed::Zero(), z - p.z) > GolfCourse::SpotReach() || (y - p.y).Abs() > F(3, 2)) continue;
            armLocked(s, static_cast<uint8_t>(1 + diceLocked() % (GolfPowers::Count - 1)));
            m_spot_back[n] = static_cast<int64_t>(m_tick) + kSpotBack;
            hide(m_spot_nodes.size() > n ? m_spot_nodes[n] : 0);
            break;
        }
    }

    void phaseLocked()
    {
        const int64_t now = static_cast<int64_t>(m_tick);
        for (size_t n = 0; n < m_spot_back.size() && n < m_spot_nodes.size(); ++n)
        {
            if (m_spot_back[n] && m_spot_back[n] <= now) { m_spot_back[n] = 0; verb(m_spot_nodes[n], "SetVisible", "1"); }
            // Turning in place, as power-ups do (the kart's boxes the same).
            if (Causal_* c = m_spot_back[n] ? nullptr : causal(m_spot_nodes[n]))
            {
                std::lock_guard<std::recursive_mutex> lk(c->TreeMutex());
                c->RowsUnder().RotateBy(Fixed::Zero(), Fixed::One(), Fixed::Zero(), F(6, 100));
                c->RowsWritten();
            }
        }
        if (m_phase == Playing)
        {
            bool all = true, anyone = false;
            for (const Slot& s : m_slots) if (!s.player.empty()) { anyone = true; all = all && s.done; }
            if (now >= m_end)
            {
                // Time: whoever is still out is scored the most.
                for (size_t i = 0; i < m_slots.size(); ++i)
                    if (!m_slots[i].player.empty() && !m_slots[i].done) finishLocked(i, kCap + 1, false);
                all = true;
                noteLocked("time on hole " + std::to_string(m_hole + 1));
            }
            if (all && anyone) { m_phase = Between; m_end = now + kBetween; }
            else if (!anyone) { m_phase = Between; m_end = now + kBetween; }
        }
        else if (m_phase == Between && now >= m_end)
        {
            if (m_hole + 1 < m_live.holes.size()) { m_phase = Playing; holeLocked(m_hole + 1); }
            else
            {
                m_phase = Results;
                m_end = now + kResults;
                for (Slot& s : m_slots) s.last_total = s.player.empty() ? -1 : s.total;
                const std::vector<size_t> rank = rankLocked();
                const bool clear = !rank.empty() && (rank.size() == 1 || m_slots[rank[0]].total != m_slots[rank[1]].total);
                noteLocked(rank.empty() ? "the round is over" : clear ? "the round is over -- " + nameOfLocked(rank[0]) + " wins with " + std::to_string(m_slots[rank[0]].total)
                                                                     : std::string("the round is over -- a tie"));
            }
        }
        else if (m_phase == Results && now >= m_end) lobbyLocked();
    }

    // ── the powers ──────────────────────────────────────────────────────

    /*
     * A POWER PUT DOWN at (x, ?, z), as its user looked (yaw, degrees): on
     * whatever surface is highest there, no further than placeReach() from
     * the user's ball (a teleport: teleportReach()). False -- and the power
     * kept -- when there is nothing there, or the pools are spent.
     */
    bool usePowerLocked(size_t i, Fixed x, Fixed y, Fixed z, int yaw)
    {
        (void)y;
        Slot& s = m_slots[i];
        const uint8_t kind = s.power;
        if (kind == GolfPowers::None || s.player.empty() || (m_phase != Lobby && m_phase != Playing)) return false;
        Fixed bx, by, bz;
        if (s.out || !ballAt(s, bx, by, bz)) { bx = m_play.tee_x; by = m_play.tee_y; bz = m_play.tee_z; }
        const Fixed reach = kind == GolfPowers::Teleport ? teleportReach() : kind == GolfPowers::Cannon ? cannonReach() : placeReach();
        Fixed dx = x - bx, dz = z - bz;
        const Fixed d = Fixed::Length(dx, Fixed::Zero(), dz);
        if (d > reach) { x = bx + dx * reach / d; z = bz + dz * reach / d; }
        Fixed top;
        const GolfCourse::Slab* on = GolfCourse::TopAt(m_play, x, z, top);
        if (!on) return false;
        Fixed ys, yc;
        GolfCourse::Degrees(F(yaw), ys, yc);
        switch (kind)
        {
            case GolfPowers::Pad:
            {
                if (m_play.pads.size() >= m_pad_nodes.size()) return false;
                GolfCourse::Pad p; p.x = x; p.z = z; p.y = top; p.yaw = yaw; p.sx = ys; p.cz = yc;
                m_play.pads.push_back(p);
                drawPadLocked(m_play.pads.size() - 1);
                break;
            }
            case GolfPowers::Spring:
                if (m_play.springs.size() >= m_spring_nodes.size()) return false;
                m_play.springs.push_back({ x, z, top });
                drawSpringLocked(m_play.springs.size() - 1);
                break;
            case GolfPowers::Bumper:
            case GolfPowers::Sand:
            {
                if (m_play.slabs.size() >= m_slab_nodes.size()) return false;
                GolfCourse::Slab b = kind == GolfPowers::Bumper
                    ? GolfCourse::Box(x, top + F(2, 5), z, F(3), F(4, 5), F(1, 2), GolfCourse::Wall)
                    : GolfCourse::Box(x, top + F(3, 100), z, F(4), F(6, 100), F(4), GolfCourse::Sand);
                if (kind == GolfPowers::Bumper) { b.ys = ys; b.yc = yc; }   // its long side across the user's view
                m_play.slabs.push_back(b);
                drawSlabLocked(m_play.slabs.size() - 1);
                break;
            }
            case GolfPowers::Teleport:
                if (s.out || s.done || !restingLocked(static_cast<int>(i))) return false;
                putBallLocked(i, x, top + ballR() + F(1, 200), z);
                break;
            default:
            {
                if (kind == GolfPowers::Magnet && !m_play.cup) return false;
                if (kind == GolfPowers::Cannon)
                {
                    // From rest, at 45 degrees: over level ground it comes
                    // down `far` away at a speed whose square is g x far.
                    Causal_* k = causal(s.ball);
                    const Fixed far = Fixed::Length(x - bx, Fixed::Zero(), z - bz);
                    if (s.out || s.done || !k || !restingLocked(static_cast<int>(i)) || far < F(1)) return false;
                    const Fixed speed = (F(98, 10) * far).Sqrt();
                    std::lock_guard<std::recursive_mutex> lk(k->TreeMutex());
                    k->RowsUnder().Impulse((x - bx) / far, Fixed::One(), (z - bz) / far, Fixed::Half() * k->MassUnder() * speed * speed);
                    k->RowsWritten();
                    s.fx = bx; s.fy = by; s.fz = bz;   // lost from there, it goes back there
                }
                Effect e; e.kind = kind; e.owner = static_cast<int>(i);
                e.x = x; e.z = z; e.y = top + (kind == GolfPowers::BlackHole ? F(1, 2) : Fixed::Zero());
                e.sx = ys; e.cz = yc;
                const int64_t now = static_cast<int64_t>(m_tick);
                e.until = now + GolfPowers::Ticks(kind);
                e.next = now + GolfPowers::kStrikeEvery;
                if (kind == GolfPowers::Wind || kind == GolfPowers::Cannon) { e.x = bx; e.z = bz; e.y = by; }
                if (kind == GolfPowers::Magnet) { e.x = m_play.cup_x; e.z = m_play.cup_z; e.y = m_play.cup_y; }
                takePuffsLocked(e);
                m_effects.push_back(e);
                break;
            }
        }
        noteLocked(nameOfLocked(i) + " put down a " + GolfPowers::Name(kind));
        s.power = s.spare; s.spare = GolfPowers::None;   // used: the spare comes to hand
        return true;
    }

    // The powers at work, a tick on: the tornado wanders and strikes; what is
    // over is gone.
    void effectsLocked()
    {
        const int64_t now = static_cast<int64_t>(m_tick);
        for (Effect& e : m_effects)
        {
            if (e.kind != GolfPowers::Tornado) continue;
            // A new way every second, from the dice; held over the course.
            if ((now - e.until) % kHz == 0)
            {
                Fixed s, c;
                (Fixed::TwoPi() * F(static_cast<int64_t>(diceLocked() % 360), 360)).SinCos(s, c);
                e.vx = s * twisterWander(); e.vz = c * twisterWander();
            }
            Fixed nx = e.x + e.vx * dt(), nz = e.z + e.vz * dt(), top;
            if (GolfCourse::TopAt(m_play, nx, nz, top)) { e.x = nx; e.z = nz; e.y = top; }
            else { e.vx = -e.vx; e.vz = -e.vz; }
            if (now >= e.next)
            {
                e.next = now + GolfPowers::kStrikeEvery;
                Fixed s, c;
                (Fixed::TwoPi() * F(static_cast<int64_t>(diceLocked() % 360), 360)).SinCos(s, c);
                const Fixed out = strikeAround() * F(static_cast<int64_t>(diceLocked() % 100), 100);
                const Fixed sx = e.x + s * out, sz = e.z + c * out;
                Fixed sy;
                if (GolfCourse::TopAt(m_play, sx, sz, sy)) strikeLocked(sx, sy, sz);
            }
        }
        for (size_t n = 0; n < m_effects.size();)
        {
            if (m_effects[n].until > now) { ++n; continue; }
            freePuffsLocked(m_effects[n].puffs, kEffectPuffs);
            m_effects.erase(m_effects.begin() + static_cast<long>(n));
        }
    }

    /*
     * LIGHTNING: a burst at (x, y, z) whose energy spreads as it goes out, so
     * a ball d away intercepts its share of the sphere there -- the ball's
     * cross-section over the sphere's, r^2 / 4d^2 -- along the line from the
     * strike (and a little up), a quarter of it through a wall or a block.
     */
    void strikeLocked(Fixed x, Fixed y, Fixed z)
    {
        for (size_t i = 0; i < m_slots.size(); ++i)
        {
            Slot& s = m_slots[i];
            if (s.out || s.player.empty()) continue;
            Causal_* k = causal(s.ball);
            if (!k || k->Environment() != causal(m_world)) continue;
            std::lock_guard<std::recursive_mutex> lk(k->TreeMutex());
            OrderVector& r = k->RowsUnder();
            const Fixed dx = r.x - x, dy = r.y - y, dz = r.z - z;
            Fixed d = Fixed::Length(dx, dy, dz);
            if (d > strikeReach()) continue;
            if (d < F(1, 2)) d = F(1, 2);
            Fixed share = strikeEnergy() * ballR() * ballR() / (F(4) * d * d);
            if (share > strikeMost()) share = strikeMost();
            for (int t = 1; t < 5; ++t)
            {
                const Fixed f = F(t, 5);
                if (wallAt(x + dx * f, y + dy * f + F(1, 5), z + dz * f)) { share = share / F(4); break; }
            }
            r.Impulse(dx / d, dy / d + F(3, 10), dz / d, share);
            k->RowsWritten();
        }
        Strike st; st.x = x; st.y = y; st.z = z; st.at = static_cast<int64_t>(m_tick);
        drawStrikeLocked(st);
        m_strikes.push_back(st);
    }
    bool wallAt(Fixed x, Fixed y, Fixed z) const
    {
        for (const GolfCourse::Slab& b : m_play.slabs) if (b.kind == GolfCourse::Wall && GolfCourse::Inside(b, x, y, z)) return true;
        return false;
    }
    void endEffectsLocked()
    {
        for (Effect& e : m_effects) freePuffsLocked(e.puffs, kEffectPuffs);
        m_effects.clear();
        for (Strike& s : m_strikes) freePuffsLocked(s.puffs, 4);
        m_strikes.clear();
    }

    // ── drawing what the trace laid (the same on every runtime) ──────────

    // The sky and the cup's rim are the course's (its Theme): said when they change.
    void skyLocked()
    {
        const GolfCourse::Theme& t = m_live.theme;
        const std::string sky = f4(t.sky[0], t.sky[1], t.sky[2], 1.0f), turf = f4(t.turf[0], t.turf[1], t.turf[2], 1.0f);
        if (m_camera && sky != m_sky_said) { verb(m_camera, "SetBackground", sky); m_sky_said = sky; }
        if (m_cup_rim && turf != m_rim_said) { verb(m_cup_rim, "SetColor", turf); m_rim_said = turf; }
    }
    void drawHoleLocked()
    {
        verb(m_world, "Move", "0, 0, 0");
        skyLocked();
        for (size_t n = 0; n < m_play.slabs.size(); ++n) drawSlabLocked(n);
        for (size_t n = m_play.slabs.size(); n < m_slab_nodes.size(); ++n) parkNode(m_slab_nodes[n]);
        if (m_cup)
        {
            // The cup itself is never drawn (golf_cup.etcs): its floor and flag are.
            if (m_play.cup) placeNode(m_cup, m_play.cup_x, m_play.cup_y - GolfCourse::CupUnder(), m_play.cup_z);
            else parkNode(m_cup);
        }
        if (m_tee) { placeNode(m_tee, m_play.tee_x, m_play.tee_y + F(1, 100), m_play.tee_z); verb(m_tee, "SetVisible", "1"); }
        for (size_t n = 0; n < m_pad_nodes.size(); ++n)
            if (n < m_play.pads.size()) drawPadLocked(n);
            else { parkNode(m_pad_nodes[n].plate); for (ETCS::RID a : m_pad_nodes[n].arrow) parkNode(a); }
        for (size_t n = 0; n < m_spring_nodes.size(); ++n)
            if (n < m_play.springs.size()) drawSpringLocked(n);
            else { parkNode(m_spring_nodes[n].plate); for (ETCS::RID a : m_spring_nodes[n].arrow) parkNode(a); }
        for (size_t n = 0; n < m_spot_nodes.size(); ++n)
            if (n < m_play.pickups.size())
            {
                const GolfCourse::Spot& p = m_play.pickups[n];
                placeNode(m_spot_nodes[n], p.x, p.y + F(6, 10), p.z);
                verb(m_spot_nodes[n], "SetVisible", "1");
            }
            else parkNode(m_spot_nodes[n]);
    }
    void drawSlabLocked(size_t n)
    {
        if (n >= m_slab_nodes.size()) return;
        const GolfCourse::Slab& b = m_play.slabs[n];
        const ETCS::RID node = m_slab_nodes[n];
        verb(node, "Create", fmt3(b.w.ToFloat()) + ", " + fmt3(b.h.ToFloat()) + ", " + fmt3(b.d.ToFloat()));
        // The course's own colours (its Theme): turf, wall, sand.
        const GolfCourse::Theme& t = m_live.theme;
        const float* colour = b.kind == GolfCourse::Wall ? t.wall : b.kind == GolfCourse::Sand ? t.sand : t.turf;
        static const char* solid[]  = { "0.3, 0.1", "0.7, 0.05", "0.1, 0.6" };   // the green's give, a bumper's, sand's
        verb(node, "SetColor", f4(colour[0], colour[1], colour[2], 1.0f));
        verb(node, "SetSolid", solid[b.kind]);
        if (Causal_* c = causal(node))
        {
            std::lock_guard<std::recursive_mutex> lk(c->TreeMutex());
            OrderVector& r = c->RowsUnder();
            r.PlaceAt(b.x, b.y, b.z);
            GolfCourse::Turn(b, r);
            c->RowsWritten();
        }
        verb(node, "SetVisible", "1");
    }
    void drawPadLocked(size_t n)
    {
        if (n >= m_pad_nodes.size()) return;
        const GolfCourse::Pad& p = m_play.pads[n];
        poseNode(m_pad_nodes[n].plate, p.x, p.y + F(2, 100), p.z, p.yaw);
        verb(m_pad_nodes[n].plate, "SetVisible", "1");
        for (ETCS::RID a : m_pad_nodes[n].arrow) verb(a, "SetVisible", "1");
    }
    void drawSpringLocked(size_t n)
    {
        if (n >= m_spring_nodes.size()) return;
        const GolfCourse::Spot& p = m_play.springs[n];
        placeNode(m_spring_nodes[n].plate, p.x, p.y + F(25, 1000), p.z);
        verb(m_spring_nodes[n].plate, "SetVisible", "1");
        for (ETCS::RID a : m_spring_nodes[n].arrow) verb(a, "SetVisible", "1");
    }
    // The pads' arrows run along them like a belt; the springs' close on
    // their middles -- on the tick, so the same everywhere (drawn only).
    void arrowsLocked()
    {
        const size_t pads = std::min(m_pad_nodes.size(), m_play.pads.size());
        const size_t springs = std::min(m_spring_nodes.size(), m_play.springs.size());
        if (!pads && !springs) return;
        const Fixed run = GolfCourse::PadLength() - F(9, 10);
        const Fixed beat = F(static_cast<int64_t>(m_tick % static_cast<uint64_t>(padBeat())), padBeat());
        for (size_t n = 0; n < pads; ++n)
        {
            const GolfCourse::Pad& p = m_play.pads[n];
            for (int k = 0; k < 3; ++k)
            {
                const Fixed at = ((F(k) + beat) / F(3) - Fixed::Half()) * run;
                poseNode(m_pad_nodes[n].arrow[k], p.x + p.sx * at, p.y + F(6, 100), p.z + p.cz * at, p.yaw, true);
            }
        }
        static const int side[4][3] = { { 1, 0, 270 }, { 0, 1, 180 }, { -1, 0, 90 }, { 0, -1, 0 } };
        const Fixed out = F(115, 100) - F(65, 100) * beat;
        for (size_t n = 0; n < springs; ++n)
        {
            const GolfCourse::Spot& p = m_play.springs[n];
            for (int k = 0; k < 4; ++k)
                poseNode(m_spring_nodes[n].arrow[k], p.x + out * F(side[k][0]), p.y + F(6, 100), p.z + out * F(side[k][1]), side[k][2], true);
        }
        verb(m_world, "Move", "0, 0, 0");
    }

    // The powers' pictures: soft puffs, glowing or smoke, from one pool.
    int takePuffLocked(int64_t until)
    {
        for (size_t n = 0; n < m_puff_nodes.size(); ++n)
            if (m_puff_until[n] < 0) { m_puff_until[n] = until; return static_cast<int>(n); }
        return -1;
    }
    void freePuffsLocked(int* puffs, int count)
    {
        for (int k = 0; k < count; ++k)
            if (puffs[k] >= 0) { m_puff_until[puffs[k]] = -1; m_puff_size[puffs[k]].clear(); parkNode(m_puff_nodes[puffs[k]]); puffs[k] = -1; }
    }
    void puffLook(int n, const char* colour, const char* soft, const char* amount)
    {
        if (n < 0) return;
        const ETCS::RID r = m_puff_nodes[n];
        verb(r, "SetColor", colour);
        verb(r, std::string(soft) == "SetGlow" ? "SetSmoke" : "SetGlow", "0");
        verb(r, soft, amount);
        verb(r, "SetVisible", "1");
    }
    void takePuffsLocked(Effect& e)
    {
        const int count = e.kind == GolfPowers::BlackHole ? 2 : e.kind == GolfPowers::Cannon ? 3 : e.kind == GolfPowers::Smoke ? kEffectPuffs : 6;
        for (int k = 0; k < count; ++k) e.puffs[k] = takePuffLocked(e.until);
        if (e.kind == GolfPowers::BlackHole)
        {
            puffLook(e.puffs[0], "0.55, 0.25, 0.95, 1.0", "SetGlow", "1.4");
            puffLook(e.puffs[1], "0.02, 0.0, 0.05, 1.0", "SetSmoke", "0.85");
        }
        else if (e.kind == GolfPowers::Wind)
            for (int k = 0; k < count; ++k) puffLook(e.puffs[k], "0.85, 0.95, 1.0, 1.0", "SetGlow", "0.7");
        else if (e.kind == GolfPowers::Magnet)
            for (int k = 0; k < count; ++k) puffLook(e.puffs[k], k % 2 ? "0.95, 0.25, 0.25, 1.0" : "0.3, 0.5, 1.0, 1.0", "SetGlow", "1.1");
        else if (e.kind == GolfPowers::Freeze)
            for (int k = 0; k < count; ++k) puffLook(e.puffs[k], "0.75, 0.92, 1.0, 1.0", "SetSmoke", "0.45");
        else if (e.kind == GolfPowers::Smoke)
            for (int k = 0; k < count; ++k) puffLook(e.puffs[k], "0.5, 0.5, 0.53, 1.0", "SetSmoke", "0.95");
        else
            for (int k = 0; k < count; ++k) puffLook(e.puffs[k], "0.42, 0.42, 0.46, 1.0", "SetSmoke", "0.55");
    }
    void drawStrikeLocked(Strike& s)
    {
        const int64_t until = s.at + kHz;
        for (int k = 0; k < 4; ++k) s.puffs[k] = takePuffLocked(until);
        puffLook(s.puffs[0], "0.80, 0.90, 1.0, 1.0", "SetGlow", "2.5");     // the bolt
        puffLook(s.puffs[1], "1.0, 0.95, 0.70, 1.0", "SetGlow", "1.8");     // the flash
        puffLook(s.puffs[2], "0.30, 0.30, 0.32, 1.0", "SetSmoke", "0.6");   // its smoke
        puffLook(s.puffs[3], "0.35, 0.34, 0.36, 1.0", "SetSmoke", "0.5");
        if (s.puffs[0] >= 0) { resizePuff(s.puffs[0], "0.25, 14.0, 0.25"); placeNode(m_puff_nodes[s.puffs[0]], s.x, s.y + F(7), s.z); }
    }
    // Each tick: where every puff of every power is, and how big.
    void puffsLocked()
    {
        const int64_t now = static_cast<int64_t>(m_tick);
        bool any = false;
        for (Effect& e : m_effects)
        {
            any = true;
            const int64_t ticks = now - (e.until - GolfPowers::Ticks(e.kind));
            const Fixed age = F(ticks, kHz);
            Fixed s, c;
            if (e.kind == GolfPowers::BlackHole)
            {
                // Breathing, in tenths: a size is a verb, and it is there half a minute.
                (age * F(4)).SinCos(s, c);
                sizePuff(e.puffs[0], F(7, 10) + F((s * F(3, 2)).ToInt(), 10), e.x, e.y, e.z);
                sizePuff(e.puffs[1], F(22, 10) + F((c * F(7, 2)).ToInt(), 10), e.x, e.y, e.z);
            }
            else if (e.kind == GolfPowers::Wind)
                for (int k = 0; k < 6; ++k)
                {
                    // Streaks blowing past the user's ball the way the wind goes.
                    const Fixed along = F((static_cast<int64_t>(now) * 3 + k * 37) % 300, 10) - F(15);
                    const Fixed across = F(k - 3) * F(3, 2);
                    if (e.puffs[k] < 0) continue;
                    poseStreak(e.puffs[k], e.x + e.sx * along + e.cz * across, e.y + F(1) + F(k % 3, 2), e.z + e.cz * along - e.sx * across, e.sx, e.cz);
                }
            else if (e.kind == GolfPowers::Magnet)
                for (int k = 0; k < 6; ++k)
                {
                    // Beads round the cup, red and blue by turns, closing on it and out again.
                    (age * F(4) + Fixed::TwoPi() * F(k, 6)).SinCos(s, c);
                    const Fixed out = F(3, 2) - F((ticks * 2 + k * 9) % 50, 50);
                    sizePuff(e.puffs[k], F(35, 100), e.x + s * out, e.y + F(3, 10), e.z + c * out);
                }
            else if (e.kind == GolfPowers::Cannon)
                for (int k = 0; k < 3; ++k)   // smoke where it was fired from, swelling as it rises (in steps: a size is a verb)
                {
                    const int64_t step = std::min<int64_t>(ticks, 60) / 6;
                    sizePuff(e.puffs[k], F(6, 10) + F(step, 5) + F(k, 5), e.x + F(k - 1, 4), e.y + F(2, 10) + F(std::min<int64_t>(ticks, 60) * (k + 1), 60), e.z);
                }
            else if (e.kind == GolfPowers::Smoke)
            {
                /*
                 * A RING of smoke round where it went off, not a cloud on it:
                 * out from one to six over its thick five seconds -- fast,
                 * then slowing -- and on to seven and a half as it thins, so
                 * it hides the green from anyone looking across it. Placed
                 * every tick; its size and thickness said kSmokeSteps times
                 * over its life (a verb a puff each): swelling all along,
                 * dense while thick, thinning to nothing after.
                 */
                const int thick = GolfPowers::kSmokeThick, life = GolfPowers::kSmokeTicks;
                const int64_t t = std::min<int64_t>(ticks, life);
                Fixed r;
                if (t < thick) { const Fixed v = Fixed::One() - F(t, thick); r = F(1) + F(5) * (Fixed::One() - v * v); }
                else           r = F(6) + F(3, 2) * F(t - thick, life - thick);
                const int step = static_cast<int>(t * GolfPowers::kSmokeSteps / life);
                const Fixed wide = F(28, 10) + F(27, 10) * F(step, GolfPowers::kSmokeSteps);
                for (int k = 0; k < kEffectPuffs; ++k)
                {
                    Fixed sn, cs;
                    (Fixed::TwoPi() * F(k, kEffectPuffs) + F(t, 3000)).SinCos(sn, cs);
                    sizePuff(e.puffs[k], wide, e.x + sn * r, e.y + F(7, 10) + F(k % 2, 3), e.z + cs * r);
                }
                if (step != e.said)
                {
                    e.said = step;
                    const int at = step * life / GolfPowers::kSmokeSteps;
                    const float thin = at < thick ? 0.0f : static_cast<float>(at - thick) / (life - thick);
                    for (int k = 0; k < kEffectPuffs; ++k)
                        if (e.puffs[k] >= 0) verb(m_puff_nodes[static_cast<size_t>(e.puffs[k])], "SetSmoke", fmt3(0.95f * (1.0f - thin) + 0.03f));
                }
            }
            else if (e.kind == GolfPowers::Freeze)
            {
                // A cold haze on each ball it holds (as many as there are puffs).
                int k = 0;
                for (size_t j = 0; j < m_slots.size() && k < 6; ++j)
                {
                    Fixed jx, jy, jz;
                    if (static_cast<int>(j) == e.owner || m_slots[j].out || !ballAt(m_slots[j], jx, jy, jz)) continue;
                    sizePuff(e.puffs[k++], F(7, 10), jx, jy, jz);
                }
                for (; k < 6; ++k) if (e.puffs[k] >= 0 && m_puff_size[e.puffs[k]] != "hid") { hide(m_puff_nodes[e.puffs[k]]); m_puff_size[e.puffs[k]] = "hid"; }
            }
            else
                for (int k = 0; k < 6; ++k)
                {
                    (age * F(6) + F(k)).SinCos(s, c);
                    const Fixed up = F(k) * F(9, 10), wide = F(6, 10) + F(k) * F(4, 10);
                    sizePuff(e.puffs[k], wide * F(2), e.x + s * wide * F(3, 10), e.y + up + F(1, 2), e.z + c * wide * F(3, 10));
                }
        }
        for (size_t n = 0; n < m_strikes.size();)
        {
            Strike& s = m_strikes[n];
            const int64_t age = now - s.at;
            if (age > kHz) { freePuffsLocked(s.puffs, 4); m_strikes.erase(m_strikes.begin() + static_cast<long>(n)); continue; }
            any = true;
            if (age > 6) freePuffsLocked(&s.puffs[0], 1);
            if (age > 10) freePuffsLocked(&s.puffs[1], 1);
            sizePuff(s.puffs[1], F(15, 10) + F(age, 10), s.x, s.y + F(1, 2), s.z);
            const Fixed rise = F(age, 25);
            sizePuff(s.puffs[2], F(1) + rise, s.x - F(1, 2), s.y + F(1, 2) + rise, s.z);
            sizePuff(s.puffs[3], F(8, 10) + rise, s.x + F(1, 2), s.y + F(1, 2) + rise * F(3, 2), s.z + F(1, 3));
            ++n;
        }
        if (any) verb(m_world, "Move", "0, 0, 0");
    }
    void sizePuff(int n, Fixed size, Fixed x, Fixed y, Fixed z)
    {
        if (n < 0) return;
        const float d = size.ToFloat();
        resizePuff(n, f3(d, d * 0.9f, d));
        placeNode(m_puff_nodes[n], x, y, z);
    }
    // A puff's size said when it changes, not a tick: a verb is a line of the
    // scene's record, and most puffs are one size for as long as they last.
    void resizePuff(int n, const std::string& size)
    {
        if (m_puff_size[n] == size) return;
        if (m_puff_size[n] == "hid") verb(m_puff_nodes[n], "SetVisible", "1");
        verb(m_puff_nodes[n], "Create", size);
        m_puff_size[n] = size;
    }
    void poseStreak(int n, Fixed x, Fixed y, Fixed z, Fixed sx, Fixed cz)
    {
        const ETCS::RID r = m_puff_nodes[n];
        resizePuff(n, "0.12, 0.12, 2.2");
        if (Causal_* c = causal(r))
        {
            std::lock_guard<std::recursive_mutex> lk(c->TreeMutex());
            OrderVector& o = c->RowsUnder();
            o.PlaceAt(x, y, z);
            GolfCourse::Slab b; b.ys = sx; b.yc = cz;
            GolfCourse::Turn(b, o);
            c->RowsWritten();
        }
    }

    // ── this runtime's own: the eye, the hand, the shadows, the card ─────

    // The ball the eye is on: this runtime's own, or (watching, or out) the cup.
    bool eyeOnLocked(float out[3])
    {
        const int i = slotOf(m_me);
        Fixed x, y, z;
        if (i >= 0 && !m_slots[i].out && ballAt(m_slots[i], x, y, z)) { out[0] = x.ToFloat(); out[1] = y.ToFloat(); out[2] = z.ToFloat(); return true; }
        const bool cup = m_play.cup && m_phase != Lobby;
        out[0] = (cup ? m_play.cup_x : m_play.tee_x).ToFloat();
        out[1] = (cup ? m_play.cup_y : m_play.tee_y).ToFloat();
        out[2] = (cup ? m_play.cup_z : m_play.tee_z).ToFloat();
        return true;
    }
    void cameraLocked()
    {
        float b[3];
        if (!eyeOnLocked(b)) return;
        const float moved = std::fabs(b[0] - m_seen[0]) + std::fabs(b[1] - m_seen[1]) + std::fabs(b[2] - m_seen[2]);
        if (!m_follow && moved < 1e-4f) return;
        m_seen[0] = b[0]; m_seen[1] = b[1]; m_seen[2] = b[2];
        m_follow = false;
        placeCameraLocked();
    }
    void placeCameraLocked()
    {
        ETCS::Held<Camera_> cam = ETCS::resolve_held<Camera_>("Camera", m_camera);
        if (!cam) return;
        ViewFrustum v = cam->GetView();
        const float* b = m_seen;
        const float cp = std::cos(m_pitch);
        v.position = Point3D{ b[0] - m_dist * cp * std::sin(m_yaw), b[1] + m_dist * std::sin(m_pitch), b[2] - m_dist * cp * std::cos(m_yaw) };
        v.look_at  = Point3D{ b[0], b[1], b[2] };
        v.up       = Point3D{ 0.0f, 1.0f, 0.0f };
        cam->SetView(v);
    }
    // The line through a pointer on the camera's frame (window pixels).
    bool rayLocked(float px, float py, Point3D& o, Point3D& d)
    {
        ETCS::Held<Camera_> cam = ETCS::resolve_held<Camera_>("Camera", m_camera);
        if (!cam) return false;
        void* plane = static_cast<ETCS::Entity*>(cam.get())->getInterfacePointer(ETCS::Buffer("Drawable2D"));
        if (!plane) return false;
        const Rect2D f = static_cast<Drawable2D_*>(plane)->Bounds();
        if (f.w == 0 || f.h == 0) return false;
        const float fx = (px - static_cast<float>(f.x)) / static_cast<float>(f.w);
        const float fy = (py - static_cast<float>(f.y)) / static_cast<float>(f.h);
        return ViewRay(cam->GetView(), fx, fy, static_cast<float>(f.w) / static_cast<float>(f.h), o, d);
    }
    // Does the pointer's line pass through this runtime's ball? A little
    // generous: a ball is small on the screen, and a press that grazes it means it.
    bool onMyBallLocked(float px, float py)
    {
        const int i = slotOf(m_me);
        Fixed x, y, z;
        Point3D o, d;
        if (i < 0 || m_slots[i].out || !ballAt(m_slots[i], x, y, z) || !rayLocked(px, py, o, d)) return false;
        const float ox = x.ToFloat() - o.x, oy = y.ToFloat() - o.y, oz = z.ToFloat() - o.z;
        const float t = ox * d.x + oy * d.y + oz * d.z;
        if (t < 0.0f) return false;
        const float cx = ox - d.x * t, cy = oy - d.y * t, cz = oz - d.z * t;
        const float rr = ballR().ToFloat() * 1.6f;
        return cx * cx + cy * cy + cz * cz <= rr * rr;
    }
    uint8_t heldPowerLocked() const { const int i = slotOf(m_me); return i < 0 ? static_cast<uint8_t>(GolfPowers::None) : m_slots[i].power; }

    /*
     * THE PULL. Where the pointer's line meets the plane through the ball
     * facing the camera gives a drag: so far to the side, so far down the
     * picture. The shot is opposite it, ALONG THE GROUND THE BALL IS ON --
     * the drag's way down the picture is the way the camera faces, level, and
     * the whole is laid on the slope under the ball. NOT LOCKED TO IT: the
     * drag's own rise off the ground (as dragged, on the plane facing the
     * camera) is let through damped, the more the longer the pull -- none of
     * it for a tap, half of it at the full pull -- so a putt stays on the
     * ground and a long pull lifts. MORE LOFT IS WHAT THE CAMERA DOES AFTER:
     * lowered from where it was at the press (the right button, held with the
     * left), the shot tips up by as much more; raised again, back down to
     * the drag's own and no further.
     */
    static constexpr float kRiseKept = 0.5f;   // of the drag's own rise off the ground, at the full pull
    void pullLocked(float px, float py)
    {
        const int i = slotOf(m_me);
        Fixed bx, by, bz;
        Point3D o, d;
        if (i < 0 || !ballAt(m_slots[i], bx, by, bz) || !rayLocked(px, py, o, d)) return;
        const float b[3] = { bx.ToFloat(), by.ToFloat(), bz.ToFloat() };
        const float sy = std::sin(m_yaw), cy = std::cos(m_yaw), sp = std::sin(m_pitch), cp = std::cos(m_pitch);
        // The camera's own axes at the ball: the way it looks, and up its picture.
        const float n[3] = { cp * sy, -sp, cp * cy }, up[3] = { sp * sy, cp, sp * cy };
        const float den = d.x * n[0] + d.y * n[1] + d.z * n[2];
        if (std::fabs(den) < 1e-6f) return;
        const float t = ((b[0] - o.x) * n[0] + (b[1] - o.y) * n[1] + (b[2] - o.z) * n[2]) / den;
        if (t <= 0.0f) return;
        const float drag[3] = { b[0] - (o.x + d.x * t), b[1] - (o.y + d.y * t), b[2] - (o.z + d.z * t) };   // opposite the pointer
        // What of it is up the picture, and the rest: across it, which is level already.
        const float fore = drag[0] * up[0] + drag[1] * up[1] + drag[2] * up[2];
        const float side[3] = { drag[0] - fore * up[0], 0.0f, drag[2] - fore * up[2] };
        float len = std::sqrt(drag[0] * drag[0] + drag[1] * drag[1] + drag[2] * drag[2]);
        if (len < 1e-5f) { m_pull_len = Fixed::Zero(); hideArrowLocked(); return; }
        // Level: the picture's up is the camera's way ahead.
        float s[3] = { side[0] + fore * sy, 0.0f, side[2] + fore * cy };
        // Onto the ground under the ball, if it is on any.
        float g[3] = { 0.0f, 1.0f, 0.0f };
        Fixed top;
        if (const GolfCourse::Slab* on = GolfCourse::TopAt(m_play, bx, bz, top, by))
            if ((by - ballR() - top).ToFloat() < 0.3f)
            {
                OrderVector turn;
                GolfCourse::Turn(*on, turn);
                Fixed gx = Fixed::Zero(), gy = Fixed::One(), gz = Fixed::Zero();
                turn.RotateVector(gx, gy, gz);
                g[0] = gx.ToFloat(); g[1] = gy.ToFloat(); g[2] = gz.ToFloat();
            }
        const float into = s[0] * g[0] + s[1] * g[1] + s[2] * g[2];
        for (int k = 0; k < 3; ++k) s[k] -= into * g[k];
        const float flat = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
        if (flat < 1e-5f) return;
        // ...and tipped up off it: by the drag's own rise, damped the more the
        // shorter the pull, and by as much as the camera has come down since the press.
        const float most = m_max_pull.ToFloat();
        const float rise = std::asin(clampf((drag[0] * g[0] + drag[1] * g[1] + drag[2] * g[2]) / len, -1.0f, 1.0f));
        if (len > most) len = most;
        const float kept = kRiseKept * (len / most) * std::max(0.0f, rise);
        const float loft = clampf(kept + std::max(0.0f, m_pull_pitch - m_pitch), 0.0f, 1.4f), cl = std::cos(loft), sl = std::sin(loft);
        for (int k = 0; k < 3; ++k) s[k] = (s[k] / flat * cl + g[k] * sl) * len;
        m_pull[0] = Fixed::From(s[0]); m_pull[1] = Fixed::From(s[1]); m_pull[2] = Fixed::From(s[2]);
        m_pull_len = Fixed::From(len);
        arrowLocked(b, s, len);
    }
    // Out of the ball along the pull, as long as it, a gentle green to a hard
    // red by how much of the most it is; and its shadow laid under it.
    void arrowLocked(const float b[3], const float s[3], float len)
    {
        const float most = m_max_pull.ToFloat(), r = ballR().ToFloat();
        if (!m_arrow || len < minPull().ToFloat()) { hideArrowLocked(); return; }
        const float u[3] = { s[0] / len, s[1] / len, s[2] / len };
        const float mid = r + len * 0.5f, k = len / most, w = 0.12f + 0.08f * k;
        verb(m_arrow, "Create", f3(w, len, w));
        verb(m_arrow, "SetPosition", f3(b[0] + u[0] * mid, b[1] + u[1] * mid, b[2] + u[2] * mid));
        verb(m_arrow, "Aim", f3(u[0], u[1], u[2]));
        verb(m_arrow, "SetColor", f3(0.25f + 0.7f * k, 0.85f - 0.6f * k, 0.25f) + ", 1");
        verb(m_arrow, "SetVisible", "1");
        if (!m_arrow_shadow) return;
        const float run = len * std::sqrt(u[0] * u[0] + u[2] * u[2]);
        if (run < minPull().ToFloat()) { verb(m_arrow_shadow, "SetVisible", "0"); return; }
        const float half = 0.5f * std::atan2(u[0], u[2]);
        const float c = std::cos(half), sn = std::sin(half);
        const float degrees = 2.0f * std::acos(c * 0.70710678f) * 180.0f / kPi;
        verb(m_arrow_shadow, "Create", f3(w, run, 0.02f));
        verb(m_arrow_shadow, "SetPosition", f3(b[0] + u[0] * mid, b[1] - r + 0.02f, b[2] + u[2] * mid));
        verb(m_arrow_shadow, "Orient", f3(c, sn, -sn) + ", " + std::to_string(degrees));
        verb(m_arrow_shadow, "SetVisible", "1");
    }
    void hideArrowLocked() { hide(m_arrow); hide(m_arrow_shadow); }
    void hideGhostLocked() { hide(m_ghost); hide(m_ghost_arrow); m_ghost_ok = false; }
    // What the left holds, put down: no shot, no power.
    void putDownLocked()
    {
        m_pull_len = Fixed::Zero();
        hideArrowLocked();
        hideGhostLocked();
    }
    void shootLocked()
    {
        hideArrowLocked();
        if (m_pull_len < minPull()) return;
        proposeLocked("shot " + std::to_string(milliOf(m_pull[0])) + " " + std::to_string(milliOf(m_pull[1])) + " " + std::to_string(milliOf(m_pull[2])));
        m_pull_len = Fixed::Zero();
    }
    static long long milliOf(Fixed f) { return static_cast<long long>(std::llround(static_cast<double>(f.ToFloat()) * 1000.0)); }
    // The view's bearing, as a power's way: degrees, 0 toward +z, 90 toward +x.
    int aimLocked() const { int a = static_cast<int>(std::lround(m_yaw * 180.0f / kPi)) % 360; return a < 0 ? a + 360 : a; }
    void placeLocked()
    {
        const bool ok = m_ghost_ok;
        hideGhostLocked();
        if (!ok) return;
        proposeLocked("use " + std::to_string(milliOf(m_ghost_at[0])) + " " + std::to_string(milliOf(m_ghost_at[1])) + " "
                      + std::to_string(milliOf(m_ghost_at[2])) + " " + std::to_string(aimLocked()));
        m_armed = false;
    }
    // Where the pointer's line first meets a surface of the hole: the ring
    // there, and the arrow for a power that goes the way you look.
    void ghostLocked(float px, float py)
    {
        Point3D o, d;
        m_ghost_ok = false;
        if (!rayLocked(px, py, o, d)) { hideGhostLocked(); return; }
        float best = 1e9f;
        for (const GolfCourse::Slab& b : m_play.slabs)
        {
            float t;
            if (rayBox(b, o, d, t) && t < best) best = t;
        }
        if (best > 500.0f) { hideGhostLocked(); return; }
        const float at[3] = { o.x + d.x * best, o.y + d.y * best, o.z + d.z * best };
        m_ghost_at[0] = Fixed::From(at[0]); m_ghost_at[1] = Fixed::From(at[1]); m_ghost_at[2] = Fixed::From(at[2]);
        m_ghost_ok = true;
        const uint8_t kind = heldPowerLocked();
        const float size = kind == GolfPowers::BlackHole ? 3.0f : kind == GolfPowers::Tornado ? 4.0f : kind == GolfPowers::Sand ? 4.0f : 2.0f;
        verb(m_ghost, "Create", f3(size, 0.05f, size));
        verb(m_ghost, "SetPosition", f3(at[0], at[1] + 0.05f, at[2]));
        verb(m_ghost, "SetVisible", "1");
        if (GolfPowers::Aimed(kind))
        {
            const float yaw = m_yaw;
            verb(m_ghost_arrow, "SetPosition", f3(at[0] + std::sin(yaw) * 1.4f, at[1] + 0.3f, at[2] + std::cos(yaw) * 1.4f));
            verb(m_ghost_arrow, "Aim", f3(std::sin(yaw), 0.0f, std::cos(yaw)));
            verb(m_ghost_arrow, "SetVisible", "1");
        }
        else hide(m_ghost_arrow);
    }
    // A line against a slab, in floats: this runtime's pointer only, never a
    // line of the trace until it is said in thousandths.
    static bool rayBox(const GolfCourse::Slab& b, const Point3D& o, const Point3D& d, float& t)
    {
        OrderVector r;
        GolfCourse::Turn(b, r);
        const float qw = r.qw.ToFloat(), qx = r.qx.ToFloat(), qy = r.qy.ToFloat(), qz = r.qz.ToFloat();
        auto unturn = [&](float v[3]) {
            // v' = q* v q
            const float x = -qx, y = -qy, z = -qz, w = qw;
            const float tx = 2.0f * (y * v[2] - z * v[1]), ty = 2.0f * (z * v[0] - x * v[2]), tz = 2.0f * (x * v[1] - y * v[0]);
            const float rx = v[0] + w * tx + (y * tz - z * ty), ry = v[1] + w * ty + (z * tx - x * tz), rz = v[2] + w * tz + (x * ty - y * tx);
            v[0] = rx; v[1] = ry; v[2] = rz;
        };
        float p[3] = { o.x - b.x.ToFloat(), o.y - b.y.ToFloat(), o.z - b.z.ToFloat() }, dd[3] = { d.x, d.y, d.z };
        unturn(p); unturn(dd);
        const float hl[3] = { b.w.ToFloat() * 0.5f, b.h.ToFloat() * 0.5f, b.d.ToFloat() * 0.5f };
        float in = 0.0f, out = 1e9f;
        for (int k = 0; k < 3; ++k)
        {
            if (std::fabs(dd[k]) < 1e-7f) { if (std::fabs(p[k]) > hl[k]) return false; continue; }
            float t0 = (-hl[k] - p[k]) / dd[k], t1 = (hl[k] - p[k]) / dd[k];
            if (t0 > t1) std::swap(t0, t1);
            in = std::max(in, t0); out = std::min(out, t1);
            if (in > out) return false;
        }
        t = in;
        return true;
    }

    // Every ball's shadow: a dark disc on whatever is straight under it, laid
    // to that surface, so a ball in the air is seen to be in the air.
    void shadowsLocked()
    {
        for (Slot& s : m_slots)
        {
            if (!s.shadow) continue;
            Fixed x, y, z, top;
            const GolfCourse::Slab* on = nullptr;
            Causal_* k = causal(s.ball);
            if (!s.out && k && k->Environment() == causal(m_world) && ballAt(s, x, y, z)) on = GolfCourse::TopAt(m_play, x, z, top, y);
            if (!on) { if (s.shadow_shown) { hide(s.shadow); s.shadow_shown = false; } continue; }
            if (Causal_* c = causal(s.shadow))
            {
                std::lock_guard<std::recursive_mutex> lk(c->TreeMutex());
                OrderVector& r = c->RowsUnder();
                r.PlaceAt(x, top + F(35, 1000), z);
                GolfCourse::Turn(*on, r);
                c->RowsWritten();
            }
            if (!s.shadow_shown) { verb(s.shadow, "SetVisible", "1"); s.shadow_shown = true; }
        }
    }

    // The card: the hole, your strokes and total, the clock and your power --
    // or the lobby's word, or the round's end.
    void cardLocked(double dt_ms)
    {
        if (!m_card) return;
        m_card_ms += dt_ms;
        if (m_card_ms < 150.0 && !m_card_text.empty()) return;
        m_card_ms = 0.0;
        const int i = slotOf(m_me);
        std::string t;
        const std::string holes = std::to_string(m_live.holes.size());
        if (m_phase == Lobby)
            t = "lobby: " + courseName(m_course) + ", " + holes + " holes -- " + (m_authority ? "Enter starts the round" : "the host starts the round") + " -- knock a ball about";
        else if (m_phase == Results) t = m_note;
        else
        {
            t = "hole " + std::to_string(m_hole + 1) + "/" + holes + " (par " + std::to_string(m_play.par) + ")";
            if (i >= 0) t += "   strokes " + std::to_string(m_slots[i].strokes) + "   total " + std::to_string(m_slots[i].total);
            t += "   " + clock(m_phase == Playing ? m_end - static_cast<int64_t>(m_tick) : 0);
            if (m_phase == Between) t += "   next hole in " + std::to_string((m_end - static_cast<int64_t>(m_tick) + kHz - 1) / kHz);
        }
        if (i >= 0 && m_slots[i].power != GolfPowers::None)
            t += std::string("   power: ") + GolfPowers::Name(m_slots[i].power) + (m_armed ? " -- ARMED: click where it goes" : " (E)")
               + (m_slots[i].spare != GolfPowers::None ? std::string("   then ") + GolfPowers::Name(m_slots[i].spare) : std::string());
        if (t == m_card_text) return;
        m_card_text = t;
        verb(m_card, "SetText", t);
    }
    /*
     * THE LOBBY'S MENU, drawn in the canvas (RenderProvider::Menu, bound by
     * golf_play.etcs): the next round's course and how long a hole has (the
     * host's to move), your ball's colour and your name (yours), start, a new
     * course for one made from a seed, the first hole from above, and the
     * players with their last round. Said to the widget as lines, rebuilt
     * each frame, only those that changed sent. The keys and the pointer both
     * move it and every change it makes is a line of the trace -- the page's
     * own controls say the same lines, so either moves the other. Folded away
     * (M, or its last button) the course is all there to knock a ball about.
     */
    std::vector<std::string> menuItemsLocked() const
    {
        std::vector<std::string> items;
        const bool seated = slotOf(m_me) >= 0;
        if (m_authority) { items.push_back("course"); items.push_back("length"); }
        if (seated) { items.push_back("colour"); items.push_back("name"); }
        if (m_authority)
        {
            items.push_back("start");
            if (!m_defs.empty() && m_defs[m_course].generated) items.push_back("reroll");
        }
        items.push_back("settings");
        items.push_back("fold");
        return items;
    }
    bool menuShownLocked() const { return m_menu_rid && (m_settings || (m_menu_open && m_phase == Lobby && !m_me.empty() && m_ready)); }

    /*
     * THE SETTINGS: this runtime's own, and none of them the match's -- no
     * line, nothing hashed: how fast a drag turns the camera, how far a
     * notch of the wheel brings it in. O opens them over the course, in a
     * round or in the lobby (whose menu has a button for them too), on the
     * lobby's menu; Setting and Settings are the same for a page.
     */
    static constexpr float kPaceChoices[3] = { 0.5f, 1.0f, 2.0f };
    static const char* paceName(float v) { return v < 0.75f ? "slow" : v > 1.5f ? "fast" : "normal"; }
    bool settingLocked(const std::string& name, const std::string& value)
    {
        const float v = static_cast<float>(std::atof(value.c_str()));
        if (!(v >= 0.1f && v <= 5.0f)) return false;
        if (name == "look") { m_look_speed = v; return true; }
        if (name == "zoom") { m_zoom_speed = v; return true; }
        return false;
    }
    void settingStepLocked(const std::string& id, int dir)
    {
        float& v = id == "look" ? m_look_speed : m_zoom_speed;
        if (id != "look" && id != "zoom") return;
        int at = 0;
        for (int k = 0; k < 3; ++k) if (kPaceChoices[k] <= v + 1e-3f) at = k;
        v = kPaceChoices[(at + dir + 3) % 3];
    }
    bool settingsKeyLocked(uint16_t key)
    {
        static const char* const items[3] = { "look", "zoom", "close" };
        const bool up = key == 265 || key == 'W', down = key == 264 || key == 'S';
        const bool left = key == 263 || key == 'A', right = key == 262 || key == 'D';
        const bool enter = key == 257 || key == 335 || key == ' ';
        m_set_focus = ((m_set_focus % 3) + 3) % 3;
        if (key == 256) { m_settings = false; return true; }
        if (up)   { m_set_focus = (m_set_focus + 2) % 3; return true; }
        if (down) { m_set_focus = (m_set_focus + 1) % 3; return true; }
        if (!left && !right && !enter) return false;
        const std::string at = items[m_set_focus];
        if (at == "close") { if (enter) m_settings = false; return true; }
        settingStepLocked(at, left ? -1 : 1);
        return true;
    }
public:
    bool Setting(const std::string& name, const std::string& value) { std::lock_guard<std::mutex> g(m_mu); return settingLocked(name, value); }
    // "look <scale> zoom <scale>"
    std::string Settings() { std::lock_guard<std::mutex> g(m_mu); return "look " + fmt3(m_look_speed) + " zoom " + fmt3(m_zoom_speed); }
    void ShowSettings(bool on) { std::lock_guard<std::mutex> g(m_mu); m_settings = on && m_menu_rid; m_editing = false; }
private:
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
            return true;   // typing arms nothing
        }
        if (key == 'M') { m_menu_open = !m_menu_open; return true; }
        if (!m_menu_open) return false;
        const std::vector<std::string> items = menuItemsLocked();
        const bool up = key == 265 || key == 'W', down = key == 264 || key == 'S';
        const bool left = key == 263 || key == 'A', right = key == 262 || key == 'D';
        const bool enter = key == 257 || key == 335;
        const int n = static_cast<int>(items.size());
        m_menu = ((m_menu % n) + n) % n;
        if (up)   { m_menu = (m_menu + n - 1) % n; return true; }
        if (down) { m_menu = (m_menu + 1) % n; return true; }
        if (!left && !right && !enter) return false;
        const std::string& at = items[static_cast<size_t>(m_menu)];
        if (at == "start" || at == "reroll" || at == "name" || at == "fold" || at == "settings") { if (enter) menuActLocked(at, "press", 0); return true; }
        // Enter anywhere else is the host's start, as it is with the menu folded.
        if (enter) { menuActLocked("start", "press", 0); return true; }
        menuActLocked(at, left ? "prev" : "next", 0);
        return true;
    }
    // A press at (x, y) of the window: true when it was on the menu's panel.
    bool menuPressLocked(int x, int y)
    {
        ETCS::Buffer a("Menu.Point"), d((std::to_string(x) + " " + std::to_string(y)).c_str());
        try { if (ETCS::Entity* e = ETCS::etcs_resolve_rid_anywhere(&ETCS::getLoader(), m_menu_rid)) e->call(a, d); }
        catch (...) { return false; }
        std::istringstream in(d.toString());
        std::string id, part;
        int n = 0;
        in >> id >> part >> n;
        if (id.empty() || id == "-") return false;   // off the panel
        if (m_editing && id != "name") { m_editing = false; if (validName(m_edit)) restyleLocked(-2, m_edit); }
        if (id == "+") return true;                  // on it, on nothing of it
        if (m_settings)
        {
            m_set_focus = id == "look" ? 0 : id == "zoom" ? 1 : 2;
            if (id == "close") m_settings = false; else settingStepLocked(id, part == "prev" ? -1 : 1);
            return true;
        }
        const std::vector<std::string> items = menuItemsLocked();
        for (size_t k = 0; k < items.size(); ++k) if (items[k] == id) m_menu = static_cast<int>(k);
        menuActLocked(id, part, n);
        return true;
    }
    void menuActLocked(const std::string& id, const std::string& part, int n)
    {
        const int dir = part == "prev" ? -1 : 1;
        if (id == "fold") { m_menu_open = false; return; }
        if (id == "settings") { m_settings = true; m_set_focus = 0; m_editing = false; return; }
        if (id == "colour")
        {
            const int i = slotOf(m_me);
            if (i < 0) return;
            int c = m_slots[static_cast<size_t>(i)].colour;
            if (c < 0) c = i % static_cast<int>(kMaxBalls);
            c = part == "swatch" ? n : ((c + dir) % kPaletteN + kPaletteN) % kPaletteN;
            restyleLocked(c, std::string());
            return;
        }
        if (id == "name") { const int i = slotOf(m_me); if (i >= 0) { m_editing = true; m_edit = nameOfLocked(static_cast<size_t>(i)); } return; }
        if (!m_authority) return;
        if (id == "start") { takeLocked(m_me, "round " + std::to_string(rollLocked())); return; }
        if (id == "reroll") { if (m_defs[m_course].generated) configLocked(m_course, m_hole_secs, true); return; }
        if (id == "course")
        {
            const int count = static_cast<int>(m_defs.size());
            configLocked(((m_course + dir) % count + count) % count, m_hole_secs, true);
            return;
        }
        if (id == "length")
        {
            static const uint32_t lengths[] = { 60, 120, 180, 300 };
            int at = 0;
            for (int l = 0; l < 4; ++l) if (lengths[l] <= m_hole_secs) at = l;
            configLocked(m_course, lengths[(at + dir + 4) % 4], false);
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
        if (m_settings)
        {
            L.push_back("title|SETTINGS");
            L.push_back("sub|this tab's own -- none of it is the match's");
            L.push_back(std::string("choice|look|look|") + paceName(m_look_speed) + "|1");
            L.push_back(std::string("choice|zoom|zoom|") + paceName(m_zoom_speed) + "|1");
            L.push_back("button|close|done|1|1");
            L.push_back("hint|arrows or click change them  O shuts");
            sendMenuLocked(L, std::string(m_set_focus == 0 ? "look" : m_set_focus == 1 ? "zoom" : "close"));
            return;
        }
        const bool host = m_authority;
        const int me = slotOf(m_me);
        L.push_back("title|LOBBY");
        L.push_back("sub|" + (m_session ? labelLocked(m_owner) + "'s match" : std::string("practice")) + " -- next round");
        L.push_back("choice|course|course|" + courseName(m_course) + (m_layout ? "  #" + std::to_string(m_layout % 10000) : std::string()) + "|" + (host ? "1" : "0"));
        L.push_back("choice|length|a hole|" + clock(static_cast<int64_t>(m_hole_secs) * kHz) + "|" + (host ? "1" : "0"));
        if (me >= 0)
        {
            const Slot& s = m_slots[static_cast<size_t>(me)];
            std::string sw;
            for (int c = 0; c < kPaletteN; ++c) sw += (c ? "," : "") + hexOf(kPalette[c]);
            L.push_back("swatches|colour|colour|" + std::to_string(s.colour >= 0 ? s.colour : me % static_cast<int>(kMaxBalls)) + "|" + sw + "|1");
            L.push_back("field|name|name|" + (m_editing ? m_edit : nameOfLocked(static_cast<size_t>(me))) + "|1|" + (m_editing ? "1" : "0"));
        }
        L.push_back(host ? std::string("button|start|start the round|1|1") : "button|start|" + labelLocked(m_owner) + " starts the round|0|0");
        if (host && !m_defs.empty() && m_defs[m_course].generated) L.push_back("button|reroll|new course|1|0");
        L.push_back("button|settings|settings (O)|1|0");
        L.push_back("button|fold|knock a ball about|1|0");
        L.push_back("head|player        last");
        for (size_t i : rankLocked())
        {
            const Slot& s = m_slots[i];
            char line[96];
            std::snprintf(line, sizeof line, "%-12s %5s", nameOfLocked(i).substr(0, 12).c_str(),
                          s.last_total < 0 ? "-" : std::to_string(s.last_total).c_str());
            L.push_back("row|" + hexOf(colourOfLocked(i)) + "|" + line);
        }
        L.push_back(std::string("hint|") + (m_editing ? "type a name  Enter keeps it  Esc drops it"
                                            : host ? "arrows move it  Enter starts  M folds  or click"
                                                   : "arrows: your colour and name  M folds  or click"));
        previewLocked(L);
        const std::vector<std::string> items = menuItemsLocked();
        const int n = static_cast<int>(items.size());
        sendMenuLocked(L, items[static_cast<size_t>(((m_menu % n) + n) % n)]);
    }
    // Only what changed goes over.
    void sendMenuLocked(const std::vector<std::string>& L, const std::string& focus)
    {
        if (m_menu_sent.size() > L.size()) m_menu_sent.resize(L.size());
        for (size_t k = 0; k < L.size(); ++k)
        {
            if (k < m_menu_sent.size() && m_menu_sent[k] == L[k]) continue;
            verb(m_menu_rid, "SetLine", std::to_string(k) + " " + L[k]);
            if (k < m_menu_sent.size()) m_menu_sent[k] = L[k]; else m_menu_sent.push_back(L[k]);
        }
        if (m_menu_lines != L.size()) { verb(m_menu_rid, "SetLines", std::to_string(L.size())); m_menu_lines = L.size(); }
        if (focus != m_menu_focus) { verb(m_menu_rid, "Focus", focus); m_menu_focus = focus; }
    }
    // The hole out from above, in the course's own colours: its ground and
    // walls (a turned one by its middle and its size, unturned), its sand,
    // the tee, the cup, pads, springs and spots.
    void previewLocked(std::vector<std::string>& L) const
    {
        const GolfCourse::Hole& h = m_play;
        if (h.slabs.empty()) return;
        float x0 = 1e9f, x1 = -1e9f, z0 = 1e9f, z1 = -1e9f;
        for (const auto& b : h.slabs)
        {
            const bool across = b.ys.Abs() > F(7, 10);   // turned about a quarter: its width runs along z
            const float w = (across ? b.d : b.w).ToFloat() * 0.5f, d = (across ? b.w : b.d).ToFloat() * 0.5f;
            x0 = std::min(x0, b.x.ToFloat() - w); x1 = std::max(x1, b.x.ToFloat() + w);
            z0 = std::min(z0, b.z.ToFloat() - d); z1 = std::max(z1, b.z.ToFloat() + d);
        }
        // Square, about its middle: the picture is.
        const float half = std::max(x1 - x0, z1 - z0) * 0.5f + 1.0f, mx = (x0 + x1) * 0.5f, mz = (z0 + z1) * 0.5f;
        auto f1 = [](float v) { char b[16]; std::snprintf(b, sizeof b, "%.1f", v); return std::string(b); };
        L.push_back("preview|" + f1(mx - half) + "|" + f1(mz - half) + "|" + f1(mx + half) + "|" + f1(mz + half));
        const GolfCourse::Theme& t = m_live.theme;
        L.push_back("rect|" + f1(mx) + "|" + f1(mz) + "|" + f1(2 * half) + "|" + f1(2 * half) + "|" + hexOf(t.sky));
        for (int kind : { GolfCourse::Green, GolfCourse::Sand, GolfCourse::Wall })
            for (const auto& b : h.slabs)
            {
                if (b.kind != kind) continue;
                const bool across = b.ys.Abs() > F(7, 10);
                L.push_back("rect|" + f1(b.x.ToFloat()) + "|" + f1(b.z.ToFloat()) + "|" + f1((across ? b.d : b.w).ToFloat()) + "|" + f1((across ? b.w : b.d).ToFloat()) + "|"
                            + hexOf(kind == GolfCourse::Wall ? t.wall : kind == GolfCourse::Sand ? t.sand : t.turf));
            }
        auto disc = [&](Fixed x, Fixed z, float r, const char* c) { L.push_back("disc|" + f1(x.ToFloat()) + "|" + f1(z.ToFloat()) + "|" + f1(r) + "|" + c); };
        for (const auto& p : h.pads)    disc(p.x, p.z, 1.2f, "2f8fd0");
        for (const auto& p : h.springs) disc(p.x, p.z, 1.2f, "c04fa0");
        for (const auto& p : h.pickups) disc(p.x, p.z, 1.0f, "f2cc26");
        disc(h.tee_x, h.tee_z, 1.2f, "f0e3c7");
        if (h.cup) disc(h.cup_x, h.cup_z, 1.2f, "101014");
    }

    // The little board, top right: everyone's total, best first, in their
    // ball's colour -- this round's, or in the lobby the last one completed.
    void boardLocked()
    {
        std::vector<std::pair<std::string, int>> rows;
        const bool last = m_phase == Lobby;
        for (size_t i : rankLocked())
        {
            const Slot& s = m_slots[i];
            const int total = last ? s.last_total : s.total;
            if (last && total < 0) continue;
            std::string r = nameOfLocked(i) + "  " + std::to_string(total);
            if (!last) r += s.holed ? "  (in)" : s.done ? "  (out)" : "  (" + std::to_string(s.strokes) + ")";
            rows.push_back({ r, static_cast<int>(i) });
        }
        for (size_t n = 0; n < m_board.size(); ++n)
        {
            BoardRow& b = m_board[n];
            const bool show = n < rows.size() && (m_phase != Lobby || m_board_held || !rows.empty());
            const std::string text = show ? std::to_string(n + 1) + ". " + rows[n].first : std::string();
            if (show && text != b.text)
            {
                const float* c = colourOfLocked(static_cast<size_t>(rows[n].second));
                verb(b.rid, "SetText", text);
                verb(b.rid, "SetColor", f4(0.55f + 0.45f * c[0], 0.55f + 0.45f * c[1], 0.55f + 0.45f * c[2], 1.0f));
                b.text = text;
            }
            if (show != b.shown) { verb(b.rid, "SetHidden", show ? "0" : "1"); b.shown = show; }
        }
    }
    void fpsLocked(double dt_ms)
    {
        if (!m_fps || !m_camera) return;
        m_fps_ms += dt_ms;
        if (m_fps_ms < 1000.0 && m_fps_seen >= 0) return;
        const std::string said = answer(m_camera, "Renders");
        const int64_t now = said.empty() ? -1 : std::atoll(said.c_str());
        // ...and where they were drawn: "gpu" through the device, "cpu" on the host.
        if (now >= 0 && m_fps_seen >= 0 && m_fps_ms > 0.0)
            verb(m_fps, "SetText", std::to_string(static_cast<int>(static_cast<double>(now - m_fps_seen) * 1000.0 / m_fps_ms + 0.5)) + " fps "
                                   + (answer(m_camera, "OnDevice") == "1" ? "gpu" : "cpu"));
        m_fps_seen = now;
        m_fps_ms = 0.0;
    }

    // ── the state's hash: what two copies must agree on ─────────────────

    std::string hashLocked()
    {
        std::string st = std::to_string(m_tick) + " " + std::to_string(m_phase) + " " + std::to_string(m_end) + " " + std::to_string(m_course) + " "
                       + std::to_string(m_layout) + " " + std::to_string(m_hole) + " " + std::to_string(m_hole_secs) + " " + std::to_string(m_rng);
        for (const Slot& s : m_slots)
        {
            st += "|" + s.player + " " + std::to_string(s.strokes) + " " + std::to_string(s.total) + " " + std::to_string(s.holed) + std::to_string(s.done)
                + std::to_string(s.out) + " " + std::to_string(s.park_at) + " " + std::to_string(s.power) + " " + std::to_string(s.spare) + " "
                + std::to_string(s.colour) + " " + s.name + " " + std::to_string(s.fx.raw) + " "
                + std::to_string(s.fy.raw) + " " + std::to_string(s.fz.raw);
            if (Causal_* k = causal(s.ball))
            {
                std::lock_guard<std::recursive_mutex> lk(k->TreeMutex());
                const OrderVector& r = k->Order4();
                const Fixed v[] = { r.x, r.y, r.z, r.ox, r.oy, r.oz, r.energy };
                for (const Fixed& f : v) st += " " + std::to_string(f.raw);
            }
        }
        for (const Effect& e : m_effects) st += "|e" + std::to_string(e.kind) + " " + std::to_string(e.x.raw) + " " + std::to_string(e.z.raw) + " " + std::to_string(e.until);
        for (const auto& p : m_play.pads) st += "|p" + std::to_string(p.x.raw) + " " + std::to_string(p.z.raw);
        st += "|" + std::to_string(m_play.slabs.size()) + " " + std::to_string(m_play.springs.size());
        for (int64_t b : m_spot_back) st += "|s" + std::to_string(b);
        char buf[17];
        std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(XXH3_64bits(st.data(), st.size())));
        return buf;
    }

    // ── pooled nodes: placed by their rows, parked in berths ────────────

    void placeNode(ETCS::RID node, Fixed x, Fixed y, Fixed z)
    {
        Causal_* c = causal(node);
        if (!c) return;
        { std::lock_guard<std::recursive_mutex> lk(c->TreeMutex()); c->RowsUnder().PlaceAt(x, y, z); }
        c->RowsWritten();
    }
    // Placed and turned `yaw` degrees about +y; `flat` lays an arrow (its
    // length +y) along the ground first, a quarter turn about x.
    void poseNode(ETCS::RID node, Fixed x, Fixed y, Fixed z, int yaw, bool flat = false)
    {
        Causal_* c = causal(node);
        if (!c) return;
        GolfCourse::Slab b;
        GolfCourse::Degrees(F(yaw), b.ys, b.yc);
        if (flat) { b.ts = Fixed::One(); b.tc = Fixed::Zero(); }
        { std::lock_guard<std::recursive_mutex> lk(c->TreeMutex());
          OrderVector& r = c->RowsUnder();
          r.PlaceAt(x, y, z);
          GolfCourse::Turn(b, r); }
        c->RowsWritten();
    }
    /*
     * A pooled node not in use: hidden, far below, IN A BERTH OF ITS OWN
     * (KartBattle::parkNode's reason: members whose reaches meet are a pair
     * every interaction, and a pool parked at one point is n^2 of them).
     */
    void parkNode(ETCS::RID node)
    {
        if (!node) return;
        auto at = m_berths.find(node);
        if (at == m_berths.end()) at = m_berths.emplace(node, static_cast<uint32_t>(m_berths.size())).first;
        const int64_t k = at->second, row = 2 * kBerthsOut + 1;
        placeNode(node, F((k % row - kBerthsOut) * kBerth), F(-250), F((k / row % row - kBerthsOut) * kBerth));
        hide(node);
    }
    static constexpr int64_t kBerth = 10, kBerthsOut = 25;

    // ── small reads ─────────────────────────────────────────────────────

    int slotOf(const std::string& who) const
    {
        if (who.empty()) return -1;
        for (size_t i = 0; i < m_slots.size(); ++i) if (m_slots[i].player == who) return static_cast<int>(i);
        return -1;
    }
    // A player as a person names one: its seat, or else its label.
    int slotNamedLocked(const std::string& who) const
    {
        const int i = slotOf(who);
        if (i >= 0 || who.empty()) return i;
        for (size_t k = 0; k < m_slots.size(); ++k)
            if (!m_slots[k].player.empty() && (labelLocked(m_slots[k].player) == who || m_slots[k].name == who)) return static_cast<int>(k);
        return -1;
    }
    // A player as shown: the name its `look` gave, or else its seat's label.
    std::string nameOfLocked(size_t i) const
    {
        const Slot& s = m_slots[i];
        return s.name.empty() ? labelLocked(s.player) : s.name;
    }
    const float* colourOfLocked(size_t i) const
    {
        const Slot& s = m_slots[i];
        return s.colour >= 0 && s.colour < kPaletteN ? kPalette[s.colour] : kColors[i % kMaxBalls];
    }
    void paintBallLocked(size_t i)
    {
        const float* c = colourOfLocked(i);
        verb(m_slots[i].ball, "SetColor", f4(c[0], c[1], c[2], 1.0f));
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
            if (static_cast<int>(k) != mine && !m_slots[k].player.empty() && (m_slots[k].name == n || labelLocked(m_slots[k].player) == n)) return true;
        return false;
    }
    std::vector<size_t> rankLocked() const
    {
        std::vector<size_t> out;
        for (size_t i = 0; i < m_slots.size(); ++i) if (!m_slots[i].player.empty()) out.push_back(i);
        const bool last = m_phase == Lobby;
        std::stable_sort(out.begin(), out.end(), [&](size_t a, size_t b) {
            const int ta = last ? m_slots[a].last_total : m_slots[a].total, tb = last ? m_slots[b].last_total : m_slots[b].total;
            return ta < tb;
        });
        return out;
    }
    size_t playersLocked() const { size_t n = 0; for (const Slot& s : m_slots) if (!s.player.empty()) ++n; return n; }
    int courseIndex(const std::string& name) const
    {
        for (size_t c = 0; c < m_defs.size(); ++c) if (m_defs[c].name == name) return static_cast<int>(c);
        return -1;
    }
    std::string courseName(int c) const { return c >= 0 && c < static_cast<int>(m_defs.size()) ? m_defs[c].name : std::string("-"); }
    const char* phaseName() const { return m_phase == Playing ? "playing" : m_phase == Between ? "between" : m_phase == Results ? "results" : "lobby"; }
    int64_t secsLeft() const { return m_phase == Playing ? (m_end - static_cast<int64_t>(m_tick) + kHz - 1) / kHz : 0; }
    void noteLocked(const std::string& n) { m_note = n; ETCS_LOG("GolfMatch", n); }
    static std::string clock(int64_t ticks)
    {
        if (ticks < 0) ticks = 0;
        const int64_t secs = (ticks + kHz - 1) / kHz;
        char b[48];
        std::snprintf(b, sizeof b, "%lld:%02lld", static_cast<long long>(secs / 60), static_cast<long long>(secs % 60));
        return b;
    }
    static std::string fmt3(float v) { char b[32]; std::snprintf(b, sizeof b, "%.3f", v); return b; }
    static std::string f3(float a, float b, float c) { return std::to_string(a) + ", " + std::to_string(b) + ", " + std::to_string(c); }
    static std::string f4(float a, float b, float c, float d) { return f3(a, b, c) + ", " + std::to_string(d); }
    static Causal_* causal(ETCS::RID r) { return r ? ETCS::resolve_in_family<Causal_>("Causal", r) : nullptr; }
    static void hide(ETCS::RID r) { if (r) verb(r, "SetVisible", "0"); }
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
    // A verb on a node, by its own tag: what this module knows of the module
    // that made it is the verb's name.
    static bool verb(ETCS::RID node, const char* name, const std::string& args)
    {
        if (!node) return false;
        ETCS::Entity* e = ETCS::etcs_resolve_rid_anywhere(&ETCS::getLoader(), node);
        if (!e) return false;
        ETCS::Buffer action((e->getSourceTag().toString() + "." + name).c_str());
        ETCS::Buffer payload(args.c_str());
        try { e->call(action, payload); } catch (...) { return false; }
        return true;
    }

    // ── members ─────────────────────────────────────────────────────────

    ETCS::RID m_world = 0, m_camera = 0, m_card = 0, m_fps = 0, m_arrow = 0, m_arrow_shadow = 0, m_ghost = 0, m_ghost_arrow = 0;
    std::vector<Slot> m_slots;
    // The pools.
    std::vector<ETCS::RID> m_slab_nodes, m_spot_nodes, m_puff_nodes;
    std::vector<int64_t>   m_puff_until;           // drawn only: the tick each puff is free from, -1 free
    std::vector<std::string> m_puff_size;          // ...and the size each was last given ("hid": up, not shown)
    std::vector<PadNodes>    m_pad_nodes;
    std::vector<SpringNodes> m_spring_nodes;
    ETCS::RID m_cup = 0, m_tee = 0, m_cup_rim = 0;
    std::string m_sky_said, m_rim_said;            // drawn: the course's sky and turf as last said
    std::map<ETCS::RID, uint32_t> m_berths;
    std::vector<BoardRow> m_board;
    std::string m_note;

    // The match: the courses, the course out, the hole in play and what is on it.
    std::vector<GolfCourse::Course> m_defs;
    GolfCourse::Course m_live;
    GolfCourse::Hole   m_play;                     // the hole in play, with what powers put down on it
    size_t   m_course_pads = 0, m_course_springs = 0, m_course_slabs = 0;
    int      m_course = 0;
    uint64_t m_layout = 0;
    size_t   m_hole = 0;
    Phase    m_phase = Lobby;
    int64_t  m_end = 0;
    uint32_t m_hole_secs = 120;
    uint64_t m_rng = 0x9E3779B97F4A7C15ull;
    std::vector<int64_t> m_spot_back;              // the tick each spot is full again; 0 full
    std::vector<Effect>  m_effects;
    std::vector<Strike>  m_strikes;                // drawn only
    Fixed    m_max_pull = F(3), m_max_speed = F(12);
    bool     m_ready = false;

    // This runtime's own: the eye, the hand, the card, the board.
    float    m_dist = 7.0f, m_yaw = 0.0f, m_pitch = 0.96f;
    float    m_seen[3] = { 0.0f, 0.0f, 0.0f };
    bool     m_follow = true;
    Mode     m_mode = Idle;
    bool     m_right = false, m_armed = false, m_board_held = false;
    float    m_last_x = 0.0f, m_last_y = 0.0f;
    Fixed    m_pull[3], m_pull_len;
    float    m_pull_pitch = 0.0f;                  // the camera's pitch when the pull began: lower since is loft
    Fixed    m_ghost_at[3];
    bool     m_ghost_ok = false;
    double   m_card_ms = 0.0;
    std::string m_card_text;
    double   m_fps_ms = 0.0;
    int64_t  m_fps_seen = -1;
    // The lobby's menu: where it is, what is being typed, and what its widget was last told.
    ETCS::RID m_menu_rid = 0;
    int      m_menu = 0, m_menu_shown = -1;
    bool     m_menu_open = true, m_editing = false;
    std::string m_edit, m_menu_focus;
    std::vector<std::string> m_menu_sent;
    // The settings (Setting): this runtime's, none of them the match's.
    bool     m_settings = false;
    int      m_set_focus = 0;
    float    m_look_speed = 1.0f, m_zoom_speed = 1.0f;
    size_t   m_menu_lines = 0;
};

#endif // GOLFPROVIDER_GOLFMATCH_H__
