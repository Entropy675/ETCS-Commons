#ifndef RENDERPROVIDER_MENU_H__
#define RENDERPROVIDER_MENU_H__

#include <atomic>
#include "../../../core_defs.h"
#include "../../../ontology.h"
#include "TextLabel.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

/*
 * ── Menu ───────────────────────────────────────────────────────────────────
 *
 * A PANEL OF CONTROLS, DRAWN, FOR WHATEVER OWNS THE GAME. A lobby between
 * rounds is the same few things in every game -- a title, a choice to step
 * through, a row of colours, a name, a button, a picture of what is chosen,
 * who is playing -- so it is one widget here rather than a page of text rows
 * in each game. One entity, one raster (as the Throbber is), with its label a
 * hidden child rasterising text into it.
 *
 * WHAT IT SHOWS IS SAID AS LINES, and the menu keeps no state of the
 * game's: SetLine <n> <text> sets one, SetLines <count> says how many there
 * are, and a game sends only the lines that changed -- an action's payload is
 * one buffer (MAX_TAG_BUFFER_SIZE), so a whole menu is never one call. The
 * picture is redrawn once, on the next frame, however many lines moved.
 * Fields are separated by '|':
 *
 *   title|<text>                         sub|<text>
 *   choice|<id>|<label>|<value>|<on>     a value stepped with < and >
 *   swatches|<id>|<label>|<chosen>|<rrggbb>,<rrggbb>,...|<on>
 *   field|<id>|<label>|<value>|<on>|<editing>
 *   button|<id>|<label>|<on>|<primary>
 *   head|<text>                          row|<rrggbb or ->|<text>
 *   hint|<text>
 *   preview|<x0>|<z0>|<x1>|<z1>          the picture's extent, then
 *   rect|<x>|<z>|<w>|<d>|<rrggbb>        rects and discs in it, by centre
 *   disc|<x>|<z>|<r>|<rrggbb>
 *
 * `on` 0 draws a control dimmed and leaves it unpickable -- a guest sees the
 * host's settings and cannot move them. Controls sit in a left column in the
 * order given; the preview and the table share the right.
 *
 * POINTED AT, NOT LISTENING. The game owns its pointer and its keys; it asks
 * Point(x, y) where a press landed (in the space SetPosition is in) and is
 * told "<id> <part>" -- prev, next, swatch <n>, field, press -- or "-". Focus
 * names the control the keys are on, Hover the one under the pointer; both
 * are drawn, nothing more. Centred in its parent (the camera) every frame it
 * shows, so the window's size is nobody's number to keep.
 *
 * ANIMATED WHILE SHOWN: the focused control's edge breathes and an edited
 * field's caret blinks, on the frame edge that is already running. Hidden,
 * it costs one call a frame.
 */
