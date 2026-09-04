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
    etcs_mark_pixel_path(static_cast<ETCS::Entity*>(held.get()));
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

class PaintTool : public DeletableBase<PaintTool>
{
public:
    WIRE_TYPE_IDENTITY(PaintTool);

    PaintTool() = default;
    bool DeleteConcrete() override { return true; }

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

private:
    PaintBrushState m_brush;
    bool m_active = false;
    std::vector<PaintStrokePoint> m_points;
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
        m_entries[node] = Entry{ Kind::Color, { r, g, b, a }, 0.0f };
    }

    void AddSize(ETCS::RID node, float radius)
    {
        if (node == 0 || radius <= 0.0f) return;
        m_entries[node] = Entry{ Kind::Size, {}, radius };
    }

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
            ETCS_LOG("PaintPalette", "colour -> " << e.rgba[0] << ", " << e.rgba[1]
                     << ", " << e.rgba[2] << ", " << e.rgba[3]);
        }
        else
        {
            m_tool->SetRadius(e.radius);
            ETCS_LOG("PaintPalette", "radius -> " << e.radius);
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
    enum class Kind : uint8_t { Color, Size };
    struct Entry { Kind kind; float rgba[4]; float radius; };

    std::unordered_map<ETCS::RID, Entry> m_entries;
    PaintTool* m_tool = nullptr;
};

class PaintLayer : public DeletableBase<PaintLayer>
{
public:
    WIRE_TYPE_IDENTITY(PaintLayer);

    PaintLayer() = default;
    bool DeleteConcrete() override { return true; }

    bool Create(uint32_t w, uint32_t h)
    {
        if (w == 0 || h == 0) return false;
        m_width = w;
        m_height = h;
        m_visible = true;
        m_opacity = 1.0f;
        m_pixels.assign(static_cast<size_t>(w) * h * 4, 0);
        return true;
    }

    void SetVisible(bool visible) { m_visible = visible; }

    void SetOpacity(float opacity)
    {
        m_opacity = std::clamp(opacity, 0.0f, 1.0f);
    }

    void Clear(float r, float g, float b, float a)
    {
        const uint8_t R = static_cast<uint8_t>(std::clamp(r, 0.0f, 1.0f) * 255.0f);
        const uint8_t G = static_cast<uint8_t>(std::clamp(g, 0.0f, 1.0f) * 255.0f);
        const uint8_t B = static_cast<uint8_t>(std::clamp(b, 0.0f, 1.0f) * 255.0f);
        const uint8_t A = static_cast<uint8_t>(std::clamp(a, 0.0f, 1.0f) * 255.0f);
        for (size_t i = 0; i + 3 < m_pixels.size(); i += 4)
        {
            m_pixels[i + 0] = R;
            m_pixels[i + 1] = G;
            m_pixels[i + 2] = B;
            m_pixels[i + 3] = A;
        }
    }

    void DrawPixel(int32_t x, int32_t y, float r, float g, float b, float a)
    {
        if (x < 0 || y < 0 ||
            static_cast<uint32_t>(x) >= m_width ||
            static_cast<uint32_t>(y) >= m_height)
            return;

        size_t i = (static_cast<size_t>(y) * m_width + static_cast<size_t>(x)) * 4;
        m_pixels[i + 0] = static_cast<uint8_t>(std::clamp(r, 0.0f, 1.0f) * 255.0f);
        m_pixels[i + 1] = static_cast<uint8_t>(std::clamp(g, 0.0f, 1.0f) * 255.0f);
        m_pixels[i + 2] = static_cast<uint8_t>(std::clamp(b, 0.0f, 1.0f) * 255.0f);
        m_pixels[i + 3] = static_cast<uint8_t>(std::clamp(a, 0.0f, 1.0f) * 255.0f);
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

    // Smoke-path composite: push a coarse preview of non-transparent pixels as
    // DrawRect stamps. Full pixel upload belongs on an ImageSurface path once
    // Pinta-shaped layers own a real Pixels_ buffer end-to-end.
    void BlitTo(ETCS::RID target, int32_t ox, int32_t oy, uint32_t w, uint32_t h,
                float opacity)
    {
        Surface_* surface = ETCS::resolve_in_family<Surface_>("Surface", target);
        if (!surface || !m_visible) return;

        const float alpha = std::clamp(opacity, 0.0f, 1.0f) * m_opacity;
        if (alpha <= 0.0f) return;

        const uint32_t draw_w = (w == 0) ? m_width  : std::min(w, m_width);
        const uint32_t draw_h = (h == 0) ? m_height : std::min(h, m_height);

        // Step subsample for the smoke path so a full-layer blit does not
        // issue width*height DrawRect calls.
        constexpr uint32_t step = 4;
        for (uint32_t y = 0; y < draw_h; y += step)
        {
            for (uint32_t x = 0; x < draw_w; x += step)
            {
                size_t i = (static_cast<size_t>(y) * m_width + x) * 4;
                if (i + 3 >= m_pixels.size()) continue;
                if (m_pixels[i + 3] == 0) continue;
                const float pr = m_pixels[i + 0] / 255.0f;
                const float pg = m_pixels[i + 1] / 255.0f;
                const float pb = m_pixels[i + 2] / 255.0f;
                const float pa = (m_pixels[i + 3] / 255.0f) * alpha;
                surface->DrawRect(static_cast<int32_t>(ox + x),
                                  static_cast<int32_t>(oy + y),
                                  step, step, pr, pg, pb, pa);
            }
        }
    }

    uint32_t width() const { return m_width; }
    uint32_t height() const { return m_height; }
    bool visible() const { return m_visible; }
    float opacity() const { return m_opacity; }
    const std::vector<uint8_t>& pixels() const { return m_pixels; }

private:
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    float m_opacity = 1.0f;
    bool m_visible = true;
    std::vector<uint8_t> m_pixels;
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
        m_layers.clear();
        m_active_layer = nullptr;
        return true;
    }

