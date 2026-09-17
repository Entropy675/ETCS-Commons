#ifndef PAINTPROVIDER_H__
#define PAINTPROVIDER_H__

#define ETCS_DLL_EXPORTS
#include "../../core_defs.h"
#include "../../ontology.h"
#include "Contract_PaintProvider.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

// PaintProvider is intentionally a thin ontology layer on top of the existing
// RenderProvider::Surface family. It does not invent a second render backend; it
// composes the existing window- and image-surface verbs and adds a Pinta-like
// document/canvas/tool model around them.
//
// Script-facing surface: only the ETCS work/stream verbs that are useful and
// safe at the language boundary are exported. Internal helper setters and state
// mutation remain C++-only: they are part of the runtime model but not part of
// the user-visible ETCS contract. This matches ChessProvider's intent: the
// language exposes the valid subset; the C++ type enforces the rest.
//
// Input affinity: ConsumeInput is meant to run on the first detached script
// thread (the same side as Window::ProduceEvents / the OS event pump) so brush
// and canvas state never cross onto a generic worker mid-stroke.
//
// Pointer events: InputEvent carries an ABSOLUTE, content-area-relative
// position for INPUT_MOTION (see ontology/InputSource.h). There is no cursor to
// integrate -- the event already says where the pointer is, in the same space
// the canvas is measured in, so the brush lands under the actual cursor from
// the first sample rather than from wherever an accumulator started.
//
// That primitive matters more here than anywhere else in the codebase. An
// integrated cursor drifts by exactly the events it missed, and a stroke offset
// from the pointer is not a stroke anybody wants. It also means CaptureMouse
// stays off, which is correct: painting wants the desktop pointer, not the FPS
// look mode.

struct PaintColor
{
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
};

enum class PaintBlendMode : uint8_t
{
    Normal = 0,
    Multiply = 1,
    Screen = 2,
    Erase = 3,
};

struct PaintStrokePoint
{
    int32_t x = 0;
    int32_t y = 0;
    uint32_t pressure = 255;
};

struct PaintBrushState
{
    float radius_px = 8.0f;
    float hardness = 0.75f;
    PaintColor color{1.0f, 0.0f, 0.0f, 1.0f};
    PaintBlendMode blend = PaintBlendMode::Normal;
    bool enabled = true;
};

static inline ETCS::Entity* paint_resolve_tag(const char* tag, ETCS::RID rid)
{
    if (rid == 0) return nullptr;
    auto& ridMap = ETCS::EventNode::getInstance().ridMap;
    auto it = ridMap.find(ETCS::Buffer(tag));
    if (it == ridMap.end()) return nullptr;
    return it->second.invoke_get(rid);
}

// Stamp a filled disc of the current brush onto a Surface (live feedback).
// Approximates the brush with a axis-aligned rect of diameter 2*radius for
// the smoke path; a later pass can use ImageSurface pixel upload.
/*
 * EVERY PIXEL OWNER ABOVE THE TARGET NOW HOLDS A STALE COPY.
 *
 * The walk and the rule are ontology/Pixels.h's; what is specific here is the
 * reason for taking it. Every other caller marks because a node in the TREE
 * changed. This one marks because a brush wrote into a node's buffer from
 * OUTSIDE the tree, which nothing in the tree can notice.
 *
 * That is the whole reason strokes were landing and never appearing: the
 * canvas buffer had the paint in it from the first stamp; the compositor
 * above it had blitted a copy before any of that happened, was never told
 * otherwise, and correctly re-presented its snapshot 900 times.
 *
 * By RID and held, because the target belongs to another module and may be
 * deleted between two points of one stroke.
 */
static inline void paint_mark_pixel_path(ETCS::RID target)
{
    ETCS::Held<Surface_> held = ETCS::resolve_held<Surface_>("Surface", target);
    if (!held) return;
    etcs_mark_observed(static_cast<ETCS::Entity*>(held.get()));
}

static inline void paint_stamp_surface(ETCS::RID target, int32_t x, int32_t y,
                                       const PaintBrushState& brush)
{
    Surface_* surface = ETCS::resolve_in_family<Surface_>("Surface", target);
    if (!surface) return;
    const int r = std::max(1, static_cast<int>(brush.radius_px));
    surface->DrawRect(x - r, y - r,
                      static_cast<uint32_t>(r * 2),
                      static_cast<uint32_t>(r * 2),
                      brush.color.r, brush.color.g, brush.color.b, brush.color.a);
    paint_mark_pixel_path(target);
}

/*
 * WHAT A STROKE BECOMES.
 *
 * The kind is not "which brush" -- radius, colour, hardness and blend already
 * say that, and they vary independently of this. It is the shape of the whole
 * INTERACTION: how many points matter, whether the marks land while the pointer
 * moves or only when it is let go, and whether anything is committed at all.
 * Three different answers, and a tool belongs to exactly one of them:
 *
 *   CONTINUOUS   Brush, Smudge      every sample marks, segments interpolated
 *   ANCHORED     Line, Rect, Ellipse   two points; preview while dragging,
 *                                      committed on release
 *   PLACED       Fill, Glyph        one point, one commit, no drag at all
 *   MEASURED     Ruler              nothing is ever committed
 *
 * That grouping is what the input edge switches on, and it is why Ruler is a
 * tool rather than a mode: it takes the same press-drag-release a line takes and
 * differs only in committing nothing, so making it anything else would mean a
 * second path through the same gesture.
 */
// GLFW's numbering, which is what arrives on the pointer ring (pushButton takes
// the platform's index unchanged). Named here so the input edge does not test a
// bare 1 and leave the reader to guess which button that is.
static constexpr uint16_t PAINT_BUTTON_LEFT  = 0;
static constexpr uint16_t PAINT_BUTTON_RIGHT = 1;

enum class PaintToolKind : uint8_t
{
    Brush,      // stamp every sample, join consecutive ones
    Line,       // anchor to release, straight
    Rect,       // anchor and release as opposite corners
    Ellipse,    // the same two corners, inscribed
    Fill,       // flood from the point, bounded by colour
    Smudge,     // carry pixels along the stroke instead of laying new ones
    Ruler,      // measure and show; commit nothing
    Glyph       // place text at the point
};

inline const char* paint_tool_kind_name(PaintToolKind k)
{
    switch (k)
    {
    case PaintToolKind::Brush:   return "brush";
    case PaintToolKind::Line:    return "line";
    case PaintToolKind::Rect:    return "rect";
    case PaintToolKind::Ellipse: return "ellipse";
    case PaintToolKind::Fill:    return "fill";
    case PaintToolKind::Smudge:  return "smudge";
    case PaintToolKind::Ruler:   return "ruler";
    case PaintToolKind::Glyph:   return "glyph";
    }
    return "brush";
}

// Parsed from a script or a palette entry. An unknown name is the brush and
// says so, rather than silently selecting nothing and leaving the user with a
// tool that does not mark.
inline PaintToolKind paint_tool_kind_from(const std::string& name)
{
    if (name == "line")    return PaintToolKind::Line;
    if (name == "rect")    return PaintToolKind::Rect;
    if (name == "ellipse") return PaintToolKind::Ellipse;
    if (name == "fill")    return PaintToolKind::Fill;
    if (name == "smudge")  return PaintToolKind::Smudge;
    if (name == "ruler")   return PaintToolKind::Ruler;
    if (name == "glyph")   return PaintToolKind::Glyph;
    if (name != "brush")
        ETCS_LOG("PaintTool", "unknown tool kind '" << name << "' -- using the brush.");
    return PaintToolKind::Brush;
}

// Which of the four interaction shapes a kind has. Asked by the input edge
// rather than open-coded there, so adding a tool is one line in each of these
// rather than a new case in every branch that cares.
inline bool paint_kind_is_anchored(PaintToolKind k)
{
    return k == PaintToolKind::Line || k == PaintToolKind::Rect
        || k == PaintToolKind::Ellipse || k == PaintToolKind::Ruler;
}
inline bool paint_kind_is_placed(PaintToolKind k)
{
    return k == PaintToolKind::Fill || k == PaintToolKind::Glyph;
}
inline bool paint_kind_commits(PaintToolKind k)
{
    return k != PaintToolKind::Ruler;
}










class PaintTool : public DeletableBase<PaintTool>
{
public:
    WIRE_TYPE_IDENTITY(PaintTool);

    PaintTool() = default;
    bool DeleteConcrete() override { return true; }

    void SetKind(const std::string& name) { m_kind = paint_tool_kind_from(name); }
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
    void SetTolerance(uint32_t tolerance) { m_tolerance = std::min(tolerance, 255u); }
    uint32_t tolerance() const { return m_tolerance; }

    void SetRadius(float radius)
    {
        m_brush.radius_px = std::max(1.0f, radius);
    }

    void SetColor(float r, float g, float b, float a)
    {
        m_brush.color = PaintColor{r, g, b, a};
    }

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
    PaintBrushState m_brush;
    PaintToolKind m_kind = PaintToolKind::Brush;
    std::string m_text = "Text";
    uint32_t m_text_px = 16;
    uint32_t m_tolerance = 24;
    bool m_active = false;
    std::vector<PaintStrokePoint> m_points;
};

/*
 * A LAYER IS A Layer_, and that is the whole of layer prioritisation.
 *
 * A stack of layers is an order with a frame of reference -- a HERE, and a
 * bounded set of others near it -- which is exactly what the Layer family means
 * (ontology/Layer.h), so the stack is not a second mechanism PaintProvider
 * invents. It is the relation the leaf declares plus the ordered read RIDList
 * already performs. A layer window is then a list of KEYS: a row sets this
 * number, `Reorder()` tells whichever list holds this layer that its computed
 * order is now a lie, and the next composite reads the new stack. Nothing about
 * the pixels moves.
 *
 * NOT A Surface, which is the distinction the family exists to allow. Both are
 * Layers; a surface is one you can draw ON, and a paint layer is one that owns
 * its own raster and is composited BY a surface. While orderability was claimed
 * in SurfaceBase these could not be separated, and a layer had to either become
 * a surface or invent its own stacking.
 *
 * ONE OPERATOR, the rest derived by OrderableBase. Lower composites first, so
 * higher ends up on top -- the same direction Drawable_::Order() reads, because
 * "what you see" and "what is on top" have to be the same answer whether the
 * thing stacking is a drawable or a layer.
 *
 * NAMED, because the window that sets the order has to show something. The name
 * is a label and only a label: identity is the RID (Orderable.h says so), and
 * two layers may perfectly well share a name and an order.
 */
