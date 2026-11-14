#ifndef PAINTPROVIDER_PAINTPAGEPANEL_H__
#define PAINTPROVIDER_PAINTPAGEPANEL_H__

#include "../../../core_defs.h"
#include "../../../ontology.h"
#include "PaintPages.h"   // in order: everything above this in the module is visible here

/*
 * ── PaintPagePanel ───────────────────────────────────────────────────────
 *
 * THE STORE, AS A LIST YOU CAN SEE -- the layer window's bargain, for pages.
 * A row is [thumb][name ....][x] assembled by a script (paint_page_row.etcs)
 * from RenderProvider's own leaves in the caller's tree; this type maps those
 * nodes to what a press on each means and draws none of them. The rows are a
 * window onto the store's pages, newest first, and the wheel over the list
 * moves that window a row per notch (PaintInput::RouteEvent), so the count of
 * rows is the resident set and not a cap on how many pages there are -- the
 * same argument paint_layers.etcs makes for layers.
 *
 * The picture per row is the thumbnail the store made when the page was saved
 * (PaintPages::Thumb), copied into the row's retained raster: a list that
 * decoded a page's layers to draw a row would cost 6 MB of PAM per row per
 * refresh, which is why the store keeps one.
 *
 *   thumb / body  load that page (PaintPages::Load); the present is flushed
 *                 to its slot first, as every switch does
 *   name          the same, and pressed again on the page already on screen
 *                 it opens the name for typing -- Enter keeps, Escape drops
 *   x             delete that page from the store (PaintPages::Delete). The
 *                 page on screen stays on screen; it is only no longer in a
 *                 slot, and the next save gives it a new one
 *
 * Bound into the gear menu's own input (PaintInput::BindPagePanel), because
 * that is the pane it lives on; it could as easily sit in a window of its
 * own. Tells the menu after a load (PaintCanvasMenu.PageChanged, by verb, as
 * that type is declared below this one) so the width and height readouts
 * follow the page.
 */
class PaintPagePanel : public DeletableBase<PaintPagePanel>
{
public:
    WIRE_TYPE_IDENTITY(PaintPagePanel);

    PaintPagePanel() = default;
    bool DeleteConcrete() override { return true; }

    enum class Region : uint8_t { Body, Label, Delete };

    bool Create()
    {
        m_rows.clear();
        m_regions.clear();
        this->addTag("active");
        return true;
    }

