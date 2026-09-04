#ifndef COMPOSITEDRAWABLE2D_H__
#define COMPOSITEDRAWABLE2D_H__

#include "../../core_defs.h"
#include "../../ontology.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

// ---------------------------------------------------------------------------
// CompositeDrawable2D — a Drawable2D that OWNS PIXELS, and therefore a place
// where a subtree stops being a thousand draw calls and becomes one blit.
//
// Everything nested under it renders into its buffer instead of into the
// destination. When nothing under it has changed, drawing it is a single
// Blit of an image that already exists. The cost of a frame becomes
// proportional to what CHANGED rather than to what exists, which is the whole
// point and is not an optimisation the tree could have applied by itself:
// only a node that owns pixels can hold the merged result.
//
// A MERGE POINT, NOT A NEW MECHANISM. This is a leaf claiming four families
// that already existed -- Drawable2D, Pixels, Clippable, Deletable -- and it
// is the natural grouping because the tree already says which regions move
// together. A node's children are exactly the things whose coordinates are
// relative to it, so they are exactly the things that can be flattened into
// it without anything outside needing to know. Merging along parental paths
// is not a policy choice; it is the only grouping the coordinate rule allows.
//
// A NODE THAT OWNS PIXELS IS A COORDINATE ORIGIN. That is the one rule the
// rest of this file follows from, and PolygonDrawable2D's own ancestor walk
// now stops here for exactly that reason: a child of a compositor states its
// points in the compositor's space, and the compositor's buffer IS that
// space, so the offset between them is zero. Whatever the compositor is
// nested inside is the compositor's problem, resolved once, when IT is blitted.
//
// THE DIRTY FLAG IS Pixels_'s OWN, and it already had the right shape.
// Writers call MarkDirty, one consumer calls TakeDirty. Here the sequence
// composes rather than collides:
//
//   1. a descendant changes -> it walks up and MarkDirty()s this node
//   2. DrawInto calls TakeDirty(): true, so it recomposes
//   3. recomposing writes pixels, which sets the flag again
//   4. the destination's Blit calls TakeDirty(): true, so it re-uploads
//
// and on a frame where nothing changed, step 2 is false, no children are
// walked at all, and step 4 is false so the device reuses its texture. Two
// consumers of one flag, in sequence, each leaving it in the state the next
// one needs.
//
// WHAT IT IS NOT. Its shape is its rectangle -- a compositor is a buffer, and
// a buffer is rectangular. A non-rectangular merge wants the shape as a mask
// during the final Blit, which Surface_::Blit cannot express today (the same
// gap PolygonDrawable2D::BlitConcrete documents). Clipping children to the
// buffer works and is done; masking the RESULT does not, and pretending
// otherwise would be the one dishonest thing this file could do.
// ---------------------------------------------------------------------------
class CompositeDrawable2D : public Drawable2DBase<CompositeDrawable2D>,
                            public PixelsBase<CompositeDrawable2D>,
                            public ClippableBase<CompositeDrawable2D>,
                            public DeletableBase<CompositeDrawable2D>
{
public:
    WIRE_TYPE_IDENTITY(CompositeDrawable2D);

    // --- Orderable_ (required by Surface, which Drawable refines) ---
    int32_t m_order = 0;
    bool operator<(const CompositeDrawable2D& o) const { return m_order < o.m_order; }
    int32_t Order() override { return m_order; }

    CompositeDrawable2D()  = default;
    ~CompositeDrawable2D() = default;

    bool Create(uint32_t w, uint32_t h)
    {
        if (w == 0 || h == 0)
        {
            ETCS_LOG("CompositeDrawable2D", "Create with a zero dimension (" << w << "x" << h << ").");
            return false;
        }
        m_w = w;
        m_h = h;
        Allocate(w, h);       // idempotent for an unchanged size (Pixels_)
        this->addTag("active");
        return true;
    }

    void SetPosition(int32_t x, int32_t y) { m_x = x; m_y = y; MarkDirty(); }
    void SetOrder(int32_t z)               { m_order = z; Reorder(); MarkDirty(); }

    /*
     * The two family verbs (ontology/Drawable2D.h, ontology/Resizable.h), so
     * a layout solver can place and size this without knowing what it is.
     *
     * ── THEY STAGE. THEY DO NOT APPLY. ───────────────────────────────────
     *
     * THE BUFFER BELONGS TO WHICHEVER THREAD IS COMPOSING, and that is not
     * this one. A resize arrives on the event pump, inside GLFW's
     * framebuffer callback; the frame edge is meanwhile walking this tree in
     * ConsumeFrames and reading these very bytes. The first version of this
     * reallocated here, and the crash is exactly what that predicts:
     *
     *     Pixels_::Composite  <-  recompose  <-  DrawInto  <-  ConsumeFrames
     *
     * segfaulting on a block the pump thread had just freed underneath it.
     * It survives a slow drag, where resize events land between frames, and
     * dies on a burst -- which is what dragging a window ACROSS DISPLAYS
     * produces, as the window manager re-settles the frame two or three
     * times in a few milliseconds. Reproduced 4 runs in 8 by replaying that
     * burst; caught under gdb on the first try.
     *
     * A lock would be the wrong fix. Every pixel operation in the ontology
     * reads this buffer, so guarding it means a reader-writer lock on the
     * hot path of every composite in the system, to serialise against an
     * event that happens when a human drags a corner.
     *
     * So the geometry is STAGED and applied at the top of recompose(), which
     * is already the one place that owns the buffer exclusively -- the same
     * deferral VulkanSurface makes for its Vulkan calls, for the same
     * reason. The layout keeps writing whenever it likes and nothing it
     * writes takes effect in the middle of somebody reading.
     *
     * A LAYOUT PASS ALSO LANDS ATOMICALLY as a side effect, which is a
     * smaller bug fixed for free: Solve calls MoveTo and then ResizeTo, and
     * a frame that fell between them drew the node at its new size in its
     * old place.
     *
     * Both mark the pixel path, because everything above holds a merged copy
     * of a child that is about to move or change shape.
     */
    bool MoveTo(Point2D p) override
    {
        {
            std::lock_guard<std::mutex> g(m_pending_mtx);
            stageFrom();
            m_pending_x = p.x;
            m_pending_y = p.y;
        }
        MarkDirty();
        etcs_mark_pixel_path(this);
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
        MarkDirty();
        etcs_mark_pixel_path(this);
        return true;
    }

    // The colour the buffer is reset to at the start of every recomposition.
    // Transparent by default, which is what a layer wants -- an opaque
    // default would make every compositor a rectangle you cannot see past.
    void SetBackground(float r, float g, float b, float a)
    {
        m_bg[0] = r; m_bg[1] = g; m_bg[2] = b; m_bg[3] = a;
        MarkDirty();
    }

    /*
 * KEEP WHAT IS ALREADY IN THE BUFFER, instead of clearing to the background.
 *
 * A compositor normally owns its pixels outright: it clears, walks its
 * children, and the result is a function of the tree. That is right for a
 * scene and wrong for a CANVAS, where something outside the tree -- a brush
 * stamping through the Surface verbs -- is also a writer, and the buffer is
 * the accumulated picture rather than a derived one. Clearing it every
 * recompose throws away exactly the thing being made; the symptom is a paint
 * program where strokes land and vanish, with nothing in the log to say why.
 *
 * Retained does not mean static. Children still draw on top every pass, so a
 * cursor or a selection rectangle over a retained canvas behaves as it does
 * anywhere else. What changes is only who is assumed to own the pixels
 * underneath them.
 */
    void SetRetain(bool on) { m_retain = on; MarkDirty(); }
    bool Retained() const   { return m_retain; }

    // ── Drawable2D_ dispatch ─────────────────────────────────────────────

    Rect2D BoundsConcrete() override { return Rect2D{ m_x, m_y, m_w, m_h }; }

    // Rectangular, and see the header comment for why that is the honest
    // answer rather than a simplification: the shape of a buffer is the
    // buffer.
    bool ContainsLocalConcrete(int32_t x, int32_t y) override
    {
        return x >= 0 && y >= 0
            && x < static_cast<int32_t>(m_w)
            && y < static_cast<int32_t>(m_h);
    }

    // ── Surface_ dispatch: these RASTERISE, they do not retain ───────────
    //
    // The difference from PolygonDrawable2D is the whole difference between
    // the two leaves. A polygon has nowhere to put pixels, so it remembers
    // what it was asked to draw and replays it into someone else's surface.
    // This one has somewhere to put them, so it puts them there. Children
    // calling DrawRect on their destination are calling THESE.

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

    // Reached as Pixels_, never as a concrete type -- so a child compositor,
    // an ImageSurface, or a provider this module has never heard of all
    // composite into here identically.
    void BlitConcrete(Surface_* source, int32_t x, int32_t y,
                      uint32_t w, uint32_t h, float opacity) override
    {
        if (!source) return;
        Pixels_* px = static_cast<Pixels_*>(
            source->getInterfacePointer(ETCS::Buffer("Pixels")));
        if (!px)
        {
            ETCS_LOG("CompositeDrawable2D", "Blit source RID:" << source->getRID()
                     << " owns no pixels -- a device-side source cannot be read "
                        "back into a CPU composite.");
            return;
        }
        (void)w; (void)h;   // 1:1, as everywhere else on this side -- Pixels_::Composite
        Composite(*px, x, y, opacity);
    }

    // ── Clippable_ dispatch ──────────────────────────────────────────────
    //
    // Nothing device-side to set: this rasteriser reads CurrentClip at draw
    // time instead. The family's arithmetic is what matters here, and it is
    // inherited -- this is only the acknowledgement that a region was set.
    void SetScissorConcrete(int32_t, int32_t, uint32_t, uint32_t) override {}

    // ── Drawable_ dispatch: the merge ────────────────────────────────────
    void DrawIntoConcrete(Surface_* dst) override
    {
        if (!dst) return;

        /*
 * THE ONE BRANCH THIS WHOLE FILE EXISTS FOR.
 *
 * TakeDirty() is false exactly when nothing under this node has changed
 * since the last composition -- so the entire subtree is skipped, not
 * walked and re-emitted, and what reaches the destination is one Blit of
 * an image that is already correct. On a scene where one node moved, only
 * the compositors on the path from that node to the root recompose;
 * everything else is a blit.
 */
        // Two questions, not one. TakeDirty covers every discrete change --
        // a child moved, a colour was set, a node was spawned. Animating
        // covers what a flag structurally cannot: a node that changes DURING
        // the walk, whose mark this frame's own upload then consumes (see
        // ontology/Drawable.h). Asking costs one virtual call per child on a
        // settled tree and is what lets a moving one schedule its own next
        // frame.
        if (TakeDirty() || anyChildAnimating())
            recompose();

        const Point2D base = parentAbsoluteOrigin();
        dst->Blit(this, base.x + m_x, base.y + m_y, m_w, m_h, 1.0f);
    }

    // ── Resizable_ / Deletable_ ──────────────────────────────────────────

    WindowSize GetSizeConcrete() override { return WindowSize{ m_w, m_h }; }

    bool DeleteConcrete() override
    {
        std::string conjugate_key = getSourceModule().toString() + ":" + getSourceTag().toString();
        ETCS_LOG("CompositeDrawable2D", "firing self-DestroyEvent for RID:" << getRID());
        return ETCS::DestroyEvent{conjugate_key.c_str(), this}();
    }

    // How many recompositions this node has actually performed. Not
    // bookkeeping for its own sake: it is the only way to observe from
    // outside that a clean frame skipped the subtree, which is the claim
    // this class makes and the thing a test has to be able to check.
    uint64_t Recompositions() const { return m_recompositions; }

    // A compositor animates when anything under it does -- so an outer
    // compositor asking this one gets the whole subtree's answer, and the
    // recursion terminates at leaves that inherit the family's default of no.
    bool Animating() override { return anyChildAnimating(); }

private:
    bool anyChildAnimating()
    {
        std::vector<Drawable_*> ordered;
        collectDrawableChildren(ordered);
        for (Drawable_* child : ordered)
            if (child->Animating()) return true;
        return false;
    }

    /*
 * Rebuild the buffer from the subtree: reset, clip to our own extent, draw
 * every Drawable child into OURSELVES, unclip.
 *
 * Children receive `this` as their destination, so their DrawRect and Blit
 * land in this buffer, and their coordinate walk stops here (see
 * PolygonDrawable2D::parentAbsoluteOrigin) because a node that owns pixels
 * is a coordinate origin. Nothing in the child knows it is being composited
 * rather than drawn to a window, which is what makes a subtree relocatable
 * between the two.
 *
 * The clip is pushed even though FillRect and Composite already bound
 * themselves to the buffer: it is what makes "a child addresses only its
 * parent's space" ENFORCED rather than merely true of the current
 * implementations, and it is the mechanism a non-rectangular merge would
 * extend rather than replace.
 */
    /*
     * Seed the staged geometry from the live values the first time anything
     * stages, so a MoveTo alone does not carry a stale size along with it.
     * Called under m_pending_mtx.
     */
    void stageFrom()
    {
        if (m_pending) return;
        m_pending   = true;
        m_pending_x = m_x;  m_pending_y = m_y;
        m_pending_w = m_w;  m_pending_h = m_h;
    }

    /*
     * Apply whatever the layout staged. ON THE COMPOSE THREAD, at the top of
     * recompose, before a single byte is read -- see MoveTo above for why
     * that is the only safe moment.
     *
     * The retained case is the one with work in it. A canvas's pixels ARE the
     * picture, so they are put back: background first, then the old bytes
     * into the top-left. Cropping on shrink and fresh background on grow is
     * the only answer that needs no resampling policy, and a paint program
     * that silently resampled on every drag of a window corner would be worse
     * than one that does not.
     *
     * THE BACKGROUND FILL IS NOT COSMETIC. Without it the grown margin sits
     * at alpha 0, the compositor above shows through, and a widened white
     * canvas grows a band of window-chrome grey down its side -- which reads
     * as the layout being wrong rather than the buffer being empty.
     */
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

        std::vector<uint8_t> old;
        const uint32_t ow = m_w;
        // Sized from the BUFFER, not from m_w: they agree today and a copy
        // whose bounds come from a different variable than its bytes is the
        // shape of the next overrun.
        if (m_retain && PixelData())
            old.assign(PixelData(), PixelData() + PixelBytes());
        const uint32_t old_rows = (ow == 0) ? 0
                                : static_cast<uint32_t>(old.size() / (static_cast<size_t>(ow) * 4));

        m_w = nw;
        m_h = nh;
        Allocate(m_w, m_h);

        if (!old.empty())
        {
            ClearTo(m_bg[0], m_bg[1], m_bg[2], m_bg[3]);
            const uint32_t cw = ow < m_w ? ow : m_w;
            const uint32_t ch = old_rows < m_h ? old_rows : m_h;
            for (uint32_t y = 0; y < ch; ++y)
                std::memcpy(PixelData() + static_cast<size_t>(y) * m_w * 4,
                            old.data()  + static_cast<size_t>(y) * ow   * 4,
                            static_cast<size_t>(cw) * 4);
        }
    }

    void recompose()
    {
        applyPendingGeometry();
        ++m_recompositions;
        // Logged because it is the observable form of this class's only
        // claim. A scene that settles logs one of these per compositor and
        // then goes quiet; one that logs every frame is one where something
        // is marking dirty that should not be, and that is a bug you want to
        // see rather than pay for silently.
        ETCS_LOG("CompositeDrawable2D", "recompose #" << m_recompositions
                 << " RID:" << getRID() << " (" << m_w << "x" << m_h << ")");
        // See SetRetain: a canvas's buffer is the picture, not a derived image.
        if (!m_retain) ClearTo(m_bg[0], m_bg[1], m_bg[2], m_bg[3]);

        PushClip(0, 0, m_w, m_h);

        std::vector<Drawable_*> ordered;
        collectDrawableChildren(ordered);
        for (Drawable_* child : ordered) child->DrawInto(this);

        PopClip();
    }

    /*
 * Where this node's PARENT sits, stopping at the first ancestor that owns
 * pixels -- because that ancestor is a coordinate origin, and this node's
 * position is already stated in its space.
 *
 * Identical rule to PolygonDrawable2D's, and it has to be: the two leaves
 * are interchangeable as children, so they must agree on what their
 * coordinates mean.
 */
    Point2D parentAbsoluteOrigin()
    {
        Point2D acc{0, 0};
        for (ETCS::Entity* node = getParent(); node; node = node->getParent())
        {
            void* d2 = node->getInterfacePointer(ETCS::Buffer("Drawable2D"));
            if (!d2) break;
            if (node->getInterfacePointer(ETCS::Buffer("Pixels"))) break;  // origin
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
    // See SetRetain.
    bool m_retain = false;

    // Geometry the layout has asked for, not yet applied. Guarded because
    // the writer is the event pump and the reader is the frame edge; the
    // mutex is taken twice per resize and never during a composite.
    std::mutex m_pending_mtx;
    bool       m_pending   = false;
    int32_t    m_pending_x = 0, m_pending_y = 0;
    uint32_t   m_pending_w = 0, m_pending_h = 0;
    float    m_bg[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    uint64_t m_recompositions = 0;
};

#endif
