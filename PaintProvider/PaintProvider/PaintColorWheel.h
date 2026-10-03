#ifndef PAINTPROVIDER_PAINTCOLORWHEEL_H__
#define PAINTPROVIDER_PAINTCOLORWHEEL_H__

#include "../../../core_defs.h"
#include "../../../ontology.h"
#include "PaintLayerPanel.h"   // in order: everything above this in the module is visible here

/*
 * ── PaintColorWheel ────────────────────────────────────────────────────────
 *
 * A REGION WHERE POSITION MEANS COLOUR.
 *
 * The palette maps a NODE to a colour -- one swatch, one answer, fixed when the
 * toolbar was written. A wheel maps a POINT to one, which is a different kind of
 * mapping and the reason this is its own type rather than another PaintPalette
 * entry: there is no node per colour and there could not be, because the answer
 * is continuous.
 *
 * It still owns no pixels. The disc is drawn by whatever script lays it out, and
 * what is stated here is only the geometry the pick is computed against -- so a
 * square picker, a strip, or somebody else's gradient all work by giving this
 * the same centre and radius they drew with.
 *
 * HSV BY ANGLE AND DISTANCE, which is the one convention worth having: hue runs
 * round, saturation runs out from the middle, and value is a separate control
 * because a disc has only two dimensions and pretending otherwise gives you a
 * picker that cannot reach dark colours.
 *
 * OPEN MEANS "IN THE ROUTING SET", and that is the whole of showing and hiding.
 * A wheel drawn but not routed would still swallow clicks -- it is a drawable
 * sitting over the canvas, and PickAt does not care whether anybody meant it to
 * be visible. So opening ADDS the pane to the router and closing REMOVES it,
 * which makes "is it open" and "does it receive clicks" the same fact rather
 * than two that can disagree.
 */
/*
 * WHAT A PRESS ON THE WHEEL WAS. Three answers, because the caller has three
 * different things to do with them and a bool could only carry two.
 *
 *   Missed    not the picker at all -- the popup's backing. Dismiss.
 *   Adjusted  the value strip: the disc just changed brightness. STAY OPEN,
 *             because nobody sets the brightness in order to stop choosing.
 *   Picked    a colour. Take it and dismiss.
 */
enum class PaintPick : uint8_t { Missed, Adjusted, Picked };

class PaintColorWheel : public DeletableBase<PaintColorWheel>
{
public:
    WIRE_TYPE_IDENTITY(PaintColorWheel);

    PaintColorWheel() = default;
    bool DeleteConcrete() override { return true; }

    // Centre and radius in the wheel root's own space -- the space a pick
    // arrives in, and the space the script laid the disc out in.
    bool Create(int32_t cx, int32_t cy, uint32_t radius)
    {
        m_cx = cx; m_cy = cy;
        m_radius = (radius == 0) ? 1u : radius;
        m_open = false;
        this->addTag("active");
        return true;
    }

