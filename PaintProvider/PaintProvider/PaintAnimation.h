#ifndef PAINTPROVIDER_PAINTANIMATION_H__
#define PAINTPROVIDER_PAINTANIMATION_H__

#include "../../../core_defs.h"
#include "../../../ontology.h"
#include "PaintPagePanel.h"   // in order: everything above this in the module is visible here

/*
 * ── PaintAnimation ───────────────────────────────────────────────────────
 *
 * FRAMES CUT FROM THE PICTURE, AND PUT BACK. A region of the page, chosen by
 * the animation tool's drag (PaintInput), and a sequence of frames the size
 * of that region: `snap` takes a frame from what is visible in the region
 * now, `put` lays the current frame back onto the active layer there. Between
 * those two the page is the drawing board and the frames are the reel -- draw,
 * snap, draw, snap, then scrub or play the reel in the window. A GIF comes in
 * as frames (ImportGif, stb's decoder, the region resized to the file's) and
 * the reel goes out as one (ExportGif, the encoder above).
 *
 * THE WINDOW IS THE LAYER WINDOW'S SHAPE AGAIN: rows of [thumb][#][x] from a
 * row script, a preview raster the current frame is fitted into, and controls
 * on the bar that are palette calls (PaintPalette::AddCall) to the verbs
 * below. This type maps the rows to frames and draws none of it.
 *
 * PLAYING IS ANIMATED, the family (ontology/Animated.h): while playing, the
 * frame edge advances the reel at the rate set, and the preview follows. dt
 * is honoured here, unlike the throbber's, because a rate in frames per
 * second is a real duration and "twelve a second" must mean the same on
 * every display.
 *
 * THE REGION IS A CAMERA ON THE PAGE. It is drawn by the surface in
 * document space (PaintSurface::SetCamera), so it stays where it is on the
 * page as the view pans and zooms. The animation tool pressed inside it
 * carries it (PaintInput, MoveRegionTo) -- same size, so the reel is kept --
 * and while it moves the preview is a viewfinder onto what it frames now.
 * Every frame remembers where on the page it was snapped: `put` lays it back
 * THERE, and choosing a frame takes the camera to it, so a reel shot across
 * the page is a set of places as well as pictures.
 */