class Menu : public Drawable2DBase<Menu>,
             public PixelsBase<Menu>,
             public AnimatedBase<Menu>,
             public DeletableBase<Menu>
{
public:
    WIRE_TYPE_IDENTITY(Menu);

    int32_t m_order = 0;
    bool operator<(const Menu& o) const
    { return Hidden() != o.Hidden() ? Hidden() : m_order < o.m_order; }
    int32_t Order() override { return m_order; }

    Menu()  = default;
    ~Menu() = default;

    bool Create(uint32_t w, uint32_t h)
    {
        std::lock_guard<std::mutex> g(m_mu);
        m_w = w ? w : 640; m_h = h ? h : 420;
        ensureLabel();
        this->Allocate(m_w, m_h);
        this->addTag("active");
        repaint();
        return true;
    }
    void SetPosition(int32_t x, int32_t y) { m_x = x; m_y = y; m_center = false; etcs_mark_observed(this); }
    void SetOrder(int32_t z) { m_order = z; this->Reorder(); etcs_mark_observed(this); }
    void SetLine(uint32_t n, const std::string& text)
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (n >= kMaxLines) return;
        if (m_lines.size() <= n) m_lines.resize(n + 1);
        if (m_lines[n] == text) return;
        m_lines[n] = text;
        m_dirty = true;
    }
    void SetLines(uint32_t count)
    {
        std::lock_guard<std::mutex> g(m_mu);
        if (count > kMaxLines) count = kMaxLines;
        if (m_lines.size() == count) return;
        m_lines.resize(count);
        m_dirty = true;
    }
    void Focus(const std::string& id)  { std::lock_guard<std::mutex> g(m_mu); if (id != m_focus) { m_focus = id; m_dirty = true; } }
    void Hover(int32_t x, int32_t y)
    {
        std::lock_guard<std::mutex> g(m_mu);
        const std::string full = hitAt(x - m_x, y - m_y, false);
        const std::string h = full.substr(0, full.find(' '));
        const size_t sw = full.find(" swatch ");
        const int s = sw == std::string::npos ? -1 : std::atoi(full.c_str() + sw + 8);
        if (h != m_hover || s != m_hover_swatch) { m_hover = h; m_hover_swatch = s; m_dirty = true; }
    }
    // What a press at (x, y) of the parent's space is on: "<id> <part>"; "+"
    // on the panel but on nothing of it (the panel's own, not what is behind
    // it); "-" off it, or hidden.
    std::string Point(int32_t x, int32_t y) const
    {
        if (!Inside(x, y)) return "-";
        std::lock_guard<std::mutex> g(m_mu);
        const std::string h = hitAt(x - m_x, y - m_y, false);
        return h.empty() ? "+" : h;
    }
    bool Inside(int32_t x, int32_t y) const
    {
        return !this->Hidden() && x >= m_x && y >= m_y && x < m_x + static_cast<int32_t>(m_w) && y < m_y + static_cast<int32_t>(m_h);
    }

    // ── Animated_ ────────────────────────────────────────────────────────
    bool AnimatingConcrete() override
    {
        if (this->Hidden()) return false;
        centre();
        return true;
    }
    void AdvanceConcrete(double dt_ms) override
    {
        std::lock_guard<std::mutex> g(m_mu);
        m_t += static_cast<float>(dt_ms) * 0.001f;
        if (m_t > 1000.0f) m_t -= 1000.0f;
        const int beat = static_cast<int>(m_t * 8.0f);   // eight looks a second: enough for a breath and a caret
        if (m_dirty) { m_dirty = false; parse(); repaint(); m_beat = beat; return; }
        if (beat != m_beat) { m_beat = beat; repaint(); }
    }

    // ── Drawable2D_ / Surface_ ───────────────────────────────────────────
    Rect2D BoundsConcrete() override { return Rect2D{ m_x, m_y, m_w, m_h }; }
    bool ContainsLocalConcrete(int32_t x, int32_t y) override
    { return x >= 0 && y >= 0 && x < static_cast<int32_t>(m_w) && y < static_cast<int32_t>(m_h); }
    WindowSize GetSizeConcrete() override { return WindowSize{ m_w, m_h }; }
    void ClearConcrete(float r, float g, float b, float a) override { this->ClearTo(r, g, b, a); }
    void DrawRectConcrete(int32_t x, int32_t y, uint32_t w, uint32_t h, float r, float g, float b, float a) override
    { this->FillRect(x, y, w, h, r, g, b, a); }
    void BlitConcrete(Surface_* source, int32_t x, int32_t y, uint32_t w, uint32_t h, float opacity) override
    {
        (void)w; (void)h;
        if (!source) return;
        void* raw = static_cast<ETCS::Entity*>(source)->getInterfacePointer(ETCS::Buffer("Pixels"));
        if (raw) this->Composite(*static_cast<Pixels_*>(raw), x, y, opacity);
    }
    void DrawIntoConcrete(Surface_* dst) override
    {
        if (!dst) return;
        const Point2D base = this->parentAbsoluteOrigin();
        if (Pixels_* raw = etcs_direct_pixels(static_cast<ETCS::Entity*>(dst)))
            render_composite_raw(*raw, this->PixelData(), m_w, m_h, base.x + m_x, base.y + m_y, 1.0f);
        else
            dst->Blit(this, base.x + m_x, base.y + m_y, 0, 0, 1.0f);
    }
    bool DeleteConcrete() override { return true; }
    bool MoveTo(Point2D p) override { SetPosition(p.x, p.y); return true; }

