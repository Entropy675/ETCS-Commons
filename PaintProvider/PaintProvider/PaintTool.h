#ifndef PAINTPROVIDER_PAINTTOOL_H__
#define PAINTPROVIDER_PAINTTOOL_H__

#include "../../../core_defs.h"
#include "../../../ontology.h"
#include "PaintModifierKeys.h"   // in order: everything above this in the module is visible here

class PaintTool : public DeletableBase<PaintTool>
{
public:
    WIRE_TYPE_IDENTITY(PaintTool);

    PaintTool() = default;
    bool DeleteConcrete() override { return true; }

    void SetKind(const std::string& name)
    {
        m_kind = paint_tool_kind_from(name);
        SetMotionCoalesceMs(paint_tool_default_coalesce_ms(m_kind));
    }
    PaintToolKind kind() const { return m_kind; }

    /*
 * WHAT THE GLYPH TOOL PLACES, held on the tool rather than passed at the click.
 *
 * A press carries a position and nothing else (ontology/InputSource.h), so the
 * text has to be somewhere already -- and the tool is where every other thing a
 * mark is made OF already lives, beside the colour and the radius. Set it, then
 * click; the same string lands again at the next click, which is what you want
 * when placing a label in several places and is trivially overridden when you
 * do not.
 */
    void SetText(const std::string& text) { m_text = text; }
    const std::string& text() const { return m_text; }

    void SetTextSize(uint32_t px) { m_text_px = (px == 0) ? 1u : px; }
    uint32_t textSize() const { return m_text_px; }

    // FILL TOLERANCE, in 0..255 per channel. 0 means "exactly this colour",
    // which is right for flat art and useless on anything antialiased -- the
    // edge pixels of a stroke are neither the stroke's colour nor the paper's,
    // so a zero-tolerance fill stops one pixel short and leaves a halo.
    // The wand reads the same number, so what it selects is what a fill at the
    // same point would have painted.
    void SetTolerance(uint32_t tolerance) { m_tolerance = std::min(tolerance, 255u); }
    uint32_t tolerance() const { return m_tolerance; }

    // HOW THE SELECT TOOL DRAWS ITS BOUNDARY. On the tool beside the tolerance
    // and the text, because it is one more thing a gesture is made OF; the
    // other kinds carry it unread, as smudge carries a colour. See
    // PaintSelectMode.
    void SetMode(const std::string& name) { m_mode = paint_select_mode_from(name); }
    void CycleMode() { m_mode = paint_select_mode_next(m_mode); }
    PaintSelectMode mode() const { return m_mode; }
    // The shape tool's, kept separately: switching tools must not forget which
    // outline the other one was on.
    void SetShape(const std::string& name) { m_shape = paint_shape_mode_from(name); }
    void CycleShape() { m_shape = paint_shape_mode_next(m_shape); }
    PaintShapeMode shape() const { return m_shape; }

    /*
     * WHICH NIB IS LOADED, on the brush rather than beside it, because the two
     * things a tip decides are already brush state (PaintTipMode). Setting the
     * tip sets both, so the pair cannot drift; SetBlendMode still reaches blend
     * alone, for the stylus-that-erases a toolbar arrow has no room to offer.
     */
    void SetTip(const std::string& name) { applyTip(paint_tip_mode_from(name)); }
    void CycleTip()
    {
        applyTip(static_cast<PaintTipMode>(
            (static_cast<uint8_t>(m_brush.tip) + 1) % 3));
    }
    PaintTipMode tip() const { return m_brush.tip; }

    void SetRadius(float radius)
    {
        m_brush.size_px = std::max(1.0f, radius);
    }

    /*
 * SETTING A COLOUR DOES NOT SET THE OPACITY IT IS LAID DOWN AT.
 *
 * Alpha is held separately from the swatch it came with, because the two are
 * chosen at different moments and by different controls: a palette press or a
 * wheel pick says WHICH colour, the alpha control says how strongly. Letting a
 * swatch carry alpha through would reset the strength every time somebody
 * changed colour, which is the opposite of what a strength control is for.
 *
 * So a picked colour keeps its rgb and takes the tool's current alpha. A caller
 * that genuinely means "this colour at this opacity" -- an eyedropper restoring
 * a sampled pixel, say -- uses SetColorWithAlpha.
 */
    void SetColor(float r, float g, float b, float a)
    {
        (void)a;
        m_brush.color = PaintColor{r, g, b, m_alpha};
    }

