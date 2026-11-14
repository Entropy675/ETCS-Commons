#ifndef PAINTPROVIDER_PAINTSURFACE_H__
#define PAINTPROVIDER_PAINTSURFACE_H__

#include "../../../core_defs.h"
#include "../../../ontology.h"
#include "PaintDocument.h"   // in order: everything above this in the module is visible here

class PaintSurface : public DeletableBase<PaintSurface>,
                    public AnimatedBase<PaintSurface>
{
public:
    WIRE_TYPE_IDENTITY(PaintSurface);

    /*
 * ── the view follows the pane ────────────────────────────────────────────
 *
 * A compositor's ResizeTo is DEFERRED: it stages the extent and applies it on
 * its next recompose (CompositeDrawable2D::ResizeTo). So the pane is NOT the
 * new size at the moment anything asked it to be, and every caller that
 * resized it and then re-rendered -- the layout's solve, the boot script after
 * it, the page after a window resize -- drew the view for the extent the pane
 * was about to stop having. The picture itself survives that, because it is
 * re-projected from the document on the next render anyway; the ruler does
 * not. Its band is chrome on a RETAINED raster, so a band drawn against the
 * old extent simply stays where it was: a strip of wood lying across the
 * bottom of the page at the height the pane used to end.
 *
 * Waiting a frame is not a thing a script can say, and guessing an interval
 * from the page is not a thing it should have to. So the surface asks instead.
 * AnimatingConcrete is "the pane is not the size I last drew for"; one step later it
 * is, and the answer goes back to false. On a settled view that is one size
 * read per frame, which is the whole bargain the family offers
 * (ontology/Animated.h).
 */
    bool AnimatingConcrete() override
    {
        const WindowSize s = targetSize();
        return s.width != 0 && s.height != 0
            && (s.width != m_drawn_w || s.height != m_drawn_h);
    }

    void AdvanceConcrete(double) override { Render(); }

    PaintSurface() = default;
    bool DeleteConcrete() override { return true; }

    bool Create(ETCS::RID target)
    {
        m_target = target;
        return true;
    }

    void AttachDocument(ETCS::RID document)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintDocument", document);
        if (!raw) return;
        m_document = static_cast<PaintDocument*>(raw->getTrueType());
    }

    void SetTarget(ETCS::RID target) { m_target = target; }

    /*
 * ── the projection ───────────────────────────────────────────────────────
 *
 * THE DOCUMENT IS DRAWN INSIDE THE VIEW, not as the view. Pan is where the
 * document's origin sits in view space; zoom is how many view pixels one
 * document pixel occupies. Everything else -- where a stroke lands, where a
 * ruler's ticks go, what the zoom widget reports -- is these two numbers.
 *
 * KEPT ON THE SURFACE rather than on the document, because it is a property of
 * LOOKING and not of the picture. Two surfaces onto one document are two views
 * of it at different magnifications, which is the arrangement that makes the
 * split obvious; putting pan and zoom on the document would make the second
 * view impossible and would also mean saving a file wrote down where the
 * scrollbars were.
 */
    void SetPan(int32_t x, int32_t y) { m_pan_x = x; m_pan_y = y; ClampPan(); }
    void PanBy(int32_t dx, int32_t dy) { m_pan_x += dx; m_pan_y += dy; ClampPan(); }

    /*
 * ── how far the projection reaches ───────────────────────────────────────
 *
 * THERE IS NO EMPTY SHEET. What lies outside the document is not absence, it is
 * the surface's own layer -- the window is a layer too, and an "empty" one is
 * just a layer nothing has been drawn on. So panning off the edge of the drawing
 * does not take you nowhere; it takes you onto the layer underneath, which is as
 * real a place to stand as the page is.
 *
 * THE PROJECTION EXTENDS PAST THE PAGE BY THE PAGE'S OWN EXTENT, in every
 * direction. That is what makes the pivot target ALWAYS PRESENT: you can put the
 * corner of the page in the middle of the view and turn about it, or work right
 * up to an edge with room beyond it, without the thing you are pivoting about
 * having to be somewhere the drawing happens to reach. A bound tighter than this
 * would make some pivots unreachable; no bound at all -- which is what this had
 * -- lets the page leave the view entirely and leaves nothing to pivot about,
 * which is the same failure from the other end.
 *
 * So the pannable region is the page grown by one page in each direction, and
 * the clamp keeps the VIEW inside it rather than keeping the page inside the
 * view. Those are different rules and only the first one lets an edge sit in the
 * middle of the screen.
 */
    void ClampPan()
    {
        const uint32_t dw = m_document ? m_document->width()  : 0;
        const uint32_t dh = m_document ? m_document->height() : 0;
        if (dw == 0 || dh == 0) return;

        // The view's own size, asked of the surface rather than remembered: it
        // follows the window, and a cached copy would clamp against yesterday's.
        // By the RESIZABLE family, which is where GetSize lives -- a Surface
        // composes Resizable but does not re-declare it, so asking the wrong
        // family for it is a compile error rather than a wrong answer.
        WindowSize vs{ 0, 0 };
        if (Resizable_* v = ETCS::resolve_in_family<Resizable_>("Resizable", m_target))
            vs = v->GetSize();
        if (vs.width == 0 || vs.height == 0) return;

        const float pw = dw * m_zoom;      // the page, in view pixels
        const float ph = dh * m_zoom;

        // Page grown by one page each way: document space [-dw, 2*dw].
        const int32_t max_x = static_cast<int32_t>(pw);
        const int32_t min_x = static_cast<int32_t>(vs.width)  - static_cast<int32_t>(2.0f * pw);
        const int32_t max_y = static_cast<int32_t>(ph);
        const int32_t min_y = static_cast<int32_t>(vs.height) - static_cast<int32_t>(2.0f * ph);

        // A page smaller than the view makes min > max -- every position is
        // inside the region, so the clamp has nothing to say and must not
        // invent an answer by applying the bounds in the wrong order.
        if (min_x <= max_x) m_pan_x = std::clamp(m_pan_x, min_x, max_x);
        if (min_y <= max_y) m_pan_y = std::clamp(m_pan_y, min_y, max_y);
    }

    /*
 * THE READOUT, pushed rather than polled.
 *
 * Whoever changed the zoom is not always whoever can update a label: the wheel
 * gesture belongs to the page, the +/- buttons are resolved inside PaintPalette,
 * and a script may set a zoom outright. If the label were the caller's
 * responsibility each of those three would have to remember, and the one that
 * forgot would leave a number on screen that used to be true.
 *
 * So the surface tells the label, because the surface is the one thing all three
 * go through. By verb name over Entity::call -- the same cross-module seam
 * PaintLayerPanel uses for its row names, and for the same reason: this module
 * cannot and should not know what a TextLabel is.
 */
    void BindZoomLabel(ETCS::RID label) { m_zoom_label = label; m_zoom_label_pushed = -1; push_zoom_label(); }

    // Clamped to something usable at both ends: below 1/16 a document is a
    // speck and the sample step stops resolving it, above 32 one pixel fills a
    // tile and panning gets unusable long before anything breaks.
    void SetZoom(float z)
    {
        m_zoom = std::clamp(z, 0.0625f, 32.0f);
        // The region is measured in view pixels and therefore moves with the
        // zoom: a pan that was inside it at 400% can be outside it at 50%.
        ClampPan();
        push_zoom_label();
    }

    /*
 * ZOOM ABOUT A POINT, which is the only kind a wheel can sensibly do.
 *
 * Zooming about the origin walks whatever you were looking at off the edge, so
 * the point under the pointer is held FIXED: the document coordinate under it
 * is computed before the change and the pan is solved so that the same document
 * coordinate lands back under the same view coordinate afterwards.
 */
    void ZoomAt(float z, int32_t vx, int32_t vy)
    {
        const float before = m_zoom;
        const double dx = (vx - m_pan_x) / static_cast<double>(before);
        const double dy = (vy - m_pan_y) / static_cast<double>(before);
        SetZoom(z);
        m_pan_x = static_cast<int32_t>(std::lround(vx - dx * m_zoom));
        m_pan_y = static_cast<int32_t>(std::lround(vy - dy * m_zoom));
        ClampPan();
    }

    void ZoomBy(float factor, int32_t vx, int32_t vy) { ZoomAt(m_zoom * factor, vx, vy); }

    // The two conversions every caller that owns a pointer needs. Rounded
    // rather than truncated: truncation biases every coordinate toward the
    // origin, which at a zoom below 1 is visible as a stroke that drifts.
    int32_t ViewToDocX(int32_t vx) const
    { return static_cast<int32_t>(std::lround((vx - m_pan_x) / m_zoom)); }
    int32_t ViewToDocY(int32_t vy) const
    { return static_cast<int32_t>(std::lround((vy - m_pan_y) / m_zoom)); }
    int32_t DocToViewX(int32_t dx) const
    { return m_pan_x + static_cast<int32_t>(std::lround(dx * m_zoom)); }
    int32_t DocToViewY(int32_t dy) const
    { return m_pan_y + static_cast<int32_t>(std::lround(dy * m_zoom)); }

    int32_t panX() const { return m_pan_x; }
    int32_t panY() const { return m_pan_y; }
    float   zoom() const { return m_zoom; }
    // What a zoom readout shows. Rounded to a whole percent because that is the
    // precision anybody reads it at.
    int32_t zoomPercent() const { return static_cast<int32_t>(std::lround(m_zoom * 100.0f)); }

    void Render()
    {
        if (!m_document || m_target == 0) return;
        Surface_* view = ETCS::resolve_in_family<Surface_>("Surface", m_target);

        /*
         * ONE CHANGE, AND IT IS THE FINISHED PICTURE.
         *
         * The clear below and every layer after it are writes toward one frame,
         * and the view is somebody else's raster -- a compositor, which copies a
         * frame out whenever it is told the raster changed. Told three times, it
         * copied three times, and one of those copies was the cleared view with no
         * document in it, which is the bare-paper flash on a smear.
         *
         * Batched, the statement is made once, here, when the picture is whole.
         * See ObservableBase::BeginBatch. It is not a lock: it does not stop the
         * frame edge from compositing while this runs.
         */
        etcs_observed_batch frame(view ? static_cast<ETCS::Entity*>(view) : nullptr);
        /*
         * A SECOND BATCH, FOR A SECOND RASTER. The ruler is drawn in the margin
         * OUTSIDE the pane, which is the frame's pixels rather than the pane's
         * (draw_edge_ruler), so its DrawRects mark the frame -- four band clears
         * and a few dozen ticks, each one of them a change on the frame's edges
         * unless they are gathered. Held here rather than inside the ruler so the
         * mark lands after the pane's own picture has been stated too, and so a
         * page with no frame bound holds nothing extra.
         */
        ETCS::Entity* frame_e = (m_ruler_frame != 0 && m_ruler_frame != m_target)
            ? static_cast<ETCS::Entity*>(
                  ETCS::resolve_in_family<Surface_>("Surface", m_ruler_frame))
            : nullptr;
        etcs_observed_batch margin(frame_e);

        // CLEARED FIRST, which a full-view document never needed. A projection
        // does not cover the surface, so without this the area outside the
        // document keeps whatever the last frame left there and panning smears.
        if (view) view->Clear(m_bg[0], m_bg[1], m_bg[2], m_bg[3]);
        // WHAT THIS FRAME IS FOR, recorded before it is drawn -- see AnimatingConcrete.
        {
            const WindowSize ts = targetSize();
            m_drawn_w = ts.width; m_drawn_h = ts.height;
        }
        m_document->RenderToSurface(m_target, m_pan_x, m_pan_y, m_zoom);
        draw_peer_views(view);
        draw_camera(view);
        // The pane's own edge, marked out. After the document because it is chrome
        // rather than part of the picture -- and, with a frame bound, not on the
        // picture's raster at all.
        draw_edge_ruler(view);

        // Redundant while the batches are held -- closing them makes these exact
        // statements -- and kept for the case where they are not: a surface that
        // never claimed Observable is not batched (etcs_observed_batch), and then
        // this is the only mark the picture gets. Marking twice costs one extra
        // edge update; marking zero times loses the frame.
        paint_mark_pixel_path(m_target);
        if (m_ruler_frame != 0 && m_ruler_frame != m_target)
            paint_mark_pixel_path(m_ruler_frame);
    }

    /*
 * ── WHERE EVERYONE ELSE IS LOOKING ───────────────────────────────────────
 *
 * PRESENCE, NOT HISTORY, and that distinction is the whole reason this is a
 * list on the surface rather than an entry in the notebook. A camera position
 * changes on every pan and is worthless a second later: it has no causal
 * successor, so recording it would fill the one structure whose value is that
 * everything in it caused something. It is stored once, overwritten in place,
 * and never replayed.
 *
 * THE RECT IS DERIVED, NOT SENT. What crosses the wire is a document-space
 * rectangle; each page draws it through its OWN projection, so two people at
 * different zooms still see each other's frame in the right place on the
 * picture. Sending view-space pixels would mean a frame that is only correct
 * for the sender, which is the opposite of the point.
 *
 * ONE CALL PER PEER (SetPeer), because a roster of eight at forty bytes each
 * is over the 256-byte call buffer and a file for something this small would
 * be a bridge built for one crossing.
 */
    struct PeerView
    {
        std::string name;
        int32_t x = 0, y = 0, w = 0, h = 0;
        float   rgb[3] = { 0.8f, 0.8f, 0.8f };
    };

    void ClearPeers() { m_peers.clear(); }

    // Upsert by name: a peer that pans twice between two reads should move, not
    // appear twice.
    void SetPeer(const std::string& name, int32_t x, int32_t y, int32_t w, int32_t h,
                 float r, float g, float b)
    {
        if (name.empty()) return;
        for (PeerView& p : m_peers)
            if (p.name == name)
            {
                p.x = x; p.y = y; p.w = w; p.h = h;
                p.rgb[0] = r; p.rgb[1] = g; p.rgb[2] = b;
                return;
            }
        PeerView p;
        p.name = name; p.x = x; p.y = y; p.w = w; p.h = h;
        p.rgb[0] = r; p.rgb[1] = g; p.rgb[2] = b;
        m_peers.push_back(std::move(p));
    }

    size_t peerCount() const { return m_peers.size(); }

    /*
 * THE ANIMATION CAMERA (PaintAnimation's region), drawn here like the frames
 * above: in DOCUMENT space, through this pane's projection, every Render --
 * so it stays on the page under every pan and zoom, as a selection does. It
 * was four panes on the sheet placed from the region, re-placed only by the
 * input's own repaint, and a zoom from the toolbar left the box where the
 * screen had been rather than where the page was.
 */
    void SetCamera(bool on, int32_t x, int32_t y, int32_t w, int32_t h)
    {
        m_cam_on = on; m_cam_x = x; m_cam_y = y; m_cam_w = w; m_cam_h = h;
    }

    // This pane's own visible rectangle IN DOCUMENT SPACE -- what a page sends
    // so everyone else can draw it. Derived from the projection rather than
    // remembered, so it cannot go stale behind a pan.
    void ViewRect(int32_t& x, int32_t& y, int32_t& w, int32_t& h)
    {
        const WindowSize ts = targetSize();
        x = ViewToDocX(0);
        y = ViewToDocY(0);
        w = ViewToDocX(static_cast<int32_t>(ts.width))  - x;
        h = ViewToDocY(static_cast<int32_t>(ts.height)) - y;
    }