class PaintLayer : public LayerBase<PaintLayer>,
                   public PixelsBase<PaintLayer>,
                   public DeletableBase<PaintLayer>
{
public:
    WIRE_TYPE_IDENTITY(PaintLayer);

    PaintLayer() = default;
    bool DeleteConcrete() override { return true; }

    // --- Orderable_ ---
    int32_t m_order = 0;
    bool operator<(const PaintLayer& o) const { return m_order < o.m_order; }

    /*
 * Reorder() AFTER the key moves, never before: the seam marks the holding list
 * stale and the next ordered read rebuilds, so marking first would cache the
 * order this call is about to invalidate. Idempotent and cheap -- a burst of
 * row drags before one composite costs one rebuild (Orderable_::Reorder).
 */
    void SetOrder(int32_t order)
    {
        m_order = order;
        this->Reorder();
    }

    void SetName(const std::string& name) { m_name = name; }
    const std::string& name() const { return m_name; }
    int32_t order() const { return m_order; }

    /*
 * --- Layer_ ---
 *
 * WHAT THIS IS A VIEW OF: the document that composites it, which is simply the
 * PARENT. Nothing is stored and nothing has to be kept in step, because
 * membership IS parenthood here -- a layer belongs to a document by being its
 * typed child, so the question the family asks and the question the tree
 * already answers are the same question.
 *
 * That is the whole reason to categorise by trait rather than by a side list.
 * The earlier shape had the document hold a vector of layer pointers and hand
 * each layer a subject RID to remember; two records of one fact, either of
 * which could be right while the other was stale. There is now one.
 *
 * Falls back to the family's default when there is no parent: a layer created
 * but not yet added to anything is a view of itself, which is true and is what
 * every caller already handles.
 */
    ETCS::RID Subject() override
    {
        ETCS::Entity* p = this->getParent();
        return p ? p->getRID() : this->getRID();
    }

    /*
 * THE BUFFER IS Pixels_'s, not this type's.
 *
 * A layer is a grid of RGBA bytes with a size, which is what the Pixels family
 * already means -- so claiming it removes the width, the height and the vector
 * this class used to keep beside them, rather than adding anything. What comes
 * back for free is everything a host-addressable buffer can answer: PixelData,
 * PixelStride, FillRect, ClearTo, and the mark that tells an observer the bytes
 * moved.
 *
 * It also makes a layer a legal target for anything that writes pixels by RID
 * without knowing what produced them -- Glyphs_::RasterizeText is the one this
 * was needed for, and it is the reason text can land IN a layer rather than
 * beside it as another node.
 */
    bool Create(uint32_t w, uint32_t h)
    {
        if (w == 0 || h == 0) return false;
        m_visible = true;
        m_opacity = 1.0f;
        this->Allocate(w, h);
        return true;
    }

    void SetVisible(bool visible) { m_visible = visible; }
    void ToggleVisible()           { m_visible = !m_visible; }

    void SetOpacity(float opacity)
    {
        m_opacity = std::clamp(opacity, 0.0f, 1.0f);
    }

    /*
 * A SECOND ALPHA, and it is deliberately not m_opacity.
 *
 * Hovering a row in the layer window dims every OTHER layer so the hovered one
 * reads on its own. That is a view state -- it lasts as long as the pointer is
 * where it is and must leave no trace -- while m_opacity is the document's: the
 * thing the user set and expects to find again. One float for both would mean a
 * hover that ends badly saves its dimming into the picture, which is the class
 * of bug that loses work rather than merely looking wrong.
 *
 * Multiplied, not substituted, in BlitTo: a layer already at 0.5 that gets
 * dimmed is dimmer still, which is what "everything except this one recedes"
 * means.
 */
    void SetDim(float dim) { m_dim = std::clamp(dim, 0.0f, 1.0f); }
    float dim() const { return m_dim; }

    /*
 * HOW MUCH OF THIS LAYER HAS BEEN PAINTED ON, counted rather than described.
 *
 * The only honest answer to "did this pane receive that stroke" -- a stack that
 * lists the right layers proves nothing about which of them the pointer reached,
 * and that is exactly the question a pass budget has to be tested against. Alpha,
 * not colour: a dab of the paper's own white is still a dab.
 */
    size_t InkedPixels() const
    {
        const uint8_t* px = this->PixelData();
        if (!px) return 0;
        size_t n = 0;
        for (size_t i = 3; i < this->PixelBytes(); i += 4)
            if (px[i] != 0) ++n;
        return n;
    }

    void Report() const
    {
        ETCS_LOG("PaintLayer", "RID:" << this->getRID()
                 << " '" << m_name << "' order=" << m_order
                 << (m_visible ? " visible" : " hidden")
                 << " opacity=" << m_opacity << " dim=" << m_dim
                 << " inked=" << InkedPixels());
    }

    // REPLACES every byte including alpha, which is what clearing means and
    // what Pixels_::ClearTo already does -- blending a colour over whatever was
    // there would leave the old contents showing through at any alpha below 1.
    void Clear(float r, float g, float b, float a) { this->ClearTo(r, g, b, a); }

    void DrawPixel(int32_t x, int32_t y, float r, float g, float b, float a)
    {
        uint8_t* px = this->PixelData();
        if (!px) return;
        if (x < 0 || y < 0 ||
            static_cast<uint32_t>(x) >= this->PixelWidth() ||
            static_cast<uint32_t>(y) >= this->PixelHeight())
            return;

        size_t i = (static_cast<size_t>(y) * this->PixelWidth() + static_cast<size_t>(x)) * 4;
        px[i + 0] = static_cast<uint8_t>(std::clamp(r, 0.0f, 1.0f) * 255.0f);
        px[i + 1] = static_cast<uint8_t>(std::clamp(g, 0.0f, 1.0f) * 255.0f);
        px[i + 2] = static_cast<uint8_t>(std::clamp(b, 0.0f, 1.0f) * 255.0f);
        px[i + 3] = static_cast<uint8_t>(std::clamp(a, 0.0f, 1.0f) * 255.0f);
    }

    void DrawBrush(int32_t cx, int32_t cy, const PaintBrushState& brush)
    {
        const int r = std::max(1, static_cast<int>(brush.radius_px));
        const int r2 = r * r;
        for (int dy = -r; dy <= r; ++dy)
        {
            for (int dx = -r; dx <= r; ++dx)
            {
                if (dx * dx + dy * dy > r2) continue;
                DrawPixel(cx + dx, cy + dy,
                          brush.color.r, brush.color.g, brush.color.b, brush.color.a);
            }
        }
    }

    void DrawLine(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                  float r, float g, float b, float a)
    {
        const int dx = std::abs(x1 - x0);
        const int dy = std::abs(y1 - y0);
        const int sx = (x0 < x1) ? 1 : -1;
        const int sy = (y0 < y1) ? 1 : -1;
        int err = dx - dy;
        int x = x0;
        int y = y0;
        while (true)
        {
            DrawPixel(x, y, r, g, b, a);
            if (x == x1 && y == y1) break;
            const int e2 = 2 * err;
            if (e2 > -dy) { err -= dy; x += sx; }
            if (e2 <  dx) { err += dx; y += sy; }
        }
    }

    /*
 * ── the shapes ───────────────────────────────────────────────────────────
 *
 * OUTLINES ARE STROKED WITH THE BRUSH, not plotted a pixel wide. A shape tool
 * is still holding whatever nib is selected, and a rectangle drawn with a
 * 30px brush should look like it -- so every outline reduces to DrawBrush along
 * a path, which is also why they all come out with the same ends and joins the
 * freehand stroke has. One mark-making primitive, several ways of deciding
 * where it goes.
 */
    void DrawRectOutline(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                         const PaintBrushState& brush)
    {
        const int32_t lx = std::min(x0, x1), rx = std::max(x0, x1);
        const int32_t ty = std::min(y0, y1), by = std::max(y0, y1);
        StrokeLine(lx, ty, rx, ty, brush);
        StrokeLine(rx, ty, rx, by, brush);
        StrokeLine(rx, by, lx, by, brush);
        StrokeLine(lx, by, lx, ty, brush);
    }

    void FillRectBrush(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                       const PaintColor& c)
    {
        const int32_t lx = std::min(x0, x1), rx = std::max(x0, x1);
        const int32_t ty = std::min(y0, y1), by = std::max(y0, y1);
        this->FillRect(lx, ty, static_cast<uint32_t>(rx - lx + 1),
                       static_cast<uint32_t>(by - ty + 1), c.r, c.g, c.b, c.a);
    }

    /*
 * INSCRIBED IN THE DRAG, like every other program's ellipse: the two points are
 * opposite corners of the bounding box rather than centre and radius. That is
 * not a preference, it is what makes the rectangle tool and the ellipse tool
 * interchangeable mid-gesture -- the same drag means the same region.
 *
 * Parametric rather than midpoint-incremental, because the step count is chosen
 * from the SIZE: a 4px ellipse does not need 360 samples and a 900px one needs
 * more than that. Overdraw is free here (every sample is a brush dab landing on
 * its neighbour), so the only cost of a step too fine is time, and the cost of
 * one too coarse is a dotted curve.
 */
    void DrawEllipseOutline(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                            const PaintBrushState& brush)
    {
        const double cx = (x0 + x1) * 0.5, cy = (y0 + y1) * 0.5;
        const double rx = std::abs(x1 - x0) * 0.5, ry = std::abs(y1 - y0) * 0.5;
        if (rx < 0.5 && ry < 0.5) { DrawBrush(x0, y0, brush); return; }

        const double span = std::max(rx, ry);
        const int steps = std::clamp(static_cast<int>(span * 4.0), 16, 2048);
        int32_t px = 0, py = 0;
        for (int i = 0; i <= steps; ++i)
        {
            const double t = (2.0 * 3.14159265358979323846 * i) / steps;
            const int32_t ex = static_cast<int32_t>(std::lround(cx + rx * std::cos(t)));
            const int32_t ey = static_cast<int32_t>(std::lround(cy + ry * std::sin(t)));
            if (i == 0) DrawBrush(ex, ey, brush);
            else        StrokeLine(px, py, ex, ey, brush);
            px = ex; py = ey;
        }
    }

    // The freehand segment, exposed so the shapes above and the input edge use
    // one implementation of "drag the nib from here to there".
    void StrokeLine(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                    const PaintBrushState& brush)
    {
        const int dx = std::abs(x1 - x0), dy = std::abs(y1 - y0);
        const int steps = std::max(1, std::max(dx, dy));
        for (int i = 0; i <= steps; ++i)
        {
            const int32_t x = x0 + (x1 - x0) * i / steps;
            const int32_t y = y0 + (y1 - y0) * i / steps;
            DrawBrush(x, y, brush);
        }
    }

    /*
 * ── flood fill ───────────────────────────────────────────────────────────
 *
 * Scanline, four-connected, bounded by the colour under the seed.
 *
 * TOLERANCE IS PER CHANNEL AND INCLUDES ALPHA, because the thing being filled
 * is usually transparent: a fresh ink layer is alpha 0 everywhere, and a fill
 * that compared only RGB would treat "transparent black" and "opaque black" as
 * the same region and flood straight through a black line.
 *
 * SCANLINE RATHER THAN PER-PIXEL RECURSION, and rather than a queue of single
 * pixels: a 1024x768 region is 786k pixels, and a four-way queue visits each of
 * them with four pushes. Runs collapse that to one entry per row segment, which
 * is the difference between a fill you can feel and one you cannot.
 *
 * MATCHED AGAINST THE SEED'S ORIGINAL COLOUR, captured before anything is
 * written -- comparing against the buffer as it changes would stop the fill at
 * its own leading edge the moment the new colour is within tolerance of the old.
 */
    size_t FloodFill(int32_t sx, int32_t sy, const PaintColor& c, uint32_t tolerance)
    {
        uint8_t* px = this->PixelData();
        if (!px) return 0;
        const int32_t w = static_cast<int32_t>(this->PixelWidth());
        const int32_t h = static_cast<int32_t>(this->PixelHeight());
        if (sx < 0 || sy < 0 || sx >= w || sy >= h) return 0;

        const size_t seed_i = (static_cast<size_t>(sy) * w + sx) * 4;
        const uint8_t seed[4] = { px[seed_i], px[seed_i+1], px[seed_i+2], px[seed_i+3] };
        // Pixels_::toByte is its own business; the same clamp-and-scale, spelled
        // here, because a fill writes bytes and has to say which ones.
        auto to_byte = [](float v) {
            return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
        };
        const uint8_t want[4] = { to_byte(c.r), to_byte(c.g), to_byte(c.b), to_byte(c.a) };

        // Already this colour: the fill would repaint the region with what it
        // already has and loop forever looking for an edge that is not there.
        bool same = true;
        for (int k = 0; k < 4; ++k) if (seed[k] != want[k]) { same = false; break; }
        if (same) return 0;

        const int tol = static_cast<int>(tolerance);
        auto matches = [&](size_t i) {
            for (int k = 0; k < 4; ++k)
                if (std::abs(static_cast<int>(px[i + k]) - static_cast<int>(seed[k])) > tol)
                    return false;
            return true;
        };

        size_t filled = 0;
        std::vector<std::pair<int32_t,int32_t>> stack;   // (x, y) seeds of runs
        stack.emplace_back(sx, sy);
        while (!stack.empty())
        {
            auto [x, y] = stack.back();
            stack.pop_back();
            size_t i = (static_cast<size_t>(y) * w + x) * 4;
            if (!matches(i)) continue;

            int32_t left = x;
            while (left > 0 && matches((static_cast<size_t>(y) * w + (left - 1)) * 4)) --left;
            int32_t right = x;
            while (right + 1 < w && matches((static_cast<size_t>(y) * w + (right + 1)) * 4)) ++right;

            for (int32_t rx2 = left; rx2 <= right; ++rx2)
            {
                size_t ri = (static_cast<size_t>(y) * w + rx2) * 4;
                ::std::memcpy(px + ri, want, 4);
                ++filled;
            }
            // One seed per contiguous run on each neighbouring row, found by
            // walking the run we just filled -- this is the whole saving.
            for (int32_t ny : { y - 1, y + 1 })
            {
                if (ny < 0 || ny >= h) continue;
                bool run = false;
                for (int32_t rx2 = left; rx2 <= right; ++rx2)
                {
                    const bool m = matches((static_cast<size_t>(ny) * w + rx2) * 4);
                    if (m && !run) { stack.emplace_back(rx2, ny); run = true; }
                    else if (!m)   { run = false; }
                }
            }
        }
        if (filled) etcs_mark_observed(this);
        return filled;
    }

    /*
 * ── smudge ───────────────────────────────────────────────────────────────
 *
 * CARRIES PIXELS ALONG THE STROKE instead of laying new ones, which is why it
 * takes a direction rather than just a point: what it writes at the head of the
 * dab is what it read one step BEHIND, blended by strength. A smudge with no
 * previous position has nothing to carry and does nothing, which is correct for
 * the first sample of a stroke.
 *
 * Read from a copy of the disc before writing any of it. Reading and writing the
 * same buffer in one pass means later pixels in the dab sample what earlier ones
 * just wrote, which turns a smear into a streak that runs away in the direction
 * of the loop rather than the direction of the hand.
 */
    void SmudgeDab(int32_t fx, int32_t fy, int32_t tx, int32_t ty,
                   const PaintBrushState& brush, float strength)
    {
        uint8_t* px = this->PixelData();
        if (!px) return;
        const int32_t w = static_cast<int32_t>(this->PixelWidth());
        const int32_t h = static_cast<int32_t>(this->PixelHeight());
        const int r  = std::max(1, static_cast<int>(brush.radius_px));
        const int r2 = r * r;
        const float k = std::clamp(strength, 0.0f, 1.0f);
        if (k <= 0.0f) return;

        const int side = r * 2 + 1;
        std::vector<uint8_t> src(static_cast<size_t>(side) * side * 4, 0);
        for (int dy = -r; dy <= r; ++dy)
            for (int dx = -r; dx <= r; ++dx)
            {
                const int sxp = fx + dx, syp = fy + dy;
                if (sxp < 0 || syp < 0 || sxp >= w || syp >= h) continue;
                ::std::memcpy(&src[((static_cast<size_t>(dy + r) * side) + (dx + r)) * 4],
                              px + (static_cast<size_t>(syp) * w + sxp) * 4, 4);
            }

        for (int dy = -r; dy <= r; ++dy)
            for (int dx = -r; dx <= r; ++dx)
            {
                if (dx * dx + dy * dy > r2) continue;
                const int dxp = tx + dx, dyp = ty + dy;
                if (dxp < 0 || dyp < 0 || dxp >= w || dyp >= h) continue;
                const uint8_t* sp = &src[((static_cast<size_t>(dy + r) * side) + (dx + r)) * 4];
                uint8_t* dp = px + (static_cast<size_t>(dyp) * w + dxp) * 4;
                for (int c2 = 0; c2 < 4; ++c2)
                    dp[c2] = static_cast<uint8_t>(dp[c2] + (sp[c2] - dp[c2]) * k);
            }
        etcs_mark_observed(this);
    }

    // Smoke-path composite: push a coarse preview of non-transparent pixels as
    // DrawRect stamps. Reads the buffer through Pixels_ now, so the one thing
    // still to do here is the real upload -- a Surface that can take a Pixels
    // source directly, rather than width*height/16 DrawRect calls.
    void BlitTo(ETCS::RID target, int32_t ox, int32_t oy, uint32_t w, uint32_t h,
                float opacity, float zoom = 1.0f)
    {
        Surface_* surface = ETCS::resolve_in_family<Surface_>("Surface", target);
        if (!surface || !m_visible) return;

        const uint8_t* px = this->PixelData();
        if (!px) return;

        const float alpha = std::clamp(opacity, 0.0f, 1.0f) * m_opacity * m_dim;
        if (alpha <= 0.0f) return;

        const uint32_t pw = this->PixelWidth();
        const uint32_t ph = this->PixelHeight();
        const size_t   bytes = this->PixelBytes();
        const uint32_t draw_w = (w == 0) ? pw : std::min(w, pw);
        const uint32_t draw_h = (h == 0) ? ph : std::min(h, ph);

        /*
     * THE PROJECTION IS APPLIED HERE, which is the only place it can be: the
     * layer's pixels are in DOCUMENT space and the surface wants VIEW space, and
     * this is the one function that touches both.
     *
     * The sample step shrinks as the zoom grows, so a magnified document is
     * sampled more finely rather than drawn as bigger blocks -- otherwise zooming
     * in would reveal the subsample grid instead of the picture. Clamped at 1:
     * below that the step would be finer than the pixels are.
     *
     * Rects are drawn one sample wider and taller than the step, deliberately.
     * At a fractional zoom consecutive samples land a fractional distance apart,
     * and a rect exactly `step*zoom` across leaves a seam wherever that rounds
     * down. Overdrawing by one covers it, and costs nothing because the
     * neighbour paints over it anyway.
     */
        const float z = (zoom <= 0.0f) ? 1.0f : zoom;
        const uint32_t step = std::max(1u, static_cast<uint32_t>(4.0f / z));
        const uint32_t rw = std::max(1u, static_cast<uint32_t>(step * z + 1.0f));

        for (uint32_t y = 0; y < draw_h; y += step)
        {
            for (uint32_t x = 0; x < draw_w; x += step)
            {
                size_t i = (static_cast<size_t>(y) * pw + x) * 4;
                if (i + 3 >= bytes) continue;
                if (px[i + 3] == 0) continue;
                const float pr = px[i + 0] / 255.0f;
                const float pg = px[i + 1] / 255.0f;
                const float pb = px[i + 2] / 255.0f;
                const float pa = (px[i + 3] / 255.0f) * alpha;
                surface->DrawRect(ox + static_cast<int32_t>(x * z),
                                  oy + static_cast<int32_t>(y * z),
                                  rw, rw, pr, pg, pb, pa);
            }
        }
    }

    uint32_t width() const { return this->PixelWidth(); }
    uint32_t height() const { return this->PixelHeight(); }
    bool visible() const { return m_visible; }
    float opacity() const { return m_opacity; }

private:
    float m_opacity = 1.0f;
    bool m_visible = true;
    std::string m_name = "Layer";
    // View state, not document state -- see SetDim. 1.0 is "not dimmed", which
    // is what every layer is until a row is hovered.
    float m_dim = 1.0f;
};


class PaintDocument : public DeletableBase<PaintDocument>
{
public:
    WIRE_TYPE_IDENTITY(PaintDocument);

    PaintDocument() = default;
    bool DeleteConcrete() override { return true; }

    bool Create(uint32_t w, uint32_t h, const std::string& name = "Untitled")
    {
        m_width = w;
        m_height = h;
        m_name = name;
        m_active_layer = nullptr;
        return true;
    }

    void SetName(const std::string& name) { m_name = name; }

    /*
 * THE STACK, read from the TYPED CHILDREN this document actually holds.
 *
 * Membership is parenthood: a layer belongs to a document by being spawned as
 * its child (`doc.spawn(PaintProvider::PaintLayer ink)`), so there is no second
 * list to keep in step and no way for "in the document" and "in the tree" to
 * disagree. That is what the trait is for -- see PaintLayer::Subject.
 *
 * ORDERED BY THE LAYERS' OWN RELATION, and read straight out of the list that
 * holds them: getOrderedTypedChildren dispatches to RIDList::collect_ordered,
 * which sorts by the pointee's operator< when the concrete type declares one,
 * and PaintLayer does. So the composite order, the layer window's row order and
 * Layer_::Neighbourhood are not three implementations of one rule -- they are
 * one read.
 *
 * FILTERED BY TAG, because a document may hold children that are not layers and
 * an order across two concrete types is a question with no answer (core/
 * RIDList.h). Within the tag it is a real order, which is all this needs.
 */
    void OrderedLayers(std::vector<PaintLayer*>& out) const
    {
        out.clear();
        std::vector<std::pair<ETCS::Buffer, ETCS::RID>> kids;
        this->getOrderedTypedChildren(kids);
        out.reserve(kids.size());
        static const ETCS::Buffer kLayerTag("PaintLayer");
        for (const auto& entry : kids)
        {
            if (!(entry.first == kLayerTag)) continue;
            ETCS::Entity* child = this->getTypedChild(entry.first, entry.second);
            if (!child) continue;
            if (auto* l = static_cast<PaintLayer*>(child->getTrueType())) out.push_back(l);
        }
    }

    /*
 * Move a layer to a stated depth and renumber the rest to match.
 *
 * The layer window drags a row; what it means is "this one is now Nth from the
 * bottom", which is a statement about the WHOLE stack rather than about one key.
 * Renumbering densely from the resulting order is what makes the next drag mean
 * the same thing -- leaving gaps or duplicates would make a second drag land
 * somewhere the user did not point at. One SetOrder per layer, and each one
 * marks the holding list stale (Orderable_::Reorder), so the rebuild happens
 * once on the next ordered read rather than once per layer.
 */
    void MoveLayerTo(ETCS::RID layer_rid, int32_t depth)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintLayer", layer_rid);
        if (!raw) return;
        auto* moved = static_cast<PaintLayer*>(raw->getTrueType());
        if (!moved) return;

        std::vector<PaintLayer*> stack;
        OrderedLayers(stack);
        auto it = std::find(stack.begin(), stack.end(), moved);
        if (it == stack.end()) return;
        stack.erase(it);

