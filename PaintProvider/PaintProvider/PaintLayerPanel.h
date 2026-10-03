#ifndef PAINTPROVIDER_PAINTLAYERPANEL_H__
#define PAINTPROVIDER_PAINTLAYERPANEL_H__

#include "../../../core_defs.h"
#include "../../../ontology.h"
#include "PaintPalette.h"   // in order: everything above this in the module is visible here

/*
 * ── PaintLayerPanel ────────────────────────────────────────────────────────
 *
 * A LIST OF KEYS INTO THE LAYER ORDER, and nothing else -- the same bargain
 * PaintPalette strikes with the toolbar. It owns no pixels: the rows are
 * ordinary nodes in the caller's 2D tree, laid out by a script, and what this
 * adds is the only part that is about layers -- this node means that row, that
 * region of it means that action.
 *
 * WHY THE ROWS ARE THE SCRIPT'S. Two separate reasons, and only one of them is
 * a limitation. The architectural one is the toolbar's: appearance belongs in a
 * file you can edit without touching C++. The mechanical one is that a module
 * CANNOT spawn another module's type -- make_typed_child reaches
 * EventNode::stream.module_registry, which only the loader's LoaderStream has
 * (CommandExecutor.h) -- so PaintProvider could not create a PolygonDrawable2D
 * if it wanted to. It drives them instead, through Entity::call, which is
 * available to a module precisely because it is not the loader-only overload.
 *
 * WHY A BOUNDED NUMBER OF ROWS IS NOT A COMPROMISE. A window shows the layers
 * NEAR the one you are looking at -- a neighbourhood in the stack's own order
 * (ontology/Layer.h) -- so the row count is the resident set, not a cap on how
 * many layers may exist. Scrolling moves the frame of reference and the same
 * rows are re-bound to different layers. The bytes behind that churn are the
 * arena's business and are already recycled: every ArenaAllocator-backed
 * container, RIDList node storage included, returns exact-size slots to its
 * scope's free list, so same-size reuse is the existing behaviour rather than
 * something this type implements (core/MemoryArena.h).
 *
 * A ROW IS FOUR NODES, because a row is four questions. Background is "select
 * this layer", eye is "show or hide it", label is "what is it called" and
 * doubles as the rename target, delete is "take it out of the document". Any
 * of the four may be 0 in a script that does not want that affordance, and the
 * row still works for the ones it declared.
 */
