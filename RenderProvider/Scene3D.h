#ifndef SCENE3D_H__
#define SCENE3D_H__

#include "../../core_defs.h"
#include "../../ontology.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

// ---------------------------------------------------------------------------
// Scene3D — the Drawable3D leaf, and the counterpart of PolygonDrawable2D one
// dimension up.
//
// SELF-SIMILAR, exactly as the 2D tree is. There is no "scene" class and no
// "object" class: a scene IS a box, and the box you call Project on is simply
// the one nobody nested inside anything else. A child states its extent
// relative to its parent's min corner, which is the 3D reading of the same
// rule Drawable2D states about Bounds -- so moving a container moves its
// subtree with nothing to recompute, and that is the entire mechanism behind
// WASD moving the whole scene by translating one box.
//
// WHAT PROJECTION MEANS HERE. Project fills the camera -- it does not return
// a picture of the scene, it returns the camera, because the camera IS the
// image plane (ontology/Camera.h). Everything this class does with pixels it
// does through the camera's own Pixels_, reached by family name, so a camera
// from another module would be filled identically.
//
// DEPTH IS THIS FAMILY'S, and this is where it is actually paid for. The
// buffer below resolves occlusion between boxes with NO tree relationship at
// all -- two siblings, or a child of one subtree in front of the root of
// another -- which the graph cannot answer and a painter's-order sort can
// only approximate. DepthFor and DepthAt read out of the same projection that
// produced the frame, which is what makes the three answers agree rather than
// merely resemble each other.
//
// ONE WALK, NOT A RECURSION THROUGH Project. The node Project is called on
// collects its whole Scene3D subtree with absolute origins and rasterises the
// lot against one depth buffer. Recursing through the family's own Project
// would give each node its own buffer and therefore its own private idea of
// what is in front, which is precisely the thing depth exists to prevent. A
// FOREIGN Drawable3D child -- a leaf from a module this one has never heard
// of -- is projected through the family interface instead, and it owns its
// own occlusion; that is a real limitation, stated rather than hidden.
//
// THE KEY BITSET is a TBuffer<NUM_KEYS/8>: one bit per key in the ontology's
// whole key spectrum (ontology/InputSource.h), set on a down event and
// cleared on an up. It is deliberately a bitset rather than a "current
// direction", because holding W and A is two facts, not a third one, and a
// consumer that stored the resultant could not answer which key released.
// The stream edge writes it; StepFromHeld reads it and integrates. See
// RenderProvider.h's ConsumeInput for the edge itself.
// ---------------------------------------------------------------------------
class Scene3D : public Drawable3DBase<Scene3D>,
                public DeletableBase<Scene3D>,
                public LifecycleBase<Scene3D>
{
public:
    WIRE_TYPE_IDENTITY(Scene3D);

    // --- Orderable_ (required by Surface, which Drawable refines) ---
    int32_t m_order = 0;
    bool operator<(const Scene3D& o) const { return m_order < o.m_order; }
    int32_t Order() override { return m_order; }

    Scene3D()  = default;
    ~Scene3D() = default;

    // The centre, read out of row 0. Every reader below goes through this
    // rather than holding its own copy -- one position, one writer.
    Point3D Pos() const { return Point3D{ m_ov.x, m_ov.y, m_ov.z }; }
    const OrderVector& Order4() const { return m_ov; }

    // A box centred on its own origin, so SetPosition places the CENTRE --
    // which is what a script means by "put the cube here", and what keeps
    // rotating or scaling it later from also moving it.
    bool Create(float w, float h, float d)
    {
        if (w <= 0.0f || h <= 0.0f || d <= 0.0f)
        {
            ETCS_LOG("Scene3D", "Create with a non-positive extent ("
                     << w << "x" << h << "x" << d << ").");
            return false;
        }
        m_half = Point3D{ w * 0.5f, h * 0.5f, d * 0.5f };
        // Row 0's fourth slot: identity is a coordinate, so it is filled in
        // where the point starts existing rather than derived at each read.
        m_ov.rid = getRID();
        // Row 2's fourth slot. A box is not a point -- it is already an
        // aggregate over the volume it occupies -- so its reach is its own
        // bounding sphere, and it is an aggregate from the moment it exists.
        // Only a node with no extent is a leaf here.
        m_ov.radius = std::sqrt(m_half.x * m_half.x + m_half.y * m_half.y + m_half.z * m_half.z);
        this->addTag("active");
        return true;
    }

    // A teleport, and deliberately not a motion: it moves the point without
    // touching what the point is carrying (OrderVector::PlaceAt).
    void SetPosition(float x, float y, float z)
    {
        m_ov.PlaceAt(x, y, z);
        markViewersDirty();
    }
    void Move(float dx, float dy, float dz)
    {
        m_ov.PlaceAt(m_ov.x + dx, m_ov.y + dy, m_ov.z + dz);
        markViewersDirty();
    }

    // Energy in and energy out, exposed because they are the primitive the
    // input edge drives and the only honest way to script a push.
    void Impulse(float dx, float dy, float dz, float joules)
    {
        m_ov.Impulse(dx, dy, dz, joules);
    }
    void Halt() { m_ov.Rest(); }

    // How fast heat leaves this point into whatever contains it, per second.
    // Zero means a perfect insulator, which is a legitimate thing to be and
    // the reason this is not hardcoded: a scene root modelling open air and a
    // sealed box in it are the same class with different numbers.
    void SetEmissivity(float per_sec)
    {
        if (per_sec >= 0.0f) m_emissivity = per_sec;
    }
    float Emissivity() const { return m_emissivity; }
    float EmittedToEnvironment() const { return m_emitted_out; }

    // THIS NODE'S OWN CLOCK: the number of entropy emissions it has committed.
    // Not a diagnostic counter -- it is the entity's local time, emitted
    // rather than received (ontology/OrderVector.h). A node holding still with
    // nothing left to shed stops ticking, which is the right answer for a
    // thing to which nothing is happening.
    uint64_t CausalTicks() const { return m_ticks; }

    // The most recent crossing, as the OrderVector it is. What a guard nesting
    // a generator capture inside this node's emission would read -- reserved,
    // and readable from the shell today (Scene3D.Order).
    const OrderVector& LastEmission() const { return m_last_emission; }
    void SetColor(float r, float g, float b, float a)
    {
        m_color[0] = r; m_color[1] = g; m_color[2] = b; m_color[3] = a;
        markViewersDirty();
    }
    void SetOrder(int32_t z) { m_order = z; Reorder(); markViewersDirty(); }

    // Terminal speed, in scene units per SECOND -- not per tick. The right
    // value is a property of the scene's scale, which the script knows and
    // this class cannot guess; per-second is what makes it independent of how
    // often the input edge happens to run.
    void SetSpeed(float units_per_sec)
    {
        if (units_per_sec > 0.0f) m_speed = units_per_sec;
    }
    float Speed() const { return m_speed; }

    // How fast motion bleeds off, per second. It sets BOTH halves of the feel
    // at once and that is not a coincidence: with a fixed terminal speed, the
    // acceleration needed to reach it is speed * damping, so one number gives
    // you the coast and the responsiveness together. High damping is crisp and
    // stops dead; low damping drifts.
    void SetDamping(float per_sec)
    {
        if (per_sec > 0.0f) m_damping = per_sec;
    }
    float Damping() const { return m_damping; }

    // Whether this node is drawn at all. A node with no extent still has
    // children, and turning one off is how a script hides a subtree without
    // destroying it -- the children's coordinates stay relative to a box
    // that is still there.
    void SetVisible(bool on) { m_visible = on; markViewersDirty(); }

    // ── the held-key bitset ──────────────────────────────────────────────
    //
    // One bit per key, indexed by the key code itself. No mapping table and
    // no bounds surprise: a code outside the spectrum is dropped here rather
    // than reaching an array.

    void KeyDown(uint16_t key)
    {
        if (key >= NUM_KEYS) return;
        m_held.buf[key >> 3] |= static_cast<char>(1u << (key & 7u));
        publishMotionBits();
    }
    void KeyUp(uint16_t key)
    {
        if (key >= NUM_KEYS) return;
        m_held.buf[key >> 3] &= static_cast<char>(~(1u << (key & 7u)));
        publishMotionBits();
    }
    bool Held(uint16_t key) const
    {
        if (key >= NUM_KEYS) return false;
        return (m_held.buf[key >> 3] & static_cast<char>(1u << (key & 7u))) != 0;
    }
    void ClearHeld() { m_held.clear(); publishMotionBits(); }

    /*
 * THE LOOK, from pointer deltas -- the other half of what the input edge
 * carries, arriving through the same stream and handled in the same work
 * function (RenderProvider.h's ConsumeInput).
 *
 * The orientation lives on the scene's OWN row 3 (ontology/OrderVector.h) and
 * is applied to whichever camera projects it. That reads oddly for a moment --
 * a scene setting a camera's pose -- and then stops: what the input names is
 * the RELATION between the viewer and the world, which is the same quantity
 * whether you turn the head or turn the room. The scene is what the input is
 * bound to, so the scene is where the relation lives, and it lives in the row
 * that exists to hold an angle.
 *
 * RECORDED HERE, COMPOSED AT THE OBSERVER. This function only accumulates two
 * pending angles; the rotation is folded into row 3 in Project, on the frame
 * thread. Same division as the keys: the input edge records, the observer
 * integrates, and the only thing crossing threads is a small atomic.
 */
    void PointerDelta(int32_t dx, int32_t dy)
    {
        if (dx == 0 && dy == 0) return;
        const float rad = radiansPerPixel();
        // Both negated: moving the mouse right turns the view right, which
        // means the WORLD swings left, and this is the world's angle.
        addPending(m_pending_yaw,   -static_cast<float>(dx) * rad);
        addPending(m_pending_pitch, -static_cast<float>(dy) * rad);
        m_look_dirty.store(true, std::memory_order_relaxed);
    }

    /*
 * SENSITIVITY AS A PHYSICAL RATIO, which is what the first two attempts got
 * wrong.
 *
 * The turn rate is derived, not chosen:
 *
 *     radians per count = 2*pi * turns * screen_dpi
 *                         ---------------------------
 *                          mouse_dpi * frame_width_px
 *
 * ONE TURN PER PASS is the standard: dragging the pointer across the width of
 * the window rotates the view through exactly 2*pi and brings you back where
 * you started. It is the only ratio with an obvious meaning, and it is what
 * makes the window itself the unit rather than a number somebody picked.
 *
 * Read it as a chain of unit conversions and it is obvious. A pointer event
 * carries COUNTS; counts / mouse_dpi is INCHES of hand movement;
 * frame_width_px / screen_dpi is the INCHES the frame occupies on the glass;
 * so the fraction is "what share of a screen-width did the hand travel", and
 * multiplying by turns gives the angle. Nothing in it is a tuned constant.
 *
 * WHY THE EARLIER FORMS WERE TOO FAST. "4*pi / frame_width" treats one
 * DELTA UNIT as one screen pixel. That is true only if the deltas are
 * unaccelerated screen units at the screen's own scale -- which they are
 * not: with acceleration they are several times larger, and with raw motion
 * they are counts, of which a 1600-DPI mouse emits sixteen hundred per inch
 * against a screen's ninety-six. Either way the hand crosses the screen in a
 * small fraction of a screen-width's worth of units, and the view spins.
 *
 * MOUSE DPI IS A PARAMETER, NOT A MEASUREMENT, and that is a real limit
 * rather than an omission: neither X11, Wayland, Win32 nor GLFW exposes a
 * pointing device's counts-per-inch. The default is 800 because it is the
 * most common stock setting.
 *
 * IT IS ALSO THE ONE THING THAT CAN STILL MAKE THIS FEEL WRONG, and the error
 * is exactly linear: a 3200-DPI mouse against an assumed 800 emits four times
 * the counts for the same movement, so the view turns four times too fast and
 * nothing else in the chain is at fault. If it overshoots by roughly N, the
 * mouse is roughly N * 800 DPI -- SetMouseDpi is the whole of the correction,
 * and setting it right makes every other number here true.
 */
    void SetMouseDpi(float dpi)  { if (dpi  > 0.0f) m_mouse_dpi  = dpi; }
    void SetScreenDpi(float dpi) { if (dpi  > 0.0f) m_screen_dpi = dpi; }
    void SetTurnsPerPass(float t){ if (t    > 0.0f) m_turns      = t; }
    void SetSensitivity(float scale) { if (scale > 0.0f) m_sens_scale = scale; }

    float MouseDpi() const        { return m_mouse_dpi; }
    float ScreenDpi() const       { return m_screen_dpi; }
    float TurnsPerPass() const    { return m_turns; }
    float Sensitivity() const     { return m_sens_scale; }
    float RadiansPerCount() const { return radiansPerPixel(); }

    // Restart the pipeline after a key change. A scene at rest marks nothing,
    // so nothing re-renders, so nothing calls Interact and the first keypress
    // would never take effect -- one mark is what closes that loop back up.
    // After it, the motion sustains its own frames until it settles.
    void WakeObservers() { markViewersDirty(); }

    /*
 * Is this subtree still producing new images?
 *
 * True while a key is held or the point still carries kinetic energy -- so it
 * covers the coast after release, not just the push, which is the half a
 * dirty flag alone gets wrong.
 *
 * IT EXISTS BECAUSE A FLAG CANNOT SAY THIS. The dirty flag answers "something
 * changed since you last looked", and its one consumer per mark is what makes
 * it cheap. A scene in motion changes DURING the look: the projection moves
 * the point, marks the compositor, and the surface's own Blit then consumes
 * that mark as its upload signal in the same frame -- correct for both of
 * them, and it leaves nothing behind to schedule the next frame with. So a
 * moving scene has to be asked, not flagged, and being asked is cheap: it is
 * a load and a compare against a number the motion already maintains.
 */
    bool InMotion() const
    {
        return m_motion.load(std::memory_order_relaxed) != 0
            || m_look_dirty.load(std::memory_order_relaxed)
            || m_ov.KineticEnergy() > 0.0f;
    }

    /*
 * ADVANCE THE MOTION AT THE MOMENT IT IS OBSERVED.
 *
 * Integration happens here rather than on a clock of its own, and the reason
 * is not economy: a step taken between two frames is a step nobody can see,
 * and a step taken at projection time is sampled at exactly the rate the
 * result is looked at. dt is the interval since the last projection, so
 * position is a continuous function of elapsed time and a frame that took
 * longer covers proportionally more ground -- which IS the interpolation,
 * done by measuring instead of by guessing between two fixed ticks.
 *
 * It also closes the loop that keeps frames coming. Moving marks the viewers
 * dirty, which is what makes the next frame render, which advances the motion
 * again; when the keys are released and the velocity decays to rest,
 * StepFromHeld stops returning true, nothing is marked, and the whole
 * pipeline goes quiet on its own. No idle spin anywhere, and no thread whose
 * job is to ask whether anything happened.
 */
    /*
 * ONE CAUSAL INTERACTION for this node and everything under it: settle the
 * entropy owed since the last one, then advance the motion.
 *
 * That order is the whole of the lazy-commit contract. Emission is charged
 * for the interval that just ENDED, over a heat total nothing touched during
 * it; drag then adds the heat that the NEXT interval will be charged for. Do
 * it the other way round and every commit bills for heat that had not
 * happened yet when the interval started.
 *
 * Called from Project and nowhere else, because being observed is the causal
 * interaction this system actually has: a node nobody looked at has had no
 * interaction to commit on, and it owes exactly the same amount whenever
 * somebody finally does -- which is what EmissionOver's exponential form
 * guarantees (ontology/OrderVector.h). Queries like DepthFor deliberately do
 * NOT interact: asking how far away something is should not warm the room.
 */
    void Interact()
    {
        commitEntropy();
        AdvanceForObserver();
        for (Scene3D* kid : ownChildren()) kid->Interact();
    }

    void AdvanceForObserver()
    {
        const auto now = std::chrono::steady_clock::now();
        if (m_stepped)
        {
            float dt = std::chrono::duration<float>(now - m_last_step).count();
            // Capped for the same reason any measured timestep is: a stall --
            // a swapped-out thread, a lid closing, a breakpoint -- would
            // otherwise arrive as one enormous step and throw the scene across
            // the map. A capped dt loses time rather than sanity.
            if (dt > 0.1f) dt = 0.1f;
            StepFromHeld(dt);
        }
        m_last_step = now;
        m_stepped   = true;
    }

    /*
 * THE ONE THING THAT CROSSES THREADS, and the reason it is not the bitset
 * itself. The full-spectrum bitset is written by the input edge and read by
 * nothing else; what the PROJECTION needs is six bits of it, and it reads
 * them from another thread entirely (the frame edge). So the six are
 * republished into one atomic word whenever a key changes, and the wide
 * record stays an ordinary array with a single writer.
 *
 * Making the whole 128-byte bitset atomic would be answering a question
 * nobody asked -- no reader wants all of it -- and leaving it plain while
 * two threads touched it would be a data race whose symptom is a key that
 * occasionally sticks. One word is the actual shared state; this is it.
 */
    static constexpr uint32_t MOVE_FWD = 1u << 0;   // W
    static constexpr uint32_t MOVE_BCK = 1u << 1;   // S
    static constexpr uint32_t MOVE_LFT = 1u << 2;   // A
    static constexpr uint32_t MOVE_RGT = 1u << 3;   // D
    static constexpr uint32_t MOVE_UP  = 1u << 4;   // Q
    static constexpr uint32_t MOVE_DWN = 1u << 5;   // E

    /*
 * Advance the motion by dt seconds. Returns true if the node actually moved,
 * so the caller can leave a settled scene alone rather than marking it dirty
 * sixty times a second for a displacement of zero.
 *
 * THE SCENE MOVES, NOT THE CAMERA, and that is the whole trick: what a
 * projection can see is the RELATIVE position of the two, and moving the
 * scene means one translation at the root relocates every box in the tree
 * for free (the coordinate rule), with no second copy of anyone's position
 * to keep in step. W/S is depth, A/D lateral, Q/E vertical -- the scene
 * going one way reads as the viewer going the other, which is why W pushes
 * the scene AWAY.
 *
 * HELD KEYS ARE AN ACCELERATION, NOT A DISPLACEMENT. Adding a fixed step per
 * tick is teleportation dressed as movement: the scene jumps the instant a
 * key goes down, jumps to a dead stop the instant it comes up, and moves at
 * whatever speed the tick rate happens to be. So a held key applies an
 * impulse, velocity carries, and drag brings it back down -- which is both
 * the acceleration and the interpolation, since position becomes a
 * continuous function of elapsed time rather than a sum of tick-sized
 * jumps. Release a key and it coasts to rest instead of stopping mid-air.
 *
 * DRAG IS INTEGRATED EXACTLY, exp(-k*dt) rather than v -= v*k*dt. The
 * difference matters here because dt is measured, not assumed: the explicit
 * form is only stable while k*dt < 1, and one scheduling hiccup on a
 * loaded machine is enough to make it overshoot into oscillation. The exact
 * form is unconditionally stable and costs one exp per tick.
 *
 * THE DIRECTION IS NORMALISED, so W+A is the same speed as W. Summing unit
 * steps per axis makes every diagonal 1.41x faster, which is invisible in a
 * screenshot and immediately obvious to anyone holding two keys.
 */
    bool StepFromHeld(float dt)
    {
        if (!(dt > 0.0f)) return false;

        /*
     * THE SCENE MOVES OPPOSITE THE VIEWER, and that sign is the whole of
     * what W means. W is "the viewer goes forward", so the scene goes
     * BACKWARD along the view -- things get closer and larger. Written the
     * other way it looks equally plausible and is immediately wrong on
     * screen, which is how the two were found swapped: holding W made the
     * world recede.
     *
     * RELATIVE TO THE FACING, not to the world axes. Once the mouse can
     * turn the view, a fixed-axis W walks sideways the moment you look
     * anywhere but down +z. So the input is built in the viewer's own frame
     * and rotated by yaw into the scene's.
     *
     * YAW ONLY, deliberately: looking up should not make W climb. Pitch
     * aims the view; the feet stay on the plane, which is what every ground
     * control does and the reason Q/E exist for the axis it leaves out.
     */
        const uint32_t bits = m_motion.load(std::memory_order_relaxed);
        float fwd_in = 0.0f, right_in = 0.0f, up_in = 0.0f;
        if (bits & MOVE_FWD) fwd_in   += 1.0f;
        if (bits & MOVE_BCK) fwd_in   -= 1.0f;
        if (bits & MOVE_RGT) right_in += 1.0f;
        if (bits & MOVE_LFT) right_in -= 1.0f;
        if (bits & MOVE_UP)  up_in    += 1.0f;
        if (bits & MOVE_DWN) up_in    -= 1.0f;

        // The viewer's GROUND frame: the current facing (row 3 applied to the
        // reference forward) flattened onto the horizontal plane, and right =
        // up x forward, the same handedness buildView uses.
        //
        // Flattened rather than used whole, which is the pitch exclusion made
        // concrete: looking up should aim the view, not lift the feet. When
        // the facing is near-vertical the horizontal part vanishes and the
        // last usable frame is kept, so walking while staring at the sky is
        // still walking somewhere.
        float fx = m_ref_fwd.x, fyv = m_ref_fwd.y, fz = m_ref_fwd.z;
        m_ov.RotateVector(fx, fyv, fz);
        const float fl = std::sqrt(fx * fx + fz * fz);
        if (fl > 1e-4f) { fx /= fl; fz /= fl; m_ground_fx = fx; m_ground_fz = fz; }
        else            { fx = m_ground_fx;   fz = m_ground_fz; }
        const float rx = fz, rz = -fx;

        float dx = -(fx * fwd_in + rx * right_in);
        float dy = -up_in;
        float dz = -(fz * fwd_in + rz * right_in);

        const float mag = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (mag > 0.0f)
        {
            /*
     * The impulse that lands exactly on SetSpeed's terminal and no higher.
     *
     * Kinetic energy at the terminal speed is 1/2 m v_max^2, and drag takes
     * a fraction (1 - exp(-k dt)) ~ k dt of it away each tick, so an
     * impulse of 1/2 m v_max^2 * k * dt is what replaces exactly what drag
     * removes at that speed -- the fixed point of the two operations, which
     * is what a terminal speed IS.
     */
            const float joules = 0.5f * m_mass * m_speed * m_speed * m_damping * dt;
            m_ov.Impulse(dx, dy, dz, joules);
        }

        // Drag is a TRANSFER, not a subtraction: kinetic goes down, E stays,
        // and the difference is heat by definition (ontology/OrderVector.h).
        // So the energy this scene has spent being pushed around is still on
        // the point and readable, rather than having quietly left the model.
        m_ov.Dissipate(std::exp(-m_damping * dt));

        // Below this the point is not coasting, it is dithering the last ulps
        // of a decaying float forever -- and every one of those ticks would
        // mark a camera dirty and re-project a frame identical to the last.
        // Coming to rest is what lets a released key actually settle, and it
        // is a real state change rather than a threshold hack: all of the
        // remaining kinetic energy becomes heat.
        float vx, vy, vz;
        m_ov.Velocity(m_mass, vx, vy, vz);
        const float v2   = vx * vx + vy * vy + vz * vz;
        const float rest = m_speed * 1e-3f;
        if (mag == 0.0f && v2 < rest * rest)
        {
            m_ov.Rest();
            return false;
        }
        if (v2 == 0.0f) return false;

        m_ov.Advance(dt, m_mass);
        markViewersDirty();
        return true;
    }

    // ── Drawable3D_ dispatch ─────────────────────────────────────────────

    Box3D Bounds3DConcrete() override
    {
        const Point3D p = Pos();
        return Box3D{ Point3D{ p.x - m_half.x, p.y - m_half.y, p.z - m_half.z },
                      Point3D{ p.x + m_half.x, p.y + m_half.y, p.z + m_half.z } };
    }

    bool ContainsLocal3DConcrete(Point3D p) override
    {
        return std::fabs(p.x) <= m_half.x
            && std::fabs(p.y) <= m_half.y
            && std::fabs(p.z) <= m_half.z;
    }

    // The whole-node span, over the eight corners of the WHOLE SUBTREE --
    // not just this box. A container's depth is the depth of what is in it,
    // which is what makes this usable as the ordering key the family's
    // comment describes: sorting containers by their contents' extent is the
    // only sort that says anything about what is actually in front.
    DepthSpan DepthForConcrete(Camera_* camera) override
    {
        View v;
        if (!buildView(camera, v)) return DepthSpan{-1.0f, -1.0f};

        std::vector<Node> nodes;
        collectSubtree(Point3D{0,0,0}, nodes);

        float lo = 0.0f, hi = 0.0f;
        bool first = true;
        for (const Node& n : nodes)
            for (int i = 0; i < 8; ++i)
            {
                const Point3D c = corner(n, i);
                const float d = (c.x - v.eye.x) * v.fwd.x
                              + (c.y - v.eye.y) * v.fwd.y
                              + (c.z - v.eye.z) * v.fwd.z;
                if (first) { lo = hi = d; first = false; }
                else if (d < lo) lo = d;
                else if (d > hi) hi = d;
            }
        if (first) return DepthSpan{-1.0f, -1.0f};
        return DepthSpan{lo, hi};
    }

    /*
 * Per-pixel depth, read straight out of the buffer the last projection
 * filled -- so it is the depth of what is actually VISIBLE there, after
 * occlusion, not of whichever box happens to be asked first.
 *
 * Negative for a pixel nothing occupies, for a pixel outside the frame, and
 * for a camera this node has not projected into. That last one is the honest
 * answer rather than a silent zero: a depth for a view that was never
 * rendered is not a number this node has.
 */
    float DepthAtConcrete(Camera_* camera, int32_t x, int32_t y) override
    {
        if (!camera || camera->getRID() != m_depth_cam) return -1.0f;
        if (x < 0 || y < 0) return -1.0f;
        if (static_cast<uint32_t>(x) >= m_depth_w || static_cast<uint32_t>(y) >= m_depth_h)
            return -1.0f;
        const float d = m_depth[static_cast<size_t>(y) * m_depth_w + x];
        return (d == kFar) ? -1.0f : d;
    }

    /*
 * THE PROJECTION. Fills the camera's pixels and hands the camera back as
 * the Drawable2D it already is.
 *
 * Order of operations, and each step is load-bearing:
 *   1. read the pose and lens off the camera, build an orthonormal basis
 *   2. size the depth buffer to the camera's own frame -- the camera says
 *      how big its image plane is, which is why nothing here takes a size
 *   3. clear colour and depth together, because a stale depth value with a
 *      fresh colour is how a frame ends up with holes in it
 *   4. rasterise every box in the subtree against ONE depth buffer
 *   5. register the camera as a viewer, so a later move marks it dirty
 */
    Drawable2D_* ProjectConcrete(Camera_* camera) override
    {
        applyLookTo(camera);
        Interact();

        View v;
        if (!buildView(camera, v)) return nullptr;

        Pixels_* px = cameraPixels(camera);
        if (!px)
        {
            ETCS_LOG("Scene3D", "camera RID:" << camera->getRID()
                     << " owns no pixels -- a projection has nowhere to land.");
            return nullptr;
        }

        m_depth.assign(static_cast<size_t>(v.w) * v.h, kFar);
        m_depth_w   = v.w;
        m_depth_h   = v.h;
        m_depth_cam = camera->getRID();

        std::vector<Node> nodes;
        collectSubtree(Point3D{0,0,0}, nodes);
        coverSubtree(nodes);
        for (const Node& n : nodes) rasterBox(*px, v, n);

        px->MarkDirty();
        registerViewer(camera->getRID());
        ++m_projections;

        // The foreign case, and the reason it is separate: a 3D leaf from
        // another module has its own geometry this walk cannot read, so it
        // is asked to project itself. It gets the same camera and therefore
        // lands in the same pixels, but it brings its own occlusion.
        for (Drawable3D_* alien : foreignChildren())
            alien->Project(camera);

        return cameraPlane(camera);
    }

    // ── Surface_ / Resizable_ dispatch ───────────────────────────────────
    //
    // A scene node has no pixels of its own -- it is geometry, and the only
    // place it becomes an image is a camera's plane. So the surface verbs
    // are honest no-ops rather than an emulation: calling Clear on a box has
    // no meaning that is not already the camera's answer.
    //
    // It is still a Surface, and that is not a formality: it is what lets a
    // scene sit in the same tree, be ordered by the same relation, and be
    // reached by the same family lookups as everything else.

    void ClearConcrete(float, float, float, float) override {}
    void DrawRectConcrete(int32_t, int32_t, uint32_t, uint32_t,
                          float, float, float, float) override {}
    void BlitConcrete(Surface_*, int32_t, int32_t, uint32_t, uint32_t, float) override {}

    // The extent of a box is Box3D, not a width and a height. Reporting a
    // pixel size it does not have would be a number every caller could use
    // and none could trust.
    WindowSize GetSizeConcrete() override { return WindowSize{0, 0}; }

    // Asked to draw itself onto a 2D surface with no camera named, a scene
    // has nothing to say -- which camera's view would it be? The screen sees
    // a scene by drawing the CAMERA, and the camera is an ordinary
    // Drawable2D that nests wherever any other one does.
    void DrawIntoConcrete(Surface_*) override
    {
        ETCS_LOG("Scene3D", "DrawInto on RID:" << getRID()
                 << " -- a scene reaches a 2D surface through a camera; draw the camera.");
    }

    // The same answer a camera gives, from the node the motion is actually on
    // -- so a scene reached through any path reports honestly, not only
    // through the camera that happens to be watching it.
    bool Animating() override { return InMotion(); }

    /*
 * Lifecycle_: stop being a thing other entities are still pointing at.
 *
 * The viewer list is the point. A camera registers itself here when it
 * projects, and this node marks those cameras dirty whenever it moves -- so a
 * scene reclaimed by a closure while a frame edge is still running would keep
 * reaching for cameras that are themselves being torn down. Dropping the list
 * and coming to rest is how it stops participating, and it has to happen while
 * the node is still whole enough to do it.
 */
    void ReleaseConcrete()
    {
        m_viewers.clear();
        m_ov.Rest();
        ClearHeld();
    }

    // ── Deletable_ ───────────────────────────────────────────────────────
    bool DeleteConcrete() override
    {
        std::string conjugate_key = getSourceModule().toString() + ":" + getSourceTag().toString();
        ETCS_LOG("Scene3D", "firing self-DestroyEvent for RID:" << getRID());
        return ETCS::DestroyEvent{conjugate_key.c_str(), this}();
    }

    uint64_t Projections() const { return m_projections; }

private:
    // A box flattened out of the tree: absolute centre, half-extent, colour.
    struct Node
    {
        Point3D pos;
        Point3D half;
        float   color[4];
    };

    // The camera's pose and lens, resolved once per projection into the form
    // the rasteriser actually uses.
    struct View
    {
        Point3D  eye, fwd, right, up;
        float    tan_half, aspect, near_p, far_p;
        uint32_t w, h;
    };

    static constexpr float kFar = std::numeric_limits<float>::infinity();

    // ── camera access, always by family name ─────────────────────────────
    //
    // Camera_ declares the view and the scene and nothing else; its pixels,
    // its extent and its 2D membership are other families' answers. Crossing
    // between them is a lookup, never a cast -- the rule the whole ontology
    // runs on (ontology/Drawable.h).

    static Pixels_* cameraPixels(Camera_* c)
    {
        if (!c) return nullptr;
        void* p = c->getInterfacePointer(ETCS::Buffer("Pixels"));
        return p ? static_cast<Pixels_*>(p) : nullptr;
    }
    static Drawable2D_* cameraPlane(Camera_* c)
    {
        if (!c) return nullptr;
        void* p = c->getInterfacePointer(ETCS::Buffer("Drawable2D"));
        return p ? static_cast<Drawable2D_*>(p) : nullptr;
    }

    bool buildView(Camera_* camera, View& out) const
    {
        if (!camera) return false;
        Drawable2D_* plane = cameraPlane(camera);
        if (!plane) return false;

        const Rect2D frame = plane->Bounds();
        if (frame.w == 0 || frame.h == 0) return false;

        const ViewFrustum v = camera->GetView();
        if (v.far_plane <= v.near_plane || v.near_plane <= 0.0f) return false;
        if (v.fov_y_radians <= 0.0f || v.fov_y_radians >= 3.14159265f) return false;

        Point3D f{ v.look_at.x - v.position.x,
                   v.look_at.y - v.position.y,
                   v.look_at.z - v.position.z };
        if (!normalise(f)) return false;

        // right = up x forward, NOT forward x up. Both produce an orthonormal
        // basis and only one produces the right-handed one: standing at the
        // origin looking down +z with +y overhead, your right hand points at
        // +x, and up x forward is the product that says so. The other order
        // mirrors the image left-to-right -- which looks plausible in a
        // symmetric scene and turns A and D into each other in every other
        // one. Caught exactly that way.
        Point3D r = cross(v.up, f);
        if (!normalise(r)) return false;          // up parallel to forward: no basis
        Point3D u = cross(f, r);                  // already unit: two unit vectors, perpendicular

        out.eye      = v.position;
        out.fwd      = f;
        out.right    = r;
        out.up       = u;
        out.tan_half = std::tan(v.fov_y_radians * 0.5f);
        out.aspect   = static_cast<float>(frame.w) / static_cast<float>(frame.h);
        out.near_p   = v.near_plane;
        out.far_p    = v.far_plane;
        out.w        = frame.w;
        out.h        = frame.h;
        return true;
    }

    // ── the subtree walk ─────────────────────────────────────────────────
    //
    // A child's position is stated relative to its parent's CENTRE, which is
    // the 3D reading of Drawable2D's parent-relative rule, and is why one
    // translation at the root relocates everything below it.
    void collectSubtree(Point3D origin, std::vector<Node>& out)
    {
        const Point3D abs{ origin.x + m_ov.x, origin.y + m_ov.y, origin.z + m_ov.z };
        if (m_visible)
        {
            Node n;
            n.pos  = abs;
            n.half = m_half;
            n.color[0] = m_color[0]; n.color[1] = m_color[1];
            n.color[2] = m_color[2]; n.color[3] = m_color[3];
            out.push_back(n);
        }
        for (Scene3D* kid : ownChildren()) kid->collectSubtree(abs, out);
    }

    // Children of this module's own 3D leaf, which are the ones whose
    // geometry this walk can read. Identified by tag rather than by a cast:
    // the family pointer says "a 3D node", it does not say "one of mine",
    // and reading another module's fields off a family pointer is exactly
    // the mistake the interface-pointer discipline exists to prevent.
    std::vector<Scene3D*> ownChildren()
    {
        std::vector<Scene3D*> out;
        for (ETCS::Entity* e : drawable3DChildren())
            if (isOwnLeaf(e)) out.push_back(static_cast<Scene3D*>(e->getTrueType()));
        return out;
    }
    std::vector<Drawable3D_*> foreignChildren()
    {
        std::vector<Drawable3D_*> out;
        for (ETCS::Entity* e : drawable3DChildren())
            if (!isOwnLeaf(e))
                out.push_back(static_cast<Drawable3D_*>(
                    e->getInterfacePointer(ETCS::Buffer("Drawable3D"))));
        return out;
    }
    static bool isOwnLeaf(ETCS::Entity* e)
    {
        return e && e->getSourceTag() == ETCS::Buffer("Scene3D");
    }
    std::vector<ETCS::Entity*> drawable3DChildren()
    {
        std::vector<ETCS::Entity*> out;
        std::vector<std::pair<ETCS::Buffer, ETCS::RID>> kids;
        getOrderedTypedChildren(kids);
        for (const auto& entry : kids)
        {
            ETCS::Entity* child = getTypedChild(entry.first, entry.second);
            if (!child) continue;
            if (!child->getInterfacePointer(ETCS::Buffer("Drawable3D"))) continue;
            out.push_back(child);
        }
        return out;
    }

    // ── dirty propagation ────────────────────────────────────────────────
    //
    // The 3D counterpart of PolygonDrawable2D's markCompositorsDirty, and it
    // has to walk a different edge: a camera is not an ancestor of the scene
    // it views, it NAMES one. So the scene records who has projected it and
    // marks those, walking to the top of its own subtree first because it is
    // the root of a projection that cameras register against.
    //
    // Registration happens in Project rather than in a setter, which means a
    // camera that has never rendered is never marked -- correct, since it has
    // no image to invalidate.
    void registerViewer(ETCS::RID cam)
    {
        if (cam == 0) return;
        if (std::find(m_viewers.begin(), m_viewers.end(), cam) == m_viewers.end())
            m_viewers.push_back(cam);
    }
    void markViewersDirty()
    {
        Scene3D* root = this;
        for (ETCS::Entity* node = getParent(); node; node = node->getParent())
        {
            if (!node->getInterfacePointer(ETCS::Buffer("Drawable3D"))) break;
            if (!isOwnLeaf(node)) break;
            root = static_cast<Scene3D*>(node->getTrueType());
        }
        for (ETCS::RID cam : root->m_viewers)
        {
            Camera_* c = ETCS::resolve_in_family<Camera_>("Camera", cam);
            if (c) markPixelPath(c);
        }
    }

    /*
 * Mark this node and every pixel-owning ancestor of it.
 *
 * The camera alone is not enough, and the frame that did not change is how
 * you find that out: a camera nested in a compositor is drawn by that
 * compositor's recomposition, and a compositor recomposes only when IT is
 * dirty. Marking the camera and stopping leaves a stale image sitting in a
 * clean parent, which is drawn, correctly, forever.
 *
 * So the walk is the same one PolygonDrawable2D::markCompositorsDirty
 * makes, for the same reason, and it stops at nothing: EVERY pixel owner up
 * the chain holds a merged copy of what changed, so every one of them is
 * out of date. Reached as Entity, since the chain crosses families -- a
 * camera's parent is a compositor, whose parent may be anything.
 */
    static void markPixelPath(ETCS::Entity* node)
    {
        for (; node; node = node->getParent())
        {
            void* p = node->getInterfacePointer(ETCS::Buffer("Pixels"));
            if (p) static_cast<Pixels_*>(p)->MarkDirty();
        }
    }

    // ── geometry helpers ─────────────────────────────────────────────────

    static Point3D cross(const Point3D& a, const Point3D& b)
    {
        return Point3D{ a.y * b.z - a.z * b.y,
                        a.z * b.x - a.x * b.z,
                        a.x * b.y - a.y * b.x };
    }
    static bool normalise(Point3D& v)
    {
        const float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        if (!(len > 1e-6f)) return false;
        v.x /= len; v.y /= len; v.z /= len;
        return true;
    }
    static Point3D corner(const Node& n, int i)
    {
        return Point3D{ n.pos.x + ((i & 1) ? n.half.x : -n.half.x),
                        n.pos.y + ((i & 2) ? n.half.y : -n.half.y),
                        n.pos.z + ((i & 4) ? n.half.z : -n.half.z) };
    }

    // A point in the camera's own frame: x right, y up, z straight ahead.
    // The projection is a division by z, so this is the space where "in front
    // of the eye" is a question with an answer, and it is where the near-plane
    // clip below has to happen -- after the divide there is nothing left to
    // clip against, and a point behind the eye has already been mapped to a
    // mirrored position on the plane as if it were in front.
    static Point3D toView(const View& v, Point3D p)
    {
        const float ex = p.x - v.eye.x, ey = p.y - v.eye.y, ez = p.z - v.eye.z;
        return Point3D{ ex * v.right.x + ey * v.right.y + ez * v.right.z,
                        ex * v.up.x    + ey * v.up.y    + ez * v.up.z,
                        ex * v.fwd.x   + ey * v.fwd.y   + ez * v.fwd.z };
    }

    // Pixel x/y plus the view-space distance that IS the depth.
    struct Vertex { float x, y, z; };

    static Vertex toScreen(const View& v, Point3D p)
    {
        const float ndc_x = (p.x / p.z) / (v.tan_half * v.aspect);
        const float ndc_y = (p.y / p.z) / v.tan_half;
        return Vertex{ (ndc_x * 0.5f + 0.5f) * static_cast<float>(v.w),
                       (0.5f - ndc_y * 0.5f) * static_cast<float>(v.h),
                       p.z };
    }

    /*
 * Sutherland-Hodgman against the single plane z = near, in view space.
 *
 * Written because dropping a triangle with any vertex behind the eye is not
 * a small approximation: it deletes exactly the surfaces you are standing
 * on. A ground plane large enough to reach past the camera has two of its
 * four corners behind it, so BOTH of its triangles go, and a floor
 * disappears entirely the moment you walk onto it -- which is how this got
 * found, with a 30x30 slab rendering as a horizon line.
 *
 * One plane is all that is needed. The other five frustum planes clip
 * against the SCREEN, and the rasteriser's bounding box already does that
 * for free; z = near is the only one whose violation the projection cannot
 * survive, because it is the only one that divides by a number of the wrong
 * sign.
 *
 * Convex in, convex out: three vertices in gives three or four out, which
 * is why the fan below is at most two triangles.
 */
    static int clipNear(const Point3D in[3], float near_p, Point3D out[4])
    {
        int n = 0;
        for (int i = 0; i < 3; ++i)
        {
            const Point3D& a = in[i];
            const Point3D& b = in[(i + 1) % 3];
            const bool a_in = a.z >= near_p;
            const bool b_in = b.z >= near_p;

            if (a_in) out[n++] = a;
            if (a_in != b_in)
            {
                const float t = (near_p - a.z) / (b.z - a.z);
                out[n++] = Point3D{ a.x + (b.x - a.x) * t,
                                    a.y + (b.y - a.y) * t,
                                    near_p };
            }
        }
        return n;   // 0 (wholly behind), 3, or 4
    }

    /*
 * A box as six faces, each two triangles, each depth-tested per pixel.
 *
 * No back-face culling and none wanted: the depth buffer already answers
 * "which surface is in front", and it answers it for faces of DIFFERENT
 * boxes too, which culling never could. The shade per face is what makes a
 * cube read as a solid rather than a silhouette -- flat lighting, one
 * constant per face, because a scene of axis-aligned boxes has exactly six
 * distinct normals and nothing here needs a light to be a thing.
 */
    void rasterBox(Pixels_& px, const View& v, const Node& n)
    {
        Point3D c[8];
        for (int i = 0; i < 8; ++i) c[i] = toView(v, corner(n, i));

        // Corner index bits: 1=+x, 2=+y, 4=+z. Faces wound as two triangles.
        static const int faces[6][4] = {
            {0, 2, 6, 4},   // -x
            {1, 5, 7, 3},   // +x
            {0, 4, 5, 1},   // -y
            {2, 3, 7, 6},   // +y
            {0, 1, 3, 2},   // -z
            {4, 6, 7, 5},   // +z
        };
        static const float shade[6] = { 0.62f, 0.78f, 0.48f, 1.00f, 0.70f, 0.86f };

        for (int f = 0; f < 6; ++f)
        {
            const float s = shade[f];
            const float col[4] = { n.color[0] * s, n.color[1] * s, n.color[2] * s, n.color[3] };
            const int* q = faces[f];
            clipAndFill(px, v, c[q[0]], c[q[1]], c[q[2]], col);
            clipAndFill(px, v, c[q[0]], c[q[2]], c[q[3]], col);
        }
    }

    // Clip in view space, project what survives, fan it. The two steps are
    // separate because they answer different questions and only one of them
    // can be done after the divide -- see clipNear.
    void clipAndFill(Pixels_& px, const View& v,
                     Point3D a, Point3D b, Point3D c, const float col[4])
    {
        const Point3D tri[3] = {a, b, c};
        Point3D poly[4];
        const int n = clipNear(tri, v.near_p, poly);
        if (n < 3) return;

        Vertex s[4];
        for (int i = 0; i < n; ++i)
        {
            if (poly[i].z > v.far_p) return;   // wholly past the far plane
            s[i] = toScreen(v, poly[i]);
        }
        for (int i = 1; i + 1 < n; ++i)
            triangle(px, v, s[0], s[i], s[i + 1], col);
    }

    /*
 * One depth-tested triangle, barycentric over its bounding box.
 *
 * Depth is interpolated as 1/z and inverted per pixel rather than
 * interpolated directly: screen space is a perspective divide away from
 * scene space, so a linear blend of z along an edge is simply the wrong
 * number, and the error is largest exactly where two surfaces are close
 * enough for it to matter.
 *
 * Every vertex reaching here is already in front of the near plane
 * (clipAndFill), so there is no sign check left to make and no vertex whose
 * divide can go the wrong way.
 */
    void triangle(Pixels_& px, const View& v,
                  const Vertex& a, const Vertex& b, const Vertex& c, const float col[4])
    {
        const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        if (std::fabs(area) < 1e-6f) return;

        int32_t x0 = static_cast<int32_t>(std::floor(std::min({a.x, b.x, c.x})));
        int32_t x1 = static_cast<int32_t>(std::ceil (std::max({a.x, b.x, c.x})));
        int32_t y0 = static_cast<int32_t>(std::floor(std::min({a.y, b.y, c.y})));
        int32_t y1 = static_cast<int32_t>(std::ceil (std::max({a.y, b.y, c.y})));
        x0 = std::max<int32_t>(x0, 0);
        y0 = std::max<int32_t>(y0, 0);
        x1 = std::min<int32_t>(x1, static_cast<int32_t>(v.w) - 1);
        y1 = std::min<int32_t>(y1, static_cast<int32_t>(v.h) - 1);
        if (x1 < x0 || y1 < y0) return;

        const float inv_area = 1.0f / area;
        const float iza = 1.0f / a.z, izb = 1.0f / b.z, izc = 1.0f / c.z;

        uint8_t* base = px.PixelData();
        if (!base) return;
        const size_t stride = px.PixelStride();

        const uint8_t cr = toByte(col[0]), cg = toByte(col[1]),
                      cb = toByte(col[2]), ca = toByte(col[3]);

        for (int32_t y = y0; y <= y1; ++y)
        {
            const float py = static_cast<float>(y) + 0.5f;
            for (int32_t x = x0; x <= x1; ++x)
            {
                const float pxf = static_cast<float>(x) + 0.5f;

                float w0 = (b.x - a.x) * (py - a.y) - (b.y - a.y) * (pxf - a.x);
                float w1 = (c.x - b.x) * (py - b.y) - (c.y - b.y) * (pxf - b.x);
                float w2 = (a.x - c.x) * (py - c.y) - (a.y - c.y) * (pxf - c.x);
                if (area < 0.0f) { w0 = -w0; w1 = -w1; w2 = -w2; }
                if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;

                // w1 belongs to a, w2 to b, w0 to c -- each weight is the
                // area of the triangle OPPOSITE its vertex.
                const float la = w1 * std::fabs(inv_area);
                const float lb = w2 * std::fabs(inv_area);
                const float lc = w0 * std::fabs(inv_area);

                const float inv_z = la * iza + lb * izb + lc * izc;
                if (!(inv_z > 0.0f)) continue;
                const float z = 1.0f / inv_z;

                float& slot = m_depth[static_cast<size_t>(y) * v.w + x];
                if (z >= slot) continue;
                slot = z;

                uint8_t* d = base + static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4;
                d[0] = cr; d[1] = cg; d[2] = cb; d[3] = ca;
            }
        }
    }

    /*
 * Commit every joule of entropy owed since this node's last interaction,
 * into the node that CONTAINS it.
 *
 * The environment of a point is its parent, and heat crossing that boundary
 * is the only energy transfer in this model that is not caused by something
 * doing work: a warm box in a cold room warms the room by being in it. The
 * parent absorbs exactly what the child emits -- one number, moved -- so
 * energy is conserved across the boundary rather than approximately tracked
 * on both sides of it.
 *
 * AT THE ROOT THE HEAT LEAVES THE MODEL, and that is stated rather than
 * hidden: there is no parent, so the emission is counted into a running
 * total and dropped. A scene root emitting into a world this system does not
 * represent is exactly what an open system is, and the counter is what makes
 * the leak an observable quantity instead of a silent non-conservation.
 */
    void commitEntropy()
    {
        const auto now = std::chrono::steady_clock::now();
        if (!m_interacted) { m_last_interaction = now; m_interacted = true; return; }

        float dt = std::chrono::duration<float>(now - m_last_interaction).count();
        m_last_interaction = now;
        if (dt > 1.0f) dt = 1.0f;   // same cap, same reason, as the motion step

        // The EVENT, and it is an OrderVector like any other: where it left,
        // whose boundary it crossed, how much energy, all of it unordered
        // (ontology/OrderVector.h). Kept as the node's last crossing so a
        // guard capturing a generator during this step has it to read.
        const OrderVector e = m_ov.EmitEvent(m_ov.EmissionOver(dt, m_emissivity), dt);
        if (!(e.energy > 0.0f)) return;
        m_last_emission = e;

        // The emitter keeps its own step's meta: the span it just settled and
        // the uncertainty of the crossing it just made. Not a copy of the
        // emission -- the emission is what LEFT, these are the current state
        // of the thing that emitted it, and they are what an entity is asked
        // for when something wants to know where its causality is now.
        m_ov.interval    = e.interval;
        m_ov.uncertainty = e.uncertainty;

        // The tick. Counted on emissions that ACTUALLY happened, never on
        // interactions that found nothing owed -- a node with no heat to shed
        // has had no time pass for it, and incrementing here anyway would make
        // this a count of how often somebody looked, which is the observer's
        // clock and not this node's.
        ++m_ticks;

        // The whole vector moves: heat lands as heat, and anything ordered it
        // carried would land as an impulse. One transfer, no conversion.
        if (Scene3D* env = ownParent()) env->m_ov.Absorb(e);
        else                            m_emitted_out += e.energy;
    }

    // The containing node, when there is one of this module's own leaves
    // above -- the environment a point emits into. A foreign 3D parent is not
    // one: this module cannot put heat into a representation it cannot read,
    // and pretending otherwise would be inventing the number.
    Scene3D* ownParent()
    {
        ETCS::Entity* p = getParent();
        if (!p || !isOwnLeaf(p)) return nullptr;
        return static_cast<Scene3D*>(p->getTrueType());
    }

    /*
 * Fold the pending look into row 3, and point the camera where it says.
 *
 * THE REFERENCE FORWARD IS THE ONE THE SCRIPT SET. Row 3 is a rotation, and a
 * rotation is only an orientation once there is something to rotate FROM. The
 * first time through, the direction the script framed with LookAt is captured
 * as that reference and row 3 is the identity -- so a scene that has never
 * seen a mouse renders exactly what was asked for, and the first delta nudges
 * from there rather than snapping to an axis.
 *
 * YAW ABOUT WORLD UP, PITCH ABOUT THE CURRENT RIGHT. Both composed onto row 3
 * from the left (OrderVector::RotateBy), which is what makes the two behave
 * as a head turning rather than as a body rolling: yaw in world space keeps
 * the horizon level at any pitch, and pitch in the view's own frame is the
 * one axis that stays meaningful as yaw changes. Euler order and gimbal lock
 * do not arise, because there are no Euler angles.
 *
 * THE PITCH CLAMP IS A REJECTION, not a wrap. A delta that would carry the
 * forward vector past the pole is dropped and the yaw still applies, so
 * pushing the mouse up at the top of the arc keeps turning instead of
 * flipping the world over. At the pole itself the camera basis collapses and
 * buildView refuses -- the view would blank for as long as the mouse was
 * held there -- so this is a limit rather than a failure.
 */
    void applyLookTo(Camera_* camera)
    {
        if (!camera) return;
        ViewFrustum v = camera->GetView();

        float rx = v.look_at.x - v.position.x;
        float ry = v.look_at.y - v.position.y;
        float rz = v.look_at.z - v.position.z;
        float dist = std::sqrt(rx * rx + ry * ry + rz * rz);
        if (!(dist > 0.0f)) { dist = 1.0f; rz = 1.0f; }

        if (!m_look_seeded)
        {
            m_ref_fwd = Point3D{ rx / dist, ry / dist, rz / dist };
            // Row 2: what the look turns about is the eye, in the scene's
            // frame -- a first-person look is a rotation about the viewer, and
            // that is exactly what a pivot is for.
            m_ov.SetPivot(v.position.x - m_ov.x, v.position.y - m_ov.y, v.position.z - m_ov.z);
            m_ov.Orient(0.0f, 0.0f, 0.0f, 0.0f);
            m_look_seeded = true;
            return;
        }
        if (!m_look_dirty.exchange(false, std::memory_order_relaxed)) return;

        const float dyaw   = takePending(m_pending_yaw);
        const float dpitch = takePending(m_pending_pitch);

        if (dyaw != 0.0f) m_ov.RotateBy(0.0f, 1.0f, 0.0f, dyaw);

        if (dpitch != 0.0f)
        {
            // The right axis of the CURRENT orientation, which is what pitch
            // has to turn about for the horizon to stay level.
            float fwx = m_ref_fwd.x, fwy = m_ref_fwd.y, fwz = m_ref_fwd.z;
            m_ov.RotateVector(fwx, fwy, fwz);
            float rgx = fwz, rgy = 0.0f, rgz = -fwx;          // up x forward, flattened
            const float rl = std::sqrt(rgx * rgx + rgz * rgz);
            if (rl > 1e-5f)
            {
                rgx /= rl; rgz /= rl;
                OrderVector probe = m_ov;
                probe.RotateBy(rgx, rgy, rgz, dpitch);
                float px = m_ref_fwd.x, py = m_ref_fwd.y, pz = m_ref_fwd.z;
                probe.RotateVector(px, py, pz);
                if (std::fabs(py) < 0.999f) m_ov = probe;     // else: at the pole, drop it
            }
        }

        float fx2 = m_ref_fwd.x, fy2 = m_ref_fwd.y, fz2 = m_ref_fwd.z;
        m_ov.RotateVector(fx2, fy2, fz2);
        v.look_at = Point3D{ v.position.x + fx2 * dist,
                             v.position.y + fy2 * dist,
                             v.position.z + fz2 * dist };
        camera->SetView(v);

        m_frame_w = cameraWidth(camera);
    }

    // The conversion chain in SetMouseDpi's comment, applied. Falls back to a
    // nominal width until the first projection has told us the real one.
    float radiansPerPixel() const
    {
        const float w = static_cast<float>(m_frame_w ? m_frame_w : 1024u);
        const float screen_inches = w / m_screen_dpi;
        const float counts_per_pass = m_mouse_dpi * screen_inches;
        if (!(counts_per_pass > 0.0f)) return 0.0f;
        return (2.0f * 3.14159265f * m_turns / counts_per_pass) * m_sens_scale;
    }

    static uint32_t cameraWidth(Camera_* c)
    {
        Drawable2D_* plane = cameraPlane(c);
        return plane ? plane->Bounds().w : 0u;
    }

    // Accumulate onto an atomic float. A CAS loop rather than a plain
    // load/store because two pointer events in the same instant are ordinary
    // and losing one is a dropped mouse sample -- cheap to do correctly.
    static void addPending(std::atomic<float>& a, float d)
    {
        float cur = a.load(std::memory_order_relaxed);
        while (!a.compare_exchange_weak(cur, cur + d,
                                        std::memory_order_relaxed,
                                        std::memory_order_relaxed)) {}
    }
    static float takePending(std::atomic<float>& a)
    {
        return a.exchange(0.0f, std::memory_order_relaxed);
    }

    /*
 * Bring row 2's radius up to date over the whole subtree.
 *
 * Cover rather than Reduce, and that is the distinction the two calls exist
 * for: this node's POSITION is fixed by the script or by w/a/s/d, and reducing
 * it would drag the container around every time something inside it moved --
 * a scene root that follows its own contents is not a container. So the
 * position stays and only the reach is recomputed.
 *
 * PARENT AND CHILD GO THROUGH THE SAME CALL, which is the whole reason the
 * rows are on OrderVector rather than on an aggregate type: each box's own
 * radius is its bounding sphere, each container's is the reach over its
 * members' positions PLUS their radii, and the recursion bottoms out wherever
 * a radius is zero. A coarse level over a fine one is exact rather than a
 * bound over bounds that has quietly lost the guarantee.
 *
 * What this buys, today: world.Order() reports a reach that means something,
 * and OrderVector::GapTo between two scenes is a one-comparison proof that
 * nothing in either could have touched anything in the other.
 */
    void coverSubtree(const std::vector<Node>& nodes)
    {
        if (nodes.empty()) { m_ov.radius = 0.0f; return; }
        std::vector<OrderVector> parts;
        parts.reserve(nodes.size());
        for (const Node& n : nodes)
        {
            OrderVector p;
            p.x = n.pos.x; p.y = n.pos.y; p.z = n.pos.z;
            p.radius = std::sqrt(n.half.x * n.half.x + n.half.y * n.half.y + n.half.z * n.half.z);
            parts.push_back(p);
        }
        m_ov.Cover(parts.data(), parts.size());
    }

    // Reduce the wide bitset to the six bits the projection reads. Called on
    // the input thread only, once per key change -- not per tick, and not on
    // the reader's side, which is the whole point of publishing it.
    void publishMotionBits()
    {
        uint32_t bits = 0;
        if (Held('W')) bits |= MOVE_FWD;
        if (Held('S')) bits |= MOVE_BCK;
        if (Held('A')) bits |= MOVE_LFT;
        if (Held('D')) bits |= MOVE_RGT;
        if (Held('Q')) bits |= MOVE_UP;
        if (Held('E')) bits |= MOVE_DWN;
        m_motion.store(bits, std::memory_order_relaxed);
    }

    static uint8_t toByte(float f)
    {
        if (f <= 0.0f) return 0;
        if (f >= 1.0f) return 255;
        return static_cast<uint8_t>(f * 255.0f + 0.5f);
    }

    // Row 0 of the order vector IS this node's centre, in the PARENT's space.
    // Not a copy of it and not kept in step with it -- there is one position
    // here, and the motion integrator writes the same three floats the
    // projection reads (ontology/OrderVector.h).
    OrderVector m_ov;
    Point3D m_half{0.5f, 0.5f, 0.5f};
    float   m_color[4] = {0.8f, 0.8f, 0.85f, 1.0f};
    bool    m_visible  = true;
    float   m_speed    = 6.0f;    // terminal, scene units per second
    float   m_damping  = 8.0f;    // kinetic -> heat, per second
    float   m_mass     = 1.0f;

    ETCS::TBuffer<NUM_KEYS / 8> m_held;   // one bit per key in the spectrum
    std::atomic<uint32_t>       m_motion{0};   // the six bits that cross threads

    // The look. Atomics for the same reason the motion bits are: written by
    // the input edge, read by the projection, on different threads. The
    // ORIENTATION itself is not here -- it is row 3 of m_ov, where an angle
    // belongs; these are only the deltas waiting to be folded into it.
    std::atomic<float> m_pending_yaw{0.0f};
    std::atomic<float> m_pending_pitch{0.0f};
    std::atomic<bool>  m_look_dirty{false};
    bool               m_look_seeded = false;
    Point3D            m_ref_fwd{0.0f, 0.0f, 1.0f};   // what row 3 rotates FROM
    uint32_t           m_frame_w    = 0;
    float              m_ground_fx  = 0.0f;   // last usable horizontal facing
    float              m_ground_fz  = 1.0f;
    float              m_sens_scale = 1.0f;
    float              m_mouse_dpi  = 800.0f;   // not discoverable; see SetMouseDpi
    float              m_screen_dpi = 96.0f;    // Window.ScreenDpi reports the real one
    float              m_turns      = 1.0f;     // full rotations per screen-width pass

    std::chrono::steady_clock::time_point m_last_step{};
    bool                                  m_stepped = false;

    // The entropy ledger: when this node last interacted, how fast it sheds
    // heat, and what has left the model entirely through the root.
    std::chrono::steady_clock::time_point m_last_interaction{};
    bool                                  m_interacted  = false;
    float                                 m_emissivity  = 0.5f;
    float                                 m_emitted_out = 0.0f;
    uint64_t                              m_ticks       = 0;
    OrderVector                           m_last_emission{};

    std::vector<float>     m_depth;
    uint32_t               m_depth_w   = 0;
    uint32_t               m_depth_h   = 0;
    ETCS::RID              m_depth_cam = 0;
    std::vector<ETCS::RID> m_viewers;
    uint64_t               m_projections = 0;
};

#endif