    /*
 * ── THE DISC IS PAINTED, NOT ASSEMBLED ───────────────────────────────────
 *
 * It used to be twelve polygons in a script, and the number twelve was the
 * problem: the PICK was continuous -- computed from the angle and the distance --
 * while the PICTURE was twelve flat wedges, so the colour you got was almost
 * never the colour you clicked on. A picker whose output does not match its own
 * appearance is not a picker, it is a guess with a legend.
 *
 * So the pane's own raster IS the spectrum. Hue around, saturation outward,
 * brightness from the strip down the right edge, and every pixel written from
 * exactly the conversion Pick will run on the coordinates of that pixel. What
 * you click is what you get, by construction rather than by agreement.
 *
 * WHY IT CAN BE A ONE-SHOT WRITE rather than a drawable that redraws per frame:
 * the pane is a retained compositor, so nothing clears it, and the picture only
 * depends on the value. Painted when it is created and again whenever the value
 * changes, which is every moment it could have become wrong.
 *
 * ~50k pixels of trigonometry, once per open. A per-frame version of this would
 * be the wrong shape twice over: it would cost that every frame, and it would
 * make a popup that is not moving into an animating node.
 */
    void PaintDisc()
    {
        Pixels_* px = ETCS::resolve_in_family<Pixels_>("Pixels", m_root);
        if (!px) return;
        uint8_t* d = px->PixelData();
        if (!d) return;

        const int32_t w = static_cast<int32_t>(px->PixelWidth());
        const int32_t h = static_cast<int32_t>(px->PixelHeight());
        m_pane_h = h;
        const uint32_t stride = px->PixelStride();
        const double PI = 3.14159265358979323846;
        const double rad = static_cast<double>(m_radius);

        for (int32_t y = 0; y < h; ++y)
        {
            uint8_t* row = d + static_cast<size_t>(y) * stride;
            for (int32_t x = 0; x < w; ++x)
            {
                uint8_t* p = row + static_cast<size_t>(x) * 4;
                float r = 0, g = 0, b = 0, a = 1.0f;

                if (in_pick_button(x, y))
                {
                    // A pale tab with a dark dropper glyph: a bar with a bulb at
                    // its end, drawn as two rectangles. Legible at 8 px, and
                    // distinct from every disc colour by being nearly grey.
                    const int32_t bx = x - 6, by = y - (m_pane_h - 18);
                    const bool bar  = (bx >= 8 && bx < 24 && by >= 6 && by < 8);
                    const bool bulb = (bx >= 24 && bx < 30 && by >= 4 && by < 10);
                    if (bar || bulb) { r = 0.12f; g = 0.12f; b = 0.14f; }
                    else             { r = 0.86f; g = 0.86f; b = 0.84f; }
                }
                else if (in_value_strip(x, y))
                {
                    // Black at the bottom, the pure hue-plane brightness at the
                    // top: the axis the disc cannot show, because a disc has two
                    // dimensions and a colour has three.
                    const float v = strip_value_at(y);
                    r = g = b = v;
                }
                else
                {
                    const double dx = x - m_cx, dy = y - m_cy;
                    const double dist = std::sqrt(dx * dx + dy * dy);
                    if (dist <= rad)
                    {
                        double ang = std::atan2(dy, dx);
                        if (ang < 0) ang += 2.0 * PI;
                        hsv_to_rgb(static_cast<float>(ang / (2.0 * PI)),
                                   static_cast<float>(dist / rad), m_value, r, g, b);
                    }
                    else
                    {
                        // The popup's own backing, so the pane needs no separate
                        // rectangle behind the disc -- one writer per buffer.
                        r = 0.10f; g = 0.10f; b = 0.13f; a = 0.94f;
                    }
                }
                p[0] = paint_to_byte(r); p[1] = paint_to_byte(g);
                p[2] = paint_to_byte(b); p[3] = paint_to_byte(a);
            }
        }
        etcs_mark_observed(px);
    }