class PaintAnimation : public DeletableBase<PaintAnimation>,
                       public AnimatedBase<PaintAnimation>
{
public:
    WIRE_TYPE_IDENTITY(PaintAnimation);

    PaintAnimation() = default;
    bool DeleteConcrete() override { return true; }

    enum class Region : uint8_t { Body, Delete };

    static constexpr int32_t MIN_FPS = 1, MAX_FPS = 60;

    bool Create(ETCS::RID document)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintDocument", document);
        if (!raw) { ETCS_LOG("PaintAnimation", "Create: RID:" << document << " is not a PaintDocument."); return false; }
        m_document = static_cast<PaintDocument*>(raw->getTrueType());
        this->addTag("active");
        return true;
    }

    void BindSurface(ETCS::RID surface)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintSurface", surface);
        if (raw) m_surface = static_cast<PaintSurface*>(raw->getTrueType());
    }

    // The window (shown when a region exists), the preview raster, and the
    // readout label ("12 fps  3/8").
    void BindWindow(ETCS::RID pane)   { m_window = pane; paint_node_hidden(pane, !m_has_region); }
    // The pane this window IS -- what PaintInput raises on a press and keeps
    // on the canvas across a resize. Asked rather than re-bound, so the one
    // BindWindow above stays the only place that says which pane this is.
    ETCS::RID window() const { return m_window; }
    void BindPreview(ETCS::RID node)  { m_preview = node; }
    void BindReadout(ETCS::RID node)  { m_readout = node; }

    /*
 * THE WINDOW MOVES BY ITS BAR, FOLDS BY ITS EYE AND CLOSES BY ITS x.
 * The first two as the layer and sharing windows do (PaintInput routes the
 * press, the motion and the release here): folded, it is its bar
 * (paint_window_fold) and the camera is off the page. Closed, it is gone
 * with the camera, and the tool in hand becomes `move` (the bar lights it,
 * PaintPalette::PickTool) -- the animation tool is what the window belongs
 * to, and one left in hand with nothing showing would still carry an
 * invisible camera. Either way the reel is kept, and the animation tool --
 * a click, or a new drag -- brings the window back.
 */
    void BindPalette(ETCS::RID palette) { m_palette = palette; }
    void BindTitle(ETCS::RID node) { if (node) m_title.push_back(node); }
    void BeginTitle() { m_title_open = true; }
    bool PressTitle(ETCS::RID node, Point2D at)
    {
        if (!m_has_region || m_closed || node == 0
            || std::find(m_title.begin(), m_title.end(), node) == m_title.end()) return false;
        ETCS::Held<Drawable2D_> w = ETCS::resolve_held<Drawable2D_>("Drawable2D", m_window);
        if (!w) return true;
        const Rect2D b = w->Bounds();
        m_moving = true;
        m_grab = at;
        m_origin = Point2D{ b.x, b.y };
        return true;
    }
    bool moving() const { return m_moving; }
    void DragWindow(Point2D at)
    {
        if (m_moving && !paint_node_moved(m_window, m_origin.x + (at.x - m_grab.x), m_origin.y + (at.y - m_grab.y)))
            m_moving = false;
    }
    void EndWindowDrag() { m_moving = false; }

    void Fold(bool folded) { if (m_shown == !folded) return; m_shown = !folded; if (folded) m_playing = false; show(); }
    void Open()  { if (!m_has_region || (m_shown && !m_closed)) return; m_shown = true; m_closed = false; show(); }
    void Close()
    {
        if (m_closed) return;
        m_closed = true;
        m_playing = false;
        show();
        if (ETCS::Entity* pal = paint_resolve_tag("PaintPalette", m_palette))
        {
            ETCS::Buffer act; act.write("PaintPalette.PickTool");
            ETCS::Buffer arg; arg.write("move");
            try { pal->call(act, arg); } catch (...) {}
        }
    }
    bool shown() const { return m_shown && !m_closed; }

    // A press on the bar's eye: fold, or open again.
    bool PressView(ETCS::RID node)
    {
        if (node == 0 || (node != m_view_eye && node != m_view_iris && node != m_view_pupil)) return false;
        Fold(m_shown);
        return true;
    }

    void BeginRow() { m_title_open = false; m_rows.push_back(Row{}); }
    void RowNode(const std::string& what, ETCS::RID node)
    {
        if (node == 0) return;
        // The bar's eye (paint_eye.etcs between BeginTitle and BeginRow).
        if (m_title_open)
        {
            if      (what == "eye")   m_view_eye   = node;
            else if (what == "iris")  m_view_iris  = node;
            else if (what == "pupil") m_view_pupil = node;
            else ETCS_LOG("PaintAnimation", "RowNode: '" << what << "' on the title bar -- only an eye goes there.");
            tint_view();
            return;
        }
        if (m_rows.empty()) { ETCS_LOG("PaintAnimation", "RowNode before BeginRow -- ignored."); return; }
        const size_t idx = m_rows.size() - 1;
        Row& row = m_rows[idx];
        if      (what == "bg")    { row.bg    = node; m_regions[node] = Hit{ idx, Region::Body }; }
        else if (what == "thumb") { row.thumb = node; m_regions[node] = Hit{ idx, Region::Body }; }
        else if (what == "label") { row.label = node; m_regions[node] = Hit{ idx, Region::Body }; }
        else if (what == "del")   { row.del   = node; m_regions[node] = Hit{ idx, Region::Delete }; }
        else ETCS_LOG("PaintAnimation", "RowNode: '" << what << "' is not a part of a row "
                      "(bg thumb label del) -- RID:" << node << " is attached to nothing.");
    }

    void SetRowColors(float sr, float sg, float sb, float ur, float ug, float ub)
    {
        m_row_sel[0] = sr; m_row_sel[1] = sg; m_row_sel[2] = sb;
        m_row_idle[0] = ur; m_row_idle[1] = ug; m_row_idle[2] = ub;
    }

    // ── the region ───────────────────────────────────────────────────────

    /*
 * The region, in document pixels. Normalised and clipped to the page; a
 * region under 2x2 is a click, not a frame, and clears nothing -- the tool's
 * drag has to mean something before it replaces what was there. A change of
 * SIZE drops the frames, because a frame is the region's size by definition
 * and a reel of mixed sizes is not one reel; a move keeps them.
 */
    bool SetRegion(int32_t x0, int32_t y0, int32_t x1, int32_t y1)
    {
        if (!m_document) return false;
        int32_t lx = std::min(x0, x1), rx = std::max(x0, x1);
        int32_t ty = std::min(y0, y1), by = std::max(y0, y1);
        lx = std::clamp(lx, 0, static_cast<int32_t>(m_document->width()));
        rx = std::clamp(rx, 0, static_cast<int32_t>(m_document->width()));
        ty = std::clamp(ty, 0, static_cast<int32_t>(m_document->height()));
        by = std::clamp(by, 0, static_cast<int32_t>(m_document->height()));
        if (rx - lx < 2 || by - ty < 2) return false;
        const uint32_t w = static_cast<uint32_t>(rx - lx), h = static_cast<uint32_t>(by - ty);
        if (m_has_region && (w != m_w || h != m_h) && !m_frames.empty())
        {
            ETCS_LOG("PaintAnimation", "region resized " << m_w << "x" << m_h << " -> " << w << "x" << h
                     << "; " << m_frames.size() << " frame(s) of the old size dropped.");
            m_frames.clear();
            m_where.clear();
            m_at = 0;
        }
        m_x = lx; m_y = ty; m_w = w; m_h = h;
        m_has_region = true;
        m_shown = true;
        m_closed = false;
        m_live = true;
        show();
        ETCS_LOG("PaintAnimation", "region " << m_w << "x" << m_h << " at " << m_x << "," << m_y);
        return true;
    }

    bool hasRegion() const { return m_has_region; }

    // Whether a document point is inside the camera -- where the animation
    // tool's press carries it rather than drawing a new one.
    bool RegionHas(int32_t x, int32_t y) const
    {
        return m_has_region && m_shown && !m_closed && x >= m_x && y >= m_y
            && x < m_x + static_cast<int32_t>(m_w) && y < m_y + static_cast<int32_t>(m_h);
    }
    int32_t regionX() const { return m_x; }
    int32_t regionY() const { return m_y; }

    // The camera to a new corner, the same size, kept on the page. The reel
    // is kept: its frames are this size wherever they were taken.
    void MoveRegionTo(int32_t x, int32_t y)
    {
        if (!m_has_region || !m_document) return;
        m_x = std::clamp(x, 0, std::max(0, static_cast<int32_t>(m_document->width()) - static_cast<int32_t>(m_w)));
        m_y = std::clamp(y, 0, std::max(0, static_cast<int32_t>(m_document->height()) - static_cast<int32_t>(m_h)));
        m_live = true;
        show();
    }

    // ── the reel ─────────────────────────────────────────────────────────

    // A frame from what is visible in the region now, after the current one
    // (so snapping in sequence builds the reel in order), and it becomes the
    // current frame.
    bool Snap()
    {
        if (!m_document || !m_has_region) { ETCS_LOG("PaintAnimation", "snap: no region -- drag one with the animation tool."); return false; }
        std::vector<uint8_t> px;
        if (!m_document->CompositeVisible(px)) return false;
        const uint32_t dw = m_document->width();
        PaintImage f; f.w = m_w; f.h = m_h; f.rgba.resize(static_cast<size_t>(m_w) * m_h * 4);
        for (uint32_t y = 0; y < m_h; ++y)
            std::memcpy(f.rgba.data() + static_cast<size_t>(y) * m_w * 4,
                        px.data() + (static_cast<size_t>(m_y + y) * dw + m_x) * 4,
                        static_cast<size_t>(m_w) * 4);
        const size_t at = m_frames.empty() ? 0 : std::min(m_frames.size(), m_at + 1);
        m_frames.insert(m_frames.begin() + static_cast<std::ptrdiff_t>(at), std::move(f));
        m_where.insert(m_where.begin() + static_cast<std::ptrdiff_t>(at), Point2D{ m_x, m_y });
        m_at = at;
        m_live = false;
        Refresh();
        ETCS_LOG("PaintAnimation", "snapped frame " << (m_at + 1) << " of " << m_frames.size());
        return true;
    }

    // The current frame onto the active layer where it was snapped, as one
    // undoable step (PaintDocument::PastePixels).
    bool Put()
    {
        if (!m_document || m_frames.empty() || !m_has_region) return false;
        const PaintImage& f = m_frames[m_at];
        const Point2D at = m_where[m_at];
        if (!m_document->PastePixels(f.rgba.data(), f.w, f.h, at.x, at.y)) return false;
        if (m_surface) m_surface->Render();
        ETCS_LOG("PaintAnimation", "put frame " << (m_at + 1) << " at " << at.x << "," << at.y);
        return true;
    }

    void Remove(size_t index)
    {
        if (index >= m_frames.size()) return;
        m_frames.erase(m_frames.begin() + static_cast<std::ptrdiff_t>(index));
        m_where.erase(m_where.begin() + static_cast<std::ptrdiff_t>(index));
        if (m_at >= m_frames.size()) m_at = m_frames.empty() ? 0 : m_frames.size() - 1;
        Refresh();
    }

    // Choosing a frame takes the camera to where it was snapped.
    void Select(size_t index) { if (index < m_frames.size()) go_to(index); }
    void Next() { if (!m_frames.empty()) go_to((m_at + 1) % m_frames.size()); }
    void Prev() { if (!m_frames.empty()) go_to((m_at + m_frames.size() - 1) % m_frames.size()); }

    void SetFps(int32_t fps) { m_fps = std::clamp(fps, MIN_FPS, MAX_FPS); Refresh(); }
    void StepFps(int32_t by) { SetFps(m_fps + by); }
    void Play()  { m_playing = !m_frames.empty(); m_clock = 0.0; m_live = false; Refresh(); }
    void Pause() { m_playing = false; Refresh(); }
    void Toggle() { if (m_playing) Pause(); else Play(); }

    /*
 * A GIF's frames become the reel and its size becomes the region's, at the
 * region's corner (or the page's, with no region yet): the file is the
 * authority on its own size. Its delay sets the rate.
 */
    bool ImportGif(const std::string& path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) { ETCS_LOG("PaintAnimation", "import " << path << ": cannot open."); return false; }
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::vector<PaintImage> frames; int delay_ms = 100; std::string why;
        if (!paint_gif::decode(bytes.data(), bytes.size(), frames, delay_ms, why))
        { ETCS_LOG("PaintAnimation", "import " << path << ": " << why); return false; }
        if (!m_document) return false;
        const uint32_t w = frames[0].w, h = frames[0].h;
        const int32_t x = m_has_region ? m_x : 0, y = m_has_region ? m_y : 0;
        if (x + static_cast<int32_t>(w) > static_cast<int32_t>(m_document->width())
            || y + static_cast<int32_t>(h) > static_cast<int32_t>(m_document->height()))
        {
            ETCS_LOG("PaintAnimation", "import " << path << ": " << w << "x" << h << " does not fit the page at "
                     << x << "," << y << " -- resize the page or move the region.");
            return false;
        }
        m_frames = std::move(frames);
        m_where.assign(m_frames.size(), Point2D{ x, y });
        m_at = 0;
        m_x = x; m_y = y; m_w = w; m_h = h; m_has_region = true;
        m_fps = std::clamp(static_cast<int32_t>(std::lround(1000.0 / std::max(1, delay_ms))), MIN_FPS, MAX_FPS);
        m_shown = true;
        m_closed = false;
        m_live = false;
        show();
        ETCS_LOG("PaintAnimation", "imported " << path << ": " << m_frames.size() << " frame(s) " << w << "x" << h
                 << " at " << m_fps << " fps");
        return true;
    }

    // The reel to the user as a file. In the browser that is the page's job
    // -- it reads what ExportGif wrote and hands it to the download -- so this
    // raises the same event the menu's file verbs do (PaintCanvasMenu::
    // page_event) and the page calls ExportGif with a path of its own.
    void Download()
    {
#if defined(__EMSCRIPTEN__)
        MAIN_THREAD_EM_ASM({
            window.dispatchEvent(new CustomEvent('etcs-menu', { detail: 'gif' }));
        });
#else
        ETCS_LOG("PaintAnimation", "download: no file dialog on this substrate. From the terminal: anim.ExportGif(<path>)");
#endif
    }

    bool ExportGif(const std::string& path)
    {
        if (m_frames.empty()) { ETCS_LOG("PaintAnimation", "export " << path << ": no frames."); return false; }
        std::vector<uint8_t> out; std::string why;
        const uint32_t delay_cs = static_cast<uint32_t>(std::max(1, static_cast<int>(std::lround(100.0 / m_fps))));
        if (!paint_gif::encode(m_frames, delay_cs, out, why))
        { ETCS_LOG("PaintAnimation", "export " << path << ": " << why); return false; }
        std::ofstream o(path, std::ios::binary | std::ios::trunc);
        if (!o) { ETCS_LOG("PaintAnimation", "export " << path << ": cannot open for writing."); return false; }
        o.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
        ETCS_LOG("PaintAnimation", "exported " << path << ": " << m_frames.size() << " frame(s) " << m_w << "x" << m_h
                 << ", " << out.size() << " bytes, " << m_fps << " fps");
        return true;
    }

    // ── the rows ─────────────────────────────────────────────────────────

    bool owns(ETCS::RID node) const { return node != 0 && m_regions.find(node) != m_regions.end(); }

    bool Contains(Point2D at) const { return paint_window_contains(m_window, at); }

    void Scroll(int32_t delta)
    {
        const int32_t rows = static_cast<int32_t>(m_rows.size());
        const int32_t total = static_cast<int32_t>(m_frames.size());
        const int32_t most = (total > rows) ? (total - rows) : 0;
        m_scroll = std::clamp(m_scroll + delta, 0, most);
        Refresh();
    }

    bool Apply(ETCS::RID node, bool is_press)
    {
        auto it = m_regions.find(node);
        if (it == m_regions.end()) return false;
        if (!is_press) return true;
        const Hit hit = it->second;
        if (hit.row >= m_rows.size()) return true;
        const size_t index = static_cast<size_t>(m_scroll) + hit.row;
        if (index >= m_frames.size()) return true;
        if (hit.region == Region::Delete) Remove(index);
        else                              Select(index);
        return true;
    }

    /*
 * Everything the window says, restated: the rows against the reel from the
 * scroll offset, the current one highlighted, the preview fitted with the
 * current frame, the readout. After any change rather than on a clock --
 * except while playing, when Advance calls it once per frame step.
 */
    void Refresh()
    {
        {
            const int32_t most = (m_frames.size() > m_rows.size())
                               ? static_cast<int32_t>(m_frames.size() - m_rows.size()) : 0;
            m_scroll = std::clamp(m_scroll, 0, most);
        }
        // Keep the current frame in the window while playing.
        if (m_playing && !m_rows.empty())
        {
            if (m_at < static_cast<size_t>(m_scroll)) m_scroll = static_cast<int32_t>(m_at);
            else if (m_at >= static_cast<size_t>(m_scroll) + m_rows.size())
                m_scroll = static_cast<int32_t>(m_at + 1 - m_rows.size());
        }
        for (size_t i = 0; i < m_rows.size(); ++i)
        {
            Row& row = m_rows[i];
            const size_t at = static_cast<size_t>(m_scroll) + i;
            if (at >= m_frames.size())
            {
                paint_node_fill(row.bg, m_row_idle[0], m_row_idle[1], m_row_idle[2], 0.0f);
                paint_node_hidden(row.thumb, true);
                paint_node_hidden(row.del, true);
                paint_node_text(row.label, "");
                continue;
            }
            const float* c = (at == m_at) ? m_row_sel : m_row_idle;
            paint_node_fill(row.bg, c[0], c[1], c[2], 1.0f);
            paint_node_hidden(row.thumb, false);
            paint_node_hidden(row.del, false);
            paint_fit(row.thumb, m_frames[at]);
            paint_node_text(row.label, std::to_string(at + 1));
        }
        const bool live = m_live && !m_playing;
        if (m_preview)
        {
            if (live)                  paint_fit(m_preview, camera_view());
            else if (m_frames.empty()) paint_fit(m_preview, PaintImage{});
            else                       paint_fit(m_preview, m_frames[m_at]);
        }
        if (m_readout)
        {
            std::string t = std::to_string(m_fps) + " fps  ";
            t += m_frames.empty() ? "no frames" : (std::to_string(m_at + 1) + "/" + std::to_string(m_frames.size()));
            if (m_playing) t += "  playing";
            else if (live) t += "  camera";
            paint_node_text(m_readout, t);
        }
    }

    void Report() const
    {
        ETCS_LOG("PaintAnimation", (m_has_region ? std::to_string(m_w) + "x" + std::to_string(m_h) + " at "
                                    + std::to_string(m_x) + "," + std::to_string(m_y) : std::string("no region"))
                 << ", " << m_frames.size() << " frame(s), at " << (m_frames.empty() ? 0 : m_at + 1)
                 << ", " << m_fps << " fps" << (m_playing ? ", playing" : ""));
    }

    // ── Animated_ ────────────────────────────────────────────────────────

    bool AnimatingConcrete() override { return m_playing && m_frames.size() > 1; }

    // dt honoured: the rate is a duration. Playing does not move the camera:
    // it is the reel being watched, not the frames being chosen.
    void AdvanceConcrete(double dt_ms) override
    {
        if (!m_playing || m_frames.size() < 2) return;
        m_clock += dt_ms;
        const double per = 1000.0 / m_fps;
        bool stepped = false;
        while (m_clock >= per) { m_clock -= per; m_at = (m_at + 1) % m_frames.size(); stepped = true; }
        if (stepped) Refresh();
    }