class PaintLayerPanel : public DeletableBase<PaintLayerPanel>,
                        public AnimatedBase<PaintLayerPanel>
{
public:
    WIRE_TYPE_IDENTITY(PaintLayerPanel);

    PaintLayerPanel() = default;
    bool DeleteConcrete() override { return true; }

    /*
     * Drag is its own region and that is the point of it. A press on the row
     * BODY used to start a restack, and the body is also the second half of the
     * double-click that opens the name -- so on a wide window, where the body is
     * most of the row, a drag and a rename were the same gesture and the text
     * field won. A dotted strip says "hold here", takes the drag, and leaves the
     * body to mean only what it always meant.
     */
    enum class Region : uint8_t
    { Body, Eye, Label, Delete, Title, Add, Drag, MergeUp, MergeDown, View };

    bool Create()
    {
        m_rows.clear();
        m_regions.clear();
        this->addTag("active");
        return true;
    }

    void BindDocument(ETCS::RID document)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintDocument", document);
        if (!raw) return;
        m_document = static_cast<PaintDocument*>(raw->getTrueType());
        Refresh();
    }

    /*
 * Rows are added in the order they are DRAWN -- top of the window first --
 * while the stack reports bottom-first, so Refresh below reverses. Stating it
 * this way round means the script lists rows in the order it lays them out,
 * which is the order somebody reading the script sees them on screen.
 */
    /*
 * FIVE NODES NOW, because a row answers five questions. The thumbnail is the
 * new one and it is not a control: it is a picture of the layer, painted into
 * whatever raster the script put there (see paint_thumb), so a row can be told
 * apart by what is ON it rather than by a name somebody has to have chosen
 * well. Picking it means the same as picking the row body -- it is the layer,
 * so it selects the layer.
 */
    /*
 * ── A ROW IS ASSEMBLED, NOT DECLARED ─────────────────────────────────────
 *
 * BeginRow opens one and RowNode attaches a part to it by name. This replaced
 * AddRow's five positional RIDs plus AddRowExtras' four, and the reason is not
 * tidiness: a script CANNOT hand another script a node it spawned, because
 * `run` carries RIDs in and no names out (resolve_run_bindings). So a row whose
 * parts all had to arrive in one call could only ever be written in one file,
 * which is why the eye's fourteen points were copied once per row -- seven
 * places to change a glyph and six to forget.
 *
 * With a row open, any script holding this panel can contribute a part. The eye
 * is its own file now (paint_eye.etcs) and draws itself into the row's pane;
 * the row script does not need to know what an eye is made of, and nothing
 * needs to know both.
 *
 * AN UNKNOWN PART NAME IS REFUSED LOUDLY. It is the one mistake this shape
 * makes possible that the positional form did not, and a part silently not
 * attached is a control that does nothing for a reason nobody can see.
 */
    void BeginRow()
    {
        m_title_open = false;
        m_rows.push_back(Row{});
    }

    void RowNode(const std::string& what, ETCS::RID node)
    {
        if (node == 0) return;
        /*
     * THE TITLE BAR'S EYE arrives through the same script the rows use
     * (paint_eye.etcs registers by RowNode, since a script cannot hand its
     * nodes back), so between BeginTitle and the first BeginRow the eye and
     * its pupil are the window's view toggle rather than a row's control.
     */
        if (m_title_open)
        {
            if      (what == "eye")   { m_view_eye  = node; BindView(node); }
            else if (what == "iris")  { m_view_iris = node; BindView(node); }
            else if (what == "pupil") { m_view_trim.push_back(node); BindView(node); }
            else ETCS_LOG("PaintLayerPanel", "RowNode: '" << what << "' between BeginTitle and BeginRow -- "
                          "only an eye goes on the title bar; RID:" << node << " is attached to nothing.");
            return;
        }
        if (m_rows.empty()) { ETCS_LOG("PaintLayerPanel", "RowNode before BeginRow -- ignored."); return; }
        const size_t idx = m_rows.size() - 1;
        Row& row = m_rows[idx];

        // The parts that are picked, and what a press on each one means.
        if      (what == "bg")    { row.bg    = node; m_regions[node] = Hit{ idx, Region::Body }; }
        else if (what == "eye")   { row.eye   = node; m_regions[node] = Hit{ idx, Region::Eye }; }
        // THE IRIS AND THE PUPIL ARE THE EYE for a press or a hover. They are
        // drawn OVER the oblong, and the pick takes the topmost node -- so with
        // them listed as decoration only, the middle of every eye was the one
        // place on it that did nothing. The iris is tinted with the eye
        // (Refresh); the pupil keeps the colour the script gave it.
        else if (what == "iris")  { row.iris = node; m_regions[node] = Hit{ idx, Region::Eye }; row.trim.push_back(node); }
        else if (what == "pupil") { m_regions[node] = Hit{ idx, Region::Eye }; row.trim.push_back(node); }
        else if (what == "thumb") { row.thumb = node; m_regions[node] = Hit{ idx, Region::Body }; }
        else if (what == "label") { row.label = node; m_regions[node] = Hit{ idx, Region::Label }; }
        else if (what == "del")   { row.del   = node; m_regions[node] = Hit{ idx, Region::Delete }; }
        else if (what == "grip")  { row.grip  = node; m_regions[node] = Hit{ idx, Region::Drag }; }
        // THE DOTS ARE THE GRIP, for the reason the iris is the eye: they are
        // drawn over it and the pick takes the topmost node, so as decoration
        // they made the middle of the handle -- the part that says "hold here"
        // -- the one place on it a press did not start a drag.
        else if (what == "dots")  { m_regions[node] = Hit{ idx, Region::Drag }; row.trim.push_back(node); }
        else if (what == "up")    { row.mup   = node; m_regions[node] = Hit{ idx, Region::MergeUp }; }
        else if (what == "down")  { row.mdn   = node; m_regions[node] = Hit{ idx, Region::MergeDown }; }
        // DECORATION, by any of the names a row script has for it. Not a
        // control: it is listed so that hiding the row hides it too, which a
        // polygon cannot inherit from a parent it does not have. Any number of
        // them, because a row keeps growing ornaments and each one that has no
        // slot is one that stays on screen over an empty panel.
        else if (what == "trim")
            row.trim.push_back(node);
        else
        {
            ETCS_LOG("PaintLayerPanel", "RowNode: '" << what << "' is not a part of a row "
                     "(bg eye iris pupil thumb label del grip up down, or dots/trim) -- RID:" << node
                     << " is attached to nothing.");
        }
    }

    // Opens the title bar for an eye (see RowNode); the first BeginRow closes it.
    void BeginTitle() { m_title_open = true; }

    // The window's body: the backing plate and anything else that is not the
    // title bar, hidden with the rows when the window is collapsed -- a
    // collapsed window IS its title bar, and a plate left behind is a window
    // that only looks collapsed.
    void BindBody(ETCS::RID node) { if (node) m_body.push_back(node); }

    // The + on the title bar. A control of the WINDOW rather than of a row, so
    // it is bound like the title handle is and carries no row index.
    void BindAdd(ETCS::RID node)
    {
        if (node) m_regions[node] = Hit{ SIZE_MAX, Region::Add };
    }

    /*
 * THE VIEW TOGGLE, beside the +. Collapsed, the window is its title bar and
 * nothing else -- every row, and the rows' own controls, are hidden.
 *
 * NOT A CLOSE, and the distinction is the same one the title comment already
 * makes: a layer window with a cross on it is a window somebody closes by
 * accident and then cannot reopen. This leaves the bar on screen, which is both
 * the thing you press to get the list back and the reminder that there is one.
 */
    void BindView(ETCS::RID node)
    {
        if (node) m_regions[node] = Hit{ SIZE_MAX, Region::View };
    }

    bool collapsed() const { return m_collapsed; }

    /*
 * THE TITLE BAR IS THE HANDLE: a press on it and the window follows the
 * pointer until the release. Any number of nodes may be the handle (the bar
 * and the word on it), and the window they move is the pane the script laid
 * the rows out in -- moved by its own SetPosition (DragWindow says why), so the
 * rows, being placed in the pane's space, come along with nothing recomputed.
 *
 * There is deliberately NO close: a layer window with a cross on it is a
 * window the user will close by accident and then have no way to reopen, and
 * the layers are not optional. Rows can go (Region::Delete); the window stays.
 * (The pane moves by its SetPosition verb -- DragWindow says why not MoveTo.)
 */
    void BindTitle(ETCS::RID handle)
    {
        if (handle) m_regions[handle] = Hit{ SIZE_MAX, Region::Title };
    }
    void BindWindow(ETCS::RID pane) { m_window = pane; }
    ETCS::RID window() const { return m_window; }   // see PaintAnimation::window

    /*
 * THE VIEW TO PUT BACK, because half of what this window does changes the
 * PICTURE and not just the list. Restacking a layer, hiding one, deleting one
 * or adding one all re-order or re-veil what the composite draws -- and a
 * press on a row returns from the input edge before any tool runs, so nothing
 * else was asking the surface to render. The rows updated and the canvas kept
 * showing the arrangement from before the press until something unrelated
 * happened to repaint it.
 *
 * Bound rather than reached for, the same way the palette and the wheel bind
 * one: a panel does not know what is showing the document, and there may be
 * more than one thing.
 */
    void BindSurface(ETCS::RID surface)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintSurface", surface);
        if (raw) m_surface = static_cast<PaintSurface*>(raw->getTrueType());
    }

    void SetHoverDim(float dim) { m_hover_dim = std::clamp(dim, 0.0f, 1.0f); }

    // Eye fills for the two states, and the row tint for selected / not. Given
    // by the script for the same reason the swatch colours are: this is a look.
    void SetEyeColors(float vr, float vg, float vb, float hr, float hg, float hb)
    {
        m_eye_shown[0] = vr; m_eye_shown[1] = vg; m_eye_shown[2] = vb;
        m_eye_hidden[0] = hr; m_eye_hidden[1] = hg; m_eye_hidden[2] = hb;
    }

    // The iris, open and shut. Open is the page's highlight so an eye that is
    // looking reads as looking; shut sinks into the band, the same way the
    // oblong does, so a closed eye is one dark shape rather than a bright dot
    // on a dim one.
    void SetIrisColors(float vr, float vg, float vb, float hr, float hg, float hb)
    {
        m_iris_open[0] = vr; m_iris_open[1] = vg; m_iris_open[2] = vb;
        m_iris_shut[0] = hr; m_iris_shut[1] = hg; m_iris_shut[2] = hb;
    }

    void SetRowColors(float sr, float sg, float sb, float ur, float ug, float ub)
    {
        m_row_sel[0] = sr; m_row_sel[1] = sg; m_row_sel[2] = sb;
        m_row_idle[0] = ur; m_row_idle[1] = ug; m_row_idle[2] = ub;
    }

    // The colour a row takes while it is being dragged, and its ghost with it.
    void SetDragColor(float r, float g, float b) { m_row_drag[0] = r; m_row_drag[1] = g; m_row_drag[2] = b; }

    /*
 * THE GHOST: a row-sized pane that follows the pointer while a row is dragged,
 * showing the layer being carried (its thumb and name), and the mark: a bar
 * in the gap the layer will land in. Both are the script's nodes, spawned in
 * the window and passed through by the pick (SetPassthrough), so what is under
 * the pointer is still the row it is over. Neither is required -- without them
 * a drag still restacks, it just shows only the colour of the row.
 */
    void BindGhost(ETCS::RID pane) { m_ghost = pane; paint_node_hidden(pane, true); }
    void GhostNode(const std::string& what, ETCS::RID node)
    {
        if      (what == "thumb") m_ghost_thumb = node;
        else if (what == "label") m_ghost_label = node;
        else ETCS_LOG("PaintLayerPanel", "GhostNode: '" << what << "' is not a part of the ghost (thumb label).");
    }
    void BindDropMark(ETCS::RID node) { m_drop_mark = node; paint_node_hidden(node, true); }

    /*
 * SCROLL MOVES THE FRAME OF REFERENCE, not the rows. `delta` is in rows, so a
 * wheel notch is +/-1 and the page layer does not have to know the row height.
 * Clamped to the stack rather than wrapping: a list that scrolls past its end
 * and reappears is a list you cannot find anything in.
 */
    void Scroll(int32_t delta)
    {
        if (!m_document) return;
        std::vector<PaintLayer*> stack;
        m_document->OrderedLayers(stack);
        const int32_t rows  = static_cast<int32_t>(m_rows.size());
        const int32_t total = static_cast<int32_t>(stack.size());
        const int32_t most  = (total > rows) ? (total - rows) : 0;
        m_scroll = std::clamp(m_scroll + delta, 0, most);
        // The rows now mean different layers, so whatever was being isolated is
        // not what is under the pointer any more. Straight to full rather than
        // faded: the reason for the dim is gone, not going.
        m_hovering = 0;
        m_subject  = 0;
        m_dim_target = 1.0f;
        m_dim_now    = 1.0f;
        m_document->IsolateLayer(0, 1.0f);
        Refresh();
    }

    /*
 * BIND THE VISIBLE NEIGHBOURHOOD TO THE ROWS and push each row's appearance
 * out through the nodes' own verbs.
 *
 * Called after anything that changes what the window should say -- a scroll, a
 * reorder, a rename, a delete -- rather than on a clock, because the stack only
 * changes when something changes it and a panel that polls would be redrawing a
 * list nobody touched sixty times a second.
 *
 * TOP ROW IS THE TOP OF THE STACK. OrderedLayers reports bottom-first (the
 * composite order), a layer window reads top-first, so the index walks back
 * from the end. Getting this backwards is the classic layer-panel bug and it is
 * invisible until two layers actually overlap.
 */
    void Refresh()
    {
        if (!m_document) return;
        std::vector<PaintLayer*> stack;
        m_document->OrderedLayers(stack);

        /*
     * COLLAPSED IS A ROW STATE, not a second drawing path. Every row and every
     * control on it goes hidden and the title bar is left; the bar is what
     * re-expands it, so the window can never be lost. Done here rather than in
     * the press because a Refresh from anywhere else -- a page switch, a
     * delete -- must not quietly bring the rows back.
     */
        tint_view_eye();
        if (m_collapsed)
        {
            for (Row& row : m_rows)
            {
                row.layer = 0;
                for (ETCS::RID n : { row.bg, row.eye, row.thumb, row.label,
                                     row.del, row.grip, row.mup, row.mdn })
                    if (n) paint_node_hidden(n, true);
                for (ETCS::RID n : row.trim) paint_node_hidden(n, true);
            }
            for (ETCS::RID n : m_body) paint_node_hidden(n, true);
            return;
        }
        for (ETCS::RID n : m_body) paint_node_hidden(n, false);

        /*
     * THE HOVER DIM CANNOT OUTLIVE THE ROW IT CAME FROM. Hovering a row dims
     * every other layer (IsolateLayer) and only another hover undoes it -- so
     * a row deleted under the pointer, or a scroll that re-binds the rows, or
     * a page switch, left the picture faded with the eye column still saying
     * every layer was visible. Re-asserted here, where the rows are being
     * re-bound anyway: if the layer being isolated is no longer one of them,
     * nobody is, and IsolateLayer(0) is how that is said.
     */
        if (m_hovering != 0)
        {
            bool still_here = false;
            for (auto* l : stack) if (l->getRID() == m_hovering) { still_here = true; break; }
            if (!still_here)
            {
                m_hovering = 0;
                m_subject  = 0;
                m_dim_target = 1.0f;
                m_dim_now    = 1.0f;
                m_document->IsolateLayer(0, 1.0f);
            }
        }

        // The stack may have shrunk under the offset (a delete, a page with
        // fewer layers): clamped here rather than only in Scroll, so a window
        // scrolled to the bottom of a long stack does not show empty rows over
        // a short one.
        const size_t total = stack.size();
        {
            const int32_t most = (total > m_rows.size())
                               ? static_cast<int32_t>(total - m_rows.size()) : 0;
            m_scroll = std::clamp(m_scroll, 0, most);
        }
        for (size_t i = 0; i < m_rows.size(); ++i)
        {
            Row& row = m_rows[i];
            const size_t from_top = i + static_cast<size_t>(m_scroll);
            PaintLayer* layer = (from_top < total)
                              ? stack[total - 1 - from_top] : nullptr;
            row.layer = layer ? layer->getRID() : 0;

            // THE ROW ITSELF COMES BACK FIRST. Collapsing hides the row pane
            // and every part on it; the parts below are unhidden one by one as
            // the row is re-bound, but the pane is their parent and was never
            // unhidden -- so expanding brought back nothing, and the window
            // looked broken until a page switch. An empty slot keeps its pane
            // shown too: it is drawn transparent, not absent.
            paint_node_hidden(row.bg, false);
            paint_node_hidden(row.label, false);
            if (!layer)
            {
                // An empty slot is drawn as nothing rather than hidden: the
                // window is a fixed frame and a gap in it is honest.
                paint_node_fill(row.bg,    m_row_idle[0], m_row_idle[1], m_row_idle[2], 0.0f);
                paint_node_fill(row.eye,   0.0f, 0.0f, 0.0f, 0.0f);
                paint_node_hidden(row.eye, true);
                paint_node_hidden(row.thumb, true);
                paint_node_hidden(row.del, true);
                paint_node_fill(row.del,   0.0f, 0.0f, 0.0f, 0.0f);
                paint_node_hidden(row.grip,  true);
                paint_node_hidden(row.mup,   true);
                paint_node_hidden(row.mdn,   true);
                for (ETCS::RID n : row.trim) paint_node_hidden(n, true);
                paint_node_text(row.label, "");
                continue;
            }

            const bool selected = (m_document->activeLayer() == layer);
            const bool carried  = m_drag_live && row.layer == m_dragging;
            const float* tint = carried ? m_row_drag : (selected ? m_row_sel : m_row_idle);
            paint_node_fill(row.bg, tint[0], tint[1], tint[2], 1.0f);
            // EVERY layer has an eye, including the base one: hiding the paper
            // to see what is under it is exactly what the control is for, and
            // it is the delete that the base refuses, not the eye.
            paint_node_hidden(row.eye, false);
            tint_eye(row.eye, row.iris, layer->visible());
            paint_node_hidden(row.thumb, false);
            paint_thumb(row.thumb, layer);
            // The base layer has no delete: it is the page's ground and the
            // document refuses to remove it (PaintDocument::RemoveLayer), so a
            // button that would only ever be refused is not drawn or picked.
            const bool base = (from_top == total - 1);
            paint_node_hidden(row.del, base);
            paint_node_fill(row.del, 0.75f, 0.28f, 0.30f, base ? 0.0f : 1.0f);
            // The grip is always there -- even the base row can be dragged, it
            // simply has nowhere below to land. The merge arrows are hidden at
            // the ends of the stack for the reason the delete is: an arrow that
            // can only ever be refused is a control that teaches nothing.
            paint_node_hidden(row.grip,  false);
            for (ETCS::RID n : row.trim) paint_node_hidden(n, false);
            paint_node_hidden(row.mup,   from_top == 0);
            paint_node_hidden(row.mdn,   base);
            // The row being typed into shows the BUFFER and a caret, not the
            // name it still has -- otherwise the keys go somewhere invisible
            // and the rename reads as the window ignoring you.
            if (m_editing && m_renaming == row.layer)
                paint_node_text(row.label, (m_edit + "_").c_str());
            else
                paint_node_text(row.label, layer->name().c_str());
        }
    }

    /*
 * A PRESS ON A ROW, interpreted by WHICH PART of the row it landed on.
 *
 * Returns true when the node was one of ours -- which is also the answer to
 * "was this press a panel interaction rather than a stroke", and the only thing
 * the input edge needs from this type. Exactly PaintPalette::Apply's contract,
 * for exactly the same reason.
 *
 * A SECOND PRESS ON THE SAME LABEL IS A RENAME, which is what a double click is
 * once the panel is the only thing that knows a label was just pressed. No
 * timer: the rule is "pressed twice with nothing else in between", which is
 * stricter than a clock and does not need one.
 */
    bool Apply(ETCS::RID node, bool is_press, Point2D at = Point2D{ 0, 0 })
    {
        auto it = m_regions.find(node);
        if (it == m_regions.end()) return false;
        if (!is_press) return true;                // ours, but a release does nothing

        const Hit hit = it->second;
        if (hit.region == Region::Title)
        {
            // The window's origin at the press and the press itself, both in
            // the pane's parent's space -- the same space `at` arrives in.
            ETCS::Held<Drawable2D_> w = ETCS::resolve_held<Drawable2D_>("Drawable2D", m_window);
            if (!w) return true;
            const Rect2D b = w->Bounds();
            m_moving = true;
            m_grab   = at;
            m_origin = Point2D{ b.x, b.y };
            m_renaming = 0;
            return true;
        }
        if (!m_document) return true;
        if (hit.region == Region::Add)
        {
            end_edit(false);
            m_document->NewLayer();
            Refresh();
            repaint();                 // a carry may have landed on the way in
            return true;
        }
        if (hit.region == Region::View)
        {
            end_edit(false);
            m_collapsed = !m_collapsed;
            Refresh();                 // which is where hidden actually happens
            std::vector<ETCS::RID> bar;
            for (const auto& [node, h] : m_regions) if (h.region == Region::Title) bar.push_back(node);
            paint_window_fold(m_window, m_collapsed, paint_nodes_bottom(bar), m_full_h);
            repaint();
            return true;
        }
        Row& row = m_rows[hit.row];
        if (row.layer == 0) return true;           // an empty slot is still ours

        switch (hit.region)
        {
        /*
     * PRESS TO CHOOSE, PRESS AGAIN TO RENAME. The label itself opens the field
     * on its own press, but a TextLabel is only as wide as the word in it -- a
     * layer called "Ink" is a twenty-pixel target in a two-hundred-pixel row,
     * and everywhere else along the name is the body. So the body answers the
     * same gesture: a press on the row that is ALREADY the active one is the
     * second half of a double click, and opens the name. Choosing a different
     * row still just chooses it.
     */
        case Region::Body:
            if (!m_editing && m_document->activeLayer()
                && m_document->activeLayer()->getRID() == row.layer)
            {
                begin_edit(row.layer);
                m_dragging = 0;
                break;
            }
            end_edit(false);
            m_document->SetActiveLayer(row.layer);
            // A press on the body chooses, and does NOT begin a restack any
            // more: the grip does that (Region::Drag). The two shared this
            // gesture and the rename always won it.
            repaint();                 // SetActiveLayer lands a carry in flight
            break;

        /*
     * THE GRIP. Held as a RID so a restack between the press and the drop
     * cannot leave this pointing at a row that now means another layer.
     * Choosing the row too, because reaching for a row's handle and finding
     * you have moved a different layer than the one you then look at is the
     * kind of surprise that makes people stop using a control.
     */
        case Region::Drag:
            end_edit(false);
            m_document->SetActiveLayer(row.layer);
            m_dragging = row.layer;
            m_drag_live = false;
            m_drag_press = window_local(at);
            m_drag_grab_dy = m_drag_press.y - row_rect(hit.row).y;
            repaint();
            break;

        case Region::MergeUp:
        case Region::MergeDown:
            end_edit(false);
            m_document->MergeLayer(row.layer, hit.region == Region::MergeDown ? -1 : 1);
            if (m_dragging == row.layer) m_dragging = 0;
            Refresh();
            repaint();
            break;

        case Region::Eye:
            if (ETCS::Entity* raw = paint_resolve_tag("PaintLayer", row.layer))
                static_cast<PaintLayer*>(raw->getTrueType())->ToggleVisible();
            end_edit(false);
            repaint();                 // the composite has one layer more or less in it
            break;

        /*
     * SECOND PRESS ON THE SAME LABEL OPENS THE NAME FOR TYPING, which is what
     * a double click is once the panel is the only thing that knows a label
     * was just pressed. No timer: the rule is "pressed twice with nothing else
     * in between", which is stricter than a clock and needs no clock.
     *
     * The field is this panel's, not a text box in the picture: the keys are
     * offered here first while it is open (PaintInput::KeyDown -> KeyIn) and
     * the row draws the buffer with a caret, so what is being typed is on the
     * row it will name.
     */
        case Region::Label:
            if (m_renaming == row.layer) break;    // already open, keep typing
            begin_edit(row.layer);
            m_dragging = 0;                        // typing is not a drag
            break;

        case Region::Delete:
            if (m_renaming == row.layer) end_edit(false);
            m_document->RemoveLayer(row.layer);
            if (m_dragging == row.layer) m_dragging = 0;
            repaint();
            break;

        case Region::Title:
        case Region::Add:
        case Region::View:
            // All answered above, before the row lookup -- they are the
            // window's controls, not a row's. Named here so the switch stays
            // total and a new region cannot be added without deciding what it
            // means.
            break;
        }
        Refresh();
        return true;
    }

    /*
 * ── dragging a row ───────────────────────────────────────────────────────
 *
 * A PRESS ON THE GRIP IS NOT YET A DRAG. It chooses the row (Apply), and only
 * a pointer that has moved DRAG_START_PX from where it went down makes it one:
 * then the row takes the drag colour, the ghost appears under the pointer and
 * the mark shows where the layer would land. A press and release that never
 * moved is a click, and moves nothing.
 *
 * BY POSITION, NOT BY NODE. Where a live drag lands is the row under the
 * pointer's height in the window, clamped to the rows that hold layers -- so a
 * release on a row's dots, in the two-pixel gap between rows, over the title
 * bar or over the empty slots below the stack all land somewhere sensible. By
 * node, each of those was a release over something that was not a row, and
 * each silently cancelled the drag. Released outside the window it is still a
 * cancel: putting a layer somewhere the user did not point at is worse than
 * doing nothing.
 */
    static constexpr int32_t DRAG_START_PX = 4;

    bool draggingRow() const { return m_dragging != 0; }

    // Motion while the grip is held. True when it was the drag's, which is
    // also what keeps it from hovering the eyes it passes over.
    bool DragRow(Point2D at)
    {
        if (m_dragging == 0) return false;
        const Point2D l = window_local(at);
        if (!m_drag_live)
        {
            if (std::abs(l.x - m_drag_press.x) < DRAG_START_PX
                && std::abs(l.y - m_drag_press.y) < DRAG_START_PX) return true;
            m_drag_live = true;
            show_ghost();
            Refresh();                 // the carried row in the drag colour
        }
        const int32_t from = row_of_layer(m_dragging);
        const int32_t last = last_layer_row();
        if (last < 0) return true;
        if (m_ghost)
        {
            const Rect2D first_r = row_rect(0), last_r = row_rect(static_cast<size_t>(last));
            const int32_t gy = std::clamp(l.y - m_drag_grab_dy, first_r.y, last_r.y);
            paint_node_moved(m_ghost, first_r.x, gy);
        }
        place_mark(from, row_at(l.y));
        return true;
    }

    bool Drop(Point2D at)
    {
        if (m_dragging == 0) return false;
        const bool live = m_drag_live;
        const ETCS::RID layer = m_dragging;
        end_row_drag();
        if (!live || !m_document || !Contains(at)) { Refresh(); return true; }

        const int32_t target = row_at(window_local(at).y);
        std::vector<PaintLayer*> stack;
        m_document->OrderedLayers(stack);
        const size_t total = stack.size();
        const size_t from_top = static_cast<size_t>(target) + static_cast<size_t>(m_scroll);
        if (target >= 0 && from_top < total && m_rows[static_cast<size_t>(target)].layer != layer)
        {
            // Depth counts from the BOTTOM (PaintDocument::MoveLayerTo, the
            // order key); rows count from the top. Turned round here, the one
            // place that knows both conventions.
            const int32_t depth = static_cast<int32_t>(total - 1 - from_top);
            m_document->MoveLayerTo(layer, depth);
            ETCS_LOG("PaintLayerPanel", "dropped on row " << target << " -- depth " << depth);
        }
        Refresh();
        repaint();                     // a different stack composites differently
        return true;
    }

    // The button came up somewhere this panel never heard about (another
    // pane took the release): whatever was being carried is put down.
    void CancelRowDrag()
    {
        if (m_dragging == 0) return;
        const bool live = m_drag_live;
        end_row_drag();
        if (live) Refresh();
    }

    /*
 * The window under the pointer while the title bar is held. True while that
 * is so, which tells the input edge the motion was a move, not a hover and
 * not a stroke.
 *
 * BY THE SetPosition VERB, not the family's MoveTo, and the difference is
 * visible: a compositor's MoveTo only STAGES the new origin, to be applied at
 * the top of its own next recompose (CompositeDrawable2D::applyPendingGeometry),
 * and a pane whose contents did not change is not recomposed by the frame --
 * so the window answered "moved" and stayed exactly where it was. SetPosition
 * is what the colour wheel's pane moves by (PaintColorWheel::move_pane) and
 * takes effect on the next compose of the parent.
 */
    bool moving() const { return m_moving; }
    bool DragWindow(Point2D at)
    {
        if (!m_moving) return false;
        if (!paint_node_moved(m_window,
                              m_origin.x + (at.x - m_grab.x),
                              m_origin.y + (at.y - m_grab.y)))
        { m_moving = false; return false; }
        return true;
    }

    bool EndWindowDrag()
    {
        if (!m_moving) return false;
        m_moving = false;
        return true;
    }

    // Whether a point in the pane's parent's space is over the window. Asked
    // for a wheel notch, which carries no point of its own and so cannot be
    // picked (PaintInput::RouteEvent) -- the last routed position stands in
    // for it, and the window's bounds are the only thing that has to hold.
    bool Contains(Point2D at) const { return paint_window_contains(m_window, at); }

    /*
 * HOVER: the layer whose EYE is under the pointer at full strength, every other
 * one dimmed.
 *
 * THE EYE AND NOT THE ROW, which is the whole of the gesture. Isolating on the
 * row meant the picture faded whenever the pointer crossed the window on its
 * way to anything -- choosing a layer, renaming one, reaching the +, or just
 * passing over -- so the answer to "which layer is this" arrived constantly and
 * uninvited, and the thing being looked at was the thing being hidden. The eye
 * is the control that is ABOUT visibility, so hovering it is the one moment
 * where "show me only this layer" is what the hand is already asking; a press
 * there commits the same thing permanently.
 *
 * Stated to the document as one call over the whole stack (PaintDocument::
 * IsolateLayer) rather than as a dim per row, so leaving an eye is the same
 * call with a different subject and there is no per-row bookkeeping to get
 * wrong when the pointer skips one. Any other node -- another part of the row,
 * the title bar, the picture -- clears the isolation, which is what "the
 * pointer is not on an eye" means without needing an exit event.
 */
    void Hover(ETCS::RID node)
    {
        if (!m_document) return;
        auto it = m_regions.find(node);
        const bool on_eye = (it != m_regions.end() && it->second.region == Region::Eye
                             && it->second.row < m_rows.size());
        const ETCS::RID subject = on_eye ? m_rows[it->second.row].layer : 0;
        if (subject == m_hovering) return;          // nothing changed; do not re-walk the stack
        m_hovering = subject;
        /*
     * THE TARGET, NOT THE VALUE. A hover has a DURATION -- the pointer rests on
     * the eye for as long as the question is being asked -- and a snap to the
     * dim is the one thing that throws that away. Set where the isolation is
     * going; Tick walks it there a frame at a time.
     */
        if (subject != 0) m_subject = subject;
        m_dim_target = (subject != 0) ? m_hover_dim : 1.0f;
    }

    /*
 * ── the fade, one frame at a time ────────────────────────────────────────
 *
 * The panel claims Animated (ontology/Animated.h) and is stepped by whatever
 * drives that family, and only while there is somewhere left to go: Animating
 * is what keeps the driver from re-compositing the document sixty times a
 * second for a dim that has arrived.
 *
 * THE SUBJECT SURVIVES THE FADE OUT. On the way in it is the layer whose eye is
 * under the pointer; on the way out there is no layer under the pointer at all,
 * and naming nobody would dim the one that was being shown before bringing it
 * back -- a flash of exactly the wrong thing. So the last subject is held until
 * the dim reaches 1.0 and everything is at full strength again, at which point
 * it means nothing and is dropped.
 */
    bool Fading() const { return m_dim_now != m_dim_target; }

    bool AnimatingConcrete() override { return m_document && Fading(); }

    /*
 * A DURATION, NOT A PER-VISIT STEP. This used to move 0.085 per frame, which
 * read as a fade only because the display happened to be running at sixty --
 * on a 144Hz panel the same code is a flicker and on a 30Hz one it is a slide.
 * The whole swing takes FADE_FULL_MS whatever the rate; the isolation depth is
 * a fraction of that swing, so the actual fade is proportionally shorter and
 * the two ends stay in step with each other, which is what makes leaving an eye
 * feel like the reverse of arriving at one.
 */
    void AdvanceConcrete(double dt_ms) override
    {
        constexpr float FADE_FULL_MS = 150.0f;
        const float step = static_cast<float>(dt_ms) / FADE_FULL_MS;
        if (m_dim_now < m_dim_target) m_dim_now = std::min(m_dim_target, m_dim_now + step);
        else                          m_dim_now = std::max(m_dim_target, m_dim_now - step);
        m_document->IsolateLayer(m_subject, m_dim_now);
        if (m_dim_now >= 1.0f) m_subject = 0;
        repaint();
    }

    // The rename by verb: a caller that already has the whole name -- a script,
    // the page, a test -- does not have to type it a key at a time.
    bool CommitRename(const std::string& name)
    {
        if (m_renaming == 0 || !m_document) return false;
        m_document->RenameLayer(m_renaming, name);
        m_editing = false;
        m_edit.clear();
        m_renaming = 0;
        Refresh();
        return true;
    }

    /*
 * A KEY WHILE A NAME IS OPEN, and nothing otherwise.
 *
 * Returns true when it was consumed, which is the whole of what the input edge
 * needs: while this is open the panel is where typing goes, and every key it
 * takes is one the canvas does not act on. Enter keeps the name, Escape drops
 * it, Backspace edits; anything the key map has no character for is swallowed
 * rather than ignored, so a function key pressed mid-rename does not fall
 * through to a tool.
 *
 * GLFW's codes, named for the same reason PaintInput names them.
 */
    bool KeyIn(uint16_t key)
    {
        if (!m_editing) return false;
        constexpr uint16_t KEY_ESCAPE = 256, KEY_ENTER = 257, KEY_BACKSPACE = 259;
        if (key == KEY_ENTER)     { end_edit(true);  return true; }
        if (key == KEY_ESCAPE)    { end_edit(false); return true; }
        if (key == KEY_BACKSPACE)
        {
            if (!m_edit.empty()) m_edit.pop_back();
            Refresh();
            return true;
        }
        const char ch = paint_key_to_char(key);
        if (ch == 0) return true;
        if (m_edit.size() < 48) m_edit.push_back(ch);
        Refresh();
        return true;
    }

    bool editing() const { return m_editing; }

    void Report() const
    {
        ETCS_LOG("PaintLayerPanel", m_rows.size() << " row(s), scroll " << m_scroll
                 << ", document " << (m_document ? "bound" : "UNBOUND")
                 << (m_renaming ? " [renaming]" : "")
                 << (m_dragging ? " [dragging]" : "")
                 << (m_moving ? " [moving window]" : ""));
        for (size_t i = 0; i < m_rows.size(); ++i)
            ETCS_LOG("PaintLayerPanel", "  row " << i << " -> layer RID:" << m_rows[i].layer);
    }

    /*
 * THE SAME FOUR ACTIONS, ADDRESSED BY ROW INSTEAD OF BY NODE.
 *
 * Apply above answers "a press landed on this node"; these answer "do row N's
 * eye", which is what a caller that is not a pointer has. A work function is
 * exported and callable from anywhere -- a script, another module, the page's
 * JS through etcs_web_call -- so exposing the actions this way costs one verb
 * each and means the window is drivable without synthesising a pick at made-up
 * coordinates. There is no ceiling on how many verbs a type may have, and no
 * reason to make every caller pretend to be a mouse.
 *
 * Row indices are what the window shows, top first, so they mean the same thing
 * a caller can see. Out of range and empty rows are silent: a page that asks
 * about a row the stack has scrolled past is asking a reasonable question with
 * the answer "nothing there".
 */
    bool SelectRow(size_t row)
    {
        if (row >= m_rows.size() || m_rows[row].layer == 0 || !m_document) return false;
        m_document->SetActiveLayer(m_rows[row].layer);
        m_renaming = 0;
        Refresh();
        return true;
    }

    bool ToggleRow(size_t row)
    {
        if (row >= m_rows.size() || m_rows[row].layer == 0) return false;
        ETCS::Entity* raw = paint_resolve_tag("PaintLayer", m_rows[row].layer);
        if (!raw) return false;
        static_cast<PaintLayer*>(raw->getTrueType())->ToggleVisible();
        Refresh();
        repaint();
        return true;
    }

    bool RemoveRow(size_t row)
    {
        if (row >= m_rows.size() || m_rows[row].layer == 0 || !m_document) return false;
        const ETCS::RID layer = m_rows[row].layer;
        m_document->RemoveLayer(layer);
        if (m_renaming == layer) m_renaming = 0;
        if (m_dragging == layer) m_dragging = 0;
        Refresh();
        repaint();
        return true;
    }

    // MOVE, not swap: the layer at `from` takes the depth of the row at `to` and
    // the stack closes up behind it, which is what dragging a row between two
    // others means. Same call the pointer drag ends in.
    bool MoveRow(size_t from, size_t to)
    {
        if (from >= m_rows.size() || to >= m_rows.size() || !m_document) return false;
        if (m_rows[from].layer == 0) return false;
        std::vector<PaintLayer*> stack;
        m_document->OrderedLayers(stack);
        const size_t total = stack.size();
        const size_t to_from_top = to + static_cast<size_t>(m_scroll);
        if (to_from_top >= total) return false;
        m_document->MoveLayerTo(m_rows[from].layer,
                                static_cast<int32_t>(total - 1 - to_from_top));
        Refresh();
        repaint();
        return true;
    }

    // Arm a rename by row, for the same reason: a page that knows a name field
    // was double-clicked should not have to press a label twice to say so.
    bool ArmRename(size_t row)
    {
        if (row >= m_rows.size() || m_rows[row].layer == 0) return false;
        m_renaming = m_rows[row].layer;
        return true;
    }

    // Hover by row, and -1 -- any out-of-range index -- for "the pointer is
    // nowhere near this window", which is the call a pane-leave makes. By ROW
    // rather than by node, so a caller that is not a pointer can ask for the
    // same isolation the eye's hover gives (see Hover).
    void HoverRow(int32_t row)
    {
        if (!m_document) return;
        const ETCS::RID subject =
            (row >= 0 && static_cast<size_t>(row) < m_rows.size())
                ? m_rows[static_cast<size_t>(row)].layer : 0;
        if (subject == m_hovering) return;
        m_hovering = subject;
        if (subject != 0) m_subject = subject;
        m_dim_target = (subject != 0) ? m_hover_dim : 1.0f;
    }

    /*
 * THE POINTER LEFT THIS WINDOW ENTIRELY.
 *
 * Needed because "leaving" is not an event anybody sends: with a pass budget,
 * a pane that does not contain the point is never offered it, so a panel that
 * inferred leaving from a node it did not recognise would never hear the
 * motion that told it. The router says so explicitly instead -- see
 * PaintRouter::Route -- and this is what it says.
 *
 * Drops the drag as well as the hover. A drag released outside the window is
 * a cancelled drag, not a drop at the nearest row: putting the layer somewhere
 * the user did not point at is worse than doing nothing.
 */
    void Left()
    {
        CancelRowDrag();
        m_moving = false;
        HoverRow(-1);
    }

    size_t rowCount() const { return m_rows.size(); }
    ETCS::RID rowLayer(size_t i) const { return i < m_rows.size() ? m_rows[i].layer : 0; }
    int32_t scroll() const { return m_scroll; }

    /*
 * IS THIS NODE ONE OF MINE -- asked by the router, answered from the index the
 * press path already builds (m_regions, plus the pane the title bar moves).
 *
 * Exists so somebody else can decide what a press ELSEWHERE means without
 * having to know what this panel is made of. The alternative was exporting the
 * row layout, or re-running the pick; this is the one bit either of those would
 * have been used to compute.
 */
    bool owns(ETCS::RID node) const
    {
        if (node == 0) return false;
        if (node == m_window) return true;
        return m_regions.find(node) != m_regions.end();
    }

    /*
 * CLOSE THE NAME FIELD FROM OUTSIDE, keeping what was typed.
 *
 * KEEPING, not dropping, because that is what a press somewhere else means
 * everywhere else: the text is the user's work and leaving is not a cancel.
 * Escape is still the cancel and is still the only one (KeyIn).
 *
 * Idempotent -- end_edit returns immediately when nothing is open -- so the
 * router may call it on every press without first asking whether it needs to.
 */
    void CloseEdit() { end_edit(true); }