private:
    /*
     * AN OUTLINE AND A NAME, not a filled rectangle. A translucent fill over
     * somebody else's frame tints the PICTURE inside it, and the picture is the
     * thing both of you are looking at -- so the one place a presence marker
     * must not be is on top of the work. Four edges and a label at the corner
     * says the same thing and costs the artwork nothing.
     */
    void draw_peer_views(Surface_* view)
    {
        if (!view || m_peers.empty()) return;
        for (const PeerView& p : m_peers)
        {
            const int32_t x0 = DocToViewX(p.x), y0 = DocToViewY(p.y);
            const int32_t x1 = DocToViewX(p.x + p.w), y1 = DocToViewY(p.y + p.h);
            const int32_t vx = std::min(x0, x1), vy = std::min(y0, y1);
            const int32_t vw = std::abs(x1 - x0), vh = std::abs(y1 - y0);
            if (vw <= 1 || vh <= 1) continue;

            const uint32_t uw = static_cast<uint32_t>(vw), uh = static_cast<uint32_t>(vh);
            const float r = p.rgb[0], g = p.rgb[1], b = p.rgb[2];
            view->DrawRect(vx, vy, uw, 2u, r, g, b, 0.85f);
            view->DrawRect(vx, vy + vh - 2, uw, 2u, r, g, b, 0.85f);
            view->DrawRect(vx, vy, 2u, uh, r, g, b, 0.85f);
            view->DrawRect(vx + vw - 2, vy, 2u, uh, r, g, b, 0.85f);

            // The name inside the top-left corner, so it stays with the frame
            // when the frame is half off the pane. Through the same provider the
            // ruler labels use -- a surface has one, and text landing IN the
            // raster rather than beside it as a node is what Glyphs_ is for.
            if (m_glyphs != 0)
                if (ETCS::Held<Glyphs_> gl = ETCS::resolve_held<Glyphs_>("Glyphs", m_glyphs))
                    gl->RasterizeText(m_target, p.name.c_str(), 0, 12,
                                      vx + 4, vy + 4, r, g, b, 0.95f);
        }
    }

    // The page's highlight, as the drag that chose it is previewed in.
    void draw_camera(Surface_* view)
    {
        if (!view || !m_cam_on) return;
        const int32_t x0 = DocToViewX(m_cam_x), y0 = DocToViewY(m_cam_y);
        const int32_t x1 = DocToViewX(m_cam_x + m_cam_w), y1 = DocToViewY(m_cam_y + m_cam_h);
        const int32_t vw = x1 - x0, vh = y1 - y0;
        if (vw < 2 || vh < 2) return;
        const uint32_t uw = static_cast<uint32_t>(vw), uh = static_cast<uint32_t>(vh);
        view->DrawRect(x0, y0, uw, 2u, 0.35f, 0.55f, 0.95f, 1.0f);
        view->DrawRect(x0, y1 - 2, uw, 2u, 0.35f, 0.55f, 0.95f, 1.0f);
        view->DrawRect(x0, y0, 2u, uh, 0.35f, 0.55f, 0.95f, 1.0f);
        view->DrawRect(x1 - 2, y0, 2u, uh, 0.35f, 0.55f, 0.95f, 1.0f);
    }

    std::vector<PeerView> m_peers;
    bool    m_cam_on = false;
    int32_t m_cam_x = 0, m_cam_y = 0, m_cam_w = 0, m_cam_h = 0;

