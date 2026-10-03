#ifndef PAINTPROVIDER_PAINTCANVASMENU_H__
#define PAINTPROVIDER_PAINTCANVASMENU_H__

#include "../../../core_defs.h"
#include "../../../ontology.h"
#include "PaintColorWheel.h"   // in order: everything above this in the module is visible here

/*
 * ── PaintCanvasMenu ──────────────────────────────────────────────────────
 *
 * THE PENDING STATE OF THE SETTINGS MENU, and nothing else: a width, a height
 * and an anchor that the menu's steppers and cells edit, and that two buttons
 * hand to the document as a resize or a new canvas. Pending rather than live,
 * because a page re-stated on every stepper press would be nine resizes to get
 * from 1024 to 1600, each one clipping and shifting the picture.
 *
 * NO PIXELS AND NO NODES, on the toolbar's bargain (PaintPalette's header
 * note): the menu's look is a script -- PaintProvider/scripts/paint_menu.etcs
 * -- and every control in it is a rectangle the palette maps to a verb here
 * (PaintPalette::AddCall). What this type adds is the part that is about the
 * canvas: what the numbers mean, where they may go, and pushing them back onto
 * the readouts the script bound, as the surface pushes its zoom
 * (PaintSurface::push_zoom_label) -- so a stepper, a script and a Report all
 * leave the same number on screen.
 *
 * Steps of 64 because the page's own sizes are multiples of it and a finer step
 * is thirty presses to a common size; the range is the file reader's.
 */
class PaintCanvasMenu : public DeletableBase<PaintCanvasMenu>
{
public:
    WIRE_TYPE_IDENTITY(PaintCanvasMenu);

    PaintCanvasMenu() = default;
    bool DeleteConcrete() override { return true; }

    static constexpr int32_t STEP_PX = 64;
    static constexpr int32_t MIN_PX  = 64;
    static constexpr int32_t MAX_PX  = 8192;