    void SetName(const std::string& name) { m_name = name; }

    void AddLayer(ETCS::RID layer_rid)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintLayer", layer_rid);
        if (!raw) return;
        auto* layer = static_cast<PaintLayer*>(raw->getTrueType());
        if (layer) m_layers.push_back(layer);
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

    void RenderToSurface(ETCS::RID target, int32_t x, int32_t y)
    {
        Surface_* surface = ETCS::resolve_in_family<Surface_>("Surface", target);
        if (!surface) return;

        for (auto* layer : m_layers)
        {
            if (!layer || !layer->visible()) continue;
            layer->BlitTo(target, x, y, 0, 0, layer->opacity());
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
    const std::vector<PaintLayer*>& layers() const { return m_layers; }
    PaintLayer* activeLayer() const { return m_active_layer; }

private:
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    std::string m_name = "Untitled";
    std::vector<PaintLayer*> m_layers;
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

    void Render()
    {
        if (!m_document || m_target == 0) return;
        m_document->RenderToSurface(m_target, 0, 0);
    }

    // Live stroke stamp onto the bound view surface (not a full layer composite).
    void StampBrush(int32_t x, int32_t y, const PaintBrushState& brush)
    {
        if (m_target == 0) return;
        paint_stamp_surface(m_target, x, y, brush);
    }

    PaintDocument* document() const { return m_document; }
    ETCS::RID target() const { return m_target; }

private:
    PaintDocument* m_document = nullptr;
    ETCS::RID m_target = 0;
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

    void RouteEvent(const InputEvent& ev)
    {
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
    void HandleEvent(const InputEvent& ev)
    {
        if (ev.action == INPUT_MOTION)
        {
            // TAKEN, NOT ACCUMULATED. The event carries where the pointer is,
            // in content-area pixels -- the same space the canvas is in -- so
            // there is nothing to integrate and nothing to drift.
            m_cursor_x = ev.x;
            m_cursor_y = ev.y;
            m_cursor_seen = true;

            if (m_tool && m_tool->active())
            {
                m_tool->MoveStroke(m_cursor_x, m_cursor_y);
                // INTERPOLATED, because the pointer is sampled once per poll
                // pass and a hand moves further than one pixel in that time.
                // Stamping only where samples land gives a dotted line at any
                // speed above a crawl; joining consecutive samples is what
                // makes a stroke a stroke.
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
                m_cursor_x = ev.x;
                m_cursor_y = ev.y;
                m_cursor_seen = true;
            }
            if (m_tool && m_cursor_seen)
            {
                m_tool->BeginStroke(m_cursor_x, m_cursor_y);
                m_last_x = m_cursor_x;
                m_last_y = m_cursor_y;
                apply_sample(m_cursor_x, m_cursor_y);
            }
        }
        else if (ev.action == INPUT_UP || ev.action == INPUT_BUTTON_UP)
        {
            if (m_tool) m_tool->EndStroke();
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
 */
    void ScriptPointer(int32_t x, int32_t y)
    {
        InputEvent ev{};
        ev.action = INPUT_MOTION;
        ev.x = static_cast<int16_t>(x);
        ev.y = static_cast<int16_t>(y);
        HandleEvent(ev);
    }

    void ScriptPress()
    {
        InputEvent ev{};
        ev.action = INPUT_DOWN;
        HandleEvent(ev);
    }

    void ScriptRelease()
    {
        InputEvent ev{};
        ev.action = INPUT_UP;
        HandleEvent(ev);
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
};

// ── work / stream surface ───────────────────────────────────────────────────

DEFINE_WORK_FUNC(PaintTool, SetRadius)
{
    (void)ctx;
    float radius = 0.0f;
    data >> radius;
    self.SetRadius(radius);
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

DEFINE_WORK_FUNC_TYPED(PaintDocument, AddLayer, (ETCS::RID, layer))
{
    (void)ctx;
    self.AddLayer(layer);
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