        const int32_t slot = std::clamp(depth, 0, static_cast<int32_t>(stack.size()));
        stack.insert(stack.begin() + slot, moved);
        for (size_t i = 0; i < stack.size(); ++i)
            stack[i]->SetOrder(static_cast<int32_t>(i));
    }

    void RenameLayer(ETCS::RID layer_rid, const std::string& name)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintLayer", layer_rid);
        if (!raw) return;
        if (auto* l = static_cast<PaintLayer*>(raw->getTrueType())) l->SetName(name);
    }

    /*
 * Out of the stack, not out of existence -- which is exactly what
 * Entity::detachFromParent already means: downward reachability ends, the
 * upward link and the entity itself remain. So a removed layer is still a live
 * entity a script holds a name for and can re-parent or inspect; it is simply
 * no longer one of the views this document composites.
 *
 * Deleting it is a different verb with a different meaning, and it is the one
 * Deletable already provides.
 */
    void RemoveLayer(ETCS::RID layer_rid)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintLayer", layer_rid);
        if (!raw) return;
        auto* layer = static_cast<PaintLayer*>(raw->getTrueType());
        if (!layer) return;
        if (m_active_layer == layer) m_active_layer = nullptr;
        layer->SetDim(1.0f);          // it is nobody's hover target now
        layer->detachFromParent();
    }

    /*
 * HOVER ISOLATION: the named layer at full strength, every other one dimmed.
 *
 * Stated as one call over the whole stack rather than as a dim per row, because
 * the property being set is a property of the STACK -- "exactly one of you is the
 * subject" -- and setting it per row makes leaving a row a second, separate
 * bookkeeping problem that gets it wrong the moment the pointer skips a row.
 * Naming a layer that is not here, or 0, is therefore how you say "nobody is"
 * and is the same call as ClearIsolate.
 */
    void IsolateLayer(ETCS::RID layer_rid, float dim)
    {
        PaintLayer* subject = nullptr;
        if (layer_rid != 0)
        {
            if (ETCS::Entity* raw = paint_resolve_tag("PaintLayer", layer_rid))
                subject = static_cast<PaintLayer*>(raw->getTrueType());
        }
        const float other = std::clamp(dim, 0.0f, 1.0f);
        std::vector<PaintLayer*> stack;
        OrderedLayers(stack);
        for (auto* l : stack)
            l->SetDim((subject && l == subject) ? 1.0f : (subject ? other : 1.0f));
    }

    void ClearIsolate() { IsolateLayer(0, 1.0f); }

    /*
 * The stack as text, bottom to top, for a layer window to build rows from and
 * for a test to assert on. RID first because that is what a row has to hold to
 * name this layer again -- every other verb here takes one.
 */
    void Report() const
    {
        std::vector<PaintLayer*> stack;
        OrderedLayers(stack);
        ETCS_LOG("PaintDocument", "'" << m_name << "' " << stack.size() << " layer(s), bottom first:");
        for (auto* l : stack)
            ETCS_LOG("PaintDocument", "  RID:" << l->getRID()
                     << " order=" << l->order()
                     << " '" << l->name() << "'"
                     << (l->visible() ? " visible" : " hidden")
                     << " opacity=" << l->opacity()
                     << " dim=" << l->dim()
                     << " inked=" << l->InkedPixels()
                     << (l == m_active_layer ? "  <- active" : ""));
    }

    void SetActiveLayer(ETCS::RID layer_rid)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintLayer", layer_rid);
        if (!raw) return;
        m_active_layer = static_cast<PaintLayer*>(raw->getTrueType());
    }

    void ClearLayer(ETCS::RID layer_rid, float r, float g, float b, float a)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintLayer", layer_rid);
        if (!raw) return;
        auto* layer = static_cast<PaintLayer*>(raw->getTrueType());
        if (layer) layer->Clear(r, g, b, a);
    }

    void RenderToSurface(ETCS::RID target, int32_t x, int32_t y, float zoom = 1.0f)
    {
        Surface_* surface = ETCS::resolve_in_family<Surface_>("Surface", target);
        if (!surface) return;

        std::vector<PaintLayer*> stack;
        OrderedLayers(stack);
        for (auto* layer : stack)
        {
            if (!layer->visible()) continue;
            layer->BlitTo(target, x, y, 0, 0, layer->opacity(), zoom);
        }
    }

    // Apply one brush sample to the active layer (document-side commit).
    void ApplyBrush(int32_t x, int32_t y, const PaintBrushState& brush)
    {
        if (m_active_layer) m_active_layer->DrawBrush(x, y, brush);
    }

    uint32_t width() const { return m_width; }
    uint32_t height() const { return m_height; }
    const std::string& name() const { return m_name; }
    // The stack, for a caller that wants it in one call. Built rather than
    // returned by reference: there is no stored vector any more -- see
    // OrderedLayers for why membership is the tree.
    std::vector<PaintLayer*> layers() const
    {
        std::vector<PaintLayer*> out;
        OrderedLayers(out);
        return out;
    }
    PaintLayer* activeLayer() const { return m_active_layer; }

private:
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    std::string m_name = "Untitled";
    PaintLayer* m_active_layer = nullptr;
};



class PaintSurface : public DeletableBase<PaintSurface>
{
public:
    WIRE_TYPE_IDENTITY(PaintSurface);

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
        // CLEARED FIRST, which a full-view document never needed. A projection
        // does not cover the surface, so without this the area outside the
        // document keeps whatever the last frame left there and panning smears.
        if (view) view->Clear(m_bg[0], m_bg[1], m_bg[2], m_bg[3]);
        m_document->RenderToSurface(m_target, m_pan_x, m_pan_y, m_zoom);
    }

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
    void StampBrush(int32_t x, int32_t y, const PaintBrushState& brush)
    {
        if (m_target == 0) return;
        PaintBrushState scaled = brush;
        scaled.radius_px = std::max(1.0f, brush.radius_px * m_zoom);
        paint_stamp_surface(m_target, DocToViewX(x), DocToViewY(y), scaled);
    }

    PaintDocument* document() const { return m_document; }
    ETCS::RID target() const { return m_target; }

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

        ETCS::Held<Drawable2D_> node = ETCS::resolve_held<Drawable2D_>("Drawable2D", m_zoom_label);
        if (!node) return;
        ETCS::Entity* e = static_cast<ETCS::Entity*>(node.get());
        if (!e) return;
        ETCS::Buffer action;
        action.write((e->getSourceTag().toString() + ".SetText").c_str());
        ETCS::Buffer payload;
        payload.write((std::to_string(pct) + "%").c_str());
        try { e->call(action, payload); } catch (...) {}
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
};

/*
 * ── PaintPalette ─────────────────────────────────────────────────────────
 *
 * A TOOLBAR THAT OWNS NO PIXELS.
 *
 * The obvious shape for this is a widget: a rectangle that knows how to draw
 * swatches and how to hit-test them. It is the wrong shape here, and the
 * reason is the one PaintProvider is built on -- this module owns no raster
 * backend and should not grow one for a row of coloured squares. A swatch IS
 * a rectangle in somebody else's 2D tree, and that tree already knows how to
 * draw it, where it is, and whether a point is on it (ontology/Drawable2D.h).
 *
 * So what is left for a palette to be is a MAPPING: this node means that
 * colour, that node means this radius. Nothing about how they look, nothing
 * about where they sit. Which means the toolbar's appearance is a script --
 * PaintProvider/scripts/paint_toolbar.etcs builds it out of RenderProvider
 * shapes -- and rearranging it, restyling it, or replacing it with a colour
 * wheel from a third module changes no C++ at all.
 *
 * Keyed by RID, because that is what a pick hands back and what a script can
 * name. The entries apply to the bound tool, and PaintInput stamps from
 * m_tool->brush() every sample, so a selection takes effect on the next
 * stroke with nothing to propagate.
 */
class PaintPalette : public DeletableBase<PaintPalette>
{
public:
    WIRE_TYPE_IDENTITY(PaintPalette);

    PaintPalette() = default;
    bool DeleteConcrete() override { return true; }

    void BindTool(ETCS::RID tool)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintTool", tool);
        if (!raw) return;
        m_tool = static_cast<PaintTool*>(raw->getTrueType());
    }

    void AddColor(ETCS::RID node, float r, float g, float b, float a)
    {
        if (node == 0) return;
        m_entries[node] = Entry{ Kind::Color, { r, g, b, a }, 0.0f, PaintToolKind::Brush };
    }

    void AddSize(ETCS::RID node, float radius)
    {
        if (node == 0 || radius <= 0.0f) return;
        m_entries[node] = Entry{ Kind::Size, {}, radius, PaintToolKind::Brush };
    }

    // A third thing a node can mean, alongside a colour and a size: which TOOL
    // it selects. Same mapping, same Apply, so a tool button is a rectangle in
    // the toolbar script exactly as a swatch is.
    void AddTool(ETCS::RID node, const std::string& kind)
    {
        if (node == 0) return;
        m_entries[node] = Entry{ Kind::Tool, {}, 0.0f, paint_tool_kind_from(kind) };
    }

    /*
 * AND A FOURTH: a node that steps the ZOOM.
 *
 * A view setting rather than a tool setting, which is the one argument against
 * putting it here -- and it loses to the argument for: this type is already
 * "what a picked node means", the +/- region is picked exactly as a swatch is,
 * and a fourth mapping type would duplicate the whole of AddX/Apply to hold one
 * float. The surface it applies to is bound separately for that reason: the
 * palette drives a tool and now also a view, and says so.
 *
 * `factor` multiplies rather than adds, because zoom is perceived
 * multiplicatively -- 1.25 is one notch in at every magnification, while +25%
 * is a big step at 100% and an imperceptible one at 800%.
 */
    void AddZoom(ETCS::RID node, float factor)
    {
        if (node == 0 || factor <= 0.0f) return;
        Entry e{ Kind::Zoom, {}, factor, PaintToolKind::Brush };
        m_entries[node] = e;
    }

    void BindSurface(ETCS::RID surface)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintSurface", surface);
        if (raw) m_surface = static_cast<PaintSurface*>(raw->getTrueType());
    }

    /*
 * REPLACE A SWATCH'S COLOUR, which is what a colour wheel pick does to the slot
 * it was opened from.
 *
 * The mapping is updated here; the swatch's APPEARANCE is the script's, and is
 * driven by whoever picked -- PaintPalette holds no drawable and cannot restyle
 * one (see this type's header note). Returns false for a node that is not a
 * colour entry, so a caller can tell "there is no such swatch" from "done".
 */
    bool SetColorOf(ETCS::RID node, float r, float g, float b, float a)
    {
        auto it = m_entries.find(node);
        if (it == m_entries.end() || it->second.kind != Kind::Color) return false;
        it->second.rgba[0] = r; it->second.rgba[1] = g;
        it->second.rgba[2] = b; it->second.rgba[3] = a;
        return true;
    }

    // The most recent colour entry a press landed on, which is the slot a
    // colour wheel replaces when it is dismissed. 0 when the last selection was
    // not a colour.
    ETCS::RID lastColorNode() const { return m_last_color; }

    bool Knows(ETCS::RID node) const { return m_entries.find(node) != m_entries.end(); }

    // True when `node` was one of ours -- which is also the answer to "was
    // this press a tool change rather than a brush stroke", and the only
    // thing the input edge needs from this type.
    bool Apply(ETCS::RID node)
    {
        auto it = m_entries.find(node);
        if (it == m_entries.end()) return false;
        if (!m_tool)
        {
            ETCS_LOG("PaintPalette", "selection on RID:" << node
                     << " has no tool bound -- nothing to apply it to.");
            return true;   // still ours: it was a palette press, it just went nowhere
        }
        const Entry& e = it->second;
        if (e.kind == Kind::Color)
        {
            m_tool->SetColor(e.rgba[0], e.rgba[1], e.rgba[2], e.rgba[3]);
            m_last_color = node;
            ETCS_LOG("PaintPalette", "colour -> " << e.rgba[0] << ", " << e.rgba[1]
                     << ", " << e.rgba[2] << ", " << e.rgba[3]);
        }
        else if (e.kind == Kind::Size)
        {
            m_tool->SetRadius(e.radius);
            ETCS_LOG("PaintPalette", "radius -> " << e.radius);
        }
        else if (e.kind == Kind::Tool)
        {
            m_tool->SetKind(paint_tool_kind_name(e.tool));
            ETCS_LOG("PaintPalette", "tool -> " << paint_tool_kind_name(e.tool));
        }
        else
        {
            if (!m_surface)
            {
                ETCS_LOG("PaintPalette", "zoom step on RID:" << node
                         << " has no surface bound -- BindSurface first.");
                return true;
            }
            // About the centre of the document's current on-screen box, since a
            // button press carries no meaningful point to zoom about -- unlike a
            // wheel, which zooms about the pointer.
            m_surface->ZoomBy(e.radius, m_surface->panX(), m_surface->panY());
            m_surface->Render();
            ETCS_LOG("PaintPalette", "zoom -> " << m_surface->zoomPercent() << "%");
        }
        return true;
    }

    void Report() const
    {
        ETCS_LOG("PaintPalette", m_entries.size() << " entries, tool "
                 << (m_tool ? "bound" : "UNBOUND"));
        for (const auto& [rid, e] : m_entries)
        {
            if (e.kind == Kind::Color)
                ETCS_LOG("PaintPalette", "  RID:" << rid << "  colour "
                         << e.rgba[0] << ", " << e.rgba[1] << ", " << e.rgba[2]);
            else
                ETCS_LOG("PaintPalette", "  RID:" << rid << "  radius " << e.radius);
        }
    }

private:
    enum class Kind : uint8_t { Color, Size, Tool, Zoom };
    struct Entry { Kind kind; float rgba[4]; float radius; PaintToolKind tool; };

    std::unordered_map<ETCS::RID, Entry> m_entries;
    PaintTool* m_tool = nullptr;
    // For the zoom entries only -- see AddZoom on why a view setting is mapped
    // by the same type that maps tool settings.
    PaintSurface* m_surface = nullptr;
    // The slot a colour wheel would replace -- see lastColorNode.
    mutable ETCS::RID m_last_color = 0;
};

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
class PaintLayerPanel : public DeletableBase<PaintLayerPanel>
{
public:
    WIRE_TYPE_IDENTITY(PaintLayerPanel);

    PaintLayerPanel() = default;
    bool DeleteConcrete() override { return true; }

    enum class Region : uint8_t { Body, Eye, Label, Delete };

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
    void AddRow(ETCS::RID bg, ETCS::RID eye, ETCS::RID label, ETCS::RID del)
    {
        const size_t idx = m_rows.size();
        m_rows.push_back(Row{ bg, eye, label, del, 0 });
        if (bg)    m_regions[bg]    = Hit{ idx, Region::Body };
        if (eye)   m_regions[eye]   = Hit{ idx, Region::Eye };
        if (label) m_regions[label] = Hit{ idx, Region::Label };
        if (del)   m_regions[del]   = Hit{ idx, Region::Delete };
    }

    void SetHoverDim(float dim) { m_hover_dim = std::clamp(dim, 0.0f, 1.0f); }

    // Eye fills for the two states, and the row tint for selected / not. Given
    // by the script for the same reason the swatch colours are: this is a look.
    void SetEyeColors(float vr, float vg, float vb, float hr, float hg, float hb)
    {
        m_eye_shown[0] = vr; m_eye_shown[1] = vg; m_eye_shown[2] = vb;
        m_eye_hidden[0] = hr; m_eye_hidden[1] = hg; m_eye_hidden[2] = hb;
    }

    void SetRowColors(float sr, float sg, float sb, float ur, float ug, float ub)
    {
        m_row_sel[0] = sr; m_row_sel[1] = sg; m_row_sel[2] = sb;
        m_row_idle[0] = ur; m_row_idle[1] = ug; m_row_idle[2] = ub;
    }

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