    // Seeded from the document's extent, so the menu opens showing the page as
    // it is and "resize" with nothing stepped is a no-op rather than a surprise.
    bool Create(ETCS::RID document)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintDocument", document);
        if (!raw) { ETCS_LOG("PaintCanvasMenu", "Create: RID:" << document << " is not a PaintDocument."); return false; }
        m_document = static_cast<PaintDocument*>(raw->getTrueType());
        m_width  = std::clamp(static_cast<int32_t>(m_document->width()),  MIN_PX, MAX_PX);
        m_height = std::clamp(static_cast<int32_t>(m_document->height()), MIN_PX, MAX_PX);
        m_anchor = 4;
        this->addTag("active");
        return true;
    }

    // The view to re-render once the document changes under it -- the same
    // reason the palette and the wheel bind one.
    void BindSurface(ETCS::RID surface)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintSurface", surface);
        if (raw) m_surface = static_cast<PaintSurface*>(raw->getTrueType());
    }

    /*
 * BOTH NUMBERS AT ONCE, which the steppers cannot express. They move in 64s,
 * and 1080 is not a multiple of 64 -- so the one extent most people actually
 * want, 1920x1080, was not reachable from this menu at all however long you
 * held the +. The presets in the script call this; the steppers still do the
 * fine work from wherever it lands.
 */
    void SetExtent(int32_t w, int32_t h)
    {
        m_width  = clamp_px(w);
        m_height = clamp_px(h);
        push_readouts();
    }

    void StepWidth(int32_t delta)  { m_width  = clamp_px(m_width  + delta); push_readouts(); }
    void StepHeight(int32_t delta) { m_height = clamp_px(m_height + delta); push_readouts(); }

    void SetAnchor(int32_t anchor)
    {
        m_anchor = std::clamp(anchor, 0, 8);
        show_anchor();
    }

    void BindWidthReadout(ETCS::RID label)  { m_w_label = label; push_readouts(); }
    void BindHeightReadout(ETCS::RID label) { m_h_label = label; push_readouts(); }

    // The nine cells, by grid index; painted at once so the chosen one shows
    // from the moment the script binds it.
    void BindAnchorCell(int32_t index, ETCS::RID node)
    {
        if (index < 0 || index > 8 || node == 0) return;
        m_cells[index] = node;
        show_anchor();
    }

    /*
 * WHICH WAY THE PICTURE GOES, drawn on the cells it is not anchored to.
 *
 * A bright cell says which part of the page stays put, and that is one fact
 * short of the question anyone actually has: where does everything else move?
 * Every other cell now carries an arrow pointing away from the chosen one,
 * which is the direction the new room appears in -- so the grid reads as a
 * diagram of the resize rather than as nine buttons.
 *
 * ASCII, because the glyph table is ASCII (RenderProvider's TextLabel font is
 * 0x20..0x7E). The two diagonals share a stroke each: "\\" is up-left and
 * down-right, "/" is up-right and down-left, which is what those characters
 * already look like.
 */
    void BindAnchorArrow(int32_t index, ETCS::RID label)
    {
        if (index < 0 || index > 8 || label == 0) return;
        m_arrows[index] = label;
        show_anchor();
    }

    void ApplyResize()
    {
        if (!m_document) { ETCS_LOG("PaintCanvasMenu", "resize: no document -- Create first."); return; }
        if (m_document->Resize(static_cast<uint32_t>(m_width), static_cast<uint32_t>(m_height), m_anchor)
            && m_surface)
            m_surface->Render();
    }

    /*
 * NEW IS A NEW PAGE, not a wiped one. It used to call PaintDocument::New,
 * which clears the layers where they stand -- so the picture that was there
 * was gone, the history gained nothing, and the page list stayed empty however
 * many times it was pressed. Through the store instead: the present is saved
 * to its slot first and a fresh page opens at the size the two steppers show,
 * which is what makes a second page exist to switch back to.
 *
 * Without a store bound -- a native session with no database -- it falls back
 * to the document's own New, which is the same picture minus the history.
 */
    void ApplyNew()
    {
        if (!m_document) { ETCS_LOG("PaintCanvasMenu", "new: no document -- Create first."); return; }
        bool ok = false;
        if (m_pages)
            ok = m_pages->NewAt(static_cast<uint32_t>(m_width), static_cast<uint32_t>(m_height));
        else
            ok = m_document->New(static_cast<uint32_t>(m_width), static_cast<uint32_t>(m_height));
        if (!ok) return;
        refresh_pages();
        if (m_surface) m_surface->Render();
    }

    /*
 * SAVE IS THE STORE'S. With a page store bound, the menu's save puts the
 * present page in its slot (PaintPages::Save, the same sqlite the page list
 * below reads) rather than handing a PNG to the browser -- the header's
 * download button is where a file leaves the page, and a second control that
 * did the same thing under the word "save" read as the store not working.
 * Without a store (a native session with no database) it falls back to the
 * page's download, which is then the only place a picture can go.
 *
 * LOAD IS STILL THE PAGE'S: it takes a file, and a path is something the
 * SUBSTRATE produces -- in the browser the upload control's dialog turns one
 * into a file, and there is no dialog on the desktop side yet. So under
 * emscripten this raises a DOM event the page answers with the same code its
 * header button runs; natively it names the verb to type. Loading a STORED
 * page is a press on its row in the list below.
 */
    void Save()
    {
        if (m_pages) { if (m_pages->Save()) refresh_pages(); return; }
        page_event("save");
    }
    void Load() { page_event("load"); }

    /*
 * A FILE HAS ARRIVED, AND THE PAGE ASKS WHAT IT IS FOR: another layer of the
 * picture (PaintDocument::ImportImage) or a new picture the file's size
 * (PaintDocument::ImportCanvas). The question is a popup on the sheet --
 * PaintProvider/scripts/paint_import.etcs, opened through the palette
 * (PaintPalette::OpenPopup) so it dismisses as every popup does -- and the
 * answer is one of the three verbs below, which the prompt's buttons call
 * (PaintPalette::AddCall). Held here rather than asked by the page's JS,
 * because "a file came in" is the same event on every substrate and the choice
 * belongs with the canvas that will act on it.
 *
 * With no prompt bound (a native session driven from the terminal) the file
 * goes in as a layer at once: an unanswerable question is not a wait.
 */
    /*
 * ── the pages ────────────────────────────────────────────────────────────
 *
 * The store keeps every page and ctrl+PageUp/PageDown walks them
 * (PaintPages); the list you can SEE is PaintPagePanel, on this menu's pane,
 * bound to the same store. What this type keeps of the pages is `new` (above)
 * and the two readouts, which have to follow a load made from the list --
 * PageChanged is how the panel says so, by verb, since it is declared before
 * this type.
 */
    void BindPages(ETCS::RID pages)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintPages", pages);
        if (raw) m_pages = static_cast<PaintPages*>(raw->getTrueType());
    }

    // The page on screen changed under the menu: the steppers show its size.
    void PageChanged()
    {
        if (!m_document) return;
        m_width  = std::clamp(static_cast<int32_t>(m_document->width()),  MIN_PX, MAX_PX);
        m_height = std::clamp(static_cast<int32_t>(m_document->height()), MIN_PX, MAX_PX);
        push_readouts();
        refresh_pages();
    }

    // The reel a multi-frame GIF goes to on arrival (OfferImport).
    void BindAnimation(ETCS::RID anim) { m_anim = anim; }

    // The list on this pane, told to re-read the store after this type
    // changed it (a new page, a save). By verb, for the ordering reason above.
    void BindPagePanel(ETCS::RID panel) { m_page_panel = panel; }
    void refresh_pages()
    {
        if (m_page_panel == 0) return;
        if (ETCS::Entity* e = paint_resolve_tag("PaintPagePanel", m_page_panel))
        {
            ETCS::Buffer act; act.write("PaintPagePanel.Refresh");
            ETCS::Buffer arg;
            try { e->call(act, arg); } catch (...) {}
        }
    }

    /*
 * The tool an import leaves selected. Bound here rather than reached through
 * the palette because what happens after a file arrives is this type's
 * business (answer_import), and the palette's tool pointer is its own.
 */
    void BindTool(ETCS::RID tool)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintTool", tool);
        if (raw) m_tool = static_cast<PaintTool*>(raw->getTrueType());
    }

    void BindImportPrompt(ETCS::RID palette, ETCS::RID pane, ETCS::RID input, ETCS::RID caption)
    {
        m_prompt_palette = palette;
        m_prompt_pane    = pane;
        m_prompt_input   = input;
        m_prompt_caption = caption;
    }

    void OfferImport(const std::string& path)
    {
        /*
     * A GIF WITH MORE THAN ONE FRAME IS A REEL, not a picture, and it goes to
     * the animation without asking: the one thing the file can be for is the
     * one thing it is. A still GIF is a picture and takes the ordinary
     * question below. Decided here rather than by the page, because "a file
     * came in" is the same event on every substrate and this is where the
     * canvas decides what a file is for.
     */
        if (m_anim != 0 && paint_gif_frames_in(path) > 1)
        {
            if (ETCS::Entity* a = paint_resolve_tag("PaintAnimation", m_anim))
                static_cast<PaintAnimation*>(a->getTrueType())->ImportGif(path);
            return;
        }
        m_pending = path;
        if (m_prompt_palette == 0 || m_prompt_pane == 0 || m_prompt_input == 0)
        {
            ETCS_LOG("PaintCanvasMenu", "import " << path << ": no prompt bound -- as a layer.");
            ImportAsLayer();
            return;
        }
        paint_node_text(m_prompt_caption, paint_path_stem(path));
        if (ETCS::Entity* p = paint_resolve_tag("PaintPalette", m_prompt_palette))
            static_cast<PaintPalette*>(p->getTrueType())->OpenPopup(m_prompt_pane, m_prompt_input);
    }

    void ImportAsLayer()  { answer_import(false); }
    void ImportAsCanvas() { answer_import(true); }
    void ImportCancel()
    {
        m_pending.clear();
        close_prompt();
    }

    void Report() const
    {
        ETCS_LOG("PaintCanvasMenu", "pending " << m_width << "x" << m_height
                 << " anchor " << m_anchor << " (" << anchor_name(m_anchor) << ")"
                 << ", document " << (m_document ? std::to_string(m_document->width()) + "x"
                                                   + std::to_string(m_document->height())
                                                 : std::string("unbound")));
    }

    int32_t width()  const { return m_width; }
    int32_t height() const { return m_height; }
    int32_t anchor() const { return m_anchor; }