private:
    /*
     * TRIM IS A LIST, and it is a list because there turned out to be more than
     * one of them and no way to tell in advance how many. A row's parts divide
     * into CONTROLS (a press on them means something, so each needs its own
     * name and its own Region) and DECORATION that has no meaning of its own
     * and must simply appear and disappear with the row: the eye's pupil, the
     * grip's dots. `pupil` was a named slot for the first of those, which made
     * the second one -- added in the same patch, four lines away in the row
     * script -- have nowhere to go, so it was never registered, never hidden,
     * and six sets of dots floated over an empty panel. A slot per decoration
     * is a bug waiting for the next decoration; a list is not.
     */
    struct Row { ETCS::RID bg, eye, thumb, label, del; ETCS::RID layer;
                 ETCS::RID grip = 0, mup = 0, mdn = 0;
                 ETCS::RID iris = 0;             // tinted with the eye -- see RowNode
                 std::vector<ETCS::RID> trim; };
    struct Hit { size_t row; Region region; };
    bool m_collapsed = false;
    uint32_t m_full_h = 0;             // the window's height while folded -- paint_window_fold
    // The title bar's own eye (BeginTitle), tinted open or shut with the
    // window, and the window's body parts, hidden with the rows.
    bool      m_title_open = false;
    ETCS::RID m_view_eye   = 0;
    ETCS::RID m_view_iris  = 0;
    std::vector<ETCS::RID> m_view_trim;
    std::vector<ETCS::RID> m_body;

    void tint_eye(ETCS::RID eye, ETCS::RID iris, bool open)
    {
        const float* o = open ? m_eye_shown : m_eye_hidden;
        const float* i = open ? m_iris_open : m_iris_shut;
        paint_node_fill(eye,  o[0], o[1], o[2], 1.0f);
        if (iris) paint_node_fill(iris, i[0], i[1], i[2], 1.0f);
    }

    // The title bar's eye says whether the rows are showing; shut, its pupil
    // goes with the iris it sits on.
    void tint_view_eye()
    {
        if (!m_view_eye) return;
        tint_eye(m_view_eye, m_view_iris, !m_collapsed);
        for (ETCS::RID n : m_view_trim) paint_node_hidden(n, m_collapsed);
    }

    float m_iris_open[3] = { 0.35f, 0.55f, 0.95f };
    float m_iris_shut[3] = { 0.106f, 0.110f, 0.078f };
    float m_row_drag[3]  = { 0.86f, 0.60f, 0.22f };

    // ── the row drag's geometry, all in the window's own space ───────────
    ETCS::RID m_ghost = 0, m_ghost_thumb = 0, m_ghost_label = 0, m_drop_mark = 0;
    bool      m_drag_live = false;
    Point2D   m_drag_press{ 0, 0 };
    int32_t   m_drag_grab_dy = 0;      // where in its row the grip was taken

    Point2D window_local(Point2D at) const
    {
        ETCS::Held<Drawable2D_> w = ETCS::resolve_held<Drawable2D_>("Drawable2D", m_window);
        if (!w) return at;
        const Rect2D b = w->Bounds();
        return Point2D{ at.x - b.x, at.y - b.y };
    }

    // A row's plate is its pane, so its box in the window is the row's.
    Rect2D row_rect(size_t i) const
    {
        if (i >= m_rows.size()) return Rect2D{ 0, 0, 0, 0 };
        ETCS::Held<Drawable2D_> r = ETCS::resolve_held<Drawable2D_>("Drawable2D", m_rows[i].bg);
        return r ? r->Bounds() : Rect2D{ 0, 0, 0, 0 };
    }

    int32_t last_layer_row() const
    {
        int32_t last = -1;
        for (size_t i = 0; i < m_rows.size(); ++i) if (m_rows[i].layer) last = static_cast<int32_t>(i);
        return last;
    }

    int32_t row_of_layer(ETCS::RID layer) const
    {
        for (size_t i = 0; i < m_rows.size(); ++i) if (m_rows[i].layer == layer) return static_cast<int32_t>(i);
        return -1;
    }

    // The row a height lands on: the first whose bottom edge is below it, so
    // the gap under a row belongs to that row, and anything past the last
    // layer is the last layer.
    int32_t row_at(int32_t ly) const
    {
        const int32_t last = last_layer_row();
        for (int32_t i = 0; i <= last; ++i)
        {
            const Rect2D r = row_rect(static_cast<size_t>(i));
            if (ly < r.y + static_cast<int32_t>(r.h) + 2) return i;
        }
        return last;
    }

    void show_ghost()
    {
        if (!m_ghost) return;
        PaintLayer* layer = nullptr;
        if (ETCS::Entity* raw = paint_resolve_tag("PaintLayer", m_dragging))
            layer = static_cast<PaintLayer*>(raw->getTrueType());
        if (layer)
        {
            if (m_ghost_thumb) paint_thumb(m_ghost_thumb, layer);
            if (m_ghost_label) paint_node_text(m_ghost_label, layer->name());
        }
        paint_node_fill(m_ghost, m_row_drag[0], m_row_drag[1], m_row_drag[2], 0.92f);
        paint_node_hidden(m_ghost, false);
    }

    // The bar goes in the gap the layer will land in: above the target when
    // it moves up, below it when it moves down, nowhere when it would not move.
    void place_mark(int32_t from, int32_t target)
    {
        if (!m_drop_mark) return;
        if (target < 0 || target == from) { paint_node_hidden(m_drop_mark, true); return; }
        const Rect2D r = row_rect(static_cast<size_t>(target));
        const int32_t y = (from >= 0 && target < from) ? r.y - 2 : r.y + static_cast<int32_t>(r.h);
        paint_node_moved(m_drop_mark, r.x, y);
        paint_node_hidden(m_drop_mark, false);
    }

    void end_row_drag()
    {
        m_dragging = 0;
        m_drag_live = false;
        if (m_ghost) paint_node_hidden(m_ghost, true);
        if (m_drop_mark) paint_node_hidden(m_drop_mark, true);
    }

    /*
 * Closing the field, keeping the name or not. Rename goes through the document
 * (RenameLayer) rather than the layer, because a rename is a change to the
 * page -- it is what Touch and therefore the window's own refresh hang off.
 * An empty name is a cancel: a layer with no name is a row you cannot read.
 */
    void begin_edit(ETCS::RID layer)
    {
        if (m_editing) end_edit(false);
        if (!m_document) return;
        m_document->SetActiveLayer(layer);
        m_renaming = layer;
        m_editing  = true;
        m_edit.clear();
        if (ETCS::Entity* raw = paint_resolve_tag("PaintLayer", layer))
            m_edit = static_cast<PaintLayer*>(raw->getTrueType())->name();
        ETCS_LOG("PaintLayerPanel", "renaming RID:" << layer
                 << " -- type, Enter to keep, Esc to drop it.");
        Refresh();
    }

    void end_edit(bool keep)
    {
        if (!m_editing) { m_renaming = 0; return; }
        const ETCS::RID subject = m_renaming;
        const std::string text = m_edit;
        m_editing = false;
        m_edit.clear();
        m_renaming = 0;
        if (keep && m_document && subject != 0 && !text.empty())
        {
            m_document->RenameLayer(subject, text);
            ETCS_LOG("PaintLayerPanel", "RID:" << subject << " renamed \"" << text << "\"");
        }
        Refresh();
    }

    /*
 * ── the row's picture of its layer ───────────────────────────────────────
 *
 * WHAT IS ON THE LAYER, drawn into whatever raster the script put in the row.
 * A name tells you which layer you MEANT; a thumbnail tells you which one you
 * are looking at, and with two imported images and a paper layer the names are
 * all anybody has to go on otherwise.
 *
 * BY RID, INTO SOMEBODY ELSE'S PIXELS, which is the same seam draw_lift uses:
 * the node is a compositor from another module, reached as Pixels_ and written
 * directly (CompositeDrawable2D's own note says it is reached as Pixels_ and
 * never as a concrete type). It has to be RETAINED in the script, or its next
 * recompose clears what was just written.
 *
 * AVERAGED, NOT SAMPLED, and that is the difference between a thumbnail and a
 * blank square. A page is 1024 wide and a thumb is 24, so one nearest-neighbour
 * sample per destination pixel reads one source pixel in 1800 and a six-pixel
 * stroke across the picture survives in none of them -- measured: a drawn
 * layer's thumbnail came back as bare checker. Each destination pixel now
 * averages its whole source cell, at up to 8x8 evenly spaced taps, so a thin
 * mark arrives faint rather than absent and the cost stays fixed however big
 * the page is.
 *
 * FITTED: the thumb is square and a page is not, so the picture is scaled by
 * the larger axis and centred, which is what makes two layers of one document
 * produce thumbnails that line up. Over a checker,
 * because the common case is a layer that is mostly transparent and a
 * transparent thumbnail drawn on a dark row is indistinguishable from an empty
 * one.
 */
    static void paint_thumb(ETCS::RID node, PaintLayer* layer)
    {
        if (node == 0) return;
        Pixels_* dst = ETCS::resolve_in_family<Pixels_>("Pixels", node);
        if (!dst) return;
        uint8_t* out = dst->PixelData();
        if (!out) return;
        const int32_t tw = static_cast<int32_t>(dst->PixelWidth());
        const int32_t th = static_cast<int32_t>(dst->PixelHeight());
        if (tw <= 0 || th <= 0) return;

        const int32_t lw = layer ? static_cast<int32_t>(layer->width())  : 0;
        const int32_t lh = layer ? static_cast<int32_t>(layer->height()) : 0;
        const uint8_t* src = layer ? layer->PixelData() : nullptr;
        const float scale = (lw > 0 && lh > 0)
            ? std::max(static_cast<float>(lw) / tw, static_cast<float>(lh) / th) : 0.0f;
        const int32_t ox = (lw > 0 && scale > 0.0f)
            ? (tw - static_cast<int32_t>(lw / scale)) / 2 : 0;
        const int32_t oy = (lh > 0 && scale > 0.0f)
            ? (th - static_cast<int32_t>(lh / scale)) / 2 : 0;

        for (int32_t y = 0; y < th; ++y)
            for (int32_t x = 0; x < tw; ++x)
            {
                // The checker under everything, 4px squares.
                const bool light = (((x >> 2) + (y >> 2)) & 1) != 0;
                float r = light ? 0.44f : 0.34f, g = r, b = r;

                if (src && scale > 0.0f)
                {
                    const int32_t sx0 = static_cast<int32_t>((x - ox) * scale);
                    const int32_t sy0 = static_cast<int32_t>((y - oy) * scale);
                    const int32_t sx1 = std::min(static_cast<int32_t>((x - ox + 1) * scale), lw);
                    const int32_t sy1 = std::min(static_cast<int32_t>((y - oy + 1) * scale), lh);
                    if (sx0 >= 0 && sy0 >= 0 && sx0 < lw && sy0 < lh && sx1 > sx0 && sy1 > sy0)
                    {
                        const int32_t stepx = std::max(1, (sx1 - sx0) / 8);
                        const int32_t stepy = std::max(1, (sy1 - sy0) / 8);
                        float ar = 0, ag = 0, ab = 0, aa = 0; int taps = 0;
                        for (int32_t sy = sy0; sy < sy1; sy += stepy)
                            for (int32_t sx = sx0; sx < sx1; sx += stepx)
                            {
                                const uint8_t* sp = src + (static_cast<size_t>(sy) * lw + sx) * 4;
                                const float a = sp[3] / 255.0f;
                                ar += (sp[0] / 255.0f) * a; ag += (sp[1] / 255.0f) * a;
                                ab += (sp[2] / 255.0f) * a; aa += a;
                                ++taps;
                            }
                        if (taps > 0 && aa > 0.0f)
                        {
                            // Premultiplied while summing, so a cell that is
                            // mostly transparent keeps the colour of the part
                            // that is not, at that part's weight.
                            const float cover = aa / taps;
                            r = r * (1.0f - cover) + (ar / aa) * cover;
                            g = g * (1.0f - cover) + (ag / aa) * cover;
                            b = b * (1.0f - cover) + (ab / aa) * cover;
                        }
                    }
                }
                uint8_t* dp = out + (static_cast<size_t>(y) * tw + x) * 4;
                dp[0] = paint_to_byte(r); dp[1] = paint_to_byte(g);
                dp[2] = paint_to_byte(b); dp[3] = 255;
            }
        etcs_mark_observed(dst);
    }

    // What the window changed, shown. Silent with nothing bound: a panel driven
    // from a test or a script has no view to put back.
    void repaint() { if (m_surface) m_surface->Render(); }

    PaintDocument* m_document = nullptr;
    PaintSurface*  m_surface  = nullptr;
    // The pane the title bar moves, and the move in flight: where the window
    // and the pointer were at the press, so each motion is a fresh offset from
    // there rather than a sum of deltas that drifts.
    ETCS::RID m_window = 0;
    // The name being typed, and whether anything is being typed at all --
    // m_renaming alone said "armed", which is not the same as "has the
    // keyboard" (KeyIn).
    // The fade: what is shown at full strength, where the dim on everything
    // else is going, and where it has got to (Tick).
    ETCS::RID   m_subject    = 0;
    float       m_dim_target = 1.0f;
    float       m_dim_now    = 1.0f;
    bool        m_editing = false;
    std::string m_edit;
    bool      m_moving = false;
    Point2D   m_grab{ 0, 0 };
    Point2D   m_origin{ 0, 0 };
    std::vector<Row> m_rows;
    std::unordered_map<ETCS::RID, Hit> m_regions;
    int32_t   m_scroll    = 0;
    ETCS::RID m_renaming  = 0;
    ETCS::RID m_dragging  = 0;
    ETCS::RID m_hovering  = 0;
    float m_hover_dim = 0.25f;
    /*
 * THE DEFAULTS ARE THE RULER'S, so a script that never calls SetEyeColors or
 * SetRowColors gets the panel that belongs to this window rather than one a
 * shade off the canvas background -- see paint_layers.etcs for the argument.
 * Ink and band are PaintSurface::m_ruler_ink and m_ruler_bg; the selected row
 * is the ruler's in-band highlight, the colour a tick takes inside the page.
 */
    float m_eye_shown[3]  = { 0.94f, 0.89f, 0.78f };
    float m_eye_hidden[3] = { 0.35f, 0.36f, 0.28f };
    float m_row_sel[3]    = { 0.35f, 0.55f, 0.95f };
    float m_row_idle[3]   = { 0.15f, 0.16f, 0.11f };
};

#endif // PAINTPROVIDER_PAINTLAYERPANEL_H__