        const size_t total = stack.size();
        for (size_t i = 0; i < m_rows.size(); ++i)
        {
            Row& row = m_rows[i];
            const size_t from_top = i + static_cast<size_t>(m_scroll);
            PaintLayer* layer = (from_top < total)
                              ? stack[total - 1 - from_top] : nullptr;
            row.layer = layer ? layer->getRID() : 0;

            if (!layer)
            {
                // An empty slot is drawn as nothing rather than hidden: the
                // window is a fixed frame and a gap in it is honest.
                SetFill(row.bg,    m_row_idle[0], m_row_idle[1], m_row_idle[2], 0.0f);
                SetFill(row.eye,   0.0f, 0.0f, 0.0f, 0.0f);
                SetFill(row.del,   0.0f, 0.0f, 0.0f, 0.0f);
                SetText(row.label, "");
                continue;
            }

            const bool selected = (m_document->activeLayer() == layer);
            SetFill(row.bg,
                    selected ? m_row_sel[0] : m_row_idle[0],
                    selected ? m_row_sel[1] : m_row_idle[1],
                    selected ? m_row_sel[2] : m_row_idle[2], 1.0f);
            SetFill(row.eye,
                    layer->visible() ? m_eye_shown[0] : m_eye_hidden[0],
                    layer->visible() ? m_eye_shown[1] : m_eye_hidden[1],
                    layer->visible() ? m_eye_shown[2] : m_eye_hidden[2], 1.0f);
            SetFill(row.del, 0.75f, 0.28f, 0.30f, 1.0f);
            SetText(row.label, layer->name().c_str());
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
    bool Apply(ETCS::RID node, bool is_press)
    {
        auto it = m_regions.find(node);
        if (it == m_regions.end()) return false;
        if (!is_press) return true;                // ours, but a release does nothing
        if (!m_document) return true;

        const Hit hit = it->second;
        Row& row = m_rows[hit.row];
        if (row.layer == 0) return true;           // an empty slot is still ours

        switch (hit.region)
        {
        case Region::Body:
            m_document->SetActiveLayer(row.layer);
            m_renaming = 0;
            // A press on a row body is also where a drag begins -- Drop below
            // is what ends it. Held as a RID so a restack between the two
            // cannot leave this pointing at a row that now means another layer.
            m_dragging = row.layer;
            break;

        case Region::Eye:
            if (ETCS::Entity* raw = paint_resolve_tag("PaintLayer", row.layer))
                static_cast<PaintLayer*>(raw->getTrueType())->ToggleVisible();
            m_renaming = 0;
            break;

        case Region::Label:
            if (m_renaming == row.layer)
            {
                ETCS_LOG("PaintLayerPanel", "rename armed on RID:" << row.layer
                         << " -- CommitRename <text> to set it.");
            }
            else
            {
                m_document->SetActiveLayer(row.layer);
                m_renaming = row.layer;
                m_dragging = row.layer;
            }
            break;

        case Region::Delete:
            m_document->RemoveLayer(row.layer);
            if (m_renaming == row.layer) m_renaming = 0;
            if (m_dragging == row.layer) m_dragging = 0;
            break;
        }
        Refresh();
        return true;
    }

    /*
 * THE DROP, which is the whole of reordering: the dragged layer takes the
 * depth of the row it was released over.
 *
 * Depth counted from the BOTTOM, because that is what PaintDocument::MoveLayerTo
 * takes and what the order key means; the row index counts from the top, so it
 * is turned around here rather than in the document -- one place converts
 * between the two conventions and it is the one that knows both.
 */
    bool Drop(ETCS::RID node)
    {
        if (m_dragging == 0) return false;
        auto it = m_regions.find(node);
        if (it == m_regions.end()) { m_dragging = 0; return false; }
        if (!m_document) { m_dragging = 0; return true; }

        /*
     * RELEASED OVER THE ROW IT STARTED ON IS NOT A DRAG, it is the second half
     * of a click. Without this a plain selection ends in a MoveLayerTo -- which
     * lands the layer back where it already was, so it looks harmless, and
     * quietly renumbers the whole stack on every click. That is the kind of
     * no-op that only becomes visible once something else depends on the
     * numbers not moving.
     */
        if (m_rows[it->second.row].layer == m_dragging) { m_dragging = 0; return true; }

        std::vector<PaintLayer*> stack;
        m_document->OrderedLayers(stack);
        const size_t total = stack.size();
        const size_t from_top = it->second.row + static_cast<size_t>(m_scroll);
        if (from_top < total)
        {
            const int32_t depth = static_cast<int32_t>(total - 1 - from_top);
            m_document->MoveLayerTo(m_dragging, depth);
        }
        m_dragging = 0;
        Refresh();
        return true;
    }

    /*
 * HOVER: the layer under the pointer at full strength, every other one dimmed.
 *
 * Stated to the document as one call over the whole stack (PaintDocument::
 * IsolateLayer) rather than as a dim per row, so leaving a row is the same call
 * with a different subject and there is no per-row bookkeeping to get wrong when
 * the pointer skips one. A node that is not ours clears the isolation, which is
 * what "the pointer left the panel" means without needing an exit event.
 */
    void Hover(ETCS::RID node)
    {
        if (!m_document) return;
        auto it = m_regions.find(node);
        const ETCS::RID subject = (it == m_regions.end()) ? 0 : m_rows[it->second.row].layer;
        if (subject == m_hovering) return;          // nothing changed; do not re-walk the stack
        m_hovering = subject;
        m_document->IsolateLayer(subject, m_hover_dim);
    }

    // The rename the label press armed. Separate from Apply because the text
    // does not come from the pointer -- it arrives from a key channel or from
    // the page, and the panel only has to know which layer it belongs to.
    bool CommitRename(const std::string& name)
    {
        if (m_renaming == 0 || !m_document) return false;
        m_document->RenameLayer(m_renaming, name);
        m_renaming = 0;
        Refresh();
        return true;
    }

    void Report() const
    {
        ETCS_LOG("PaintLayerPanel", m_rows.size() << " row(s), scroll " << m_scroll
                 << ", document " << (m_document ? "bound" : "UNBOUND")
                 << (m_renaming ? " [renaming]" : "")
                 << (m_dragging ? " [dragging]" : ""));
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
    // nowhere near this window", which is the call a pane-leave makes.
    void HoverRow(int32_t row)
    {
        if (!m_document) return;
        const ETCS::RID subject =
            (row >= 0 && static_cast<size_t>(row) < m_rows.size())
                ? m_rows[static_cast<size_t>(row)].layer : 0;
        if (subject == m_hovering) return;
        m_hovering = subject;
        m_document->IsolateLayer(subject, m_hover_dim);
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
        m_dragging = 0;
        HoverRow(-1);
    }

    size_t rowCount() const { return m_rows.size(); }
    ETCS::RID rowLayer(size_t i) const { return i < m_rows.size() ? m_rows[i].layer : 0; }
    int32_t scroll() const { return m_scroll; }

private:
    struct Row { ETCS::RID bg, eye, label, del; ETCS::RID layer; };
    struct Hit { size_t row; Region region; };

    /*
 * DRIVING SOMEBODY ELSE'S NODE, by the verb name the type exports.
 *
 * Entity::call with "<Tag>.<Work>" is the cross-module seam -- the same one
 * loaders/etcs.cc's etcs_web_call uses -- and it is what lets a panel restyle a
 * polygon it did not create and could not create (see this class's header
 * note). The tag is read off the entity rather than assumed, so a script may
 * build a row out of whatever drawable leaf it likes as long as that leaf
 * exports SetFill.
 *
 * Silent when the node is 0 or gone: a row that declared no eye has no eye to
 * colour, and a node deleted underneath us is the script's business, not an
 * error to raise once per frame.
 */
    static void SetFill(ETCS::RID node, float r, float g, float b, float a)
    {
        if (node == 0) return;
        // By FAMILY, because the concrete type is the script's choice: a row may
        // be built from any Drawable2D leaf that exports SetFill. Held for the
        // call, since it is somebody else's entity (core/Entity.h).
        ETCS::Held<Drawable2D_> node_e = ETCS::resolve_held<Drawable2D_>("Drawable2D", node);
        if (!node_e) return;
        ETCS::Entity* e = static_cast<ETCS::Entity*>(node_e.get());
        if (!e) return;
        ETCS::Buffer action;
        action.write((e->getSourceTag().toString() + ".SetFill").c_str());
        ETCS::Buffer payload;
        payload.write((std::to_string(r) + " " + std::to_string(g) + " "
                     + std::to_string(b) + " " + std::to_string(a)).c_str());
        try { e->call(action, payload); } catch (...) {}
    }

    static void SetText(ETCS::RID node, const char* text)
    {
        if (node == 0) return;
        ETCS::Held<Drawable2D_> node_e = ETCS::resolve_held<Drawable2D_>("Drawable2D", node);
        if (!node_e) return;
        ETCS::Entity* e = static_cast<ETCS::Entity*>(node_e.get());
        if (!e) return;
        ETCS::Buffer action;
        action.write((e->getSourceTag().toString() + ".SetText").c_str());
        ETCS::Buffer payload;
        payload.write(text);
        try { e->call(action, payload); } catch (...) {}
    }

    PaintDocument* m_document = nullptr;
    std::vector<Row> m_rows;
    std::unordered_map<ETCS::RID, Hit> m_regions;
    int32_t   m_scroll    = 0;
    ETCS::RID m_renaming  = 0;
    ETCS::RID m_dragging  = 0;
    ETCS::RID m_hovering  = 0;
    float m_hover_dim = 0.25f;
    float m_eye_shown[3]  = { 0.85f, 0.85f, 0.88f };
    float m_eye_hidden[3] = { 0.30f, 0.30f, 0.34f };
    float m_row_sel[3]    = { 0.28f, 0.30f, 0.38f };
    float m_row_idle[3]   = { 0.18f, 0.18f, 0.22f };
};

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

    // The router this wheel appears in, and the pane it appears as. Held by RID
    // because opening and closing are calls on somebody else's entity.
    void BindRouter(ETCS::RID router) { m_router = router; }
    void BindPane(ETCS::RID root, ETCS::RID input) { m_root = root; m_input = input; }

    void SetValue(float v) { m_value = std::clamp(v, 0.0f, 1.0f); }

    bool open() const { return m_open; }

    void Open()
    {
        if (m_open || m_router == 0 || m_root == 0) return;
        if (ETCS::Entity* raw = paint_resolve_tag("PaintRouter", m_router))
            route_add(raw);
        m_open = true;
        ETCS_LOG("PaintColorWheel", "opened over the canvas.");
    }

    void Close()
    {
        if (!m_open || m_router == 0) return;
        if (ETCS::Entity* raw = paint_resolve_tag("PaintRouter", m_router))
            route_remove(raw);
        m_open = false;
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
    bool Pick(int32_t x, int32_t y)
    {
        const double dx = x - m_cx, dy = y - m_cy;
        const double dist = std::sqrt(dx * dx + dy * dy);
        if (dist > m_radius) return false;

        const double PI = 3.14159265358979323846;
        double ang = std::atan2(dy, dx);              // -PI..PI
        if (ang < 0) ang += 2.0 * PI;
        const float h = static_cast<float>(ang / (2.0 * PI));
        const float sat = static_cast<float>(std::min(1.0, dist / m_radius));

        float r = 0, g = 0, b = 0;
        hsv_to_rgb(h, sat, m_value, r, g, b);
        Apply(r, g, b, 1.0f);
        return true;
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
            const ETCS::RID slot = m_palette->lastColorNode();
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

    void route_remove(ETCS::Entity* router)
    {
        ETCS::Buffer act;  act.write("PaintRouter.RemovePane");
        ETCS::Buffer arg;  arg.write(std::to_string(m_root).c_str());
        try { router->call(act, arg); } catch (...) {}
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
    float    m_value = 1.0f;
    bool     m_open  = false;
    ETCS::RID m_slot = 0;
    float    m_picked[4] = { 0, 0, 0, 1 };
};




class PaintInput : public DeletableBase<PaintInput>
{
public:
    WIRE_TYPE_IDENTITY(PaintInput);

    PaintInput() = default;
    bool DeleteConcrete() override { return true; }

    void BindDocument(ETCS::RID document)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintDocument", document);
        if (!raw) return;
        m_document = static_cast<PaintDocument*>(raw->getTrueType());
    }

    void BindTool(ETCS::RID tool)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintTool", tool);
        if (!raw) return;
        m_tool = static_cast<PaintTool*>(raw->getTrueType());
    }

    void BindSurface(ETCS::RID surface)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintSurface", surface);
        if (!raw) return;
        m_surface = static_cast<PaintSurface*>(raw->getTrueType());
    }

    void SetBrush(float radius, float r, float g, float b, float a)
    {
        if (!m_tool) return;
        m_tool->SetRadius(radius);
        m_tool->SetColor(r, g, b, a);
    }

    /*
 * ── ROUTING ──────────────────────────────────────────────────────────────
 *
 * THE INPUT CHANNEL SPEAKS ONE FRAME AND EVERY TARGET SPEAKS ITS OWN.
 *
 * A window reports a pointer in content-area pixels. That was enough while
 * the canvas WAS the window -- the two frames coincided, so ConsumeInput
 * could take ev.x/ev.y as canvas coordinates and paint. The moment anything
 * else is on screen the coincidence ends, and it ends silently: the toolbar
 * sits at the bottom of the window, a press on it is still "inside the
 * canvas" as far as raw coordinates go, and the editor paints a dot where
 * the user meant to pick a colour.
 *
 * So the event is ROUTED before it is interpreted. Pick the deepest node
 * under the point (Drawable2D_::PickAt, ontology/Drawable2D.h), which walks
 * the same ordered child list the renderer draws with -- so what receives the
 * event is what the user can actually see -- and hands back the point already
 * translated into that node's own space, because the descent computed it on
 * the way down.
 *
 * WHAT ARRIVES IS THEN ORDINARY. A palette node consumes the press as a
 * selection; the canvas gets an InputEvent identical in shape to the one a
 * bare window would have sent, differing only in that its coordinates now
 * mean what the canvas thinks coordinates mean. HandleEvent is untouched --
 * the stroke machine never learns that routing exists, which is what keeps it
 * testable without a window.
 *
 * Unrouted stays a valid configuration: with no root bound this is exactly
 * the old behaviour, which paint_surface.etcs still relies on.
 */
    /*
 * THE POINTER IN THE WINDOW'S FRAME, kept separately from m_cursor_x/y.
 *
 * Those two are the CANVAS's frame -- what the stroke machine integrates
 * against -- and are written with translated coordinates. The press channel
 * needs the untranslated position instead, because it has to be routed before
 * anyone knows which frame it belongs to. Two frames, two variables; sharing
 * one was the bug this design exists to avoid.
 */
    void NoteRoutedCursor(int32_t x, int32_t y) { m_win_x = x; m_win_y = y; }
    int32_t RoutedCursorX() const { return m_win_x; }
    int32_t RoutedCursorY() const { return m_win_y; }

    void BindRoot(ETCS::RID root)     { m_root = root; }
    void BindCanvas(ETCS::RID canvas) { m_canvas = canvas; }

    void BindPalette(ETCS::RID palette)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintPalette", palette);
        if (!raw) return;
        m_palette = static_cast<PaintPalette*>(raw->getTrueType());
    }