    void BindPages(ETCS::RID pages)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintPages", pages);
        if (raw) m_pages = static_cast<PaintPages*>(raw->getTrueType());
        Refresh();
    }

    // The menu whose readouts follow a load, reached by verb -- see the header.
    void BindMenu(ETCS::RID menu)    { m_menu = menu; }
    // The list's own pane, for the wheel's containment test (Contains).
    void BindWindow(ETCS::RID pane)  { m_window = pane; }

    void SetRowColors(float sr, float sg, float sb, float ur, float ug, float ub)
    {
        m_row_sel[0] = sr; m_row_sel[1] = sg; m_row_sel[2] = sb;
        m_row_idle[0] = ur; m_row_idle[1] = ug; m_row_idle[2] = ub;
    }

    // A row is assembled the way a layer row is: opened, then parts by name.
    void BeginRow() { m_rows.push_back(Row{}); }

    void RowNode(const std::string& what, ETCS::RID node)
    {
        if (node == 0) return;
        if (m_rows.empty()) { ETCS_LOG("PaintPagePanel", "RowNode before BeginRow -- ignored."); return; }
        const size_t idx = m_rows.size() - 1;
        Row& row = m_rows[idx];
        if      (what == "bg")    { row.bg    = node; m_regions[node] = Hit{ idx, Region::Body }; }
        else if (what == "thumb") { row.thumb = node; m_regions[node] = Hit{ idx, Region::Body }; }
        else if (what == "label") { row.label = node; m_regions[node] = Hit{ idx, Region::Label }; }
        else if (what == "del")   { row.del   = node; m_regions[node] = Hit{ idx, Region::Delete }; }
        else ETCS_LOG("PaintPagePanel", "RowNode: '" << what << "' is not a part of a row "
                      "(bg thumb label del) -- RID:" << node << " is attached to nothing.");
    }

    bool owns(ETCS::RID node) const { return node != 0 && m_regions.find(node) != m_regions.end(); }

    bool Contains(Point2D at) const { return paint_window_contains(m_window, at); }

    // Rows, top first; negative toward the newest page. Clamped, as the layer
    // window's is: a list that wraps is one you cannot find anything in.
    void Scroll(int32_t delta)
    {
        const int32_t rows  = static_cast<int32_t>(m_rows.size());
        const int32_t total = static_cast<int32_t>(m_ids.size());
        const int32_t most  = (total > rows) ? (total - rows) : 0;
        m_scroll = std::clamp(m_scroll + delta, 0, most);
        Refresh();
    }

    /*
 * A press on one of the parts. `is_press` false is a release, which is ours
 * to swallow (so the sheet under the menu does not see the second half of a
 * click) and does nothing.
 */
    bool Apply(ETCS::RID node, bool is_press)
    {
        auto it = m_regions.find(node);
        if (it == m_regions.end()) return false;
        if (!is_press) return true;
        if (!m_pages) return true;
        const Hit hit = it->second;
        if (hit.row >= m_rows.size()) return true;
        const int64_t id = m_rows[hit.row].id;
        if (id == 0) return true;                       // an empty slot is still ours
        // The row being typed into keeps the keys whatever part of it is
        // pressed; only its x, or a press somewhere else, ends the field.
        if (m_editing && m_renaming == id && hit.region != Region::Delete) return true;

        switch (hit.region)
        {
        case Region::Label:
            // On the page already on screen a press is the first half of a
            // rename; anywhere else it is a load, like the body.
            if (id == m_pages->current()) { begin_edit(id); return true; }
            [[fallthrough]];
        case Region::Body:
            end_edit(false);
            if (id != m_pages->current() && m_pages->Load(id)) page_changed();
            break;
        case Region::Delete:
            if (m_editing && m_renaming == id) end_edit(false);
            m_pages->Delete(id);
            break;
        }
        Refresh();
        return true;
    }

    bool KeyIn(uint16_t key)
    {
        if (!m_editing) return false;
        constexpr uint16_t KEY_ESCAPE = 256, KEY_ENTER = 257, KEY_BACKSPACE = 259;
        if (key == KEY_ENTER)     { end_edit(true);  return true; }
        if (key == KEY_ESCAPE)    { end_edit(false); return true; }
        if (key == KEY_BACKSPACE) { if (!m_edit.empty()) m_edit.pop_back(); Refresh(); return true; }
        const char ch = paint_key_to_char(key);
        if (ch == 0) return true;
        if (m_edit.size() < 48) m_edit.push_back(ch);
        Refresh();
        return true;
    }

    bool editing() const { return m_editing; }
    void CloseEdit() { end_edit(true); }

    /*
 * Re-bind the rows to the store's pages, newest first, from the scroll
 * offset. Called after anything that changes what the list should say -- a
 * save, a load, a delete, a rename, a scroll -- rather than on a clock.
 */
    void Refresh()
    {
        m_ids.clear();
        std::vector<PaintPages::PageInfo> pages;
        if (m_pages) m_pages->Pages(pages);
        std::vector<const PaintPages::PageInfo*> newest;
        for (size_t i = pages.size(); i-- > 0; ) newest.push_back(&pages[i]);
        for (const auto* p : newest) m_ids.push_back(p->id);
        const int64_t here = m_pages ? m_pages->current() : 0;

        {
            const int32_t most = (newest.size() > m_rows.size())
                               ? static_cast<int32_t>(newest.size() - m_rows.size()) : 0;
            m_scroll = std::clamp(m_scroll, 0, most);
        }
        for (size_t i = 0; i < m_rows.size(); ++i)
        {
            Row& row = m_rows[i];
            const size_t at = i + static_cast<size_t>(m_scroll);
            const PaintPages::PageInfo* info = (at < newest.size()) ? newest[at] : nullptr;
            row.id = info ? info->id : 0;
            if (!info)
            {
                // An empty slot is drawn as nothing rather than hidden: the
                // list is a fixed frame and a gap in it is honest.
                paint_node_fill(row.bg, m_row_idle[0], m_row_idle[1], m_row_idle[2], 0.0f);
                paint_node_hidden(row.thumb, true);
                paint_node_hidden(row.del, true);
                paint_node_text(row.label, "");
                continue;
            }
            const bool current = (info->id == here && here != 0);
            const float* c = current ? m_row_sel : m_row_idle;
            paint_node_fill(row.bg, c[0], c[1], c[2], 1.0f);
            paint_node_hidden(row.thumb, false);
            paint_node_hidden(row.del, false);
            paint_thumb(row.thumb, info->id);
            const std::string name = info->name.empty() ? ("page " + std::to_string(info->id)) : info->name;
            if (m_editing && m_renaming == info->id)
                paint_node_text(row.label, m_edit + "_");
            else
                paint_node_text(row.label, name + " " + std::to_string(info->w) + "x" + std::to_string(info->h));
        }
    }

    void Report() const
    {
        ETCS_LOG("PaintPagePanel", m_rows.size() << " row(s), scroll " << m_scroll << ", "
                 << m_ids.size() << " page(s)" << (m_editing ? " [renaming]" : ""));
    }

