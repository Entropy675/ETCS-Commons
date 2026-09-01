#ifndef TEXTLABEL_H__
#define TEXTLABEL_H__

#include "../../core_defs.h"
#include "../../ontology.h"

#include <cstdint>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// TextLabel — the Glyphs leaf, and a Drawable2D so it can simply be nested.
//
// TWO FAMILIES, AND THE PAIRING IS THE POINT. Glyphs answers "what would this
// run occupy" and "put this run there"; Drawable2D answers "I am a node in a
// tree with a position". A label is both, so a caption over a camera view is a
// CHILD of the camera and needs no drawing code anywhere -- the same
// composition a polygon or a nested compositor already gets. Nothing about the
// camera knows text exists.
//
// A BUILT-IN 5x8 FONT, and deliberately not a font file. A font file means a
// path, a loader, a failure mode on a machine that does not have it, and a
// rasteriser -- four things to get wrong before a frame counter can be drawn.
// The table below is 95 glyphs of five bytes each: it cannot fail to load, it
// renders identically everywhere, and it is enough for every label this system
// currently wants. A real typeface is a different provider implementing this
// same family, which is the whole reason the family exists.
//
// SIZE IS AN INTEGER SCALE of the 8-pixel cell, so glyphs stay on the pixel
// grid at every size. Text that is antialiased or hinted is what a real font
// backend brings; a bitmap font that lands between pixels is just blurry, and
// blurry is worse than blocky at these sizes.
//
// IT DRAWS THROUGH Surface_::DrawRect, one rect per run of lit pixels in a
// column, never by touching bytes. So it works into a compositor, a camera, an
// offscreen layer, or straight onto the device surface, with no knowledge of
// which -- and a backend that has no CPU pixels at all still gets its text.
// ---------------------------------------------------------------------------
class TextLabel : public Drawable2DBase<TextLabel>,
                  public GlyphsBase<TextLabel>,
                  public DeletableBase<TextLabel>
{
public:
    WIRE_TYPE_IDENTITY(TextLabel);

    // --- Orderable_ (required by Surface, which Drawable refines) ---
    int32_t m_order = 0;
    bool operator<(const TextLabel& o) const { return m_order < o.m_order; }
    int32_t Order() override { return m_order; }

    TextLabel()  = default;
    ~TextLabel() = default;

    static constexpr uint32_t CELL_W = 5;   // glyph columns
    static constexpr uint32_t CELL_H = 8;   // glyph rows: 7 of face, 1 of descender
    static constexpr uint32_t ADVANCE = CELL_W + 1;   // one blank column between

    bool Create(uint32_t size_px)
    {
        SetSize(size_px);
        this->addTag("active");
        return true;
    }

    // Rounded to a whole multiple of the cell height, and never zero: a
    // bitmap font at a fractional scale is a blurry bitmap font.
    void SetSize(uint32_t size_px)
    {
        uint32_t s = size_px / CELL_H;
        m_scale = s ? s : 1;
        markDirtyPath();
    }
    uint32_t Scale() const { return m_scale; }

    void SetText(const std::string& text) { m_text = text; markDirtyPath(); }
    const std::string& Text() const       { return m_text; }

    void SetPosition(int32_t x, int32_t y) { m_x = x; m_y = y; markDirtyPath(); }
    void SetOrder(int32_t z)               { m_order = z; Reorder(); markDirtyPath(); }
    void SetColor(float r, float g, float b, float a)
    {
        m_color[0] = r; m_color[1] = g; m_color[2] = b; m_color[3] = a;
        markDirtyPath();
    }

    // A slab behind the text, so a caption stays legible over whatever it is
    // drawn on. Zero alpha means none, which is the default -- a label should
    // not decide it needs a background.
    void SetBackground(float r, float g, float b, float a)
    {
        m_bg[0] = r; m_bg[1] = g; m_bg[2] = b; m_bg[3] = a;
        markDirtyPath();
    }
    void SetPadding(uint32_t px) { m_pad = px; markDirtyPath(); }

    /*
 * Report a surface's frame rate instead of a fixed string.
 *
 * THE ONE DYNAMIC BINDING, and it is here rather than in a script because of
 * WHEN the number is true. A frame counter is text whose content is a
 * property of the frame loop, and the only instant both are known is the
 * draw -- a script cannot run per frame, and a work function that pushed the
 * number in would be reporting the rate as of whenever it last ran.
 *
 * So the label formats at DrawInto, from the surface it names. By RID, like
 * every other cross-entity reference here, so the surface may be deleted
 * without the label holding a pointer into it -- an unresolvable source just
 * falls back to the fixed text.
 */
    void BindFps(ETCS::RID surface) { m_fps_src = surface; markDirtyPath(); }
    ETCS::RID FpsSource() const     { return m_fps_src; }

    // ── Glyphs_ dispatch ─────────────────────────────────────────────────

    TextExtent MeasureTextConcrete(const char* text, uint32_t, uint32_t size_px) override
    {
        const uint32_t scale = size_px ? (size_px / CELL_H ? size_px / CELL_H : 1) : m_scale;
        uint32_t n = 0;
        for (const char* p = text; p && *p; ++p) ++n;
        const uint32_t w = n ? (n * ADVANCE - 1) * scale : 0;
        return TextExtent{ w, CELL_H * scale, (CELL_H - 1) * scale };
    }

    TextExtent RasterizeTextConcrete(ETCS::RID target, const char* text,
                                     uint32_t font, uint32_t size_px,
                                     int32_t x, int32_t y,
                                     float r, float g, float b, float a) override
    {
        Surface_* dst = ETCS::resolve_in_family<Surface_>("Surface", target);
        if (!dst)
        {
            ETCS_LOG("TextLabel", "RasterizeText target RID:" << target
                     << " does not resolve as a Surface -- nothing drawn.");
            return TextExtent{0, 0, 0};
        }
        const uint32_t scale = size_px ? (size_px / CELL_H ? size_px / CELL_H : 1) : m_scale;
        drawRun(dst, text, x, y, scale, r, g, b, a);
        return MeasureTextConcrete(text, font, size_px);
    }

    // ── Drawable2D_ dispatch ─────────────────────────────────────────────

    Rect2D BoundsConcrete() override
    {
        const std::string shown = liveText();
        const TextExtent e = MeasureTextConcrete(shown.c_str(), 0, 0);
        return Rect2D{ m_x, m_y, e.width + m_pad * 2, e.height + m_pad * 2 };
    }

    // Rectangular: the shape of a run of text is the box it occupies. Picking
    // per-glyph would make a label impossible to click between its letters,
    // which is not what anybody means by clicking a label.
    bool ContainsLocalConcrete(int32_t x, int32_t y) override
    {
        const Rect2D b = BoundsConcrete();
        return x >= 0 && y >= 0
            && x < static_cast<int32_t>(b.w) && y < static_cast<int32_t>(b.h);
    }

    // ── Surface_ dispatch ────────────────────────────────────────────────
    //
    // A label owns no pixels; it is a thing that writes into someone else's.
    // Honest no-ops rather than an emulation, the same answer Scene3D gives
    // for the same reason.
    void ClearConcrete(float, float, float, float) override {}
    void DrawRectConcrete(int32_t, int32_t, uint32_t, uint32_t,
                          float, float, float, float) override {}
    void BlitConcrete(Surface_*, int32_t, int32_t, uint32_t, uint32_t, float) override {}

    WindowSize GetSizeConcrete() override
    {
        const Rect2D b = BoundsConcrete();
        return WindowSize{ b.w, b.h };
    }

    // ── Drawable_ dispatch ───────────────────────────────────────────────
    void DrawIntoConcrete(Surface_* dst) override
    {
        const std::string shown = liveText();
        if (!dst || shown.empty()) return;

        const Point2D base = parentAbsoluteOrigin();
        const int32_t ox = base.x + m_x;
        const int32_t oy = base.y + m_y;

        if (m_bg[3] > 0.0f)
        {
            const Rect2D b = BoundsConcrete();
            dst->DrawRect(ox, oy, b.w, b.h, m_bg[0], m_bg[1], m_bg[2], m_bg[3]);
        }
        drawRun(dst, shown.c_str(),
                ox + static_cast<int32_t>(m_pad),
                oy + static_cast<int32_t>(m_pad),
                m_scale, m_color[0], m_color[1], m_color[2], m_color[3]);

        drawChildren(dst);
    }

    // ── Deletable_ ───────────────────────────────────────────────────────
    bool DeleteConcrete() override
    {
        std::string conjugate_key = getSourceModule().toString() + ":" + getSourceTag().toString();
        ETCS_LOG("TextLabel", "firing self-DestroyEvent for RID:" << getRID());
        return ETCS::DestroyEvent{conjugate_key.c_str(), this}();
    }

    // A label bound to a frame rate is never settled -- the number it shows
    // changes without anyone marking it. That is exactly the question the
    // family added for self-animating nodes (ontology/Drawable.h), and
    // answering it is what keeps the compositors above this label recomposing
    // while the counter is live.
    bool Animating() override { return m_fps_src != 0; }

private:
    /*
 * What this label actually shows: its own text, or the bound surface's
 * current rate substituted for a "%f" marker in it.
 *
 * The marker rather than replacing the whole string, so a caption keeps its
 * label -- "FPS %f" reads as "FPS 53.0" and the script still owns the wording.
 * Resolved by TAG, not by casting a family pointer: Fps is a property of this
 * module's own surface and not of the Surface family, and reading another
 * module's fields off a family pointer is the mistake the interface-pointer
 * discipline exists to prevent.
 */
    std::string liveText() const
    {
        if (m_fps_src == 0) return m_text;
        const std::string marker = "%f";
        const size_t at = m_text.find(marker);
        if (at == std::string::npos) return m_text;

        Surface_* s = ETCS::resolve_in_family<Surface_>("Surface", m_fps_src);
        if (!s || s->getSourceTag() != ETCS::Buffer("Surface")) return m_text;

        const float fps = static_cast<Surface*>(s->getTrueType())->Fps();
        // One decimal, formatted by hand: this runs every frame and a
        // stringstream here would allocate three times per draw for a number
        // with four significant figures in it.
        const int whole = static_cast<int>(fps);
        const int tenth = static_cast<int>((fps - static_cast<float>(whole)) * 10.0f + 0.5f);
        std::string out = m_text.substr(0, at);
        out += std::to_string(whole);
        out += '.';
        out += static_cast<char>('0' + (tenth > 9 ? 9 : (tenth < 0 ? 0 : tenth)));
        out += m_text.substr(at + marker.size());
        return out;
    }

    /*
 * One run of text, as rectangles.
 *
 * VERTICAL RUNS, not one rect per pixel. A glyph column is five bits; the
 * loop emits one DrawRect per unbroken run of set bits, so a capital I is two
 * rects rather than fourteen and a full frame counter is a few dozen calls
 * instead of hundreds. That matters because these land in a retained
 * composition (VulkanSurface) where every rect is a draw command.
 */
    void drawRun(Surface_* dst, const char* text, int32_t x, int32_t y,
                 uint32_t scale, float r, float g, float b, float a)
    {
        if (!dst || !text) return;
        const int32_t s = static_cast<int32_t>(scale);
        int32_t pen = x;

        for (const char* p = text; *p; ++p, pen += static_cast<int32_t>(ADVANCE) * s)
        {
            const uint8_t* col = glyph(*p);
            if (!col) continue;
            for (uint32_t c = 0; c < CELL_W; ++c)
            {
                uint8_t bits = col[c];
                uint32_t row = 0;
                while (row < CELL_H)
                {
                    if (!(bits & (1u << row))) { ++row; continue; }
                    uint32_t run = 0;
                    while (row + run < CELL_H && (bits & (1u << (row + run)))) ++run;
                    dst->DrawRect(pen + static_cast<int32_t>(c) * s,
                                  y + static_cast<int32_t>(row) * s,
                                  static_cast<uint32_t>(s),
                                  static_cast<uint32_t>(run) * scale,
                                  r, g, b, a);
                    row += run;
                }
            }
        }
    }

    // Where this node's PARENT sits, stopping at the first pixel-owning
    // ancestor. Identical to PolygonDrawable2D's and Camera3D's -- the four
    // 2D leaves are interchangeable as children, so they must agree on what a
    // position means.
    Point2D parentAbsoluteOrigin()
    {
        Point2D acc{0, 0};
        for (ETCS::Entity* node = getParent(); node; node = node->getParent())
        {
            void* d2 = node->getInterfacePointer(ETCS::Buffer("Drawable2D"));
            if (!d2) break;
            if (node->getInterfacePointer(ETCS::Buffer("Pixels"))) break;   // origin
            const Rect2D pb = static_cast<Drawable2D_*>(d2)->Bounds();
            acc.x += pb.x;
            acc.y += pb.y;
        }
        return acc;
    }

    // Changing a label changes the image, so every pixel owner above it holds
    // a stale copy. The same walk PolygonDrawable2D, Camera3D and Scene3D all
    // make -- three leaves, one rule.
    void markDirtyPath()
    {
        for (ETCS::Entity* n = this; n; n = n->getParent())
        {
            void* p = n->getInterfacePointer(ETCS::Buffer("Pixels"));
            if (p) static_cast<Pixels_*>(p)->MarkDirty();
        }
    }

    /*
 * The font: 95 glyphs, ASCII 32..126, five columns each, bit 0 = top row.
 *
 * EIGHT ROWS, NOT SEVEN, and the eighth is the reason. A 5x7 cell has nowhere
 * to put a descender, so g, j, p, q and y get folded up onto the baseline --
 * which is why the first render of this font said "SetBack9round". The five
 * that need it are authored against the full cell; every other glyph leaves
 * row 7 clear, which is what a baseline is.
 *
 * Column-major because that is how the renderer walks it -- a vertical run of
 * set bits within one column is one rectangle, and storing rows would mean
 * transposing on every draw. An out-of-range character renders as nothing
 * rather than as a box: a label with an accent in it should lose the accent,
 * not gain a row of tofu.
 */
    static const uint8_t* glyph(char ch)
    {
        static const uint8_t FONT[95][5] = {
            {0x00,0x00,0x00,0x00,0x00}, // ' '
            {0x00,0x00,0x2F,0x00,0x00}, // !
            {0x00,0x07,0x00,0x07,0x00}, // "
            {0x14,0x7F,0x14,0x7F,0x14}, // #
            {0x24,0x2A,0x7F,0x2A,0x12}, // $
            {0x23,0x13,0x08,0x64,0x62}, // %
            {0x36,0x49,0x55,0x22,0x50}, // &
            {0x00,0x05,0x03,0x00,0x00}, // '
            {0x00,0x1C,0x22,0x41,0x00}, // (
            {0x00,0x41,0x22,0x1C,0x00}, // )
            {0x14,0x08,0x3E,0x08,0x14}, // *
            {0x08,0x08,0x3E,0x08,0x08}, // +
            {0x00,0x50,0x30,0x00,0x00}, // ,
            {0x08,0x08,0x08,0x08,0x08}, // -
            {0x00,0x60,0x60,0x00,0x00}, // .
            {0x20,0x10,0x08,0x04,0x02}, // /
            {0x3E,0x51,0x49,0x45,0x3E}, // 0
            {0x00,0x42,0x7F,0x40,0x00}, // 1
            {0x42,0x61,0x51,0x49,0x46}, // 2
            {0x21,0x41,0x45,0x4B,0x31}, // 3
            {0x18,0x14,0x12,0x7F,0x10}, // 4
            {0x27,0x45,0x45,0x45,0x39}, // 5
            {0x3C,0x4A,0x49,0x49,0x30}, // 6
            {0x01,0x71,0x09,0x05,0x03}, // 7
            {0x36,0x49,0x49,0x49,0x36}, // 8
            {0x06,0x49,0x49,0x29,0x1E}, // 9
            {0x00,0x36,0x36,0x00,0x00}, // :
            {0x00,0x56,0x36,0x00,0x00}, // ;
            {0x08,0x14,0x22,0x41,0x00}, // <
            {0x14,0x14,0x14,0x14,0x14}, // =
            {0x00,0x41,0x22,0x14,0x08}, // >
            {0x02,0x01,0x51,0x09,0x06}, // ?
            {0x32,0x49,0x79,0x41,0x3E}, // @
            {0x7E,0x11,0x11,0x11,0x7E}, // A
            {0x7F,0x49,0x49,0x49,0x36}, // B
            {0x3E,0x41,0x41,0x41,0x22}, // C
            {0x7F,0x41,0x41,0x22,0x1C}, // D
            {0x7F,0x49,0x49,0x49,0x41}, // E
            {0x7F,0x09,0x09,0x09,0x01}, // F
            {0x3E,0x41,0x49,0x49,0x7A}, // G
            {0x7F,0x08,0x08,0x08,0x7F}, // H
            {0x00,0x41,0x7F,0x41,0x00}, // I
            {0x20,0x40,0x41,0x3F,0x01}, // J
            {0x7F,0x08,0x14,0x22,0x41}, // K
            {0x7F,0x40,0x40,0x40,0x40}, // L
            {0x7F,0x02,0x0C,0x02,0x7F}, // M
            {0x7F,0x04,0x08,0x10,0x7F}, // N
            {0x3E,0x41,0x41,0x41,0x3E}, // O
            {0x7F,0x09,0x09,0x09,0x06}, // P
            {0x3E,0x41,0x51,0x21,0x5E}, // Q
            {0x7F,0x09,0x19,0x29,0x46}, // R
            {0x46,0x49,0x49,0x49,0x31}, // S
            {0x01,0x01,0x7F,0x01,0x01}, // T
            {0x3F,0x40,0x40,0x40,0x3F}, // U
            {0x1F,0x20,0x40,0x20,0x1F}, // V
            {0x3F,0x40,0x38,0x40,0x3F}, // W
            {0x63,0x14,0x08,0x14,0x63}, // X
            {0x07,0x08,0x70,0x08,0x07}, // Y
            {0x61,0x51,0x49,0x45,0x43}, // Z
            {0x00,0x7F,0x41,0x41,0x00}, // [
            {0x02,0x04,0x08,0x10,0x20}, // backslash
            {0x00,0x41,0x41,0x7F,0x00}, // ]
            {0x04,0x02,0x01,0x02,0x04}, // ^
            {0x40,0x40,0x40,0x40,0x40}, // _
            {0x00,0x01,0x02,0x04,0x00}, // `
            {0x20,0x54,0x54,0x54,0x78}, // a
            {0x7F,0x48,0x44,0x44,0x38}, // b
            {0x38,0x44,0x44,0x44,0x20}, // c
            {0x38,0x44,0x44,0x48,0x7F}, // d
            {0x38,0x54,0x54,0x54,0x18}, // e
            {0x08,0x7E,0x09,0x01,0x02}, // f
            {0x98,0xA4,0xA4,0xA4,0x78}, // g
            {0x7F,0x08,0x04,0x04,0x78}, // h
            {0x00,0x44,0x7D,0x40,0x00}, // i
            {0x40,0x80,0x84,0x7D,0x00}, // j
            {0x7F,0x10,0x28,0x44,0x00}, // k
            {0x00,0x41,0x7F,0x40,0x00}, // l
            {0x7C,0x04,0x18,0x04,0x78}, // m
            {0x7C,0x08,0x04,0x04,0x78}, // n
            {0x38,0x44,0x44,0x44,0x38}, // o
            {0xFC,0x24,0x24,0x24,0x18}, // p
            {0x18,0x24,0x24,0x24,0xFC}, // q
            {0x7C,0x08,0x04,0x04,0x08}, // r
            {0x48,0x54,0x54,0x54,0x20}, // s
            {0x04,0x3F,0x44,0x40,0x20}, // t
            {0x3C,0x40,0x40,0x20,0x7C}, // u
            {0x1C,0x20,0x40,0x20,0x1C}, // v
            {0x3C,0x40,0x30,0x40,0x3C}, // w
            {0x44,0x28,0x10,0x28,0x44}, // x
            {0x9C,0xA0,0xA0,0xA0,0x7C}, // y
            {0x44,0x64,0x54,0x4C,0x44}, // z
            {0x00,0x08,0x36,0x41,0x00}, // {
            {0x00,0x00,0x7F,0x00,0x00}, // |
            {0x00,0x41,0x36,0x08,0x00}, // }
            {0x08,0x08,0x2A,0x1C,0x08}, // ~
        };
        const unsigned char u = static_cast<unsigned char>(ch);
        if (u < 32 || u > 126) return nullptr;
        return FONT[u - 32];
    }

    std::string m_text;
    int32_t     m_x = 0;
    int32_t     m_y = 0;
    uint32_t    m_scale = 2;
    uint32_t    m_pad   = 0;
    float       m_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float       m_bg[4]    = {0.0f, 0.0f, 0.0f, 0.0f};
    ETCS::RID   m_fps_src  = 0;
};

#endif