private:
    struct Row { ETCS::RID bg = 0, thumb = 0, label = 0, del = 0; };
    struct Hit { size_t row; Region region; };

    // The frame fitted into a raster, box-averaged, over the checker; an empty
    // frame is the checker alone.
    static void paint_fit(ETCS::RID node, const PaintImage& f)
    {
        if (node == 0) return;
        Pixels_* dst = ETCS::resolve_in_family<Pixels_>("Pixels", node);
        if (!dst) return;
        uint8_t* out = dst->PixelData();
        if (!out) return;
        const int32_t tw = static_cast<int32_t>(dst->PixelWidth()), th = static_cast<int32_t>(dst->PixelHeight());
        if (tw <= 0 || th <= 0) return;
        const int32_t lw = static_cast<int32_t>(f.w), lh = static_cast<int32_t>(f.h);
        const bool have = lw > 0 && lh > 0 && f.rgba.size() >= static_cast<size_t>(lw) * lh * 4;
        const float scale = have ? std::max(static_cast<float>(lw) / tw, static_cast<float>(lh) / th) : 0.0f;
        const int32_t ox = have ? (tw - static_cast<int32_t>(lw / scale)) / 2 : 0;
        const int32_t oy = have ? (th - static_cast<int32_t>(lh / scale)) / 2 : 0;
        for (int32_t y = 0; y < th; ++y)
            for (int32_t x = 0; x < tw; ++x)
            {
                const bool light = (((x >> 2) + (y >> 2)) & 1) != 0;
                float r = light ? 0.44f : 0.34f, g = r, b = r;
                if (have)
                {
                    const int32_t sx0 = static_cast<int32_t>((x - ox) * scale), sy0 = static_cast<int32_t>((y - oy) * scale);
                    const int32_t sx1 = std::min(static_cast<int32_t>((x - ox + 1) * scale), lw);
                    const int32_t sy1 = std::min(static_cast<int32_t>((y - oy + 1) * scale), lh);
                    if (sx0 >= 0 && sy0 >= 0 && sx0 < lw && sy0 < lh && sx1 > sx0 && sy1 > sy0)
                    {
                        const int32_t stepx = std::max(1, (sx1 - sx0) / 8), stepy = std::max(1, (sy1 - sy0) / 8);
                        float ar = 0, ag = 0, ab = 0, aa = 0; int taps = 0;
                        for (int32_t sy = sy0; sy < sy1; sy += stepy)
                            for (int32_t sx = sx0; sx < sx1; sx += stepx)
                            {
                                const uint8_t* sp = f.rgba.data() + (static_cast<size_t>(sy) * lw + sx) * 4;
                                const float a = sp[3] / 255.0f;
                                ar += (sp[0] / 255.0f) * a; ag += (sp[1] / 255.0f) * a; ab += (sp[2] / 255.0f) * a; aa += a; ++taps;
                            }
                        if (taps > 0 && aa > 0.0f)
                        {
                            const float cover = aa / taps;
                            r = r * (1.0f - cover) + (ar / aa) * cover;
                            g = g * (1.0f - cover) + (ag / aa) * cover;
                            b = b * (1.0f - cover) + (ab / aa) * cover;
                        }
                    }
                }
                uint8_t* dp = out + (static_cast<size_t>(y) * tw + x) * 4;
                dp[0] = paint_to_byte(r); dp[1] = paint_to_byte(g); dp[2] = paint_to_byte(b); dp[3] = 255;
            }
        etcs_mark_observed(dst);
    }

    // The window (whole, or folded to its bar) and the camera (only while
    // open), and the window's contents restated.
    void show()
    {
        const bool up = m_has_region && m_shown && !m_closed;
        paint_node_hidden(m_window, !m_has_region || m_closed);
        paint_window_fold(m_window, !m_shown, paint_nodes_bottom(m_title), m_full_h);
        tint_view();
        if (m_surface)
        {
            m_surface->SetCamera(up, m_x, m_y, static_cast<int32_t>(m_w), static_cast<int32_t>(m_h));
            m_surface->Render();
        }
        Refresh();
    }

    // The layer window's eye colours, so one eye means one thing on the sheet.
    void tint_view()
    {
        static constexpr float white_open[3] = { 0.94f, 0.89f, 0.78f }, white_shut[3] = { 0.35f, 0.36f, 0.28f };
        static constexpr float iris_open[3]  = { 0.35f, 0.55f, 0.95f }, iris_shut[3]  = { 0.106f, 0.110f, 0.078f };
        const float* w = m_shown ? white_open : white_shut;
        const float* i = m_shown ? iris_open  : iris_shut;
        if (m_view_eye)   paint_node_fill(m_view_eye,  w[0], w[1], w[2], 1.0f);
        if (m_view_iris)  paint_node_fill(m_view_iris, i[0], i[1], i[2], 1.0f);
        if (m_view_pupil) paint_node_hidden(m_view_pupil, !m_shown);
    }

    void go_to(size_t index)
    {
        m_at = index;
        m_live = false;
        if (index < m_where.size() && (m_where[index].x != m_x || m_where[index].y != m_y))
        {
            m_x = m_where[index].x; m_y = m_where[index].y;
            show();
            return;
        }
        Refresh();
    }

    // What the camera frames now, from the visible picture -- the viewfinder.
    PaintImage camera_view() const
    {
        PaintImage f;
        std::vector<uint8_t> px;
        if (!m_document || !m_has_region || !m_document->CompositeVisible(px)) return f;
        const uint32_t dw = m_document->width(), dh = m_document->height();
        if (m_x < 0 || m_y < 0 || m_x + m_w > dw || m_y + m_h > dh) return f;
        f.w = m_w; f.h = m_h; f.rgba.resize(static_cast<size_t>(m_w) * m_h * 4);
        for (uint32_t y = 0; y < m_h; ++y)
            std::memcpy(f.rgba.data() + static_cast<size_t>(y) * m_w * 4,
                        px.data() + (static_cast<size_t>(m_y + y) * dw + m_x) * 4,
                        static_cast<size_t>(m_w) * 4);
        return f;
    }

    PaintDocument* m_document = nullptr;
    PaintSurface*  m_surface  = nullptr;
    ETCS::RID m_window = 0, m_preview = 0, m_readout = 0;
    std::vector<ETCS::RID> m_title;
    bool     m_moving = false;
    Point2D  m_grab{ 0, 0 }, m_origin{ 0, 0 };
    bool      m_title_open = false;    // between BeginTitle and the first BeginRow
    ETCS::RID m_view_eye = 0, m_view_iris = 0, m_view_pupil = 0;
    uint32_t  m_full_h = 0;            // the window's height while folded -- paint_window_fold

    bool     m_has_region = false;
    bool     m_shown = true;           // open, not folded to the bar -- see Fold
    bool     m_closed = false;         // gone, window and camera -- see Close
    ETCS::RID m_palette = 0;           // whose tool becomes `move` on Close
    bool     m_live = false;           // the preview is the camera, not a frame
    int32_t  m_x = 0, m_y = 0;
    uint32_t m_w = 0, m_h = 0;

    std::vector<PaintImage> m_frames;
    std::vector<Point2D>    m_where;   // where on the page each frame was snapped
    size_t   m_at = 0;
    int32_t  m_fps = 12;
    bool     m_playing = false;
    double   m_clock = 0.0;

    std::vector<Row> m_rows;
    std::unordered_map<ETCS::RID, Hit> m_regions;
    int32_t  m_scroll = 0;
    float m_row_sel[3]  = { 0.35f, 0.55f, 0.95f };
    float m_row_idle[3] = { 0.15f, 0.16f, 0.11f };
};

#endif // PAINTPROVIDER_PAINTANIMATION_H__
