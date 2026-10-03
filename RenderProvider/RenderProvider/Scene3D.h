#ifndef RENDERPROVIDER_SCENE3D_H__
#define RENDERPROVIDER_SCENE3D_H__

#include "../../../core_defs.h"
#include "../../../ontology.h"
#include "SceneSink.h"
#include "Mesh.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
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
// The stream edge writes it; stepOver reads it and integrates. See
// RenderProvider.h's ConsumeInput for the edge itself.
// ---------------------------------------------------------------------------
class Scene3D : public Drawable3DBase<Scene3D>,
                public CausalBase<Scene3D>,
                public DeletableBase<Scene3D>,
                public LifecycleBase<Scene3D>
{
public:
    WIRE_TYPE_IDENTITY(Scene3D);

    // --- Orderable_ (required by Surface, which Drawable refines) ---
    int32_t m_order = 0;
    // Hidden loses every comparison; then the stated order.
    bool operator<(const Scene3D& o) const
    { return Hidden() != o.Hidden() ? Hidden() : m_order < o.m_order; }
    int32_t Order() override { return m_order; }

    // WHAT A VERB SETS IS A VALUE ON THE TAG SURFACE: speed, damping,
    // colour, order, sensitivity (and the family's emissivity, CausalBase),
    // each through the funnel under its own flag, so the record keeps the
    // verb, a replay sets it again and a store keeps what it says. The
    // members below are the working copies the step and the picture read,
    // and onValue is their one writer -- a verb, a replay and a restore all
    // land there. Mass has no verb: it is what a box is, a constant.
    Scene3D() = default;
    void onValue(const ETCS::Buffer& key, const std::string* value) override
    {
        int64_t w[4];
        const std::string_view k(key.c_str());
        if (k == "speed" || k == "damping")
        {
            const bool speed = (k == "speed");
            if (!readWords(value, w, 1)) w[0] = (speed ? kSpeed : kDamping).raw;
            std::lock_guard<std::recursive_mutex> lk(TreeMutex());   // the step reads these under it
            (speed ? m_speed : m_damping) = Fixed::FromRaw(w[0]);
        }
        else if (k == "color")
        {
            if (!readWords(value, w, 4)) { for (int i = 0; i < 4; ++i) m_color[i] = kColor[i]; }
            else for (int i = 0; i < 4; ++i) m_color[i] = Fixed::FromRaw(w[i]).ToFloat();
            markViewersDirty();
        }
        else if (k == "order")
        {
            m_order = readWords(value, w, 1) ? static_cast<int32_t>(w[0]) : 0;
            Reorder(); markViewersDirty();
        }
        else if (k == "sensitivity")
            m_sens_scale = readWords(value, w, 1) ? Fixed::FromRaw(w[0]).ToFloat() : kSensitivity;
        else CausalBase<Scene3D>::onValue(key, value);
    }
    ~Scene3D() = default;

    // The centre, read out of row 0 -- the rows are the family's (CausalBase),
    // and every reader below goes through them rather than a copy: one
    // position, one writer. The float is the picture's.
    Point3D Pos() const { return Point3D{ Order4().x.ToFloat(), Order4().y.ToFloat(), Order4().z.ToFloat() }; }

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
        // Row 0's fourth slot, the identity, is the family base's to fill
        // (CausalBase::refreshIdentityLocked): the state hash, not the RID.
        // Row 2's fourth slot. A box is not a point -- it is already an
        // aggregate over the volume it occupies -- so its reach is its own
        // bounding sphere, and it is an aggregate from the moment it exists.
        // Only a node with no extent is a leaf here.
        // Causal, so computed on the causal side: the extent's numbers cross
        // the boundary once, here, and the reach is their Fixed length.
        Rows().radius = Fixed::Length(Fixed::From(m_half.x), Fixed::From(m_half.y), Fixed::From(m_half.z));
        this->addTag("active");
        /*
 * The symmetric half of ReleaseConcrete's, and needed for the same reason: a
 * node ARRIVING changes what the scene projects, and the addTag above marks
 * only the parent chain -- which a camera is not on.
 *
 * It worked by accident until it didn't. A spawn is normally followed by
 * SetPosition/SetColor, and those already fan out, so the new node appeared;
 * a bare spawn + Create left the compositor's recompose count unchanged.
 * Measured both ways.
 */
        markViewersDirty();
        return true;
    }

    // A teleport, and deliberately not a motion: it moves the point without
    // touching what the point is carrying (OrderVector::PlaceAt).
    // Every writer of the rows outside a step takes the tree's lock, as the
    // step does (CausalBase): a script's verb and a frame's step land whole.
    void SetPosition(float x, float y, float z)
    {
        { std::lock_guard<std::recursive_mutex> lk(TreeMutex()); Rows().PlaceAt(Fixed::From(x), Fixed::From(y), Fixed::From(z)); }
        markViewersDirty();
    }
    void Move(float dx, float dy, float dz)
    {
        { std::lock_guard<std::recursive_mutex> lk(TreeMutex());
          Rows().PlaceAt(Order4().x + Fixed::From(dx), Order4().y + Fixed::From(dy), Order4().z + Fixed::From(dz)); }
        markViewersDirty();
    }

    // The push a script makes, at the boundary: floats in, Fixed on the rows.
    // The family's Fixed form stays reachable beside it (a Causal container
    // pushing a member speaks Fixed).
    using CausalBase<Scene3D>::Impulse;
    void Impulse(float dx, float dy, float dz, float joules)
    {
        CausalBase<Scene3D>::Impulse(Fixed::From(dx), Fixed::From(dy), Fixed::From(dz), Fixed::From(joules));
    }
    void Halt() { std::lock_guard<std::recursive_mutex> lk(TreeMutex()); Rows().Rest(); }
    void SetEmissivity(float per_sec) { CausalBase<Scene3D>::SetEmissivity(Fixed::From(per_sec)); }
    float Emissivity() const { return CausalBase<Scene3D>::Emissivity().ToFloat(); }
    float EmittedToEnvironment() const { return EmittedOut().ToFloat(); }
    void SetColor(float r, float g, float b, float a)
    {
        this->addTag("color", words({ Fixed::From(r).raw, Fixed::From(g).raw, Fixed::From(b).raw, Fixed::From(a).raw }));
    }
    void SetOrder(int32_t z) { this->addTag("order", words({ z })); }

    // Terminal speed, in scene units per SECOND -- not per tick. The right
    // value is a property of the scene's scale, which the script knows and
    // this class cannot guess; per-second is what makes it independent of how
    // often the input edge happens to run.
    void SetSpeed(float units_per_sec)
    {
        if (units_per_sec > 0.0f) this->addTag("speed", words({ Fixed::From(units_per_sec).raw }));
    }
    float Speed() const { return m_speed.ToFloat(); }

    // How fast motion bleeds off, per second. It sets BOTH halves of the feel
    // at once and that is not a coincidence: with a fixed terminal speed, the
    // acceleration needed to reach it is speed * damping, so one number gives
    // you the coast and the responsiveness together. High damping is crisp and
    // stops dead; low damping drifts.
    void SetDamping(float per_sec)
    {
        if (per_sec > 0.0f) this->addTag("damping", words({ Fixed::From(per_sec).raw }));
    }
    float Damping() const { return m_damping.ToFloat(); }

    // Whether this node's own box is drawn. Its children still are, and
    // their coordinates stay relative to a box that is still there -- how a
    // script draws a group with no box of its own. The Drawable "hidden"
    // flag, so it is on the surface like every other presence, rather than a
    // private bool beside it (DrawableBase::SetHidden).
    void SetVisible(bool on) { SetHidden(!on); markViewersDirty(); }
    // The shape this node is drawn with (a Mesh, by RID); 0 is its box. Both
    // paths draw it: the device from the geometry sent once, the host from
    // the same triangles (rasterMesh).
    void SetMesh(ETCS::RID mesh) { m_mesh = mesh; markViewersDirty(); }

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
 * THE LOOK, from the pointer's POSITION -- absolute, not accumulated.
 *
 * THE MODEL. The camera's frame is the plane bisecting its view cone, so the
 * frame's bounding box IS the field of view. Where the pointer sits on that
 * box therefore names a direction, directly: its offset from the frame centre
 * is a position on a rotational axis, one per dimension. Across the width that
 * axis is the full circle -- 2*pi, so the left edge is half a turn one way and
 * the right edge half a turn the other. Across the height it is the same
 * construction with a limited range, because up is not a circle you come back
 * around (clampPitch).
 *
 * NOTHING ACCUMULATES. The angle is a function of where the pointer is, so it
 * is right the moment it is read and cannot drift, lag or double-count. Put
 * the pointer back where it was and the view is exactly back where it was.
 * There is no state to re-prime and nothing that can be one event behind.
 *
 * WHY THIS AND NOT DELTAS. A relative look needs the pointer to have somewhere
 * to keep going, so it needs the pointer LOCKED -- hidden, and teleported back
 * to the centre as it strays. That is a request the display may refuse: Qubes
 * proxies windows from another domain, XWayland answers to a compositor,
 * remote X has no local pointer to move. A refused warp does not degrade, it
 * INVERTS THE MEANING of the input: the toolkit sets its own last-position to
 * the centre before warping, so when the warp does not land the next report
 * yields "distance from the centre" where a delta should be. The screen
 * silently becomes a joystick -- enormous apparent gain, and a view that keeps
 * turning after the hand stops, which is what read as inertia.
 *
 * Asking for no warping makes that unreachable rather than handled. The cost
 * is honest and small: one frame width is one full turn, so a fast spin is a
 * flick to the edge rather than an unbounded sweep.
 *
 * The orientation still lives on the scene's OWN row 3 (ontology/OrderVector.h)
 * and is applied to whichever camera projects it -- what the input names is the
 * RELATION between viewer and world, the same quantity whether you turn the
 * head or turn the room.
 *
 * RECORDED HERE, COMPOSED AT THE OBSERVER, as before: this stores a position,
 * Project turns it into row 3 on the frame thread, and what crosses threads is
 * two relaxed atomics.
 */
    void PointerPosition(int32_t x, int32_t y)
    {
        m_ptr_x.store(x, std::memory_order_relaxed);
        m_ptr_y.store(y, std::memory_order_relaxed);
        m_ptr_seen.store(true, std::memory_order_relaxed);
        m_look_dirty.store(true, std::memory_order_relaxed);
    }


    /*
 * SENSITIVITY IS THE YAW SPAN: how many full turns the frame's width covers.
 * 1.0 means the width is exactly one turn, so the pointer's position over the
 * view names a direction and every direction is reachable without the pointer
 * leaving the frame.
 *
 * YAW ONLY, and the asymmetry is the axes', not an oversight. Compression
 * means going round an axis more than once inside the frame, and only a circle
 * can do that. Pitch is bounded, so multiplying its span pushes the ends of
 * the range inside the frame and everything past them is a dead zone -- at 2.0
 * the top and bottom quarters of the frame did nothing. The height maps onto
 * the pitch range exactly once, always.
 *
 * IT IS A RANGE, NOT A RATE, and that distinction is what makes it tunable at
 * last. Nothing accumulates in an absolute control, so this cannot compound,
 * cannot drift, and means the same thing after an hour as in the first second.
 * Above 1.0 the axis is compressed -- you reach a half turn before the pointer
 * reaches the edge, and the outer part of the frame is past the stop; below
 * 1.0 it is stretched, and the frame's edge is less than a half turn.
 *
 * WHAT THIS UNIT REPLACES, twice, and why both failed. Turns-per-pass and then
 * view-pixels-per-pointer-pixel were both RATES, multiplying a delta, and a
 * rate is only meaningful if the deltas are trustworthy -- which they were not
 * on a display that refuses to move a pointer (see PointerPosition). Worse, a
 * rate has no natural value: turns-per-pass ignored the lens, and the pixel
 * ratio needed the lens measured to mean anything. A span has an obvious one.
 * The frame covering exactly one turn is the setting a first-person view
 * wants, and every other value is a stated preference against it.
 *
 * The lens does not enter this at all any more, which is the right outcome
 * rather than a loss: field of view decides how much of the world the frame
 * SHOWS, and the span decides how much of it the frame REACHES. They were
 * conflated while the rate was derived from tan(fov/2), and separating them is
 * why changing SetLens no longer changes how far a movement turns you.
 */
    void SetSensitivity(float span)  { if (span  > 0.0f) this->addTag("sensitivity", words({ Fixed::From(span).raw })); }

    // The look's state, as the two angles it actually is. Reported rather than
    // inferred: the previous limit was a rejection test on a rotated vector's
    // y component, so "how far up is this looking" had no answer anywhere and
    // the only way to check a bound was to look at the screen and guess. A
    // constraint nothing can read is a constraint nothing can test.
    float Yaw() const   { return m_yaw; }
    // The VIEW's elevation, not the rotation applied to reach it -- the
    // question this exists to answer is how far up the camera is looking, and
    // the rotation answers it only for a scene seeded on the horizon.
    float Pitch() const { return m_pitch; }

    float Sensitivity() const    { return m_sens_scale; }
    uint32_t FrameWidth() const  { return m_frame_w ? m_frame_w : NOMINAL_FRAME_W; }
    uint32_t FrameHeight() const { return m_frame_h ? m_frame_h : NOMINAL_FRAME_H; }

    // What the frame's two axes cover, which is the whole of the mapping.
    // Reported rather than stored: both are the sensitivity applied to a fixed
    // span, so there is one setting and these are its two consequences.
    float YawSpanTurns()  const { return m_sens_scale; }
    // Fixed, and not a consequence of the sensitivity: a bounded axis cannot
    // be compressed without putting part of the frame past its own stop. See
    // applyLookTo's pitch mapping.
    float PitchSpanDeg()  const { return 2.0f * PITCH_LIMIT_RAD * 57.2957795f; }
    // Where the view is aimed when the pointer is at the middle of the frame's
    // height -- the seeded direction's own elevation, in degrees. The pitch
    // range is centred on the horizon, NOT on this.
    float ReferenceElevationDeg() const { return m_ref_elev * 57.2957795f; }

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
            || Order4().KineticEnergy().IsPositive();
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
 * stepOver stops marking, and the whole pipeline goes quiet on its own. No idle spin anywhere, and no thread whose
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
    using CausalBase<Scene3D>::Interact;    // the driver's form, beside the observed one
    void Interact()
    {
        // THE OBSERVED INTERACTION IS THE FAMILY'S, with the two spans an
        // observer measures (two ceilings; the reasoning is on the clocks
        // below) handed in as the Fixed values the rows will see -- the whole of what the wall clock contributed, and what
        // CausalBase puts on the tape so an observed history replays. The
        // hop under it is the driver's hop: commit, step, members, contacts.
        // Under the tree's lock, as the driver's interaction is: a Run on a
        // script's thread and a frame observing at the same time take turns
        // on the tree rather than interleaving inside a step.
        std::lock_guard<std::recursive_mutex> lk(TreeMutex());
        const Fixed commit = Fixed::From(static_cast<double>(m_entropy_clock.Take()) * 0.001);
        const Fixed step   = Fixed::From(static_cast<double>(m_motion_clock.Take())  * 0.001);
        InteractObserved(commit, step);   // a first observation has nothing behind it: both zero, nothing happens
    }

    // The family's step: what a held key pushes with and what drag takes,
    // then the rows advance (CausalBase::AdvanceConcrete).
    void AdvanceConcrete(Fixed dt) override { stepOver(dt); }
    Fixed MassConcrete() const override { return m_mass; }

    // The driver and the hash are the family's; named here so the verbs and
    // the header's readers find them beside the observed path.
    using CausalBase<Scene3D>::Run;
    uint64_t Hash() { return CausalHash(); }

    /*
 * NOT Animated_, AND THAT IS A CLAIM RATHER THAN AN OVERSIGHT. The family
 * (ontology/Animated.h) is for a thing a driver advances; these steps are
 * charged for by being LOOKED AT -- see Interact above -- so a frame edge
 * stepping this node whether or not a camera is on it would be a different
 * model, not the same one wired up more neatly. What the family and this node
 * genuinely share is the MEASUREMENT, and that is StepClock
 * (ontology/StepClock.h), which both hold one of.
 *
 * TWO CLOCKS, TWO CEILINGS, and they no longer pretend otherwise. Motion
 * refuses to believe in more than a tenth of a second of unobserved movement;
 * the entropy ledger will credit a whole second of unobserved cooling, because
 * a box does not stop being warm while nobody is looking at it and a scene
 * does stop being thrown across the map. Both numbers are on the clocks
 * below, where the difference is visible, and both spans go to the family
 * (CausalBase::InteractObserved) as the two inputs they are.
 */

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
    bool stepOver(Fixed sdt)
    {
        if (!sdt.IsPositive()) return false;

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
        int fwd_in = 0, right_in = 0, up_in = 0;
        if (bits & MOVE_FWD) fwd_in   += 1;
        if (bits & MOVE_BCK) fwd_in   -= 1;
        if (bits & MOVE_RGT) right_in += 1;
        if (bits & MOVE_LFT) right_in -= 1;
        if (bits & MOVE_UP)  up_in    += 1;
        if (bits & MOVE_DWN) up_in    -= 1;

        bool pushing = false;
        if (fwd_in != 0 || right_in != 0 || up_in != 0)
        {
            // The viewer's GROUND frame: the current facing (row 3 applied to
            // the reference forward) flattened onto the horizontal plane, and
            // right = up x forward, the same handedness buildView uses.
            //
            // Flattened rather than used whole, which is the pitch exclusion
            // made concrete: looking up should aim the view, not lift the
            // feet. When the facing is near-vertical the horizontal part
            // vanishes and the last usable frame is kept, so walking while
            // staring at the sky is still walking somewhere. Only read while
            // a key is down: a coasting node does not need to know where
            // forward is.
            Fixed fx = Fixed::From(m_ref_fwd.x), fyv = Fixed::From(m_ref_fwd.y), fz = Fixed::From(m_ref_fwd.z);
            Order4().RotateVector(fx, fyv, fz);
            const Fixed fl = Fixed::Length(fx, Fixed::Zero(), fz);
            if (fl > Fixed::From(1e-4)) { fx /= fl; fz /= fl; m_ground_fx = fx; m_ground_fz = fz; }
            else                        { fx = m_ground_fx;   fz = m_ground_fz; }
            const Fixed rx = fz, rz = -fx;

            const Fixed F = Fixed::FromInt(fwd_in), R = Fixed::FromInt(right_in), U = Fixed::FromInt(up_in);
            const Fixed dx = -(fx * F + rx * R);
            const Fixed dy = -U;
            const Fixed dz = -(fz * F + rz * R);

            pushing = !dx.IsZero() || !dy.IsZero() || !dz.IsZero();
            if (pushing)
            {
                /*
         * The impulse that lands exactly on SetSpeed's terminal and no higher:
         * kinetic energy at the terminal speed is 1/2 m v_max^2, and drag takes
         * (1 - exp(-k dt)) ~ k dt of it each tick, so 1/2 m v_max^2 * k * dt
         * replaces exactly what drag removes -- the fixed point of the two
         * operations, which is what a terminal speed IS.
         */
                const Fixed joules = Fixed::Half() * m_mass * m_speed * m_speed * m_damping * sdt;
                Rows().Impulse(dx, dy, dz, joules);
            }
        }

        // Drag is a TRANSFER, not a subtraction: kinetic goes down, E stays,
        // and the difference is heat by definition (ontology/OrderVector.h).
        // exp(-k dt) rather than v -= v*k*dt: the exact form is stable for
        // any measured dt, and it is Fixed's own exp, so the same on every
        // platform. A function of (damping, dt) alone, so a driver stepping
        // at one rate pays the series once and reads it back after.
        if (sdt != m_drag_dt || m_damping != m_drag_damping)
        {
            m_drag_dt = sdt; m_drag_damping = m_damping;
            m_drag_factor = (-(m_damping * sdt)).Exp();
        }
        Rows().Dissipate(m_drag_factor);

        // Below this the point is not coasting, it is dithering the last bits
        // of a decaying number forever, and every tick would re-project an
        // identical frame. Coming to rest is a real state change: all of the
        // remaining kinetic energy becomes heat.
        Fixed vx, vy, vz;
        Order4().Velocity(m_mass, vx, vy, vz);
        const Fixed v2   = vx * vx + vy * vy + vz * vz;
        const Fixed rest = m_speed * Fixed::From(1e-3);
        if (!pushing && v2 < rest * rest)
        {
            Rows().Rest();
            return false;
        }
        if (v2.IsZero()) return false;

        Rows().AdvanceBy(vx, vy, vz, sdt);   // the velocity just read: one reading, not two
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
        std::lock_guard<std::recursive_mutex> lk(TreeMutex());   // one state of the rows (Causal.h, Order4)
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
        // The whole projection under the tree's lock: the picture is of one
        // state, not of rows a Run on another thread is halfway through.
        std::lock_guard<std::recursive_mutex> lk(TreeMutex());
        applyLookTo(camera);
        Interact();

        View v;
        if (!buildView(camera, v)) return nullptr;

        /*
 * WHERE THE SPANS GO, and it is the ONLY thing that differs between a host
 * projection and a device one.
 *
 * Everything above this line and everything below it -- the view, the flatten,
 * the corner transform, the near clip, the perspective divide, the fan, the
 * depth test -- is arithmetic about a scene and a lens, and none of it knows
 * or needs to know whose memory the answer lands in. What varies is one
 * question asked once per surviving run of pixels: write these bytes, or emit
 * this rectangle through the destination.
 *
 * A CAMERA THAT OWNS PIXELS gets the first, unchanged and byte-for-byte.
 * A camera that owns NONE gets the second, and that is not a fallback: it is
 * the same thing PolygonDrawable2D already does one dimension down, in its own
 * words -- "it draws THROUGH the destination surface, using only the three
 * verbs every Surface already owes ... which is what lets the same scene
 * realise onto a window's swapchain, an offscreen CPU layer, or anything a
 * future provider registers under Surface, with no branch here for which one
 * it got". Every Camera_ IS a Surface_, so the camera itself is that
 * destination, and what it does with a DrawRect is its own business:
 * a camera with a Device records it and replays it onto whatever it is drawn
 * into, so on a window surface drawing through a device the frame is produced
 * by the device and never exists as host pixels at all.
 *
 * DEPTH STAYS HERE EITHER WAY. m_depth is the scene's, not the camera's --
 * DepthAt and DepthFor are answered off it (Drawable3D.h: the three
 * granularities "are the SAME camera question ... and their answers have to
 * agree"), so resolving occlusion on the host and emitting only the runs that
 * survived is what keeps all three agreeing on both paths.
 */
        /*
 * THE CAMERA DECIDES, not the presence of a buffer.
 *
 * A camera that owns pixels can still be asked to project through a device --
 * Camera3D keeps its buffer across the toggle rather than freeing it, because
 * the toggle is meant to be flipped while running and a reallocation per flip
 * is exactly what its MoveTo comment is careful about. So "has Pixels" and
 * "wants the host" are different questions, and only the second is this one.
 *
 * DeviceProjection() is the request AND a device still being there
 * (ontology/Camera.h), derived rather than stored, so a Device deleted
 * mid-flight lands here as a quiet fall back to the buffer the camera never
 * gave up.
 */
        // A camera on the device path that can take a scene gets the scene
        // and not a raster: the device draws it (RenderProvider/SceneSink.h). A device
        // camera that cannot -- another provider's -- still gets the runs.
        if (camera->DeviceProjection())
            if (void* raw = camera->getInterfacePointer(ETCS::Buffer(RP_SCENE_SINK)))
            {
                std::vector<Node> nodes;
                collectSubtree(Point3D{0,0,0}, nodes);
                coverRows();
                projectToDevice(static_cast<SceneSink*>(raw), v, nodes);
                m_depth_cam = 0;            // no host picture: DepthAt says so
                this->Observe(camera->getRID());
                etcs_mark_observed(camera);
                ++m_projections;
                { const auto aliens = foreignChildren(); for (Drawable3D_* alien : *aliens) alien->Project(camera); }
                return cameraPlane(camera);
            }

        Sink sink;
        if (camera->DeviceProjection()) sink.dst = cameraSurface(camera);
        else                            sink.px  = cameraPixels(camera);

        if (!sink.dst && !sink.px)
        {
            // Neither: a camera on the host path that owns no buffer. Nothing
            // to write into and nothing to draw through.
            sink.dst = cameraSurface(camera);
            if (!sink.dst)
            {
                ETCS_LOG("Scene3D", "camera RID:" << camera->getRID()
                         << " owns no pixels and is not a surface either -- a "
                            "projection has nowhere to land.");
                return nullptr;
            }
        }

        m_depth.assign(static_cast<size_t>(v.w) * v.h, kFar);
        m_depth_w   = v.w;
        m_depth_h   = v.h;
        m_depth_cam = camera->getRID();

        std::vector<Node> nodes;
        collectSubtree(Point3D{0,0,0}, nodes);
        coverRows();
        for (const Node& n : nodes)
        {
            // The same picture the device makes: a node's Mesh with its
            // triangles, the box otherwise, both placed by T*R*S.
            if (const Mesh* mesh = n.mesh ? meshOf(n.mesh) : nullptr) rasterMesh(sink, v, n, *mesh);
            else                                                        rasterBox(sink, v, n);
        }

        // The camera now holds an image of me, so it is an observer of me in
        // the literal sense -- registered here rather than in a setter, so a
        // camera that has never rendered is never marked (it has no image to
        // invalidate). Marking it is what tells its own uploader to re-take.
        this->Observe(camera->getRID());
        etcs_mark_observed(camera);
        ++m_projections;

        // The foreign case, and the reason it is separate: a 3D leaf from
        // another module has its own geometry this walk cannot read, so it
        // is asked to project itself. It gets the same camera and therefore
        // lands in the same pixels, but it brings its own occlusion.
        { const auto aliens = foreignChildren(); for (Drawable3D_* alien : *aliens) alien->Project(camera); }

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
    bool NeedsFrame() override { return InMotion(); }

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
        /*
 * TELL THE VIEWERS FIRST, before this node stops being part of the scene.
 *
 * The generic machinery marks correctly and it is still not enough here: a
 * node leaving marks the nearest Observable at or above its parent
 * (Entity::markStateChange), which bubbles to the scene root -- and a camera
 * is NOT above the root, so nothing that bubbles reaches it. Its own
 * DrawInto, where it would read the edge it holds, only runs if the
 * compositor ABOVE it decided to walk that far, and the compositor decides
 * that from its own bit, which nothing has set. See markViewersDirty's own
 * comment: the bit alone leaves the camera holding a correct answer nobody
 * ever asks it for.
 *
 * So the same fan-out every setter on this type already does, on the way out.
 * Measured before and after: deleting a Scene3D node left the compositor's
 * recompose count unchanged until a mouse move woke it for unrelated reasons.
 *
 * BEFORE the Unobserve loop below, which drops this node's own edges -- the
 * walk to the root has to happen while the parent link is still whole.
 */
        markViewersDirty();
        { std::vector<uint64_t> cams; ObserverRids(cams);
          for (uint64_t c : cams) Unobserve(c); }
        { std::lock_guard<std::recursive_mutex> lk(TreeMutex()); Rows().Rest(); }
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
        Point3D   pos;
        Point3D   half;
        float     color[4];
        Matrix4   rot;        // the node's own row 3, for the device (the host draws boxes axis-aligned)
        ETCS::RID mesh;       // 0: the unit box, scaled to the extent
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

    /*
 * The raster sink -- see ProjectConcrete for why this is the only thing that
 * varies. Exactly one of the two is set.
 *
 * A RUN, NOT A PIXEL, is what both are asked for. The host path wrote each
 * pixel as it passed the depth test, which is the same bytes either way; the
 * device path could not be expressed at all at that granularity, because a
 * DrawRect per pixel is not a drawing, it is a denial of service. So the
 * scanline accumulates a run of consecutive survivors and flushes it once,
 * which is the same "one DrawRect per span" PolygonDrawable2D fills with.
 */
    struct Sink
    {
        Pixels_*  px  = nullptr;   // host: write the bytes
        Surface_* dst = nullptr;   // no buffer: emit the run through the surface

        void run(int32_t y, int32_t x0, int32_t x1, const float col[4]) const
        {
            const uint32_t w = static_cast<uint32_t>(x1 - x0 + 1);
            if (w == 0) return;

            if (dst)
            {
                // In the camera's OWN space. Where that lands on a destination
                // is composed at DrawInto time by the camera, exactly as it is
                // for every other 2D node -- nothing here states an absolute
                // position, which is the upward half of the 2D contract.
                dst->DrawRect(x0, y, w, 1, col[0], col[1], col[2], col[3]);
                return;
            }

            uint8_t* base = px->PixelData();
            if (!base) return;
            uint8_t* d = base + static_cast<size_t>(y) * px->PixelStride()
                              + static_cast<size_t>(x0) * 4;
            // REPLACE, not blend -- the depth test already decided this run is
            // what is visible here, which is what the byte-write always meant.
            const uint8_t c[4] = { toByte(col[0]), toByte(col[1]),
                                   toByte(col[2]), toByte(col[3]) };
            for (uint32_t i = 0; i < w; ++i, d += 4) std::memcpy(d, c, 4);
        }
    };

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
    // Every camera is a Surface (CameraBase composes it through Drawable2D),
    // but Camera_ does not INHERIT Surface_ -- the lineage is in the base, so
    // reaching it is a lookup like every other family crossing here.
    static Surface_* cameraSurface(Camera_* c)
    {
        if (!c) return nullptr;
        void* p = c->getInterfacePointer(ETCS::Buffer("Surface"));
        return p ? static_cast<Surface_*>(p) : nullptr;
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
        const Point3D p = Pos();
        const Point3D abs{ origin.x + p.x, origin.y + p.y, origin.z + p.z };
        if (!Hidden())
        {
            Node n;
            n.pos  = abs;
            n.half = m_half;
            n.color[0] = m_color[0]; n.color[1] = m_color[1];
            n.color[2] = m_color[2]; n.color[3] = m_color[3];
            // A node's row 3 is its facing -- except the root's, which the look
            // control writes to aim the camera (applyLookTo): that one is the
            // viewer's, not the box's, and the box stays where it stands.
            n.rot  = out.empty() ? Matrix4::Identity() : Order4().ToMatrix4();
            n.rot.at(0,3) = n.rot.at(1,3) = n.rot.at(2,3) = 0.0f;
            n.mesh = m_mesh;
            out.push_back(n);
        }
        { const auto kids = ownChildren(); for (Scene3D* kid : *kids) kid->collectSubtree(abs, out); }
    }

    /*
     * THE DEVICE PROJECTION: the same subtree, as one op per node, handed to
     * the camera (RenderProvider/SceneSink.h) for the device to draw with its own depth
     * buffer. Nothing is rasterised here, so DepthAt has no picture to answer
     * from on this path (it says so); DepthFor, over corners, still does.
     *
     * The matrices are the picture's floats, built from the rows at this
     * boundary and nowhere else: view from the camera's basis (the same basis
     * toView uses), projection with depth to [0,1] and y up, model as
     * translate * rotate * scale -- the OrderVector's own 4x4 with the extent
     * on it. Column-major, as both devices take them.
     */
    void projectToDevice(SceneSink* sink, const View& v, const std::vector<Node>& nodes)
    {
        float view[16], proj[16];
        // Rows right/up/fwd as the columns of the transpose: column-major.
        view[0] = v.right.x; view[4] = v.right.y; view[8]  = v.right.z; view[12] = -(v.right.x*v.eye.x + v.right.y*v.eye.y + v.right.z*v.eye.z);
        view[1] = v.up.x;    view[5] = v.up.y;    view[9]  = v.up.z;    view[13] = -(v.up.x*v.eye.x    + v.up.y*v.eye.y    + v.up.z*v.eye.z);
        view[2] = v.fwd.x;   view[6] = v.fwd.y;   view[10] = v.fwd.z;   view[14] = -(v.fwd.x*v.eye.x   + v.fwd.y*v.eye.y   + v.fwd.z*v.eye.z);
        view[3] = 0.0f;      view[7] = 0.0f;      view[11] = 0.0f;      view[15] = 1.0f;
        for (float& f : proj) f = 0.0f;
        const float n = v.near_p, f = v.far_p;
        proj[0]  = 1.0f / (v.tan_half * v.aspect);
        proj[5]  = 1.0f / v.tan_half;
        proj[10] = f / (f - n);
        proj[11] = 1.0f;
        proj[14] = -(n * f) / (f - n);
        sink->BeginScene(view, proj);

        for (const Node& nd : nodes)
        {
            DeviceMeshOp op;
            op.mesh = nd.mesh;
            // A mesh is in unit space like the box is; the extent is the size
            // of either, so a shape swapped in stands where the box stood.
            const float sx = nd.half.x * 2.0f;
            const float sy = nd.half.y * 2.0f;
            const float sz = nd.half.z * 2.0f;
            // model = T * R * S, column-major: column c is R's column c scaled.
            for (int c = 0; c < 3; ++c)
            {
                const float sc = c == 0 ? sx : c == 1 ? sy : sz;
                for (int r = 0; r < 3; ++r) op.model[c * 4 + r] = nd.rot.at(r, c) * sc;
                op.model[c * 4 + 3] = 0.0f;
            }
            op.model[12] = nd.pos.x; op.model[13] = nd.pos.y; op.model[14] = nd.pos.z; op.model[15] = 1.0f;
            for (int i = 0; i < 4; ++i) op.color[i] = nd.color[i];
            sink->AddMesh(op);
        }
    }

    // Children of this module's own 3D leaf, which are the ones whose
    // geometry this walk can read. Identified by tag rather than by a cast:
    // the family pointer says "a 3D node", it does not say "one of mine",
    // and reading another module's fields off a family pointer is exactly
    // the mistake the interface-pointer discipline exists to prevent.
    //
    // BOTH LISTS ARE KEPT against this entity's hash epoch, the way
    // CausalBase keeps its Causal children: the typed-child lists move only
    // through funnels that bump it, and a projection walks every node of the
    // subtree three times a frame (collect, cover, interact) -- six typed
    // walks and six allocations per node per frame, for lists that change
    // when a script spawns something.
    // Snapshots go out, shared: a walker (the frame edge, a script's Project)
    // iterates a list nobody rebuilds under it, for one reference count --
    // the shape CausalBase::causalChildren has.
    using OwnKids     = std::shared_ptr<const std::vector<Scene3D*>>;
    using ForeignKids = std::shared_ptr<const std::vector<Drawable3D_*>>;
    OwnKids     ownChildren()     { std::lock_guard<std::recursive_mutex> lk(TreeMutex()); refreshKids(); return m_own_kids; }
    ForeignKids foreignChildren() { std::lock_guard<std::recursive_mutex> lk(TreeMutex()); refreshKids(); return m_foreign_kids; }
    void refreshKids()
    {
        const uint32_t epoch = hashEpoch();
        if (m_own_kids && epoch == m_kids_epoch) return;
        auto own = std::make_shared<std::vector<Scene3D*>>();
        auto foreign = std::make_shared<std::vector<Drawable3D_*>>();
        for (ETCS::Entity* e : drawable3DChildren())
        {
            if (isOwnLeaf(e)) own->push_back(static_cast<Scene3D*>(e->getTrueType()));
            else foreign->push_back(static_cast<Drawable3D_*>(e->getInterfacePointer(ETCS::Buffer("Drawable3D"))));
        }
        m_own_kids = std::move(own); m_foreign_kids = std::move(foreign);
        m_kids_epoch = epoch;
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
    void markViewersDirty()
    {
        Scene3D* root = this;
        for (ETCS::Entity* node = getParent(); node; node = node->getParent())
        {
            if (!node->getInterfacePointer(ETCS::Buffer("Drawable3D"))) break;
            if (!isOwnLeaf(node)) break;
            root = static_cast<Scene3D*>(node->getTrueType());
        }

        // The general statement first: everything watching the root is stale.
        root->MarkObserved(root->getRID());

        /*
 * Then wake the PATH to each camera, which the bit alone cannot do. A camera
 * is not below me, so nothing I mark bubbles to it -- and its own DrawInto,
 * where it would read its bit, only runs if the compositor ABOVE it decided
 * to walk that far. Marking only the bit leaves the camera holding a correct
 * answer nobody ever asks it for.
 *
 * This is the one place the observer list itself is needed rather than the
 * answer, which is what ObserverRids exists for.
 */
        std::vector<uint64_t> cams;
        root->ObserverRids(cams);
        for (uint64_t cam : cams)
            if (Camera_* c = ETCS::resolve_in_family<Camera_>("Camera", cam))
                markPixelPath(c);
    }

    // Mark a camera and every observer above it (ontology/Observable.h). Taken
    // as Entity because the chain crosses families: a camera's parent is a
    // compositor, whose parent may be anything at all.
    static void markPixelPath(ETCS::Entity* node) { etcs_mark_observed(node); }

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
    // A point of the node's unit space -- the box's corner, a mesh's vertex
    // -- placed in the scene: scaled by the extent, turned by row 3,
    // carried to the position. T * R * S, exactly the model matrix the
    // device is handed (projectToDevice).
    static Point3D place(const Node& n, float ux, float uy, float uz)
    {
        const float sx = ux * n.half.x * 2.0f, sy = uy * n.half.y * 2.0f, sz = uz * n.half.z * 2.0f;
        return Point3D{ n.pos.x + n.rot.at(0,0) * sx + n.rot.at(0,1) * sy + n.rot.at(0,2) * sz,
                        n.pos.y + n.rot.at(1,0) * sx + n.rot.at(1,1) * sy + n.rot.at(1,2) * sz,
                        n.pos.z + n.rot.at(2,0) * sx + n.rot.at(2,1) * sy + n.rot.at(2,2) * sz };
    }
    static Point3D corner(const Node& n, int i)
    {
        return place(n, (i & 1) ? 0.5f : -0.5f, (i & 2) ? 0.5f : -0.5f, (i & 4) ? 0.5f : -0.5f);
    }

    // The Mesh a node names, by RID and tag -- the way the surface finds it
    // to send to the device (HostSurface::sendMeshes). Not a family.
    static const Mesh* meshOf(ETCS::RID rid)
    {
        ETCS::Entity* e = ETCS::etcs_resolve_rid_anywhere(&ETCS::getLoader(), rid);
        if (!e || e->getSourceTag() != ETCS::Buffer("Mesh")) return nullptr;
        return static_cast<const Mesh*>(e->getTrueType());
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
    void rasterBox(const Sink& sink, const View& v, const Node& n)
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
            clipAndFill(sink, v, c[q[0]], c[q[1]], c[q[2]], col);
            clipAndFill(sink, v, c[q[0]], c[q[2]], c[q[3]], col);
        }
    }

    /*
     * A node's Mesh, triangle by triangle, through the same clip and fill the
     * box takes. Lit as the device lights it (shaders/mesh.frag): one fixed
     * lamp in the scene, 0.45 ambient and 0.55 of the normal's cosine, with
     * the triangle's stated normal turned by the node's row 3 -- so a shape
     * reads the same on the host and through the device. The box keeps its
     * six fixed shades: that is the picture every scene has always had.
     */
    void rasterMesh(const Sink& sink, const View& v, const Node& n, const Mesh& mesh)
    {
        const std::vector<float>&    vb = mesh.Vertices();
        const std::vector<uint32_t>& ib = mesh.Indices();
        static const float lx = 0.4f / 1.0028f, ly = 0.8f / 1.0028f, lz = 0.45f / 1.0028f;   // normalised (0.4, 0.8, 0.45)
        for (size_t t = 0; t + 2 < ib.size(); t += 3)
        {
            const uint32_t a = ib[t], b = ib[t + 1], c = ib[t + 2];
            if ((a + 1) * 6 > vb.size() || (b + 1) * 6 > vb.size() || (c + 1) * 6 > vb.size()) continue;
            const Point3D pa = toView(v, place(n, vb[a * 6], vb[a * 6 + 1], vb[a * 6 + 2]));
            const Point3D pb = toView(v, place(n, vb[b * 6], vb[b * 6 + 1], vb[b * 6 + 2]));
            const Point3D pc = toView(v, place(n, vb[c * 6], vb[c * 6 + 1], vb[c * 6 + 2]));
            // The normal: the first vertex's, through the rotation alone.
            const float nx0 = vb[a * 6 + 3], ny0 = vb[a * 6 + 4], nz0 = vb[a * 6 + 5];
            const float nx = n.rot.at(0,0) * nx0 + n.rot.at(0,1) * ny0 + n.rot.at(0,2) * nz0;
            const float ny = n.rot.at(1,0) * nx0 + n.rot.at(1,1) * ny0 + n.rot.at(1,2) * nz0;
            const float nz = n.rot.at(2,0) * nx0 + n.rot.at(2,1) * ny0 + n.rot.at(2,2) * nz0;
            const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
            const float cosl = len > 0.0f ? (nx * lx + ny * ly + nz * lz) / len : 0.0f;
            const float lit  = 0.45f + 0.55f * (cosl > 0.0f ? cosl : 0.0f);
            const float col[4] = { n.color[0] * lit, n.color[1] * lit, n.color[2] * lit, n.color[3] };
            clipAndFill(sink, v, pa, pb, pc, col);
        }
    }

    // Clip in view space, project what survives, fan it. The two steps are
    // separate because they answer different questions and only one of them
    // can be done after the divide -- see clipNear.
    void clipAndFill(const Sink& sink, const View& v,
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
            triangle(sink, v, s[0], s[i], s[i + 1], col);
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
    void triangle(const Sink& sink, const View& v,
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

        if (!sink.dst && !sink.px->PixelData()) return;

        for (int32_t y = y0; y <= y1; ++y)
        {
            const float py = static_cast<float>(y) + 0.5f;

            // The open run on this scanline. A pixel that fails the coverage
            // or depth test ENDS it -- flushing there rather than at the end
            // of the line is what keeps a run a run: the survivors of one
            // triangle are contiguous per scanline only between rejections.
            int32_t run_x0 = 0;
            int32_t run_x1 = -1;
            auto flush = [&]() { if (run_x1 >= run_x0) sink.run(y, run_x0, run_x1, col);
                                 run_x1 = -1; };

            for (int32_t x = x0; x <= x1; ++x)
            {
                const float pxf = static_cast<float>(x) + 0.5f;

                float w0 = (b.x - a.x) * (py - a.y) - (b.y - a.y) * (pxf - a.x);
                float w1 = (c.x - b.x) * (py - b.y) - (c.y - b.y) * (pxf - b.x);
                float w2 = (a.x - c.x) * (py - c.y) - (a.y - c.y) * (pxf - c.x);
                if (area < 0.0f) { w0 = -w0; w1 = -w1; w2 = -w2; }
                if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) { flush(); continue; }

                // w1 belongs to a, w2 to b, w0 to c -- each weight is the
                // area of the triangle OPPOSITE its vertex.
                const float la = w1 * std::fabs(inv_area);
                const float lb = w2 * std::fabs(inv_area);
                const float lc = w0 * std::fabs(inv_area);

                const float inv_z = la * iza + lb * izb + lc * izc;
                if (!(inv_z > 0.0f)) { flush(); continue; }
                const float z = 1.0f / inv_z;

                float& slot = m_depth[static_cast<size_t>(y) * v.w + x];
                if (z >= slot) { flush(); continue; }
                slot = z;

                if (run_x1 < run_x0) run_x0 = x;   // opening a new run
                run_x1 = x;
            }
            flush();   // the line ended while a run was still open
        }
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
 * THE STATE IS TWO ANGLES, AND ROW 3 IS REBUILT FROM THEM. Not composed onto
 * -- rebuilt, every time, from a yaw and a pitch this type owns. That is the
 * change that makes both of the limits below expressible at all, and it is
 * worth being explicit about why the incremental version could not have them.
 *
 * Composing each delta onto row 3 leaves the accumulated angles nowhere. The
 * orientation is a quaternion, which is a fine thing to hold a rotation in and
 * a useless thing to ask "how far up is this looking" -- so a pitch limit had
 * to be enforced by ROTATING FIRST AND INSPECTING THE RESULT, dropping the
 * whole delta if the answer came out past the pole. Two costs followed. The
 * limit was a cliff: near the top, a delta the size of an ordinary flick was
 * discarded entire, so the view stopped dead rather than easing into the stop.
 * And yaw had no bounded representation anywhere, so nothing could wrap it.
 *
 * With the angles held, both are arithmetic:
 *
 *   YAW WRAPS. It is an angle on a circle, so its range is the circle:
 *   accumulated and folded back into [-pi, pi). Turning right forever is the
 *   same few numbers recurring rather than a float32 growing without bound --
 *   which is not merely untidy, it is a loss of precision that shows up as the
 *   view getting coarser the longer you play, since a float carries far fewer
 *   fractional bits at 40,000 radians than at 3.
 *
 *   PITCH CLAMPS, and does not reach the axis. Up and down are NOT a circle:
 *   looking past straight up does not continue, it inverts the world. So pitch
 *   saturates at PITCH_LIMIT_RAD, short of the pole by a real margin rather
 *   than by an epsilon -- at the pole the forward vector is parallel to world
 *   up, the camera's right is undefined, and buildView correctly refuses to
 *   produce a basis, which is a blank frame. Clamping the scalar also DISCARDS
 *   the surplus, so holding the mouse up at the stop and then pulling down
 *   moves immediately, instead of first unwinding an invisible debt.
 *
 * YAW ABOUT WORLD UP, PITCH ABOUT THE REFERENCE RIGHT, applied pitch-then-yaw.
 * That order is what makes the two behave as a head turning rather than a body
 * rolling: the horizon stays level at any pitch, and pitch stays meaningful as
 * yaw changes. Euler order and gimbal lock do not arise -- there are two
 * angles and one composition, not a chain of three.
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

            // The axis pitch turns about, fixed at seeding rather than
            // recomputed per delta: it is the reference frame's right, and the
            // reference frame does not move. Flattened (y dropped) because
            // pitch about anything with a vertical component is roll.
            float rgx = m_ref_fwd.z, rgz = -m_ref_fwd.x;
            const float rl = std::sqrt(rgx * rgx + rgz * rgz);
            if (rl > 1e-5f) { m_ref_right = Point3D{ rgx / rl, 0.0f, rgz / rl }; }
            else
            {
                // The scene was pointed straight up or down to begin with, so
                // "right" is not determined by the look direction. Any
                // horizontal axis is as good as any other; naming one beats
                // leaving the first pitch undefined.
                m_ref_right = Point3D{ 1.0f, 0.0f, 0.0f };
                ETCS_LOG("Scene3D", "initial look is vertical -- pitch axis defaulted to "
                         "world +X. Point the camera off the pole to choose it.");
            }

            /*
             * HOW HIGH THE REFERENCE ALREADY LOOKS, and the whole reason the
             * limit needs it. m_ref_fwd is wherever the scene was pointed at
             * seeding -- scene3d.etcs seeds it 8.9 degrees below the horizon --
             * and the pitch applied below is a rotation FROM it, not an
             * elevation. Bounding the rotation therefore bounded the wrong
             * quantity: +-85 degrees about a reference already tilted down 8.9
             * put the actual view between +76.1 and -93.9, and -93.9 is past
             * straight down, where the world comes back the other way up. That
             * inversion is the view flipping on the way down the frame.
             *
             * Exact rather than approximate: m_ref_right is horizontal by
             * construction (y dropped above), so the rotation stays in the
             * plane of the horizontal direction and world up, and elevations
             * simply add.
             */
            m_ref_elev = std::asin(m_ref_fwd.y < -1.0f ? -1.0f
                                 : (m_ref_fwd.y > 1.0f ? 1.0f : m_ref_fwd.y));

            // Row 2: what the look turns about is the eye, in the scene's
            // frame -- a first-person look is a rotation about the viewer, and
            // that is exactly what a pivot is for.
            { std::lock_guard<std::recursive_mutex> lk(TreeMutex());
              Rows().SetPivot(Fixed::From(v.position.x) - Order4().x, Fixed::From(v.position.y) - Order4().y, Fixed::From(v.position.z) - Order4().z);
              Rows().Orient(Fixed::Zero(), Fixed::Zero(), Fixed::Zero(), Fixed::Zero()); }
            m_yaw = 0.0f;
            // The view's elevation, which at seeding is the reference's. Yaw
            // has no such absolute zero worth naming -- it is a circle -- so
            // the two angles are deliberately in different frames: the one
            // with a limit is stated where the limit means something.
            m_pitch = m_ref_elev;
            m_look_seeded = true;
            return;
        }
        if (!m_ptr_seen.load(std::memory_order_relaxed)) return;
        if (!m_look_dirty.exchange(false, std::memory_order_relaxed)) return;

        /*
     * THE ANGLES ARE READ, NOT INTEGRATED. Offset from the frame centre, as a
     * fraction of the frame, scaled onto each axis's range.
     *
     * Yaw needs no wrap and pitch needs no clamp applied afterwards: the
     * pointer cannot leave the frame, so the fractions are bounded, so the
     * angles are bounded by construction. clampPitch is still called because
     * it is where the limit is DEFINED -- the span below is stated in terms of
     * it -- and a value that is already inside a range costs nothing to check.
     */
        const float w = static_cast<float>(m_frame_w ? m_frame_w : NOMINAL_FRAME_W);
        const float h = static_cast<float>(m_frame_h ? m_frame_h : NOMINAL_FRAME_H);
        const float fx = (static_cast<float>(m_ptr_x.load(std::memory_order_relaxed)) / w) - 0.5f;
        const float fy = (static_cast<float>(m_ptr_y.load(std::memory_order_relaxed)) / h) - 0.5f;

        m_yaw   = wrapAngle(fx * 6.28318530718f * m_sens_scale);
        /*
     * THE FRAME'S HEIGHT READ AS AN ELEVATION: top edge +85 degrees, bottom
     * edge -85, horizon exactly halfway down. Negated because screen y grows
     * downward.
     *
     * NO SPAN MULTIPLIER ON THIS AXIS, and that is not an omission.
     * Compressing an axis means going round it more than once before the
     * pointer reaches the edge, which a circle can do and a bounded range
     * cannot: multiplying pitch only pushed the ends of the range inside the
     * frame, so at 2.0 the top and bottom quarters were past the stop and did
     * nothing at all. Sensitivity is turns across the WIDTH, which is what it
     * has always claimed to be.
     *
     * The height therefore maps onto the pitch range exactly, so clampPitch is
     * finally the no-op check its own comment describes rather than the thing
     * doing the work.
     */
        m_pitch = clampPitch(-fy * 2.0f * PITCH_LIMIT_RAD);

        // Rebuilt from the two angles, never accumulated onto. RotateBy
        // composes from the left, so the pitch written first is applied first
        // and the yaw written second wraps around it -- pitch in the reference
        // frame, yaw in the world's, which is the level-horizon order.
        //
        // m_pitch is where the view should END UP; what is applied is the
        // rotation that gets there from where the reference already was.
        // ref_elev MINUS the target, not plus, and the direction was measured
        // rather than reasoned: a positive rotation about m_ref_right tilts
        // the forward vector DOWN, so composing it the other way round both
        // left the result unbounded and pointed the control the wrong way --
        // the pointer at the top of the frame aimed the camera at the ground.
        // Written this way the identity is exact: the view's elevation comes
        // out as m_pitch, whatever the reference was.
        const float pitch_rot = m_ref_elev - m_pitch;
        // The look's two angles cross into the rows here: the mouse is an
        // input like a key, and row 3 is causal state -- so the angles are
        // taken as Fixed and the spinor is composed by Fixed's own series.
        Fixed fx2 = Fixed::From(m_ref_fwd.x), fy2 = Fixed::From(m_ref_fwd.y), fz2 = Fixed::From(m_ref_fwd.z);
        {
            std::lock_guard<std::recursive_mutex> lk(TreeMutex());
            Rows().Orient(Fixed::Zero(), Fixed::Zero(), Fixed::Zero(), Fixed::Zero());
            if (pitch_rot != 0.0f)
                Rows().RotateBy(Fixed::From(m_ref_right.x), Fixed::From(m_ref_right.y), Fixed::From(m_ref_right.z), Fixed::From(pitch_rot));
            if (m_yaw != 0.0f)
                Rows().RotateBy(Fixed::Zero(), Fixed::One(), Fixed::Zero(), Fixed::From(m_yaw));
            Order4().RotateVector(fx2, fy2, fz2);
        }
        v.look_at = Point3D{ v.position.x + fx2.ToFloat() * dist,
                             v.position.y + fy2.ToFloat() * dist,
                             v.position.z + fz2.ToFloat() * dist };
        camera->SetView(v);

        // The frame's extent is what the mapping is stated against, and it can
        // change under us (a resize of the camera's plane), so it is
        // re-measured every look rather than sampled once at seeding.
        m_frame_w = cameraWidth(camera);
        m_frame_h = cameraHeight(camera);
    }

    /*
 * THE LENS NO LONGER FEEDS THE LOOK, and radiansPerPixel/radPerViewPixel are
 * gone with it. An absolute mapping is stated against the FRAME, not against
 * what the frame shows: the pointer's position over the box is the angle, and
 * how wide a cone that box represents is a separate question. Deriving the
 * rate from tan(fov/2) conflated the two, which is why changing SetLens used
 * to change how far a movement turned you.
 */

    static uint32_t cameraWidth(Camera_* c)
    {
        Drawable2D_* plane = cameraPlane(c);
        return plane ? plane->Bounds().w : 0u;
    }

    static uint32_t cameraHeight(Camera_* c)
    {
        Drawable2D_* plane = cameraPlane(c);
        return plane ? plane->Bounds().h : 0u;
    }

    /*
 * Fold an angle back onto the circle, into [-pi, pi).
 *
 * SUBTRACTING MULTIPLES RATHER THAN fmod-ing ONCE, because the input is an
 * accumulated angle plus one frame's worth of delta -- so it is already in
 * range or a single turn outside it, and a loop that almost never runs beats a
 * division. The bound on iterations is the caller's: no single frame's pending
 * yaw is more than a few turns.
 *
 * The range is half-open on purpose. Exactly -pi and exactly +pi are the same
 * direction, and letting both exist means a value that is arithmetically
 * bounded but still has two spellings -- which is the sort of thing that reads
 * as a flicker when something compares them.
 */
    static float wrapAngle(float a)
    {
        constexpr float TWO_PI = 6.28318530718f;
        constexpr float PI     = 3.14159265359f;
        while (a >= PI)  a -= TWO_PI;
        while (a < -PI)  a += TWO_PI;
        return a;
    }

    /*
 * Saturate pitch short of the pole. See applyLookTo's header for why this is a
 * clamp and yaw is a wrap -- up is not a circle you come back around.
 *
 * THE MARGIN IS FIVE DEGREES, not an epsilon, and the size of it is the whole
 * decision. What fails at the pole is not the arithmetic, it is the BASIS:
 * forward becomes parallel to world up, their cross product goes to zero, and
 * the camera has no right vector to build a view from. Approaching that, the
 * basis does not fail so much as become badly CONDITIONED -- the horizon's
 * direction is decided by an ever-smaller cross product, so it swings further
 * and further for the same tiny movement. The last couple of degrees are
 * visibly unstable well before anything actually breaks, which is why a limit
 * set just shy of the singularity does not feel like a limit at all, it feels
 * like the control coming apart at the top of its range.
 *
 * The previous limit was 87.4 degrees, and it was expressed as a rejection
 * test on the forward vector's y (|y| < 0.999) rather than as an angle -- so
 * what it actually bounded was hard to see, and it sat inside the unstable
 * band. Five degrees is a margin you can state, and one that leaves the sky
 * and the floor both comfortably in view.
 */
    // 85 degrees. sin(85 deg) = 0.9962, so the forward vector keeps a
    // horizontal component of ~0.087 -- small, but two orders of magnitude
    // clear of the zero that collapses the basis. At class scope because the
    // vertical axis's SPAN is stated in terms of it (applyLookTo): the frame's
    // height maps onto exactly this range, so the limit and the mapping cannot
    // drift apart.
    static constexpr float PITCH_LIMIT_RAD = 1.48353f;

    static float clampPitch(float p)
    {
        if (p >  PITCH_LIMIT_RAD) return  PITCH_LIMIT_RAD;
        if (p < -PITCH_LIMIT_RAD) return -PITCH_LIMIT_RAD;
        return p;
    }

    // (addPending/takePending are gone: an absolute look accumulates nothing,
    // so there is no pending angle to add onto or take away.)

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
    // FROM THE ROWS, NOT FROM THE PICTURE: the members' positions are the
    // rows summed down the tree in Fixed, never the floats the projection
    // made of them, because the reach lands in row 2 and row 2 is hashed.
    // The half-extent is the one float that enters, crossed once -- it is
    // the size a script stated at Create.
    void coverRows()
    {
        std::vector<OrderVector> parts;
        gatherParts(Fixed::Zero(), Fixed::Zero(), Fixed::Zero(), parts);
        std::lock_guard<std::recursive_mutex> lk(TreeMutex());
        Rows().Cover(parts.data(), parts.size());
    }
    void gatherParts(Fixed ox, Fixed oy, Fixed oz, std::vector<OrderVector>& out)
    {
        Fixed ax, ay, az;
        {
            std::lock_guard<std::recursive_mutex> lk(TreeMutex());   // one whole position, not a mid-step one
            ax = ox + Order4().x; ay = oy + Order4().y; az = oz + Order4().z;
        }
        if (!Hidden())
        {
            OrderVector p;
            p.x = ax; p.y = ay; p.z = az;
            p.radius = Fixed::Length(Fixed::From(m_half.x), Fixed::From(m_half.y), Fixed::From(m_half.z));
            out.push_back(p);
        }
        { const auto kids = ownChildren(); for (Scene3D* kid : *kids) kid->gatherParts(ax, ay, az, out); }
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
    Point3D m_half{0.5f, 0.5f, 0.5f};
    // A value's words (Entity::putWord), and back: false when there is no
    // value or it is not `n` words -- the caller's default then applies.
    static std::string words(std::initializer_list<int64_t> ws)
    {
        std::string v;
        for (int64_t x : ws) ETCS::Entity::putWord(v, x);
        return v;
    }
    static bool readWords(const std::string* v, int64_t* out, size_t n)
    {
        if (!v || v->size() != 8 * n) return false;
        size_t at = 0;
        for (size_t i = 0; i < n; ++i) ETCS::Entity::getWord(*v, at, out[i]);
        return true;
    }

    ETCS::RID m_mesh = 0;
    static constexpr float kColor[4] = {0.8f, 0.8f, 0.85f, 1.0f};
    float   m_color[4] = {kColor[0], kColor[1], kColor[2], kColor[3]};
    // Causal: they decide what an impulse is and how motion decays, so they
    // are Fixed and cross from a script's floats once, in their setters.
    static inline const Fixed kSpeed   = Fixed::FromInt(6);
    static inline const Fixed kDamping = Fixed::FromInt(8);
    Fixed   m_speed    = kSpeed;      // terminal, scene units per second
    Fixed   m_damping  = kDamping;    // kinetic -> heat, per second
    Fixed   m_mass     = Fixed::One();
    Fixed   m_drag_dt, m_drag_damping, m_drag_factor;   // exp(-k dt) for the last (k, dt) seen

    OwnKids     m_own_kids;        // the child lists at m_kids_epoch (refreshKids)
    ForeignKids m_foreign_kids;
    uint32_t                  m_kids_epoch = 0;

    ETCS::TBuffer<NUM_KEYS / 8> m_held;   // one bit per key in the spectrum
    std::atomic<uint32_t>       m_motion{0};   // the six bits that cross threads

    // The look. Atomics for the same reason the motion bits are: written by
    // the input edge, read by the projection, on different threads. The
    // ORIENTATION itself is not here -- it is row 3 of the rows (Order4), where an angle
    // belongs; these are only the deltas waiting to be folded into it.
    // Where the pointer is, in the camera frame's own pixels. Written by the
    // input edge, read by the frame thread. Relaxed on both sides: the two
    // components are read one frame at a time and a mismatched pair would be
    // a sub-pixel disagreement in a value that is about to be turned into an
    // angle, which is not a hazard worth a fence.
    std::atomic<int32_t> m_ptr_x{0};
    std::atomic<int32_t> m_ptr_y{0};
    // Until the pointer has been seen once, the look holds whatever LookAt
    // set -- so a scene renders as authored rather than snapping to whatever
    // corner the cursor happens to be in.
    std::atomic<bool>  m_ptr_seen{false};
    std::atomic<bool>  m_look_dirty{false};
    bool               m_look_seeded = false;
    Point3D            m_ref_fwd{0.0f, 0.0f, 1.0f};   // what row 3 rotates FROM
    Point3D            m_ref_right{1.0f, 0.0f, 0.0f}; // and what pitch turns about

    // THE LOOK'S ACTUAL STATE. Row 3 is derived from these every time, not the
    // other way round -- see applyLookTo. Held here rather than read back out
    // of the quaternion because a bounded yaw and a clamped pitch are
    // questions a quaternion cannot answer without being taken apart.
    // Frame-thread only, like the rest of applyLookTo; the atomics above are
    // the input edge's side of the handoff.
    float              m_yaw   = 0.0f;   // wrapped to [-pi, pi)
    float              m_pitch = 0.0f;   // clamped short of the pole
    // What the rate falls back to before any camera has been looked through.
    // A starting value only: the first frame measures the real lens and
    // replaces it, so nothing in the model rests on these.
    static constexpr uint32_t NOMINAL_FRAME_W    = 1024u;
    static constexpr uint32_t NOMINAL_FRAME_H    = 768u;

    uint32_t           m_frame_w    = 0;
    uint32_t           m_frame_h    = 0;

    Fixed              m_ground_fx;                 // last usable horizontal facing
    Fixed              m_ground_fz  = Fixed::One();
    /*
 * ONE view pixel of scene movement per pixel of pointer movement.
 *
 * The default is 1.0 because 1.0 is the only value in this unit that means
 * something on its own: the world tracks the cursor. Every other number is a
 * preference stated as a multiple of that, which is what makes this a knob
 * worth handing over -- 0.5 is "half as fast as my hand", and that is a
 * sentence, where "0.5 turns per pass" was a measurement of the wrong thing.
 *
 * For scale against what this replaces: the old rate, at the 60-degree lens
 * render_scene3d.etcs uses, worked out to 4.08 in these units. Measured
 * directly -- 40 pixels of pointer travel slid the image 167 pixels.
 */
/*
 * TWO full turns across the frame's width, not one.
 *
 * One turn is the tidier number and the worse control. It puts a half turn --
 * the commonest thing anybody asks a look for, turning to face behind them --
 * at the very edge of the frame, so the pointer spends its time at the borders.
 * Two turns puts a half turn at a QUARTER of the width from centre, which keeps
 * ordinary use in the middle of the frame with room either side.
 *
 * The axis is compressed rather than clipped: every direction is still
 * reachable, twice over, and the frame's own edges both land back at the
 * starting direction. Nothing is lost by it.
 *
 * PITCH IS COMPRESSED BY THE SAME FACTOR, so its stops arrive a quarter of the
 * way up and down from centre. That follows the same reasoning, and it is why
 * one knob scales both: a diagonal drag should not change meaning halfway
 * through. Decoupling them is a one-line change if the wasted vertical band
 * turns out to matter more than the uniformity.
 */
    static constexpr float kSensitivity = 2.0f;
    float              m_sens_scale = kSensitivity;
    // The seeded direction's elevation. Frame-thread only, written once at
    // seeding -- see applyLookTo's seeding block for what it is for.
    float              m_ref_elev   = 0.0f;

    // Motion: a tenth of a second is the most unobserved movement this will
    // believe in. See Interact().
    StepClock                             m_motion_clock{ 100.0 };

    // The entropy ledger: the interval this node was last charged for, how fast
    // it sheds heat, and what has left the model entirely through the root. A
    // full second, deliberately unequal to the motion ceiling above it.
    StepClock                             m_entropy_clock{ 1000.0 };

    std::vector<float>     m_depth;
    uint32_t               m_depth_w   = 0;
    uint32_t               m_depth_h   = 0;
    ETCS::RID              m_depth_cam = 0;
    uint64_t               m_projections = 0;
};

#endif