public:
    // WHAT THE SURFACE'S OWN LAYER LOOKS LIKE -- not a "background", which would
    // imply the page is the only real thing and the rest is absence. It is the
    // layer the page sits on, it is always there, and panning onto it is a
    // legitimate place to be (see ClampPan).
    void SetBackground(float r, float g, float b, float a)
    { m_bg[0] = r; m_bg[1] = g; m_bg[2] = b; m_bg[3] = a; }

    // Live stroke stamp onto the bound view surface (not a full layer composite).
    /*
 * Live feedback, and it takes DOCUMENT coordinates like everything else that
 * marks -- so it has to project them itself. The radius scales too: a dab
 * previewed at its document size would be the wrong size on screen at any zoom
 * but 1, which reads as the brush changing when you scroll.
 */
    // Whatever leaf claiming Glyphs labels the edge ruler. Its own rather than
    // the document's: the ruler is a property of the VIEW, and a surface may be
    // asked to draw one with no document attached yet.
    void BindGlyphs(ETCS::RID glyphs) { m_glyphs = glyphs; }

    // Off is a legitimate thing to want -- it is chrome, and a screenshot of the
    // picture should be able to not have it.
    void ShowEdgeRuler(bool on) { m_edge_ruler = on; }

    // THE RASTER THE EDGE RULER IS DRAWN ON: a surface the size of the frame
    // the drawable pane sits inside, at that frame's origin -- the pane's
    // Bounds() are read in the frame's space and used as offsets in this one,
    // so the two spaces must coincide. A node of its own rather than the frame
    // itself, because the frame is a compositor the frame edge rebuilds on its
    // thread while Render writes on the input thread: one buffer with two
    // writers and no order between them, which showed as the band flickering
    // over the toolbar during a drag (boot_paint_panels.etcs, ruler_pane).
    // Bound rather than derived from the tree, because where chrome belongs is
    // a composition choice. Unbound, the marks fall back inside the pane; see
    // draw_edge_ruler.
    void BindRulerFrame(ETCS::RID frame) { m_ruler_frame = frame; }

    // The band's own two colours. Separate from SetBackground because they were
    // the same colour and that was the bug: with the band and the area beyond the
    // page in one shade there was nothing to say where the drawable region ended.
    void SetRulerBackground(float r, float g, float b, float a)
    { m_ruler_bg[0] = r; m_ruler_bg[1] = g; m_ruler_bg[2] = b; m_ruler_bg[3] = a; }
    void SetRulerInk(float r, float g, float b, float a)
    { m_ruler_ink[0] = r; m_ruler_ink[1] = g; m_ruler_ink[2] = b; m_ruler_ink[3] = a; }

    void StampBrush(int32_t x, int32_t y, const PaintBrushState& brush)
    {
        if (m_target == 0) return;
        PaintBrushState scaled = brush;
        scaled.size_px = std::max(1.0f, brush.size_px * m_zoom);
        paint_stamp_surface(m_target, DocToViewX(x), DocToViewY(y), scaled);
    }

    PaintDocument* document() const { return m_document; }
    ETCS::RID target() const { return m_target; }