private:
    struct Row { ETCS::RID bg = 0, thumb = 0, label = 0, del = 0; int64_t id = 0; };
    struct Hit { size_t row; Region region; };

    // The stored thumbnail into the row's raster. Sizes usually agree (the
    // row script makes the raster THUMB_W x THUMB_H); when they do not the
    // picture is sampled nearest rather than refused, so an old store still
    // draws. A page saved before there were thumbnails draws the checker.
    void paint_thumb(ETCS::RID node, int64_t id)
    {
        if (node == 0 || !m_pages) return;
        Pixels_* dst = ETCS::resolve_in_family<Pixels_>("Pixels", node);
        if (!dst) return;
        uint8_t* out = dst->PixelData();
        if (!out) return;
        const int32_t tw = static_cast<int32_t>(dst->PixelWidth());
        const int32_t th = static_cast<int32_t>(dst->PixelHeight());
        if (tw <= 0 || th <= 0) return;

        int32_t sw = 0, sh = 0;
        std::vector<uint8_t> src;
        const bool have = m_pages->Thumb(id, sw, sh, src);
        for (int32_t y = 0; y < th; ++y)
            for (int32_t x = 0; x < tw; ++x)
            {
                uint8_t* dp = out + (static_cast<size_t>(y) * tw + x) * 4;
                if (have)
                {
                    const int32_t sx = std::min(sw - 1, x * sw / tw), sy = std::min(sh - 1, y * sh / th);
                    const uint8_t* sp = src.data() + (static_cast<size_t>(sy) * sw + sx) * 4;
                    dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2]; dp[3] = 255;
                }
                else
                {
                    const bool light = (((x >> 2) + (y >> 2)) & 1) != 0;
                    dp[0] = dp[1] = dp[2] = light ? 112 : 87; dp[3] = 255;
                }
            }
        etcs_mark_observed(dst);
    }

    void page_changed()
    {
        if (m_menu == 0) return;
        if (ETCS::Entity* e = paint_resolve_tag("PaintCanvasMenu", m_menu))
        {
            ETCS::Buffer act; act.write("PaintCanvasMenu.PageChanged");
            ETCS::Buffer arg;
            try { e->call(act, arg); } catch (...) {}
        }
    }

    // The field opens on the name the page has, not empty: a rename is an
    // edit of it, and a name that vanishes at the first press reads as lost.
    void begin_edit(int64_t id)
    {
        if (m_editing) end_edit(false);
        m_renaming = id;
        m_editing  = true;
        m_edit.clear();
        std::vector<PaintPages::PageInfo> pages;
        if (m_pages) m_pages->Pages(pages);
        for (const auto& p : pages) if (p.id == id) { m_edit = p.name; break; }
        Refresh();
    }

    void end_edit(bool keep)
    {
        if (!m_editing) { m_renaming = 0; return; }
        const int64_t id = m_renaming;
        const std::string text = m_edit;
        m_editing = false;
        m_edit.clear();
        m_renaming = 0;
        if (keep && m_pages && id != 0 && !text.empty()) m_pages->Rename(id, text);
        Refresh();
    }

    PaintPages* m_pages  = nullptr;
    ETCS::RID   m_menu   = 0;
    ETCS::RID   m_window = 0;
    std::vector<Row>  m_rows;
    std::vector<int64_t> m_ids;             // newest first -- what the rows window onto
    std::unordered_map<ETCS::RID, Hit> m_regions;
    int32_t     m_scroll   = 0;
    bool        m_editing  = false;
    int64_t     m_renaming = 0;
    std::string m_edit;
    float m_row_sel[3]  = { 0.35f, 0.55f, 0.95f };
    float m_row_idle[3] = { 0.15f, 0.16f, 0.11f };
};

#endif // PAINTPROVIDER_PAINTPAGEPANEL_H__