    void BindTool(ETCS::RID tool)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintTool", tool);
        if (raw) m_tool = static_cast<PaintTool*>(raw->getTrueType());
    }

    void BindPalette(ETCS::RID palette)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintPalette", palette);
        if (raw) m_palette = static_cast<PaintPalette*>(raw->getTrueType());
    }

    // The view this popup is over. Kept for the scripts that bind it; nothing
    // here asks it to draw any more -- a move or a close is a change to the
    // tree, and the compose walk redraws what the pane was covering.
    void BindSurface(ETCS::RID surface)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintSurface", surface);
        if (raw) m_surface = static_cast<PaintSurface*>(raw->getTrueType());
    }

    // The router this wheel appears in, and the pane it appears as. Held by RID
    // because opening and closing are calls on somebody else's entity.
    void BindRouter(ETCS::RID router) { m_router = router; }
    // Painted here rather than in Create, because Create runs before a script
    // has said which pane this is -- and the disc needs the pane's pixels.
    void BindPane(ETCS::RID root, ETCS::RID input)
    {
        m_root = root; m_input = input;
        PaintDisc();
    }

    // Repaints, because the disc IS the value: leaving the picture behind after
    // changing what it means is the bug this whole rework is about.
    void SetValue(float v)
    {
        m_value = std::clamp(v, 0.0f, 1.0f);
        PaintDisc();
    }

    bool open() const { return m_open; }

    void Open()
    {
        if (m_open || m_router == 0 || m_root == 0) return;
        if (ETCS::Entity* raw = paint_resolve_tag("PaintRouter", m_router))
            route_add(raw);
        // Open is ONE fact: in the routing set and drawn. It used to be only the
        // first, because this pane was never drawn at all -- reparented so it is,
        // a popup that did not hide itself would sit over the middle of the
        // picture permanently. See ontology/DrawableBase.h.
        set_pane_hidden(false);
        // Cheap insurance: a pane resized or cleared by anything else since the
        // last open would otherwise show a stale or empty picker.
        PaintDisc();
        m_open = true;
        ETCS_LOG("PaintColorWheel", "opened over the canvas.");
    }

    /*
 * OPEN ABOVE A POINT, FOR A NAMED SWATCH.
 *
 * (x, y) is the top-left of the control that asked, in root space, and the disc
 * is placed so its BOTTOM edge sits just above that -- a popup belongs over the
 * thing that summoned it, and a toolbar at the bottom of the window means "over"
 * is upward. The pane carries the wedges with it, because they are its children
 * and their coordinates are its own (paint_wheel.etcs says so); nothing is
 * recomputed here but the pane's position.
 *
 * Clamped to the origin so a control near the top edge opens partly over itself
 * rather than off-screen, where it would be unreachable and look like a control
 * that does nothing.
 */
    void OpenAt(int32_t x, int32_t y, ETCS::RID slot)
    {
        m_target = slot;
        if (m_root != 0)
        {
            const int32_t gap = 8;
            int32_t px = x - m_cx;
            int32_t py = y - gap - m_cy - static_cast<int32_t>(m_radius);
            if (px < 0) px = 0;
            if (py < 0) py = 0;
            move_pane(px, py);
            ETCS_LOG("PaintColorWheel", "pane to " << px << "," << py
                     << " (asked above " << x << "," << y << ") for swatch RID:" << slot);
        }
        Open();
    }

    // Which swatch the next pick replaces, when something more specific than
    // "the last one selected" is known. 0 restores the old behaviour.
    void SetTargetSlot(ETCS::RID slot) { m_target = slot; }

    void Close()
    {
        if (!m_open || m_router == 0) return;
        if (ETCS::Entity* raw = paint_resolve_tag("PaintRouter", m_router))
            route_remove(raw);
        m_open = false;
        set_pane_hidden(true);
        ETCS_LOG("PaintColorWheel", "closed.");
    }

    /*
 * A PICK, in the wheel's own space.
 *
 * Outside the disc is not a colour and is not an error either -- it is a click
 * on the wheel's backing panel, which dismisses. Returning false there lets the
 * caller treat "missed the disc" the same way it treats "clicked the canvas",
 * which is what makes the wheel feel like a popup rather than a trap.
 */
    /*
 * ── the eyedropper ───────────────────────────────────────────────────────
 *
 * A BUTTON IN THE WHEEL'S OWN RASTER, like the value strip, because the wheel
 * paints its pane and picks by position -- a node for it would be the one
 * thing in this pane that is not read the way everything else here is. Pressing
 * it closes the wheel and puts the tool into Eyedrop; the next press on the
 * picture samples what is seen there (PaintDocument::SampleAt) and hands it to
 * Apply, which is the same door a disc pick goes through, so the aimed swatch
 * takes it too. Then the previous kind comes back (EndPick).
 */
    void BeginPick()
    {
        if (!m_tool) return;
        m_kind_before_pick = m_tool->kind();
        m_tool->SetKind(paint_tool_kind_name(PaintToolKind::Eyedrop));
        Close();
        ETCS_LOG("PaintColorWheel", "eyedropper: press the picture to take its colour");
    }
    void EndPick()
    {
        if (!m_tool) return;
        if (m_tool->kind() == PaintToolKind::Eyedrop)
            m_tool->SetKind(paint_tool_kind_name(m_kind_before_pick));
    }

    // The bottom-left corner of the pane, clear of the disc: 40x14 at (6, h-18).
    bool in_pick_button(int32_t x, int32_t y) const
    {
        return x >= 6 && x < 46 && y >= m_pane_h - 18 && y < m_pane_h - 4;
    }

    PaintPick Pick(int32_t x, int32_t y)
    {
        if (in_pick_button(x, y)) { BeginPick(); return PaintPick::Picked; }
        // The strip first: it overlaps nothing, and a press in it is a change of
        // brightness rather than a choice of colour, so it must not dismiss.
        if (in_value_strip(x, y))
        {
            SetValue(strip_value_at(y));
            ETCS_LOG("PaintColorWheel", "value -> " << m_value);
            return PaintPick::Adjusted;
        }

        const double dx = x - m_cx, dy = y - m_cy;
        const double dist = std::sqrt(dx * dx + dy * dy);
        if (dist > m_radius) return PaintPick::Missed;

        const double PI = 3.14159265358979323846;
        double ang = std::atan2(dy, dx);              // -PI..PI
        if (ang < 0) ang += 2.0 * PI;
        const float h = static_cast<float>(ang / (2.0 * PI));
        const float sat = static_cast<float>(std::min(1.0, dist / m_radius));

        float r = 0, g = 0, b = 0;
        // THE SAME CONVERSION PaintDisc RAN FOR THIS PIXEL, on the same inputs.
        // That identity is the whole point -- see PaintDisc.
        hsv_to_rgb(h, sat, m_value, r, g, b);
        Apply(r, g, b, 1.0f);
        return PaintPick::Picked;
    }

    /*
 * WHAT A PICKED COLOUR DOES: it becomes the tool's, and it REPLACES the palette
 * slot the user last selected from.
 *
 * Replacing the slot is the half that makes the wheel worth having -- otherwise
 * a picked colour lasts until the next swatch press and is unrecoverable. The
 * swatch's own fill is not changed here, because a palette holds no drawables
 * (see PaintPalette's header note); the caller that owns the toolbar restyles
 * it, and lastSlot() below is how it knows which one.
 */
    void Apply(float r, float g, float b, float a)
    {
        if (m_tool) m_tool->SetColor(r, g, b, a);
        m_picked[0] = r; m_picked[1] = g; m_picked[2] = b; m_picked[3] = a;
        m_slot = 0;
        if (m_palette)
        {
            // The arrow that opened this wheel named its swatch (OpenAt); only
            // fall back to "the last one selected" when nothing did.
            const ETCS::RID slot = (m_target != 0) ? m_target : m_palette->lastColorNode();
            if (slot != 0 && m_palette->SetColorOf(slot, r, g, b, a)) m_slot = slot;
        }
        ETCS_LOG("PaintColorWheel", "picked " << r << ", " << g << ", " << b
                 << (m_slot ? " -> replaced swatch RID:" : " (no swatch to replace)")
                 << (m_slot ? std::to_string(m_slot) : std::string()));
    }

    // The swatch the last pick replaced, and the colour it was given -- what a
    // script needs to restyle it, since this type cannot.
    ETCS::RID lastSlot() const { return m_slot; }
    float pickedR() const { return m_picked[0]; }
    float pickedG() const { return m_picked[1]; }
    float pickedB() const { return m_picked[2]; }

    void Report() const
    {
        ETCS_LOG("PaintColorWheel", (m_open ? "open" : "closed")
                 << " centre " << m_cx << "," << m_cy << " r=" << m_radius
                 << " value=" << m_value
                 << " last slot RID:" << m_slot);
    }