private:
    /*
 * ── THE DRAWABLE PANE'S EDGE, MARKED EVERY 100 PIXELS ────────────────────
 *
 * TWO THINGS IT IS FOR, and they are both about having a reference at all. The
 * size of the drawing area is otherwise only knowable by measuring the window
 * with something else, and the ruler tool reports distances with nothing on
 * screen to check them against -- a number with no scale beside it.
 *
 * OUTSIDE THE PANE, IN THE MARGIN AROUND IT, when the page gives it one
 * (BindRulerFrame). That is the correction: these used to be drawn INSIDE the
 * pane, along its inner edge, which put them on the picture -- over the paper
 * near the edges, and paintable, so the first stroke near a corner went through
 * the scale you were checking it against. A pane inset inside a frame has a band
 * that belongs to nobody who draws: the router hands presses to the PANE
 * (PaintInput::BindCanvas), so a point in the band is not the picture, and marks
 * there cannot be drawn on. The ticks point OUTWARD from the pane's edge, so the
 * pane's boundary is the zero line for both axes.
 *
 * WITH NO FRAME BOUND they fall back inside the pane, which is what a page with
 * no margin can have; it is not the intended arrangement and the fallback is
 * here so that such a page still gets a scale rather than nothing.
 *
 * IN DOCUMENT PIXELS, WHICH IS THE CORRECTION THAT MATTERS MOST HERE. These used
 * to count the pane, on the reasoning that the ruler describes the drawing AREA --
 * which is a coherent thing to measure and is not what anyone reads a ruler for.
 * At any zoom but 1 it meant the mark labelled 100 was not 100 of anything you
 * could draw, so every number on the edge was wrong. A ruler measures the thing
 * being measured: the marks now sit at round DOCUMENT coordinates projected
 * through the same pan and zoom the picture goes through, so a mark labelled 400
 * is against document x=400 at every zoom, and the scale slides with the paper
 * when you pan. The ruler TOOL and this edge now agree, which they never did.
 *
 * THE SPACING IS CHOSEN, NOT FIXED. Marks every 100 document pixels are 10 px
 * apart at 10% zoom (illegible) and 400 apart at 400% (useless). ruler_step walks
 * a 1-2-5 ladder until one interval is at least RULER_MIN_TICK_PX on screen, which
 * is what keeps the labels readable and the density roughly constant across the
 * whole zoom range.
 *
 * DRAWN, NOT SPAWNED. A script could place these as nodes, and the first resize,
 * pan or zoom would leave them wrong: the spacing, the values and the band's width
 * are all functions of state only the thing being drawn into holds. Four edges, so
 * a mark near a corner is reachable from either side of it.
 */
    // Marks closer together than this are a smear rather than a scale, and their
    // labels overlap. It is what picks the step out of the ladder below.
    static constexpr float RULER_MIN_TICK_PX = 72.0f;

    /*
 * A 1-2-5 LADDER, walked until one interval is far enough apart on screen.
 *
 * Fixed spacing cannot work once the marks are document coordinates: 100 doc px
 * is 10 screen px at 10% zoom and 400 at 400%. Stepping 1, 2, 5, 10, 20, 50, ...
 * keeps every label a round number a person can do arithmetic with -- which
 * stepping by, say, screen-pixels-divided-by-zoom would not.
 */
    // How far out from the pane the margin is cleared each frame, whatever
    // the band measures this frame (draw_edge_ruler's clear says why).
    static constexpr int32_t RULER_CLEAR_MAX = 64;

    static int32_t ruler_step(float zoom)
    {
        const float z = (zoom <= 0.0f) ? 1.0f : zoom;
        int32_t decade = 1;
        for (int guard = 0; guard < 12; ++guard)
        {
            for (int32_t m : { 1, 2, 5 })
                if (m * decade * z >= RULER_MIN_TICK_PX) return m * decade;
            decade *= 10;
        }
        return decade;
    }

    // The first multiple of `s` at or below `v`. Written out because integer
    // division truncates toward zero, so the obvious (v / s) * s steps the wrong
    // way once the pan puts document coordinates negative -- which it does the
    // moment you scroll off the top-left of the page.
    static int32_t floor_multiple(int32_t v, int32_t s)
    {
        if (s <= 0) return v;
        const int32_t q = (v >= 0) ? (v / s) : -(((-v) + s - 1) / s);
        return q * s;
    }

    void draw_edge_ruler(Surface_* pane_view)
    {
        if (!m_edge_ruler) return;

        // The pane's extent, always: it is what the numbers count, whichever
        // raster they end up on.
        WindowSize ps{ 0, 0 };
        if (Resizable_* v = ETCS::resolve_in_family<Resizable_>("Resizable", m_target))
            ps = v->GetSize();
        if (ps.width == 0 || ps.height == 0) return;
        const int32_t pw = static_cast<int32_t>(ps.width);
        const int32_t ph = static_cast<int32_t>(ps.height);

        // The raster to mark, and where the pane sits on it. Defaults are the
        // pane itself at its own origin -- the no-frame fallback above.
        Surface_* dst     = pane_view;
        ETCS::RID dst_rid = m_target;
        int32_t   ox = 0, oy = 0;      // pane origin in dst's space
        int32_t   dw = pw, dh = ph;    // dst's extent
        bool      outside = false;

        if (m_ruler_frame != 0 && m_ruler_frame != m_target)
        {
            Surface_*    frame = ETCS::resolve_in_family<Surface_>("Surface", m_ruler_frame);
            Drawable2D_* pane  = ETCS::resolve_in_family<Drawable2D_>("Drawable2D", m_target);
            WindowSize   fs{ 0, 0 };
            if (Resizable_* fv = ETCS::resolve_in_family<Resizable_>("Resizable", m_ruler_frame))
                fs = fv->GetSize();
            // Bounds(), so the offset is whatever the tree currently says rather
            // than a margin the script also had to tell this object about.
            if (frame && pane && fs.width != 0 && fs.height != 0)
            {
                const Rect2D pr = pane->Bounds();
                dst = frame; dst_rid = m_ruler_frame;
                ox = pr.x;   oy = pr.y;
                dw = static_cast<int32_t>(fs.width);
                dh = static_cast<int32_t>(fs.height);
                outside = true;
            }
        }
        if (!dst) return;

        const int32_t major = 10;    // tick length at a labelled mark
        const int32_t minor = 5;     // and at the halfway one
        const uint32_t label_px = 8;

        // The span of DOCUMENT the pane currently shows, on each axis. These are
        // what the marks are placed against, and they move with pan and zoom.
        const int32_t step = ruler_step(m_zoom);
        const int32_t dx0 = ViewToDocX(0),  dx1 = ViewToDocX(pw);
        const int32_t dy0 = ViewToDocY(0),  dy1 = ViewToDocY(ph);

        // ON THE BAND the marks are ink on wood, so they are the ink colour. In
        // the no-frame fallback they are drawn over the PICTURE instead -- white
        // paper in the middle, the surface's own dark layer around it -- and cream
        // would vanish on the paper, so that path keeps a mid blue that reads on
        // both. Two colours because there are two things to be legible against.
        const float r = outside ? m_ruler_ink[0] : 0.35f;
        const float g = outside ? m_ruler_ink[1] : 0.55f;
        const float b = outside ? m_ruler_ink[2] : 0.95f;
        const float a = outside ? m_ruler_ink[3] : 0.85f;

        ETCS::Held<Glyphs_> glyphs;
        if (m_glyphs != 0) glyphs = ETCS::resolve_held<Glyphs_>("Glyphs", m_glyphs);

        // How wide the band must be to hold a tick and the widest number beside
        // it. MEASURED, not computed from the size: the advance width is the
        // provider's, and a band sized by arithmetic here clips the labels on any
        // provider that disagrees. The widest number is now a document coordinate,
        // so panning far out makes it wider and the band follows.
        int32_t label_w = 0;
        if (glyphs)
        {
            const int32_t biggest = std::max(std::max(std::abs(dx0), std::abs(dx1)),
                                             std::max(std::abs(dy0), std::abs(dy1)));
            const TextExtent e = glyphs->MeasureText(
                std::to_string(((biggest / step) + 1) * step).c_str(), 0, label_px);
            label_w = static_cast<int32_t>(e.width);
        }
        // TWO WIDTHS, because the two axes need different things of the margin.
        // A label along the top or bottom edge is laid beside its tick and needs
        // only its HEIGHT of room; one along the left or right needs its WIDTH,
        // and a four-digit document coordinate is wider than it is tall. Sizing
        // both sides from the wider one gated the top labels out of a margin
        // that had ample room for them.
        const int32_t band_h = major + 4 + static_cast<int32_t>(label_px);
        const int32_t band_v = major + 4 + std::max(label_w, 12);
        const int32_t band   = std::max(band_h, band_v);   // what gets cleared

        // Per side, because a page is free to leave a margin on some edges and
        // not others -- the toolbar takes the bottom of this one's frame. Capped
        // at the band rather than filling the room, so the ruler is the same
        // width everywhere and does not swallow whatever else is out there.
        const int32_t bl = outside ? std::clamp(ox, 0, band) : 0;
        const int32_t bt = outside ? std::clamp(oy, 0, band) : 0;
        const int32_t br = outside ? std::clamp(dw - (ox + pw), 0, band) : 0;
        const int32_t bb = outside ? std::clamp(dh - (oy + ph), 0, band) : 0;

        /*
         * AND THE WHOLE FRAME IS WIPED WHEN THE GEOMETRY MOVES, only then.
         *
         * The per-side clears below cover the band as it is NOW, which was
         * enough while the pane only ever changed EXTENT under a fixed margin:
         * the old marks were inside the new band, so drawing the new one
         * covered them. Once the pane follows the window (the layout, in
         * boot_paint_panels.etcs) it can also GROW, and then the previous
         * band lies inside the new PANE area -- the one region this function
         * must never paint. Nothing clears it and it is chrome on a retained
         * raster stacked over the picture, so it stays: a strip of wood lying
         * across the bottom of the page at the height the pane used to end.
         *
         * ClearTo and not FillRect, because FillRect is source-over and
         * refuses alpha 0 (Pixels_), and transparent is exactly what the
         * pane's own area has to be on this raster.
         */
        if (outside)
        {
            const int32_t geo[6] = { ox, oy, pw, ph, dw, dh };
            if (::std::memcmp(geo, m_ruler_geo, sizeof(geo)) != 0)
            {
                if (Pixels_* rp = ETCS::resolve_in_family<Pixels_>("Pixels", dst_rid))
                    rp->ClearTo(0.0f, 0.0f, 0.0f, 0.0f);
                ::std::memcpy(m_ruler_geo, geo, sizeof(geo));
            }
        }

        // THE BAND IS THE RULER'S TO CLEAR. It is chrome on a raster somebody
        // else retains, so without this a resize leaves the previous extent's
        // marks sitting beside the new ones.
        if (outside)
        {
            // ITS OWN COLOUR, not the surface's background. Those were the same
            // shade, so the band and the empty area beyond the page ran together
            // and there was no telling where the drawable region stopped. Wood,
            // because that is what a ruler is, and because a warm brown is far
            // enough from both the dark layer outside the page and the white of
            // the page itself to be a boundary at a glance.
            //
            // CLEARED TO A FIXED WIDTH, NOT THE BAND'S. The band follows the
            // widest label, so it changes with the zoom -- 36px at 100%, 31px at
            // 125% -- and a clear that shrank with it left the previous width's
            // pixels standing beyond the new edge: the "1" of a "1024x768" drawn
            // at the wider band, sitting in front of the same extent drawn five
            // pixels further in. The margin is the ruler's whatever the band
            // measures, up to a cap so a page with a deep margin (the toolbar's
            // bottom) is not painted over to the frame's edge.
            const float* w = m_ruler_bg;
            const int32_t cl = std::clamp(ox, 0, RULER_CLEAR_MAX);
            const int32_t ct = std::clamp(oy, 0, RULER_CLEAR_MAX);
            const int32_t cr = std::clamp(dw - (ox + pw), 0, RULER_CLEAR_MAX);
            const int32_t cb = std::clamp(dh - (oy + ph), 0, RULER_CLEAR_MAX);
            const uint32_t span = static_cast<uint32_t>(cl + pw + cr);
            if (ct > 0) dst->DrawRect(ox - cl, oy - ct, span, static_cast<uint32_t>(ct),
                                      w[0], w[1], w[2], w[3]);
            if (cb > 0) dst->DrawRect(ox - cl, oy + ph, span, static_cast<uint32_t>(cb),
                                      w[0], w[1], w[2], w[3]);
            if (cl > 0) dst->DrawRect(ox - cl, oy, static_cast<uint32_t>(cl),
                                      static_cast<uint32_t>(ph), w[0], w[1], w[2], w[3]);
            if (cr > 0) dst->DrawRect(ox + pw, oy, static_cast<uint32_t>(cr),
                                      static_cast<uint32_t>(ph), w[0], w[1], w[2], w[3]);
        }

        // THE EDGE, ONE PIXEL OF BLACK ALONG THE PANE'S BOUNDARY, on the band's
        // side of it. The boundary used to be whatever contrast the band's colour
        // happened to have against the paper and against the dark layer outside
        // the page, and against the second of those it had almost none. A line
        // that is black regardless of either colour is a boundary regardless of
        // either colour. Drawn on the band, not the pane: the pane's edge pixels
        // are the picture's, and this is chrome.
        if (outside)
        {
            const float k = 0.0f;
            if (bt > 0) dst->DrawRect(ox - (bl > 0 ? 1 : 0), oy - 1,
                                      static_cast<uint32_t>(pw + (bl > 0) + (br > 0)), 1, k, k, k, 1.0f);
            if (bb > 0) dst->DrawRect(ox - (bl > 0 ? 1 : 0), oy + ph,
                                      static_cast<uint32_t>(pw + (bl > 0) + (br > 0)), 1, k, k, k, 1.0f);
            if (bl > 0) dst->DrawRect(ox - 1, oy, 1, static_cast<uint32_t>(ph), k, k, k, 1.0f);
            if (br > 0) dst->DrawRect(ox + pw, oy, 1, static_cast<uint32_t>(ph), k, k, k, 1.0f);
        }

        // One tick length per side, so a side with a narrow margin gets a short
        // tick instead of one that runs off the frame.
        const int32_t tl = outside ? std::min(major, bl) : major;
        const int32_t tt = outside ? std::min(major, bt) : major;
        const int32_t tr = outside ? std::min(major, br) : major;
        const int32_t tb = outside ? std::min(major, bb) : major;

        // The extent label first, since the top axis has to know where it ends:
        // a tick label that starts under the extent's tail is two numbers in one
        // place (the "150" through the "768"), so a label there is not drawn --
        // the tick still is, and the next label says where the count is.
        // THE DOCUMENT'S extent, not the pane's, because that is the unit the
        // two axes are counting. The pane's size is a fact about the window and
        // is not what anybody reading a scale wants.
        const std::string ext = m_document
            ? std::to_string(m_document->width()) + "x" + std::to_string(m_document->height())
            : std::to_string(pw) + "x" + std::to_string(ph);
        const int32_t ext_x = outside ? ox - bl + 2 : ox + major + 2;
        int32_t ext_end = 0;
        if (glyphs && outside)
            ext_end = ext_x + static_cast<int32_t>(glyphs->MeasureText(ext.c_str(), 0, label_px).width) + 4;

        // ── the horizontal axis, walked in DOCUMENT coordinates ──────────────
        //
        // The loop variable is the number on the label; where it lands is derived
        // from it through the same projection the picture uses, so the mark and
        // the pixel it names cannot drift apart.
        for (int32_t d = floor_multiple(dx0, step); d <= dx1; d += step)
        {
            const int32_t vx = DocToViewX(d);
            if (vx < 0 || vx >= pw) continue;
            const int32_t cx = ox + vx;
            const int32_t hvx = DocToViewX(d + step / 2);
            const bool    half = (step >= 2 && hvx >= 0 && hvx < pw);
            const int32_t hx = ox + hvx;
            if (outside)
            {
                // From one pixel out, so the edge line underneath stays whole.
                if (tt > 1) dst->DrawRect(cx, oy - tt, 1, static_cast<uint32_t>(tt - 1), r, g, b, a);
                if (tb > 1) dst->DrawRect(cx, oy + ph + 1, 1, static_cast<uint32_t>(tb - 1), r, g, b, a);
                if (half && bt >= minor)
                    dst->DrawRect(hx, oy - minor, 1, static_cast<uint32_t>(minor - 1), r, g, b, a);
                if (half && bb >= minor)
                    dst->DrawRect(hx, oy + ph + 1, 1, static_cast<uint32_t>(minor - 1), r, g, b, a);
                // No label at the origin: the corner carries the extent, and a
                // "0" under it is two numbers fighting for twelve pixels. Nor
                // one that would start under the extent's tail (ext_end).
                if (glyphs && d != 0 && bt >= band_h && cx + 3 >= ext_end)
                    glyphs->RasterizeText(dst_rid, std::to_string(d).c_str(), 0, label_px,
                                          cx + 3, oy - band_h + 2, r, g, b, a);
            }
            else
            {
                dst->DrawRect(cx, oy, 1, static_cast<uint32_t>(tt), r, g, b, a);
                dst->DrawRect(cx, oy + ph - tb, 1, static_cast<uint32_t>(tb), r, g, b, a);
                if (half)
                {
                    dst->DrawRect(hx, oy, 1, static_cast<uint32_t>(minor), r, g, b, a);
                    dst->DrawRect(hx, oy + ph - minor, 1, static_cast<uint32_t>(minor), r, g, b, a);
                }
                if (glyphs)
                    glyphs->RasterizeText(dst_rid, std::to_string(d).c_str(), 0, label_px,
                                          cx + 3, oy + major + 2, r, g, b, a);
            }
        }

        // ── the vertical axis, the same walk on the other coordinate ─────────
        for (int32_t d = floor_multiple(dy0, step); d <= dy1; d += step)
        {
            const int32_t vy = DocToViewY(d);
            if (vy < 0 || vy >= ph) continue;
            const int32_t cy = oy + vy;
            const int32_t hvy = DocToViewY(d + step / 2);
            const bool    half = (step >= 2 && hvy >= 0 && hvy < ph);
            const int32_t hy = oy + hvy;
            if (outside)
            {
                if (tl > 1) dst->DrawRect(ox - tl, cy, static_cast<uint32_t>(tl - 1), 1, r, g, b, a);
                if (tr > 1) dst->DrawRect(ox + pw + 1, cy, static_cast<uint32_t>(tr - 1), 1, r, g, b, a);
                if (half && bl >= minor)
                    dst->DrawRect(ox - minor, hy, static_cast<uint32_t>(minor - 1), 1, r, g, b, a);
                if (half && br >= minor)
                    dst->DrawRect(ox + pw + 1, hy, static_cast<uint32_t>(minor - 1), 1, r, g, b, a);
                if (glyphs && d != 0 && bl >= band_v)
                    glyphs->RasterizeText(dst_rid, std::to_string(d).c_str(), 0, label_px,
                                          ox - band_v + 2, cy + 3, r, g, b, a);
            }
            else
            {
                dst->DrawRect(ox, cy, static_cast<uint32_t>(tl), 1, r, g, b, a);
                dst->DrawRect(ox + pw - tr, cy, static_cast<uint32_t>(tr), 1, r, g, b, a);
                if (half)
                {
                    dst->DrawRect(ox, hy, static_cast<uint32_t>(minor), 1, r, g, b, a);
                    dst->DrawRect(ox + pw - minor, hy, static_cast<uint32_t>(minor), 1, r, g, b, a);
                }
                if (glyphs)
                    glyphs->RasterizeText(dst_rid, std::to_string(d).c_str(), 0, label_px,
                                          ox + major + 2, cy + 3, r, g, b, a);
            }
        }

        // The extent itself, where the two labelled axes meet -- the one number
        // somebody reading the edge actually wanted. In the corner of the margin
        // when there is one, so it is outside the picture like the rest.
        if (glyphs)
        {
            if (outside && bt >= band_h && bl > 0)
                glyphs->RasterizeText(dst_rid, ext.c_str(), 0, label_px,
                                      ext_x, oy - band_h + 2, r, g, b, a);
            else if (!outside)
                glyphs->RasterizeText(dst_rid, ext.c_str(), 0, label_px,
                                      ext_x, oy + major + 2, r, g, b, a);
        }
    }

