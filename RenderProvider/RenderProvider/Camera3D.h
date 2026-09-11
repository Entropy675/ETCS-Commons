#ifndef CAMERA3D_H__
#define CAMERA3D_H__

#include "../../../core_defs.h"
#include "../../../ontology.h"

#include <cmath>
#include <cstdint>
#include <mutex>
#include <vector>

// ---------------------------------------------------------------------------
// Camera3D — the Camera leaf: a 2D plane that a 3D scene fills.
//
// STRUCTURALLY A CompositeDrawable2D WITH A DIFFERENT SOURCE, and the
// resemblance is the point rather than duplication. A compositor's pixels come
// from its children; a camera's come from a scene it names. Everything after
// that -- owning a buffer, the dirty flag deciding whether to rebuild, one
// Blit into the destination, nesting in the 2D tree with parent-relative
// coordinates -- is identical, because both are Drawable2D nodes that own
// pixels, and that is all the rest of the system ever asks.
//
// So a camera view drops into the 2D tree with no adapter: put it under a
// compositor and it composites, put UI nodes under IT and they draw on top of
// the 3D view, blit it into an ImageSurface and you have a screenshot. "The 3D
// view is the main surface and the 2D tree is the UI over it" is a
// composition, not an architecture, and this file is where that stops being a
// claim.
//
// THE DIRTY SEQUENCE is CompositeDrawable2D's, one step longer:
//
//   1. the scene moves -> it marks its registered viewers (Scene3D.h)
//   2. DrawInto calls TakeObserved(own RID): true, so it renders
//   3. Render resolves the scene and asks it to Project into here
//   4. Project writes pixels, which sets the flag again
//   5. the destination's Blit calls TakeObserved(its own RID): true, so it re-uploads
//
// and a still scene stops at step 2: no resolve, no projection, no depth
// buffer, one blit of an image the device already holds. A 3D view that costs
// nothing while nothing moves is the same property the compositor has, gained
// the same way.
//
// WHY THE SCENE IS AN RID AND NOT A POINTER. The camera outliving its scene is
// an ordinary thing -- the script deletes the scene, or the closure that made
// it ends -- and a camera holding a raw pointer would find that out by
// dereferencing it. Resolution by RID and family (ontology/Camera.h) turns
// that into a false return, once per frame, at a cost nobody can measure.
// ---------------------------------------------------------------------------
class Camera3D : public CameraBase<Camera3D>,
                 public PixelsBase<Camera3D>,
                 public ClippableBase<Camera3D>,
                 public DeletableBase<Camera3D>
{
public:
    WIRE_TYPE_IDENTITY(Camera3D);

    // --- Orderable_ (required by Surface, which Drawable refines) ---
    int32_t m_order = 0;
    bool operator<(const Camera3D& o) const { return m_order < o.m_order; }
    int32_t Order() override { return m_order; }

    Camera3D()  = default;
    ~Camera3D() = default;

    bool Create(uint32_t w, uint32_t h)
    {
        if (w == 0 || h == 0)
        {
            ETCS_LOG("Camera3D", "Create with a zero dimension (" << w << "x" << h << ").");
            return false;
        }
        m_w = w;
        m_h = h;
        Allocate(w, h);          // idempotent for an unchanged size (Pixels_)
        // Watch my own scene: the render gate in DrawIntoConcrete reads this.
        this->ObserveSelf();
        this->addTag("active");
        return true;
    }

    void SetPosition(int32_t x, int32_t y)
    { m_x = x; m_y = y; markPath(); MarkObservedBelow(); }
    void SetOrder(int32_t z)               { m_order = z; Reorder(); markPath(); }

    /*
     * The family verbs -- a camera is an ordinary 2D node and lays out as one.
     *
     * STAGED, NOT APPLIED, for the reason CompositeDrawable2D::MoveTo sets out
     * at length: a resize arrives on the event pump while the frame edge is
     * reading this buffer, and reallocating under it is a segfault waiting for
     * a burst of resize events to line up. Applied in DrawIntoConcrete, on the
     * thread that owns the pixels.
     *
     * Nothing is preserved across the resize, unlike a compositor's: a
     * camera's buffer is DERIVED, and the next projection fills it completely.
     * Carrying the old frame forward would show one stale image for one frame
     * and cost a copy to do it.
     */
    bool MoveTo(Point2D p) override
    {
        {
            std::lock_guard<std::mutex> g(m_pending_mtx);
            stageFrom();
            m_pending_x = p.x;
            m_pending_y = p.y;
        }
        markPath();
        return true;
    }

    bool ResizeTo(WindowSize s) override
    {
        if (s.width == 0 || s.height == 0) return false;
        {
            std::lock_guard<std::mutex> g(m_pending_mtx);
            stageFrom();
            m_pending_w = s.width;
            m_pending_h = s.height;
        }
        markPath();
        return true;
    }

    // What the frame is cleared to before the scene is drawn into it. The sky,
    // in other words -- and transparent by default, so a camera nested over
    // other 2D content shows it through wherever no geometry landed.
    void SetBackground(float r, float g, float b, float a)
    {
        m_bg[0] = r; m_bg[1] = g; m_bg[2] = b; m_bg[3] = a;
        markPath();
    }

    // Pose and lens go in together (ontology/Camera.h on why), so the two
    // script-facing setters below both round-trip through the whole struct
    // rather than writing half of it.
    void LookAt(float ex, float ey, float ez, float tx, float ty, float tz)
    {
        ViewFrustum v = m_view;
        v.position = Point3D{ex, ey, ez};
        v.look_at  = Point3D{tx, ty, tz};
        SetViewConcrete(v);
    }
    void SetLens(float fov_degrees, float near_plane, float far_plane)
    {
        ViewFrustum v = m_view;
        v.fov_y_radians = fov_degrees * 3.14159265f / 180.0f;
        v.near_plane    = near_plane;
        v.far_plane     = far_plane;
        SetViewConcrete(v);
    }

    // ── Camera_ dispatch ─────────────────────────────────────────────────

    void SetViewConcrete(ViewFrustum view) override
    {
        m_view = view;
        markPath();              // a moved eye is a different image
    }
    ViewFrustum GetViewConcrete() override { return m_view; }

    void SetSceneConcrete(ETCS::RID scene) override
    {
        m_scene = scene;
        markPath();
    }
    ETCS::RID GetSceneConcrete() override { return m_scene; }

    // Resolve, hand over self, done. The whole of what this family mandates,
    // and deliberately the whole of what this method does: how the image is
    // produced is the scene's business (ontology/Camera.h), and a camera that
    // knew would be describing one renderer.
    /*
     * THE DEVICE TOGGLE -- the standing preference half. Camera.h holds the
     * other half, and holds it as a derivation rather than a second field: the
     * effective mode is this AND a Device child still being there, computed at
     * read time, so the two can never disagree.
     *
     * DEFAULTS TO TRUE, and that is what makes attaching a device enough. See
     * Camera.h: once presence implies use, "forced host" and "use one if you
     * have one" are the only two states that differ, so this is a boolean whose
     * default is yes rather than a mode anyone has to select.
     *
     * SO ASKING FOR IT WITH NO DEVICE IS NOT REFUSED, which reverses what this
     * did when the default was false. Then it would have been a toggle that
     * said yes and changed nothing -- worth refusing. Now it records a standing
     * preference that takes effect the moment a device appears, which is a real
     * answer to a real question. Turning it OFF always succeeds; there is
     * always a CPU.
     */
    bool SetDeviceProjectionConcrete(bool on) override
    {
        if (m_want_device == on) return true;
        m_want_device = on;
        // A mode change is a state change: what this camera will produce next
        // frame is different, so everything watching it has something to re-take.
        etcs_mark_observed(this);
        MarkObservedBelow();
        return true;
    }

    bool DeviceProjectionRequestedConcrete() const override { return m_want_device; }

    bool RenderConcrete() override
    {
        if (m_scene == 0) return false;
        // Held for the same reason the compose walk is: Project walks somebody
        // else's scene graph, so the answer has to stay true for the whole
        // projection rather than for the instant it was given. Falsy now also
        // means "being deleted right now", which is nothing to render either.
        ETCS::Held<Drawable3D_> scene = ETCS::resolve_held<Drawable3D_>("Drawable3D", m_scene);
        if (!scene)
        {
            ETCS_LOG("Camera3D", "scene RID:" << m_scene
                     << " is gone or going -- nothing to render.");
            return false;
        }
        /*
         * CLEARED THE WAY IT WILL BE DRAWN. On the host that is the buffer's
         * own ClearTo; through a device it is the SURFACE verb, which this
         * camera records and replays like every other op -- because in that
         * mode there is no buffer to clear, and Clear on a retained op list
         * means "a new composition starts here" exactly as it does for
         * PolygonDrawable2D and VulkanSurface.
         */
        if (this->DeviceProjection()) Clear(m_bg[0], m_bg[1], m_bg[2], m_bg[3]);
        else                          ClearTo(m_bg[0], m_bg[1], m_bg[2], m_bg[3]);
        ++m_renders;
        return scene->Project(this) != nullptr;
    }

    // ── Drawable2D_ dispatch ─────────────────────────────────────────────

    Rect2D BoundsConcrete() override { return Rect2D{ m_x, m_y, m_w, m_h }; }

    // Rectangular, for the same reason a compositor is: the shape of an image
    // plane is the plane.
    bool ContainsLocalConcrete(int32_t x, int32_t y) override
    {
        return x >= 0 && y >= 0
            && x < static_cast<int32_t>(m_w)
            && y < static_cast<int32_t>(m_h);
    }

    // ── Surface_ dispatch: these RASTERISE into the frame ────────────────
    //
    // A camera is a surface you can draw ON as well as read: a crosshair, a
    // label, a debug overlay. Children nested under it get these as their
    // destination and land in the projected image, above the geometry,
    // because they run after Render in the same recomposition.

    /*
     * TWO SINKS, ONE SET OF VERBS. On the host these rasterise into the
     * camera's own buffer, which is what they have always done. Through a
     * device they RECORD, and DrawInto replays them onto the destination --
     * the same retained model PolygonDrawable2D and VulkanSurface both use,
     * for the reason PolygonDrawable2D states: "the thread that decides what
     * to draw is not the thread that draws it".
     *
     * The buffer is left alone in device mode rather than freed. It is the
     * camera's size times four bytes, the toggle is meant to be flipped while
     * running, and dropping it would make each flip a reallocation -- see
     * MoveTo on why reallocating a camera's buffer off the drawing thread is
     * the one thing this class is careful about.
     */
    void ClearConcrete(float r, float g, float b, float a) override
    {
        if (this->DeviceProjection())
        {
            // A new composition starts here, exactly as it does for every
            // other retained surface -- so the ops before it are dropped
            // rather than drawn under it.
            std::lock_guard<std::mutex> lk(m_ops_mtx);
            m_ops.clear();
            m_ops.push_back(Op{Op::Kind::Clear, 0, 0, 0, 0, {r, g, b, a}});
            return;
        }
        ClearTo(r, g, b, a);
    }

    void DrawRectConcrete(int32_t x, int32_t y, uint32_t w, uint32_t h,
                          float r, float g, float b, float a) override
    {
        int32_t cx, cy; uint32_t cw, ch;
        CurrentClip(cx, cy, cw, ch);
        clipToRegion(x, y, w, h, cx, cy, cw, ch);
        if (w == 0 || h == 0) return;

        if (this->DeviceProjection())
        {
            // In the CAMERA's own space. Where it lands is composed at replay
            // time (DrawInto), which is the upward half of the 2D contract and
            // the reason nothing here stores an absolute position.
            std::lock_guard<std::mutex> lk(m_ops_mtx);
            m_ops.push_back(Op{Op::Kind::Rect, x, y, w, h, {r, g, b, a}});
            return;
        }
        FillRect(x, y, w, h, r, g, b, a);
    }

    void BlitConcrete(Surface_* source, int32_t x, int32_t y,
                      uint32_t w, uint32_t h, float opacity) override
    {
        if (!source) return;
        Pixels_* px = static_cast<Pixels_*>(
            source->getInterfacePointer(ETCS::Buffer("Pixels")));
        if (!px)
        {
            ETCS_LOG("Camera3D", "Blit source RID:" << source->getRID()
                     << " owns no pixels -- a device-side source cannot be read "
                        "back into a camera frame.");
            return;
        }
        (void)w; (void)h;        // 1:1, as everywhere on this side -- Pixels_::Composite
        Composite(*px, x, y, opacity);
    }

    // ── Clippable_ ───────────────────────────────────────────────────────
    // Nothing device-side to set; this rasteriser reads CurrentClip at draw
    // time. Same as CompositeDrawable2D, and for the same reason.
    void SetScissorConcrete(int32_t, int32_t, uint32_t, uint32_t) override {}

    /*
     * The recording, for device mode only. One flat list in call order,
     * because that IS the composition -- a layered picture is drawn
     * back-to-front and the order calls arrive in is the order they must be
     * replayed in, which is the same reason VulkanSurface keeps ONE
     * m_pendingDraws rather than three lists.
     *
     * Coordinates are the camera's own; the destination offset is added at
     * replay. Clear carries no rect: it means the whole frame, whose size is
     * known only once there is a destination to state it against.
     */
    struct Op
    {
        enum class Kind : uint8_t { Clear, Rect };
        Kind     kind;
        int32_t  x, y;
        uint32_t w, h;
        float    c[4];
    };

    // ── Drawable_ dispatch: render-if-stale, then one blit ───────────────
    void DrawIntoConcrete(Surface_* dst) override
    {
        if (!dst) return;
        applyPendingGeometry();     // before anything reads the buffer

        // The branch this class shares with CompositeDrawable2D, plus the one
        // thing a flag cannot express. The observed bit covers every discrete change
        // -- the eye moved, the scene was rebound, a box was repainted. A
        // scene in MOTION is not a discrete change: it changes during the very
        // walk that draws it, so the mark it leaves is consumed by this
        // frame's own upload and there is nothing left to schedule the next
        // frame with. Asking is what closes that loop, and it costs a load.
        if (TakeObserved(getRID()) || sceneInMotion() || anyChildAnimating())
        {
            // No self-clear: Render's writes mark with origin=this, so my own
            // edge is skipped at the source rather than cleared afterwards.
            Render();
            drawOverlay();
        }

        const Point2D base = parentAbsoluteOrigin();

        /*
         * ONE BLIT, OR THE RECORDING REPLAYED, and which one is the whole
         * visible difference between the two modes.
         *
         * The host frame is a buffer, so it reaches the destination the way
         * every buffer does -- one Blit, which on a VulkanSurface is an upload
         * and a textured quad. The device frame was never a buffer: it is the
         * spans the projection produced, replayed onto the destination as its
         * own draws, so on a VulkanSurface they become device rects and the
         * frame is produced without host pixels existing at any point.
         *
         * Replayed under the lock, into a local copy: the frame edge runs on
         * a different thread from the projection that fills this list.
         */
        if (this->DeviceProjection())
        {
            std::vector<Op> ops;
            { std::lock_guard<std::mutex> lk(m_ops_mtx); ops = m_ops; }
            for (const Op& o : ops)
            {
                if (o.kind == Op::Kind::Clear)
                    dst->DrawRect(base.x + m_x, base.y + m_y, m_w, m_h,
                                  o.c[0], o.c[1], o.c[2], o.c[3]);
                else
                    dst->DrawRect(base.x + m_x + o.x, base.y + m_y + o.y, o.w, o.h,
                                  o.c[0], o.c[1], o.c[2], o.c[3]);
            }
            return;
        }

        dst->Blit(this, base.x + m_x, base.y + m_y, m_w, m_h, 1.0f);
    }

    /*
 * A camera animates when the scene it looks at is moving, OR when anything it
 * overlays does -- and the second half was missing.
 *
 * The dirty bit already covers a child changing DISCRETELY: TextLabel::SetText
 * marks its own path and that bubbles onto this camera's Observable, so
 * TakeObserved above fires. Animating is for the other kind of child, the one
 * with no discrete change to mark because what it displays changes on its own.
 * TextLabel::BindFps makes exactly that: Animating() true forever, because a
 * live readout is never "already up to date".
 *
 * Nothing asked. CompositeDrawable2D asks its children, so a label under a
 * compositor works; a label under a CAMERA answered a question no one put to
 * it, and the hud in scene3d.etcs sat frozen with the pipeline settled around
 * it. The walk itself is Drawable_::anyChildAnimating now, beside the child
 * list it reads -- one copy for every node that composites.
 *
 * THE COST IS REAL AND IT IS THE POINT. An animating overlay child now keeps
 * this camera rendering -- a full re-projection per frame, because the overlay
 * is composited into the same buffer the 3D image is drawn in and cannot be
 * repainted alone without clearing what is under it. That is what asking for a
 * live per-frame readout over a 3D view costs. A scene with no animating child
 * still settles exactly as before.
 */
    bool Animating() override { return sceneInMotion() || anyChildAnimating(); }

    // ── Resizable_ / Deletable_ ──────────────────────────────────────────

    WindowSize GetSizeConcrete() override { return WindowSize{ m_w, m_h }; }

    bool DeleteConcrete() override
    {
        std::string conjugate_key = getSourceModule().toString() + ":" + getSourceTag().toString();
        ETCS_LOG("Camera3D", "firing self-DestroyEvent for RID:" << getRID());
        return ETCS::DestroyEvent{conjugate_key.c_str(), this}();
    }

    // How many projections this camera has actually driven. The observable
    // form of the claim above -- a settled scene stops incrementing it, and a
    // scene under WASD does not.
    uint64_t Renders() const { return m_renders; }

private:
    /*
 * Is the bound scene still moving?
 *
 * Answered only for this module's own 3D leaf, and by TAG rather than by a
 * cast: the family pointer says "a 3D node", it does not say "one of mine",
 * and reading another module's fields off a family pointer is the mistake the
 * interface-pointer discipline exists to prevent. Scene3D makes the same
 * split for the same reason when it walks its subtree.
 *
 * A FOREIGN leaf falls back to the dirty flag alone, which is correct for
 * anything that changes discretely and wrong for anything that animates
 * itself. The general fix is a family-level "has your projection changed
 * since epoch N" -- one counter, one compare, and it would subsume this and
 * the flag both. Not added here because it changes the dispatch set for every
 * 3D leaf that exists, which is a bigger decision than this file gets to make
 * on its own.
 */
    bool sceneInMotion()
    {
        if (m_scene == 0) return false;
        // Three calls through the pointer, so it is held across them.
        ETCS::Held<Drawable3D_> scene = ETCS::resolve_held<Drawable3D_>("Drawable3D", m_scene);
        if (!scene) return false;
        if (scene->getSourceTag() != ETCS::Buffer("Scene3D")) return false;
        return static_cast<Scene3D*>(scene->getTrueType())->InMotion();
    }

    // Mark this frame stale AND every observer above it: marking only myself
    // leaves a stale view inside a clean parent, which is blitted, correctly,
    // forever. The walk lives in ontology/Observable.h.
    void markPath() { etcs_mark_observed(this); }

    // Seeded from the live values on first stage, so a MoveTo alone does not
    // drag a stale size along with it. Called under m_pending_mtx.
    void stageFrom()
    {
        if (m_pending) return;
        m_pending   = true;
        m_pending_x = m_x;  m_pending_y = m_y;
        m_pending_w = m_w;  m_pending_h = m_h;
    }

    // On the compose thread only. See MoveTo.
    void applyPendingGeometry()
    {
        int32_t nx, ny; uint32_t nw, nh;
        {
            std::lock_guard<std::mutex> g(m_pending_mtx);
            if (!m_pending) return;
            m_pending = false;
            nx = m_pending_x; ny = m_pending_y;
            nw = m_pending_w; nh = m_pending_h;
        }
        m_x = nx; m_y = ny;
        if (nw == m_w && nh == m_h) return;
        m_w = nw; m_h = nh;
        Allocate(m_w, m_h);
    }

    // 2D children draw over the projected frame, in the camera's own space.
    // Run after Render inside the same dirty window, so an overlay never
    // appears for a frame without the geometry under it or vice versa.
    void drawOverlay()
    {
        std::vector<Drawable_*> ordered;
        collectDrawableChildren(ordered);
        if (ordered.empty()) return;

        PushClip(0, 0, m_w, m_h);
        for (Drawable_* child : ordered)
        {
            // A 3D child of a camera is scenery, not UI -- it is drawn by
            // being projected, which Render already did or will do through
            // the bound scene. Drawing it here would paint it flat.
            if (child->getInterfacePointer(ETCS::Buffer("Drawable3D"))) continue;
            child->DrawInto(this);
        }
        PopClip();
    }

    // Where this node's PARENT sits, stopping at the first ancestor that is a
    // raster, because such an ancestor is a coordinate origin. Identical to
    // CompositeDrawable2D's and PolygonDrawable2D's -- the three are
    // interchangeable as children, so they must agree on what a position
    // means.
    Point2D parentAbsoluteOrigin()
    {
        Point2D acc{0, 0};
        for (ETCS::Entity* node = getParent(); node; node = node->getParent())
        {
            void* d2 = node->getInterfacePointer(ETCS::Buffer("Drawable2D"));
            if (!d2) break;
            if (node->getInterfacePointer(ETCS::Buffer("Raster"))) break;  // origin
            const Rect2D pb = static_cast<Drawable2D_*>(d2)->Bounds();
            acc.x += pb.x;
            acc.y += pb.y;
        }
        return acc;
    }

    static void clipToRegion(int32_t& x, int32_t& y, uint32_t& w, uint32_t& h,
                             int32_t cx, int32_t cy, uint32_t cw, uint32_t ch)
    {
        const int64_t x0 = std::max<int64_t>(x, cx);
        const int64_t y0 = std::max<int64_t>(y, cy);
        const int64_t x1 = std::min<int64_t>(static_cast<int64_t>(x) + w,
                                             static_cast<int64_t>(cx) + cw);
        const int64_t y1 = std::min<int64_t>(static_cast<int64_t>(y) + h,
                                             static_cast<int64_t>(cy) + ch);
        if (x1 <= x0 || y1 <= y0) { w = 0; h = 0; return; }
        x = static_cast<int32_t>(x0);
        y = static_cast<int32_t>(y0);
        w = static_cast<uint32_t>(x1 - x0);
        h = static_cast<uint32_t>(y1 - y0);
    }

    int32_t  m_x = 0;
    int32_t  m_y = 0;
    uint32_t m_w = 0;
    uint32_t m_h = 0;

    // Geometry the layout asked for, not yet applied -- written by the event
    // pump, read by the frame edge. See MoveTo.
    std::mutex m_pending_mtx;
    bool       m_pending   = false;
    int32_t    m_pending_x = 0, m_pending_y = 0;
    uint32_t   m_pending_w = 0, m_pending_h = 0;
    float    m_bg[4] = {0.0f, 0.0f, 0.0f, 0.0f};

    // A default that renders something rather than nothing: eye back along
    // -z, looking at the origin, 60 degrees. A script that forgets LookAt
    // gets a view of its scene instead of a black rectangle it has to debug.
    ViewFrustum m_view{ Point3D{0.0f, 0.0f, -8.0f},
                        Point3D{0.0f, 0.0f,  0.0f},
                        Point3D{0.0f, 1.0f,  0.0f},
                        60.0f * 3.14159265f / 180.0f, 0.1f, 200.0f };

    ETCS::RID m_scene  = 0;

    // Device mode: the request (Camera.h derives the effective mode) and the
    // recording it produces.
    // True by default: a camera uses a device when it has one (Camera.h).
    bool               m_want_device = true;
    std::vector<Op>    m_ops;
    mutable std::mutex m_ops_mtx;
    uint64_t  m_renders = 0;
};

#endif
