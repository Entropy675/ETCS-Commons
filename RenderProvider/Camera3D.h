#ifndef CAMERA3D_H__
#define CAMERA3D_H__

#include "../../core_defs.h"
#include "../../ontology.h"

#include <cmath>
#include <cstdint>

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
//   2. DrawInto calls TakeDirty(): true, so it renders
//   3. Render resolves the scene and asks it to Project into here
//   4. Project writes pixels, which sets the flag again
//   5. the destination's Blit calls TakeDirty(): true, so it re-uploads
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
        this->addTag("active");
        return true;
    }

    void SetPosition(int32_t x, int32_t y) { m_x = x; m_y = y; markPath(); }
    void SetOrder(int32_t z)               { m_order = z; Reorder(); markPath(); }

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
    bool RenderConcrete() override
    {
        if (m_scene == 0) return false;
        Drawable3D_* scene = ETCS::resolve_in_family<Drawable3D_>("Drawable3D", m_scene);
        if (!scene)
        {
            ETCS_LOG("Camera3D", "scene RID:" << m_scene
                     << " no longer resolves -- nothing to render.");
            return false;
        }
        ClearTo(m_bg[0], m_bg[1], m_bg[2], m_bg[3]);
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

    void ClearConcrete(float r, float g, float b, float a) override
    {
        ClearTo(r, g, b, a);
    }

    void DrawRectConcrete(int32_t x, int32_t y, uint32_t w, uint32_t h,
                          float r, float g, float b, float a) override
    {
        int32_t cx, cy; uint32_t cw, ch;
        CurrentClip(cx, cy, cw, ch);
        clipToRegion(x, y, w, h, cx, cy, cw, ch);
        if (w == 0 || h == 0) return;
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

    // ── Drawable_ dispatch: render-if-stale, then one blit ───────────────
    void DrawIntoConcrete(Surface_* dst) override
    {
        if (!dst) return;

        // The branch this class shares with CompositeDrawable2D, plus the one
        // thing a flag cannot express. TakeDirty covers every discrete change
        // -- the eye moved, the scene was rebound, a box was repainted. A
        // scene in MOTION is not a discrete change: it changes during the very
        // walk that draws it, so the mark it leaves is consumed by this
        // frame's own upload and there is nothing left to schedule the next
        // frame with. Asking is what closes that loop, and it costs a load.
        if (TakeDirty() || sceneInMotion())
        {
            Render();
            drawOverlay();
        }

        const Point2D base = parentAbsoluteOrigin();
        dst->Blit(this, base.x + m_x, base.y + m_y, m_w, m_h, 1.0f);
    }

    // A camera animates exactly when the scene it is looking at is moving --
    // the family's own question (ontology/Drawable.h), answered by the node
    // that actually knows. This is what keeps the compositors above it
    // recomposing while a scene coasts, and lets them all go quiet together
    // the moment it settles.
    bool Animating() override { return sceneInMotion(); }

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
        Drawable3D_* scene = ETCS::resolve_in_family<Drawable3D_>("Drawable3D", m_scene);
        if (!scene) return false;
        if (scene->getSourceTag() != ETCS::Buffer("Scene3D")) return false;
        return static_cast<Scene3D*>(scene->getTrueType())->InMotion();
    }

    /*
 * Mark this frame stale, AND every pixel-owning ancestor with it.
 *
 * MarkDirty alone marks the camera, which is not enough and fails silently:
 * a camera nested in a compositor is only redrawn when that compositor
 * recomposes, and a compositor recomposes only when IT is dirty -- so a
 * stale view sits inside a clean parent and is blitted, correctly, forever.
 * Found by moving a scene and watching a perfectly good frame not change.
 *
 * Same walk PolygonDrawable2D::markCompositorsDirty and Scene3D's own
 * markPixelPath make. Three nodes, one rule: whoever holds a merged copy of
 * what changed is out of date, and every pixel owner above you holds one.
 */
    void markPath()
    {
        for (ETCS::Entity* n = this; n; n = n->getParent())
        {
            void* p = n->getInterfacePointer(ETCS::Buffer("Pixels"));
            if (p) static_cast<Pixels_*>(p)->MarkDirty();
        }
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

    // Where this node's PARENT sits, stopping at the first ancestor that owns
    // pixels, because such an ancestor is a coordinate origin. Identical to
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
            if (node->getInterfacePointer(ETCS::Buffer("Pixels"))) break;   // origin
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
    float    m_bg[4] = {0.0f, 0.0f, 0.0f, 0.0f};

    // A default that renders something rather than nothing: eye back along
    // -z, looking at the origin, 60 degrees. A script that forgets LookAt
    // gets a view of its scene instead of a black rectangle it has to debug.
    ViewFrustum m_view{ Point3D{0.0f, 0.0f, -8.0f},
                        Point3D{0.0f, 0.0f,  0.0f},
                        Point3D{0.0f, 1.0f,  0.0f},
                        60.0f * 3.14159265f / 180.0f, 0.1f, 200.0f };

    ETCS::RID m_scene  = 0;
    uint64_t  m_renders = 0;
};

#endif