    // Bound the same way and consulted in the same place as the palette: both
    // answer "was this press a control rather than a stroke", and neither can
    // answer it without knowing where the press landed. See RouteEvent.
    void BindPanel(ETCS::RID panel)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintLayerPanel", panel);
        if (!raw) return;
        m_panel = static_cast<PaintLayerPanel*>(raw->getTrueType());
    }

    /*
 * THE POINTER IS NO LONGER OVER THIS PANE, said by the router rather than
 * inferred here.
 *
 * A pane that does not contain the point is never offered the event (the pass
 * budget only walks panes that do), so leaving cannot be noticed from the
 * inside -- the last thing a pane sees is the motion that was still over it.
 * Anything holding state that only makes sense while the pointer is present
 * gets to drop it here.
 *
 * The stroke's own continuity flag goes with it, for the reason RouteEvent
 * already forgets the cursor on scenery: a stroke that left the pane and came
 * back would otherwise be joined by a straight line across wherever it went.
 */
    // Any leaf claiming Glyphs. Kept as a RID rather than a pointer because it
    // belongs to another module and may be deleted between two clicks.
    void BindGlyphs(ETCS::RID glyphs) { m_glyphs = glyphs; }

    /*
 * THE WHEEL THIS INPUT ANSWERS FOR.
 *
 * Bound on TWO inputs and meaning different things on each, which is worth
 * saying plainly. On the wheel's own pane it is the thing a pick goes to. On the
 * CANVAS pane it is the thing a press dismisses -- "clicking anywhere on the
 * canvas closes it" is a statement about the canvas, so the canvas is where it
 * is implemented, and a press that closes a wheel is swallowed rather than also
 * starting a stroke.
 */
    void BindWheel(ETCS::RID wheel)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintColorWheel", wheel);
        if (raw) m_wheel = static_cast<PaintColorWheel*>(raw->getTrueType());
    }

    // Whether this input's pane IS the wheel (pick) or merely dismisses it.
    void SetWheelPane(bool is_wheel) { m_is_wheel_pane = is_wheel; }

    void RouteLeave()
    {
        m_cursor_seen = false;
        if (m_panel) m_panel->Left();
    }

    void RouteEvent(const InputEvent& ev)
    {
        /*
     * BEFORE THE PICK, because a scroll has no point to pick with -- its x/y
     * are the delta. The router already decided this pane is the one under the
     * pointer, which is the whole of the routing decision a wheel needs; what
     * is left is a view change on this pane's surface, and that is not a
     * question about which NODE was hit.
     */
        if (ev.action == INPUT_SCROLL) { HandleEvent(ev); return; }

        if (m_root == 0) { HandleEvent(ev); return; }   // unrouted: the old path

        // Held for the walk, not merely resolved: PickAt descends somebody
        // else's tree, so the answer has to stay true for the whole descent
        // rather than for the instant it was given (core/Entity.h).
        ETCS::Held<Drawable2D_> root = ETCS::resolve_held<Drawable2D_>("Drawable2D", m_root);
        if (!root)
        {
            ETCS_LOG("PaintInput", "routing root RID:" << m_root
                     << " is gone or going -- dropping the event.");
            return;
        }

        const Pick2D hit = root->PickAt(Point2D{ ev.x, ev.y });
        if (!hit) return;                       // outside the tree entirely
        const ETCS::RID hit_rid = hit.node->getRID();

        /*
     * A PRESS ON THE PALETTE IS NOT A STROKE, and saying so HERE rather than
     * in the palette is deliberate: what a press means is a property of where
     * it landed, and this is the only place that knows both.
     */
        const bool is_press   = (ev.action == INPUT_DOWN || ev.action == INPUT_BUTTON_DOWN);
        const bool is_release  = (ev.action == INPUT_UP   || ev.action == INPUT_BUTTON_UP);

        if (m_palette && is_press && m_palette->Apply(hit_rid))
        {
            m_on_palette = true;
            return;
        }
        if (is_release && m_on_palette)
        {
            m_on_palette = false;               // the press that ended was a selection
            return;
        }

        /*
     * THE LAYER PANEL, consulted exactly where the palette is and for the same
     * reason -- what a press means is a property of where it landed, and this is
     * the only place that knows both.
     *
     * Three edges rather than the palette's two, because a panel row is
     * draggable: a press selects or toggles, a release over a row is a DROP that
     * restacks, and motion is a hover. The hover goes through unconditionally so
     * that moving OFF the panel clears the isolation -- PaintLayerPanel::Hover
     * reads a node it does not own as "nobody", which is what leaving means
     * without an exit event to carry it.
     */
        /*
     * THE WHEEL, ahead of the panel and the palette because it is ON TOP of
     * them: while it is open it is the topmost pane, and a press that reaches
     * any other pane at all is a press outside it.
     */
        if (m_wheel && is_press)
        {
            if (m_is_wheel_pane)
            {
                // Inside the disc picks and closes; outside it, on the popup's
                // own backing, just closes. Either way the press is spent.
                m_wheel->Pick(hit.local.x, hit.local.y);
                m_wheel->Close();
                return;
            }
            if (m_wheel->open())
            {
                // A press anywhere else dismisses and does NOT also draw --
                // otherwise every dismissal leaves a dab where the user was
                // only trying to put the popup away.
                m_wheel->Close();
                return;
            }
        }

        if (m_panel)
        {
            if (ev.action == INPUT_MOTION) m_panel->Hover(hit_rid);
            if (is_release && m_panel->Drop(hit_rid)) { m_on_panel = false; return; }
            if (is_press && m_panel->Apply(hit_rid, true)) { m_on_panel = true; return; }
            if (is_release && m_on_panel) { m_on_panel = false; return; }
        }

        /*
     * Anything that is not the canvas is scenery -- drawable over the picture
     * without becoming part of it. Forgetting the cursor on the way out
     * matters: a stroke that began on the canvas, left over the toolbar and
     * came back would otherwise be joined by a straight line across the gap.
     */
        if (m_canvas != 0 && hit_rid != m_canvas)
        {
            if (ev.action == INPUT_MOTION) m_cursor_seen = false;
            return;
        }

        InputEvent local = ev;
        local.x = static_cast<int16_t>(hit.local.x);
        local.y = static_cast<int16_t>(hit.local.y);
        HandleEvent(local);
    }

    // Drive the tool's stroke state machine from ONE input event, commit the
    // sample to the active layer, and stamp live feedback onto the bound view
    // surface. One event rather than a buffer of them, because the stream hands
    // them over one at a time (ConsumeInput below).
    //
    // Public because it is the seam a test can reach: the stream path needs a
    // live producer, and the state machine is worth asserting on without one.
    /*
 * ── view space and document space ────────────────────────────────────────
 *
 * Everything arriving here is in VIEW pixels -- that is what a window reports
 * and what PickAt translated into the pane's frame. Everything that MARKS is in
 * DOCUMENT pixels, because that is where the layers are. The projection between
 * them belongs to the surface (PaintSurface), so these two are the only place
 * the conversion happens and every marking path below is document-space by the
 * time it runs.
 *
 * Unprojected when there is no surface bound, which is the identity and is what
 * the headless tests and paint_surface.etcs both rely on.
 */
    int32_t to_doc_x(int32_t vx) const { return m_surface ? m_surface->ViewToDocX(vx) : vx; }
    int32_t to_doc_y(int32_t vy) const { return m_surface ? m_surface->ViewToDocY(vy) : vy; }

    void HandleEvent(const InputEvent& ev)
    {
        /*
     * ── the right button ─────────────────────────────────────────────────
     *
     * It does not draw. It CLEARS whatever gesture is in flight and then pans,
     * which are two halves of one idea: the right button is how you get out of
     * something and move somewhere else, and neither of those should leave a
     * mark. An anchored shape abandoned this way is abandoned completely --
     * CancelStroke rather than EndStroke, so nothing is committed.
     *
     * Handled before anything else because it must not fall through: reaching
     * the ordinary paths with a right button held is how a pan ends up drawing
     * a line across the picture.
     */
        if ((ev.action == INPUT_BUTTON_DOWN || ev.action == INPUT_BUTTON_UP)
            && ev.key == PAINT_BUTTON_RIGHT)
        {
            if (ev.action == INPUT_BUTTON_DOWN)
            {
                m_panning  = true;
                m_pan_from_x = ev.x;
                m_pan_from_y = ev.y;
                if (m_tool && m_tool->active()) m_tool->CancelStroke();
                repaint_view();          // drop any preview the cancelled gesture left
            }
            else
            {
                m_panning = false;
            }
            return;
        }
        if (ev.action == INPUT_MOTION && m_panning)
        {
            // In VIEW pixels, unscaled: dragging the sheet should move it one
            // screen pixel per screen pixel of hand movement at every zoom,
            // which is what makes panning feel like dragging paper.
            if (m_surface)
            {
                m_surface->PanBy(ev.x - m_pan_from_x, ev.y - m_pan_from_y);
                repaint_view();
                static int n = 0;
                if ((n++ % 8) == 0)
                    ETCS_LOG("PaintInput", "pan " << m_surface->panX()
                             << "," << m_surface->panY());
            }
            m_pan_from_x = ev.x;
            m_pan_from_y = ev.y;
            m_cursor_seen = false;       // the stroke's continuity does not survive a pan
            return;
        }

        /*
     * ── the wheel ────────────────────────────────────────────────────────
     *
     * ZOOM ABOUT THE POINTER, which is the only thing a wheel can sensibly mean
     * over a projected document -- zooming about anything else walks what you
     * were looking at off the edge.
     *
     * The notch arrives on the pointer ring with its delta in x/y
     * (ontology/InputSource.h::INPUT_SCROLL), and the position to hold fixed is
     * the last one routed -- a wheel carries no position of its own, for the
     * same reason a button press used to carry none.
     *
     * IN VIEW SPACE, unconverted: ZoomAt solves the pan so that the document
     * point currently under that view point stays under it, so handing it a
     * document coordinate would hold the wrong thing still.
     */
        if (ev.action == INPUT_SCROLL)
        {
            if (!m_surface) return;
            const float factor = (ev.y > 0 || ev.x > 0) ? 1.25f : 0.8f;
            m_surface->ZoomBy(factor, RoutedCursorX(), RoutedCursorY());
            m_surface->Render();
            ETCS_LOG("PaintInput", "wheel -> " << m_surface->zoomPercent() << "%"
                     << " about " << RoutedCursorX() << "," << RoutedCursorY());
            return;
        }

        if (ev.action == INPUT_MOTION)
        {
            // TAKEN, NOT ACCUMULATED. The event carries where the pointer is,
            // in content-area pixels -- the same space the canvas is in -- so
            // there is nothing to integrate and nothing to drift.
            m_cursor_x = to_doc_x(ev.x);
            m_cursor_y = to_doc_y(ev.y);
            m_cursor_seen = true;

            if (m_tool && m_tool->active())
            {
                m_tool->MoveStroke(m_cursor_x, m_cursor_y);
                /*
             * WHICH OF THE THREE THINGS A DRAG DOES, decided by the tool's kind
             * rather than by a flag the input edge keeps.
             *
             * An ANCHORED tool has not made a mark yet and must not: what it
             * shows while the pointer moves is a PREVIEW on the view surface,
             * wiped by the next composite, so dragging out a rectangle does not
             * leave forty rectangles behind it. The commit happens once, on
             * release (see below), which is also what makes such a stroke a
             * single undoable thing rather than a smear of them.
             *
             * A CONTINUOUS tool marks here, because that is what continuous
             * means -- and interpolates, because the pointer is sampled once per
             * poll pass and a hand moves further than one pixel in that time.
             * Stamping only where samples land gives a dotted line at any speed
             * above a crawl; joining consecutive samples is what makes a stroke
             * a stroke.
             *
             * A PLACED tool did its work on the press and ignores the drag
             * entirely -- dragging after a flood fill is not a second fill.
             */
                const PaintToolKind k = m_tool->kind();
                if (paint_kind_is_anchored(k))
                    preview_anchored(k, m_tool->anchorX(), m_tool->anchorY(),
                                     m_cursor_x, m_cursor_y);
                else if (k == PaintToolKind::Smudge)
                    apply_smudge(m_last_x, m_last_y, m_cursor_x, m_cursor_y);
                else if (!paint_kind_is_placed(k))
                    apply_segment(m_last_x, m_last_y, m_cursor_x, m_cursor_y);
            }
            m_last_x = m_cursor_x;
            m_last_y = m_cursor_y;
        }
        else if (ev.action == INPUT_DOWN || ev.action == INPUT_BUTTON_DOWN)
        {
            /*
         * A BUTTON BRINGS ITS OWN POSITION (ontology/InputSource.h), so it
         * does not have to wait for one to have arrived -- and should not,
         * since a click on a fresh window is a perfectly ordinary first
         * event. A key press still does, because a key knows nothing about
         * where the pointer is.
         */
            if (ev.action == INPUT_BUTTON_DOWN)
            {
                m_cursor_x = to_doc_x(ev.x);
                m_cursor_y = to_doc_y(ev.y);
                m_cursor_seen = true;
            }
            if (m_tool && m_cursor_seen)
            {
                m_tool->BeginStroke(m_cursor_x, m_cursor_y);
                m_last_x = m_cursor_x;
                m_last_y = m_cursor_y;

                const PaintToolKind k = m_tool->kind();
                // A PLACED tool is finished here: one point is the whole
                // gesture, so it commits on the press and the release that
                // follows has nothing left to do.
                if (paint_kind_is_placed(k))       apply_placed(k, m_cursor_x, m_cursor_y);
                // An anchored tool marks nothing yet -- the press is only where
                // the shape starts. Smudge likewise: it carries pixels from
                // somewhere, and on the first sample there is no somewhere.
                else if (!paint_kind_is_anchored(k) && k != PaintToolKind::Smudge)
                    apply_sample(m_cursor_x, m_cursor_y);
            }
        }
        else if (ev.action == INPUT_UP || ev.action == INPUT_BUTTON_UP)
        {
            /*
         * THE COMMIT, and the only place an anchored tool ever writes to the
         * document. Read the anchor before EndStroke, which is what clears the
         * gesture.
         *
         * Ruler commits nothing by construction (paint_kind_commits), so
         * letting go of it simply removes the measurement -- which is what a
         * ruler you have finished with should do.
         */
            if (m_tool)
            {
                const PaintToolKind k = m_tool->kind();
                if (m_tool->active() && paint_kind_is_anchored(k) && paint_kind_commits(k))
                    commit_anchored(k, m_tool->anchorX(), m_tool->anchorY(),
                                    m_cursor_x, m_cursor_y);
                m_tool->EndStroke();
                // The preview lives on the view surface, so whatever the drag
                // drew there has to go whether or not anything was committed.
                if (paint_kind_is_anchored(k)) repaint_view();
            }
        }
    }

    /*
 * THE SAME EVENTS, FROM A SCRIPT. Pointer/Press/Release build an InputEvent and
 * hand it to HandleEvent -- the identical path a real device takes, one
 * function call later in it.
 *
 * Worth exporting on its own merits rather than only for the test: a paint
 * program wants scripted strokes (macros, replay, a demo that draws itself),
 * and this is what that is. It also makes the stroke state machine reachable
 * without a producer, which is the difference between a state machine that is
 * asserted on and one that is only ever watched.
 *
 * Deliberately NOT a second implementation. Anything these did differently
 * from the stream path would be a test of the wrong thing.
 *
 * WHICH IS WHY THEY ROUTE. These called HandleEvent directly, so a machine with
 * a root bound routed everything that arrived on an edge and nothing that
 * arrived from a script -- and routing is a property of how the machine is
 * CONFIGURED (BindRoot), not of which transport delivered the event. RouteEvent
 * falls through to HandleEvent when no root is bound, so an unrouted machine
 * behaves exactly as it did.
 *
 * It is also what makes a browser toolbar work at all: a canvas beside the
 * window is a separate DOM element, its clicks are not in the window's stream,
 * and the page hands them here through PaintInput.Pointer/Press
 * (etcs_web_call, loaders/etcs.cc).
 */
    void ScriptPointer(int32_t x, int32_t y)
    {
        InputEvent ev{};
        ev.action = INPUT_MOTION;
        ev.x = static_cast<int16_t>(x);
        ev.y = static_cast<int16_t>(y);
        // Both halves of what the routed consumer does with a position, in the
        // same order: record it as the routed cursor (a press carries no
        // coordinates and routes on the last position THIS path delivered --
        // see NoteRoutedCursor) and then route it.
        NoteRoutedCursor(ev.x, ev.y);
        RouteEvent(ev);
    }

    /*
     * A PRESS CARRIES NO POSITION -- x/y are meaningful for INPUT_MOTION only
     * (ontology/InputSource.h) -- so routing one on its own coordinates picks
     * whatever sits at the origin, which is nothing. The last position this path
     * delivered is where the press happened, which is the same assumption the
     * routed key edge makes (ConsumeRouted) and the same one absolute positions
     * make safe. Filled in here so the two paths route a press identically.
     */
    void ScriptPress()
    {
        InputEvent ev{};
        ev.action = INPUT_DOWN;
        ev.x = static_cast<int16_t>(RoutedCursorX());
        ev.y = static_cast<int16_t>(RoutedCursorY());
        RouteEvent(ev);
    }

    void ScriptRelease()
    {
        InputEvent ev{};
        ev.action = INPUT_UP;
        ev.x = static_cast<int16_t>(RoutedCursorX());
        ev.y = static_cast<int16_t>(RoutedCursorY());
        RouteEvent(ev);
    }

    bool StrokeActive() const { return m_tool && m_tool->active(); }

    PaintDocument* document() const { return m_document; }
    PaintTool* tool() const { return m_tool; }
    PaintSurface* surface() const { return m_surface; }
    int32_t cursorX() const { return m_cursor_x; }
    int32_t cursorY() const { return m_cursor_y; }

