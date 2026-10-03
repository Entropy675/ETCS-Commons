#ifndef PAINTPROVIDER_PAINTTEXTBAR_H__
#define PAINTPROVIDER_PAINTTEXTBAR_H__

#include "../../../core_defs.h"
#include "../../../ontology.h"
#include "PaintAnimation.h"   // in order: everything above this in the module is visible here

/*
 * ── THE TEXT BAR ─────────────────────────────────────────────────────────────
 *
 * What opens over a text box while it is open: its font, its size, its colour,
 * and the two ways out -- `ok`, which ends the edit, and `x`, which removes the
 * box. The bar being up IS the box being open, which in a shared session IS
 * the box being claimed (PaintDocument::SelectTextBox): nothing here decides
 * any of that, it only follows the document's answer.
 *
 * THE LOOK IS THE SCRIPT'S (paint_textbar.etcs) and every control is a palette
 * call naming a verb here with the node pressed, as in the other windows. A
 * press on the bar is the palette's, so it never reaches the canvas and never
 * counts as the press elsewhere that would close the box.
 *
 * IT FOLLOWS THE BOX: above it, or below when the box is at the top of the
 * view, kept inside the view. Placed whenever the view is repainted
 * (PaintInput::repaint_view) and checked on the frame edge too (Animated), for
 * the changes that arrive without a stroke -- a claim refused, an undo.
 */
