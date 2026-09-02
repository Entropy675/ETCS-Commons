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
        else if (ev.action == INPUT_DOWN)
        {
            // A stroke cannot begin before the pointer has been seen once: the
            // brush would land at the origin rather than under the cursor.
            if (m_tool && m_cursor_seen)
            {
                m_tool->BeginStroke(m_cursor_x, m_cursor_y);
                m_last_x = m_cursor_x;
                m_last_y = m_cursor_y;
                apply_sample(m_cursor_x, m_cursor_y);
            }
        }
        else if (ev.action == INPUT_UP)
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
 * The standing input edge. Blocking, one event at a time, the same shape
 * Scene3D::ConsumeInput uses -- and it has to read `stream`, not `data`.
 *
 * `data` in a stream function is the CONFIG buffer: whatever the script wrote
 * after the verb, delivered once when the edge opens. The events arrive on
 * `stream`. Reading data here parses the (empty) configuration as though it
 * were an input record and then never looks again, so the edge sits open
 * having consumed nothing -- which reads as "input does not work" rather than
 * as a mistake in the wiring.
 *
 * hasData() is deliberately not used: a cross-tag pair resolves to
 * StrategyPipe, whose consumer fd is blocking, so a drain loop built on it
 * spins here and stalls on a same-module pair. Blocking readRaw is what every
 * consumer in this codebase does.
 */
DEFINE_STREAM_FUNC_CONSUME(PaintInput, ConsumeInput)
{
    (void)data;

    ETCS_LOG("PaintInput::ConsumeInput", "paint edge open on RID:" << self.getRID()
             << " -- pointer position places the brush, any key or button strokes.");

    while (stream.isOpen())
    {
        if (ctx.isInterrupted() || ctx.isTerminated()) break;

        ETCS::Buffer slot;
        if (!stream.readRaw(slot)) break;

        InputEvent ev{};
        slot.readRaw(&ev, sizeof(InputEvent));
        self.HandleEvent(ev);
    }

    ETCS_LOG("PaintInput::ConsumeInput", "paint edge closed.");
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