private:
    void apply_sample(int32_t x, int32_t y)
    {
        if (!m_tool) return;
        const PaintBrushState& brush = m_tool->brush();
        if (m_document) m_document->ApplyBrush(x, y, brush);
        if (m_surface) m_surface->StampBrush(x, y, brush);
    }

    // Stamp along the segment between two samples, spaced so consecutive
    // stamps overlap by half a radius. Spacing by the BRUSH rather than by a
    // fixed step means a wide brush costs no more stamps than it needs and a
    // one-pixel brush still draws a continuous line.
    void apply_segment(int32_t x0, int32_t y0, int32_t x1, int32_t y1)
    {
        if (!m_tool) return;
        const float step = std::max(1.0f, m_tool->brush().radius_px * 0.5f);
        const float dx = static_cast<float>(x1 - x0);
        const float dy = static_cast<float>(y1 - y0);
        const int   n  = static_cast<int>(std::sqrt(dx * dx + dy * dy) / step);

        for (int i = 1; i <= n; ++i)
        {
            const float t = static_cast<float>(i) / static_cast<float>(n + 1);
            apply_sample(x0 + static_cast<int32_t>(dx * t),
                         y0 + static_cast<int32_t>(dy * t));
        }
        apply_sample(x1, y1);
    }

    /*
 * ── the four gesture shapes ──────────────────────────────────────────────
 *
 * PREVIEW GOES TO THE VIEW, COMMIT GOES TO THE DOCUMENT, and that split is what
 * makes an anchored tool possible at all. apply_sample writes to both because a
 * freehand mark is final the instant it is made; a shape is not final until the
 * button comes up, so everything before that lands only on the surface the next
 * composite overwrites (PaintSurface::Render re-blits every layer).
 */
    void preview_anchored(PaintToolKind kind, int32_t ax, int32_t ay,
                          int32_t bx, int32_t by)
    {
        if (!m_surface) return;
        repaint_view();                       // wipe the previous frame's preview
        const ETCS::RID target = m_surface->target();
        if (target == 0) return;
        Surface_* view = ETCS::resolve_in_family<Surface_>("Surface", target);
        if (!view || !m_tool) return;

        /*
     * BACK INTO VIEW SPACE TO DRAW IT. The anchor and the cursor are document
     * coordinates (everything past HandleEvent's conversion is), and the preview
     * goes onto the view surface -- so the projection runs the other way here.
     * The nib width scales with it for the same reason the live stamp does: a
     * preview drawn at document width would not match the mark it is previewing.
     */
        const float z = m_surface->zoom();
        const int32_t vax = m_surface->DocToViewX(ax), vay = m_surface->DocToViewY(ay);
        const int32_t vbx = m_surface->DocToViewX(bx), vby = m_surface->DocToViewY(by);
        const PaintColor& c = m_tool->brush().color;
        const int w = std::max(1, static_cast<int>(m_tool->brush().radius_px * z));
        switch (kind)
        {
        case PaintToolKind::Line:
            preview_line(view, vax, vay, vbx, vby, c, w);
            break;
        case PaintToolKind::Rect:
            preview_line(view, vax, vay, vbx, vay, c, w);
            preview_line(view, vbx, vay, vbx, vby, c, w);
            preview_line(view, vbx, vby, vax, vby, c, w);
            preview_line(view, vax, vby, vax, vay, c, w);
            break;
        case PaintToolKind::Ellipse:
            preview_ellipse(view, vax, vay, vbx, vby, c, w);
            break;
        case PaintToolKind::Ruler:
            preview_ruler(view, vax, vay, vbx, vby, ax, ay, bx, by, z);
            break;
        default: break;
        }
        paint_mark_pixel_path(target);
    }

    void commit_anchored(PaintToolKind kind, int32_t ax, int32_t ay,
                         int32_t bx, int32_t by)
    {
        if (!m_document) return;
        PaintLayer* layer = m_document->activeLayer();
        if (!layer || !m_tool) return;
        const PaintBrushState& brush = m_tool->brush();

        switch (kind)
        {
        case PaintToolKind::Line:    layer->StrokeLine(ax, ay, bx, by, brush); break;
        case PaintToolKind::Rect:    layer->DrawRectOutline(ax, ay, bx, by, brush); break;
        case PaintToolKind::Ellipse: layer->DrawEllipseOutline(ax, ay, bx, by, brush); break;
        default: break;
        }
    }

    /*
 * A PLACED TOOL IS ONE CLICK AND ONE COMMIT, straight to the document -- there
 * is no drag to preview and nothing to take back on release.
 */
    void apply_placed(PaintToolKind kind, int32_t x, int32_t y)
    {
        if (!m_document || !m_tool) return;
        PaintLayer* layer = m_document->activeLayer();
        if (!layer) return;

        if (kind == PaintToolKind::Fill)
        {
            const size_t n = layer->FloodFill(x, y, m_tool->brush().color,
                                              m_tool->tolerance());
            ETCS_LOG("PaintInput", "fill at " << x << "," << y << " -> " << n << " px");
        }
        else if (kind == PaintToolKind::Glyph)
        {
            place_glyphs(layer, x, y);
        }
        repaint_view();
    }

    /*
 * TEXT GOES INTO THE LAYER'S PIXELS, not beside it as another node.
 *
 * Glyphs_::RasterizeText writes into whatever owns pixels, named by RID -- which
 * a PaintLayer now is (it claims Pixels_), so a placed glyph run becomes part of
 * the picture and is erased, filled, smudged and undone like anything else in
 * it. A TextLabel node would have been easier and would have produced text that
 * floats above the painting forever.
 *
 * The provider is whatever the script bound -- any leaf claiming Glyphs, which
 * today means RenderProvider's TextLabel. Measured first so the run can be
 * CENTRED on the click: placing text by its top-left corner means aiming at a
 * spot and watching the text appear somewhere below and to the right of it.
 */
    void place_glyphs(PaintLayer* layer, int32_t x, int32_t y)
    {
        if (m_glyphs == 0) { ETCS_LOG("PaintInput", "glyph tool with no glyph "
                                      "provider bound -- BindGlyphs first."); return; }
        ETCS::Held<Glyphs_> g = ETCS::resolve_held<Glyphs_>("Glyphs", m_glyphs);
        if (!g) { ETCS_LOG("PaintInput", "glyph provider RID:" << m_glyphs
                           << " is gone."); return; }

        const std::string& text = m_tool->text();
        const uint32_t px = m_tool->textSize();
        const TextExtent e = g->MeasureText(text.c_str(), 0, px);
        const PaintColor& c = m_tool->brush().color;
        g->RasterizeText(layer->getRID(), text.c_str(), 0, px,
                         x - static_cast<int32_t>(e.width) / 2,
                         y - static_cast<int32_t>(e.height) / 2,
                         c.r, c.g, c.b, c.a);
        etcs_mark_observed(layer);
    }

    // Smudge carries pixels, so it needs both ends of the step rather than one
    // point -- see PaintLayer::SmudgeDab. Strength is fixed rather than exposed
    // until there is a control for it; hardness is the closest existing dial and
    // means something else.
    void apply_smudge(int32_t x0, int32_t y0, int32_t x1, int32_t y1)
    {
        if (!m_document || !m_tool) return;
        PaintLayer* layer = m_document->activeLayer();
        if (!layer) return;
        layer->SmudgeDab(x0, y0, x1, y1, m_tool->brush(), 0.6f);
        repaint_view();
    }

    // Re-composite the document onto the view. What makes a preview a preview:
    // the view is rebuilt from the layers, so anything drawn straight onto it
    // since the last one is gone.
    void repaint_view()
    {
        if (m_surface) m_surface->Render();
    }

    // A preview segment, as a run of small rects rather than brush dabs -- the
    // view surface is somebody else's and DrawRect is the primitive every
    // Surface has. Thickness follows the nib so the preview reads as the same
    // weight the commit will land at.
    static void preview_line(Surface_* view, int32_t x0, int32_t y0,
                             int32_t x1, int32_t y1, const PaintColor& c, int w)
    {
        const int dx = std::abs(x1 - x0), dy = std::abs(y1 - y0);
        const int steps = std::max(1, std::max(dx, dy) / std::max(1, w / 2));
        for (int i = 0; i <= steps; ++i)
            view->DrawRect(x0 + (x1 - x0) * i / steps - w / 2,
                           y0 + (y1 - y0) * i / steps - w / 2,
                           static_cast<uint32_t>(w), static_cast<uint32_t>(w),
                           c.r, c.g, c.b, c.a);
    }

    static void preview_ellipse(Surface_* view, int32_t x0, int32_t y0,
                                int32_t x1, int32_t y1, const PaintColor& c, int w)
    {
        const double cx = (x0 + x1) * 0.5, cy = (y0 + y1) * 0.5;
        const double rx = std::abs(x1 - x0) * 0.5, ry = std::abs(y1 - y0) * 0.5;
        const int steps = std::clamp(static_cast<int>(std::max(rx, ry)), 24, 512);
        for (int i = 0; i <= steps; ++i)
        {
            const double t = (2.0 * 3.14159265358979323846 * i) / steps;
            view->DrawRect(static_cast<int32_t>(cx + rx * std::cos(t)) - w / 2,
                           static_cast<int32_t>(cy + ry * std::sin(t)) - w / 2,
                           static_cast<uint32_t>(w), static_cast<uint32_t>(w),
                           c.r, c.g, c.b, c.a);
        }
    }

    /*
 * THE RULER, which commits nothing and is therefore entirely this function.
 *
 * Two things are drawn: the measured span, and TICKS along it. The ticks are
 * what make it a ruler rather than a line with a length -- they are placed every
 * 10 device pixels with every tenth drawn long, so the pane's own size and width
 * can be read off it directly by counting, which is what it is for.
 *
 * The span's length is reported to the log as well, because a number you can
 * read is worth more than a number you have to count to, and the marks are for
 * aligning things rather than for arithmetic.
 */
    void preview_ruler(Surface_* view, int32_t vax, int32_t vay, int32_t vbx, int32_t vby,
                       int32_t ax, int32_t ay, int32_t bx, int32_t by, float zoom)
    {
        const PaintColor guide{ 0.95f, 0.80f, 0.20f, 0.85f };
        preview_line(view, vax, vay, vbx, vby, guide, 1);

        /*
     * TICKS ARE SPACED IN DOCUMENT PIXELS AND DRAWN IN VIEW PIXELS, which is the
     * whole of making a ruler projection-aware. A tick every 10 view pixels would
     * measure the screen; what a ruler is for is measuring the PICTURE, so the
     * spacing is 10 document pixels and it visibly opens out as you zoom in.
     *
     * The reported length is likewise the document's, so the number a ruler
     * gives you is the number a stroke would be -- it does not change when you
     * scroll the wheel, which would make it useless for the one job it has.
     */
        const double ddx = bx - ax, ddy = by - ay;
        const double doc_len = std::sqrt(ddx * ddx + ddy * ddy);
        const double vdx = vbx - vax, vdy = vby - vay;
        const double view_len = std::sqrt(vdx * vdx + vdy * vdy);
        if (view_len < 1.0 || doc_len < 1.0) return;

        const double ux = vdx / view_len, uy = vdy / view_len;
        const double nx = -uy, ny = ux;           // unit normal, for the tick marks
        const double spacing = std::max(2.0, 10.0 * zoom);   // 10 document px, on screen

        int mark = 0;
        for (double d = 0.0; d <= view_len; d += spacing, ++mark)
        {
            const bool major = (mark % 10) == 0;   // every 100 document pixels
            const double t = major ? 7.0 : 3.0;
            const double px0 = vax + ux * d, py0 = vay + uy * d;
            preview_line(view,
                         static_cast<int32_t>(px0 - nx * t), static_cast<int32_t>(py0 - ny * t),
                         static_cast<int32_t>(px0 + nx * t), static_cast<int32_t>(py0 + ny * t),
                         guide, 1);
        }
        ETCS_LOG("PaintInput", "ruler " << ax << "," << ay << " -> " << bx << "," << by
                 << "  dx=" << static_cast<int32_t>(ddx)
                 << " dy=" << static_cast<int32_t>(ddy)
                 << " len=" << static_cast<int32_t>(doc_len + 0.5) << "px (document)"
                 << " at " << static_cast<int32_t>(std::lround(zoom * 100.0f)) << "%");
    }

    PaintDocument* m_document = nullptr;
    PaintTool* m_tool = nullptr;
    PaintSurface* m_surface = nullptr;
    // Routing, all optional. Zero means "not routed", which is the
    // pre-toolbar behaviour and still what paint_surface.etcs wants.
    PaintPalette* m_palette = nullptr;
    ETCS::RID m_root   = 0;   // the frame the input channel speaks in
    ETCS::RID m_canvas = 0;   // the one node whose frame is the picture's
    bool      m_on_palette = false;
    int32_t   m_win_x = 0;    // the pointer in the WINDOW's frame -- see
    int32_t   m_win_y = 0;    // NoteRoutedCursor for why this is not m_cursor_x.
    int32_t m_cursor_x = 0;
    int32_t m_cursor_y = 0;
    // The previous sample, for joining one to the next.
    int32_t m_last_x = 0;
    int32_t m_last_y = 0;
    // Whether a position has arrived at all -- without it, a button press
    // before the first motion event would begin a stroke at the origin.
    bool    m_cursor_seen = false;
    PaintLayerPanel* m_panel = nullptr;
    // Whatever leaf claiming Glyphs the script bound -- RenderProvider's
    // TextLabel today. By RID and resolved per use, since it is another
    // module's entity (see place_glyphs).
    ETCS::RID m_glyphs = 0;
    PaintColorWheel* m_wheel = nullptr;
    bool    m_is_wheel_pane = false;
    bool    m_on_panel = false;
    // The right-button pan. Tracked in VIEW pixels because that is the frame a
    // drag is felt in -- see the motion branch.
    bool    m_panning = false;
    int32_t m_pan_from_x = 0;
    int32_t m_pan_from_y = 0;   // a press landed on the panel; its release is not a stroke
};

/*
 * ── PaintRouter ─────────────────────────────────────────────────────────
 *
 * WHO GETS THE EVENT WHEN TWO PANES BOTH WANT IT.
 *
 * A session with more than one pane -- a sheet and a tool strip, a strip and a
 * layer window -- has two things that both answer "the pointer is over me", and
 * two PaintInputs with no arbiter between them is not a division of labour, it
 * is both of them acting. That was observable: a press on the strip picked a
 * colour AND painted a dab at the strip's coordinates, because the only thing
 * standing between them was PaintInput's own "is this my canvas" guard, which
 * one pane cannot state on another's behalf.
 *
 * So the arbitration is here, and it is the ordering the panes already have.
 * Panes are walked by their root's Drawable_::Order(), HIGHEST FIRST -- the
 * reverse of the composite, which is what "topmost" means and the same rule
 * Drawable2D_::PickAt uses inside one tree. One relation decides what is drawn
 * on top, what a click inside a tree hits, and which pane owns a click: they
 * cannot disagree, because there is nothing to keep in step.
 *
 * THE BUDGET IS THE PASS-THROUGH. An event carries a number of panes it may be
 * consumed by. Each pane whose bounds contain the point takes one and delivery
 * stops at zero, so a budget of 1 is "the topmost pane that contains it, and
 * nobody below" -- the strip eats its own click -- and 2 is "the top two", which
 * is how a pane becomes an overlay that watches without blocking. Containment,
 * not interest: a pane that is under the point has had its turn whether or not
 * it did anything with it, because "I was not interested" is a claim only the
 * pane can make and making delivery depend on it means one pane's indifference
 * silently re-enables another's.
 *
 * NOT ORDERABLE ITSELF. The router stands in no order; it reads one. Its panes'
 * roots are ordinary Drawable2Ds in their own trees, so a script arranges the
 * priority with the same SetOrder it already uses for a swatch.
 */


class PaintRouter : public DeletableBase<PaintRouter>
{
public:
    WIRE_TYPE_IDENTITY(PaintRouter);

    PaintRouter() = default;
    bool DeleteConcrete() override { return true; }

    bool Create(uint32_t passes = 1)
    {
        m_panes.clear();
        m_budget = (passes == 0) ? 1u : passes;
        this->addTag("active");
        return true;
    }

    /*
 * A pane is a root to hit-test and an input to hand the hit to. Both by RID and
 * resolved per event rather than held: a pane can be deleted while the pointer
 * is moving over it, and a stale PaintInput* is the one failure this class
 * exists to be trusted through.
 */
    void AddPane(ETCS::RID root, ETCS::RID input)
    {
        if (root == 0 || input == 0) return;
        for (const Pane& p : m_panes)
            if (p.root == root && p.input == input) return;
        m_panes.push_back(Pane{ root, input });
    }

    void RemovePane(ETCS::RID root)
    {
        m_panes.erase(std::remove_if(m_panes.begin(), m_panes.end(),
                          [root](const Pane& p) { return p.root == root; }),
                      m_panes.end());
    }

    // How many panes an event may be consumed by. 0 is nonsense rather than
    // "drop everything", so it reads as 1 -- a router that delivers nothing is a
    // configuration mistake that looks exactly like a broken pointer.
    void SetPassBudget(uint32_t passes) { m_budget = (passes == 0) ? 1u : passes; }
    uint32_t passBudget() const { return m_budget; }

    /*
 * THE WALK. Highest Order() first, one unit of budget per pane that contains the
 * point, stop at zero.
 *
 * The order is read fresh per event from the roots themselves, so a script that
 * restacks a pane mid-session needs to tell this object nothing -- the same
 * reason Drawable_::Order() is a question rather than a cached field. A pane
 * whose root has gone is skipped and left in the list: the RID may be re-bound,
 * and quietly dropping panes would make a transient resolve failure permanent.
 *
 * Held for the walk, not merely resolved -- PickAt descends somebody else's tree
 * and the answer has to stay true for the whole descent (core/Entity.h).
 */
    void Route(const InputEvent& ev)
    {
        if (m_panes.empty()) return;

        /*
     * A SCROLL CARRIES A DELTA, NOT A POSITION, which makes it the one event
     * here that cannot say where it happened. Everything below -- containment,
     * the pick, the translation -- is a question about a POINT, so the point
     * used is the last one this router saw. That is not a fallback: a wheel
     * notch happens wherever the pointer already is, and asking it to carry a
     * position would mean inventing one.
     *
     * Kept as the router's own rather than read back from a pane, because the
     * router is what has seen every position regardless of which pane consumed
     * it.
     */
        const bool positional = (ev.action != INPUT_SCROLL);
        if (positional) { m_x = ev.x; m_y = ev.y; }
        const int32_t at_x = positional ? ev.x : m_x;
        const int32_t at_y = positional ? ev.y : m_y;

        // (Order, index) so the sort is on the number and the tie-break is the
        // order panes were added, matching every other ordered read here.
        std::vector<std::pair<int32_t, size_t>> ranked;
        ranked.reserve(m_panes.size());
        for (size_t i = 0; i < m_panes.size(); ++i)
        {
            ETCS::Held<Drawable_> root = ETCS::resolve_held<Drawable_>("Drawable", m_panes[i].root);
            ranked.emplace_back(root ? root->Order() : 0, i);
        }
        std::stable_sort(ranked.begin(), ranked.end(),
                         [](const std::pair<int32_t, size_t>& a,
                            const std::pair<int32_t, size_t>& b) { return a.first > b.first; });

        /*
     * LEAVING IS AN EDGE, AND ONLY THE ROUTER CAN SEE IT.
     *
     * A pane is offered an event only while it contains the point, so from
     * inside a pane the pointer never leaves -- it simply stops arriving, which
     * is indistinguishable from the pointer standing still. Anything that holds
     * state for the duration of a hover would keep it forever.
     *
     * So the transition is reported from here, where both halves are known: a
     * pane that contained the LAST position and does not contain this one is
     * told once, on the motion that crossed the boundary. Motion only -- a
     * press does not move the pointer, so it cannot cross anything.
     */
        if (ev.action == INPUT_MOTION)
        {
            for (Pane& pane : m_panes)
            {
                ETCS::Held<Drawable2D_> root = ETCS::resolve_held<Drawable2D_>("Drawable2D", pane.root);
                const bool inside = root && root->ContainsLocal(at_x, at_y);
                if (pane.inside && !inside)
                {
                    if (ETCS::Entity* raw = paint_resolve_tag("PaintInput", pane.input))
                        static_cast<PaintInput*>(raw->getTrueType())->RouteLeave();
                }
                pane.inside = inside;
            }
        }

        uint32_t left = m_budget;
        for (const auto& r : ranked)
        {
            if (left == 0) break;
            const Pane& pane = m_panes[r.second];

            ETCS::Held<Drawable2D_> root = ETCS::resolve_held<Drawable2D_>("Drawable2D", pane.root);
            if (!root) continue;
            if (!root->ContainsLocal(at_x, at_y)) continue;

            ETCS::Entity* raw = paint_resolve_tag("PaintInput", pane.input);
            if (!raw) continue;
            auto* input = static_cast<PaintInput*>(raw->getTrueType());
            if (!input) continue;

            // The pane's own root is what it routes against, so the event goes
            // over in the frame the router received it and PaintInput::RouteEvent
            // does the descent and the translation -- one place that knows how to
            // turn a window coordinate into a node's own, not two.
            // Not for a scroll: its x/y are a delta, and writing that in as the
            // routed cursor would move the point the next press routes against.
            if (positional) input->NoteRoutedCursor(ev.x, ev.y);
            input->RouteEvent(ev);
            --left;
        }
    }

    // A pointer without a device, for the page layer and for tests: the same
    // three verbs PaintInput exposes, arriving at the same walk. See
    // PaintInput::ScriptPointer for why a press carries the last position.
    void ScriptPointer(int32_t x, int32_t y)
    {
        m_x = x; m_y = y;
        InputEvent ev{};
        ev.action = INPUT_MOTION;
        ev.x = static_cast<int16_t>(x);
        ev.y = static_cast<int16_t>(y);
        Route(ev);
    }

    /*
 * WHICH BUTTON, because the right one does not mean what the left one means
 * (PaintInput's right-button branch: clear and pan, never draw). Press/Release
 * keep meaning the left button, which is what every existing caller wants and
 * what a bare "press" means in English.
 */
    void ScriptPress()   { ScriptPressButton(PAINT_BUTTON_LEFT); }
    void ScriptRelease() { ScriptReleaseButton(PAINT_BUTTON_LEFT); }

    void ScriptPressButton(uint16_t button)
    {
        InputEvent ev{};
        ev.key    = button;
        ev.action = INPUT_BUTTON_DOWN;
        ev.x = static_cast<int16_t>(m_x);
        ev.y = static_cast<int16_t>(m_y);
        Route(ev);
    }

    void ScriptReleaseButton(uint16_t button)
    {
        InputEvent ev{};
        ev.key    = button;
        ev.action = INPUT_BUTTON_UP;
        ev.x = static_cast<int16_t>(m_x);
        ev.y = static_cast<int16_t>(m_y);
        Route(ev);
    }

    size_t paneCount() const { return m_panes.size(); }

    // The panes in the order events will be offered to them, so a configuration
    // mistake reads as a wrong order rather than as a pane that never responds.
    void Report() const
    {
        ETCS_LOG("PaintRouter", "budget " << m_budget << ", " << m_panes.size() << " pane(s), top first:");
        std::vector<std::pair<int32_t, size_t>> ranked;
        for (size_t i = 0; i < m_panes.size(); ++i)
        {
            ETCS::Held<Drawable_> root = ETCS::resolve_held<Drawable_>("Drawable", m_panes[i].root);
            ranked.emplace_back(root ? root->Order() : 0, i);
        }
        std::stable_sort(ranked.begin(), ranked.end(),
                         [](const std::pair<int32_t, size_t>& a,
                            const std::pair<int32_t, size_t>& b) { return a.first > b.first; });
        for (const auto& r : ranked)
            ETCS_LOG("PaintRouter", "  order=" << r.first
                     << " root RID:" << m_panes[r.second].root
                     << " -> input RID:" << m_panes[r.second].input);
    }

private:
    // `inside` is the last answer to "did this pane contain the pointer", kept so
    // the crossing can be spotted. See Route.
    struct Pane { ETCS::RID root = 0; ETCS::RID input = 0; bool inside = false; };
    std::vector<Pane> m_panes;
    uint32_t m_budget = 1;
    int32_t  m_x = 0;
    int32_t  m_y = 0;
};