class PaintTextBar : public DeletableBase<PaintTextBar>,
                     public AnimatedBase<PaintTextBar>
{
public:
    WIRE_TYPE_IDENTITY(PaintTextBar);

    PaintTextBar() = default;
    bool DeleteConcrete() override { return true; }

    bool Create(ETCS::RID document)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintDocument", document);
        if (!raw) { ETCS_LOG("PaintTextBar", "Create: RID:" << document << " is not a PaintDocument."); return false; }
        m_document = static_cast<PaintDocument*>(raw->getTrueType());
        this->addTag("active");
        return true;
    }

    void BindSurface(ETCS::RID surface)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintSurface", surface);
        if (raw) m_surface = static_cast<PaintSurface*>(raw->getTrueType());
    }
    void BindWindow(ETCS::RID pane) { m_window = pane; paint_node_hidden(pane, true); }

    // A button meaning a font (its plate, and the word on it: both register).
    void BindFont(ETCS::RID node, uint32_t font) { if (node) m_fonts[node] = font; }
    void BindSizeLabel(ETCS::RID node) { m_size_label = node; }
    void BindColor(ETCS::RID node, float r, float g, float b)
    {
        if (!node) return;
        m_colors[node] = { r, g, b };
        paint_node_fill(node, r, g, b, 1.0f);
    }
    void SetFontTints(float r0, float g0, float b0, float r1, float g1, float b1)
    {
        m_idle[0] = r0; m_idle[1] = g0; m_idle[2] = b0;
        m_lit[0] = r1;  m_lit[1] = g1;  m_lit[2] = b1;
    }

    // ── what the controls do ─────────────────────────────────────────────
    void PickFont(ETCS::RID node)
    {
        auto it = m_fonts.find(node);
        if (it == m_fonts.end() || !m_document) return;
        m_document->SetTextFont(m_document->selectedTextBox(), it->second);
        changed();
    }
    void StepSize(int32_t by)
    {
        if (!m_document) return;
        const PaintTextBox* b = m_document->FindTextBox(m_document->selectedTextBox());
        if (!b) return;
        static const uint32_t STEPS[] = { 8, 10, 12, 14, 16, 18, 20, 24, 28, 32, 36, 40, 48, 56, 64,
                                          72, 80, 96, 112, 128, 160, 192, 240, 300, 400 };
        const size_t n = sizeof(STEPS) / sizeof(STEPS[0]);
        size_t at = 0;
        while (at + 1 < n && STEPS[at] < b->size) ++at;          // the step at or above now
        if (by > 0) at = (STEPS[at] > b->size) ? at : std::min(n - 1, at + 1);
        else        at = (at == 0) ? 0 : at - 1;
        m_document->SetTextSize(b->id, STEPS[at]);
        changed();
    }
    void PickColor(ETCS::RID node)
    {
        auto it = m_colors.find(node);
        if (it == m_colors.end() || !m_document) return;
        m_document->SetTextColor(m_document->selectedTextBox(), it->second.r, it->second.g, it->second.b, 1.0f);
        changed();
    }
    void Done()
    {
        if (!m_document) return;
        m_document->SelectTextBox(0);
        changed();
    }
    void Remove()
    {
        if (!m_document) return;
        m_document->RemoveTextBox(m_document->selectedTextBox());
        changed();
    }

    /*
     * WHERE THE BAR GOES, and whether it shows. Only a pane that moved or a
     * style that changed costs a verb: the answer is kept and compared.
     */
    void Follow()
    {
        if (!m_document || !m_surface || !m_window) return;
        const PaintTextBox* b = m_document->FindTextBox(m_document->selectedTextBox());
        const bool want = (b != nullptr) && !m_document->readOnly();
        if (!want)
        {
            if (m_shown) { paint_node_hidden(m_window, true); m_shown = false; m_for = 0; }
            return;
        }
        Rect2D view{ 0, 0, 0, 0 }, bar{ 0, 0, 0, 0 };
        {
            ETCS::Held<Drawable2D_> v = ETCS::resolve_held<Drawable2D_>("Drawable2D", m_surface->target());
            if (!v) return;
            view = v->Bounds();
        }
        {
            ETCS::Held<Drawable2D_> w = ETCS::resolve_held<Drawable2D_>("Drawable2D", m_window);
            if (!w) return;
            bar = w->Bounds();
        }
        const int32_t bx = view.x + m_surface->DocToViewX(b->x);
        const int32_t top = view.y + m_surface->DocToViewY(b->y);
        const int32_t bottom = view.y + m_surface->DocToViewY(b->y + b->h);
        int32_t y = top - static_cast<int32_t>(bar.h) - 4;
        if (y < view.y) y = bottom + 4;                           // no room above: under it
        const int32_t x = std::clamp(bx, view.x, std::max(view.x, view.x + static_cast<int32_t>(view.w) - static_cast<int32_t>(bar.w)));
        y = std::clamp(y, view.y, std::max(view.y, view.y + static_cast<int32_t>(view.h) - static_cast<int32_t>(bar.h)));
        if (!m_shown) { paint_node_hidden(m_window, false); m_shown = true; }
        if (x != m_x || y != m_y) { paint_node_moved(m_window, x, y); m_x = x; m_y = y; }
        if (b->id != m_for || b->font != m_font || b->size != m_size)
        {
            m_for = b->id; m_font = b->font; m_size = b->size;
            for (const auto& [node, font] : m_fonts)
            {
                const float* c = (font == b->font) ? m_lit : m_idle;
                paint_node_fill(node, c[0], c[1], c[2], 1.0f);
            }
            paint_node_text(m_size_label, std::to_string(b->size));
        }
    }

    // The frame edge asks every frame; the answer is whether the bar is out of
    // step with the document, which is cheap to tell.
    bool AnimatingConcrete() override
    {
        if (!m_document) return false;
        const uint32_t sel = m_document->selectedTextBox();
        return (sel != 0) != m_shown || (sel != 0 && sel != m_for);
    }
    void AdvanceConcrete(double) override { Follow(); }

private:
    struct Rgb { float r, g, b; };

    // A control changed the box: show it, and put the bar where it now goes.
    void changed()
    {
        if (m_surface) m_surface->Render();
        Follow();
    }

    PaintDocument* m_document = nullptr;
    PaintSurface*  m_surface  = nullptr;
    ETCS::RID m_window = 0, m_size_label = 0;
    std::unordered_map<ETCS::RID, uint32_t> m_fonts;
    std::unordered_map<ETCS::RID, Rgb>      m_colors;
    float m_idle[3] = { 0.17f, 0.18f, 0.13f };
    float m_lit[3]  = { 0.35f, 0.55f, 0.95f };
    bool     m_shown = false;
    int32_t  m_x = INT32_MIN, m_y = INT32_MIN;
    uint32_t m_for = 0, m_font = UINT32_MAX, m_size = 0;
};

#endif // PAINTPROVIDER_PAINTTEXTBAR_H__
