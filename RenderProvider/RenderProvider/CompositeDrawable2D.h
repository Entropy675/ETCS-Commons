#ifndef COMPOSITEDRAWABLE2D_H__
#define COMPOSITEDRAWABLE2D_H__

#include "../../../core_defs.h"
#include "../../../ontology.h"

#include <algorithm>
#include <atomic>
#include <chrono>
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
// THE DIRTY EDGE IS Observable_'s, and it is per observer. This node has two
// distinct observers and they used to share one read-and-clear bool:
//
//   1. a descendant changes -> etcs_mark_observed walks to the nearest
//      Observable, which is this node, marking every observer of it
//   2. DrawInto asks TakeObserved(getRID()) -- THIS NODE, watching ITSELF --
//      true, so it recomposes
//   3. recomposing writes pixels, which marks again
//   4. the device asks TakeObserved(its own RID): true, so it re-uploads
//
// and on a settled frame step 2 is false, no children are walked, and step 4
// is false so the device reuses its texture. Same sequence as the old single
// flag, except the two consumers no longer consume each other: with one bool,
// a SECOND device blitting this node found it already taken and went stale
// forever. That is why the self-observation in Create is not ceremony -- it is
// this node's own subscription, and forgetting it means TakeObserved answers
// true for an unregistered observer and the tree recomposes every frame.
//
// WHAT LEAVES IS NOT THE BUFFER. A destination is handed the last PUBLISHED
// frame, a copy taken at an instant somebody declared the raster whole, and the
// two triggers are the two ways that happens: the end of a recompose, and a mark
// whose origin is this node (MarkObserved), which is what a foreign writer into
// these pixels says when it has finished. The raster stays unlocked and the
// reader stops seeing into the middle of a composition. See publish().
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
        // Watch my own subtree: the recompose gate in DrawInto is this
        // subscription being read. See the header comment.
        this->ObserveSelf();
        this->addTag("active");
        return true;
    }

    // Two statements, because a move is two facts: my parent's merged copy is
    // stale (up), and every child's coordinates are stated relative to a frame
    // that just shifted (down). Only the first used to be made.
    void SetPosition(int32_t x, int32_t y)
    { m_x = x; m_y = y; etcs_mark_observed(this); MarkObservedBelow(); }
    void SetOrder(int32_t z)               { m_order = z; Reorder(); etcs_mark_observed(this); }

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
        etcs_mark_observed(this);
        MarkObservedBelow();     // the frame my children sit in moved
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
        etcs_mark_observed(this);
        return true;
    }

    // The colour the buffer is reset to at the start of every recomposition.
    // Transparent by default, which is what a layer wants -- an opaque
    // default would make every compositor a rectangle you cannot see past.
    void SetBackground(float r, float g, float b, float a)
    {
        m_bg[0] = r; m_bg[1] = g; m_bg[2] = b; m_bg[3] = a;
        etcs_mark_observed(this);
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
    void SetRetain(bool on) { m_retain = on; etcs_mark_observed(this); }
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
 * TakeObserved(getRID()) is false exactly when nothing under this node has changed
 * since the last composition -- so the entire subtree is skipped, not
 * walked and re-emitted, and what reaches the destination is one Blit of
 * an image that is already correct. On a scene where one node moved, only
 * the compositors on the path from that node to the root recompose;
 * everything else is a blit.
 */
        // Two questions, not one. The observed bit covers every discrete change --
        // a child moved, a colour was set, a node was spawned. Animating
        // covers what a flag structurally cannot: a node that changes DURING
        // the walk, whose mark this frame's own upload then consumes (see
        // ontology/Drawable.h). Asking costs one virtual call per child on a
        // settled tree and is what lets a moving one schedule its own next
        // frame.
        if (TakeObserved(getRID()) || anyChildNeedsFrame())
        {
            // No self-clear afterwards: recompose's own writes mark with
            // origin=this, so my own edge is skipped at the source. A child that
            // changes DURING the recompose still marks me, and is no longer
            // swallowed by a clear that could not tell the two apart.
            recompose();
        }

        /*
 * THE EDGE TO THIS DESTINATION, which is a SECOND question and the reason a
 * published frame can exist at all.
 *
 *   TakeObserved(getRID())   "did my subtree change"   -> do I recompose
 *   MarkObserved(getRID())   "your raster is finished" -> take the snapshot
 *
 * The second is a mark, not a poll, and it is where the OTHER publish lives
 * (MarkObserved below). It has to be: a foreign writer's sequence of writes is
 * whole only at the instant it says so, and only that writer's own thread knows
 * when that is. A reader that snapshots when it notices a mark can arrive after
 * the next sequence has begun, which measured as bare paper through the ink on a
 * smear -- rarely, and that is the worst kind.
 *
 * Registering the destination is still done here, because this is where the
 * destination becomes known, and it is what makes a device's upload edge work
 * (Observe is idempotent; an unregistered observer is answered true, so a
 * destination's first sight of this node always gets a frame).
 */
        const uint64_t dst_rid = dst->getRID();
        this->Observe(dst_rid);
        (void)TakeObserved(dst_rid);   // spent: the frame below is that answer

        const Point2D base = parentAbsoluteOrigin();

        /*
 * FROM THE PUBLISHED FRAME. The buffer everyone writes is no longer the buffer
 * anyone reads: a destination gets the last WHOLE composition, never the raster
 * with half of one in it.
 *
 * A device destination has no host address to blend into, so it takes the
 * family verb and reads the live raster -- the old behaviour, and the only one
 * available there.
 */
        if (Pixels_* dpx = static_cast<Pixels_*>(
                dst->getInterfacePointer(ETCS::Buffer("Pixels"))))
        {
            std::lock_guard<std::mutex> g(m_front_mtx);
            if (m_front.empty()) publishLocked();   // first sight of this node
            if (!m_front.empty() && m_front_w && m_front_h)
            {
                render_composite_raw(*dpx, m_front.data(), m_front_w, m_front_h,
                                     base.x + m_x, base.y + m_y, 1.0f);
                etcs_mark_observed(dpx);
                return;
            }
        }
        dst->Blit(this, base.x + m_x, base.y + m_y, m_w, m_h, 1.0f);
    }

    /*
     * ── WHERE A FRAME BECOMES PUBLISHABLE ────────────────────────────────
     *
     * A mark whose origin is THIS NODE means somebody just wrote these pixels
     * from outside and has stopped: etcs_mark_observed(x) passes x's own RID,
     * so origin == getRID() is exactly "my raster was written", where a
     * descendant's change arrives with the descendant's RID instead. The
     * distinction is already in the semantics; this reads it.
     *
     * SO THE SNAPSHOT IS TAKEN ON THE WRITER'S THREAD, at the one instant the
     * writer has declared the picture whole, rather than by a reader that
     * noticed afterwards. That is the whole difference between a feed of frames
     * and a feed of mostly-frames: PaintSurface::Render clears, blits every
     * layer and marks, and the next Render can begin immediately -- a reader
     * snapshotting "on the mark" races that next clear and loses sometimes.
     *
     * Not during a recompose: that writes these same pixels through the same
     * Pixels_ path, so every child's draw marks with origin=this and would
     * publish a half-composed tree. recompose publishes once, itself, when it
     * is done.
     */
    void MarkObserved(uint64_t origin_rid) override
    {
        // The edge statement itself is the family's, unchanged -- including the
        // coalescing a batch applies to it (ObservableBase::BeginBatch). All this
        // override adds is the one piece of work THIS class derives from a mark.
        ObservableBase<CompositeDrawable2D>::MarkObserved(origin_rid);

        /*
         * SNAPSHOT WHEN THE SEQUENCE IS OVER, which is what a batch says and a
         * mark does not. Pixels_ marks on every raster op (ontology/Pixels.h), so
         * a writer that clears a view and blits two layers into it marks three
         * times for one picture: copying on each is two copies of a half-built
         * frame, and copying on only the FIRST -- which a naive "once per frame"
         * gate does -- publishes the CLEARED view and loses the picture entirely.
         * That failure is not subtle; it blanked the document.
         *
         * Inside a batch, nothing is published: the sequence is not a picture yet.
         * EndBatch then makes one mark with origin = the batched entity, which
         * arrives here with the raster whole, and that is the copy. A writer that
         * does not batch still gets a copy per mark -- correct, just as costly as
         * it was.
         */
        if (origin_rid == getRID() && !InBatch())
            publish();
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
    bool NeedsFrame() override { return anyChildNeedsFrame(); }

private:
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

    /*
     * ── THE RECOMPOSE LOG, RATE-LIMITED ──────────────────────────────────
     *
     * This line is the observable form of this class's only claim, so it has to
     * keep saying the thing it was written to say: a settled scene logs one per
     * compositor and goes quiet, and one that recomposes every frame is one
     * where something is marking dirty that should not be. That is a bug worth
     * seeing rather than paying for silently.
     *
     * A LINE PER RECOMPOSE DESTROYED IT ANYWAY. Two compositors at frame rate
     * buried every other message in the log, so the one thing the line was for
     * -- noticing that it is happening too often -- became the thing it hid.
     *
     * So the number of recompositions in the window is the message: one line per
     * compositor per second, carrying the count. "recompose x47 in 1.0s" reads as
     * wrong at a glance where 47 consecutive lines read as scrolling, and a
     * settled scene still logs its single one, because a window with one
     * recomposition in it prints the same thing it always did.
     */
    void logRecompose()
    {
        const uint64_t now = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        if (m_log_window_ms == 0) { m_log_window_ms = now; m_log_window_n = 0; }
        ++m_log_window_n;

        // Held until the window closes, so a burst is counted rather than
        // printed. A single recomposition waits out the second and then prints
        // alone, which is what a settling scene should look like.
        if (now - m_log_window_ms < 1000) return;

        const uint64_t first = m_recompositions - m_log_window_n + 1;
        if (m_log_window_n == 1)
            ETCS_LOG("CompositeDrawable2D", "recompose #" << m_recompositions
                     << " RID:" << getRID() << " (" << m_w << "x" << m_h << ")");
        else
            ETCS_LOG("CompositeDrawable2D", "recompose x" << m_log_window_n
                     << " (#" << first << "-#" << m_recompositions << ") in "
                     << (now - m_log_window_ms) << "ms RID:" << getRID()
                     << " (" << m_w << "x" << m_h << ")");
        m_log_window_ms = now;
        m_log_window_n  = 0;
    }

    void recompose()
    {
        // ONE CHANGE, NOT ONE PER CHILD. Every child's draw below writes these
        // pixels through Pixels_ and each of those marks; batched, the walk to the
        // parent and the snapshot both happen once, when the tree is whole.
        // See ObservableBase::BeginBatch.
        etcs_observed_batch batch(this);
        applyPendingGeometry();
        ++m_recompositions;
        logRecompose();
        // See SetRetain: a canvas's buffer is the picture, not a derived image.
        if (!m_retain) ClearTo(m_bg[0], m_bg[1], m_bg[2], m_bg[3]);

        PushClip(0, 0, m_w, m_h);

        std::vector<Drawable_*> ordered;
        collectDrawableChildren(ordered);
        for (Drawable_* child : ordered) child->DrawInto(this);

        PopClip();

        // `batch` closes here: one mark upward for the whole recompose, and the
        // publish that mark triggers, both on this thread.
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
    // See SetRetain.
    bool m_retain = false;

    /*
     * ── THE PUBLISHED FRAME ──────────────────────────────────────────────
     *
     * A compositor's output is a FEED, and a half-assembled frame in it is not
     * a lesser frame -- it is a wrong one. Assembling takes many writes and the
     * reader is another thread; nothing made those two agree about when a frame
     * was whole. Measured on a smear, sampling one pixel inside an inked band
     * once per displayed frame: 15 frames in 494 showed bare paper, the reader
     * having caught the raster after the paper layer landed and before the ink
     * did. Snapshotting on the writer's own mark instead of on a reader's poll
     * took that to 3 in 1535; snapshotting at the end of the writer's BATCH,
     * which is the first form that knows where a sequence ends, took it to 0 in
     * 1223 -- and the sampled value stopped moving at all, 25..25 where it had
     * swung 25..255.
     *
     * A writer that does not batch is still copied per mark, which is correct and
     * no cheaper than it was; what it loses is the guarantee, because nothing
     * else can tell its intermediate states from its finished ones. Two threads
     * assembling one raster also still have to agree between themselves -- a
     * batch coalesces statements, it does not exclude a writer.
     *
     * A COPY, NOT A POINTER FLIP, and retain is what forces that. A retained
     * compositor's buffer IS the picture (SetRetain), so flipping would hand the
     * next frame a stale one to accumulate onto. One pass over memory, once per
     * published frame, against the many passes assembling it already costs.
     *
     * THE ONE LOCK, and it is deliberately not on the raster. Guarding the raster
     * would put a lock on every pixel operation in the system to serialise
     * against a copy; guarding the PUBLISHED frame costs two full-frame passes
     * that were happening anyway, and is sufficient, because nothing writes the
     * published frame except a publish. It never nests with the pixel path.
     */
    void publish()
    {
        std::lock_guard<std::mutex> g(m_front_mtx);
        publishLocked();
    }

    void publishLocked()
    {
        const uint8_t* src = PixelData();
        const size_t   n   = PixelBytes();
        if (!src || n == 0) { m_front.clear(); m_front_w = m_front_h = 0; return; }
        m_front.resize(n);
        std::memcpy(m_front.data(), src, n);
        m_front_w = m_w;
        m_front_h = m_h;
    }

    std::vector<uint8_t>  m_front;
    uint32_t              m_front_w = 0;
    uint32_t              m_front_h = 0;
    std::mutex            m_front_mtx;


    // Geometry the layout has asked for, not yet applied. Guarded because
    // the writer is the event pump and the reader is the frame edge; the
    // mutex is taken twice per resize and never during a composite.
    std::mutex m_pending_mtx;
    bool       m_pending   = false;
    int32_t    m_pending_x = 0, m_pending_y = 0;
    uint32_t   m_pending_w = 0, m_pending_h = 0;
    float    m_bg[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    uint64_t m_recompositions = 0;
    // The log's window, not the counter's -- see logRecompose. Touched only from
    // recompose, which is single-threaded by construction.
    uint64_t m_log_window_ms = 0;
    uint64_t m_log_window_n  = 0;
};

#endif