public:

private:
    /*
 * Only when the whole percent actually CHANGED. Zoom moves continuously under a
 * wheel and a label redrawn per event is a cross-module call per event to write
 * the same three characters; the readout is integral, so the compare is exact
 * rather than a tolerance.
 */
    void push_zoom_label()
    {
        if (m_zoom_label == 0) return;
        const int32_t pct = zoomPercent();
        if (pct == m_zoom_label_pushed) return;
        m_zoom_label_pushed = pct;

        paint_node_text(m_zoom_label, std::to_string(pct) + "%");
    }

    PaintDocument* m_document = nullptr;
    ETCS::RID m_target = 0;
    ETCS::RID m_zoom_label = 0;
    int32_t   m_zoom_label_pushed = -1;   // -1 is "never pushed", not a zoom
    // The projection -- see the block above for why it lives here and not on
    // the document.
    int32_t m_pan_x = 0;
    int32_t m_pan_y = 0;
    float   m_zoom  = 1.0f;
    float   m_bg[4] = { 0.13f, 0.13f, 0.15f, 1.0f };
    // The edge ruler: on by default, because a view with no scale on it is the
    // state this was added to fix. See draw_edge_ruler.
    bool      m_edge_ruler = true;
    ETCS::RID m_glyphs     = 0;
    // The surface the ruler marks, when the pane is inset in a larger one.
    ETCS::RID m_ruler_frame = 0;
    // The pane-on-frame geometry the band was last drawn for: ox, oy, pw, ph,
    // dw, dh. -1 so the first draw always wipes. See draw_edge_ruler.
    int32_t m_ruler_geo[6] = { -1, -1, -1, -1, -1, -1 };
    // The pane extent the last frame was drawn for -- see AnimatingConcrete.
    uint32_t m_drawn_w = 0, m_drawn_h = 0;

    WindowSize targetSize()
    {
        if (Resizable_* v = ETCS::resolve_in_family<Resizable_>("Resizable", m_target))
            return v->GetSize();
        return WindowSize{ 0, 0 };
    }
    // THE PAGE'S OWN HEADER COLOUR (#1b1c14, the --panel of index.html), so the
    // margin reads as part of the page's chrome rather than a third surface
    // between it and the paper. Near-black, so the marks are a warm off-white;
    // the boundary to the paper is not left to contrast at all -- see the
    // one-pixel edge in draw_edge_ruler.
    float m_ruler_bg[4]  = { 0.106f, 0.110f, 0.078f, 1.0f };
    float m_ruler_ink[4] = { 0.94f, 0.89f, 0.78f, 0.92f };
};

#endif // PAINTPROVIDER_PAINTSURFACE_H__