private:
    struct Item
    {
        std::string kind, id, label, value, extra;
        bool on = true, flag = false;
        int chosen = -1;
        std::vector<uint32_t> colours;
    };
    struct Shape { bool disc = false; float x = 0, z = 0, w = 0, d = 0; uint32_t rgb = 0; };
    struct Hit { std::string what; int32_t x, y, w, h; };

    static constexpr int32_t kPad = 18, kRow = 38, kLabelW = 112;

    // The page's own furniture: dark warm ink-on-paper, the Throbber's blue
    // and violet for what is live.
    static constexpr float kBg[4]     = { 0.106f, 0.110f, 0.078f, 0.93f };
    static constexpr float kEdge[4]   = { 0.39f, 0.0f, 0.89f, 1.0f };
    static constexpr float kInk[4]    = { 0.94f, 0.89f, 0.78f, 1.0f };
    static constexpr float kDim[4]    = { 0.62f, 0.60f, 0.52f, 1.0f };
    static constexpr float kAccent[4] = { 0.35f, 0.55f, 0.95f, 1.0f };
    static constexpr float kViolet[4] = { 0.58f, 0.40f, 0.95f, 1.0f };
    static constexpr float kWell[4]   = { 0.16f, 0.17f, 0.13f, 1.0f };
    static constexpr float kSolid[4]  = { 0.106f, 0.110f, 0.078f, 1.0f };   // the panel, opaque

    void ensureLabel()
    {
        if (m_label) return;
        m_label = this->template addTag<TextLabel>();
        if (!m_label) return;
        m_label->Create(TextLabel::CELL_H);
        m_label->SetHidden(true);
    }

    static uint32_t rgbOf(const std::string& s)
    {
        if (s.size() != 6) return 0x808080;
        return static_cast<uint32_t>(std::strtoul(s.c_str(), nullptr, 16));
    }
    static void split(const std::string& line, std::vector<std::string>& f)
    {
        f.clear();
        std::string cur;
        for (char c : line) { if (c == '|') { f.push_back(cur); cur.clear(); } else cur += c; }
        f.push_back(cur);
    }
    void parse()
    {
        m_items.clear(); m_shapes.clear(); m_table.clear();
        m_title.clear(); m_sub.clear(); m_hint.clear(); m_head.clear();
        m_has_preview = false;
        std::vector<std::string> f;
        auto at = [&](size_t i) { return i < f.size() ? f[i] : std::string(); };
        for (const std::string& line : m_lines)
        {
            if (line.empty()) continue;
            split(line, f);
            const std::string& k = f[0];
            if (k == "title") m_title = at(1);
            else if (k == "sub") m_sub = at(1);
            else if (k == "hint") m_hint = at(1);
            else if (k == "head") m_head = at(1);
            else if (k == "row") m_table.push_back({ at(1) == "-" ? 0xFFFFFFFFu : rgbOf(at(1)), at(2) });
            else if (k == "preview")
            {
                m_has_preview = true;
                m_px0 = std::strtof(at(1).c_str(), nullptr); m_pz0 = std::strtof(at(2).c_str(), nullptr);
                m_px1 = std::strtof(at(3).c_str(), nullptr); m_pz1 = std::strtof(at(4).c_str(), nullptr);
            }
            else if (k == "rect" || k == "disc")
            {
                Shape s; s.disc = k == "disc";
                s.x = std::strtof(at(1).c_str(), nullptr); s.z = std::strtof(at(2).c_str(), nullptr);
                s.w = std::strtof(at(3).c_str(), nullptr);
                s.d = s.disc ? s.w : std::strtof(at(4).c_str(), nullptr);
                s.rgb = rgbOf(s.disc ? at(4) : at(5));
                m_shapes.push_back(s);
            }
            else if (k == "choice" || k == "swatches" || k == "field" || k == "button")
            {
                Item it; it.kind = k; it.id = at(1); it.label = at(2);
                if (k == "choice") { it.value = at(3); it.on = at(4) != "0"; }
                else if (k == "swatches")
                {
                    it.chosen = std::atoi(at(3).c_str());
                    std::string c = at(4), one;
                    for (char ch : c + ",") { if (ch == ',') { if (!one.empty()) it.colours.push_back(rgbOf(one)); one.clear(); } else one += ch; }
                    it.on = at(5) != "0";
                }
                else if (k == "field") { it.value = at(3); it.on = at(4) != "0"; it.flag = at(5) == "1"; }
                else { it.on = at(3) != "0"; it.flag = at(4) == "1"; }
                m_items.push_back(it);
            }
        }
    }

    // Centred in the parent, re-read each frame it shows.
    void centre()
    {
        if (!m_center) return;
        ETCS::Entity* p = this->getParent();
        void* d2 = p ? p->getInterfacePointer(ETCS::Buffer("Drawable2D")) : nullptr;
        if (!d2) return;
        const Rect2D b = static_cast<Drawable2D_*>(d2)->Bounds();
        const int32_t x = (static_cast<int32_t>(b.w) - static_cast<int32_t>(m_w)) / 2;
        const int32_t y = (static_cast<int32_t>(b.h) - static_cast<int32_t>(m_h)) / 2;
        if (x != m_x || y != m_y) { m_x = x < 0 ? 0 : x; m_y = y < 0 ? 0 : y; etcs_mark_observed(this); }
    }

    std::string hitAt(int32_t x, int32_t y, bool) const
    {
        for (const Hit& h : m_hits)
            if (x >= h.x && y >= h.y && x < h.x + h.w && y < h.y + h.h) return h.what;
        return std::string();
    }

    // ── drawing ──────────────────────────────────────────────────────────
    void rrect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, const float* c, float a = 1.0f)
    {
        if (w <= 0 || h <= 0) return;
        if (r * 2 > h) r = h / 2;
        if (r * 2 > w) r = w / 2;
        const float al = c[3] * a;
        this->FillRect(x + r, y, static_cast<uint32_t>(w - 2 * r), static_cast<uint32_t>(h), c[0], c[1], c[2], al);
        this->FillRect(x, y + r, static_cast<uint32_t>(r), static_cast<uint32_t>(h - 2 * r), c[0], c[1], c[2], al);
        this->FillRect(x + w - r, y + r, static_cast<uint32_t>(r), static_cast<uint32_t>(h - 2 * r), c[0], c[1], c[2], al);
        if (r <= 0) return;
        // Quarter discs at the corners, spans per row so nothing is drawn twice.
        for (int32_t dy = 0; dy < r; ++dy)
        {
            const float fy = static_cast<float>(r - dy) - 0.5f;
            const int32_t k = static_cast<int32_t>(std::sqrt(std::max(0.0f, static_cast<float>(r * r) - fy * fy)));
            this->FillRect(x + r - k, y + dy, static_cast<uint32_t>(k), 1, c[0], c[1], c[2], al);
            this->FillRect(x + w - r, y + dy, static_cast<uint32_t>(k), 1, c[0], c[1], c[2], al);
            this->FillRect(x + r - k, y + h - 1 - dy, static_cast<uint32_t>(k), 1, c[0], c[1], c[2], al);
            this->FillRect(x + w - r, y + h - 1 - dy, static_cast<uint32_t>(k), 1, c[0], c[1], c[2], al);
        }
    }
    // A ring, drawn BEFORE what it surrounds: its inside is the panel again.
    void outline(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, int32_t t, const float* c, float a = 1.0f)
    {
        rrect(x, y, w, h, r, c, a);
        rrect(x + t, y + t, w - 2 * t, h - 2 * t, r > t ? r - t : 0, kSolid, 1.0f);
    }
    // A triangle pointing left (-1) or right (+1), its middle at (cx, cy).
    void arrow(int32_t cx, int32_t cy, int32_t s, int dir, const float* c, float a)
    {
        for (int32_t dy = -s; dy <= s; ++dy)
        {
            const int32_t len = s - std::abs(dy);
            const int32_t x0 = dir > 0 ? cx - s / 2 : cx + s / 2 - len;
            this->FillRect(x0, cy + dy, static_cast<uint32_t>(len + 1), 1, c[0], c[1], c[2], c[3] * a);
        }
    }
    int32_t textW(const std::string& t, uint32_t scale) const
    {
        return m_label ? static_cast<int32_t>(m_label->MeasureTextConcrete(t.c_str(), 0, scale * TextLabel::CELL_H).width) : 0;
    }
    void text(const std::string& t, int32_t x, int32_t y, uint32_t scale, const float* c, float a = 1.0f)
    {
        if (m_label && !t.empty())
            m_label->RasterizeTextConcrete(this->getRID(), t.c_str(), 0, scale * TextLabel::CELL_H, x, y, c[0], c[1], c[2], c[3] * a);
    }
    // A run cut to fit `w` pixels, with a mark that it was.
    std::string fit(const std::string& t, int32_t w, uint32_t scale) const
    {
        const int32_t cw = static_cast<int32_t>(TextLabel::ADVANCE * scale);
        const int32_t n = cw > 0 ? w / cw : 0;
        if (n <= 0) return std::string();
        if (static_cast<int32_t>(t.size()) <= n) return t;
        return t.substr(0, static_cast<size_t>(n > 1 ? n - 1 : n)) + "~";
    }
    static void rgbf(uint32_t rgb, float out[4])
    {
        out[0] = static_cast<float>((rgb >> 16) & 255) / 255.0f;
        out[1] = static_cast<float>((rgb >> 8) & 255) / 255.0f;
        out[2] = static_cast<float>(rgb & 255) / 255.0f;
        out[3] = 1.0f;
    }

    void repaint()
    {
        if (!m_w || !m_h || !m_label) return;
        etcs_observed_batch _batch(this);
        m_hits.clear();
        const float breath = 0.55f + 0.45f * std::sin(m_t * 4.0f);
        const bool caret = static_cast<int>(m_t * 2.0f) % 2 == 0;

        this->ClearTo(0.0f, 0.0f, 0.0f, 0.0f);
        rrect(0, 0, static_cast<int32_t>(m_w), static_cast<int32_t>(m_h), 12, kEdge, 0.9f);
        rrect(2, 2, static_cast<int32_t>(m_w) - 4, static_cast<int32_t>(m_h) - 4, 10, kBg, 1.0f);

        int32_t y = kPad;
        if (!m_title.empty()) { text(fit(m_title, static_cast<int32_t>(m_w) - 2 * kPad, 3), kPad, y, 3, kInk); y += 30; }
        if (!m_sub.empty())   { text(fit(m_sub, static_cast<int32_t>(m_w) - 2 * kPad, 2), kPad, y, 2, kDim); y += 22; }
        // A rule under the heading, blue into violet.
        const int32_t rule_w = static_cast<int32_t>(m_w) - 2 * kPad;
        for (int32_t i = 0; i < rule_w; i += 4)
        {
            const float t = static_cast<float>(i) / static_cast<float>(rule_w);
            const float c[4] = { kAccent[0] + (kViolet[0] - kAccent[0]) * t, kAccent[1] + (kViolet[1] - kAccent[1]) * t,
                                 kAccent[2] + (kViolet[2] - kAccent[2]) * t, 1.0f };
            this->FillRect(kPad + i, y, 4, 2, c[0], c[1], c[2], 0.9f);
        }
        y += 12;

        const int32_t col_w = m_has_preview || !m_table.empty() ? static_cast<int32_t>(m_w * 56 / 100) - kPad : static_cast<int32_t>(m_w) - 2 * kPad;
        const int32_t right_x = kPad + col_w + kPad, right_w = static_cast<int32_t>(m_w) - right_x - kPad;
        const int32_t top = y;

        for (const Item& it : m_items)
        {
            const float a = it.on ? 1.0f : 0.45f;
            const bool focus = it.on && it.id == m_focus;
            const bool hover = it.on && it.id == m_hover;
            const int32_t cx = kPad + kLabelW, cw = col_w - kLabelW;
            const int32_t h = kRow - 8;
            if (it.kind == "button")
            {
                const float* fill = it.flag ? kAccent : kWell;
                if (focus) outline(kPad - 3, y - 3, col_w + 6, h + 6, 11, 2, kViolet, breath);
                rrect(kPad, y, col_w, h, 8, fill, a * (hover ? 1.0f : 0.85f));
                const std::string l = fit(it.label, col_w - 16, 2);
                text(l, kPad + (col_w - textW(l, 2)) / 2, y + (h - 14) / 2, 2, it.flag ? kBg : kInk, a / (it.flag ? kBg[3] : 1.0f));
                if (it.on) m_hits.push_back({ it.id + " press", kPad, y, col_w, h });
                y += kRow;
                continue;
            }
            text(fit(it.label, kLabelW - 8, 2), kPad, y + (h - 14) / 2, 2, kDim, a);
            if (it.kind == "choice")
            {
                if (focus || hover) outline(cx - 3, y - 3, cw + 6, h + 6, 11, 2, focus ? kViolet : kAccent, focus ? breath : 0.6f);
                rrect(cx, y, cw, h, 8, kWell, a);
                const int32_t bw = h;
                rrect(cx, y, bw, h, 8, kAccent, a * 0.35f);
                rrect(cx + cw - bw, y, bw, h, 8, kAccent, a * 0.35f);
                arrow(cx + bw / 2, y + h / 2, 7, -1, kInk, a);
                arrow(cx + cw - bw / 2, y + h / 2, 7, +1, kInk, a);
                const std::string v = fit(it.value, cw - 2 * bw - 12, 2);
                text(v, cx + (cw - textW(v, 2)) / 2, y + (h - 14) / 2, 2, kInk, a);
                if (it.on)
                {
                    m_hits.push_back({ it.id + " prev", cx, y, bw, h });
                    m_hits.push_back({ it.id + " next", cx + cw - bw, y, bw, h });
                    m_hits.push_back({ it.id + " next", cx + bw, y, cw - 2 * bw, h });
                }
            }
            else if (it.kind == "swatches")
            {
                // As many as there are across the control: the discs shrink to fit, never drop off its end.
                const int32_t n = std::max<int32_t>(1, static_cast<int32_t>(it.colours.size()));
                const int32_t step = std::min<int32_t>(29, (cw - 8) / n), r = std::max<int32_t>(4, std::min<int32_t>(10, (step - 5) / 2));
                if (focus) outline(cx - 4, y - 2, std::min(cw, n * step + 6) + 2, h + 4, 10, 2, kViolet, breath);
                for (size_t i = 0; i < it.colours.size(); ++i)
                {
                    float c[4]; rgbf(it.colours[i], c);
                    const int32_t px = cx + 4 + step / 2 + static_cast<int32_t>(i) * step, py = y + h / 2;
                    if (static_cast<int>(i) == it.chosen)
                    {
                        this->FillDisc(px, py, r + 4, kInk[0], kInk[1], kInk[2], a);
                        this->FillDisc(px, py, r + 2, kBg[0], kBg[1], kBg[2], 1.0f);
                    }
                    else if (hover && m_hover_swatch == static_cast<int>(i))
                        this->FillDisc(px, py, r + 2, kAccent[0], kAccent[1], kAccent[2], a);
                    this->FillDisc(px, py, static_cast<uint32_t>(r), c[0], c[1], c[2], a);
                    if (it.on) m_hits.push_back({ it.id + " swatch " + std::to_string(i), px - step / 2, y, step, h });
                }
            }
            else if (it.kind == "field")
            {
                if (it.flag || focus || hover)
                    outline(cx - 3, y - 3, cw + 6, h + 6, 9, 2, it.flag ? kAccent : kViolet, it.flag ? 1.0f : (focus ? breath : 0.6f));
                rrect(cx, y, cw, h, 6, kWell, a);
                const std::string v = fit(it.value, cw - 24, 2);
                text(v, cx + 10, y + (h - 14) / 2, 2, kInk, a);
                if (it.flag && caret)
                    this->FillRect(cx + 10 + textW(v, 2) + 3, y + 7, 2, static_cast<uint32_t>(h - 14), kInk[0], kInk[1], kInk[2], 1.0f);
                if (it.on) m_hits.push_back({ it.id + " field", cx, y, cw, h });
            }
            y += kRow;
        }

        // The right: the picture, then who is playing.
        int32_t ry = top;
        if (m_has_preview && right_w > 40)
        {
            const int32_t side = std::min(right_w, static_cast<int32_t>(m_h) - top - kPad - 24 - static_cast<int32_t>(m_table.size() + 1) * 18);
            const int32_t pw = std::max(60, side);
            const float sx = m_px1 > m_px0 ? static_cast<float>(pw - 8) / (m_px1 - m_px0) : 1.0f;
            const float sz = m_pz1 > m_pz0 ? static_cast<float>(pw - 8) / (m_pz1 - m_pz0) : 1.0f;
            const float s = std::min(sx, sz);
            const int32_t ox = right_x + (right_w - pw) / 2;
            rrect(ox, ry, pw, pw, 8, kWell, 1.0f);
            const float mx = (m_px0 + m_px1) * 0.5f, mz = (m_pz0 + m_pz1) * 0.5f;
            for (const Shape& sh : m_shapes)
            {
                float c[4]; rgbf(sh.rgb, c);
                const int32_t px = ox + pw / 2 + static_cast<int32_t>(std::lround((sh.x - mx) * s));
                const int32_t pz = ry + pw / 2 + static_cast<int32_t>(std::lround((sh.z - mz) * s));
                if (sh.disc)
                    this->FillDisc(px, pz, static_cast<uint32_t>(std::max(1.0f, sh.w * s)), c[0], c[1], c[2], 1.0f);
                else
                {
                    const int32_t w = std::max(1, static_cast<int32_t>(std::lround(sh.w * s)));
                    const int32_t d = std::max(1, static_cast<int32_t>(std::lround(sh.d * s)));
                    this->FillRect(px - w / 2, pz - d / 2, static_cast<uint32_t>(w), static_cast<uint32_t>(d), c[0], c[1], c[2], 1.0f);
                }
            }
            ry += pw + 10;
        }
        if (!m_head.empty()) { text(fit(m_head, right_w, 2), right_x, ry, 2, kDim); ry += 20; }
        for (const auto& r : m_table)
        {
            if (ry + 16 > static_cast<int32_t>(m_h) - kPad - 20) break;
            int32_t tx = right_x;
            if (r.first != 0xFFFFFFFFu)
            {
                float c[4]; rgbf(r.first, c);
                this->FillDisc(right_x + 6, ry + 6, 6, c[0], c[1], c[2], 1.0f);
                tx += 18;
            }
            text(fit(r.second, right_w - (tx - right_x), 2), tx, ry, 2, kInk);
            ry += 18;
        }
        if (!m_hint.empty())
            text(fit(m_hint, static_cast<int32_t>(m_w) - 2 * kPad, 2), kPad, static_cast<int32_t>(m_h) - kPad - 12, 2, kDim);
    }

    static constexpr uint32_t kMaxLines = 256;

    TextLabel*         m_label = nullptr;
    mutable std::mutex m_mu;           // the lines and the hits: written by the game, read by the frame edge
    std::vector<std::string> m_lines;
    bool               m_dirty = false;
    std::string        m_focus, m_hover;
    int                m_hover_swatch = -1;
    std::string        m_title, m_sub, m_hint, m_head;
    std::vector<Item>  m_items;
    std::vector<Shape> m_shapes;
    std::vector<std::pair<uint32_t, std::string>> m_table;
    std::vector<Hit>   m_hits;
    bool               m_has_preview = false;
    float              m_px0 = 0, m_pz0 = 0, m_px1 = 1, m_pz1 = 1;
    bool               m_center = true;
    int32_t            m_x = 0, m_y = 0;
    uint32_t           m_w = 0, m_h = 0;
    float              m_t = 0.0f;
    int                m_beat = -1;
};

#endif