private:
    /*
 * CALLED BY VERB NAME, not through a PaintRouter*, and deliberately.
 *
 * This type is declared above PaintRouter, so the pointer is not available --
 * but that is a convenience, not the reason. Entity::call is the seam every
 * cross-entity operation in this file already uses, and using it here means a
 * wheel can be routed by anything that answers AddPane/RemovePane rather than
 * by this one concrete arbiter.
 */
    void route_add(ETCS::Entity* router)
    {
        ETCS::Buffer act;  act.write("PaintRouter.AddPane");
        ETCS::Buffer arg;  arg.write((std::to_string(m_root) + " "
                                    + std::to_string(m_input)).c_str());
        try { router->call(act, arg); } catch (...) {}
    }

    void set_pane_hidden(bool hidden)
    {
        paint_node_hidden(m_root, hidden);
    }

    // The pane is somebody else's drawable, so it moves by verb name like
    // everything else this type reaches across.
    void move_pane(int32_t x, int32_t y)
    {
        paint_node_moved(m_root, x, y);
    }

    void route_remove(ETCS::Entity* router)
    {
        ETCS::Buffer act;  act.write("PaintRouter.RemovePane");
        ETCS::Buffer arg;  arg.write(std::to_string(m_root).c_str());
        try { router->call(act, arg); } catch (...) {}
    }

    /*
 * THE VALUE STRIP, down the right edge of the pane.
 *
 * A disc carries two of a colour's three numbers -- hue around, saturation out --
 * and there is nowhere on it for the third. Without somewhere to put brightness
 * the picker offers tints of full-bright hues and no shades at all: no browns, no
 * maroons, nothing dark. That is the "wheel of pre-selected options" complaint in
 * its real form, and it is not fixed by drawing the disc more finely.
 *
 * Geometry derived from the disc's rather than stated separately, so a script
 * that changes Create's centre and radius does not have to know this exists.
 */
    bool in_value_strip(int32_t x, int32_t y) const
    {
        const int32_t x0 = m_cx + static_cast<int32_t>(m_radius) + 8;
        const int32_t y0 = m_cy - static_cast<int32_t>(m_radius);
        const int32_t y1 = m_cy + static_cast<int32_t>(m_radius);
        return x >= x0 && x < x0 + 14 && y >= y0 && y <= y1;
    }

    // Top of the strip is full brightness, bottom is black.
    float strip_value_at(int32_t y) const
    {
        const int32_t y0 = m_cy - static_cast<int32_t>(m_radius);
        const int32_t span = 2 * static_cast<int32_t>(m_radius);
        if (span <= 0) return 1.0f;
        const float t = static_cast<float>(y - y0) / static_cast<float>(span);
        return std::clamp(1.0f - t, 0.0f, 1.0f);
    }

    // Standard sextant conversion. Here rather than in a shared header because
    // it is four lines and this is the only thing in the tree that needs it.
    static void hsv_to_rgb(float h, float s, float v, float& r, float& g, float& b)
    {
        const float i = std::floor(h * 6.0f);
        const float f = h * 6.0f - i;
        const float p = v * (1.0f - s);
        const float q = v * (1.0f - f * s);
        const float t = v * (1.0f - (1.0f - f) * s);
        switch (static_cast<int>(i) % 6)
        {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
        }
    }

    PaintTool*    m_tool    = nullptr;
    PaintPalette* m_palette = nullptr;
    ETCS::RID m_router = 0;
    ETCS::RID m_root   = 0;
    ETCS::RID m_input  = 0;
    int32_t  m_cx = 0, m_cy = 0;
    uint32_t m_radius = 1;
    // The pane's height as last painted: the eyedropper button is placed from
    // the bottom edge, and Pick() has to agree with PaintDisc about where it is.
    int32_t  m_pane_h = 220;
    PaintToolKind m_kind_before_pick = PaintToolKind::Brush;
    float    m_value = 1.0f;
    bool     m_open  = false;
    ETCS::RID m_slot = 0;
    // The swatch an arrow named when it opened this wheel -- see OpenAt. 0 means
    // nothing named one, and the last selected colour entry is the target.
    ETCS::RID m_target = 0;
    PaintSurface* m_surface = nullptr;
    float    m_picked[4] = { 0, 0, 0, 1 };
};

#endif // PAINTPROVIDER_PAINTCOLORWHEEL_H__
