#ifndef GOLFPROVIDER_GOLFGAME_H__
#define GOLFPROVIDER_GOLFGAME_H__

#include "../../../core_defs.h"
#include "../../../ontology.h"

#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>

// ---------------------------------------------------------------------------
// GolfGame -- the hand on the club. A ball in a scene, a camera on it, and a
// pointer: what a drag means, and what letting go does.
//
//   a drag from anywhere but the ball   turns the camera about the ball
//   a drag that starts on the ball      pulls a shot back: the vector opposite
//                                       the drag, as long as the drag (to a
//                                       cap), drawn as an arrow out of the ball
//   letting go of a pull                is the shot: an impulse along it
//   the wheel                           brings the camera in or out
//   R (the key edge)                    the ball back on the tee, the card clear
//
// THE PULL IS READ ON THE VIEW PLANE: the plane through the ball facing the
// camera. A slingshot held up to the eye: the shot is the opposite of where
// the hand went on that plane, so the camera's pitch is the loft -- looking
// down on the ball is a putt, looking level is a lob. No angle is asked for;
// the view is the club.
//
// NOTHING HERE IS PHYSICS. The ball is a RenderProvider Scene3D (a Causal
// solid), the green and the cup are the scene's, and what happens after the
// release is the rows' (ontology/etcs_causal_constraints.md §12-§13): gravity
// is the world's space's, the bounce and the roll are the solid contact's, and
// the cup is a container with a space of its own -- a ball that drops in is
// taken into it by fit, and that move is what "holed" means here. This type
// reads the rows, and writes only through verbs: the shot is the ball's own
// Impulse, recorded like any line of a script.
//
// ACROSS THE MODULE BOUNDARY BY FAMILY AND BY VERB: the ball and the world as
// Causal_ (read, contained), the camera as Camera_ and Drawable2D_ (its pose,
// its frame), and every write as a call on the node's own tag. What this
// module knows of RenderProvider is the names of its verbs.
//
// ANIMATED, for the frame edge: the camera follows the ball while it moves,
// a holed ball is counted and set back on the tee, a lost one too.
// ---------------------------------------------------------------------------
class GolfGame : public AnimatedBase<GolfGame>,
                 public DeletableBase<GolfGame>
{
public:
    WIRE_TYPE_IDENTITY(GolfGame);

    GolfGame() = default;
    bool DeleteConcrete() override { return true; }

    bool Create() { this->addTag("active"); return true; }

    void BindWorld(ETCS::RID r)  { std::lock_guard<std::mutex> g(m_mu); m_world = r; }
    void BindBall(ETCS::RID r)   { std::lock_guard<std::mutex> g(m_mu); m_ball = r; m_follow = true; }
    void BindCamera(ETCS::RID r) { std::lock_guard<std::mutex> g(m_mu); m_camera = r; m_follow = true; }
    void BindCup(ETCS::RID r)    { std::lock_guard<std::mutex> g(m_mu); m_cup = r; }
    void BindArrow(ETCS::RID r)  { std::lock_guard<std::mutex> g(m_mu); m_arrow = r; }
    void BindHud(ETCS::RID r)    { std::lock_guard<std::mutex> g(m_mu); m_hud = r; m_hud_text.clear(); }

    // Where the ball goes back to, in the world's frame.
    void SetTee(float x, float y, float z) { std::lock_guard<std::mutex> g(m_mu); m_tee[0] = x; m_tee[1] = y; m_tee[2] = z; }
    // The longest pull (scene units) and the speed it gives (units/s); a
    // pull's speed is proportional to its length.
    void SetPower(float max_pull, float max_speed)
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (max_pull > 0.0f)  m_max_pull = max_pull;
        if (max_speed > 0.0f) m_max_speed = max_speed;
    }
    // The camera's distance from the ball and its bearing, in degrees.
    void SetOrbit(float distance, float yaw_deg, float pitch_deg)
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (distance > 0.0f) m_dist = clampf(distance, kMinDist, kMaxDist);
        m_yaw   = yaw_deg * kPi / 180.0f;
        m_pitch = clampf(pitch_deg * kPi / 180.0f, kMinPitch, kMaxPitch);
        m_follow = true;
    }

    // The ball back on the tee, still, and the card cleared.
    void Reset()
    {
        std::lock_guard<std::mutex> g(m_mu);
        teeLocked();
        m_strokes = 0;
        m_holed = false;
        m_message.clear();
        hudLocked();
    }
    uint32_t Strokes() const { return m_strokes; }
    bool     Holed()   const { return m_holed; }

    // ── the pointer ────────────────────────────────────────────────────────
    //
    // The stream's events, one at a time; Press/Drag/Release/Wheel are the
    // same events as verbs, so a script (a smoke test, a demo) can play a
    // stroke with no window at all.
    void Press(float x, float y)   { Pointer(event(INPUT_BUTTON_DOWN, x, y)); }
    void Drag(float x, float y)    { Pointer(event(INPUT_MOTION, x, y)); }
    void Release(float x, float y) { Pointer(event(INPUT_BUTTON_UP, x, y)); }
    void Wheel(float notches)      { Pointer(event(INPUT_SCROLL, 0.0f, notches)); }
    // What a pull would shoot right now, for a reader: x y z length.
    std::string PullReport()
    {
        std::lock_guard<std::mutex> g(m_mu);
        return std::to_string(m_pull[0]) + " " + std::to_string(m_pull[1]) + " " + std::to_string(m_pull[2]) + " "
             + std::to_string(m_pull_len) + (m_mode == Pull ? " pulling" : (m_mode == Orbit ? " orbiting" : " idle"));
    }

    void Pointer(const InputEvent& ev)
    {
        std::lock_guard<std::mutex> g(m_mu);
        const float x = ev.x, y = ev.y;
        switch (ev.action)
        {
            case INPUT_BUTTON_DOWN:
                if (m_mode != Idle) break;
                m_button = ev.key;
                m_last_x = x; m_last_y = y;
                m_mode = (ev.key == 0 && onBallLocked(x, y) && restingLocked()) ? Pull : Orbit;
                m_pull_len = 0.0f;
                break;
            case INPUT_MOTION:
            {
                // A release seen by someone else (let go outside the window):
                // the platform says the button is up, so the drag is over. A
                // pull ended unseen is put down, not shot -- where it was let
                // go is not known.
                if (m_mode != Idle && input_button_state(ev, m_button) == 0)
                {
                    if (m_mode == Pull) { m_pull_len = 0.0f; if (m_arrow) verb(m_arrow, "SetVisible", "0"); }
                    m_mode = Idle;
                    break;
                }
                if (m_mode == Orbit)
                {
                    m_yaw  += (x - m_last_x) * kTurn;
                    m_pitch = clampf(m_pitch + (y - m_last_y) * kTurn, kMinPitch, kMaxPitch);
                    m_follow = true;
                    placeCameraLocked();
                }
                else if (m_mode == Pull) pullLocked(x, y);
                m_last_x = x; m_last_y = y;
                break;
            }
            case INPUT_BUTTON_UP:
                if (m_mode == Idle || ev.key != m_button) break;
                if (m_mode == Pull) shootLocked();
                m_mode = Idle;
                break;
            case INPUT_SCROLL:
                // y is the wheel's delta: up brings the camera in.
                m_dist = clampf(m_dist * std::exp(-0.1f * static_cast<float>(ev.y)), kMinDist, kMaxDist);
                m_follow = true;
                placeCameraLocked();
                break;
            default: break;
        }
    }

    // The key edge: R puts the ball back on the tee and clears the card.
    void Key(uint16_t key, bool down)
    {
        if (down && (key == 'R' || key == 'r')) Reset();
    }

    // ── Animated: the frame edge ───────────────────────────────────────────

    bool AnimatingConcrete() override { return m_ball != 0; }
    void AdvanceConcrete(double dt_ms) override
    {
        std::lock_guard<std::mutex> g(m_mu);
        float b[3];
        if (!ballLocked(b)) return;
        // Follow: the camera keeps its bearing on the ball, wherever it goes.
        const float moved = std::fabs(b[0] - m_seen[0]) + std::fabs(b[1] - m_seen[1]) + std::fabs(b[2] - m_seen[2]);
        if (m_follow || moved > 1e-4f)
        {
            m_seen[0] = b[0]; m_seen[1] = b[1]; m_seen[2] = b[2];
            m_follow = false;
            placeCameraLocked();
        }
        // Holed: fit took the ball into the cup's space.
        if (!m_holed && m_cup && parentOfBallLocked() == m_cup)
        {
            m_holed = true;
            m_wait_ms = 0.0;
            m_message = "Holed in " + std::to_string(m_strokes) + (m_strokes == 1 ? " stroke" : " strokes");
            hudLocked();
        }
        if (m_holed)
        {
            m_wait_ms += dt_ms;
            if (m_wait_ms >= kHoledMs)
            {
                teeLocked();
                m_strokes = 0;
                m_holed = false;
                m_message.clear();
                hudLocked();
            }
        }
        // Lost: off the green and falling. Back on the tee, a stroke added.
        else if (b[1] < kLostY)
        {
            teeLocked();
            ++m_strokes;
            m_message = "Out of bounds: +1";
            hudLocked();
        }
    }