// ── work / stream surface ───────────────────────────────────────────────────

DEFINE_WORK_FUNC(PaintTool, SetRadius)
{
    (void)ctx;
    float radius = 0.0f;
    data >> radius;
    self.SetRadius(radius);
}

// SetKind <brush|line|rect|ellipse|fill|smudge|ruler|glyph>
DEFINE_WORK_FUNC(PaintTool, SetKind)
{
    (void)ctx;
    self.SetKind(data.restAsString());
}

// SetText <text...> -- what the glyph tool places. The rest of the line, so a
// caption may contain spaces without the script quoting it.
DEFINE_WORK_FUNC(PaintTool, SetText)
{
    (void)ctx;
    self.SetText(data.restAsString());
}

DEFINE_WORK_FUNC_TYPED(PaintTool, SetTextSize, (uint32_t, px))
{
    (void)ctx;
    self.SetTextSize(px);
}

// 0..255 per channel, alpha included -- see PaintTool::SetTolerance for why a
// zero-tolerance fill leaves a halo on anything antialiased.
DEFINE_WORK_FUNC_TYPED(PaintTool, SetTolerance, (uint32_t, tolerance))
{
    (void)ctx;
    self.SetTolerance(tolerance);
}

DEFINE_WORK_FUNC_TYPED(PaintTool, SetColor, (float, r), (float, g), (float, b), (float, a))
{
    (void)ctx;
    self.SetColor(r, g, b, a);
}

DEFINE_WORK_FUNC_TYPED(PaintTool, BeginStroke, (int32_t, x), (int32_t, y))
{
    (void)ctx;
    self.BeginStroke(x, y);
}

DEFINE_WORK_FUNC_TYPED(PaintTool, MoveStroke, (int32_t, x), (int32_t, y))
{
    (void)ctx;
    self.MoveStroke(x, y);
}

DEFINE_WORK_FUNC(PaintTool, EndStroke)
{
    (void)ctx; (void)data;
    self.EndStroke();
}

DEFINE_WORK_FUNC(PaintTool, CancelStroke)
{
    (void)ctx; (void)data;
    self.CancelStroke();
}

DEFINE_WORK_FUNC(PaintTool, Delete)
{
    (void)ctx; (void)data;
    self.DeleteConcrete();
}

DEFINE_WORK_FUNC_TYPED(PaintLayer, Create, (uint32_t, w), (uint32_t, h))
{
    (void)ctx;
    self.Create(w, h);
}

DEFINE_WORK_FUNC_TYPED(PaintLayer, Clear, (float, r), (float, g), (float, b), (float, a))
{
    (void)ctx;
    self.Clear(r, g, b, a);
}

DEFINE_WORK_FUNC_TYPED(PaintLayer, DrawPixel,
    (int32_t, x), (int32_t, y), (float, r), (float, g), (float, b), (float, a))
{
    (void)ctx;
    self.DrawPixel(x, y, r, g, b, a);
}

DEFINE_WORK_FUNC_TYPED(PaintLayer, DrawLine,
    (int32_t, x0), (int32_t, y0), (int32_t, x1), (int32_t, y1),
    (float, r), (float, g), (float, b), (float, a))
{
    (void)ctx;
    self.DrawLine(x0, y0, x1, y1, r, g, b, a);
}

/*
 * The order key, set from a script or from a layer window's drag. One number and
 * nothing else moves: see PaintLayer's own header note on why a stack is a
 * relation rather than a container.
 */
DEFINE_WORK_FUNC_TYPED(PaintLayer, SetOrder, (int32_t, order))
{
    (void)ctx;
    self.SetOrder(order);
}

// SetName <text...> -- the rest of the line, so a layer may be called "Line art"
// without the script quoting it.
DEFINE_WORK_FUNC(PaintLayer, SetName)
{
    (void)ctx;
    self.SetName(data.restAsString());
}

DEFINE_WORK_FUNC_TYPED(PaintLayer, SetVisible, (int32_t, visible))
{
    (void)ctx;
    self.SetVisible(visible != 0);
}

DEFINE_WORK_FUNC(PaintLayer, ToggleVisible)
{
    (void)ctx; (void)data;
    self.ToggleVisible();
}

DEFINE_WORK_FUNC_TYPED(PaintLayer, SetOpacity, (float, opacity))
{
    (void)ctx;
    self.SetOpacity(opacity);
}

DEFINE_WORK_FUNC(PaintLayer, Report)
{
    (void)ctx; (void)data;
    self.Report();
}

DEFINE_WORK_FUNC(PaintLayer, Delete)
{
    (void)ctx; (void)data;
    self.DeleteConcrete();
}

DEFINE_WORK_FUNC_TYPED(PaintDocument, Create, (uint32_t, w), (uint32_t, h), (std::string, name))
{
    (void)ctx;
    self.Create(w, h, name);
}

DEFINE_WORK_FUNC_TYPED(PaintDocument, SetActiveLayer, (ETCS::RID, layer))
{
    (void)ctx;
    self.SetActiveLayer(layer);
}

DEFINE_WORK_FUNC_TYPED(PaintDocument, ClearLayer,
    (ETCS::RID, layer), (float, r), (float, g), (float, b), (float, a))
{
    (void)ctx;
    self.ClearLayer(layer, r, g, b, a);
}

DEFINE_WORK_FUNC_TYPED(PaintDocument, RenderToSurface,
    (ETCS::RID, target), (int32_t, x), (int32_t, y))
{
    (void)ctx;
    self.RenderToSurface(target, x, y);
}

/*
 * MoveLayerTo <layer_rid> <depth> -- depth counted from the bottom, 0 being
 * first composited. What a dragged row means is a statement about the whole
 * stack, so this renumbers it densely; see PaintDocument::MoveLayerTo.
 */
DEFINE_WORK_FUNC_TYPED(PaintDocument, MoveLayerTo, (ETCS::RID, layer), (int32_t, depth))
{
    (void)ctx;
    self.MoveLayerTo(layer, depth);
}

// RenameLayer <layer_rid> <text...> -- RID first like every verb here that names
// an entity, then the rest of the line as the name.
DEFINE_WORK_FUNC(PaintDocument, RenameLayer)
{
    (void)ctx;
    ETCS::RID layer = 0;
    data >> layer;
    self.RenameLayer(layer, data.restAsString());
}

DEFINE_WORK_FUNC_TYPED(PaintDocument, RemoveLayer, (ETCS::RID, layer))
{
    (void)ctx;
    self.RemoveLayer(layer);
}

/*
 * IsolateLayer <layer_rid> <dim> -- the hover. A RID of 0 is "nobody", which is
 * the same thing ClearIsolate says and the call a row's pointer-leave makes.
 */
DEFINE_WORK_FUNC_TYPED(PaintDocument, IsolateLayer, (ETCS::RID, layer), (float, dim))
{
    (void)ctx;
    self.IsolateLayer(layer, dim);
}

DEFINE_WORK_FUNC(PaintDocument, ClearIsolate)
{
    (void)ctx; (void)data;
    self.ClearIsolate();
}

DEFINE_WORK_FUNC(PaintDocument, Report)
{
    (void)ctx; (void)data;
    self.Report();
}

DEFINE_WORK_FUNC(PaintDocument, Delete)
{
    (void)ctx; (void)data;
    self.DeleteConcrete();
}

DEFINE_WORK_FUNC_TYPED(PaintSurface, Create, (ETCS::RID, target))
{
    (void)ctx;
    self.Create(target);
}

DEFINE_WORK_FUNC_TYPED(PaintSurface, AttachDocument, (ETCS::RID, doc))
{
    (void)ctx;
    self.AttachDocument(doc);
}

DEFINE_WORK_FUNC_TYPED(PaintSurface, SetTarget, (ETCS::RID, target))
{
    (void)ctx;
    self.SetTarget(target);
}

// ── the projection ───────────────────────────────────────────────────────
//
// Pan is where the document's origin sits in view space; zoom is how many view
// pixels one document pixel occupies. Both are verbs because both are driven
// from outside -- a wheel handler in the page, a +/- button in a toolbar, a
// script restoring a saved view.

DEFINE_WORK_FUNC_TYPED(PaintSurface, SetPan, (int32_t, x), (int32_t, y))
{
    (void)ctx;
    self.SetPan(x, y);
}

DEFINE_WORK_FUNC_TYPED(PaintSurface, PanBy, (int32_t, dx), (int32_t, dy))
{
    (void)ctx;
    self.PanBy(dx, dy);
    self.Render();
}

DEFINE_WORK_FUNC_TYPED(PaintSurface, SetZoom, (float, zoom))
{
    (void)ctx;
    self.SetZoom(zoom);
    self.Render();
}

/*
 * ZoomAt <zoom> <vx> <vy> -- hold the view point (vx,vy) fixed.
 *
 * THE VERB A SCROLL WHEEL WANTS, and the reason it is a verb at all: InputEvent
 * carries keys, positions and buttons, and has no scroll axis -- adding one is
 * an ontology change touching every input source. A work function is exported
 * and callable from the page's own wheel handler through etcs_web_call, which
 * is the same seam the toolbar already uses, so the wheel works today and the
 * ontology question stays open on its own merits.
 */
DEFINE_WORK_FUNC_TYPED(PaintSurface, ZoomAt,
    (float, zoom), (int32_t, vx), (int32_t, vy))
{
    (void)ctx;
    self.ZoomAt(zoom, vx, vy);
    self.Render();
    // Logged because the usual caller is the page's wheel handler, and a verb
    // driven from outside the runtime is one you cannot otherwise watch.
    ETCS_LOG("PaintSurface", "zoom " << self.zoomPercent() << "%  pan "
             << self.panX() << "," << self.panY() << "  (about " << vx << "," << vy << ")");
}

// ZoomBy <factor> <vx> <vy> -- multiplicative, because zoom is perceived that
// way: 1.25 is one notch in at every magnification.
DEFINE_WORK_FUNC_TYPED(PaintSurface, ZoomBy,
    (float, factor), (int32_t, vx), (int32_t, vy))
{
    (void)ctx;
    self.ZoomBy(factor, vx, vy);
    self.Render();
    ETCS_LOG("PaintSurface", "zoom " << self.zoomPercent() << "%  pan "
             << self.panX() << "," << self.panY() << "  (x" << factor
             << " about " << vx << "," << vy << ")");
}

// The TextLabel (or any Drawable2D exporting SetText) that shows the zoom. See
// PaintSurface::BindZoomLabel on why the surface pushes it.
DEFINE_WORK_FUNC(PaintSurface, BindZoomLabel)
{
    (void)ctx;
    ETCS::RID label = 0;
    data >> label;
    self.BindZoomLabel(label);
}

DEFINE_WORK_FUNC_TYPED(PaintSurface, SetBackground,
    (float, r), (float, g), (float, b), (float, a))
{
    (void)ctx;
    self.SetBackground(r, g, b, a);
}

// The readout a zoom widget shows, written back into the caller's buffer so a
// script or the page can display it without a second call.
DEFINE_WORK_FUNC(PaintSurface, ZoomPercent)
{
    (void)ctx; (void)data;
    ETCS_LOG("PaintSurface", "zoom " << self.zoomPercent() << "%  pan "
             << self.panX() << "," << self.panY());
    data.writeString(std::to_string(self.zoomPercent()).c_str());
}

DEFINE_WORK_FUNC(PaintSurface, Render)
{
    (void)ctx; (void)data;
    self.Render();
}

DEFINE_WORK_FUNC(PaintSurface, Delete)
{
    (void)ctx; (void)data;
    self.DeleteConcrete();
}

DEFINE_WORK_FUNC_TYPED(PaintInput, Create,
    (ETCS::RID, document), (ETCS::RID, tool), (ETCS::RID, surface))
{
    (void)ctx;
    self.BindDocument(document);
    self.BindTool(tool);
    self.BindSurface(surface);
}

DEFINE_WORK_FUNC_TYPED(PaintInput, BindDocument, (ETCS::RID, document))
{
    (void)ctx;
    self.BindDocument(document);
}

DEFINE_WORK_FUNC_TYPED(PaintInput, BindTool, (ETCS::RID, tool))
{
    (void)ctx;
    self.BindTool(tool);
}

DEFINE_WORK_FUNC_TYPED(PaintInput, BindSurface, (ETCS::RID, surface))
{
    (void)ctx;
    self.BindSurface(surface);
}

DEFINE_WORK_FUNC_TYPED(PaintInput, SetBrush,
    (float, radius), (float, r), (float, g), (float, b), (float, a))
{
    (void)ctx;
    self.SetBrush(radius, r, g, b, a);
}

/*
 * The keyboard edge: presses begin and end strokes.
 *
 * `stream`, not `data` -- `data` is the config buffer delivered once when the
 * edge opens, and the events arrive on the stream. Blocking readRaw, because a
 * cross-tag pair is a pipe with a blocking consumer fd.
 */
/*
 * ── THE ROUTED EDGES ─────────────────────────────────────────────────────
 *
 * The same two channels, with the frame translation in front of them. Separate
 * verbs rather than a flag on the old ones because the two are genuinely
 * different contracts: ConsumeInput promises "these coordinates are the
 * canvas's", and this one promises "these coordinates are the window's and I
 * will find out whose they should be". A script binding a root and then reading
 * ConsumeInput in the ledger would be reading a lie.
 *
 * Structurally identical otherwise -- same drain, same break conditions, one
 * call different -- so nothing about the edge's behaviour has to be learned
 * twice.
 */
// ── PaintPalette ─────────────────────────────────────────────────────────

DEFINE_WORK_FUNC(PaintPalette, BindTool)
{
    (void)ctx;
    ETCS::RID tool = 0;
    data >> tool;
    self.BindTool(tool);
}

// AddColor <node_rid> <r> <g> <b> <a> -- the RID first, matching every other
// verb here that names an entity, and matching what a pick hands back.
DEFINE_WORK_FUNC_TYPED(PaintPalette, AddColor,
    (ETCS::RID, node), (float, r), (float, g), (float, b), (float, a))
{
    (void)ctx;
    self.AddColor(node, r, g, b, a);
}

DEFINE_WORK_FUNC_TYPED(PaintPalette, AddSize, (ETCS::RID, node), (float, radius))
{
    (void)ctx;
    self.AddSize(node, radius);
}

// AddTool <node_rid> <kind> -- a third thing a toolbar node can mean.
// AddZoom <node_rid> <factor> -- a node that steps the zoom. See
// PaintPalette::AddZoom on why a view setting is mapped by the same type that
// maps tool settings.
DEFINE_WORK_FUNC_TYPED(PaintPalette, AddZoom, (ETCS::RID, node), (float, factor))
{
    (void)ctx;
    self.AddZoom(node, factor);
}

DEFINE_WORK_FUNC(PaintPalette, BindSurface)
{
    (void)ctx;
    ETCS::RID surface = 0;
    data >> surface;
    self.BindSurface(surface);
}

DEFINE_WORK_FUNC(PaintPalette, AddTool)
{
    (void)ctx;
    ETCS::RID node = 0;
    data >> node;
    self.AddTool(node, data.restAsString());
}

/*
 * SetColorOf <node_rid> <r> <g> <b> <a> -- replace what a swatch MEANS.
 *
 * The swatch's own fill is the caller's to change (SetFill on the drawable),
 * because a palette holds no drawables. Two calls for one intent, and that is
 * the honest split: this module owns the mapping, the tree owns the appearance.
 */
DEFINE_WORK_FUNC_TYPED(PaintPalette, SetColorOf,
    (ETCS::RID, node), (float, r), (float, g), (float, b), (float, a))
{
    (void)ctx;
    if (!self.SetColorOf(node, r, g, b, a))
        ETCS_LOG("PaintPalette", "SetColorOf RID:" << node
                 << " -- not a colour swatch of this palette.");
}

DEFINE_WORK_FUNC(PaintPalette, Report)
{
    (void)ctx; (void)data;
    self.Report();
}

DEFINE_WORK_FUNC(PaintPalette, Delete)
{
    (void)ctx; (void)data;
    self.DeleteConcrete();
}

// ── PaintLayerPanel ──────────────────────────────────────────────────────
//
// Rows are declared by the script that draws them; everything else here is a
// look the script states or an interaction the input edge routes in.

DEFINE_WORK_FUNC(PaintLayerPanel, Create)
{
    (void)ctx; (void)data;
    self.Create();
}

DEFINE_WORK_FUNC_TYPED(PaintLayerPanel, BindDocument, (ETCS::RID, document))
{
    (void)ctx;
    self.BindDocument(document);
}

// AddRow <bg> <eye> <label> <delete> -- top of the window first, matching the
// order a script lays them out in. Any of the four may be 0.
DEFINE_WORK_FUNC_TYPED(PaintLayerPanel, AddRow,
    (ETCS::RID, bg), (ETCS::RID, eye), (ETCS::RID, label), (ETCS::RID, del))
{
    (void)ctx;
    self.AddRow(bg, eye, label, del);
}

DEFINE_WORK_FUNC_TYPED(PaintLayerPanel, SetHoverDim, (float, dim))
{
    (void)ctx;
    self.SetHoverDim(dim);
}