private:
    static int32_t clamp_px(int32_t v) { return std::clamp(v, MIN_PX, MAX_PX); }

    static const char* anchor_name(int32_t a)
    {
        static const char* names[9] = { "top-left", "top", "top-right", "left", "centre",
                                        "right", "bottom-left", "bottom", "bottom-right" };
        return (a >= 0 && a < 9) ? names[a] : "?";
    }

    // The prompt's answer. Closing first, since the popup's close re-renders
    // the surface and the import's own render should be the last word. The
    // menu's pending extent follows a new canvas so "resize" afterwards starts
    // from the page as it now is.
    void answer_import(bool as_canvas)
    {
        const std::string path = m_pending;
        m_pending.clear();
        close_prompt();
        if (path.empty() || !m_document) return;
        const bool ok = as_canvas ? m_document->ImportCanvas(path) : m_document->ImportImage(path);
        if (ok && as_canvas)
        {
            m_width  = std::clamp(static_cast<int32_t>(m_document->width()),  MIN_PX, MAX_PX);
            m_height = std::clamp(static_cast<int32_t>(m_document->height()), MIN_PX, MAX_PX);
            push_readouts();
        }
        /*
     * THE PICTURE ARRIVES SELECTED, WITH THE TOOL THAT MOVES IT. What anyone
     * does first with an imported image is put it where they want it, and that
     * took three steps nobody was told about: pick select, draw a region around
     * the image, then drag. The import already knows the extent -- the new
     * layer's own raster is the image -- so it states it as the selection and
     * leaves the select tool holding it: press inside and drag, and the first
     * press lifts it (PaintInput's carry).
     *
     * On the layer it just made, which is the active one, so the lift cuts from
     * the image and not from whatever was underneath (SetActiveLayer's note).
     */
        if (ok)
        {
            if (PaintLayer* at = m_document->activeLayer())
                m_document->SelectRect(0, 0,
                    static_cast<int32_t>(at->width()) - 1,
                    static_cast<int32_t>(at->height()) - 1);
            if (m_tool) m_tool->SetKind("select");
        }
        if (ok && m_surface) m_surface->Render();
    }

    void close_prompt()
    {
        if (m_prompt_palette == 0) return;
        if (ETCS::Entity* p = paint_resolve_tag("PaintPalette", m_prompt_palette))
            static_cast<PaintPalette*>(p->getTrueType())->ClosePopup();
    }

    // By verb, since these are somebody else's nodes -- see paint_node_verb.
    void push_readouts()
    {
        paint_node_text(m_w_label, std::to_string(m_width));
        paint_node_text(m_h_label, std::to_string(m_height));
    }

    /*
 * THE CHOSEN CELL IS THE BRIGHT ONE. All nine are written every time rather
 * than the two that changed, because a cell may have been bound since the last
 * change and there is no cheaper way to know. The colours are here rather than
 * in the script because the script's initial fill is overwritten on bind
 * anyway: the palette's control colour for the rest, the page's gold for the
 * one that counts.
 */
    void show_anchor()
    {
        for (int32_t i = 0; i < 9; ++i)
        {
            if (m_cells[i] != 0)
            {
                if (i == m_anchor) paint_node_fill(m_cells[i], 0.79f, 0.71f, 0.35f, 1.0f);
                else               paint_node_fill(m_cells[i], 0.22f, 0.22f, 0.27f, 1.0f);
            }
            if (m_arrows[i] == 0) continue;
            // Away from the anchor, by the sign of the difference in grid
            // coordinates -- so a cell two columns over reads the same as one,
            // which is right: it is a direction, not a distance.
            const int dx = (i % 3) - (m_anchor % 3);
            const int dy = (i / 3) - (m_anchor / 3);
            const char* mark = "";
            if      (dx == 0 && dy == 0) mark = "";
            else if (dx == 0)            mark = (dy < 0) ? "^" : "v";
            else if (dy == 0)            mark = (dx < 0) ? "<" : ">";
            else                         mark = ((dx < 0) == (dy < 0)) ? "\\" : "/";
            paint_node_text(m_arrows[i], mark);
        }
    }

    /*
 * MAIN_THREAD_EM_ASM, not EM_ASM, for the reason etcs_web_shell_write gives:
 * this runs on the router's thread, a Worker with no window. It is proxied to
 * the page and waits for it, so the listener's own synchronous verb calls
 * (ExportImage, from the download path) run on the main thread while this
 * thread is parked, not against it.
 *
 * "load" clicks a file input from a proxied call. A browser opens a file dialog
 * only on transient user activation, and whether the activation of the press
 * that reached the gear's menu -- a canvas mousedown, then a Worker, then this
 * proxy -- is still standing when the page gets here is the browser's call, not
 * this code's. Untested here. The header's upload button is the same code from
 * a real click, and remains the way that always works.
 */
    void page_event(const char* what)
    {
#if defined(__EMSCRIPTEN__)
        MAIN_THREAD_EM_ASM({
            var what = UTF8ToString($0);
            window.dispatchEvent(new CustomEvent('etcs-menu', { detail: what }));
        }, what);
        ETCS_LOG("PaintCanvasMenu", what << " -> the page (etcs-menu event).");
#else
        ETCS_LOG("PaintCanvasMenu", what << ": no file dialog on this substrate. From the terminal: "
                 << (what[0] == 's' ? "doc.ExportImage(<path>)"
                                    : "doc.ImportImage(<path>), then canvas.Render()"));
#endif
    }

    PaintDocument* m_document = nullptr;
    PaintSurface*  m_surface  = nullptr;
    int32_t m_width  = 1024;
    int32_t m_height = 768;
    int32_t m_anchor = 4;
    ETCS::RID m_w_label = 0;
    ETCS::RID m_h_label = 0;
    ETCS::RID m_cells[9]  = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    ETCS::RID m_arrows[9] = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    // The import prompt (BindImportPrompt) and the file it is asking about.
    ETCS::RID   m_prompt_palette = 0;
    ETCS::RID   m_prompt_pane    = 0;
    ETCS::RID   m_prompt_input   = 0;
    ETCS::RID   m_prompt_caption = 0;
    PaintTool*  m_tool = nullptr;
    PaintPages* m_pages = nullptr;
    ETCS::RID   m_page_panel = 0;        // the list on this pane -- see BindPagePanel
    ETCS::RID   m_anim = 0;              // where a multi-frame GIF goes -- see OfferImport
    std::string m_pending;
};

#endif // PAINTPROVIDER_PAINTCANVASMENU_H__