private:
    enum Mode { Idle, Orbit, Pull };
    static constexpr float  kPi       = 3.14159265358979f;
    static constexpr float  kTurn     = 0.006f;            // radians per pixel of an orbit drag
    static constexpr float  kMinPitch = 0.05f, kMaxPitch = 1.45f;
    static constexpr float  kMinDist  = 2.0f,  kMaxDist  = 60.0f;
    static constexpr float  kLostY    = -25.0f;
    static constexpr double kHoledMs  = 2500.0;
    static constexpr float  kMinPull  = 0.05f;             // shorter is a click, not a shot

    static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
    static InputEvent event(uint8_t action, float x, float y)
    {
        InputEvent e{};
        e.action = action;
        e.key = 0;
        e.x = static_cast<int16_t>(x); e.y = static_cast<int16_t>(y);
        e.buttons = action == INPUT_BUTTON_UP ? 0 : 1;
        return e;
    }

    // ── reading the scene ────────────────────────────────────────────────

    // The ball's centre in the world's frame, and its reach.
    bool ballLocked(float out[3], float* radius = nullptr)
    {
        ETCS::Held<Causal_> ball = ETCS::resolve_held<Causal_>("Causal", m_ball);
        if (!ball) return false;
        Fixed bx, by, bz;
        ball->Basis(bx, by, bz);
        std::lock_guard<std::recursive_mutex> lk(ball->TreeMutex());
        const OrderVector& o = ball->Order4();
        out[0] = (bx + o.x).ToFloat(); out[1] = (by + o.y).ToFloat(); out[2] = (bz + o.z).ToFloat();
        if (radius) *radius = o.radius.ToFloat();
        // The world's own frame: positions given to world members are
        // relative to it.
        if (ETCS::Held<Causal_> w = ETCS::resolve_held<Causal_>("Causal", m_world))
        {
            Fixed wx, wy, wz;
            w->Basis(wx, wy, wz);
            const OrderVector& wo = w->Order4();
            m_world_at[0] = (wx + wo.x).ToFloat(); m_world_at[1] = (wy + wo.y).ToFloat(); m_world_at[2] = (wz + wo.z).ToFloat();
        }
        return true;
    }
    bool restingLocked()
    {
        ETCS::Held<Causal_> ball = ETCS::resolve_held<Causal_>("Causal", m_ball);
        if (!ball) return false;
        std::lock_guard<std::recursive_mutex> lk(ball->TreeMutex());
        Fixed vx, vy, vz;
        ball->Order4().Velocity(Fixed::One(), vx, vy, vz);
        return Fixed::Length(vx, vy, vz) < Fixed::From(0.05);
    }
    ETCS::RID parentOfBallLocked()
    {
        ETCS::Held<Causal_> ball = ETCS::resolve_held<Causal_>("Causal", m_ball);
        if (!ball) return 0;
        ETCS::Entity* p = static_cast<ETCS::Entity*>(ball.get())->getParent();
        return p ? p->getRID() : 0;
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

    // Does the pointer's line pass through the ball? A little generous: a ball
    // is small on the screen, and a press that grazes it means it.
    bool onBallLocked(float px, float py)
    {
        float b[3], r = 0.0f;
        Point3D o, d;
        if (!ballLocked(b, &r) || !rayLocked(px, py, o, d)) return false;
        const float ox = b[0] - o.x, oy = b[1] - o.y, oz = b[2] - o.z;
        const float t = ox * d.x + oy * d.y + oz * d.z;
        if (t < 0.0f) return false;
        const float cx = ox - d.x * t, cy = oy - d.y * t, cz = oz - d.z * t;
        const float rr = r * 1.6f;
        return cx * cx + cy * cy + cz * cz <= rr * rr;
    }

    // ── the camera ─────────────────────────────────────────────────────────

    void placeCameraLocked()
    {
        float b[3];
        if (!ballLocked(b)) return;
        ETCS::Held<Camera_> cam = ETCS::resolve_held<Camera_>("Camera", m_camera);
        if (!cam) return;
        ViewFrustum v = cam->GetView();
        const float cp = std::cos(m_pitch);
        v.position = Point3D{ b[0] - m_dist * cp * std::sin(m_yaw), b[1] + m_dist * std::sin(m_pitch), b[2] - m_dist * cp * std::cos(m_yaw) };
        v.look_at  = Point3D{ b[0], b[1], b[2] };
        v.up       = Point3D{ 0.0f, 1.0f, 0.0f };
        cam->SetView(v);
    }

    // ── the pull and the shot ──────────────────────────────────────────────

    // Where the pointer's line meets the plane through the ball facing the
    // camera; the pull is from there back through the ball.
    void pullLocked(float px, float py)
    {
        float b[3], r = 0.0f;
        Point3D o, d;
        if (!ballLocked(b, &r) || !rayLocked(px, py, o, d)) return;
        ViewFrustum v;
        {
            ETCS::Held<Camera_> cam = ETCS::resolve_held<Camera_>("Camera", m_camera);
            if (!cam) return;
            v = cam->GetView();
        }   // no hold kept across the arrow's verbs, which emit
        float n[3] = { v.look_at.x - v.position.x, v.look_at.y - v.position.y, v.look_at.z - v.position.z };
        const float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (nl <= 0.0f) return;
        n[0] /= nl; n[1] /= nl; n[2] /= nl;
        const float den = d.x * n[0] + d.y * n[1] + d.z * n[2];
        if (std::fabs(den) < 1e-6f) return;
        const float t = ((b[0] - o.x) * n[0] + (b[1] - o.y) * n[1] + (b[2] - o.z) * n[2]) / den;
        if (t <= 0.0f) return;
        float s[3] = { b[0] - (o.x + d.x * t), b[1] - (o.y + d.y * t), b[2] - (o.z + d.z * t) };   // opposite the drag
        float len = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
        if (len > m_max_pull) { for (float& c : s) c *= m_max_pull / len; len = m_max_pull; }
        m_pull[0] = s[0]; m_pull[1] = s[1]; m_pull[2] = s[2];
        m_pull_len = len;
        arrowLocked(b, r);
    }

    // The arrow: out of the ball along the pull, as long as it, coloured from
    // a gentle green to a hard red by how much of the cap it is.
    void arrowLocked(const float b[3], float r)
    {
        if (!m_arrow) return;
        if (m_pull_len < kMinPull) { verb(m_arrow, "SetVisible", "0"); return; }
        const float u[3] = { m_pull[0] / m_pull_len, m_pull[1] / m_pull_len, m_pull[2] / m_pull_len };
        const float mid = r + m_pull_len * 0.5f;
        const float w = 0.12f + 0.08f * m_pull_len / m_max_pull;
        const float k = m_pull_len / m_max_pull;
        verb(m_arrow, "Create", f3(w, m_pull_len, w));
        verb(m_arrow, "SetPosition", f3(b[0] + u[0] * mid - m_world_at[0], b[1] + u[1] * mid - m_world_at[1], b[2] + u[2] * mid - m_world_at[2]));
        verb(m_arrow, "Aim", f3(u[0], u[1], u[2]));
        verb(m_arrow, "SetColor", f3(0.25f + 0.7f * k, 0.85f - 0.6f * k, 0.25f) + ", 1");
        verb(m_arrow, "SetVisible", "1");
    }

    void shootLocked()
    {
        if (m_arrow) verb(m_arrow, "SetVisible", "0");
        if (m_pull_len < kMinPull) return;
        // Speed proportional to the pull; the impulse is its kinetic energy
        // (the ball's mass is one), along the pull.
        const float speed = m_max_speed * m_pull_len / m_max_pull;
        const float joules = 0.5f * speed * speed;
        verb(m_ball, "Impulse", f3(m_pull[0], m_pull[1], m_pull[2]) + ", " + std::to_string(joules));
        ++m_strokes;
        m_message.clear();
        hudLocked();
        m_pull_len = 0.0f;
    }

    // ── writes, through the nodes' own verbs ───────────────────────────────

    void teeLocked()
    {
        if (!m_ball) return;
        // Out of the cup (or wherever fit put it) and back into the world: a
        // move, which emits, so by bare pointers used at once rather than
        // under a lifetime hold.
        if (parentOfBallLocked() != m_world)
        {
            Causal_* ball  = ETCS::resolve_in_family<Causal_>("Causal", m_ball);
            Causal_* world = ETCS::resolve_in_family<Causal_>("Causal", m_world);
            if (ball && world) world->Contain(ball);
        }
        verb(m_ball, "Halt", "");
        verb(m_ball, "SetPosition", f3(m_tee[0], m_tee[1], m_tee[2]));
        m_follow = true;
    }
    void hudLocked()
    {
        if (!m_hud) return;
        std::string t = "Strokes: " + std::to_string(m_strokes);
        if (!m_message.empty()) t += "   " + m_message;
        if (t == m_hud_text) return;
        m_hud_text = t;
        verb(m_hud, "SetText", t);
    }

    static std::string f3(float a, float b, float c)
    {
        return std::to_string(a) + ", " + std::to_string(b) + ", " + std::to_string(c);
    }
    // A verb on a node, by its own tag: what this module knows of the module
    // that made it is the verb's name.
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
    ETCS::RID  m_world = 0, m_ball = 0, m_camera = 0, m_cup = 0, m_arrow = 0, m_hud = 0;
    float      m_tee[3] = { 0.0f, 0.5f, 0.0f };
    float      m_world_at[3] = { 0.0f, 0.0f, 0.0f };
    float      m_max_pull = 4.0f, m_max_speed = 14.0f;
    float      m_dist = 9.0f, m_yaw = 0.0f, m_pitch = 0.45f;
    Mode       m_mode = Idle;
    uint16_t   m_button = 0;                               // the one that started the drag
    float      m_last_x = 0.0f, m_last_y = 0.0f;
    float      m_pull[3] = { 0.0f, 0.0f, 0.0f };
    float      m_pull_len = 0.0f;
    float      m_seen[3] = { 0.0f, 0.0f, 0.0f };
    bool       m_follow = true;
    uint32_t   m_strokes = 0;
    bool       m_holed = false;
    double     m_wait_ms = 0.0;
    std::string m_message, m_hud_text;
};

#endif // GOLFPROVIDER_GOLFGAME_H__