    void SetColorWithAlpha(float r, float g, float b, float a)
    {
        m_alpha = std::clamp(a, 0.0f, 1.0f);
        m_brush.color = PaintColor{r, g, b, m_alpha};
    }

    /*
 * OPACITY AS A WHOLE PERCENT, because that is the unit the control reads in and
 * the readout shows -- keeping it as a float here and rounding at the label
 * would let the number drift off the value actually in use.
 *
 * WHERE IT APPLIES: every tool that lays down the tool's colour -- brush, line,
 * rect, ellipse, fill, glyph. Smudge carries no colour of its own, so it is
 * unaffected, which is what "if applicable" amounts to: this sets one field, and
 * the tools that do not read that field do not change.
 */
    void SetAlphaPercent(int32_t pct)
    {
        const int32_t c = (pct < 0) ? 0 : (pct > 100 ? 100 : pct);
        m_alpha_pct = c;
        m_alpha = static_cast<float>(c) / 100.0f;
        m_brush.color.a = m_alpha;
    }
    void AdjustAlphaPercent(int32_t delta) { SetAlphaPercent(m_alpha_pct + delta); }
    int32_t alphaPercent() const { return m_alpha_pct; }

    // How often continuous pointer motion may rebuild the view (ms).
    void SetMotionCoalesceMs(double ms)
    {
        m_motion_coalesce_ms = (ms < 1.0) ? 1.0 : ms;
    }
    void AdjustMotionCoalesceMs(double delta_ms)
    {
        SetMotionCoalesceMs(m_motion_coalesce_ms + delta_ms);
    }
    double motionCoalesceMs() const { return m_motion_coalesce_ms; }

    void SetHardness(float hardness)
    {
        m_brush.hardness = std::clamp(hardness, 0.0f, 1.0f);
    }

    void SetBlendMode(const std::string& mode)
    {
        if (mode == "multiply") m_brush.blend = PaintBlendMode::Multiply;
        else if (mode == "screen") m_brush.blend = PaintBlendMode::Screen;
        else if (mode == "erase") m_brush.blend = PaintBlendMode::Erase;
        else m_brush.blend = PaintBlendMode::Normal;
    }

    void BeginStroke(int32_t x, int32_t y)
    {
        m_active = true;
        m_points.clear();
        m_points.push_back(PaintStrokePoint{x, y, 255});
    }

    void MoveStroke(int32_t x, int32_t y)
    {
        if (!m_active) return;
        m_points.push_back(PaintStrokePoint{x, y, 255});
    }

    void EndStroke()
    {
        m_active = false;
        // Keep last stroke points until the next Begin so a document-side
        // commit pass can still read them if needed; clear on next Begin.
    }

    void CancelStroke()
    {
        m_active = false;
        m_points.clear();
    }

    const PaintBrushState& brush() const { return m_brush; }
    bool active() const { return m_active; }
    const std::vector<PaintStrokePoint>& points() const { return m_points; }

    // Where an anchored gesture started. Kept here rather than on the input
    // edge because it is part of what the TOOL is doing -- a line is defined by
    // its anchor, and an input that owned it would be holding one tool's state
    // on behalf of all of them.
    int32_t anchorX() const { return m_points.empty() ? 0 : m_points.front().x; }
    int32_t anchorY() const { return m_points.empty() ? 0 : m_points.front().y; }

private:
    // The tip's two halves, set together (SetTip). Erase is the disc with the
    // blend on; every other tip leaves the blend Normal, which is what makes
    // stepping back out of the eraser put the ink back.
    void applyTip(PaintTipMode t)
    {
        m_brush.tip = t;
        m_brush.blend = (t == PaintTipMode::Erase) ? PaintBlendMode::Erase
                                                   : PaintBlendMode::Normal;
    }

    PaintBrushState m_brush;
    PaintToolKind m_kind = PaintToolKind::Brush;
    std::string m_text = "Text";
    uint32_t m_text_px = 16;
    double m_motion_coalesce_ms = PAINT_MOTION_COALESCE_DEFAULT_MS;
    // Opacity, kept as the percent the control speaks in plus the float the
    // brush needs -- see SetAlphaPercent. Fully opaque until somebody says not.
    int32_t m_alpha_pct = 100;
    float   m_alpha     = 1.0f;
    uint32_t m_tolerance = 24;
    PaintSelectMode m_mode = PaintSelectMode::Rect;
    PaintShapeMode  m_shape = PaintShapeMode::Rect;
    bool m_active = false;
    std::vector<PaintStrokePoint> m_points;
};

#endif // PAINTPROVIDER_PAINTTOOL_H__