DEFINE_WORK_FUNC_TYPED(PaintLayerPanel, SetEyeColors,
    (float, vr), (float, vg), (float, vb), (float, hr), (float, hg), (float, hb))
{
    (void)ctx;
    self.SetEyeColors(vr, vg, vb, hr, hg, hb);
}

DEFINE_WORK_FUNC_TYPED(PaintLayerPanel, SetRowColors,
    (float, sr), (float, sg), (float, sb), (float, ur), (float, ug), (float, ub))
{
    (void)ctx;
    self.SetRowColors(sr, sg, sb, ur, ug, ub);
}

// In ROWS, so a wheel notch is +/-1 and nothing outside has to know the row
// height. Negative scrolls toward the top of the stack.
DEFINE_WORK_FUNC_TYPED(PaintLayerPanel, Scroll, (int32_t, delta))
{
    (void)ctx;
    self.Scroll(delta);
}

DEFINE_WORK_FUNC(PaintLayerPanel, Refresh)
{
    (void)ctx; (void)data;
    self.Refresh();
}

// CommitRename <text...> -- the rest of the line, applied to whichever layer a
// second press on a label armed. Says so when nothing is armed rather than
// renaming something arbitrary.
DEFINE_WORK_FUNC(PaintLayerPanel, CommitRename)
{
    (void)ctx;
    if (!self.CommitRename(data.restAsString()))
        ETCS_LOG("PaintLayerPanel", "CommitRename with no rename armed -- press a "
                 "layer's name twice first.");
}

// The four row actions, addressed by index rather than by a synthesised pick --
// see PaintLayerPanel::SelectRow for why a caller should not have to pretend to
// be a mouse. All of them are exported, so the page's JS reaches them through
// etcs_web_call exactly as a script does.
DEFINE_WORK_FUNC_TYPED(PaintLayerPanel, SelectRow, (uint32_t, row))
{
    (void)ctx;
    self.SelectRow(row);
}

DEFINE_WORK_FUNC_TYPED(PaintLayerPanel, ToggleRow, (uint32_t, row))
{
    (void)ctx;
    self.ToggleRow(row);
}

DEFINE_WORK_FUNC_TYPED(PaintLayerPanel, RemoveRow, (uint32_t, row))
{
    (void)ctx;
    self.RemoveRow(row);
}

// MoveRow <from> <to> -- the layer at `from` takes the depth of the row at
// `to`, which is the drag a pointer would have performed.
DEFINE_WORK_FUNC_TYPED(PaintLayerPanel, MoveRow, (uint32_t, from), (uint32_t, to))
{
    (void)ctx;
    self.MoveRow(from, to);
}

DEFINE_WORK_FUNC_TYPED(PaintLayerPanel, ArmRename, (uint32_t, row))
{
    (void)ctx;
    self.ArmRename(row);
}

// HoverRow <row> -- any out-of-range index, -1 by convention, means "the
// pointer is nowhere near this window" and clears the isolation.
DEFINE_WORK_FUNC_TYPED(PaintLayerPanel, HoverRow, (int32_t, row))
{
    (void)ctx;
    self.HoverRow(row);
}

DEFINE_WORK_FUNC(PaintLayerPanel, Report)
{
    (void)ctx; (void)data;
    self.Report();
}

DEFINE_WORK_FUNC(PaintLayerPanel, Delete)
{
    (void)ctx; (void)data;
    self.DeleteConcrete();
}

// ── PaintColorWheel ──────────────────────────────────────────────────────
//
// Geometry and bindings are the script's; the pick and the replacement are this
// type's. Open/Close are verbs so the page or a toolbar button can raise it,
// which is the usual way it gets opened.

DEFINE_WORK_FUNC_TYPED(PaintColorWheel, Create,
    (int32_t, cx), (int32_t, cy), (uint32_t, radius))
{
    (void)ctx;
    self.Create(cx, cy, radius);
}

DEFINE_WORK_FUNC(PaintColorWheel, BindTool)
{
    (void)ctx;
    ETCS::RID tool = 0;
    data >> tool;
    self.BindTool(tool);
}

DEFINE_WORK_FUNC(PaintColorWheel, BindPalette)
{
    (void)ctx;
    ETCS::RID palette = 0;
    data >> palette;
    self.BindPalette(palette);
}

DEFINE_WORK_FUNC(PaintColorWheel, BindRouter)
{
    (void)ctx;
    ETCS::RID router = 0;
    data >> router;
    self.BindRouter(router);
}

// BindPane <root_rid> <input_rid> -- what gets added to the router when this
// opens. Open means "in the routing set"; see the class note.
DEFINE_WORK_FUNC_TYPED(PaintColorWheel, BindPane, (ETCS::RID, root), (ETCS::RID, input))
{
    (void)ctx;
    self.BindPane(root, input);
}

// The third dimension a disc cannot carry. Without it the picker cannot reach
// anything dark.
DEFINE_WORK_FUNC_TYPED(PaintColorWheel, SetValue, (float, v))
{
    (void)ctx;
    self.SetValue(v);
}

DEFINE_WORK_FUNC(PaintColorWheel, Open)
{
    (void)ctx; (void)data;
    self.Open();
}

DEFINE_WORK_FUNC(PaintColorWheel, Close)
{
    (void)ctx; (void)data;
    self.Close();
}

// Pick <x> <y> in the wheel's own space -- the pointer path calls this itself,
// so this verb is for a page or a test that has a coordinate and no pointer.
DEFINE_WORK_FUNC_TYPED(PaintColorWheel, Pick, (int32_t, x), (int32_t, y))
{
    (void)ctx;
    self.Pick(x, y);
}

DEFINE_WORK_FUNC(PaintColorWheel, Report)
{
    (void)ctx; (void)data;
    self.Report();
}

DEFINE_WORK_FUNC(PaintColorWheel, Delete)
{
    (void)ctx; (void)data;
    self.DeleteConcrete();
}

// ── PaintRouter ──────────────────────────────────────────────────────────
//
// The arbiter's script surface. Panes are added by RID pairs and the priority is
// never stated here -- it is the roots' own SetOrder, read per event. See the
// class for why that is one relation rather than two.

DEFINE_WORK_FUNC_TYPED(PaintRouter, Create, (uint32_t, passes))
{
    (void)ctx;
    self.Create(passes);
}

// AddPane <root_rid> <input_rid> -- what to hit-test, and who gets the hit.
DEFINE_WORK_FUNC_TYPED(PaintRouter, AddPane, (ETCS::RID, root), (ETCS::RID, input))
{
    (void)ctx;
    self.AddPane(root, input);
}

DEFINE_WORK_FUNC_TYPED(PaintRouter, RemovePane, (ETCS::RID, root))
{
    (void)ctx;
    self.RemovePane(root);
}

// How many panes one event may be consumed by. 1 is the topmost pane alone; 2
// lets the pane underneath see it too.
DEFINE_WORK_FUNC_TYPED(PaintRouter, SetPassBudget, (uint32_t, passes))
{
    (void)ctx;
    self.SetPassBudget(passes);
}

DEFINE_WORK_FUNC_TYPED(PaintRouter, Pointer, (int32_t, x), (int32_t, y))
{
    (void)ctx;
    self.ScriptPointer(x, y);
}

DEFINE_WORK_FUNC(PaintRouter, Press)
{
    (void)ctx; (void)data;
    self.ScriptPress();
}

DEFINE_WORK_FUNC(PaintRouter, Release)
{
    (void)ctx; (void)data;
    self.ScriptRelease();
}

// PressButton <n> / ReleaseButton <n> -- 0 left, 1 right (PAINT_BUTTON_*).
// Press/Release above are the left-button shorthands, which is what a bare
// "press" means everywhere else.
DEFINE_WORK_FUNC_TYPED(PaintRouter, PressButton, (uint32_t, button))
{
    (void)ctx;
    self.ScriptPressButton(static_cast<uint16_t>(button));
}

DEFINE_WORK_FUNC_TYPED(PaintRouter, ReleaseButton, (uint32_t, button))
{
    (void)ctx;
    self.ScriptReleaseButton(static_cast<uint16_t>(button));
}

DEFINE_WORK_FUNC(PaintRouter, Report)
{
    (void)ctx; (void)data;
    self.Report();
}

DEFINE_WORK_FUNC(PaintRouter, Delete)
{
    (void)ctx; (void)data;
    self.DeleteConcrete();
}

/*
 * THE EDGE A MULTI-PANE SESSION BINDS INSTEAD OF PaintInput's.
 *
 * Shaped exactly like PaintInput::ConsumePointer -- same channel, same filter --
 * because from the window's side nothing has changed: it is still one producer
 * writing positions and presses. What changed is that the far end arbitrates
 * before it interprets, so a session grows a second pane by adding a pane here
 * rather than by binding a second consumer to the same producer and hoping.
 */
DEFINE_STREAM_FUNC_CONSUME(PaintRouter, ConsumePointer)
{
    (void)data;

    ETCS_LOG("PaintRouter::ConsumePointer", "routed pointer edge open on RID:" << self.getRID()
             << ", " << self.paneCount() << " pane(s), budget " << self.passBudget());

    while (stream.isOpen())
    {
        if (ctx.isInterrupted() || ctx.isTerminated()) break;

        ETCS::Buffer slot;
        if (!stream.readRaw(slot)) break;

        InputEvent ev{};
        slot.readRaw(&ev, sizeof(InputEvent));
        if (ev.action != INPUT_MOTION && ev.action != INPUT_SCROLL
            && ev.action != INPUT_BUTTON_DOWN && ev.action != INPUT_BUTTON_UP) continue;
        self.Route(ev);
    }

    ETCS_LOG("PaintRouter::ConsumePointer", "routed pointer edge closed.");
}

/*
 * The key channel, arbitrated the same way. A press arrives here with no position
 * of its own (ontology/InputSource.h), so the walk uses the last routed position
 * -- the same rule PaintInput::ScriptPress follows, for the same reason.
 */
DEFINE_STREAM_FUNC_CONSUME(PaintRouter, ConsumeInput)
{
    (void)data;

    ETCS_LOG("PaintRouter::ConsumeInput", "routed key edge open on RID:" << self.getRID());

    while (stream.isOpen())
    {
        if (ctx.isInterrupted() || ctx.isTerminated()) break;

        ETCS::Buffer slot;
        if (!stream.readRaw(slot)) break;

        InputEvent ev{};
        slot.readRaw(&ev, sizeof(InputEvent));
        if (ev.action == INPUT_DOWN)      self.ScriptPress();
        else if (ev.action == INPUT_UP)   self.ScriptRelease();
    }

    ETCS_LOG("PaintRouter::ConsumeInput", "routed key edge closed.");
}

// ── PaintInput routing ───────────────────────────────────────────────────

DEFINE_WORK_FUNC(PaintInput, BindRoot)
{
    (void)ctx;
    ETCS::RID root = 0;
    data >> root;
    self.BindRoot(root);
}

DEFINE_WORK_FUNC(PaintInput, BindCanvas)
{
    (void)ctx;
    ETCS::RID canvas = 0;
    data >> canvas;
    self.BindCanvas(canvas);
}

// Any leaf claiming Glyphs -- RenderProvider::TextLabel today. Needed only by
// the glyph tool; every other kind ignores it.
DEFINE_WORK_FUNC(PaintInput, BindGlyphs)
{
    (void)ctx;
    ETCS::RID glyphs = 0;
    data >> glyphs;
    self.BindGlyphs(glyphs);
}

DEFINE_WORK_FUNC(PaintInput, BindWheel)
{
    (void)ctx;
    ETCS::RID wheel = 0;
    data >> wheel;
    self.BindWheel(wheel);
}

// 1 on the wheel's own pane (a press picks), 0 on the canvas (a press
// dismisses). Two inputs bind the same wheel and mean different things by it --
// see PaintInput::BindWheel.
DEFINE_WORK_FUNC_TYPED(PaintInput, SetWheelPane, (int32_t, is_wheel))
{
    (void)ctx;
    self.SetWheelPane(is_wheel != 0);
}

DEFINE_WORK_FUNC(PaintInput, BindPanel)
{
    (void)ctx;
    ETCS::RID panel = 0;
    data >> panel;
    self.BindPanel(panel);
}

DEFINE_WORK_FUNC(PaintInput, BindPalette)
{
    (void)ctx;
    ETCS::RID palette = 0;
    data >> palette;
    self.BindPalette(palette);
}

DEFINE_STREAM_FUNC_CONSUME(PaintInput, ConsumeRouted)
{
    (void)data;

    ETCS_LOG("PaintInput::ConsumeRouted", "press edge open on RID:" << self.getRID()
             << " -- events are routed through the 2D tree before they mean anything.");

    while (stream.isOpen())
    {
        if (ctx.isInterrupted() || ctx.isTerminated()) break;

        ETCS::Buffer slot;
        if (!stream.readRaw(slot)) break;

        InputEvent ev{};
        slot.readRaw(&ev, sizeof(InputEvent));
        if (ev.action == INPUT_MOTION) continue;
        /*
     * A KEY EVENT CARRIES NO POSITION (ontology/InputSource.h: x/y are
     * meaningful for INPUT_MOTION only), so routing one on its own
     * coordinates would pick whatever sits at the origin. The last position
     * the pointer channel delivered is where the press happened, which is
     * the same assumption the unrouted path already makes and the same one
     * absolute positions make safe.
     */
        ev.x = static_cast<int16_t>(self.RoutedCursorX());
        ev.y = static_cast<int16_t>(self.RoutedCursorY());
        self.RouteEvent(ev);
    }

    ETCS_LOG("PaintInput::ConsumeRouted", "press edge closed.");
}

DEFINE_STREAM_FUNC_CONSUME(PaintInput, ConsumeRoutedPointer)
{
    (void)data;

    ETCS_LOG("PaintInput::ConsumeRoutedPointer", "pointer edge open on RID:"
             << self.getRID() << " -- window frame in, node frame out.");

    while (stream.isOpen())
    {
        if (ctx.isInterrupted() || ctx.isTerminated()) break;

        ETCS::Buffer slot;
        if (!stream.readRaw(slot)) break;

        InputEvent ev{};
        slot.readRaw(&ev, sizeof(InputEvent));
        // Buttons ride this ring too, and carry their own position -- so this
        // edge is where a click becomes a stroke, not just where the brush
        // follows the cursor.
        if (ev.action != INPUT_MOTION
            && ev.action != INPUT_BUTTON_DOWN && ev.action != INPUT_BUTTON_UP) continue;
        self.NoteRoutedCursor(ev.x, ev.y);
        self.RouteEvent(ev);
    }

    ETCS_LOG("PaintInput::ConsumeRoutedPointer", "pointer edge closed.");
}

DEFINE_STREAM_FUNC_CONSUME(PaintInput, ConsumeInput)
{
    (void)data;

    ETCS_LOG("PaintInput::ConsumeInput", "key edge open on RID:" << self.getRID());

    while (stream.isOpen())
    {
        if (ctx.isInterrupted() || ctx.isTerminated()) break;

        ETCS::Buffer slot;
        if (!stream.readRaw(slot)) break;

        InputEvent ev{};
        slot.readRaw(&ev, sizeof(InputEvent));
        if (ev.action == INPUT_MOTION) continue;
        self.HandleEvent(ev);
    }

    ETCS_LOG("PaintInput::ConsumeInput", "key edge closed.");
}

/*
 * The pointer edge: positions place the brush.
 *
 * TWO EDGES, and the correlation cost is nil because the position is ABSOLUTE.
 * A press means "begin a stroke where the pointer is", and the pointer's
 * position is already known from the last sample -- it does not have to arrive
 * in the same stream, or in any particular order relative to the press. The
 * worst a split costs is that a press lands on a position one sample old, which
 * at pointer rates is invisible; what it buys is that a keystroke never queues
 * behind a burst of motion.
 */
DEFINE_STREAM_FUNC_CONSUME(PaintInput, ConsumePointer)
{
    (void)data;

    ETCS_LOG("PaintInput::ConsumePointer", "pointer edge open on RID:" << self.getRID());

    while (stream.isOpen())
    {
        if (ctx.isInterrupted() || ctx.isTerminated()) break;

        ETCS::Buffer slot;
        if (!stream.readRaw(slot)) break;

        InputEvent ev{};
        slot.readRaw(&ev, sizeof(InputEvent));
        // Buttons share this channel and carry their own position, so an
        // unrouted session gets click-and-drag for free.
        if (ev.action != INPUT_MOTION
            && ev.action != INPUT_BUTTON_DOWN && ev.action != INPUT_BUTTON_UP) continue;
        self.HandleEvent(ev);
    }

    ETCS_LOG("PaintInput::ConsumePointer", "pointer edge closed.");
}

// Pointer <x> <y> / Press / Release -- a stroke without a device. See
// PaintInput::ScriptPointer.
DEFINE_WORK_FUNC_TYPED(PaintInput, Pointer, (int32_t, x), (int32_t, y))
{
    (void)ctx;
    self.ScriptPointer(x, y);
}

DEFINE_WORK_FUNC(PaintInput, Press)
{
    (void)ctx; (void)data;
    self.ScriptPress();
}

DEFINE_WORK_FUNC(PaintInput, Release)
{
    (void)ctx; (void)data;
    self.ScriptRelease();
}

// Report the machine's state, so a caller can tell a refused stroke from a
// completed one -- Press before the pointer has ever been seen is refused on
// purpose (HandleEvent), and silently.
DEFINE_WORK_FUNC(PaintInput, Report)
{
    (void)data; (void)ctx;
    ETCS_LOG("PaintInput::Report", "cursor (" << self.cursorX() << ", " << self.cursorY()
             << ")  stroke " << (self.StrokeActive() ? "ACTIVE" : "idle"));
}

DEFINE_WORK_FUNC(PaintInput, Delete)
{
    (void)ctx; (void)data;
    self.DeleteConcrete();
}

#endif // PAINTPROVIDER_H__
