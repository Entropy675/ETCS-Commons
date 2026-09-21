#ifndef PAINTPROVIDER_H__
#define PAINTPROVIDER_H__

#define ETCS_DLL_EXPORTS
#include "../../core_defs.h"
#include "../../ontology.h"
#include "Contract_PaintProvider.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <thread>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(__EMSCRIPTEN__)
// For MAIN_THREAD_EM_ASM: the canvas menu's save/load reach the page's own
// file controls by dispatching a DOM event (PaintCanvasMenu::page_event).
#include <emscripten.h>
#include <emscripten/em_asm.h>
extern "C" void etcs_web_shell_write(const char* text);
extern "C" int  etcs_web_shell_try_pop_line(char* out, int cap);
#endif

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

// A float channel as a byte, rounded -- identical to Pixels_::toByte, which is
// not reachable from here (it is that family's private helper) and must not be
// re-derived differently: a colour that converts one way when a tool commits it
// and another way when the ontology composites it is a mark that changes when
// the document re-renders.
static inline uint8_t paint_to_byte(float v)
{
    if (v <= 0.0f) return 0;
    if (v >= 1.0f) return 255;
    return static_cast<uint8_t>(v * 255.0f + 0.5f);
}

/*
 * Where a node's top-left sits in ROOT space -- the space a floating pane is
 * positioned in, which is the only reason this exists.
 *
 * Accumulates each ancestor's own offset all the way up, THROUGH compositors
 * rather than stopping at one. That is deliberately not the rule a drawable uses
 * to paint itself (a compositor is a coordinate origin, so a child painting into
 * it stops there -- PolygonDrawable2D::parentAbsoluteOrigin). Here the question
 * is different: a popup is a sibling of the toolbar's compositor, not a child of
 * it, so it needs the toolbar's offset included to be placed against something
 * inside it.
 */
static inline Point2D paint_root_origin(ETCS::RID node)
{
    Point2D acc{ 0, 0 };
    ETCS::Held<Drawable2D_> h = ETCS::resolve_held<Drawable2D_>("Drawable2D", node);
    if (!h) return acc;
    for (ETCS::Entity* e = static_cast<ETCS::Entity*>(h.get()); e; e = e->getParent())
    {
        void* d2 = e->getInterfacePointer(ETCS::Buffer("Drawable2D"));
        if (!d2) break;
        const Rect2D b = static_cast<Drawable2D_*>(d2)->Bounds();
        acc.x += b.x;
        acc.y += b.y;
    }
    return acc;
}

/*
 * Does this pane contain a point given in ROOT space?
 *
 * ContainsLocal answers in the node's OWN coordinates, so asking it directly
 * with a root-space point is only correct for a pane that sits at the origin.
 * Every pane did -- the sheet root is 0,0 -- so a moved pane was simply never
 * offered the event: the colour wheel's popup could be opened, was drawn, was
 * top of the routing order, and still received nothing, because containment was
 * tested 493 pixels away from where it had been placed. The press then fell
 * through to the canvas underneath, whose job on a press outside the wheel is to
 * dismiss it, so the popup closed the instant it was aimed at.
 *
 * Same translation PaintInput::RouteEvent makes before picking, and it has to be
 * the same one, or a pane could be offered an event it then picks nothing from.
 */
static inline bool paint_pane_contains(ETCS::RID root, int32_t x, int32_t y)
{
    ETCS::Held<Drawable2D_> h = ETCS::resolve_held<Drawable2D_>("Drawable2D", root);
    if (!h) return false;
    const Point2D at = paint_root_origin(root);
    return h->ContainsLocal(x - at.x, y - at.y);
}

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

/*
 * The live dab, and IT HAS TO BE THE SHAPE THE MARK WILL BE.
 *
 * This was one DrawRect of 2r x 2r -- a SQUARE nib in the view, while the thing
 * it previews (PaintLayer::DrawBrush) keeps every pixel inside dx^2+dy^2 <= r^2
 * and is a disc. So the nib under the pointer was square, and the stroke turned
 * round the moment anything re-rendered the document through it. A preview that
 * does not agree with its commit is not a fast path, it is a lie about what the
 * tool does.
 *
 * ONE RASTER OP where the destination has host bytes -- Pixels_::FillDisc, which
 * uses the same dx^2+dy^2 <= r^2 test DrawBrush does, so the preview and the mark
 * it becomes are the same discrete circle. A device-backed view has no address to
 * write, so it gets the disc as a stack of spans through the family verb: exactly
 * round, at 2r+1 dispatched calls, which is what that backend costs.
 */
static inline void paint_stamp_surface(ETCS::RID target, int32_t x, int32_t y,
                                       const PaintBrushState& brush)
{
    const int r = std::max(1, static_cast<int>(brush.radius_px));

    if (Pixels_* px = ETCS::resolve_in_family<Pixels_>("Pixels", target))
    {
        px->FillDisc(x, y, static_cast<uint32_t>(r),
                     brush.color.r, brush.color.g, brush.color.b, brush.color.a);
        paint_mark_pixel_path(target);
        return;
    }

    Surface_* surface = ETCS::resolve_in_family<Surface_>("Surface", target);
    if (!surface) return;
    const int r2 = r * r;
    for (int dy = -r; dy <= r; ++dy)
    {
        const int k = static_cast<int>(std::sqrt(static_cast<float>(r2 - dy * dy)));
        surface->DrawRect(x - k, y + dy,
                          static_cast<uint32_t>(k * 2 + 1), 1u,
                          brush.color.r, brush.color.g, brush.color.b, brush.color.a);
    }
    paint_mark_pixel_path(target);
}

/*
 * THE PROJECTED BLIT, FROM BYTES THAT ARE NOT A Pixels_.
 *
 * ontology/ScaledComposite.h has this twice already -- scaled from a Pixels_,
 * and 1:1 from raw bytes -- and a lifted selection needs the corner the two do
 * not cover: raw bytes (a buffer the document keeps beside its layers, not an
 * entity; see PaintSelection) drawn through the view's pan and zoom. A Pixels_
 * cannot be made for it, because Pixels_ is an Entity and a module spawns no
 * entities of its own accord (PaintLayerPanel's header note says why).
 *
 * Same destination-driven, nearest resample and the same non-premultiplied
 * source-over, for the reason ScaledComposite.h gives: one output pixel written
 * once, and a preview that blends differently from the drop it previews is a
 * lie about where the pixels will land.
 *
 * THE ARITHMETIC IS ON BYTES, so it is stated on bytes: the Pixels_ overload
 * below unpacks its destination and forwards. A DESTINATION that is not an
 * entity exists too -- the composite PaintDocument::ExportImage writes to a
 * file, which has no RID to resolve a family on and must not be given one
 * (spawning an entity to hold a scratch buffer is the thing the note above
 * refuses). One blend, two ways of naming where it lands, rather than a sixth
 * copy of the same twelve lines.
 */
static inline void paint_composite_raw_scaled_bytes(uint8_t* dp, uint32_t dstw, uint32_t dsth,
                                                    uint32_t dstride,
                                                    const uint8_t* sp, uint32_t sw, uint32_t sh,
                                                    int32_t x, int32_t y,
                                                    uint32_t dw, uint32_t dh, float opacity)
{
    if (!sp || !dp || sw == 0 || sh == 0 || dw == 0 || dh == 0 || opacity <= 0.0f) return;
    const uint32_t sstride = sw * 4;
    const float o = (opacity > 1.0f) ? 1.0f : opacity;

    const int64_t x0 = std::max<int64_t>(0, x);
    const int64_t y0 = std::max<int64_t>(0, y);
    const int64_t x1 = std::min<int64_t>(dstw, static_cast<int64_t>(x) + dw);
    const int64_t y1 = std::min<int64_t>(dsth, static_cast<int64_t>(y) + dh);

    for (int64_t dy = y0; dy < y1; ++dy)
    {
        const int64_t sy = ((dy - y) * sh) / dh;
        if (sy < 0 || sy >= static_cast<int64_t>(sh)) continue;
        const uint8_t* srow = sp + static_cast<size_t>(sy) * sstride;
        uint8_t*       drow = dp + static_cast<size_t>(dy) * dstride;
        for (int64_t dx = x0; dx < x1; ++dx)
        {
            const int64_t sx = ((dx - x) * sw) / dw;
            if (sx < 0 || sx >= static_cast<int64_t>(sw)) continue;
            const uint8_t* s = srow + static_cast<size_t>(sx) * 4;
            const uint32_t sa = static_cast<uint32_t>(s[3] * o);
            if (sa == 0) continue;
            uint8_t* d = drow + static_cast<size_t>(dx) * 4;
            if (sa >= 255) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255; continue; }
            const uint32_t inv = 255u - sa;
            const uint32_t da  = d[3];
            const uint32_t oa  = sa + (da * inv) / 255u;
            if (oa == 0) { d[0] = d[1] = d[2] = d[3] = 0; continue; }
            d[0] = static_cast<uint8_t>((s[0] * sa + d[0] * da * inv / 255u) / oa);
            d[1] = static_cast<uint8_t>((s[1] * sa + d[1] * da * inv / 255u) / oa);
            d[2] = static_cast<uint8_t>((s[2] * sa + d[2] * da * inv / 255u) / oa);
            d[3] = static_cast<uint8_t>(oa);
        }
    }
}

static inline void paint_composite_raw_scaled(Pixels_& dst,
                                              const uint8_t* sp, uint32_t sw, uint32_t sh,
                                              int32_t x, int32_t y,
                                              uint32_t dw, uint32_t dh, float opacity)
{
    paint_composite_raw_scaled_bytes(dst.PixelData(), dst.PixelWidth(), dst.PixelHeight(),
                                     dst.PixelStride(), sp, sw, sh, x, y, dw, dh, opacity);
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
 *   PLACED       Fill               one point, one commit, no drag at all
 *   ANCHORED     Glyph              drag a box; text is prompted and fitted in it
 *   ANCHORED     Select             drag a region; nothing lands until it is MOVED
 *   MEASURED     Ruler              nothing is ever committed
 *
 * That grouping is what the input edge switches on, and it is why Ruler is a
 * tool rather than a mode: it takes the same press-drag-release a line takes and
 * differs only in committing nothing, so making it anything else would mean a
 * second path through the same gesture.
 *
 * Select is anchored for the same reason and commits nothing for a different
 * one: defining a region changes no pixel. What commits is the drag that begins
 * INSIDE a region -- a carry, which is not a stroke at all (PaintInput's carry
 * branch, beside the text box's), so it is not the anchored commit either.
 */
// GLFW's numbering, which is what arrives on the pointer ring (pushButton takes
// the platform's index unchanged). Named here so the input edge does not test a
// bare 1 and leave the reader to guess which button that is.
/*
 * A KEY CODE AS A CHARACTER, or 0 for "nothing typeable".
 *
 * GLFW's printable codes ARE the ASCII of the unshifted key, which is the whole
 * of this table: letters come in as 'A'..'Z' and are lowered, and everything else
 * printable passes through. The font covers 32..126 (RenderProvider::TextLabel),
 * so what this admits and what can be drawn are the same set.
 *
 * Shift is absent because the event does not carry modifiers -- see
 * PaintModifierKeys, which is where the shift state would have to come from.
 */
static inline char paint_key_to_char(uint16_t key)
{
    if (key >= 'A' && key <= 'Z') return static_cast<char>(key - 'A' + 'a');
    if (key >= 32 && key <= 126)  return static_cast<char>(key);
    return 0;
}

/*
 * ── WHICH MODIFIERS ARE HELD -- THE SHARED MECHANISM ─────────────────────────
 *
 * EVERY CHORD IN THIS MODULE READS IT HERE. Copy and paste (PaintInput::KeyDown)
 * is the first; undo and redo are the next. There must not be a second copy of
 * this, because two observers of one keyboard are two answers to one question,
 * and the one that misses an edge is wrong until the key is pressed again.
 *
 * OBSERVED FROM THE KEY STREAM ITSELF, because the event does not carry
 * modifiers and there is nowhere to put them. InputEvent's spare byte is
 * `buttons` now -- the pointer button mask (ontology/InputSource.h) -- and a key
 * event's copy of it is 0 by construction, so there is no field to read and
 * adding one would mean a window layer filling it before anything here could.
 *
 * What the stream DOES carry is the modifier keys themselves: pushKey filters
 * nothing, so ctrl arrives as an ordinary key-down (341) and an ordinary key-up,
 * exactly like a letter. So "is ctrl held" is answerable by remembering the two
 * edges, which is all this does.
 *
 * NOT InputState::getHeld, which answers the same question and is already in the
 * ontology. That snapshot lives on the PRODUCER -- the window -- and nothing
 * here holds one: events arrive down a stream pair, so reaching the snapshot
 * would mean binding the window into the router and resolving it per keystroke.
 * It would also answer nothing for a chord driven through PaintRouter::Key,
 * which has no window behind it and is how the page and the smoke scripts type.
 *
 * A BIT PER PHYSICAL KEY, not one per modifier, so letting go of the left ctrl
 * while the right is still down does not report ctrl as up.
 *
 * A key-up lost to a focus change -- alt-tab away mid-chord -- leaves that
 * modifier believed down, which is the one failure this shape has. Forget()
 * clears it, for whoever gains a focus edge to call; until then the correction
 * is the next press and release of that same key.
 */
class PaintModifierKeys
{
public:
    // GLFW's codes. Left and right are one modifier and two keys.
    static constexpr uint16_t KEY_LEFT_SHIFT  = 340, KEY_LEFT_CONTROL  = 341;
    static constexpr uint16_t KEY_LEFT_ALT    = 342, KEY_RIGHT_SHIFT   = 344;
    static constexpr uint16_t KEY_RIGHT_CONTROL = 345, KEY_RIGHT_ALT   = 346;

    /*
     * Record an edge, and answer WHETHER THAT KEY WAS A MODIFIER -- which is
     * what lets a caller swallow it. A modifier is not typeable and no pane
     * wants it, so offering it onward would only give something the chance to
     * mistake it for a keystroke.
     */
    bool Note(uint16_t key, bool down)
    {
        const uint8_t bit = bit_of(key);
        if (bit == 0) return false;
        if (down) m_down = static_cast<uint8_t>(m_down | bit);
        else      m_down = static_cast<uint8_t>(m_down & ~bit);
        return true;
    }

    bool ctrl()  const { return (m_down & (CTRL_L  | CTRL_R))  != 0; }
    bool shift() const { return (m_down & (SHIFT_L | SHIFT_R)) != 0; }
    bool alt()   const { return (m_down & (ALT_L   | ALT_R))   != 0; }

    // Everything up. See the note above on the key-up that never arrives.
    void Forget() { m_down = 0; }

private:
    static constexpr uint8_t SHIFT_L = 1u << 0, CTRL_L = 1u << 1, ALT_L = 1u << 2;
    static constexpr uint8_t SHIFT_R = 1u << 3, CTRL_R = 1u << 4, ALT_R = 1u << 5;

    static uint8_t bit_of(uint16_t key)
    {
        switch (key)
        {
        case KEY_LEFT_SHIFT:    return SHIFT_L;
        case KEY_LEFT_CONTROL:  return CTRL_L;
        case KEY_LEFT_ALT:      return ALT_L;
        case KEY_RIGHT_SHIFT:   return SHIFT_R;
        case KEY_RIGHT_CONTROL: return CTRL_R;
        case KEY_RIGHT_ALT:     return ALT_R;
        default: break;
        }
        return 0;
    }

    uint8_t m_down = 0;
};

/*
 * ONE KEYBOARD, SO ONE ANSWER -- held here rather than on a router or an input,
 * because every consumer of a chord needs the same one and none of them can
 * reach another's. A member would have to be plumbed from wherever the keys are
 * routed into everything that interprets one; a copy per consumer would go stale
 * the moment one pane swallowed an edge the others needed.
 *
 * Per module image, which is the right scope: it is this module's readers that
 * consult it, and the keys they are reading arrive on this module's edges.
 */
static inline PaintModifierKeys& paint_modifiers()
{
    static PaintModifierKeys held;
    return held;
}

static constexpr uint16_t PAINT_BUTTON_LEFT   = 0;
static constexpr uint16_t PAINT_BUTTON_RIGHT  = 1;  // GLFW right
static constexpr uint16_t PAINT_BUTTON_MIDDLE = 2;  // GLFW middle -- pan, like right

/*
 * Motion-coalesce interval (ms), and it is 1 -- which is to say, EVERY SAMPLE.
 *
 * This was 100ms scaled per tool, and it was never about input: it was a
 * flicker band-aid. A half-composed frame reached the screen on every re-render,
 * so dropping samples dropped re-renders and the tearing was merely rarer. The
 * frame feed is whole now (CompositeDrawable2D's published frame, batched marks),
 * so there is nothing left to hide and throwing away pointer samples only costs
 * fidelity -- a fast stroke measurably loses its middle.
 *
 * Kept as a knob rather than deleted (PaintTool::SetMotionCoalesceMs) because a
 * genuinely slow destination may still want it; nothing sets it any more.
 */
static constexpr double PAINT_MOTION_COALESCE_DEFAULT_MS = 1.0;

enum class PaintToolKind : uint8_t
{
    Brush,      // stamp every sample, join consecutive ones
    Line,       // anchor to release, straight
    Rect,       // anchor and release as opposite corners
    Ellipse,    // the same two corners, inscribed
    Fill,       // flood from the point, bounded by colour
    Smudge,     // carry pixels along the stroke instead of laying new ones
    Ruler,      // measure and show; commit nothing
    Glyph,      // drag a box; prompt for text; fit the run inside it
    Select,     // drag a region; drag INSIDE it to carry its pixels elsewhere
    Shape,      // the two corners again, as whichever outline PaintShapeMode names
    Eyedrop     // one press: the colour under it becomes the tool's, then the
                // previous kind comes back (PaintColorWheel::BeginPick)
};

/*
 * ── the shape tool's modes ───────────────────────────────────────────────
 *
 * ONE SLICE, SEVERAL OUTLINES. Rect and ellipse were two tools and the bar was
 * running out of room for the third; they are the same gesture -- two corners --
 * differing only in what is drawn between them, which is what a MODE is for
 * (the select tool's is the precedent). Rect and Ellipse stay as kinds of their
 * own for the scripts that name them; Shape with the matching mode draws the
 * same thing through the same primitive.
 */
enum class PaintShapeMode : uint8_t { Rect, Ellipse, Triangle, Diamond, Star };

inline const char* paint_shape_mode_name(PaintShapeMode m)
{
    switch (m)
    {
    case PaintShapeMode::Rect:     return "rect";
    case PaintShapeMode::Ellipse:  return "oval";
    case PaintShapeMode::Triangle: return "triangle";
    case PaintShapeMode::Diamond:  return "diamond";
    case PaintShapeMode::Star:     return "star";
    }
    return "rect";
}
inline PaintShapeMode paint_shape_mode_from(const std::string& n)
{
    if (n == "oval" || n == "ellipse") return PaintShapeMode::Ellipse;
    if (n == "triangle") return PaintShapeMode::Triangle;
    if (n == "diamond")  return PaintShapeMode::Diamond;
    if (n == "star")     return PaintShapeMode::Star;
    return PaintShapeMode::Rect;
}
inline PaintShapeMode paint_shape_mode_next(PaintShapeMode m)
{
    return static_cast<PaintShapeMode>((static_cast<uint8_t>(m) + 1) % 5);
}

// The outline's vertices for two corners, in document space. Rect and ellipse
// are not here: they have their own primitives (DrawRectOutline / Ellipse) and a
// polygon approximation of a circle would be a worse circle.
inline void paint_shape_vertices(PaintShapeMode m, int32_t ax, int32_t ay,
                                 int32_t bx, int32_t by,
                                 std::vector<std::pair<int32_t, int32_t>>& out)
{
    out.clear();
    const int32_t lx = std::min(ax, bx), rx = std::max(ax, bx);
    const int32_t ty = std::min(ay, by), byy = std::max(ay, by);
    const double cx = (lx + rx) * 0.5, cy = (ty + byy) * 0.5;
    const double rw = (rx - lx) * 0.5, rh = (byy - ty) * 0.5;
    switch (m)
    {
    case PaintShapeMode::Triangle:
        out = { { static_cast<int32_t>(cx), ty }, { rx, byy }, { lx, byy } };
        break;
    case PaintShapeMode::Diamond:
        out = { { static_cast<int32_t>(cx), ty }, { rx, static_cast<int32_t>(cy) },
                { static_cast<int32_t>(cx), byy }, { lx, static_cast<int32_t>(cy) } };
        break;
    case PaintShapeMode::Star:
    {
        // Five points, inner radius at 0.42 of the outer -- the proportion of a
        // regular pentagram's inner pentagon, so it reads as a star and not a
        // fat pentagon or a spiky asterisk.
        const double PI = 3.14159265358979323846;
        for (int i = 0; i < 10; ++i)
        {
            const double ang = -PI / 2.0 + i * PI / 5.0;
            const double k = (i % 2 == 0) ? 1.0 : 0.42;
            out.emplace_back(static_cast<int32_t>(std::lround(cx + std::cos(ang) * rw * k)),
                             static_cast<int32_t>(std::lround(cy + std::sin(ang) * rh * k)));
        }
        break;
    }
    default: break;
    }
}

inline const char* paint_tool_kind_name(PaintToolKind k)
{
    switch (k)
    {
    case PaintToolKind::Brush:   return "brush";
    case PaintToolKind::Line:    return "line";
    case PaintToolKind::Rect:    return "rect";
    case PaintToolKind::Ellipse: return "ellipse";
    case PaintToolKind::Shape:   return "shape";
    case PaintToolKind::Eyedrop: return "eyedrop";
    case PaintToolKind::Fill:    return "fill";
    case PaintToolKind::Smudge:  return "smudge";
    case PaintToolKind::Ruler:   return "ruler";
    case PaintToolKind::Glyph:   return "glyph";
    case PaintToolKind::Select:  return "select";
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
    if (name == "select")  return PaintToolKind::Select;
    if (name == "shape")   return PaintToolKind::Shape;
    if (name == "eyedrop") return PaintToolKind::Eyedrop;
    if (name != "brush")
        ETCS_LOG("PaintTool", "unknown tool kind '" << name << "' -- using the brush.");
    return PaintToolKind::Brush;
}

/*
 * HOW THE SELECT TOOL DECIDES WHAT IS INSIDE.
 *
 * One tool, four ways of drawing its boundary. A mode rather than four kinds,
 * because every one of them takes the same press-drag-release, produces the
 * same thing -- a mask on the document -- and hands it to the same carry; four
 * kinds would be four copies of one gesture differing in a single call, and the
 * toolbar would need four slices to say what one arrow says (PaintPalette::
 * AddModeArrow).
 *
 *   Rect, Ellipse   the two corners of the drag, read exactly as the shape
 *                   tools read them, so a rectangle drawn and a rectangle
 *                   selected over the same drag are the same region
 *   Wand            the contiguous run of colour under the press, bounded the
 *                   way a fill is and by the same tolerance
 *                   (PaintLayer::FloodMask), so what the wand takes is what the
 *                   fill would have painted
 *   Lasso           the path the pointer took, closed back to where it began
 */
enum class PaintSelectMode : uint8_t { Rect, Ellipse, Wand, Lasso };

inline const char* paint_select_mode_name(PaintSelectMode m)
{
    switch (m)
    {
    case PaintSelectMode::Rect:    return "rect";
    case PaintSelectMode::Ellipse: return "ellipse";
    case PaintSelectMode::Wand:    return "wand";
    case PaintSelectMode::Lasso:   return "lasso";
    }
    return "rect";
}

// The word a toolbar slice shows for the mode. Separate from the name because
// a slice is 60px and "ellipse" is not -- the ellipse TOOL's own label is
// "oval" for the same reason (paint_toolbar.etcs).
inline const char* paint_select_mode_label(PaintSelectMode m)
{
    return (m == PaintSelectMode::Ellipse) ? "oval" : paint_select_mode_name(m);
}

// An unknown name is the rectangle and says so, for the reason an unknown kind
// is the brush: the plainest mode, rather than no mode.
inline PaintSelectMode paint_select_mode_from(const std::string& name)
{
    if (name == "ellipse" || name == "oval") return PaintSelectMode::Ellipse;
    if (name == "wand")  return PaintSelectMode::Wand;
    if (name == "lasso") return PaintSelectMode::Lasso;
    if (name != "rect")
        ETCS_LOG("PaintTool", "unknown select mode '" << name << "' -- using rect.");
    return PaintSelectMode::Rect;
}

// The order the toolbar's arrow steps through -- the enum's own, wrapping.
inline PaintSelectMode paint_select_mode_next(PaintSelectMode m)
{
    return (m == PaintSelectMode::Lasso)
        ? PaintSelectMode::Rect
        : static_cast<PaintSelectMode>(static_cast<uint8_t>(m) + 1);
}

// Which of the four interaction shapes a kind has. Asked by the input edge
// rather than open-coded there, so adding a tool is one line in each of these
// rather than a new case in every branch that cares.
inline bool paint_kind_is_anchored(PaintToolKind k)
{
    return k == PaintToolKind::Line || k == PaintToolKind::Rect
        || k == PaintToolKind::Ellipse || k == PaintToolKind::Ruler
        || k == PaintToolKind::Glyph || k == PaintToolKind::Select
        || k == PaintToolKind::Shape;
}
inline bool paint_kind_is_placed(PaintToolKind k)
{
    return k == PaintToolKind::Fill || k == PaintToolKind::Eyedrop;
}

inline bool paint_is_pan_button(uint16_t key)
{
    return key == PAINT_BUTTON_RIGHT || key == PAINT_BUTTON_MIDDLE;
}
// Whether letting go of an anchored drag writes to the document. Select does
// not: a region is a mask, and the drag that carries its pixels is a separate
// gesture with its own commit (PaintDocument::DropSelection).
inline bool paint_kind_commits(PaintToolKind k)
{
    return k != PaintToolKind::Ruler && k != PaintToolKind::Select;
}

/*
 * ONE INTERVAL PER KIND, AND THE SPLIT IS NOW ABOUT COST RATHER THAN FLICKER.
 *
 * The old spread was a flicker band-aid and is gone. This one is not the same
 * thing returning: it divides the tools by WHAT A SAMPLE COSTS, which is a real
 * difference and was always the honest reason to coalesce.
 *
 *   CONTINUOUS (brush, smudge)  a sample stamps ONE dab, incrementally, onto the
 *                               view and the layer. Nothing is rebuilt, so every
 *                               sample is worth having -- dropping them loses
 *                               the middle of a fast stroke and nothing else.
 *
 *   ANCHORED (line, rect, ellipse, ruler, glyph, select)  a sample throws the whole
 *                               preview away and rebuilds it: repaint_view
 *                               re-composites the entire document, then the new
 *                               outline is drawn over it. At one sample per
 *                               millisecond that is a full-view rebuild per
 *                               millisecond, which is what made dragging these
 *                               feel heavy -- reported as laggy drags on the
 *                               ruler and the rectangle.
 *
 * SO THE ANCHORED KINDS GET 100ms, AND THE FRAME RATE IS NOT WHAT SETS IT.
 * One frame (16ms) was the first answer here, on the reasoning that a rebuild
 * nobody presents is work nobody sees. That reasoning is sound and the number
 * was still wrong, because it assumed a full-view rebuild FITS in a frame: it
 * does not, so at 16ms the drag asks for another one before the last has landed
 * and the rect and the ruler still felt heavy. 100ms is the interval the rebuild
 * can actually keep, measured by dragging them. A preview is feedback, not the
 * picture -- ten of them a second is enough to aim with, and the committed shape
 * is exact regardless, because release flushes (flush_coalesced_motion).
 */
inline double paint_tool_default_coalesce_ms(PaintToolKind k)
{
    return paint_kind_is_anchored(k) ? 100.0 : PAINT_MOTION_COALESCE_DEFAULT_MS;
}










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

    void SetRadius(float radius)
    {
        m_brush.radius_px = std::max(1.0f, radius);
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

/*
 * ── A SELECTION ──────────────────────────────────────────────────────────────
 *
 * A MASK, WHATEVER SHAPE DREW IT. A rectangle could be four numbers and an
 * ellipse five, but a wand's region is whatever the colour ran to and a lasso's
 * is whatever the hand did, and neither has a shorter description than "these
 * pixels". Holding the general form for all four means everything downstream --
 * is this point inside, lift these pixels, draw this outline -- is written once
 * and cannot disagree between modes about what a region is.
 *
 * ONE BYTE PER DOCUMENT PIXEL rather than a bit, which is eight times the
 * memory and none of the shifting; at a document's size that is under a
 * megabyte, and the outline walk below reads it once per boundary pixel.
 *
 * THE LIFTED PIXELS LIVE HERE TOO, because they are only ever the selection's:
 * a carry cuts the masked pixels out of the layer into `lift`, the offset says
 * where they currently hover, and the drop puts them back through the layer's
 * own blend. Between those two moments the picture on screen is the layer with
 * a hole in it plus this buffer drawn over it -- which is exactly what "the
 * pixels follow the drag" means, and why the layer itself is not touched per
 * sample.
 *
 * NOT A LAYER. It has no order, no name and no opacity, and a document has
 * exactly one; making it a Layer_ would put it in the stack a panel shows.
 */
struct PaintSelection
{
    std::vector<uint8_t> mask;        // one per document pixel; nonzero is inside
    uint32_t w = 0, h = 0;
    int32_t  x0 = 0, y0 = 0;          // bounding box, inclusive. x1 < x0 is
    int32_t  x1 = -1, y1 = -1;        // "nothing selected".
    size_t   count = 0;
    std::vector<uint8_t> lift;        // bbox-sized RGBA, alpha 0 off the mask;
                                      // empty when nothing is lifted
    int32_t  dx = 0, dy = 0;          // where the lift sits, from where it was cut

    bool     empty()  const { return x1 < x0; }
    bool     lifted() const { return !lift.empty(); }
    uint32_t width()  const { return empty() ? 0u : static_cast<uint32_t>(x1 - x0 + 1); }
    uint32_t height() const { return empty() ? 0u : static_cast<uint32_t>(y1 - y0 + 1); }

    bool at(int32_t x, int32_t y) const
    {
        if (x < 0 || y < 0 || static_cast<uint32_t>(x) >= w || static_cast<uint32_t>(y) >= h)
            return false;
        return mask[static_cast<size_t>(y) * w + static_cast<size_t>(x)] != 0;
    }

    // Everything forgotten, including the lift -- a caller that still holds
    // pixels in it lands them first (PaintDocument::fresh_selection).
    void Reset(uint32_t width, uint32_t height)
    {
        w = width; h = height;
        mask.assign(static_cast<size_t>(w) * h, 0);
        x0 = y0 = 0; x1 = y1 = -1;
        count = 0;
        lift.clear();
        dx = dy = 0;
    }

    void Set(int32_t x, int32_t y)
    {
        if (x < 0 || y < 0 || static_cast<uint32_t>(x) >= w || static_cast<uint32_t>(y) >= h)
            return;
        uint8_t& m = mask[static_cast<size_t>(y) * w + static_cast<size_t>(x)];
        if (m) return;
        m = 1;
        ++count;
        if (empty()) { x0 = x1 = x; y0 = y1 = y; return; }
        x0 = std::min(x0, x); x1 = std::max(x1, x);
        y0 = std::min(y0, y); y1 = std::max(y1, y);
    }

    // The region translated, after its pixels were. Whatever leaves the
    // document is gone from the mask too -- the pixels it covered were clipped
    // by the drop, and a selection over nothing is a trap for the next carry.
    void Shift(int32_t sx, int32_t sy)
    {
        if (empty() || (sx == 0 && sy == 0)) return;
        PaintSelection moved;
        moved.Reset(w, h);
        for (int32_t y = y0; y <= y1; ++y)
            for (int32_t x = x0; x <= x1; ++x)
                if (at(x, y)) moved.Set(x + sx, y + sy);
        mask.swap(moved.mask);
        x0 = moved.x0; y0 = moved.y0; x1 = moved.x1; y1 = moved.y1;
        count = moved.count;
    }
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
        // ROUNDED, the same way the ontology converts a float channel
        // (Pixels_::toByte). Truncating here biased every channel down by up to a
        // full level and put the committed mark one step away from the live one
        // that previewed it: a 25% dab read 191 under the pointer and 192 once the
        // document re-rendered, because 0.25*255 is 63.75 and the two paths
        // disagreed about which side of it to land on.
        px[i + 0] = paint_to_byte(r);
        px[i + 1] = paint_to_byte(g);
        px[i + 2] = paint_to_byte(b);
        px[i + 3] = paint_to_byte(a);
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

    /*
 * ── the wand ─────────────────────────────────────────────────────────────
 *
 * FloodFill's walk, writing into a MASK instead of into the pixels: the same
 * scanline runs, the same per-channel tolerance against the seed's own colour,
 * alpha included -- so the region a wand takes is the region a fill at the same
 * point would paint, which is what makes tolerance one number for both.
 *
 * Not a flag on FloodFill, because the two loops end for different reasons. A
 * fill ends when the pixels stop matching the seed, which is true of the pixels
 * it has just repainted; this one repaints nothing, so "already taken" has to
 * be a separate fact, and the mask is that fact. Reading the mask as visited is
 * also what makes the walk terminate on a region that is one flat colour, which
 * a fill only manages by changing the colour.
 *
 * Counted in pixels ADDED, so a seed already inside the selection returns 0
 * rather than re-walking the region it is in.
 */
    size_t FloodMask(int32_t sx, int32_t sy, uint32_t tolerance, PaintSelection& sel) const
    {
        const uint8_t* px = this->PixelData();
        if (!px) return 0;
        const int32_t w = static_cast<int32_t>(this->PixelWidth());
        const int32_t h = static_cast<int32_t>(this->PixelHeight());
        if (sx < 0 || sy < 0 || sx >= w || sy >= h) return 0;

        const size_t seed_i = (static_cast<size_t>(sy) * w + sx) * 4;
        const uint8_t seed[4] = { px[seed_i], px[seed_i+1], px[seed_i+2], px[seed_i+3] };
        const int tol = static_cast<int>(tolerance);
        // Bounded by BOTH rasters. The mask is what records "visited", so a
        // pixel the mask cannot hold is one the walk could revisit forever.
        auto matches = [&](int32_t x, int32_t y) {
            if (x < 0 || y < 0 || x >= w || y >= h) return false;
            if (static_cast<uint32_t>(x) >= sel.w || static_cast<uint32_t>(y) >= sel.h) return false;
            if (sel.at(x, y)) return false;
            const uint8_t* p = px + (static_cast<size_t>(y) * w + x) * 4;
            for (int k = 0; k < 4; ++k)
                if (std::abs(static_cast<int>(p[k]) - static_cast<int>(seed[k])) > tol)
                    return false;
            return true;
        };

        size_t taken = 0;
        std::vector<std::pair<int32_t,int32_t>> stack;
        stack.emplace_back(sx, sy);
        while (!stack.empty())
        {
            auto [x, y] = stack.back();
            stack.pop_back();
            if (!matches(x, y)) continue;

            int32_t left = x;
            while (matches(left - 1, y)) --left;
            int32_t right = x;
            while (matches(right + 1, y)) ++right;

            for (int32_t rx = left; rx <= right; ++rx) { sel.Set(rx, y); ++taken; }
            for (int32_t ny : { y - 1, y + 1 })
            {
                bool run = false;
                for (int32_t rx = left; rx <= right; ++rx)
                {
                    const bool m = matches(rx, ny);
                    if (m && !run) { stack.emplace_back(rx, ny); run = true; }
                    else if (!m)   { run = false; }
                }
            }
        }
        return taken;
    }

    /*
 * ── lift and drop ────────────────────────────────────────────────────────
 *
 * THE CARRY'S TWO HALVES, on the layer because they are the only two things a
 * carry does to one. Lift CUTS: the masked pixels go into `out` (bbox-sized,
 * alpha 0 wherever the mask is off) and the layer is cleared to transparent
 * under them, which is what "the source region is cleared when the move begins"
 * means and is why the hole appears at the press rather than at the release.
 *
 * Drop is SOURCE-OVER, not a replace, because a lifted region is rarely all
 * opaque -- a wand on the ink layer takes the marks and
 * the empty paper between them -- and replacing would punch that emptiness
 * through whatever was at the destination. Blended, the transparent part of the
 * carry lands as nothing, which is what it was. It is also what makes a drop at
 * zero offset put the picture back byte for byte: over a hole, source-over IS
 * the source.
 */
    void LiftPixels(const PaintSelection& sel, std::vector<uint8_t>& out)
    {
        uint8_t* px = this->PixelData();
        const uint32_t bw = sel.width(), bh = sel.height();
        out.assign(static_cast<size_t>(bw) * bh * 4, 0);
        if (!px || bw == 0 || bh == 0) return;
        const int32_t w = static_cast<int32_t>(this->PixelWidth());
        const int32_t h = static_cast<int32_t>(this->PixelHeight());
        for (int32_t y = sel.y0; y <= sel.y1; ++y)
        {
            if (y < 0 || y >= h) continue;
            for (int32_t x = sel.x0; x <= sel.x1; ++x)
            {
                if (x < 0 || x >= w || !sel.at(x, y)) continue;
                uint8_t* src = px + (static_cast<size_t>(y) * w + x) * 4;
                uint8_t* dst = &out[(static_cast<size_t>(y - sel.y0) * bw + (x - sel.x0)) * 4];
                ::std::memcpy(dst, src, 4);
                ::std::memset(src, 0, 4);
            }
        }
        etcs_mark_observed(this);
    }

    // The read half of LiftPixels: the same bytes into the same bbox-sized
    // buffer, and the layer untouched. No mark, because nothing changed.
    void CopyPixels(const PaintSelection& sel, std::vector<uint8_t>& out) const
    {
        const uint8_t* px = this->PixelData();
        const uint32_t bw = sel.width(), bh = sel.height();
        out.assign(static_cast<size_t>(bw) * bh * 4, 0);
        if (!px || bw == 0 || bh == 0) return;
        const int32_t w = static_cast<int32_t>(this->PixelWidth());
        const int32_t h = static_cast<int32_t>(this->PixelHeight());
        for (int32_t y = sel.y0; y <= sel.y1; ++y)
        {
            if (y < 0 || y >= h) continue;
            for (int32_t x = sel.x0; x <= sel.x1; ++x)
            {
                if (x < 0 || x >= w || !sel.at(x, y)) continue;
                ::std::memcpy(&out[(static_cast<size_t>(y - sel.y0) * bw + (x - sel.x0)) * 4],
                              px + (static_cast<size_t>(y) * w + x) * 4, 4);
            }
        }
    }

    // Whole-layer bytes in and out, for the history store. Size-checked rather
    // than trusted: a snapshot taken before a resize must not be written over a
    // buffer of another size.
    bool SnapshotBytes(std::vector<uint8_t>& out) const
    {
        const uint8_t* px = this->PixelData();
        const size_t n = this->PixelBytes();
        if (!px || n == 0) return false;
        out.assign(px, px + n);
        return true;
    }
    bool RestoreBytes(const std::vector<uint8_t>& in)
    {
        uint8_t* px = this->PixelData();
        if (!px || in.size() != this->PixelBytes()) return false;
        ::std::memcpy(px, in.data(), in.size());
        etcs_mark_observed(this);
        return true;
    }

    /*
 * ── re-stating the raster ────────────────────────────────────────────────
 *
 * A NEW EXTENT WITH THE OLD PIXELS TRANSLATED INTO IT: what was at (x, y) is at
 * (x + dx, y + dy), whatever falls outside the new raster is gone, and what
 * nothing landed on is `ground` -- transparent when none is given, which is what
 * every layer but the paper wants (PaintDocument::Resize says which one is the
 * paper and why).
 *
 * Copied out and back rather than moved in place, because the row pitch
 * changes with the width and an in-place shift would read rows it had already
 * overwritten. Allocate is idempotent at the same size (Pixels_::Allocate), so
 * the fill below is what empties the buffer, not the allocation.
 */
    void Rebase(uint32_t w, uint32_t h, int32_t dx, int32_t dy, const float* ground = nullptr)
    {
        if (w == 0 || h == 0) return;
        std::vector<uint8_t> old;
        const int32_t ow = static_cast<int32_t>(this->PixelWidth());
        const int32_t oh = static_cast<int32_t>(this->PixelHeight());
        const bool had = SnapshotBytes(old);

        this->Allocate(w, h);
        uint8_t* px = this->PixelData();
        if (!px) return;
        const uint8_t g[4] = {
            ground ? paint_to_byte(ground[0]) : uint8_t(0), ground ? paint_to_byte(ground[1]) : uint8_t(0),
            ground ? paint_to_byte(ground[2]) : uint8_t(0), ground ? paint_to_byte(ground[3]) : uint8_t(0) };
        for (size_t i = 0; i < this->PixelBytes(); i += 4) ::std::memcpy(px + i, g, 4);

        if (had)
        {
            const int32_t nw = static_cast<int32_t>(w), nh = static_cast<int32_t>(h);
            const int32_t x0 = std::max(0, dx), x1 = std::min(nw, dx + ow);
            const int32_t y0 = std::max(0, dy), y1 = std::min(nh, dy + oh);
            for (int32_t y = y0; y < y1 && x0 < x1; ++y)
                ::std::memcpy(px + (static_cast<size_t>(y) * w + x0) * 4,
                              old.data() + (static_cast<size_t>(y - dy) * ow + (x0 - dx)) * 4,
                              static_cast<size_t>(x1 - x0) * 4);
        }
        etcs_mark_observed(this);
    }

    // Nothing shows through anywhere: every alpha is 255. This is what makes a
    // layer a GROUND rather than marks on one -- see PaintDocument::ground_layer.
    bool Opaque() const
    {
        const uint8_t* px = this->PixelData();
        if (!px) return false;
        for (size_t i = 3; i < this->PixelBytes(); i += 4)
            if (px[i] != 255) return false;
        return true;
    }

    void DropPixels(const uint8_t* bytes, uint32_t bw, uint32_t bh, int32_t x, int32_t y)
    {
        if (!bytes || bw == 0 || bh == 0) return;
        render_composite_raw(*this, bytes, bw, bh, x, y, 1.0f);
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

        /*
     * THE PROJECTION IS APPLIED BY THE SURFACE, which is the only place it can
     * be done once and exactly: the layer's pixels are in DOCUMENT space, the
     * surface wants VIEW space, and Surface_::Blit has carried the destination
     * extent in its w/h since it was written.
     *
     * IT USED TO BE APPROXIMATED HERE, and that was the whole of the pixelation.
     * This walked the SOURCE and emitted one virtual DrawRect per sample, so how
     * much of the picture survived was bounded by how many calls it could
     * afford: `step = 4/zoom` meant every 4th pixel at 100%, each drawn as a 5x5
     * block -- a sixteenth of the stored resolution, with thin marks either
     * missed or fattened into squares, and 1:1 only above 400% zoom. A brush
     * dab looked crisp while StampBrush drew it live at view scale and went
     * blocky the instant anything re-rendered the document through here.
     *
     * One call now, and the write is a destination-driven resample
     * (ontology/ScaledComposite.h). Full resolution at every zoom, one pass over
     * memory instead of ~786k virtual calls.
     *
     * w/h ARE THE DESTINATION EXTENT, the same thing they mean on Surface_::Blit
     * -- zero for "whatever the zoom makes it". They used to clamp the SOURCE
     * here, which no caller ever asked for and which gave one name two meanings
     * across a call boundary a projected document crosses every frame.
     */
        const float z = (zoom <= 0.0f) ? 1.0f : zoom;
        const uint32_t dw = (w != 0) ? w : std::max(1u, static_cast<uint32_t>(pw * z + 0.5f));
        const uint32_t dh = (h != 0) ? h : std::max(1u, static_cast<uint32_t>(ph * z + 0.5f));

        /*
     * NOT THROUGH Surface_::Blit, and the reason is a family boundary rather
     * than a preference. Blit's source parameter is a Surface_, and a layer is
     * not one: the lineage puts Layer ABOVE Surface, and Orderable is claimed
     * once in it, so composing SurfaceBase here would give PaintLayer a second
     * LayerBase subobject and every comparison on it would be ambiguous. A
     * layer owns pixels and an order; it is deliberately not a surface.
     *
     * So the projection is done against the destination's own bytes, with the
     * resample every Blit implementor shares (ontology/ScaledComposite.h) -- one
     * output pixel written once, from an inverse-mapped source. A layer is a
     * Pixels_, so it IS a valid source for it; only "surface" is what it lacks.
     *
     * NO MARK HERE. One layer landing is not a finished picture, and this is
     * called once per visible layer from PaintDocument::RenderToSurface, which
     * marks once when the whole stack is down (PaintSurface::Render). Marking
     * per layer published the paper with no ink on it: a mark is what a
     * compositor takes as "that frame is ready to copy", and it was true four
     * layers early.
     */
        if (Pixels_* dpx = ETCS::resolve_in_family<Pixels_>("Pixels", target))
        {
            render_composite_scaled(*dpx, *this, ox, oy, dw, dh, alpha);
            return;
        }

        // A device-backed destination has no address to blend into, so it keeps
        // the old approximation: one DrawRect per sample, coarse by necessity.
        const size_t bytes = this->PixelBytes();
        const uint32_t step = std::max(1u, static_cast<uint32_t>(4.0f / z));
        const uint32_t rw = std::max(1u, static_cast<uint32_t>(step * z + 1.0f));
        for (uint32_t y = 0; y < ph; y += step)
        {
            for (uint32_t x = 0; x < pw; x += step)
            {
                size_t i = (static_cast<size_t>(y) * pw + x) * 4;
                if (i + 3 >= bytes) continue;
                if (px[i + 3] == 0) continue;
                surface->DrawRect(ox + static_cast<int32_t>(x * z),
                                  oy + static_cast<int32_t>(y * z),
                                  rw, rw,
                                  px[i] / 255.0f, px[i + 1] / 255.0f,
                                  px[i + 2] / 255.0f, (px[i + 3] / 255.0f) * alpha);
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


/*
 * ── RAW IMAGES: PAM IN, PAM OUT ──────────────────────────────────────────────
 *
 * A PATH IS THE INTERFACE, on both substrates, and that is the whole design.
 * A browser hands the page a File and a desktop hands the shell a path; those
 * are two ways of arriving at one thing, bytes at a name the process can open.
 * The page already stages every script it boots into the emscripten filesystem
 * (index.html's preRun: FS.mkdirTree, FS.writeFile), so an upload is one more
 * file written the same way and a download is one file read back. So there is
 * one import and one export with no #ifdef in either, and the substrate's part
 * is getting bytes to and from a name -- which it was already doing.
 *
 * PAM, BECAUSE IT IS THE PIXELS. P7 with TUPLTYPE RGB_ALPHA is exactly what
 * Pixels_ stores -- RGBA8, non-premultiplied, row-major, top-left -- behind a
 * seven-line text header, so a write is the header and one write of the buffer
 * and a read is a tokenizer and one copy. GIMP, ImageMagick and netpbm open it.
 * There is no image codec in libs/, and vendoring one is a decision about the
 * tree, not about this feature: PNG is one header (stb_image, lodepng) away and
 * it drops in HERE -- paint_pam_read fills a PaintImage, and a second reader
 * filling the same struct is all a second format costs. Until that call is
 * made this reads its own PAM plus P6 PPM (what most tools mean by "raw" when
 * alpha is not wanted) and writes PAM only.
 *
 * REFUSED WITH A REASON, never silently: a header this cannot read says what it
 * found and what it takes, because "nothing happened" from an upload button is
 * the failure nobody can act on.
 *
 * Free functions on std types only, so they can be checked in a test that has
 * no runtime under it.
 */
struct PaintImage
{
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> rgba;       // w*h*4, the Pixels_ format
};

// A side longer than this is not a picture anyone paints on here; it is a
// header that lies, and the allocation it asks for is the harm.
static constexpr uint32_t PAINT_IMAGE_MAX_SIDE = 16384;

// The header tokenizer both formats share: words split on whitespace, `#` to
// end of line a comment (PPM says so, PAM allows it). Stops on the byte after
// the word, which is how the caller finds where the raster starts.
struct PaintPamCursor
{
    const uint8_t* p = nullptr;
    size_t n = 0;
    size_t i = 0;

    static bool ws(uint8_t c)
    {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
    }
    bool word(std::string& out)
    {
        for (;;)
        {
            while (i < n && ws(p[i])) ++i;
            if (i < n && p[i] == '#') { while (i < n && p[i] != '\n') ++i; continue; }
            break;
        }
        if (i >= n) return false;
        const size_t s = i;
        while (i < n && !ws(p[i])) ++i;
        out.assign(reinterpret_cast<const char*>(p + s), i - s);
        return true;
    }
};

static inline bool paint_pam_number(const std::string& t, uint32_t& v)
{
    if (t.empty() || t.size() > 9) return false;
    v = 0;
    for (char ch : t)
    {
        if (ch < '0' || ch > '9') return false;
        v = v * 10 + static_cast<uint32_t>(ch - '0');
    }
    return true;
}

static inline bool paint_pam_parse(const uint8_t* p, size_t n, PaintImage& out, std::string& why)
{
    PaintPamCursor c{ p, n, 0 };
    std::string tok;
    if (!c.word(tok)) { why = "empty file"; return false; }

    uint32_t w = 0, h = 0, depth = 0, maxval = 0;
    if (tok == "P6")
    {
        std::string a, b, m;
        if (!c.word(a) || !c.word(b) || !c.word(m)
         || !paint_pam_number(a, w) || !paint_pam_number(b, h) || !paint_pam_number(m, maxval))
        { why = "P6 header is not 'width height maxval'"; return false; }
        depth = 3;
    }
    else if (tok == "P7")
    {
        std::string tupl;
        for (;;)
        {
            if (!c.word(tok)) { why = "P7 header has no ENDHDR"; return false; }
            if (tok == "ENDHDR") break;
            std::string v;
            if (!c.word(v)) { why = "P7 header ends inside " + tok; return false; }
            bool ok = true;
            if      (tok == "WIDTH")    ok = paint_pam_number(v, w);
            else if (tok == "HEIGHT")   ok = paint_pam_number(v, h);
            else if (tok == "DEPTH")    ok = paint_pam_number(v, depth);
            else if (tok == "MAXVAL")   ok = paint_pam_number(v, maxval);
            else if (tok == "TUPLTYPE") tupl = v;
            else { why = "P7 header has an unknown field '" + tok + "'"; return false; }
            if (!ok) { why = "P7 " + tok + " is not a number: '" + v + "'"; return false; }
        }
        if (depth != 3 && depth != 4)
        { why = "DEPTH " + std::to_string(depth) + " -- only 3 (RGB) and 4 (RGB_ALPHA) are read"; return false; }
        if (!tupl.empty() && tupl != "RGB_ALPHA" && tupl != "RGB")
        { why = "TUPLTYPE " + tupl + " -- only RGB_ALPHA and RGB are read"; return false; }
        if (!tupl.empty() && ((tupl == "RGB_ALPHA") != (depth == 4)))
        { why = "TUPLTYPE " + tupl + " does not match DEPTH " + std::to_string(depth); return false; }
    }
    else
    {
        why = "magic '" + tok + "' -- accepted: P7 (PAM, TUPLTYPE RGB_ALPHA or RGB) and P6 (PPM)";
        return false;
    }

    if (maxval != 255)
    { why = "MAXVAL " + std::to_string(maxval) + " -- only 8 bits per channel (255) is read"; return false; }
    if (w == 0 || h == 0 || w > PAINT_IMAGE_MAX_SIDE || h > PAINT_IMAGE_MAX_SIDE)
    {
        why = "size " + std::to_string(w) + "x" + std::to_string(h) + " -- 1.."
            + std::to_string(PAINT_IMAGE_MAX_SIDE) + " on a side";
        return false;
    }

    // Exactly one whitespace byte between the header and the raster (both
    // formats say so), and it is the byte the cursor stopped on.
    size_t at = c.i;
    if (at >= n || !PaintPamCursor::ws(p[at])) { why = "no raster after the header"; return false; }
    ++at;
    const size_t need = static_cast<size_t>(w) * h * depth;
    if (n - at < need)
    {
        why = "raster is short: " + std::to_string(n - at) + " bytes for "
            + std::to_string(w) + "x" + std::to_string(h) + "x" + std::to_string(depth);
        return false;
    }

    out.w = w; out.h = h;
    out.rgba.resize(static_cast<size_t>(w) * h * 4);
    if (depth == 4)
        ::std::memcpy(out.rgba.data(), p + at, need);
    else
        for (size_t s = 0, d = 0; s < need; s += 3, d += 4)
        {
            out.rgba[d + 0] = p[at + s + 0];
            out.rgba[d + 1] = p[at + s + 1];
            out.rgba[d + 2] = p[at + s + 2];
            out.rgba[d + 3] = 255;
        }
    return true;
}

static inline bool paint_pam_read(const std::string& path, PaintImage& out, std::string& why)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) { why = "cannot open " + path; return false; }
    in.seekg(0, std::ios::end);
    const std::streamoff len = in.tellg();
    if (len < 0) { why = "cannot size " + path; return false; }
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(len));
    if (len > 0 && !in.read(reinterpret_cast<char*>(bytes.data()), len))
    { why = "short read on " + path; return false; }
    return paint_pam_parse(bytes.data(), bytes.size(), out, why);
}

// The file's bytes, in memory: the header and the raster, nothing else. Split
// from the writer so a PAM can go somewhere that is not a path -- a database
// row (PaintPages) -- and still be the same bytes the export produces, which
// is what lets one reader (paint_pam_parse) serve both.
static inline bool paint_pam_encode(const uint8_t* rgba, uint32_t w, uint32_t h,
                                    std::vector<uint8_t>& out, std::string& why)
{
    if (!rgba || w == 0 || h == 0) { why = "nothing to write"; return false; }
    const std::string header = "P7\nWIDTH " + std::to_string(w) + "\nHEIGHT " + std::to_string(h)
                             + "\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n";
    const size_t raster = static_cast<size_t>(w) * h * 4;
    out.resize(header.size() + raster);
    ::std::memcpy(out.data(), header.data(), header.size());
    ::std::memcpy(out.data() + header.size(), rgba, raster);
    return true;
}

static inline bool paint_pam_write(const std::string& path,
                                   const uint8_t* rgba, uint32_t w, uint32_t h, std::string& why)
{
    std::vector<uint8_t> bytes;
    if (!paint_pam_encode(rgba, w, h, bytes, why)) return false;
    std::ofstream o(path, std::ios::binary | std::ios::trunc);
    if (!o) { why = "cannot open " + path + " for writing"; return false; }
    o.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!o) { why = "write to " + path + " failed"; return false; }
    return true;
}

// "photo", from "/uploads/photo.pam" -- what an imported layer is called. The
// directory is where it came from and the extension is how it was carried;
// neither is the picture's name.
static inline std::string paint_path_stem(const std::string& path)
{
    const size_t slash = path.find_last_of("/\\");
    std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
    const size_t dot = base.find_last_of('.');
    if (dot != std::string::npos && dot > 0) base.erase(dot);
    return base.empty() ? std::string("Image") : base;
}


/*
 * ── A TEXT BOX ───────────────────────────────────────────────────────────────
 *
 * TEXT THAT IS STILL TEXT. The glyph tool used to prompt for a string, rasterise
 * it into the active layer and forget it -- after which the words were pixels,
 * as editable as a brush stroke and no more. Correcting a typo meant undoing and
 * retyping the lot.
 *
 * So a box keeps its string, in DOCUMENT coordinates, and is drawn on every
 * render from what it holds. It is content rather than decoration: it pans and
 * zooms with the picture, it survives switching tools and layers, and it can be
 * picked up again later and changed.
 *
 * NOT A LAYER, and not a node in the sheet's 2D tree either. A layer is a raster
 * and this is not; a sheet node would float above the picture and never pan with
 * it. What it is, is a second kind of thing the document contains, which is why
 * the document holds them.
 *
 * ONE STRING, NO WRAPPING. The run is scaled to the largest size that fits the
 * box (see fit_text_px), so the box is the type size control -- drag a tall box
 * for big text. Wrapping would need a line breaker and a notion of leading, and
 * neither exists here yet; a second line today is a second box.
 */
struct PaintTextBox
{
    int32_t     x = 0, y = 0;       // document space, top-left
    int32_t     w = 1, h = 1;
    std::string text;
    uint32_t    id = 0;             // stable across edits, unlike an index
    // The colour it was placed with. On the box rather than read from the tool at
    // draw time, because the tool's colour moves on and this text should not: two
    // captions placed with different colours stay different.
    float       rgba[4] = { 0.08f, 0.08f, 0.10f, 1.0f };
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
        Touch();
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

/*
 * ── has anything changed, and since when ─────────────────────────────────
 *
 * A COUNT, NOT A FLAG. "Dirty" as a bool has to be cleared by whoever saved,
 * and two savers -- the page store and, one day, an autosave -- would clear
 * it for each other. A revision that only ever climbs lets each reader keep
 * the number it last saw and compare; nothing is reset and nobody's answer
 * depends on who asked before them (PaintPages::dirty).
 *
 * ONE SEAM, the same one the history has: Remember() is called at every
 * pixel commit, so it calls this. The structural verbs -- reorder, rename,
 * remove, import, the text boxes, a layer's own properties through
 * touch_document -- call it directly, because they change what a page IS and
 * Remember does not see them.
 */
    void Touch() { ++m_revision; }
    uint64_t revision() const { return m_revision; }

    void RenameLayer(ETCS::RID layer_rid, const std::string& name)
    {
        Touch();
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
        Touch();
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
        // The text boxes are content too, and the only way to read one back --
        // they are the one thing here whose state is a string rather than pixels.
        if (!m_text.empty())
            ETCS_LOG("PaintDocument", "  " << m_text.size() << " text box(es)"
                     << (m_text_show ? ", outlines shown" : "")
                     << (m_text_sel ? ", editing " + std::to_string(m_text_sel) : ""));
        for (const PaintTextBox& b : m_text)
            ETCS_LOG("PaintDocument", "  text " << b.id << " at " << b.x << "," << b.y
                     << " " << b.w << "x" << b.h << " = \"" << b.text << "\"");
        // The selection by extent and count -- the count is what a test asserts
        // on, since a wand and a rectangle over the same box differ only there.
        if (!m_sel.empty())
            ETCS_LOG("PaintDocument", "  selection " << m_sel.x0 << "," << m_sel.y0
                     << " .. " << m_sel.x1 << "," << m_sel.y1 << " = " << m_sel.count << " px"
                     << (m_sel.lifted() ? "  lifted, at +" + std::to_string(m_sel.dx)
                                          + "," + std::to_string(m_sel.dy) : std::string()));
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

    /*
 * ── the text boxes this document contains ────────────────────────────────
 *
 * Kept in creation order, which is also their draw order and their pick order
 * reversed: the last one placed is drawn on top, so it is the one a click
 * inside two overlapping boxes means.
 *
 * Addressed by an id rather than an index, because removing one would silently
 * renumber the others and a selection is held across edits.
 */
    uint32_t AddTextBox(int32_t x, int32_t y, int32_t w, int32_t h)
    {
        return AddTextBoxColoured(x, y, w, h, 0.08f, 0.08f, 0.10f, 1.0f);
    }

    uint32_t AddTextBoxColoured(int32_t x, int32_t y, int32_t w, int32_t h,
                                float r, float g, float bl, float a)
    {
        Touch();
        PaintTextBox b;
        b.rgba[0] = r; b.rgba[1] = g; b.rgba[2] = bl; b.rgba[3] = a;
        b.x = x; b.y = y;
        b.w = (w < 1) ? 1 : w;
        b.h = (h < 1) ? 1 : h;
        b.id = ++m_text_seq;
        m_text.push_back(b);
        ETCS_LOG("PaintDocument", "text box " << b.id << " at " << b.x << "," << b.y
                 << " " << b.w << "x" << b.h);
        return b.id;
    }

    PaintTextBox* FindTextBox(uint32_t id)
    {
        for (auto& b : m_text) if (b.id == id) return &b;
        return nullptr;
    }

    bool SetTextBoxText(uint32_t id, const std::string& text)
    {
        Touch();
        PaintTextBox* b = FindTextBox(id);
        if (!b) return false;
        b->text = text;
        return true;
    }

    bool RemoveTextBox(uint32_t id)
    {
        Touch();
        for (auto it = m_text.begin(); it != m_text.end(); ++it)
            if (it->id == id) { m_text.erase(it); return true; }
        return false;
    }

    // Topmost box containing a document-space point, or 0. Reverse order, so the
    // answer matches what is drawn on top.
    uint32_t TextBoxAt(int32_t dx, int32_t dy) const
    {
        for (auto it = m_text.rbegin(); it != m_text.rend(); ++it)
            if (dx >= it->x && dy >= it->y
             && dx < it->x + it->w && dy < it->y + it->h)
                return it->id;
        return 0;
    }

    size_t textBoxCount() const { return m_text.size(); }

    // Whatever leaf claiming Glyphs draws them -- the document needs its own,
    // because it is what renders them, and it may be rendered with no input
    // machine attached at all.
    void BindGlyphs(ETCS::RID glyphs) { m_glyphs = glyphs; }

    /*
 * EDITING AFFORDANCES ARE A VIEW STATE, so they are set from outside rather than
 * inferred here: the document has no opinion about which tool is in hand. The
 * input machine turns this on while the text tool is held (PaintInput), which is
 * what makes every existing box visible and therefore selectable.
 */
    void ShowTextBoxes(bool on)   { m_text_show = on; }
    void SelectTextBox(uint32_t id) { m_text_sel = id; }
    uint32_t selectedTextBox() const { return m_text_sel; }

    /*
 * ── the selection ────────────────────────────────────────────────────────
 *
 * ON THE DOCUMENT, like the active layer and the text box being typed into:
 * "what is selected" is a fact about the picture being edited, so two surfaces
 * onto one document show one selection, and it survives the tool being put
 * down. Held as a mask whatever drew it -- see PaintSelection.
 *
 * Four ways in and one representation out. Each Select* replaces the whole
 * selection rather than adding to it, because that is what one gesture means;
 * a mode that adds or subtracts would be a modifier the event does not carry
 * yet (PaintInput::KeyDown says why).
 *
 * All in DOCUMENT coordinates, like every other verb here that names a place.
 */
    bool SelectRect(int32_t ax, int32_t ay, int32_t bx, int32_t by)
    {
        if (!fresh_selection()) return false;
        // Walked over the page only: a drag can run far off it (the pannable
        // region is three pages wide, PaintSurface::ClampPan) and Set would
        // refuse every one of those pixels one at a time.
        const int32_t lx = std::max(std::min(ax, bx), 0);
        const int32_t rx = std::min(std::max(ax, bx), static_cast<int32_t>(m_width) - 1);
        const int32_t ty = std::max(std::min(ay, by), 0);
        const int32_t by2 = std::min(std::max(ay, by), static_cast<int32_t>(m_height) - 1);
        for (int32_t y = ty; y <= by2; ++y)
            for (int32_t x = lx; x <= rx; ++x)
                m_sel.Set(x, y);
        return !m_sel.empty();
    }

    // Inscribed in the drag, as the ellipse tool is (PaintLayer::
    // DrawEllipseOutline), so the two agree about which pixels a drag names.
    // Tested at pixel centres, so a 1x1 drag selects its one pixel rather than
    // an ellipse of zero area.
    bool SelectEllipse(int32_t ax, int32_t ay, int32_t bx, int32_t by)
    {
        if (!fresh_selection()) return false;
        const int32_t lx = std::min(ax, bx), rx = std::max(ax, bx);
        const int32_t ty = std::min(ay, by), by2 = std::max(ay, by);
        const double cx = lx + (rx - lx + 1) * 0.5, cy = ty + (by2 - ty + 1) * 0.5;
        const double rw = (rx - lx + 1) * 0.5,      rh = (by2 - ty + 1) * 0.5;
        // The geometry from the whole drag, the walk over the page only -- see
        // SelectRect.
        const int32_t wy0 = std::max(ty, 0), wy1 = std::min(by2, static_cast<int32_t>(m_height) - 1);
        const int32_t wx0 = std::max(lx, 0), wx1 = std::min(rx,  static_cast<int32_t>(m_width)  - 1);
        for (int32_t y = wy0; y <= wy1; ++y)
            for (int32_t x = wx0; x <= wx1; ++x)
            {
                const double u = (x + 0.5 - cx) / rw, v = (y + 0.5 - cy) / rh;
                if (u * u + v * v <= 1.0) m_sel.Set(x, y);
            }
        return !m_sel.empty();
    }

    // The wand: the run of colour under the point, on the ACTIVE layer, since
    // that is the layer a carry will cut from and the only one whose colour is
    // the question. Tolerance is the tool's, shared with the fill.
    bool SelectColor(int32_t x, int32_t y, uint32_t tolerance)
    {
        if (!m_active_layer)
        {
            ETCS_LOG("PaintDocument", "wand at " << x << "," << y
                     << " with no active layer -- nothing to read the colour from.");
            return false;
        }
        if (!fresh_selection()) return false;
        const size_t n = m_active_layer->FloodMask(x, y, tolerance, m_sel);
        ETCS_LOG("PaintDocument", "wand at " << x << "," << y << " -> " << n << " px");
        return n != 0;
    }

    /*
 * The lasso: the pointer's path, closed back to its start, filled by scanline
 * with the even-odd rule. Even-odd rather than winding because a hand-drawn
 * loop crosses itself, and even-odd gives the crossing a definite answer -- the
 * lobes -- where winding would depend on which way round each loop went.
 * Sampled at row centres against the edges, so a path that doubles back over
 * a row still contributes one span per crossing pair.
 */
    bool SelectPath(const std::vector<PaintStrokePoint>& path)
    {
        if (path.size() < 3) return false;
        if (!fresh_selection()) return false;
        int32_t ty = path.front().y, by = ty;
        for (const auto& p : path) { ty = std::min(ty, p.y); by = std::max(by, p.y); }
        ty = std::max(ty, 0);
        by = std::min(by, static_cast<int32_t>(m_height) - 1);

        std::vector<double> xs;
        const size_t n = path.size();
        for (int32_t y = ty; y <= by; ++y)
        {
            const double yc = y + 0.5;
            xs.clear();
            for (size_t i = 0; i < n; ++i)
            {
                const PaintStrokePoint& a = path[i];
                const PaintStrokePoint& b = path[(i + 1) % n];
                if ((a.y <= yc) == (b.y <= yc)) continue;    // does not cross this row
                xs.push_back(a.x + (yc - a.y) * (b.x - a.x) / static_cast<double>(b.y - a.y));
            }
            std::sort(xs.begin(), xs.end());
            for (size_t k = 0; k + 1 < xs.size(); k += 2)
            {
                const int32_t x0 = static_cast<int32_t>(std::ceil(xs[k] - 0.5));
                const int32_t x1 = static_cast<int32_t>(std::floor(xs[k + 1] - 0.5));
                for (int32_t x = x0; x <= x1; ++x) m_sel.Set(x, y);
            }
        }
        return !m_sel.empty();
    }

    // Nothing selected. A lift in flight lands where it is first -- clearing
    // must never lose pixels, only the outline round them.
    void ClearSelection()
    {
        if (m_sel.lifted()) DropSelection();
        m_sel.Reset(m_width, m_height);
    }

    // Inside the region as it currently sits -- which, mid-carry, is where the
    // lift hovers rather than where it was cut from.
    bool SelectionContains(int32_t x, int32_t y) const
    {
        if (m_sel.empty()) return false;
        return m_sel.lifted() ? m_sel.at(x - m_sel.dx, y - m_sel.dy) : m_sel.at(x, y);
    }

    /*
 * ── the carry ────────────────────────────────────────────────────────────
 *
 * LIFT, HOVER, DROP. The pixels leave the layer at the lift (PaintLayer::
 * LiftPixels), are drawn from the selection's own buffer at the offset while
 * they hover (draw_lift), and land through the layer's blend at the drop. Three
 * verbs rather than one "move by (dx, dy)" because a pointer does not know the
 * offset until the button comes up, and the picture has to be right at every
 * sample before that; MoveSelection below is the three in a row, for a caller
 * that does know.
 *
 * FROM AND ONTO THE ACTIVE LAYER, not a layer remembered at the lift. Nothing
 * changes the active layer during a drag today, and a layer pointer held across
 * one is the kind of thing a later panel drag would turn into a dangling read.
 */
    bool LiftSelection()
    {
        if (m_sel.empty() || m_sel.lifted()) return false;
        if (!m_active_layer)
        {
            ETCS_LOG("PaintDocument", "nothing to lift the selection from -- no active layer.");
            return false;
        }
        m_active_layer->LiftPixels(m_sel, m_sel.lift);
        m_sel.dx = m_sel.dy = 0;
        return true;
    }

    void SetSelectionOffset(int32_t dx, int32_t dy)
    {
        if (!m_sel.lifted()) return;
        m_sel.dx = dx; m_sel.dy = dy;
    }

    bool DropSelection()
    {
        if (!m_sel.lifted()) return false;
        if (m_active_layer)
            m_active_layer->DropPixels(m_sel.lift.data(), m_sel.width(), m_sel.height(),
                                       m_sel.x0 + m_sel.dx, m_sel.y0 + m_sel.dy);
        else
            ETCS_LOG("PaintDocument", "no active layer to drop the selection onto -- "
                     "the lifted pixels are lost.");
        // The outline follows the pixels, so the same region can be carried
        // again without re-selecting it.
        m_sel.Shift(m_sel.dx, m_sel.dy);
        m_sel.lift.clear();
        m_sel.dx = m_sel.dy = 0;
        return true;
    }

    // The scripted carry: what a drag does, in one call.
    bool MoveSelection(int32_t dx, int32_t dy)
    {
        if (!LiftSelection()) return false;
        SetSelectionOffset(dx, dy);
        return DropSelection();
    }

    const PaintSelection& selection() const { return m_sel; }
    bool hasSelection() const { return !m_sel.empty(); }

    /*
 * ── the clipboard ────────────────────────────────────────────────────────
 *
 * OWNED BY THE DOCUMENT, not the input: a copy taken with one input machine
 * has to be pasteable by another, and the document is the one thing every
 * consumer of the picture already reaches. It holds the selection's SHAPE as
 * well as its bytes, so a paste puts back the same outline and not a rectangle
 * around it -- a lasso'd copy pastes as the lasso.
 *
 * WHERE A PASTE GOES: it FLOATS over where the copy came from, as a lift that
 * has not been dropped -- the same state a carry is in between press and
 * release, so no fourth state is needed. It cannot land at once: landing on
 * its own source is source-over onto identical bytes, which changes nothing,
 * and the next carry then takes the original along with it -- a paste that
 * duplicates nothing. Floating, the original stays where it is, the next drag
 * inside carries only the copy, and everything that already lands a carry in
 * flight (a new selection, Escape, a tool change) lands the paste the same way.
 *
 * AFTER A RESIZE the bytes are still the bytes; the mask is re-stated against
 * the document's current size at paste, and whatever falls outside it is
 * clipped by DropPixels. A layer that has gone away between copy and paste
 * costs nothing: the paste lands on whichever layer is active THEN.
 */
    bool CopySelection()
    {
        if (m_sel.empty() || !m_active_layer) return false;
        m_clip.mask = m_sel.mask;
        m_clip.w = m_sel.w; m_clip.h = m_sel.h;
        m_clip.x0 = m_sel.x0; m_clip.y0 = m_sel.y0; m_clip.x1 = m_sel.x1; m_clip.y1 = m_sel.y1;
        m_clip.count = m_sel.count;
        // A lifted region's bytes are in its own buffer, not the layer.
        if (m_sel.lifted()) m_clip.lift = m_sel.lift;
        else                m_active_layer->CopyPixels(m_sel, m_clip.lift);
        m_clip.dx = m_clip.dy = 0;
        return !m_clip.lift.empty();
    }

    bool PasteSelection()
    {
        if (m_clip.lift.empty() || !m_active_layer) return false;
        Remember();
        if (m_sel.lifted()) DropSelection();      // a carry in flight lands first
        // The clip's mask, restated in the current document's space.
        m_sel.Reset(width(), height());
        for (int32_t y = m_clip.y0; y <= m_clip.y1; ++y)
            for (int32_t x = m_clip.x0; x <= m_clip.x1; ++x)
                if (m_clip.at(x, y)) m_sel.Set(x, y);
        if (m_sel.empty()) return false;
        // The lift IS the clip's bytes, cut to the bbox the restated mask has --
        // identical to the clip's unless the document shrank.
        m_sel.lift.assign(static_cast<size_t>(m_sel.width()) * m_sel.height() * 4, 0);
        const uint32_t cw = m_clip.width();
        for (int32_t y = m_sel.y0; y <= m_sel.y1; ++y)
            for (int32_t x = m_sel.x0; x <= m_sel.x1; ++x)
            {
                if (!m_sel.at(x, y)) continue;
                const size_t si = (static_cast<size_t>(y - m_clip.y0) * cw + (x - m_clip.x0)) * 4;
                const size_t di = (static_cast<size_t>(y - m_sel.y0) * m_sel.width() + (x - m_sel.x0)) * 4;
                if (si + 4 <= m_clip.lift.size()) ::std::memcpy(&m_sel.lift[di], &m_clip.lift[si], 4);
            }
        m_sel.dx = m_sel.dy = 0;
        return true;     // floating: see above
    }

    // The Delete key: the pixels go and the outline stays, so the same region
    // can be filled or pasted into next. Cut without the copy.
    bool DeleteSelection()
    {
        if (m_sel.empty() || !m_active_layer) return false;
        Remember();
        if (m_sel.lifted()) { m_sel.lift.clear(); m_sel.dx = m_sel.dy = 0; return true; }
        if (!LiftSelection()) return false;
        m_sel.lift.clear();
        return true;
    }

    // Copy, then take the pixels: the lift clears them and the buffer is let go.
    bool CutSelection()
    {
        if (!CopySelection()) return false;
        Remember();
        if (!LiftSelection()) return false;
        m_sel.lift.clear();
        return true;
    }

    bool hasClip() const { return !m_clip.lift.empty(); }

    /*
 * ── history, three deep ──────────────────────────────────────────────────
 *
 * A PLACEHOLDER, AND SHAPED AS ONE. The real history comes from the
 * persistence tag: the input event stream is itself the record of what
 * happened, and undo will be a replay of it -- unbounded, and kept across
 * sessions once saving is in. Until then this remembers the last few
 * changes as whole-layer snapshots, which is the simplest correct thing at
 * this depth and exactly what the real store will replace.
 *
 * ONE SEAM. Every committed change calls Remember() before it lands, and
 * that call is the one place the replacement plugs in: swap what Remember
 * records and what Undo/Redo restore, and no call site moves. So the depth
 * cap is a named constant and the store is a private type, and nothing
 * outside this block knows either exists.
 *
 * WHAT IT COSTS: one copy of the active layer's bytes per change, three deep
 * each way. 3 MB a snapshot at 1024x768, 18 MB in the worst case. Acceptable
 * for three; the reason the real store is not snapshots.
 */
    static constexpr size_t HISTORY_DEPTH = 3;

    void Remember()
    {
        Touch();
        if (!m_active_layer) return;
        HistoryEntry e;
        e.layer = m_active_layer->getRID();
        if (!m_active_layer->SnapshotBytes(e.bytes)) return;
        if (m_undo.size() >= HISTORY_DEPTH) m_undo.erase(m_undo.begin());
        m_undo.push_back(std::move(e));
        m_redo.clear();          // a new change is a new future
    }

    bool Undo() { return step(m_undo, m_redo, "undo"); }
    bool Redo() { return step(m_redo, m_undo, "redo"); }

    size_t undoDepth() const { return m_undo.size(); }
    size_t redoDepth() const { return m_redo.size(); }

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
            // The lifted pixels are the active layer's, so they are drawn at
            // its depth -- over it, under whatever is stacked above it -- which
            // is where they will be once dropped.
            if (layer == m_active_layer && m_sel.lifted()) draw_lift(target, x, y, zoom, layer);
        }

        // Over the layers, because a text box is not in any of them -- it is
        // still text and is drawn from its string every frame. See PaintTextBox.
        draw_text_boxes(target, x, y, zoom);
        // And over everything, because an outline is chrome: it says where the
        // region is and must not be hidden by a layer above the one it is on.
        draw_selection(surface, x, y, zoom);
    }

    /*
 * ── raw images in and out ────────────────────────────────────────────────
 *
 * BY PATH, and the page and the terminal call the same three verbs: see the
 * PAM note above PaintImage for why a path is the one interface the browser
 * and the desktop share.
 *
 * IMPORT IS A NEW LAYER, made the way the boot script makes one. addTag<> is
 * what `doc.spawn(PaintProvider::PaintLayer)` reduces to -- _make_child_
 * PaintLayer is `parent->addTag<PaintLayer>()` (ETCS_API.h) -- so the layer is
 * this document's typed child like every other, and membership is parenthood
 * with nothing to keep in step (OrderedLayers). A module may not spawn ANOTHER
 * module's type (PaintLayerPanel's note); its own it spawns exactly this way,
 * as Shell and ChessNode do.
 *
 * SIZED TO THE IMAGE, not the page: a layer's raster is its own (PaintLayer::
 * Create), the blit clips at the page edge, and resampling on the way in would
 * throw away pixels the user may yet pan into view. On top and active, because
 * what was just brought in is what is about to be worked on.
 *
 * NOT REMEMBERED. The history is whole-layer snapshots of the active layer
 * (Remember), and a new layer has no earlier bytes to return to; undoing an
 * import is RemoveLayer, which is what a row's delete already does.
 */
    bool ImportImage(const std::string& path)
    {
        Touch();
        PaintImage img;
        std::string why;
        if (!paint_pam_read(path, img, why))
        {
            ETCS_LOG("PaintDocument", "import " << path << ": " << why);
            return false;
        }
        PaintLayer* layer = this->addTag<PaintLayer>();
        if (!layer)
        {
            ETCS_LOG("PaintDocument", "import " << path << ": could not spawn a layer under '"
                     << m_name << "'.");
            return false;
        }
        layer->Create(img.w, img.h);
        layer->RestoreBytes(img.rgba);
        layer->SetName(paint_path_stem(path));

        // One past the highest key, which is the top: MoveLayerTo keeps the
        // stack dense, so nothing is above it and nothing has to be renumbered.
        std::vector<PaintLayer*> stack;
        OrderedLayers(stack);
        int32_t top = 0;
        for (auto* l : stack)
            if (l != layer) top = std::max(top, l->order() + 1);
        layer->SetOrder(top);

        // A carry in flight belongs to the layer that was active; it lands there
        // before the active layer changes under it (fresh_selection says why).
        if (m_sel.lifted()) DropSelection();
        m_active_layer = layer;

        ETCS_LOG("PaintDocument", "imported " << path << " " << img.w << "x" << img.h
                 << " -> layer '" << layer->name() << "' RID:" << layer->getRID()
                 << " order=" << top << ", active");
        return true;
    }

    /*
 * THE COMPOSITE IS WHAT RenderToSurface SHOWS, less the view: the visible
 * layers bottom first, each at its own opacity, and a lift in flight at the
 * active layer's depth -- the same walk, through the same blend
 * (paint_composite_raw_scaled_bytes is what BlitTo and draw_lift land through
 * on a host-backed view; 1:1 here, since a file has no zoom). Onto a
 * transparent page, so a picture with no paper layer leaves with its alpha
 * rather than over a colour nobody asked for.
 *
 * NOT THE DIM: that is a hover (SetDim) and a file is precisely the trace a
 * hover must not leave. NOT THE TEXT BOXES: they are strings drawn through a
 * Glyphs target by RID (draw_text_boxes) and a buffer has no RID; the export
 * log counts them so a file that lost its captions says so. Not the selection
 * outline, which is chrome.
 */
    bool CompositeVisible(std::vector<uint8_t>& out, size_t* shown = nullptr) const
    {
        if (m_width == 0 || m_height == 0) return false;
        out.assign(static_cast<size_t>(m_width) * m_height * 4, 0);
        std::vector<PaintLayer*> stack;
        OrderedLayers(stack);
        size_t n = 0;
        for (auto* layer : stack)
        {
            if (!layer->visible()) continue;
            ++n;
            paint_composite_raw_scaled_bytes(out.data(), m_width, m_height, m_width * 4,
                                             layer->PixelData(), layer->width(), layer->height(),
                                             0, 0, layer->width(), layer->height(),
                                             layer->opacity());
            if (layer == m_active_layer && m_sel.lifted())
                paint_composite_raw_scaled_bytes(out.data(), m_width, m_height, m_width * 4,
                                                 m_sel.lift.data(), m_sel.width(), m_sel.height(),
                                                 m_sel.x0 + m_sel.dx, m_sel.y0 + m_sel.dy,
                                                 m_sel.width(), m_sel.height(),
                                                 layer->opacity());
        }
        if (shown) *shown = n;
        return true;
    }

    /*
 * ONE PIXEL OF WHAT IS SEEN, for the eyedropper. The same walk as
 * CompositeVisible, bottom to top through the visible layers with each one's
 * opacity, over transparent -- and not a call to it, because compositing the
 * whole page to read one pixel is a frame's worth of work for a click.
 * Straight source-over on straight RGBA, which is what the layers hold.
 */
    bool SampleAt(int32_t x, int32_t y, float rgba[4]) const
    {
        if (x < 0 || y < 0 || static_cast<uint32_t>(x) >= m_width
            || static_cast<uint32_t>(y) >= m_height) return false;
        float dr = 0, dg = 0, db = 0, da = 0;
        std::vector<PaintLayer*> stack;
        OrderedLayers(stack);
        for (auto* layer : stack)
        {
            if (!layer->visible()) continue;
            const uint8_t* px = layer->PixelData();
            if (!px || static_cast<uint32_t>(x) >= layer->width()
                || static_cast<uint32_t>(y) >= layer->height()) continue;
            const uint8_t* p = px + (static_cast<size_t>(y) * layer->width() + x) * 4;
            const float sa = (p[3] / 255.0f) * layer->opacity();
            if (sa <= 0.0f) continue;
            const float sr = p[0] / 255.0f, sg = p[1] / 255.0f, sb = p[2] / 255.0f;
            const float oa = sa + da * (1.0f - sa);
            if (oa > 0.0f)
            {
                dr = (sr * sa + dr * da * (1.0f - sa)) / oa;
                dg = (sg * sa + dg * da * (1.0f - sa)) / oa;
                db = (sb * sa + db * da * (1.0f - sa)) / oa;
            }
            da = oa;
        }
        rgba[0] = dr; rgba[1] = dg; rgba[2] = db; rgba[3] = da;
        return true;
    }

    bool ExportImage(const std::string& path)
    {
        std::vector<uint8_t> px;
        size_t shown = 0;
        if (!CompositeVisible(px, &shown))
        {
            ETCS_LOG("PaintDocument", "export " << path << ": '" << m_name << "' has no extent.");
            return false;
        }
        std::string why;
        if (!paint_pam_write(path, px.data(), m_width, m_height, why))
        {
            ETCS_LOG("PaintDocument", "export " << path << ": " << why);
            return false;
        }
        ETCS_LOG("PaintDocument", "exported " << path << " " << m_width << "x" << m_height
                 << " PAM RGB_ALPHA, " << shown << " visible layer(s)"
                 << (m_text.empty() ? std::string()
                                    : "; " + std::to_string(m_text.size())
                                      + " text box(es) are not in it"));
        return true;
    }

    // The active layer's own bytes, alpha and all, at its own size -- what
    // SnapshotBytes already hands the history.
    bool ExportLayer(const std::string& path)
    {
        if (!m_active_layer)
        {
            ETCS_LOG("PaintDocument", "export layer " << path << ": no active layer.");
            return false;
        }
        std::vector<uint8_t> px;
        if (!m_active_layer->SnapshotBytes(px))
        {
            ETCS_LOG("PaintDocument", "export layer " << path << ": '" << m_active_layer->name()
                     << "' has no pixels -- Create it first.");
            return false;
        }
        std::string why;
        if (!paint_pam_write(path, px.data(), m_active_layer->width(), m_active_layer->height(), why))
        {
            ETCS_LOG("PaintDocument", "export layer " << path << ": " << why);
            return false;
        }
        ETCS_LOG("PaintDocument", "exported layer '" << m_active_layer->name() << "' RID:"
                 << m_active_layer->getRID() << " " << m_active_layer->width() << "x"
                 << m_active_layer->height() << " PAM RGB_ALPHA -> " << path);
        return true;
    }

    /*
 * ── re-stating the page ──────────────────────────────────────────────────
 *
 * ANCHORED, NOT SCALED. A resize here is a change of EXTENT: the picture keeps
 * its pixels and the page grows or shrinks around them, and the anchor says
 * which part of the page stays where it is -- a 3x3 grid, 0 the top-left
 * corner, 4 the centre, 8 the bottom-right, read as (anchor % 3, anchor / 3).
 * Growing from the centre puts new room on every side; growing from the
 * top-left puts it all on the right and the bottom. Resampling is a different
 * operation with a different name and does not exist here yet.
 *
 * EVERY LAYER TAKES THE PAGE'S SIZE, translated by the page's own shift. A layer
 * is allowed its own raster (an import is sized to its image, ImportImage), but
 * once the page is re-stated the honest answer to "how big is this layer" is the
 * page: what an oversize layer kept beyond the old edge was already outside the
 * composite (CompositeVisible clips to the page), and keeping nine separate
 * extents in step across a shift is bookkeeping nobody can see the result of.
 *
 * THE PAPER'S NEW AREA IS PAPER, everything else's is transparent. Which layer
 * is the paper is answered by ground_layer; the colour is the convention the
 * page boots with (boot_paint_panels.etcs: `paper.Clear(1.0, 1.0, 1.0, 1.0)`).
 *
 * NOT UNDOABLE, AND THE HISTORY IS DROPPED RATHER THAN LEFT TO LIE. The store
 * is three whole-layer snapshots of the ACTIVE layer at its current size
 * (Remember), restored only into a buffer of the same size (RestoreBytes is
 * size-checked) -- so no entry it can hold brings back the old extent, and an
 * entry kept across a resize answers every later Undo with "layer size changed
 * -- entry dropped" one press at a time. Clearing them says the same thing
 * once. A resize becomes a step when the history is the replayed event stream
 * the note above Remember describes.
 *
 * The selection and the text boxes go with the pixels, since they are places
 * IN the picture; a lift in flight lands first, on the page it was cut from.
 */
    bool Resize(uint32_t w, uint32_t h, int anchor)
    {
        Touch();
        if (!extent_ok(w, h, "resize")) return false;
        if (m_sel.lifted()) DropSelection();

        const int32_t dx = anchor_shift(anchor % 3, m_width, w);
        const int32_t dy = anchor_shift(anchor / 3, m_height, h);

        std::vector<PaintLayer*> stack;
        OrderedLayers(stack);
        PaintLayer* paper = ground_layer(stack);
        for (auto* l : stack) l->Rebase(w, h, dx, dy, (l == paper) ? PAPER_CLEAR : nullptr);

        // The mask re-stated in the new page's space, pixel for pixel with the
        // layers it selects on.
        if (!m_sel.empty())
        {
            PaintSelection moved;
            moved.Reset(w, h);
            for (int32_t y = m_sel.y0; y <= m_sel.y1; ++y)
                for (int32_t x = m_sel.x0; x <= m_sel.x1; ++x)
                    if (m_sel.at(x, y)) moved.Set(x + dx, y + dy);
            m_sel = std::move(moved);
        }
        else m_sel.Reset(w, h);
        for (PaintTextBox& b : m_text) { b.x += dx; b.y += dy; }

        m_undo.clear();
        m_redo.clear();
        ETCS_LOG("PaintDocument", "'" << m_name << "' " << m_width << "x" << m_height
                 << " -> " << w << "x" << h << " anchored at " << anchor
                 << " (pixels moved by " << dx << "," << dy << "), " << stack.size()
                 << " layer(s)" << (paper ? ", paper '" + paper->name() + "' extended" : "")
                 << "; history dropped -- a resize is not a step the snapshot store can hold.");
        m_width = w;
        m_height = h;
        return true;
    }

    /*
 * A NEW CANVAS: the same layers, the new extent, and nothing on them -- paper
 * to its clear colour, every other layer transparent. The layers stay rather
 * than being removed and re-spawned, because their names, order and the rows a
 * layer window has bound to them are the user's arrangement, not the picture.
 * The text boxes are the picture, so they go; the clipboard is not, so it
 * stays. Not undoable, for the reason Resize gives.
 */
    bool New(uint32_t w, uint32_t h)
    {
        Touch();
        if (!extent_ok(w, h, "new")) return false;
        std::vector<PaintLayer*> stack;
        OrderedLayers(stack);
        PaintLayer* paper = ground_layer(stack);
        for (auto* l : stack)
        {
            l->Allocate(w, h);          // idempotent at the same size: Clear is what empties it
            if (l == paper) l->Clear(PAPER_CLEAR[0], PAPER_CLEAR[1], PAPER_CLEAR[2], PAPER_CLEAR[3]);
            else            l->Clear(0.0f, 0.0f, 0.0f, 0.0f);
        }
        m_sel.Reset(w, h);               // the lift goes with it: there is nothing to land on
        m_text.clear();
        m_text_sel = 0;
        m_undo.clear();
        m_redo.clear();
        m_width = w;
        m_height = h;
        ETCS_LOG("PaintDocument", "'" << m_name << "' new " << w << "x" << h << ", "
                 << stack.size() << " layer(s) cleared"
                 << (paper ? ", paper '" + paper->name() + "' to its clear colour" : ""));
        return true;
    }


    /*
 * ── becoming another page ────────────────────────────────────────────────
 *
 * The two halves a page store needs and nothing else here provides: every
 * layer GONE, and a layer MADE from bytes rather than from a file. Both are
 * the document's because both are statements about its children.
 *
 * DESTROYED, NOT DETACHED. RemoveLayer detaches so a script's name still
 * resolves; a page switch is the opposite case -- nobody holds these layers
 * and the next switch would leak another stack of rasters, 3 MB each at this
 * size. So each goes through the same DestroyEvent a Delete verb fires, keyed
 * the way SqliteLocalDatabase::DeleteConcrete keys its own, which runs the
 * destructor and returns the arena footprint. The RID is read before the
 * event because the pointer is not valid after it.
 *
 * NO HOLD ACROSS THIS: a DestroyEvent waits on the ordering thread, and the
 * ordering thread may be waiting on a lifetime hold -- the one deadlock the
 * mechanism can build (Entity.h, lifetime_hold_depth). A caller that has a
 * Held<> open reads what it needs, drops it, and only then calls here.
 *
 * The history goes with the layers: its entries name them by RID and a step
 * onto a dead one is dropped with a message, but a page that was never
 * painted on has nothing to undo TO, and saying so is more honest than an
 * entry that fails later (the same reason an import is not remembered). The
 * text boxes go too: they are content, and content that belongs to the page
 * being left cannot stay on the one arriving.
 */
    void DestroyLayers()
    {
        m_sel.Reset(m_width, m_height);              // the carry's layer is about to go, lift and all
        m_active_layer = nullptr;
        m_undo.clear();
        m_redo.clear();
        m_text.clear();
        m_text_sel = 0;

        std::vector<PaintLayer*> stack;
        OrderedLayers(stack);
        for (PaintLayer* l : stack)
        {
            const std::string key = l->getSourceModule().toString() + ":" + l->getSourceTag().toString();
            const ETCS::RID rid = l->getRID();
            if (!ETCS::DestroyEvent{key.c_str(), l}())
                ETCS_LOG("PaintDocument", "layer RID:" << rid << " refused to be destroyed -- detaching it instead.");
        }
        // Whatever refused is still a child; it must at least leave the stack.
        OrderedLayers(stack);
        for (PaintLayer* l : stack) l->detachFromParent();
        Touch();
    }

    // The import's shape (ImportImage) with the file taken out: a typed child,
    // sized to its bytes, with its own name, key, visibility and opacity, made
    // ACTIVE if asked. The caller keys it -- a stored page brings its own
    // dense order, so nothing is renumbered here.
    PaintLayer* SpawnLayer(const PaintImage& img, const std::string& name, int32_t order,
                           bool visible, float opacity, bool active)
    {
        PaintLayer* layer = this->addTag<PaintLayer>();
        if (!layer)
        {
            ETCS_LOG("PaintDocument", "could not spawn layer '" << name << "' under '" << m_name << "'.");
            return nullptr;
        }
        layer->Create(img.w, img.h);
        layer->RestoreBytes(img.rgba);
        layer->SetName(name);
        layer->SetOrder(order);
        layer->SetVisible(visible);
        layer->SetOpacity(opacity);
        if (active) m_active_layer = layer;
        Touch();
        return layer;
    }

    /*
 * ── drawing the text boxes ───────────────────────────────────────────────
 *
 * PROJECTED LIKE THE PIXELS ARE, which is the whole reason this happens here
 * rather than as a node over the view: a box is at a place in the DOCUMENT, so
 * panning and zooming have to move and scale it exactly as they move the paper
 * under it. The projection is the same one the layers get -- multiply by the
 * zoom, offset by the pan -- applied to the box rather than to a raster.
 *
 * The outline is drawn only while the text tool is held (ShowTextBoxes), because
 * the rest of the time these are words in a picture and a rectangle round them
 * would be a lie about what will print.
 */
    void draw_text_boxes(ETCS::RID target, int32_t ox, int32_t oy, float zoom)
    {
        if (m_text.empty()) return;
        Surface_* surface = ETCS::resolve_in_family<Surface_>("Surface", target);
        if (!surface) return;

        ETCS::Held<Glyphs_> g;
        if (m_glyphs != 0) g = ETCS::resolve_held<Glyphs_>("Glyphs", m_glyphs);
        if (!g && m_text_warned == false)
        {
            m_text_warned = true;
            ETCS_LOG("PaintDocument", "there are text boxes but no glyph provider "
                     "bound -- BindGlyphs first, or they cannot be drawn.");
        }

        const float z = (zoom <= 0.0f) ? 1.0f : zoom;
        for (const PaintTextBox& b : m_text)
        {
            const int32_t vx = ox + static_cast<int32_t>(b.x * z);
            const int32_t vy = oy + static_cast<int32_t>(b.y * z);
            const int32_t vw = std::max(1, static_cast<int32_t>(b.w * z));
            const int32_t vh = std::max(1, static_cast<int32_t>(b.h * z));

            if (m_text_show)
            {
                /*
             * A one-pixel frame, as four thin rects -- that is what a surface can
             * draw. Coloured rather than pale: the first version was near-white
             * with low alpha, which is invisible on the paper it is drawn on, and
             * an affordance you cannot see is not one. Blue reads against both
             * white paper and dark ink; the one being typed into is stronger and
             * fully opaque, the rest are dimmer, so "which box has the keyboard"
             * is answerable at a glance.
             */
                const bool sel = (b.id == m_text_sel);
                const float r0 = sel ? 0.15f : 0.35f;
                const float g0 = sel ? 0.50f : 0.45f;
                const float b0 = sel ? 0.95f : 0.60f;
                const float a  = sel ? 1.00f : 0.55f;
                surface->DrawRect(vx, vy, static_cast<uint32_t>(vw), 1u, r0, g0, b0, a);
                surface->DrawRect(vx, vy + vh - 1, static_cast<uint32_t>(vw), 1u, r0, g0, b0, a);
                surface->DrawRect(vx, vy, 1u, static_cast<uint32_t>(vh), r0, g0, b0, a);
                surface->DrawRect(vx + vw - 1, vy, 1u, static_cast<uint32_t>(vh), r0, g0, b0, a);
            }

            if (!g || b.text.empty()) continue;

            TextExtent e{ 0, 0, 0 };
            const uint32_t px = fit_text_px(g.get(), b.text, vw, vh, e);
            const int32_t tx = vx + (vw - static_cast<int32_t>(e.width))  / 2;
            const int32_t ty = vy + (vh - static_cast<int32_t>(e.height)) / 2;
            g->RasterizeText(target, b.text.c_str(), 0, px, tx, ty,
                             b.rgba[0], b.rgba[1], b.rgba[2], b.rgba[3]);
        }
    }

    /*
 * The largest size whose run fits the box, which is what "the glyph that fits
 * inside of it" means. The font is cell-based, so the sizes are discrete and a
 * linear walk up finds the boundary in a few steps; once a size does not fit,
 * no larger one will.
 */
    static uint32_t fit_text_px(Glyphs_* g, const std::string& text,
                                int32_t box_w, int32_t box_h, TextExtent& out)
    {
        uint32_t best = 1;
        out = g->MeasureText(text.c_str(), 0, 1);
        const uint32_t max_px = static_cast<uint32_t>(std::max(1, box_h));
        for (uint32_t px = 1; px <= max_px; ++px)
        {
            const TextExtent e = g->MeasureText(text.c_str(), 0, px);
            if (static_cast<int32_t>(e.width) <= box_w
             && static_cast<int32_t>(e.height) <= box_h)
            { best = px; out = e; }
            else if (px > 1) break;
        }
        return best;
    }

    /*
 * A NEW SELECTION STARTS FROM NOTHING, and never from a lift still in the air:
 * pixels cut for a carry that was then abandoned by a fresh Select* would
 * otherwise vanish with the mask that described them. They land where they
 * hover, which is where the last sample left them.
 *
 * False for a document with no extent, which has nothing to select in.
 */
    bool fresh_selection()
    {
        if (m_width == 0 || m_height == 0) return false;
        if (m_sel.lifted()) DropSelection();
        m_sel.Reset(m_width, m_height);
        return true;
    }

    /*
 * ── drawing the lifted pixels ────────────────────────────────────────────
 *
 * Through the same projection the layers get, at the layer's own strength, so
 * the carried region looks like the piece of the layer it is. A host-backed
 * view takes it in one resample (paint_composite_raw_scaled); a device-backed
 * one has no bytes to blend into and gets the coarse per-sample DrawRect that
 * PaintLayer::BlitTo falls back to, for the same reason.
 */
    void draw_lift(ETCS::RID target, int32_t ox, int32_t oy, float zoom, const PaintLayer* layer)
    {
        const float z = (zoom <= 0.0f) ? 1.0f : zoom;
        const uint32_t bw = m_sel.width(), bh = m_sel.height();
        const int32_t lx = m_sel.x0 + m_sel.dx, ly = m_sel.y0 + m_sel.dy;
        const int32_t vx = ox + static_cast<int32_t>(std::lround(lx * z));
        const int32_t vy = oy + static_cast<int32_t>(std::lround(ly * z));
        const uint32_t dw = std::max(1u, static_cast<uint32_t>(bw * z + 0.5f));
        const uint32_t dh = std::max(1u, static_cast<uint32_t>(bh * z + 0.5f));
        const float alpha = layer->opacity() * layer->dim();

        if (Pixels_* dpx = ETCS::resolve_in_family<Pixels_>("Pixels", target))
        {
            paint_composite_raw_scaled(*dpx, m_sel.lift.data(), bw, bh, vx, vy, dw, dh, alpha);
            return;
        }
        Surface_* surface = ETCS::resolve_in_family<Surface_>("Surface", target);
        if (!surface) return;
        const uint32_t step = std::max(1u, static_cast<uint32_t>(4.0f / z));
        const uint32_t rw = std::max(1u, static_cast<uint32_t>(step * z + 1.0f));
        for (uint32_t y = 0; y < bh; y += step)
            for (uint32_t x = 0; x < bw; x += step)
            {
                const uint8_t* p = &m_sel.lift[(static_cast<size_t>(y) * bw + x) * 4];
                if (p[3] == 0) continue;
                surface->DrawRect(vx + static_cast<int32_t>(x * z), vy + static_cast<int32_t>(y * z),
                                  rw, rw, p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f,
                                  (p[3] / 255.0f) * alpha);
            }
    }

    /*
 * ── drawing the selection ────────────────────────────────────────────────
 *
 * THE OUTLINE OF THE MASK, on the view and never on a layer -- it is drawn
 * again from the mask every render, so it survives a pan, a zoom, a wheel
 * closing over it, and is gone the moment the selection is. That is what makes
 * it a preview rather than a mark, and it is why it lives here in the render
 * path rather than in the input machine, which is not the only thing that
 * re-renders (PaintPalette's zoom steps and PaintColorWheel::Close both do).
 *
 * WALKED AS EDGES, NOT AS PIXELS. A boundary pixel drawn as a z-by-z block is a
 * thick border at any zoom above 1; drawing the one-pixel EDGE of the block on
 * the side that faces out gives a hairline at every zoom, and at zoom 1 the two
 * are the same pixel. Outside the document counts as outside the mask, so a
 * selection that reaches the page edge is outlined along it rather than left
 * open.
 *
 * TWO TONES, ALTERNATING BY POSITION, because the outline has to read on white
 * paper and on black ink and on the dark layer beyond the page: whichever tone
 * vanishes against the ground, the other does not. Alternated by where the
 * segment falls on screen rather than by a counter along the path, so the
 * dashes are the same for a rectangle and a lasso and need no path to follow.
 * Not animated -- an outline that marches needs a clock, and this page has no
 * frame it could tick on.
 *
 * Shifted by the offset while lifted, since the region is wherever its pixels
 * currently hover.
 */
    void draw_selection(Surface_* view, int32_t ox, int32_t oy, float zoom)
    {
        if (m_sel.empty() || !view) return;
        const float z = (zoom <= 0.0f) ? 1.0f : zoom;
        const int32_t sx = m_sel.lifted() ? m_sel.dx : 0;
        const int32_t sy = m_sel.lifted() ? m_sel.dy : 0;
        auto vx_of = [&](int32_t x) { return ox + static_cast<int32_t>(std::lround((x + sx) * z)); };
        auto vy_of = [&](int32_t y) { return oy + static_cast<int32_t>(std::lround((y + sy) * z)); };

        for (int32_t y = m_sel.y0; y <= m_sel.y1; ++y)
        {
            const int32_t vy = vy_of(y), vh = std::max(1, vy_of(y + 1) - vy);
            for (int32_t x = m_sel.x0; x <= m_sel.x1; ++x)
            {
                if (!m_sel.at(x, y)) continue;
                const bool l = !m_sel.at(x - 1, y), r = !m_sel.at(x + 1, y);
                const bool t = !m_sel.at(x, y - 1), b = !m_sel.at(x, y + 1);
                if (!l && !r && !t && !b) continue;
                const int32_t vx = vx_of(x), vw = std::max(1, vx_of(x + 1) - vx);
                const bool light = (((vx + vy) >> 2) & 1) != 0;
                const float c = light ? 0.97f : 0.06f;
                if (l) view->DrawRect(vx,          vy,          1u, static_cast<uint32_t>(vh), c, c, c, 1.0f);
                if (r) view->DrawRect(vx + vw - 1, vy,          1u, static_cast<uint32_t>(vh), c, c, c, 1.0f);
                if (t) view->DrawRect(vx,          vy,          static_cast<uint32_t>(vw), 1u, c, c, c, 1.0f);
                if (b) view->DrawRect(vx,          vy + vh - 1, static_cast<uint32_t>(vw), 1u, c, c, c, 1.0f);
            }
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
    // The text boxes, their id counter, and the two view states the input machine
    // sets while the text tool is held. See PaintTextBox.
    std::vector<PaintTextBox> m_text;
    uint32_t  m_text_seq  = 0;
    uint32_t  m_text_sel  = 0;
    bool      m_text_show = false;
    bool      m_text_warned = false;
    ETCS::RID m_glyphs = 0;
    // The one selection, and the pixels it is carrying if any. See PaintSelection.
    uint64_t m_revision = 0;     // climbs on every change -- see Touch
    PaintSelection m_sel;
    PaintSelection m_clip;     // the clipboard: a selection's shape and bytes, kept

    struct HistoryEntry { ETCS::RID layer = 0; std::vector<uint8_t> bytes; };
    std::vector<HistoryEntry> m_undo;
    std::vector<HistoryEntry> m_redo;

    // What the paper is cleared to -- the page's convention, stated once here
    // for the two verbs that have to make new paper (Resize, New) rather than
    // read back from a layer that may have been painted on since.
    static constexpr float PAPER_CLEAR[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

    // The same ceiling a file is held to: a page bigger than this is an
    // allocation, not a picture (PAINT_IMAGE_MAX_SIDE).
    static bool extent_ok(uint32_t w, uint32_t h, const char* what)
    {
        if (w != 0 && h != 0 && w <= PAINT_IMAGE_MAX_SIDE && h <= PAINT_IMAGE_MAX_SIDE) return true;
        ETCS_LOG("PaintDocument", what << " " << w << "x" << h << " refused -- 1.."
                 << PAINT_IMAGE_MAX_SIDE << " on a side.");
        return false;
    }

    // One axis of the anchor: where the old extent's origin lands in the new
    // one so that the near edge (0), the middle (1) or the far edge (2) stays put.
    static int32_t anchor_shift(int a, uint32_t before, uint32_t after)
    {
        const int32_t d = static_cast<int32_t>(after) - static_cast<int32_t>(before);
        return (a <= 0) ? 0 : (a == 1) ? d / 2 : d;
    }

    /*
 * WHICH LAYER IS THE PAPER: the bottom of the stack, when nothing shows through
 * it. The bottom because that is where the page puts it and an import lands on
 * top (ImportImage), so it stays the bottom; opaque because a paper is a ground
 * -- the thing every other layer is seen against -- and a bottom layer with
 * holes in it is not that, it is ink over the dark layer beyond the page, and
 * treating it as paper would fill its holes in. Nothing else is remembered
 * about which layer was spawned as `paper`, so this is a question asked of the
 * pixels each time rather than a flag that could be stale.
 */
    static PaintLayer* ground_layer(const std::vector<PaintLayer*>& stack)
    {
        if (stack.empty() || !stack.front()->Opaque()) return nullptr;
        return stack.front();
    }

    // Move one entry from `from` to `to`, exchanging it with the layer's
    // current bytes so the step is its own inverse.
    bool step(std::vector<HistoryEntry>& from, std::vector<HistoryEntry>& to, const char* what)
    {
        if (from.empty()) { ETCS_LOG("PaintDocument", "nothing to " << what); return false; }
        HistoryEntry e = std::move(from.back());
        from.pop_back();
        // By RID, resolved now: the layer may have been reordered, renamed or
        // deleted since the snapshot, and only the last of those is a problem.
        PaintLayer* layer = nullptr;
        std::vector<PaintLayer*> layers;
        OrderedLayers(layers);
        for (PaintLayer* l : layers)
            if (l->getRID() == e.layer) { layer = l; break; }
        if (!layer)
        {
            ETCS_LOG("PaintDocument", what << ": that layer is gone -- entry dropped.");
            return false;
        }
        if (m_sel.lifted()) DropSelection();
        HistoryEntry now; now.layer = e.layer;
        if (!layer->SnapshotBytes(now.bytes) || !layer->RestoreBytes(e.bytes))
        {
            ETCS_LOG("PaintDocument", what << ": layer size changed since -- entry dropped.");
            return false;
        }
        to.push_back(std::move(now));
        Touch();
        return true;
    }
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

        /*
         * ONE CHANGE, AND IT IS THE FINISHED PICTURE.
         *
         * The clear below and every layer after it are writes toward one frame,
         * and the view is somebody else's raster -- a compositor, which copies a
         * frame out whenever it is told the raster changed. Told three times, it
         * copied three times, and one of those copies was the cleared view with no
         * document in it, which is the bare-paper flash on a smear.
         *
         * Batched, the statement is made once, here, when the picture is whole.
         * See ObservableBase::BeginBatch. It is not a lock: it does not stop the
         * frame edge from compositing while this runs.
         */
        etcs_observed_batch frame(view ? static_cast<ETCS::Entity*>(view) : nullptr);
        /*
         * A SECOND BATCH, FOR A SECOND RASTER. The ruler is drawn in the margin
         * OUTSIDE the pane, which is the frame's pixels rather than the pane's
         * (draw_edge_ruler), so its DrawRects mark the frame -- four band clears
         * and a few dozen ticks, each one of them a change on the frame's edges
         * unless they are gathered. Held here rather than inside the ruler so the
         * mark lands after the pane's own picture has been stated too, and so a
         * page with no frame bound holds nothing extra.
         */
        ETCS::Entity* frame_e = (m_ruler_frame != 0 && m_ruler_frame != m_target)
            ? static_cast<ETCS::Entity*>(
                  ETCS::resolve_in_family<Surface_>("Surface", m_ruler_frame))
            : nullptr;
        etcs_observed_batch margin(frame_e);

        // CLEARED FIRST, which a full-view document never needed. A projection
        // does not cover the surface, so without this the area outside the
        // document keeps whatever the last frame left there and panning smears.
        if (view) view->Clear(m_bg[0], m_bg[1], m_bg[2], m_bg[3]);
        m_document->RenderToSurface(m_target, m_pan_x, m_pan_y, m_zoom);
        // The pane's own edge, marked out. After the document because it is chrome
        // rather than part of the picture -- and, with a frame bound, not on the
        // picture's raster at all.
        draw_edge_ruler(view);

        // Redundant while the batches are held -- closing them makes these exact
        // statements -- and kept for the case where they are not: a surface that
        // never claimed Observable is not batched (etcs_observed_batch), and then
        // this is the only mark the picture gets. Marking twice costs one extra
        // edge update; marking zero times loses the frame.
        paint_mark_pixel_path(m_target);
        if (m_ruler_frame != 0 && m_ruler_frame != m_target)
            paint_mark_pixel_path(m_ruler_frame);
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
    // Whatever leaf claiming Glyphs labels the edge ruler. Its own rather than
    // the document's: the ruler is a property of the VIEW, and a surface may be
    // asked to draw one with no document attached yet.
    void BindGlyphs(ETCS::RID glyphs) { m_glyphs = glyphs; }

    // Off is a legitimate thing to want -- it is chrome, and a screenshot of the
    // picture should be able to not have it.
    void ShowEdgeRuler(bool on) { m_edge_ruler = on; }

    // THE RASTER THE EDGE RULER IS DRAWN ON: the frame the drawable pane sits
    // inside, not the pane. Bound rather than derived from the tree, because a
    // node's parent is a tree fact and where chrome belongs is a composition
    // choice -- and the frame is only the parent when a page nests them that way.
    // Unbound, the marks fall back inside the pane; see draw_edge_ruler.
    void BindRulerFrame(ETCS::RID frame) { m_ruler_frame = frame; }

    // The band's own two colours. Separate from SetBackground because they were
    // the same colour and that was the bug: with the band and the area beyond the
    // page in one shade there was nothing to say where the drawable region ended.
    void SetRulerBackground(float r, float g, float b, float a)
    { m_ruler_bg[0] = r; m_ruler_bg[1] = g; m_ruler_bg[2] = b; m_ruler_bg[3] = a; }
    void SetRulerInk(float r, float g, float b, float a)
    { m_ruler_ink[0] = r; m_ruler_ink[1] = g; m_ruler_ink[2] = b; m_ruler_ink[3] = a; }

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
 * ── THE DRAWABLE PANE'S EDGE, MARKED EVERY 100 PIXELS ────────────────────
 *
 * TWO THINGS IT IS FOR, and they are both about having a reference at all. The
 * size of the drawing area is otherwise only knowable by measuring the window
 * with something else, and the ruler tool reports distances with nothing on
 * screen to check them against -- a number with no scale beside it.
 *
 * OUTSIDE THE PANE, IN THE MARGIN AROUND IT, when the page gives it one
 * (BindRulerFrame). That is the correction: these used to be drawn INSIDE the
 * pane, along its inner edge, which put them on the picture -- over the paper
 * near the edges, and paintable, so the first stroke near a corner went through
 * the scale you were checking it against. A pane inset inside a frame has a band
 * that belongs to nobody who draws: the router hands presses to the PANE
 * (PaintInput::BindCanvas), so a point in the band is not the picture, and marks
 * there cannot be drawn on. The ticks point OUTWARD from the pane's edge, so the
 * pane's boundary is the zero line for both axes.
 *
 * WITH NO FRAME BOUND they fall back inside the pane, which is what a page with
 * no margin can have; it is not the intended arrangement and the fallback is
 * here so that such a page still gets a scale rather than nothing.
 *
 * IN DOCUMENT PIXELS, WHICH IS THE CORRECTION THAT MATTERS MOST HERE. These used
 * to count the pane, on the reasoning that the ruler describes the drawing AREA --
 * which is a coherent thing to measure and is not what anyone reads a ruler for.
 * At any zoom but 1 it meant the mark labelled 100 was not 100 of anything you
 * could draw, so every number on the edge was wrong. A ruler measures the thing
 * being measured: the marks now sit at round DOCUMENT coordinates projected
 * through the same pan and zoom the picture goes through, so a mark labelled 400
 * is against document x=400 at every zoom, and the scale slides with the paper
 * when you pan. The ruler TOOL and this edge now agree, which they never did.
 *
 * THE SPACING IS CHOSEN, NOT FIXED. Marks every 100 document pixels are 10 px
 * apart at 10% zoom (illegible) and 400 apart at 400% (useless). ruler_step walks
 * a 1-2-5 ladder until one interval is at least RULER_MIN_TICK_PX on screen, which
 * is what keeps the labels readable and the density roughly constant across the
 * whole zoom range.
 *
 * DRAWN, NOT SPAWNED. A script could place these as nodes, and the first resize,
 * pan or zoom would leave them wrong: the spacing, the values and the band's width
 * are all functions of state only the thing being drawn into holds. Four edges, so
 * a mark near a corner is reachable from either side of it.
 */
    // Marks closer together than this are a smear rather than a scale, and their
    // labels overlap. It is what picks the step out of the ladder below.
    static constexpr float RULER_MIN_TICK_PX = 72.0f;

    /*
 * A 1-2-5 LADDER, walked until one interval is far enough apart on screen.
 *
 * Fixed spacing cannot work once the marks are document coordinates: 100 doc px
 * is 10 screen px at 10% zoom and 400 at 400%. Stepping 1, 2, 5, 10, 20, 50, ...
 * keeps every label a round number a person can do arithmetic with -- which
 * stepping by, say, screen-pixels-divided-by-zoom would not.
 */
    static int32_t ruler_step(float zoom)
    {
        const float z = (zoom <= 0.0f) ? 1.0f : zoom;
        int32_t decade = 1;
        for (int guard = 0; guard < 12; ++guard)
        {
            for (int32_t m : { 1, 2, 5 })
                if (m * decade * z >= RULER_MIN_TICK_PX) return m * decade;
            decade *= 10;
        }
        return decade;
    }

    // The first multiple of `s` at or below `v`. Written out because integer
    // division truncates toward zero, so the obvious (v / s) * s steps the wrong
    // way once the pan puts document coordinates negative -- which it does the
    // moment you scroll off the top-left of the page.
    static int32_t floor_multiple(int32_t v, int32_t s)
    {
        if (s <= 0) return v;
        const int32_t q = (v >= 0) ? (v / s) : -(((-v) + s - 1) / s);
        return q * s;
    }

    void draw_edge_ruler(Surface_* pane_view)
    {
        if (!m_edge_ruler) return;

        // The pane's extent, always: it is what the numbers count, whichever
        // raster they end up on.
        WindowSize ps{ 0, 0 };
        if (Resizable_* v = ETCS::resolve_in_family<Resizable_>("Resizable", m_target))
            ps = v->GetSize();
        if (ps.width == 0 || ps.height == 0) return;
        const int32_t pw = static_cast<int32_t>(ps.width);
        const int32_t ph = static_cast<int32_t>(ps.height);

        // The raster to mark, and where the pane sits on it. Defaults are the
        // pane itself at its own origin -- the no-frame fallback above.
        Surface_* dst     = pane_view;
        ETCS::RID dst_rid = m_target;
        int32_t   ox = 0, oy = 0;      // pane origin in dst's space
        int32_t   dw = pw, dh = ph;    // dst's extent
        bool      outside = false;

        if (m_ruler_frame != 0 && m_ruler_frame != m_target)
        {
            Surface_*    frame = ETCS::resolve_in_family<Surface_>("Surface", m_ruler_frame);
            Drawable2D_* pane  = ETCS::resolve_in_family<Drawable2D_>("Drawable2D", m_target);
            WindowSize   fs{ 0, 0 };
            if (Resizable_* fv = ETCS::resolve_in_family<Resizable_>("Resizable", m_ruler_frame))
                fs = fv->GetSize();
            // Bounds(), so the offset is whatever the tree currently says rather
            // than a margin the script also had to tell this object about.
            if (frame && pane && fs.width != 0 && fs.height != 0)
            {
                const Rect2D pr = pane->Bounds();
                dst = frame; dst_rid = m_ruler_frame;
                ox = pr.x;   oy = pr.y;
                dw = static_cast<int32_t>(fs.width);
                dh = static_cast<int32_t>(fs.height);
                outside = true;
            }
        }
        if (!dst) return;

        const int32_t major = 10;    // tick length at a labelled mark
        const int32_t minor = 5;     // and at the halfway one
        const uint32_t label_px = 8;

        // The span of DOCUMENT the pane currently shows, on each axis. These are
        // what the marks are placed against, and they move with pan and zoom.
        const int32_t step = ruler_step(m_zoom);
        const int32_t dx0 = ViewToDocX(0),  dx1 = ViewToDocX(pw);
        const int32_t dy0 = ViewToDocY(0),  dy1 = ViewToDocY(ph);

        // ON THE BAND the marks are ink on wood, so they are the ink colour. In
        // the no-frame fallback they are drawn over the PICTURE instead -- white
        // paper in the middle, the surface's own dark layer around it -- and cream
        // would vanish on the paper, so that path keeps a mid blue that reads on
        // both. Two colours because there are two things to be legible against.
        const float r = outside ? m_ruler_ink[0] : 0.35f;
        const float g = outside ? m_ruler_ink[1] : 0.55f;
        const float b = outside ? m_ruler_ink[2] : 0.95f;
        const float a = outside ? m_ruler_ink[3] : 0.85f;

        ETCS::Held<Glyphs_> glyphs;
        if (m_glyphs != 0) glyphs = ETCS::resolve_held<Glyphs_>("Glyphs", m_glyphs);

        // How wide the band must be to hold a tick and the widest number beside
        // it. MEASURED, not computed from the size: the advance width is the
        // provider's, and a band sized by arithmetic here clips the labels on any
        // provider that disagrees. The widest number is now a document coordinate,
        // so panning far out makes it wider and the band follows.
        int32_t label_w = 0;
        if (glyphs)
        {
            const int32_t biggest = std::max(std::max(std::abs(dx0), std::abs(dx1)),
                                             std::max(std::abs(dy0), std::abs(dy1)));
            const TextExtent e = glyphs->MeasureText(
                std::to_string(((biggest / step) + 1) * step).c_str(), 0, label_px);
            label_w = static_cast<int32_t>(e.width);
        }
        // TWO WIDTHS, because the two axes need different things of the margin.
        // A label along the top or bottom edge is laid beside its tick and needs
        // only its HEIGHT of room; one along the left or right needs its WIDTH,
        // and a four-digit document coordinate is wider than it is tall. Sizing
        // both sides from the wider one gated the top labels out of a margin
        // that had ample room for them.
        const int32_t band_h = major + 4 + static_cast<int32_t>(label_px);
        const int32_t band_v = major + 4 + std::max(label_w, 12);
        const int32_t band   = std::max(band_h, band_v);   // what gets cleared

        // Per side, because a page is free to leave a margin on some edges and
        // not others -- the toolbar takes the bottom of this one's frame. Capped
        // at the band rather than filling the room, so the ruler is the same
        // width everywhere and does not swallow whatever else is out there.
        const int32_t bl = outside ? std::clamp(ox, 0, band) : 0;
        const int32_t bt = outside ? std::clamp(oy, 0, band) : 0;
        const int32_t br = outside ? std::clamp(dw - (ox + pw), 0, band) : 0;
        const int32_t bb = outside ? std::clamp(dh - (oy + ph), 0, band) : 0;

        // THE BAND IS THE RULER'S TO CLEAR. It is chrome on a raster somebody
        // else retains, so without this a resize leaves the previous extent's
        // marks sitting beside the new ones.
        if (outside)
        {
            // ITS OWN COLOUR, not the surface's background. Those were the same
            // shade, so the band and the empty area beyond the page ran together
            // and there was no telling where the drawable region stopped. Wood,
            // because that is what a ruler is, and because a warm brown is far
            // enough from both the dark layer outside the page and the white of
            // the page itself to be a boundary at a glance.
            const float* w = m_ruler_bg;
            const uint32_t span = static_cast<uint32_t>(bl + pw + br);
            if (bt > 0) dst->DrawRect(ox - bl, oy - bt, span, static_cast<uint32_t>(bt),
                                      w[0], w[1], w[2], w[3]);
            if (bb > 0) dst->DrawRect(ox - bl, oy + ph, span, static_cast<uint32_t>(bb),
                                      w[0], w[1], w[2], w[3]);
            if (bl > 0) dst->DrawRect(ox - bl, oy, static_cast<uint32_t>(bl),
                                      static_cast<uint32_t>(ph), w[0], w[1], w[2], w[3]);
            if (br > 0) dst->DrawRect(ox + pw, oy, static_cast<uint32_t>(br),
                                      static_cast<uint32_t>(ph), w[0], w[1], w[2], w[3]);
        }

        // THE EDGE, ONE PIXEL OF BLACK ALONG THE PANE'S BOUNDARY, on the band's
        // side of it. The boundary used to be whatever contrast the band's colour
        // happened to have against the paper and against the dark layer outside
        // the page, and against the second of those it had almost none. A line
        // that is black regardless of either colour is a boundary regardless of
        // either colour. Drawn on the band, not the pane: the pane's edge pixels
        // are the picture's, and this is chrome.
        if (outside)
        {
            const float k = 0.0f;
            if (bt > 0) dst->DrawRect(ox - (bl > 0 ? 1 : 0), oy - 1,
                                      static_cast<uint32_t>(pw + (bl > 0) + (br > 0)), 1, k, k, k, 1.0f);
            if (bb > 0) dst->DrawRect(ox - (bl > 0 ? 1 : 0), oy + ph,
                                      static_cast<uint32_t>(pw + (bl > 0) + (br > 0)), 1, k, k, k, 1.0f);
            if (bl > 0) dst->DrawRect(ox - 1, oy, 1, static_cast<uint32_t>(ph), k, k, k, 1.0f);
            if (br > 0) dst->DrawRect(ox + pw, oy, 1, static_cast<uint32_t>(ph), k, k, k, 1.0f);
        }

        // One tick length per side, so a side with a narrow margin gets a short
        // tick instead of one that runs off the frame.
        const int32_t tl = outside ? std::min(major, bl) : major;
        const int32_t tt = outside ? std::min(major, bt) : major;
        const int32_t tr = outside ? std::min(major, br) : major;
        const int32_t tb = outside ? std::min(major, bb) : major;

        // ── the horizontal axis, walked in DOCUMENT coordinates ──────────────
        //
        // The loop variable is the number on the label; where it lands is derived
        // from it through the same projection the picture uses, so the mark and
        // the pixel it names cannot drift apart.
        for (int32_t d = floor_multiple(dx0, step); d <= dx1; d += step)
        {
            const int32_t vx = DocToViewX(d);
            if (vx < 0 || vx >= pw) continue;
            const int32_t cx = ox + vx;
            const int32_t hvx = DocToViewX(d + step / 2);
            const bool    half = (step >= 2 && hvx >= 0 && hvx < pw);
            const int32_t hx = ox + hvx;
            if (outside)
            {
                // From one pixel out, so the edge line underneath stays whole.
                if (tt > 1) dst->DrawRect(cx, oy - tt, 1, static_cast<uint32_t>(tt - 1), r, g, b, a);
                if (tb > 1) dst->DrawRect(cx, oy + ph + 1, 1, static_cast<uint32_t>(tb - 1), r, g, b, a);
                if (half && bt >= minor)
                    dst->DrawRect(hx, oy - minor, 1, static_cast<uint32_t>(minor - 1), r, g, b, a);
                if (half && bb >= minor)
                    dst->DrawRect(hx, oy + ph + 1, 1, static_cast<uint32_t>(minor - 1), r, g, b, a);
                // No label at the origin: the corner carries the extent, and a
                // "0" under it is two numbers fighting for twelve pixels.
                if (glyphs && d != 0 && bt >= band_h)
                    glyphs->RasterizeText(dst_rid, std::to_string(d).c_str(), 0, label_px,
                                          cx + 3, oy - band_h + 2, r, g, b, a);
            }
            else
            {
                dst->DrawRect(cx, oy, 1, static_cast<uint32_t>(tt), r, g, b, a);
                dst->DrawRect(cx, oy + ph - tb, 1, static_cast<uint32_t>(tb), r, g, b, a);
                if (half)
                {
                    dst->DrawRect(hx, oy, 1, static_cast<uint32_t>(minor), r, g, b, a);
                    dst->DrawRect(hx, oy + ph - minor, 1, static_cast<uint32_t>(minor), r, g, b, a);
                }
                if (glyphs)
                    glyphs->RasterizeText(dst_rid, std::to_string(d).c_str(), 0, label_px,
                                          cx + 3, oy + major + 2, r, g, b, a);
            }
        }

        // ── the vertical axis, the same walk on the other coordinate ─────────
        for (int32_t d = floor_multiple(dy0, step); d <= dy1; d += step)
        {
            const int32_t vy = DocToViewY(d);
            if (vy < 0 || vy >= ph) continue;
            const int32_t cy = oy + vy;
            const int32_t hvy = DocToViewY(d + step / 2);
            const bool    half = (step >= 2 && hvy >= 0 && hvy < ph);
            const int32_t hy = oy + hvy;
            if (outside)
            {
                if (tl > 1) dst->DrawRect(ox - tl, cy, static_cast<uint32_t>(tl - 1), 1, r, g, b, a);
                if (tr > 1) dst->DrawRect(ox + pw + 1, cy, static_cast<uint32_t>(tr - 1), 1, r, g, b, a);
                if (half && bl >= minor)
                    dst->DrawRect(ox - minor, hy, static_cast<uint32_t>(minor - 1), 1, r, g, b, a);
                if (half && br >= minor)
                    dst->DrawRect(ox + pw + 1, hy, static_cast<uint32_t>(minor - 1), 1, r, g, b, a);
                if (glyphs && d != 0 && bl >= band_v)
                    glyphs->RasterizeText(dst_rid, std::to_string(d).c_str(), 0, label_px,
                                          ox - band_v + 2, cy + 3, r, g, b, a);
            }
            else
            {
                dst->DrawRect(ox, cy, static_cast<uint32_t>(tl), 1, r, g, b, a);
                dst->DrawRect(ox + pw - tr, cy, static_cast<uint32_t>(tr), 1, r, g, b, a);
                if (half)
                {
                    dst->DrawRect(ox, hy, static_cast<uint32_t>(minor), 1, r, g, b, a);
                    dst->DrawRect(ox + pw - minor, hy, static_cast<uint32_t>(minor), 1, r, g, b, a);
                }
                if (glyphs)
                    glyphs->RasterizeText(dst_rid, std::to_string(d).c_str(), 0, label_px,
                                          ox + major + 2, cy + 3, r, g, b, a);
            }
        }

        // The extent itself, where the two labelled axes meet -- the one number
        // somebody reading the edge actually wanted. In the corner of the margin
        // when there is one, so it is outside the picture like the rest.
        if (glyphs)
        {
            // THE DOCUMENT'S extent, not the pane's, because that is the unit the
            // two axes are now counting. The pane's size is a fact about the
            // window and is not what anybody reading a scale wants.
            const std::string ext = m_document
                ? std::to_string(m_document->width()) + "x" + std::to_string(m_document->height())
                : std::to_string(pw) + "x" + std::to_string(ph);
            if (outside && bt >= band_h && bl > 0)
                glyphs->RasterizeText(dst_rid, ext.c_str(), 0, label_px,
                                      ox - bl + 2, oy - band_h + 2, r, g, b, a);
            else if (!outside)
                glyphs->RasterizeText(dst_rid, ext.c_str(), 0, label_px,
                                      ox + major + 2, oy + major + 2, r, g, b, a);
        }
    }

public:

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
    // The edge ruler: on by default, because a view with no scale on it is the
    // state this was added to fix. See draw_edge_ruler.
    bool      m_edge_ruler = true;
    ETCS::RID m_glyphs     = 0;
    // The surface the ruler marks, when the pane is inset in a larger one.
    ETCS::RID m_ruler_frame = 0;
    // THE PAGE'S OWN HEADER COLOUR (#1b1c14, the --panel of index.html), so the
    // margin reads as part of the page's chrome rather than a third surface
    // between it and the paper. Near-black, so the marks are a warm off-white;
    // the boundary to the paper is not left to contrast at all -- see the
    // one-pixel edge in draw_edge_ruler.
    float m_ruler_bg[4]  = { 0.106f, 0.110f, 0.078f, 1.0f };
    float m_ruler_ink[4] = { 0.94f, 0.89f, 0.78f, 0.92f };
};

/*
 * ── PaintPages: the pages of this session, kept in a database ───────────────
 *
 * A HISTORY OF PAGES, AND ONE OF THEM IS THE PRESENT. The document is the
 * thing being painted on; this is the set of things it has been, each one a
 * row per layer in a database another module owns. "Current" is the slot the
 * present was last loaded from or saved to -- the "last changed" slot -- and
 * it is the one Save writes into. Nothing is current until something has been
 * saved or loaded, which is what m_current == 0 means.
 *
 * THROUGH THE FAMILY, BY RID. The database is reached as a Database_ through
 * the ontology (ontology/Database.h), never through DatabaseProvider's header:
 * this module says what it needs of a database -- a statement with bound
 * values, stepped a row at a time -- and any leaf claiming the family answers.
 * That is also why locality is nobody's business here: the boot script names a
 * file under /persist in the browser and a relative path on a desktop, and this
 * type does not know which.
 *
 * PAM IN THE BLOB, because it is already the pixels (the note above
 * PaintImage): a layer row is the same bytes ExportLayer writes to a file,
 * read back by the same parser, so nothing here has a format of its own and a
 * row pulled out of the database with any sqlite tool is a picture GIMP opens.
 *
 * QUICK SWITCH IS SAVE-THEN-LOAD, and the save is conditional on the document
 * having changed since it was loaded (PaintDocument::revision): switching away
 * from a page you only looked at must not touch its row, and switching away
 * from one you painted on must not lose the paint. The neighbour is the next
 * or previous id, wrapping -- by id and not by recency, because a page that
 * moved to the front every time it was saved would reorder the ring under the
 * keys that walk it.
 *
 * WHAT A PAGE DOES NOT CARRY, and says so when it matters: the text boxes.
 * They are strings, not pixels, and the export has the same gap for the same
 * reason (ExportImage); Save counts them so a page that lost its captions says
 * so in the log rather than in the next session.
 */
class PaintPages : public DeletableBase<PaintPages>
{
public:
    WIRE_TYPE_IDENTITY(PaintPages);

    PaintPages() = default;
    bool DeleteConcrete() override { return true; }

    /*
     * Bind the document and the database, and make sure the tables exist.
     * The schema is created here rather than by the script because the
     * script cannot know the columns this type reads -- a script that spells
     * them differently would be a page store that saves and never loads.
     */
    bool Create(ETCS::RID document, ETCS::RID database)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintDocument", document);
        m_document = raw ? static_cast<PaintDocument*>(raw->getTrueType()) : nullptr;
        m_db = database;
        m_current = 0;
        m_loaded_rev = m_document ? m_document->revision() : 0;
        if (!m_document) { ETCS_LOG("PaintPages", "Create: no PaintDocument at RID:" << document); return false; }

        ETCS::Held<Database_> db = ETCS::resolve_held<Database_>("Database", m_db);
        if (!db) { ETCS_LOG("PaintPages", "Create: no Database at RID:" << database << " -- Connect it first."); return false; }

        static const char* const schema[] = {
            "CREATE TABLE IF NOT EXISTS pages("
            " id INTEGER PRIMARY KEY, name TEXT NOT NULL,"
            " width INTEGER NOT NULL, height INTEGER NOT NULL, updated_at INTEGER NOT NULL)",
            // ord is the layer's POSITION in the stack, bottom first, not its
            // order key: two layers may share a key (PaintLayer says so) and a
            // stored page wants a dense, unique stacking back.
            "CREATE TABLE IF NOT EXISTS page_layers("
            " page_id INTEGER NOT NULL, ord INTEGER NOT NULL, name TEXT NOT NULL,"
            " visible INTEGER NOT NULL, opacity REAL NOT NULL, active INTEGER NOT NULL DEFAULT 0,"
            " pam BLOB NOT NULL)",
            "CREATE INDEX IF NOT EXISTS page_layers_by_page ON page_layers(page_id, ord)",
        };
        for (const char* sql : schema)
        {
            Stmt st(*db, sql);
            if (!st || st.step() < 0) { ETCS_LOG("PaintPages", "Create: schema failed -- see the database's log."); return false; }
        }
        m_loaded_rev = m_document->revision();   // what is on screen at boot is not yet a change
        ETCS_LOG("PaintPages", "bound to '" << m_document->name() << "', " << count_pages(*db) << " page(s) stored.");
        return true;
    }

    int64_t current() const { return m_current; }
    bool dirty() const { return m_document && m_document->revision() != m_loaded_rev; }

    /*
     * The present into its slot -- a new one if it has none. One transaction:
     * a page with half its layers is not a page, and the database's own
     * guard rolls back on any exit that is not the commit.
     */
    bool Save()
    {
        if (!m_document) return false;
        ETCS::Held<Database_> db = ETCS::resolve_held<Database_>("Database", m_db);
        if (!db) { ETCS_LOG("PaintPages", "Save: the database is gone."); return false; }

        std::vector<PaintLayer*> stack = m_document->layers();
        auto guard = db->Transaction();

        if (m_current == 0)
        {
            Stmt ins(*db, "INSERT INTO pages(name, width, height, updated_at) VALUES(?, ?, ?, ?)");
            if (!ins || !ins.bind(1, text(m_document->name())) || !ins.bind(2, DatabaseValue::integer(m_document->width()))
                || !ins.bind(3, DatabaseValue::integer(m_document->height())) || !ins.bind(4, DatabaseValue::integer(now()))
                || ins.step() < 0)
                return false;
            m_current = last_insert_id(*db);
            if (m_current == 0) return false;
            // A page made with no name is named after its row, the one thing
            // that is certainly unique and reads back as itself in List.
            if (m_document->name().empty()) m_document->SetName("Page " + std::to_string(m_current));
        }
        {
            Stmt up(*db, "UPDATE pages SET name = ?, width = ?, height = ?, updated_at = ? WHERE id = ?");
            if (!up || !up.bind(1, text(m_document->name())) || !up.bind(2, DatabaseValue::integer(m_document->width()))
                || !up.bind(3, DatabaseValue::integer(m_document->height())) || !up.bind(4, DatabaseValue::integer(now()))
                || !up.bind(5, DatabaseValue::integer(m_current)) || up.step() < 0)
                return false;
        }
        {
            Stmt del(*db, "DELETE FROM page_layers WHERE page_id = ?");
            if (!del || !del.bind(1, DatabaseValue::integer(m_current)) || del.step() < 0) return false;
        }
        size_t bytes = 0;
        for (size_t i = 0; i < stack.size(); ++i)
        {
            PaintLayer* l = stack[i];
            std::vector<uint8_t> pam;
            std::string why;
            if (!paint_pam_encode(l->PixelData(), l->width(), l->height(), pam, why))
            {
                ETCS_LOG("PaintPages", "Save: layer '" << l->name() << "': " << why << " -- page not saved.");
                return false;
            }
            Stmt ins(*db, "INSERT INTO page_layers(page_id, ord, name, visible, opacity, active, pam)"
                          " VALUES(?, ?, ?, ?, ?, ?, ?)");
            if (!ins || !ins.bind(1, DatabaseValue::integer(m_current)) || !ins.bind(2, DatabaseValue::integer((int64_t)i))
                || !ins.bind(3, text(l->name())) || !ins.bind(4, DatabaseValue::integer(l->visible() ? 1 : 0))
                || !ins.bind(5, DatabaseValue::real(l->opacity()))
                || !ins.bind(6, DatabaseValue::integer(l == m_document->activeLayer() ? 1 : 0))
                || !ins.bind(7, DatabaseValue::blob(pam.data(), pam.size())) || ins.step() < 0)
                return false;
            bytes += pam.size();
        }
        if (!guard.commit()) { ETCS_LOG("PaintPages", "Save: commit failed -- page not saved."); return false; }

        m_loaded_rev = m_document->revision();
        ETCS_LOG("PaintPages", "saved page " << m_current << " '" << m_document->name() << "' "
                 << m_document->width() << "x" << m_document->height() << ", " << stack.size()
                 << " layer(s), " << bytes << " bytes of PAM"
                 << (m_document->textBoxCount() ? "; " + std::to_string(m_document->textBoxCount())
                                                  + " text box(es) are not in it" : std::string()));
        persist();
        return true;
    }

    /*
 * THE FILE IS WRITTEN; NOW THE BROWSER HAS TO KEEP IT. sqlite has put the bytes
 * on the filesystem, and on the desktop that is the end of it. In the browser
 * the filesystem under /persist is a MEMFS view of IndexedDB that only reaches
 * the store when the page calls syncfs -- so the page is told, once per save,
 * through the same event bridge the menu uses. Nothing here knows what the page
 * does with it, and on the desktop this is a no-op by construction.
 */
    void persist()
    {
#if defined(__EMSCRIPTEN__)
        MAIN_THREAD_EM_ASM({
            window.dispatchEvent(new CustomEvent('etcs-persist', { detail: 'save' }));
        });
#endif
    }

    /*
     * A stored page onto the document, replacing what is there. Read first,
     * then destroy, then spawn: the read holds the database, and a
     * DestroyEvent must not be fired from inside a hold (PaintDocument::
     * DestroyLayers says why), so the rows are copied out and the hold
     * dropped before a single layer goes.
     */
    bool Load(int64_t id)
    {
        if (!m_document) return false;
        struct Row { int64_t ord; std::string name; bool visible; float opacity; bool active; std::vector<uint8_t> pam; };
        std::vector<Row> rows;
        std::string name;
        int64_t w = 0, h = 0;
        {
            ETCS::Held<Database_> db = ETCS::resolve_held<Database_>("Database", m_db);
            if (!db) { ETCS_LOG("PaintPages", "Load: the database is gone."); return false; }
            Stmt page(*db, "SELECT name, width, height FROM pages WHERE id = ?");
            if (!page || !page.bind(1, DatabaseValue::integer(id)) || page.step() != 1)
            {
                ETCS_LOG("PaintPages", "Load: no page " << id << " -- List says which exist.");
                return false;
            }
            name = str(page.col(0));
            w = page.col(1).i;
            h = page.col(2).i;
            Stmt lay(*db, "SELECT ord, name, visible, opacity, active, pam FROM page_layers WHERE page_id = ? ORDER BY ord");
            if (!lay || !lay.bind(1, DatabaseValue::integer(id))) return false;
            for (int rc = lay.step(); rc == 1; rc = lay.step())
            {
                Row r;
                r.ord     = lay.col(0).i;
                r.name    = str(lay.col(1));
                r.visible = lay.col(2).i != 0;
                r.opacity = static_cast<float>(lay.col(3).d);
                r.active  = lay.col(4).i != 0;
                const DatabaseValue blob = lay.col(5);
                if (blob.p && blob.n)
                    r.pam.assign(static_cast<const uint8_t*>(blob.p), static_cast<const uint8_t*>(blob.p) + blob.n);
                rows.push_back(std::move(r));
            }
        }

        m_document->DestroyLayers();
        m_document->Create(static_cast<uint32_t>(w), static_cast<uint32_t>(h), name);
        size_t spawned = 0;
        bool any_active = false;
        for (const Row& r : rows)
        {
            PaintImage img;
            std::string why;
            if (!paint_pam_parse(r.pam.data(), r.pam.size(), img, why))
            {
                ETCS_LOG("PaintPages", "Load: page " << id << " layer " << r.ord << " '" << r.name
                         << "': " << why << " -- skipped.");
                continue;
            }
            if (m_document->SpawnLayer(img, r.name, static_cast<int32_t>(r.ord), r.visible, r.opacity, r.active))
            {
                ++spawned;
                any_active = any_active || r.active;
            }
        }
        // The top is what is about to be worked on when the row did not say
        // -- the same choice an import makes.
        if (!any_active)
        {
            std::vector<PaintLayer*> stack = m_document->layers();
            if (!stack.empty()) m_document->SetActiveLayer(stack.back()->getRID());
        }
        m_current = id;
        m_loaded_rev = m_document->revision();
        ETCS_LOG("PaintPages", "loaded page " << id << " '" << name << "' " << w << "x" << h << ", "
                 << spawned << " of " << rows.size() << " layer(s).");
        repaint();
        return true;
    }

    /*
     * A fresh page becomes current: the present is kept if it has anything
     * unsaved, then the document is emptied and given the two layers the boot
     * script starts with -- paper under ink, ink active -- because a page
     * with nothing to paint on is not a page anyone can use. Saved at once,
     * so it has an id and Next/Prev can find it.
     */
    bool New()
    {
        if (!m_document) return false;
        flush();
        const uint32_t w = m_document->width() ? m_document->width() : 1024;
        const uint32_t h = m_document->height() ? m_document->height() : 768;
        m_document->DestroyLayers();
        m_document->Create(w, h, "");           // named after its row by Save

        PaintImage sheet;
        sheet.w = w; sheet.h = h;
        sheet.rgba.assign(static_cast<size_t>(w) * h * 4, 255);         // white paper
        if (!m_document->SpawnLayer(sheet, "Paper", 0, true, 1.0f, false)) return false;
        std::fill(sheet.rgba.begin(), sheet.rgba.end(), 0);            // transparent ink
        if (!m_document->SpawnLayer(sheet, "Ink", 1, true, 1.0f, true)) return false;

        m_current = 0;
        const bool ok = Save();
        repaint();
        return ok;
    }

    bool Next() { return step(+1); }
    bool Prev() { return step(-1); }

    // The surface to repaint when the document under it is swapped. The
    // document does not know who shows it; a load that leaves the old picture
    // on screen until the next stroke is a load that looks like it failed.
    void BindSurface(ETCS::RID surface) { m_surface = surface; }
    void repaint()
    {
        if (m_surface == 0) return;
        ETCS::Entity* raw = paint_resolve_tag("PaintSurface", m_surface);
        if (raw) static_cast<PaintSurface*>(raw->getTrueType())->Render();
    }

    void List()
    {
        ETCS::Held<Database_> db = ETCS::resolve_held<Database_>("Database", m_db);
        if (!db) { ETCS_LOG("PaintPages", "List: the database is gone."); return; }
        Stmt st(*db, "SELECT p.id, p.name, p.width, p.height, p.updated_at,"
                     " (SELECT COUNT(*) FROM page_layers l WHERE l.page_id = p.id)"
                     " FROM pages p ORDER BY p.id");
        if (!st) return;
        size_t n = 0;
        for (int rc = st.step(); rc == 1; rc = st.step(), ++n)
        {
            const int64_t id = st.col(0).i;
            ETCS_LOG("PaintPages", "  page " << id << " '" << str(st.col(1)) << "' "
                     << st.col(2).i << "x" << st.col(3).i << ", " << st.col(5).i
                     << " layer(s), updated " << st.col(4).i
                     << (id == m_current ? (dirty() ? "  <- current, changed" : "  <- current") : ""));
        }
        ETCS_LOG("PaintPages", n << " page(s)"
                 << (m_current == 0 ? "; the present is not in a slot yet" : ""));
    }

    // The slot goes; the picture on screen does not. Deleting the current page
    // leaves the present as an unsaved page, which the next Save gives a new
    // row -- the alternative, emptying the document, would make a wrong id
    // typed into Delete cost the work in front of you.
    bool Delete(int64_t id)
    {
        ETCS::Held<Database_> db = ETCS::resolve_held<Database_>("Database", m_db);
        if (!db) { ETCS_LOG("PaintPages", "Delete: the database is gone."); return false; }
        auto guard = db->Transaction();
        {
            Stmt del(*db, "DELETE FROM page_layers WHERE page_id = ?");
            if (!del || !del.bind(1, DatabaseValue::integer(id)) || del.step() < 0) return false;
        }
        {
            Stmt del(*db, "DELETE FROM pages WHERE id = ?");
            if (!del || !del.bind(1, DatabaseValue::integer(id)) || del.step() < 0) return false;
        }
        if (!guard.commit()) return false;
        if (id == m_current) m_current = 0;
        ETCS_LOG("PaintPages", "deleted page " << id
                 << (m_current == 0 ? "; the present is no longer in a slot" : ""));
        return true;
    }

private:
    PaintDocument* m_document = nullptr;
    ETCS::RID      m_db = 0;
    ETCS::RID      m_surface = 0;      // repainted after a load -- see BindSurface
    int64_t        m_current = 0;       // the slot the present came from or went to; 0 is none
    uint64_t       m_loaded_rev = 0;    // the document's revision at that moment -- see dirty()

    // One statement, finalized when the scope ends whatever the path out --
    // which is what makes every early `return false` above leave nothing open
    // for the transaction guard's rollback to trip over.
    struct Stmt
    {
        Database_& db;
        void*      s;
        Stmt(Database_& d, const char* sql) : db(d), s(d.Prepare(sql)) {}
        ~Stmt() { if (s) db.Finalize(s); }
        Stmt(const Stmt&) = delete;
        Stmt& operator=(const Stmt&) = delete;
        explicit operator bool() const { return s != nullptr; }
        bool bind(int i, const DatabaseValue& v) { return db.Bind(s, i, v); }
        int  step() { return db.Step(s); }
        DatabaseValue col(int c) { DatabaseValue v; db.Column(s, c, v); return v; }
    };

    static DatabaseValue text(const std::string& s) { return DatabaseValue::text(s.data(), s.size()); }
    static std::string str(const DatabaseValue& v)
    {
        return (v.kind == DatabaseValue::Text && v.p) ? std::string(static_cast<const char*>(v.p), v.n) : std::string();
    }
    static int64_t now()
    {
        using namespace std::chrono;
        return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
    }
    static int64_t last_insert_id(Database_& db)
    {
        Stmt st(db, "SELECT last_insert_rowid()");
        return (st && st.step() == 1) ? st.col(0).i : 0;
    }
    static int64_t count_pages(Database_& db)
    {
        Stmt st(db, "SELECT COUNT(*) FROM pages");
        return (st && st.step() == 1) ? st.col(0).i : 0;
    }

    // Keep the present if leaving it would lose something: a page that changed
    // since it was loaded -- or, for a present that was never given a slot,
    // since this store was bound to it. Not "has layers": every boot page has
    // layers, and a slot for each untouched boot would be a page a session
    // never asked for.
    void flush()
    {
        if (!m_document) return;
        if (dirty()) Save();
    }

    /*
 * THE PAGES AS A STRIP, NOT A RING. Forward from the last page is a NEW page,
 * which is how a second page comes to exist from the keyboard at all -- a
 * ring would have nowhere to put one and would need a separate verb the hand
 * on ctrl+PageDown does not know about. Back from the first page stays. The
 * present is flushed before either move (a changed page is saved; a page that
 * was never given a slot and has layers in it gets one), and loading the page
 * already on screen is refused: it would destroy and rebuild the same layers
 * for nothing and drop the undo history on the way.
 */
    bool step(int dir)
    {
        if (!m_document) return false;
        flush();
        std::vector<int64_t> ids;
        {
            ETCS::Held<Database_> db = ETCS::resolve_held<Database_>("Database", m_db);
            if (!db) { ETCS_LOG("PaintPages", "switch: the database is gone."); return false; }
            Stmt st(*db, "SELECT id FROM pages ORDER BY id");
            if (!st) return false;
            for (int rc = st.step(); rc == 1; rc = st.step()) ids.push_back(st.col(0).i);
        }
        size_t at = ids.size();
        for (size_t i = 0; i < ids.size(); ++i) if (ids[i] == m_current) { at = i; break; }
        int64_t target = 0;
        if (at == ids.size())                          // the present has no slot
            target = ids.empty() ? 0 : (dir > 0 ? ids.front() : ids.back());
        else if (dir > 0)
            target = (at + 1 < ids.size()) ? ids[at + 1] : 0;
        else
            target = (at > 0) ? ids[at - 1] : ids[at];
        if (target == 0)
        {
            ETCS_LOG("PaintPages", "switch: past the last page -- opening a new one.");
            return New();
        }
        if (target == m_current) { ETCS_LOG("PaintPages", "switch: page " << m_current << " is the first."); return false; }
        return Load(target);
    }
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
        Entry e{ Kind::Color, { r, g, b, a }, 0.0f, PaintToolKind::Brush };
        e.idle[0] = r; e.idle[1] = g; e.idle[2] = b; e.idle[3] = a;
        m_entries[node] = e;
    }

    void AddSize(ETCS::RID node, float radius)
    {
        if (node == 0 || radius <= 0.0f) return;
        Entry e{ Kind::Size, {}, radius, PaintToolKind::Brush };
        e.idle[0] = 0.16f; e.idle[1] = 0.16f; e.idle[2] = 0.20f; e.idle[3] = 1.0f;
        m_entries[node] = e;
    }

    void AddRadiusDelta(ETCS::RID node, float delta)
    {
        if (node == 0 || delta == 0.0f) return;
        Entry e{ Kind::RadiusDelta, {}, delta, PaintToolKind::Brush };
        e.idle[0] = 0.16f; e.idle[1] = 0.16f; e.idle[2] = 0.20f; e.idle[3] = 1.0f;
        m_entries[node] = e;
    }

    /*
 * A STEP IN THE TOOL'S OPACITY, in whole percent.
 *
 * This control used to step the motion-coalesce interval, which was a flicker
 * band-aid and is now pinned at every sample
 * (PAINT_MOTION_COALESCE_DEFAULT_MS). The geometry it occupies is the natural
 * home for the setting a painter actually reaches for next to size.
 */
    void AddAlphaDelta(ETCS::RID node, float delta_pct)
    {
        if (node == 0) return;
        Entry e{ Kind::AlphaDelta, {}, delta_pct, PaintToolKind::Brush };
        m_entries[node] = e;
    }

    // The wheel this palette opens. By RID and called by verb name, because
    // PaintColorWheel is declared below this type -- the same hop the wheel makes
    // to reach its router.
    void BindWheel(ETCS::RID wheel) { m_wheel = wheel; }

    /*
 * AN ARROW THAT OPENS THE WHEEL FOR ONE SWATCH.
 *
 * The wheel used to be reached by a single button and replaced whichever colour
 * entry happened to be selected last, which meant the user had to press a swatch
 * and then a separate control, and a mis-remembered selection silently
 * overwrote the wrong one. An arrow that BELONGS to a swatch removes both: the
 * target is named here, at layout time, and cannot be the wrong one.
 *
 * `slot` is a colour entry's node; anything else is refused at press time rather
 * than here, because the script is free to declare the arrow before the swatch.
 */
    void AddWheelArrow(ETCS::RID node, ETCS::RID slot)
    {
        if (node == 0) return;
        Entry e{ Kind::WheelArrow, {}, 0.0f, PaintToolKind::Brush };
        e.slot = slot;
        m_entries[node] = e;
    }

    /*
 * AN ARROW THAT STEPS A TOOL'S MODE -- the select tool's, which is the one
 * kind that has one (PaintSelectMode).
 *
 * The same shape as the wheel arrow, for the same reason: a control that
 * BELONGS to a slice names its slice at layout time, so what it changes cannot
 * be whatever happened to be pressed last. What differs is what pressing does.
 * A swatch's arrow opens a picker because a colour is continuous and a popup is
 * the only way to offer all of it; a mode is four words, and a popup for four
 * words is a wheel for a switch. So the arrow steps to the next one and the
 * readout beside it (SetModeReadout) says which -- one press per step, nothing
 * to dismiss.
 *
 * `slot` is a tool entry; anything else is refused at press time rather than
 * here, since the script is free to declare the arrow before the slice.
 */
    void AddModeArrow(ETCS::RID node, ETCS::RID slot)
    {
        if (node == 0) return;
        Entry e{ Kind::ModeArrow, {}, 0.0f, PaintToolKind::Brush };
        e.slot = slot;
        m_entries[node] = e;
    }

    // A third thing a node can mean, alongside a colour and a size: which TOOL
    // it selects. Same mapping, same Apply, so a tool button is a rectangle in
    // the toolbar script exactly as a swatch is.
    void AddTool(ETCS::RID node, const std::string& kind)
    {
        if (node == 0) return;
        Entry e{ Kind::Tool, {}, 0.0f, paint_tool_kind_from(kind) };
        e.idle[0] = 0.16f; e.idle[1] = 0.16f; e.idle[2] = 0.20f; e.idle[3] = 1.0f;
        m_entries[node] = e;
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
 * ── the general entry ────────────────────────────────────────────────────
 *
 * A NODE THAT CALLS A VERB. Every kind above is a verb this type knows how to
 * apply to a tool or a view; this one is any verb on any entity, named the way
 * Entity::call names it ("PaintCanvasMenu.StepWidth") with the argument text a
 * script would have written after it. It is what lets a whole panel be script:
 * the canvas menu's steppers, cells and buttons are rectangles bound here, and
 * the thing they drive never learns what a pointer is.
 *
 * The other kinds are not rewritten onto this one, because they are not calls
 * -- a swatch changes the tool AND records itself as the last colour, a delta
 * repeats while held, a tool change drops the selection. This is the kind for
 * the case with no such second half. The tag is read out of the action, since
 * the RID map is keyed by tag and an action already carries its own.
 */
    void AddCall(ETCS::RID node, ETCS::RID target, const std::string& action, const std::string& args)
    {
        if (node == 0 || target == 0 || action.find('.') == std::string::npos)
        {
            ETCS_LOG("PaintPalette", "AddCall on RID:" << node << " needs a target and a "
                     "'Tag.Action' -- got '" << action << "'.");
            return;
        }
        Entry e{ Kind::Call, {}, 0.0f, PaintToolKind::Brush };
        e.target = target;
        e.action = action;
        e.args   = args;
        m_entries[node] = e;
    }

    /*
 * A NODE THAT OPENS A PANE, and closes it when it is pressed again -- or when
 * anything else is pressed while it is open, which is what makes it a popup
 * rather than a panel. Open is what it is for the colour wheel (PaintColorWheel::
 * Open): membership of the routing set AND drawn, one fact, so a pane that is
 * merely invisible cannot go on swallowing clicks.
 *
 * The state lives here rather than on the pane's own type, because there is no
 * such type: the pane is a compositor from another module and its contents are
 * a script. One popup at a time, since the second would need to know about the
 * first to dismiss it. The router is needed to open anything, so BindRouter.
 */
    void AddPopup(ETCS::RID node, ETCS::RID pane, ETCS::RID input)
    {
        if (node == 0 || pane == 0 || input == 0) return;
        Entry e{ Kind::Popup, {}, 0.0f, PaintToolKind::Brush };
        e.slot   = pane;
        e.target = input;
        m_entries[node] = e;
    }

    // The router a popup joins while open. By RID and called by verb name, for
    // the reason PaintColorWheel gives: PaintRouter is declared below this type.
    void BindRouter(ETCS::RID router) { m_router = router; }

    /*
 * REPLACE A SWATCH'S COLOUR, which is what a colour wheel pick does to the slot
 * it was opened from.
 *
 * The mapping is updated here; the swatch's APPEARANCE is the script's, and is
 * driven by whoever picked -- PaintPalette holds no drawable and cannot restyle
 * one (see this type's header note). Returns false for a node that is not a
 * colour entry, so a caller can tell "there is no such swatch" from "done".
 */
    /*
 * REPLACE A SWATCH'S COLOUR -- the value it applies AND the way it looks.
 *
 * It used to set only the value, on the reasoning that a palette owns no
 * drawables and a script should restyle the swatch from lastSlot(). Nothing ever
 * did, so a picked colour applied to the tool while the swatch went on showing
 * the colour it had replaced -- and the reasoning was already untrue, since
 * Hover paints every entry through this same seam. A control that does not show
 * its own state is the bug, not the layering.
 *
 * `idle` moves with it, or the next hover-out would restore the old colour.
 */
    bool SetColorOf(ETCS::RID node, float r, float g, float b, float a)
    {
        auto it = m_entries.find(node);
        if (it == m_entries.end() || it->second.kind != Kind::Color) return false;
        Entry& e = it->second;
        e.rgba[0] = r; e.rgba[1] = g; e.rgba[2] = b; e.rgba[3] = a;
        e.idle[0] = r; e.idle[1] = g; e.idle[2] = b; e.idle[3] = a;
        if (m_hovering != node) set_node_fill(node, r, g, b, a);
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
    // The entry a picked node means: its own, or the nearest ancestor's -- a
    // label drawn on a swatch is picked as the label and means the swatch.
    auto resolve_entry(ETCS::RID node)
    {
        auto it = m_entries.find(node);
        if (it == m_entries.end() && node != 0)
        {
            ETCS::Held<Drawable2D_> node_e =
                ETCS::resolve_held<Drawable2D_>("Drawable2D", node);
            ETCS::Entity* e = node_e
                ? static_cast<ETCS::Entity*>(node_e.get()) : nullptr;
            for (; e; e = e->getParent())
            {
                it = m_entries.find(e->getRID());
                if (it != m_entries.end()) break;
            }
        }
        return it;
    }

    bool Apply(ETCS::RID node)
    {
        auto it = resolve_entry(node);

        /*
     * WHILE A POPUP IS OPEN, WHERE THE PRESS LANDED DECIDES FIRST. Outside the
     * pane -- the picture, the toolbar, the node that opened it -- it dismisses,
     * and is swallowed for the reason PaintInput swallows the press that closes
     * the wheel: a dismissal that also drew a dab would leave a mark for every
     * popup ever put away. Inside the pane it is the popup's: an entry there
     * applies as any entry does, and a press on the pane's own backing does
     * nothing at all rather than falling through to whatever the pane's input
     * would otherwise make of it.
     */
        if (m_popup_open != 0)
        {
            if (!node_within(node, m_popup_open)) { close_popup(); return true; }
            if (it == m_entries.end()) return true;
        }
        if (it == m_entries.end()) return false;

        const Entry& e = it->second;
        // The two kinds that apply to no tool, ahead of the tool check.
        if (e.kind == Kind::Popup) { open_popup(e.slot, e.target); return true; }
        if (e.kind == Kind::Call)  { call_entry(it->first, e); return true; }

        if (!m_tool)
        {
            ETCS_LOG("PaintPalette", "selection on RID:" << node
                     << " has no tool bound -- nothing to apply it to.");
            return true;   // still ours: it was a palette press, it just went nowhere
        }
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
        else if (e.kind == Kind::RadiusDelta)
        {
            const float next = std::max(1.0f, m_tool->brush().radius_px + e.radius);
            m_tool->SetRadius(next);
            set_node_text(m_radius_readout, std::to_string(static_cast<int>(next + 0.5f)));
            ETCS_LOG("PaintPalette", "radius delta " << e.radius << " -> " << next);
        }
        else if (e.kind == Kind::WheelArrow)
        {
            open_wheel_for(it->first, e.slot);
        }
        else if (e.kind == Kind::ModeArrow)
        {
            step_mode_for(it->first, e.slot);
        }
        else if (e.kind == Kind::AlphaDelta)
        {
            m_tool->AdjustAlphaPercent(static_cast<int32_t>(e.radius));
            set_node_text(m_alpha_readout, std::to_string(m_tool->alphaPercent()));
            ETCS_LOG("PaintPalette", "alpha -> " << m_tool->alphaPercent() << "%");
        }
        else if (e.kind == Kind::Tool)
        {
            m_tool->SetKind(paint_tool_kind_name(e.tool));
            ETCS_LOG("PaintPalette", "tool -> " << paint_tool_kind_name(e.tool));
            /*
             * LEAVING THE SELECT TOOL DROPS THE SELECTION. A region left standing
             * under the brush is a trap: it looks like it should mask the stroke
             * and it does not, and the next select-tool press inside it would
             * carry it. A carry in flight lands where it hovers (ClearSelection).
             * Switching TO select keeps whatever is there.
             */
            if (e.tool != PaintToolKind::Select && m_surface && m_surface->document()
                && m_surface->document()->hasSelection())
            {
                m_surface->document()->ClearSelection();
                m_surface->Render();
                ETCS_LOG("PaintPalette", "selection cleared -- tool changed");
            }
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


    /*
 * ── click and hold ───────────────────────────────────────────────────────
 *
 * A stepper pressed and held steps again, and again, and there is no clock to
 * step it on: while the button is held still no input arrives. The tick that
 * does exist is the FRAME -- a node that answers Animating() is visited every
 * composition (Drawable_::Animating, and TextLabel::BindFps is the precedent
 * for a node doing work in that visit). PaintRepeat is that node; it asks this
 * palette to Tick() once per frame while something is held.
 *
 * COUNTED IN FRAMES, NOT MILLISECONDS, for the same reason HeldCharge counts
 * accesses: the frame is the only clock, and a rate stated in its own units
 * cannot drift from it. The hold itself is a HeldCharge with a LARGE capacity,
 * spent one per frame: a real release ends it, a stated release (the mask)
 * ends it, and a pointer that left the page and never came back ends it after
 * the capacity -- which is the "much higher threshold" a mode wants, against
 * the four a stroke gets.
 */
    static constexpr uint32_t REPEAT_DELAY_FRAMES = 22;   // ~360ms before the first repeat
    static constexpr uint32_t REPEAT_EVERY_FRAMES = 3;    // then ~20 a second

    // Called after Apply() accepted a press: a delta entry becomes the held one.
    void Hold(ETCS::RID node)
    {
        auto it = resolve_entry(node);
        if (it == m_entries.end()) return;
        const Kind k = it->second.kind;
        if (k != Kind::RadiusDelta && k != Kind::AlphaDelta && k != Kind::Zoom) return;
        m_held = it->first;
        m_held_frames = 0;
        m_hold.Press();
    }
    void Release() { m_held = 0; m_hold.Release(); }
    bool holding() const { return m_held != 0; }

    // One frame of holding. The ACCESS that spends the charge -- see HeldCharge.
    void Tick()
    {
        if (m_held == 0) return;
        if (!m_hold.Held()) { ETCS_LOG("PaintPalette", "hold lapsed -- released."); Release(); return; }
        ++m_held_frames;
        if (m_held_frames < REPEAT_DELAY_FRAMES) return;
        if ((m_held_frames - REPEAT_DELAY_FRAMES) % REPEAT_EVERY_FRAMES == 0) Apply(m_held);
    }

    // Frames of unconfirmed holding a stepper survives. See HeldCharge.
    void SetHoldCapacity(uint16_t n) { m_hold.SetCapacity(n); }

    /*
 * RESOLVE FIRST, THEN COMPARE, THEN REPAINT -- the same order PaintLayerPanel's
 * Hover uses, and for a reason measured rather than guessed.
 *
 * This used to compare the RAW node against the last one and repaint before
 * resolving. A node that is not a palette entry at all -- the canvas pane, which
 * is what arrives on every pointer sample while somebody is painting -- misses
 * the early return, rewrites the fill of EVERY entry back to idle, and then
 * leaves without recording itself, so the next sample does it again. Every
 * SetFill marks the tree, so a stroke rewrote seven toolbar swatches per pointer
 * event and made the bar's compositor rebuild ~1,300 glyph rects each time, for
 * a picture that had not changed.
 *
 * What the guard has to compare is the resolved TARGET, because "the pointer is
 * over something that is not mine" and "the pointer is over nothing" are the
 * same fact to a palette, and neither is news twice.
 */
    void Hover(ETCS::RID node)
    {
        ETCS::RID target = 0;
        auto it = m_entries.find(node);
        if (it != m_entries.end()) { target = node; }
        else if (node != 0)
        {
            ETCS::Held<Drawable2D_> node_e =
                ETCS::resolve_held<Drawable2D_>("Drawable2D", node);
            ETCS::Entity* e = node_e
                ? static_cast<ETCS::Entity*>(node_e.get()) : nullptr;
            for (; e; e = e->getParent())
            {
                it = m_entries.find(e->getRID());
                if (it != m_entries.end()) { target = e->getRID(); break; }
            }
        }
        if (target == m_hovering) return;

        /*
     * THE GENERAL ENTRIES ARE NOT RESTYLED HERE, in either pass. Their look
     * belongs to whoever bound them -- the canvas menu paints its anchor cells
     * to show the chosen one (PaintCanvasMenu::show_anchor) -- and this type
     * never learned an idle colour for them, so a hover that wrote one back
     * would erase a state it did not know was there.
     */
        for (const auto& [rid, e] : m_entries)
            if (owns_look(e)) set_node_fill(rid, e.idle[0], e.idle[1], e.idle[2], e.idle[3]);
        m_hovering = target;
        // The caption of whatever was hovered goes away with the hover; the new
        // target's, if it has one, appears. See AddHoverLabel.
        show_hover_label(target);
        if (target == 0) return;

        node = target;
        const Entry& ref = it->second;
        const float t = (ref.kind == Kind::Color) ? 0.40f : 0.60f;
        for (const auto& [rid, e] : m_entries)
        {
            if (!owns_look(e)) continue;
            if (rid != node && !same_hover_group(ref, e)) continue;
            set_node_fill(rid,
                          e.idle[0] + (1.0f - e.idle[0]) * t,
                          e.idle[1] + (1.0f - e.idle[1]) * t,
                          e.idle[2] + (1.0f - e.idle[2]) * t,
                          1.0f);
        }
    }

    void SetRadiusReadout(ETCS::RID label) { m_radius_readout = label; }
    void SetAlphaReadout(ETCS::RID label) { m_alpha_readout = label; }

    /*
 * A CAPTION THAT APPEARS ON HOVER. Two bare numbers at the end of the bar were
 * a guess as to which was which, and a permanent caption under each is clutter
 * on a strip that is mostly read at a glance. So the caption is a node the
 * script places and hides, and hovering any entry of the stepper's GROUP -- the
 * +, the -, the number -- reveals it; leaving hides it again. Shown and hidden
 * through SetHidden rather than drawn here, for the reason this class draws
 * nothing: the tree already knows how.
 */
    void AddHoverLabel(ETCS::RID entry, ETCS::RID label)
    {
        if (entry == 0 || label == 0) return;
        m_hover_labels[entry] = label;
        set_node_hidden(label, true);
    }
    // The label that shows the select tool's mode -- written by the arrow, as
    // the radius readout is written by its +/- (see AddModeArrow).
    void SetModeReadout(ETCS::RID label) { m_mode_readout = label; }
    // The shape slice's, written by its arrow the same way. Bind it with
    // AddHoverLabel as well and it shows only while the slice is hovered.
    void SetShapeReadout(ETCS::RID label)
    {
        m_shape_readout = label;
        if (m_tool) set_node_text(label, paint_shape_mode_name(m_tool->shape()));
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
            else if (e.kind == Kind::Call)
                ETCS_LOG("PaintPalette", "  RID:" << rid << "  calls RID:" << e.target
                         << " " << e.action << "(" << e.args << ")");
            else if (e.kind == Kind::Popup)
                ETCS_LOG("PaintPalette", "  RID:" << rid << "  opens pane RID:" << e.slot
                         << (e.slot == m_popup_open ? " (open)" : ""));
            else
                ETCS_LOG("PaintPalette", "  RID:" << rid << "  radius " << e.radius);
        }
    }

    /*
 * HOW THIS TYPE REACHES A NODE IT DOES NOT OWN: by verb name over Entity::call,
 * since the node is another module's drawable (the header note says why there
 * is no other way). Public and static because they are the one seam for that,
 * and the canvas menu drives its readouts and its anchor cells through the same
 * three calls rather than a second copy of them.
 */
    static void set_node_fill(ETCS::RID node, float r, float g, float b, float a)
    {
        if (node == 0) return;
        ETCS::Held<Drawable2D_> node_e = ETCS::resolve_held<Drawable2D_>("Drawable2D", node);
        if (!node_e) return;
        ETCS::Entity* e = static_cast<ETCS::Entity*>(node_e.get());
        if (!e) return;
        const std::string tag = e->getSourceTag().toString();
        if (tag.find("PolygonDrawable2D") == std::string::npos) return;
        ETCS::Buffer action;
        action.write((tag + ".SetFill").c_str());
        ETCS::Buffer payload;
        payload.write((std::to_string(r) + " " + std::to_string(g) + " "
                     + std::to_string(b) + " " + std::to_string(a)).c_str());
        try { e->call(action, payload); } catch (...) {}
        // sheet_root is retained: mark the whole parent chain or the bar
        // never gets re-blitted into the sheet and fills look like a no-op.
        for (ETCS::Entity* n = e; n; n = n->getParent())
            etcs_mark_observed(n);
    }

    static void set_node_hidden(ETCS::RID node, bool hidden)
    {
        if (node == 0) return;
        ETCS::Held<Drawable2D_> node_e = ETCS::resolve_held<Drawable2D_>("Drawable2D", node);
        if (!node_e) return;
        ETCS::Entity* e = static_cast<ETCS::Entity*>(node_e.get());
        if (!e) return;
        ETCS::Buffer action;
        action.write((e->getSourceTag().toString() + ".SetHidden").c_str());
        ETCS::Buffer payload;
        payload.write(hidden ? "1" : "0");
        try { e->call(action, payload); } catch (...) {}
        for (ETCS::Entity* n = e; n; n = n->getParent())
            etcs_mark_observed(n);
    }

    static void set_node_text(ETCS::RID node, const std::string& text)
    {
        if (node == 0) return;
        ETCS::Held<Drawable2D_> node_e = ETCS::resolve_held<Drawable2D_>("Drawable2D", node);
        if (!node_e) return;
        ETCS::Entity* e = static_cast<ETCS::Entity*>(node_e.get());
        if (!e) return;
        ETCS::Buffer action;
        action.write((e->getSourceTag().toString() + ".SetText").c_str());
        ETCS::Buffer payload;
        payload.write(text.c_str());
        try { e->call(action, payload); } catch (...) {}
        for (ETCS::Entity* n = e; n; n = n->getParent())
            etcs_mark_observed(n);
    }

    // Whether `node` is `pane` or sits anywhere inside it -- the walk
    // resolve_entry makes, asked a different question.
    static bool node_within(ETCS::RID node, ETCS::RID pane)
    {
        if (node == 0 || pane == 0) return false;
        ETCS::Held<Drawable2D_> h = ETCS::resolve_held<Drawable2D_>("Drawable2D", node);
        for (ETCS::Entity* e = h ? static_cast<ETCS::Entity*>(h.get()) : nullptr; e; e = e->getParent())
            if (e->getRID() == pane) return true;
        return false;
    }

    ETCS::RID openPopup() const { return m_popup_open; }

private:
    enum class Kind : uint8_t { Color, Size, Tool, Zoom, RadiusDelta, AlphaDelta, WheelArrow, ModeArrow,
                                Call, Popup };
    struct Entry {
        Kind kind;
        float rgba[4];
        float radius;
        PaintToolKind tool;
        float idle[4] = { 0.16f, 0.16f, 0.20f, 1.0f };
        // The arrows only: the entry this arrow is a control FOR -- a colour
        // for a wheel arrow, a tool for a mode arrow. An arrow belongs to a
        // specific slice, not to "whatever was pressed last". A popup's PANE.
        ETCS::RID slot = 0;
        // A call's target and what to say to it; a popup's input (AddPopup).
        ETCS::RID   target = 0;
        std::string action;
        std::string args;
    };

    // Whether Hover may restyle this entry -- see the note in Hover.
    static bool owns_look(const Entry& e) { return e.kind != Kind::Call && e.kind != Kind::Popup; }

    static bool same_hover_group(const Entry& a, const Entry& b)
    {
        if (a.kind != b.kind) return false;
        if (a.kind == Kind::Tool) return a.tool == b.tool;
        if (a.kind == Kind::RadiusDelta || a.kind == Kind::AlphaDelta)
            return std::fabs(a.radius) == std::fabs(b.radius);
        if (a.kind == Kind::Size || a.kind == Kind::Zoom)
            return a.radius == b.radius;
        return false;
    }

    /*
 * Open the bound wheel directly above `arrow`, aimed at `slot`.
 *
 * The POSITION is computed here rather than laid out in the script because it is
 * a relation between two things the script places independently -- "above that
 * arrow" stays true when either moves, where a hard-coded pane position does
 * not. Root space, since that is where a floating pane is positioned; see
 * paint_root_origin for why that walk differs from a drawable's own.
 */
    void open_wheel_for(ETCS::RID arrow, ETCS::RID slot)
    {
        if (m_wheel == 0)
        {
            ETCS_LOG("PaintPalette", "wheel arrow on RID:" << arrow
                     << " pressed with no wheel bound -- BindWheel first.");
            return;
        }
        if (slot != 0)
        {
            auto sit = m_entries.find(slot);
            if (sit == m_entries.end() || sit->second.kind != Kind::Color)
            {
                ETCS_LOG("PaintPalette", "wheel arrow on RID:" << arrow
                         << " names RID:" << slot << ", which is not a colour entry.");
                return;
            }
        }
        ETCS::Entity* w = paint_resolve_tag("PaintColorWheel", m_wheel);
        if (!w) return;
        const Point2D at = paint_root_origin(arrow);
        ETCS::Buffer act; act.write("PaintColorWheel.OpenAt");
        ETCS::Buffer arg; arg.write((std::to_string(at.x) + ", " + std::to_string(at.y)
                                   + ", " + std::to_string(slot)).c_str());
        try { w->call(act, arg); } catch (...) {}
    }

    /*
 * Step the select tool's mode, for the slice `slot` names.
 *
 * TAKES THE TOOL UP AS WELL. A wheel pick ends with the picked colour in hand,
 * so the swatch's arrow effectively selects that swatch; an arrow that changed
 * the mode of a tool you were not holding would change a readout and nothing
 * you could see on the canvas, which reads as a control that does nothing.
 */
    void step_mode_for(ETCS::RID arrow, ETCS::RID slot)
    {
        auto sit = m_entries.find(slot);
        const bool is_select = sit != m_entries.end() && sit->second.kind == Kind::Tool
                               && sit->second.tool == PaintToolKind::Select;
        const bool is_shape  = sit != m_entries.end() && sit->second.kind == Kind::Tool
                               && sit->second.tool == PaintToolKind::Shape;
        if (!is_select && !is_shape)
        {
            ETCS_LOG("PaintPalette", "mode arrow on RID:" << arrow << " names RID:" << slot
                     << ", which is neither the select nor the shape slice.");
            return;
        }
        // Stepping the mode also takes the tool: an arrow pressed is a choice of
        // what to draw next, and asking for a second press to draw it is a
        // choice nobody meant to make.
        const PaintToolKind want = is_select ? PaintToolKind::Select : PaintToolKind::Shape;
        if (m_tool->kind() != want) m_tool->SetKind(paint_tool_kind_name(want));
        if (is_select)
        {
            m_tool->CycleMode();
            set_node_text(m_mode_readout, paint_select_mode_label(m_tool->mode()));
            ETCS_LOG("PaintPalette", "select mode -> " << paint_select_mode_name(m_tool->mode()));
        }
        else
        {
            m_tool->CycleShape();
            set_node_text(m_shape_readout, paint_shape_mode_name(m_tool->shape()));
            ETCS_LOG("PaintPalette", "shape -> " << paint_shape_mode_name(m_tool->shape()));
        }
    }

    /*
 * The call a general entry makes. The target is resolved NOW, by the tag its
 * action names, rather than held: it belongs to whoever spawned it and may be
 * gone by the time the button is pressed, and a stale pointer is the one
 * failure this mapping must not have.
 */
    void call_entry(ETCS::RID node, const Entry& e)
    {
        const std::string tag = e.action.substr(0, e.action.find('.'));
        ETCS::Entity* t = paint_resolve_tag(tag.c_str(), e.target);
        if (!t)
        {
            ETCS_LOG("PaintPalette", "RID:" << node << " calls " << e.action << " on RID:"
                     << e.target << ", which is not a live " << tag << ".");
            return;
        }
        ETCS::Buffer act; act.write(e.action.c_str());
        ETCS::Buffer arg; arg.write(e.args.c_str());
        try { t->call(act, arg); } catch (...) {}
    }

    /*
 * Open and close are the wheel's (PaintColorWheel::Open / Close): the pane
 * joins the router and is drawn, or leaves it and is hidden, as one change.
 * Closing re-renders the surface for the reason the wheel's BindSurface gives
 * -- the sheet is retained, and nothing but PaintSurface::Render puts back what
 * the pane was covering. The wheel is closed on the way in, because two popups
 * would each have to know about the other to dismiss it.
 */
    void open_popup(ETCS::RID pane, ETCS::RID input)
    {
        if (m_router == 0)
        {
            ETCS_LOG("PaintPalette", "popup pane RID:" << pane
                     << " pressed with no router bound -- BindRouter first.");
            return;
        }
        if (m_popup_open == pane) { close_popup(); return; }
        if (m_popup_open != 0) close_popup();
        if (ETCS::Entity* w = paint_resolve_tag("PaintColorWheel", m_wheel))
        {
            ETCS::Buffer act; act.write("PaintColorWheel.Close");
            ETCS::Buffer none;
            try { w->call(act, none); } catch (...) {}
        }
        if (ETCS::Entity* r = paint_resolve_tag("PaintRouter", m_router))
        {
            ETCS::Buffer act; act.write("PaintRouter.AddPane");
            ETCS::Buffer arg; arg.write((std::to_string(pane) + " " + std::to_string(input)).c_str());
            try { r->call(act, arg); } catch (...) {}
        }
        set_node_hidden(pane, false);
        m_popup_open = pane;
        ETCS_LOG("PaintPalette", "popup pane RID:" << pane << " opened.");
    }

    void close_popup()
    {
        if (m_popup_open == 0) return;
        if (ETCS::Entity* r = paint_resolve_tag("PaintRouter", m_router))
        {
            ETCS::Buffer act; act.write("PaintRouter.RemovePane");
            ETCS::Buffer arg; arg.write(std::to_string(m_popup_open).c_str());
            try { r->call(act, arg); } catch (...) {}
        }
        set_node_hidden(m_popup_open, true);
        ETCS_LOG("PaintPalette", "popup pane RID:" << m_popup_open << " closed.");
        m_popup_open = 0;
        if (m_surface) m_surface->Render();
    }

    // The label for a hovered target: its own, or the one bound to any entry in
    // the same group (a group shares one caption). 0 hides whatever is showing.
    void show_hover_label(ETCS::RID target)
    {
        ETCS::RID want = 0;
        if (target != 0)
        {
            auto tit = m_entries.find(target);
            for (const auto& [entry, label] : m_hover_labels)
            {
                if (entry == target) { want = label; break; }
                auto eit = m_entries.find(entry);
                if (tit != m_entries.end() && eit != m_entries.end()
                    && same_hover_group(tit->second, eit->second)) { want = label; break; }
            }
        }
        if (want == m_hover_label_shown) return;
        if (m_hover_label_shown != 0) set_node_hidden(m_hover_label_shown, true);
        if (want != 0) set_node_hidden(want, false);
        m_hover_label_shown = want;
    }

    std::unordered_map<ETCS::RID, Entry> m_entries;
    std::unordered_map<ETCS::RID, ETCS::RID> m_hover_labels;   // entry -> caption
    ETCS::RID m_hover_label_shown = 0;
    // The held stepper, if any -- see Hold/Tick. 600 frames is ten seconds at
    // sixty: a hold whose release was lost to the page ends on its own then.
    ETCS::RID  m_held = 0;
    uint32_t   m_held_frames = 0;
    HeldCharge m_hold{ 600 };
    PaintTool* m_tool = nullptr;
    PaintSurface* m_surface = nullptr;
    ETCS::RID m_hovering = 0;
    ETCS::RID m_radius_readout = 0;
    ETCS::RID m_alpha_readout = 0;
    ETCS::RID m_mode_readout = 0;
    ETCS::RID m_shape_readout = 0;
    ETCS::RID m_wheel = 0;
    // The popup that is open, if one is, and the router it is open IN. See
    // AddPopup: one at a time, and open means routed.
    ETCS::RID m_router = 0;
    ETCS::RID m_popup_open = 0;

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

    /*
 * The view to repaint when this popup MOVES or CLOSES -- and it needs one.
 *
 * The sheet is retained, so nothing clears it: whatever the wheel last covered
 * keeps showing the wheel. The picture underneath is only restored by the one
 * writer that clears before drawing, which is PaintSurface::Render. Without this
 * the wheel leaves a copy of itself wherever it has been, which reads as the
 * popup not closing at all.
 */
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
            // The old position is somebody else's picture again.
            if (m_surface) m_surface->Render();
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
        // See BindSurface: closing a popup over a retained sheet does not by
        // itself put back what it was covering.
        if (m_surface) m_surface->Render();
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
        ETCS::Held<Drawable2D_> h = ETCS::resolve_held<Drawable2D_>("Drawable2D", m_root);
        if (!h) return;
        ETCS::Entity* e = static_cast<ETCS::Entity*>(h.get());
        ETCS::Buffer act; act.write((e->getSourceTag().toString() + ".SetHidden").c_str());
        ETCS::Buffer arg; arg.write(hidden ? "1" : "0");
        try { e->call(act, arg); } catch (...) {}
    }

    // The pane is somebody else's drawable, so it moves by verb name like
    // everything else this type reaches across.
    void move_pane(int32_t x, int32_t y)
    {
        ETCS::Held<Drawable2D_> h = ETCS::resolve_held<Drawable2D_>("Drawable2D", m_root);
        if (!h) return;
        ETCS::Entity* e = static_cast<ETCS::Entity*>(h.get());
        ETCS::Buffer act; act.write((e->getSourceTag().toString() + ".SetPosition").c_str());
        ETCS::Buffer arg; arg.write((std::to_string(x) + ", " + std::to_string(y)).c_str());
        try { e->call(act, arg); } catch (...) {}
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


/*
 * ── PaintCanvasMenu ──────────────────────────────────────────────────────
 *
 * THE PENDING STATE OF THE SETTINGS MENU, and nothing else: a width, a height
 * and an anchor that the menu's steppers and cells edit, and that two buttons
 * hand to the document as a resize or a new canvas. Pending rather than live,
 * because a page re-stated on every stepper press would be nine resizes to get
 * from 1024 to 1600, each one clipping and shifting the picture.
 *
 * NO PIXELS AND NO NODES, on the toolbar's bargain (PaintPalette's header
 * note): the menu's look is a script -- PaintProvider/scripts/paint_menu.etcs
 * -- and every control in it is a rectangle the palette maps to a verb here
 * (PaintPalette::AddCall). What this type adds is the part that is about the
 * canvas: what the numbers mean, where they may go, and pushing them back onto
 * the readouts the script bound, as the surface pushes its zoom
 * (PaintSurface::push_zoom_label) -- so a stepper, a script and a Report all
 * leave the same number on screen.
 *
 * Steps of 64 because the page's own sizes are multiples of it and a finer step
 * is thirty presses to a common size; the range is the file reader's.
 */
class PaintCanvasMenu : public DeletableBase<PaintCanvasMenu>
{
public:
    WIRE_TYPE_IDENTITY(PaintCanvasMenu);

    PaintCanvasMenu() = default;
    bool DeleteConcrete() override { return true; }

    static constexpr int32_t STEP_PX = 64;
    static constexpr int32_t MIN_PX  = 64;
    static constexpr int32_t MAX_PX  = 8192;

    // Seeded from the document's extent, so the menu opens showing the page as
    // it is and "resize" with nothing stepped is a no-op rather than a surprise.
    bool Create(ETCS::RID document)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintDocument", document);
        if (!raw) { ETCS_LOG("PaintCanvasMenu", "Create: RID:" << document << " is not a PaintDocument."); return false; }
        m_document = static_cast<PaintDocument*>(raw->getTrueType());
        m_width  = std::clamp(static_cast<int32_t>(m_document->width()),  MIN_PX, MAX_PX);
        m_height = std::clamp(static_cast<int32_t>(m_document->height()), MIN_PX, MAX_PX);
        m_anchor = 4;
        this->addTag("active");
        return true;
    }

    // The view to re-render once the document changes under it -- the same
    // reason the palette and the wheel bind one.
    void BindSurface(ETCS::RID surface)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintSurface", surface);
        if (raw) m_surface = static_cast<PaintSurface*>(raw->getTrueType());
    }

    void StepWidth(int32_t delta)  { m_width  = clamp_px(m_width  + delta); push_readouts(); }
    void StepHeight(int32_t delta) { m_height = clamp_px(m_height + delta); push_readouts(); }

    void SetAnchor(int32_t anchor)
    {
        m_anchor = std::clamp(anchor, 0, 8);
        show_anchor();
    }

    void BindWidthReadout(ETCS::RID label)  { m_w_label = label; push_readouts(); }
    void BindHeightReadout(ETCS::RID label) { m_h_label = label; push_readouts(); }

    // The nine cells, by grid index; painted at once so the chosen one shows
    // from the moment the script binds it.
    void BindAnchorCell(int32_t index, ETCS::RID node)
    {
        if (index < 0 || index > 8 || node == 0) return;
        m_cells[index] = node;
        show_anchor();
    }

    void ApplyResize()
    {
        if (!m_document) { ETCS_LOG("PaintCanvasMenu", "resize: no document -- Create first."); return; }
        if (m_document->Resize(static_cast<uint32_t>(m_width), static_cast<uint32_t>(m_height), m_anchor)
            && m_surface)
            m_surface->Render();
    }

    void ApplyNew()
    {
        if (!m_document) { ETCS_LOG("PaintCanvasMenu", "new: no document -- Create first."); return; }
        if (m_document->New(static_cast<uint32_t>(m_width), static_cast<uint32_t>(m_height)) && m_surface)
            m_surface->Render();
    }

    /*
 * SAVE AND LOAD ARE THE PAGE'S, and this only says so. The file verbs take a
 * path (PaintDocument::ExportImage / ImportImage) and a path is something the
 * SUBSTRATE produces: in the browser it is the page's download and upload
 * controls that turn a path into a file the user can see, and there is no file
 * dialog on the desktop side yet. So under emscripten this raises a DOM event
 * the page listens for and answers with the same code its header buttons run;
 * natively it names the verb to type.
 */
    void Save() { page_event("save"); }
    void Load() { page_event("load"); }

    void Report() const
    {
        ETCS_LOG("PaintCanvasMenu", "pending " << m_width << "x" << m_height
                 << " anchor " << m_anchor << " (" << anchor_name(m_anchor) << ")"
                 << ", document " << (m_document ? std::to_string(m_document->width()) + "x"
                                                   + std::to_string(m_document->height())
                                                 : std::string("unbound")));
    }

    int32_t width()  const { return m_width; }
    int32_t height() const { return m_height; }
    int32_t anchor() const { return m_anchor; }

private:
    static int32_t clamp_px(int32_t v) { return std::clamp(v, MIN_PX, MAX_PX); }

    static const char* anchor_name(int32_t a)
    {
        static const char* names[9] = { "top-left", "top", "top-right", "left", "centre",
                                        "right", "bottom-left", "bottom", "bottom-right" };
        return (a >= 0 && a < 9) ? names[a] : "?";
    }

    // Through the palette's seam, since these are its nodes' verbs -- see
    // PaintPalette::set_node_text.
    void push_readouts()
    {
        PaintPalette::set_node_text(m_w_label, std::to_string(m_width));
        PaintPalette::set_node_text(m_h_label, std::to_string(m_height));
    }

    /*
 * THE CHOSEN CELL IS THE BRIGHT ONE. All nine are written every time rather
 * than the two that changed, because a cell may have been bound since the last
 * change and there is no cheaper way to know. The colours are here rather than
 * in the script because the script's initial fill is overwritten on bind
 * anyway: the palette's control colour for the rest, the page's gold for the
 * one that counts.
 */
    void show_anchor()
    {
        for (int32_t i = 0; i < 9; ++i)
        {
            if (m_cells[i] == 0) continue;
            if (i == m_anchor) PaintPalette::set_node_fill(m_cells[i], 0.79f, 0.71f, 0.35f, 1.0f);
            else               PaintPalette::set_node_fill(m_cells[i], 0.22f, 0.22f, 0.27f, 1.0f);
        }
    }

    /*
 * MAIN_THREAD_EM_ASM, not EM_ASM, for the reason etcs_web_shell_write gives:
 * this runs on the router's thread, a Worker with no window. It is proxied to
 * the page and waits for it, so the listener's own synchronous verb calls
 * (ExportImage, from the download path) run on the main thread while this
 * thread is parked, not against it.
 *
 * "load" clicks a file input from a proxied call. A browser opens a file dialog
 * only on transient user activation, and whether the activation of the press
 * that reached the gear's menu -- a canvas mousedown, then a Worker, then this
 * proxy -- is still standing when the page gets here is the browser's call, not
 * this code's. Untested here. The header's upload button is the same code from
 * a real click, and remains the way that always works.
 */
    void page_event(const char* what)
    {
#if defined(__EMSCRIPTEN__)
        MAIN_THREAD_EM_ASM({
            var what = UTF8ToString($0);
            window.dispatchEvent(new CustomEvent('etcs-menu', { detail: what }));
        }, what);
        ETCS_LOG("PaintCanvasMenu", what << " -> the page (etcs-menu event).");
#else
        ETCS_LOG("PaintCanvasMenu", what << ": no file dialog on this substrate. From the terminal: "
                 << (what[0] == 's' ? "doc.ExportImage(<path>)"
                                    : "doc.ImportImage(<path>), then canvas.Render()"));
#endif
    }

    PaintDocument* m_document = nullptr;
    PaintSurface*  m_surface  = nullptr;
    int32_t m_width  = 1024;
    int32_t m_height = 768;
    int32_t m_anchor = 4;
    ETCS::RID m_w_label = 0;
    ETCS::RID m_h_label = 0;
    ETCS::RID m_cells[9] = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };
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
        if (m_palette) m_palette->Hover(0);
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

        /*
     * INTO THE PANE'S OWN SPACE FIRST. A routed event carries a point in ROOT
     * space, and PickAt takes one in the space of the node it is called on --
     * the same point only when that node sits at the origin.
     *
     * Every pane that worked did sit at the origin (sheet_root is 0,0), so the
     * distinction cost nothing and stayed invisible until a pane that moves
     * needed it: the colour wheel's popup picked against unshifted coordinates
     * and answered for whatever happened to be under the wrong point.
     */
        const Point2D pane_at = paint_root_origin(m_root);
        const Point2D pane_pt{ ev.x - pane_at.x, ev.y - pane_at.y };

        const Pick2D hit = root->PickAt(pane_pt);
        if (!hit) return;                       // outside the tree entirely
        const ETCS::RID hit_rid = hit.node->getRID();

        /*
     * A PRESS ON THE PALETTE IS NOT A STROKE, and saying so HERE rather than
     * in the palette is deliberate: what a press means is a property of where
     * it landed, and this is the only place that knows both.
     */
        const bool is_press   = (ev.action == INPUT_DOWN || ev.action == INPUT_BUTTON_DOWN);
        const bool is_release  = (ev.action == INPUT_UP   || ev.action == INPUT_BUTTON_UP);

        if (m_palette && ev.action == INPUT_MOTION)
            m_palette->Hover(hit_rid);

        if (m_palette && is_press && m_palette->Apply(hit_rid))
        {
            m_on_palette = true;
            m_palette->Hold(hit_rid);           // a stepper keeps stepping until let go
            return;
        }
        if (is_release && m_on_palette)
        {
            m_on_palette = false;               // the press that ended was a selection
            m_palette->Release();
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
                /*
             * IN THE PANE'S SPACE, not the hit node's. A wheel measures a pick
             * from the centre and radius it was given (PaintColorWheel::Create),
             * both stated in the pane's coordinates -- and hit.local is local to
             * whatever was actually struck, which for a drawn wheel is one of
             * twelve wedges. Measured from a wedge's own origin, every point in
             * the disc reads as far outside it, so the pick always missed and the
             * press only ever dismissed. The wheel is drawn from the pane's
             * coordinates, so it has to be picked in them.
             */
                /*
             * THREE ANSWERS, NOT TWO. A press on the value strip changes the
             * disc's brightness and must leave the popup open -- closing on it
             * would make the third dimension of the colour unreachable in
             * practice, since choosing a shade takes two presses.
             */
                const PaintPick r = m_wheel->Pick(pane_pt.x, pane_pt.y);
                if (r != PaintPick::Adjusted) m_wheel->Close();
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

    /*
 * ── IS THE GESTURE STILL HELD ────────────────────────────────────────────
 *
 * Asked once per routed event while a button is believed down, and this is the
 * ACCESS that HeldCharge is built around (ontology/InputSource.h): asking costs a
 * unit, and the platform confirming the button tops it back up. When the charge
 * runs out, the release the platform never delivered is made HERE and sent down
 * the same path a real one takes -- flush, end, drop -- so nothing after this
 * point knows a release was ever missing.
 *
 * THREE ANSWERS FROM THE EVENT. A stated mask that names the button confirms. A
 * stated mask that does NOT is the release itself, and ends the hold at once. A
 * source that could not say (INPUT_BUTTONS_STATED clear) is where the charge
 * earns its keep: motion while believed held IS the drag and confirms, and only
 * the events that carry no evidence either way -- keys, a wheel notch -- spend
 * it. So a platform with no button report keeps the old behaviour almost
 * exactly, and one that reports gets the truth without a delay.
 */
    bool still_held(HeldCharge& charge, uint16_t button, const InputEvent& ev)
    {
        switch (input_button_state(ev, button))
        {
            case 1:  charge.Confirm(); return true;
            // A platform that REPORTED the button up is not evidence to weigh,
            // it is the release -- weighing it spent the charge one sample at a
            // time and each of those samples was a dab of phantom ink. The
            // charge is for the absence of information, not for contradicting it.
            case 0:  charge.Release(); return false;
            default:
                if (ev.action == INPUT_MOTION) { charge.Confirm(); return true; }
                return charge.Held();
        }
    }

    // A carry is a change from its lift, not its drop -- the lift is what
    // clears the source -- so the snapshot goes before it.
    bool lift_for_carry()
    {
        if (!m_document) return false;
        // Already floating -- a paste, or a carry a lost release left in the
        // air -- is carried as it is: the lift happened, and so did the
        // snapshot that went before it.
        if (m_document->selection().lifted()) return true;
        m_document->Remember();
        return m_document->LiftSelection();
    }

    // The release that should have arrived. Built from the last routed position
    // rather than the triggering event's, because the triggering event may be a
    // key or a scroll and carry no position at all.
    void release_lapsed(uint16_t button)
    {
        InputEvent up{};
        up.key     = button;
        up.action  = INPUT_BUTTON_UP;
        up.buttons = INPUT_BUTTONS_STATED;   // and nothing held: that is the point
        up.x = static_cast<int16_t>(m_pending_view_x);
        up.y = static_cast<int16_t>(m_pending_view_y);
        ETCS_LOG("PaintInput", "button " << button << " lapsed with no release -- releasing it.");
        HandleEvent(up);
    }

    // How many unconfirmed accesses a held button survives. Low here because a
    // phantom drag costs a line across the picture; something that reads a hold
    // as a MODE raises it. See HeldCharge.
    void SetHoldCapacity(uint16_t n) { m_pan_hold.SetCapacity(n); m_stroke_hold.SetCapacity(n); }

    void BindPages(ETCS::RID pages)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintPages", pages);
        if (!raw) return;
        m_pages = static_cast<PaintPages*>(raw->getTrueType());
    }


    void HandleEvent(const InputEvent& ev)
    {
        /*
     * ── lapsed holds first ────────────────────────────────────────────────
     *
     * Before any gesture branch reads the state it holds, that state is re-judged
     * against this event. The button's OWN press and release are exempt: they are
     * the statements, not questions about them.
     */
        const bool own_button_edge =
            (ev.action == INPUT_BUTTON_DOWN || ev.action == INPUT_BUTTON_UP);
        if (m_panning && !(own_button_edge && ev.key == m_pan_button)
            && !still_held(m_pan_hold, m_pan_button, ev))
            release_lapsed(m_pan_button);
        const bool primary_live = (m_tool && m_tool->active()) || m_text_drag != 0 || m_sel_carry;
        if (primary_live && !(own_button_edge && ev.key == m_stroke_button)
            && !still_held(m_stroke_hold, m_stroke_button, ev))
            release_lapsed(m_stroke_button);

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
        // Pan: right (GLFW 1) or middle (GLFW 2). Middle is the natural pan
        // button in the browser -- right opens the context menu there.
        if ((ev.action == INPUT_BUTTON_DOWN || ev.action == INPUT_BUTTON_UP)
            && paint_is_pan_button(ev.key))
        {
            if (ev.action == INPUT_BUTTON_DOWN)
            {
                m_panning  = true;
                m_pan_button = ev.key;
                m_pan_hold.Press();
                m_pan_from_x = ev.x;
                m_pan_from_y = ev.y;
                m_last_motion_ms = 0;    // allow an immediate first pan sample
                if (m_tool && m_tool->active()) m_tool->CancelStroke();
                repaint_view();          // drop any preview the cancelled gesture left
            }
            else if (ev.key == m_pan_button || m_pan_button == 0)
            {
                flush_coalesced_motion(); // apply the last pending pan delta
                m_panning = false;
                m_pan_button = 0;
                m_pan_hold.Release();
            }
            return;
        }
        if (ev.action == INPUT_MOTION && m_panning)
        {
            // Accumulate view-space samples; rebuild the sheet on the coalesce
            // interval only (see PAINT_MOTION_COALESCE_DEFAULT_MS).
            m_pending_view_x = ev.x;
            m_pending_view_y = ev.y;
            m_motion_pending = true;
            m_motion_kind = MotionKind::Pan;
            if (motion_coalesce_due())
                flush_coalesced_motion();
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

        // The document draws box outlines only while the text tool is held, and
        // nothing tells it when the tool changes -- the palette sets the kind
        // straight on the tool. So it is reconciled here, where every event
        // passes, rather than by a notification that does not exist.
        sync_text_affordance();

        if (ev.action == INPUT_MOTION && m_text_drag != 0 && m_document)
        {
            // Carrying a box. Not a stroke, not a preview, and deliberately not
            // coalesced: it is one field assignment and a re-composite, which is
            // what every other tool's sample already costs.
            PaintTextBox* b = m_document->FindTextBox(m_text_drag);
            if (!b) { m_text_drag = 0; return; }
            b->x = to_doc_x(ev.x) - m_text_grab_x;
            b->y = to_doc_y(ev.y) - m_text_grab_y;
            m_cursor_x = to_doc_x(ev.x);
            m_cursor_y = to_doc_y(ev.y);
            repaint_view();
            return;
        }

        if (ev.action == INPUT_MOTION && m_sel_carry)
        {
            /*
         * Carrying a selection. Not a stroke -- the tool holds no anchor --
         * but COALESCED where the text box's carry is not: every sample here
         * re-composites the document AND resamples the lifted pixels over it,
         * which is the full-view rebuild the anchored kinds are throttled for
         * (paint_tool_default_coalesce_ms), and the select tool is one of them.
         */
            m_cursor_x = to_doc_x(ev.x);
            m_cursor_y = to_doc_y(ev.y);
            m_pending_view_x = ev.x;
            m_pending_view_y = ev.y;
            m_motion_pending = true;
            m_motion_kind = MotionKind::Carry;
            if (motion_coalesce_due())
                flush_coalesced_motion();
            return;
        }

        if (ev.action == INPUT_MOTION)
        {
            // Position is still taken from the event (not integrated). Only the
            // expensive follow-up -- preview rebuild / segment stamp -- is
            // coalesced so a high-rate pointer does not clear+composite every
            // sample (see PAINT_MOTION_COALESCE_DEFAULT_MS).
            m_cursor_x = to_doc_x(ev.x);
            m_cursor_y = to_doc_y(ev.y);
            m_cursor_seen = true;
            m_pending_view_x = ev.x;
            m_pending_view_y = ev.y;

            if (m_tool && m_tool->active())
            {
                m_tool->MoveStroke(m_cursor_x, m_cursor_y);
                m_motion_pending = true;
                m_motion_kind = MotionKind::Stroke;
                if (motion_coalesce_due())
                    flush_coalesced_motion();
            }
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
                // ONE CHARGE FOR WHATEVER THIS PRESS STARTS -- a stroke, a text
                // box carry, a selection carry. They are different gestures with
                // one thing in common: the same release ends all of them, so the
                // same lapse must too.
                m_stroke_button = ev.key;
                m_stroke_hold.Press();
            }
            /*
         * A PRESS INSIDE AN EXISTING TEXT BOX SELECTS IT, and starts no gesture.
         *
         * Without this the only thing the text tool could do is make new boxes,
         * and every box already placed would be permanently out of reach -- which
         * is what "previously made text boxes should be selectable" rules out.
         * Checked before BeginStroke because a selection is not a stroke and must
         * not leave the tool holding an anchor it will later commit.
         */
            if (m_tool && m_cursor_seen && m_document
             && m_tool->kind() == PaintToolKind::Glyph)
            {
                const uint32_t hit_box = m_document->TextBoxAt(m_cursor_x, m_cursor_y);
                if (hit_box != 0)
                {
                    m_document->SelectTextBox(hit_box);
                    /*
                 * AND IT BECOMES DRAGGABLE FROM HERE. A press inside a box is
                 * ambiguous until the pointer either moves or does not, so it
                 * arms a move rather than choosing between the two: hold still
                 * and it was a selection, drag and the box follows. The grab
                 * OFFSET is what makes it follow rather than jump -- moving the
                 * box's corner to the pointer would teleport it by however far
                 * in you happened to press.
                 */
                    if (const PaintTextBox* b = m_document->FindTextBox(hit_box))
                    {
                        m_text_drag   = hit_box;
                        m_text_grab_x = m_cursor_x - b->x;
                        m_text_grab_y = m_cursor_y - b->y;
                    }
                    ETCS_LOG("PaintInput", "text box " << hit_box << " selected for typing");
                    repaint_view();
                    return;
                }
            }

            // A carry whose release landed on scenery never reached here
            // (RouteEvent drops those before HandleEvent) and is still in the
            // air. This press ends it where it hovers, so the pixels are back
            // in a layer before anything else is decided about them.
            if (m_sel_carry)
            {
                m_sel_carry = false;
                if (m_document) m_document->DropSelection();
            }

            /*
         * A PRESS INSIDE THE SELECTION PICKS IT UP, and starts no gesture --
         * the same shape as the press inside a text box above, and for the
         * same reason: what a press means depends on what is under it, and a
         * region you can see is a thing you expect to be able to take hold
         * of. The pixels leave the layer NOW (PaintDocument::LiftSelection),
         * so the hole is visible from the first sample of the drag rather
         * than appearing at the release. The grab offset is what makes the
         * region follow the hand instead of snapping its corner to it.
         *
         * A press OUTSIDE it falls through and begins a new region, which
         * replaces the old one at the first sample.
         */
            if (m_tool && m_cursor_seen && m_document
             && m_tool->kind() == PaintToolKind::Select
             && m_document->SelectionContains(m_cursor_x, m_cursor_y)
             && lift_for_carry())
            {
                m_sel_carry  = true;
                m_sel_grab_x = m_cursor_x;
                m_sel_grab_y = m_cursor_y;
                ETCS_LOG("PaintInput", "selection lifted at " << m_cursor_x << "," << m_cursor_y);
                repaint_view();
                return;
            }

            if (m_tool && m_cursor_seen)
            {
                // History, at the ONE point a continuous stroke starts writing.
                // An anchored kind writes at release and is remembered there
                // (commit_anchored); doing both here would spend a snapshot on a
                // preview that may commit nothing.
                if (m_document && !paint_kind_is_anchored(m_tool->kind()) && !paint_kind_is_placed(m_tool->kind()))
                    m_document->Remember();
                m_tool->BeginStroke(m_cursor_x, m_cursor_y);
                m_last_x = m_cursor_x;
                m_last_y = m_cursor_y;

                const PaintToolKind k = m_tool->kind();
                // A PLACED tool is finished here: one point is the whole
                // gesture, so it commits on the press and the release that
                // follows has nothing left to do.
                if (paint_kind_is_placed(k))       apply_placed(k, m_cursor_x, m_cursor_y);
                // The wand is the one select mode with nothing to drag: the
                // press names the colour, so the region is taken here and the
                // drag that may follow changes nothing (shape_selection).
                else if (k == PaintToolKind::Select && m_tool->mode() == PaintSelectMode::Wand
                      && m_document)
                {
                    m_document->SelectColor(m_cursor_x, m_cursor_y, m_tool->tolerance());
                    repaint_view();
                }
                // An anchored tool marks nothing yet -- the press is only where
                // the shape starts. Smudge likewise: it carries pixels from
                // somewhere, and on the first sample there is no somewhere.
                else if (!paint_kind_is_anchored(k) && k != PaintToolKind::Smudge)
                    apply_sample(m_cursor_x, m_cursor_y);
            }
        }
        else if (ev.action == INPUT_UP || ev.action == INPUT_BUTTON_UP)
        {
            if (ev.action == INPUT_BUTTON_UP) m_stroke_hold.Release();
            // Letting go of a carried box. Checked before the commit below,
            // because a carry never began a stroke and there is nothing to commit.
            if (m_text_drag != 0)
            {
                ETCS_LOG("PaintInput", "text box " << m_text_drag << " moved");
                m_text_drag = 0;
                return;
            }
            /*
         * Letting go of a carried selection: THE DROP, which is the one place
         * the select tool writes to the document. Drained first so the pixels
         * land where the last sample put them, not where the last flush did.
         */
            if (m_sel_carry)
            {
                flush_coalesced_motion();
                m_sel_carry = false;
                if (m_document && m_document->DropSelection())
                    ETCS_LOG("PaintInput", "selection dropped");
                repaint_view();
                return;
            }
            /*
         * THE COMMIT, and the only place an anchored tool ever writes to the
         * document. Read the anchor before EndStroke, which is what clears the
         * gesture.
         *
         * Ruler commits nothing by construction (paint_kind_commits), so
         * letting go of it simply removes the measurement -- which is what a
         * ruler you have finished with should do. Select commits nothing
         * either, but unlike the ruler it leaves something behind: the region,
         * stated one last time from the release position.
         */
            // Drain any motion held by the coalesce timer so the release
            // position is the one that was committed / previewed last.
            flush_coalesced_motion();
            if (m_tool)
            {
                const PaintToolKind k = m_tool->kind();
                if (m_tool->active() && paint_kind_is_anchored(k) && paint_kind_commits(k))
                {
                    if (m_document) m_document->Remember();
                    commit_anchored(k, m_tool->anchorX(), m_tool->anchorY(),
                                    m_cursor_x, m_cursor_y);
                }
                else if (m_tool->active() && k == PaintToolKind::Select)
                    end_selection(m_tool->anchorX(), m_tool->anchorY(),
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

    /*
 * A KEY, WHILE A BOX HOLDS THE FOCUS.
 *
 * Returns whether it was consumed, and that answer is the "steals all keyboard
 * input" part: a selected box swallows the keystroke, so nothing downstream sees
 * it, and with nothing selected every key falls through untouched.
 *
 * NO MODIFIERS YET, because the event does not carry any -- InputEvent reserves a
 * byte for them and the window layer does not fill it (ontology/InputSource.h).
 * So letters arrive unshifted and are taken as lowercase, which is the common
 * case for typing; capitals need that byte carried through first, and inventing a
 * shift state here from key-down/key-up pairs would be a second source of truth
 * for something the event should simply say.
 */
    bool KeyDown(uint16_t key)
    {
        if (!m_document) return false;
        // GLFW's codes. Named rather than compared as bare numbers, because a
        // bare 259 in a paint program is unreadable.
        constexpr uint16_t KEY_ESCAPE = 256, KEY_ENTER = 257, KEY_BACKSPACE = 259;
        constexpr uint16_t KEY_DELETE = 261;

        /*
     * ── chords ───────────────────────────────────────────────────────────
     *
     * Read before the text box gets the key, because ctrl+z in a box means the
     * document's undo, not a letter. GLFW's codes: the letter keys are their
     * ASCII uppercase. Each is glue to a document verb -- a script or the
     * terminal reaches the same thing by name.
     *
     *   ctrl+z          undo
     *   ctrl+shift+z    redo     (the two redo spellings, both honoured: the
     *   ctrl+y          redo      request named them as "the latter two")
     *   ctrl+c / x / v  copy, cut, paste the selection
     */
/*
     * ── chords ───────────────────────────────────────────────────────────
     *
     * Read before the text box gets the key, because ctrl+z in a box means the
     * document's undo, not a letter. GLFW's codes: the letter keys are their
     * ASCII uppercase. Each is glue to a document verb -- a script or the
     * terminal reaches the same thing by name.
     *
     *   ctrl+z          undo
     *   ctrl+shift+z    redo     (the two redo spellings, both honoured: the
     *   ctrl+y          redo      request named them as "the latter two")
     *   ctrl+c / x / v  copy, cut, paste the selection
     *   ctrl+PageUp     the previous page      (PaintPages::Prev)
     *   ctrl+PageDown   the next page          (PaintPages::Next)
     *
     * PageUp/PageDown and not ctrl+Tab, which is what a browser means by
     * "next tab" and what the window uses as its capture escape -- a chord
     * the substrate takes first is not a chord this program has.
     */
        constexpr uint16_t KEY_PAGE_UP = 266, KEY_PAGE_DOWN = 267;
        if (paint_modifiers().ctrl() && (key == KEY_PAGE_UP || key == KEY_PAGE_DOWN))
        {
            if (!m_pages) { ETCS_LOG("PaintInput", "chord ctrl+Page" << (key == KEY_PAGE_UP ? "Up" : "Down") << " -- no pages bound (BindPages)."); return true; }
            // A switch replaces the layers under everything: the carry, the
            // panel's rows and the view all describe the page that was.
            const bool did = (key == KEY_PAGE_UP) ? m_pages->Prev() : m_pages->Next();
            if (did)
            {
                m_sel_carry = false;
                if (m_panel) m_panel->Refresh();
                repaint_view();
            }
            return true;
        }

        if (paint_modifiers().ctrl())
        {
            constexpr uint16_t KEY_C = 67, KEY_V = 86, KEY_X = 88, KEY_Y = 89, KEY_Z = 90;
            bool did = false, handled = true;
            switch (key)
            {
            case KEY_Z: did = paint_modifiers().shift() ? m_document->Redo() : m_document->Undo(); break;
            case KEY_Y: did = m_document->Redo(); break;
            case KEY_C: did = m_document->CopySelection(); break;
            case KEY_X: did = m_document->CutSelection(); break;
            case KEY_V: did = m_document->PasteSelection(); break;
            default:    handled = false; break;
            }
            if (handled)
            {
                if (did) { m_sel_carry = false; repaint_view(); }
                ETCS_LOG("PaintInput", "chord ctrl+" << static_cast<char>(key)
                         << (did ? "" : " -- nothing to do"));
                return true;
            }
        }

        const uint32_t sel = m_document->selectedTextBox();
        if (sel == 0)
        {
            /*
         * ESCAPE DROPS THE SELECTION when no box has the keyboard. The only
         * other way out is to hold the select tool and click beside the
         * region, which is two things to know for what is one intention.
         * Only this key, and only while there is a selection, so with nothing
         * selected every key still reaches nobody. A carry in flight lands
         * where it hovers rather than being lost (PaintDocument::ClearSelection).
         */
            if (key == KEY_ESCAPE && m_document->hasSelection())
            {
                m_document->ClearSelection();
                m_sel_carry = false;
                ETCS_LOG("PaintInput", "selection cleared");
                repaint_view();
                return true;
            }
            if (key == KEY_DELETE && m_document->hasSelection())
            {
                m_sel_carry = false;
                ETCS_LOG("PaintInput", "selection " << (m_document->DeleteSelection() ? "deleted" : "not deleted"));
                repaint_view();
                return true;
            }
            return false;
        }
        PaintTextBox* b = m_document->FindTextBox(sel);
        if (!b) { m_document->SelectTextBox(0); return false; }

        if (key == KEY_ESCAPE || key == KEY_ENTER)
        {
            m_document->SelectTextBox(0);
            ETCS_LOG("PaintInput", "text box " << sel << " done: \"" << b->text << "\"");
            repaint_view();
            return true;
        }
        if (key == KEY_BACKSPACE)
        {
            if (!b->text.empty()) b->text.pop_back();
            repaint_view();
            return true;
        }
        if (key == KEY_DELETE)
        {
            // The box itself, since there is no caret to delete forward from.
            m_document->RemoveTextBox(sel);
            m_document->SelectTextBox(0);
            repaint_view();
            return true;
        }

        const char ch = paint_key_to_char(key);
        if (ch == 0) return true;          // consumed: a modifier or a function key
        b->text.push_back(ch);
        repaint_view();
        return true;
    }

    bool StrokeActive() const { return m_tool && m_tool->active(); }

    PaintDocument* document() const { return m_document; }
    PaintTool* tool() const { return m_tool; }
    PaintSurface* surface() const { return m_surface; }
    int32_t cursorX() const { return m_cursor_x; }
    int32_t cursorY() const { return m_cursor_y; }

private:

    enum class MotionKind : uint8_t { None, Pan, Stroke, Carry };

    static double now_ms()
    {
        using clock = ::std::chrono::steady_clock;
        return ::std::chrono::duration<double, ::std::milli>(
            clock::now().time_since_epoch()).count();
    }

    bool motion_coalesce_due()
    {
        double interval = (m_tool)
            ? m_tool->motionCoalesceMs()
            : PAINT_MOTION_COALESCE_DEFAULT_MS;
        const double t = now_ms();
        if (m_last_motion_ms <= 0.0 || (t - m_last_motion_ms) >= interval)
        {
            m_last_motion_ms = t;
            return true;
        }
        return false;
    }

    /*
     * Apply the latest pending pointer sample to the document / view. Called
     * when the coalesce interval elapses and again on button-up so the final
     * tip is never discarded. Between flushes, motion events only update the
     * pending coordinates.
     */
    void flush_coalesced_motion()
    {
        if (!m_motion_pending) return;
        m_motion_pending = false;

        if (m_motion_kind == MotionKind::Pan)
        {
            if (m_surface)
            {
                m_surface->PanBy(m_pending_view_x - m_pan_from_x,
                                 m_pending_view_y - m_pan_from_y);
                repaint_view();
            }
            m_pan_from_x = m_pending_view_x;
            m_pan_from_y = m_pending_view_y;
            m_cursor_seen = false;
            m_motion_kind = MotionKind::None;
            return;
        }

        if (m_motion_kind == MotionKind::Carry)
        {
            // Where the lift hovers: the pointer less where it took hold, in
            // document space, since the offset is applied to document pixels.
            if (m_document && m_sel_carry)
            {
                m_document->SetSelectionOffset(to_doc_x(m_pending_view_x) - m_sel_grab_x,
                                               to_doc_y(m_pending_view_y) - m_sel_grab_y);
                repaint_view();
            }
            m_motion_kind = MotionKind::None;
            return;
        }

        if (m_motion_kind == MotionKind::Stroke && m_tool && m_tool->active())
        {
            /*
             * WHICH OF THE THREE THINGS A DRAG DOES -- same split as before,
             * but once per coalesce window instead of once per OS sample.
             * apply_segment still interpolates from m_last_* to the tip, so a
             * fast stroke stays continuous rather than dotted.
             */
            const PaintToolKind k = m_tool->kind();
            if (k == PaintToolKind::Select)
                shape_selection(m_tool->anchorX(), m_tool->anchorY(),
                                m_cursor_x, m_cursor_y);
            else if (paint_kind_is_anchored(k))
                preview_anchored(k, m_tool->anchorX(), m_tool->anchorY(),
                                 m_cursor_x, m_cursor_y);
            else if (k == PaintToolKind::Smudge)
                apply_smudge(m_last_x, m_last_y, m_cursor_x, m_cursor_y);
            else if (!paint_kind_is_placed(k))
                apply_segment(m_last_x, m_last_y, m_cursor_x, m_cursor_y);
            m_last_x = m_cursor_x;
            m_last_y = m_cursor_y;
        }
        m_motion_kind = MotionKind::None;
    }

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
        case PaintToolKind::Shape:
            switch (m_tool->shape())
            {
            case PaintShapeMode::Rect:
                preview_line(view, vax, vay, vbx, vay, c, w);
                preview_line(view, vbx, vay, vbx, vby, c, w);
                preview_line(view, vbx, vby, vax, vby, c, w);
                preview_line(view, vax, vby, vax, vay, c, w);
                break;
            case PaintShapeMode::Ellipse:
                preview_ellipse(view, vax, vay, vbx, vby, c, w);
                break;
            default:
            {
                // In VIEW space, so the projected outline is the one committed.
                std::vector<std::pair<int32_t, int32_t>> v;
                paint_shape_vertices(m_tool->shape(), ax, ay, bx, by, v);
                for (size_t i = 0; i < v.size(); ++i)
                {
                    const auto& p0 = v[i]; const auto& p1 = v[(i + 1) % v.size()];
                    preview_line(view, m_surface->DocToViewX(p0.first), m_surface->DocToViewY(p0.second),
                                 m_surface->DocToViewX(p1.first), m_surface->DocToViewY(p1.second), c, w);
                }
                break;
            }
            }
            break;
        case PaintToolKind::Ruler:
            preview_ruler(view, vax, vay, vbx, vby, ax, ay, bx, by, z);
            break;
        case PaintToolKind::Glyph:
            preview_line(view, vax, vay, vbx, vay, c, w);
            preview_line(view, vbx, vay, vbx, vby, c, w);
            preview_line(view, vbx, vby, vax, vby, c, w);
            preview_line(view, vax, vby, vax, vay, c, w);
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
        case PaintToolKind::Shape:
            switch (m_tool->shape())
            {
            case PaintShapeMode::Rect:    layer->DrawRectOutline(ax, ay, bx, by, brush); break;
            case PaintShapeMode::Ellipse: layer->DrawEllipseOutline(ax, ay, bx, by, brush); break;
            default:
            {
                std::vector<std::pair<int32_t, int32_t>> v;
                paint_shape_vertices(m_tool->shape(), ax, ay, bx, by, v);
                for (size_t i = 0; i < v.size(); ++i)
                {
                    const auto& p0 = v[i]; const auto& p1 = v[(i + 1) % v.size()];
                    layer->StrokeLine(p0.first, p0.second, p1.first, p1.second, brush);
                }
                break;
            }
            }
            break;
        // A glyph commit places a BOX, not pixels, and a box is the document's
        // rather than the layer's -- see PaintTextBox and place_text_box.
        case PaintToolKind::Glyph:   place_text_box(ax, ay, bx, by); break;
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
            if (m_document) m_document->Remember();
            const size_t n = layer->FloodFill(x, y, m_tool->brush().color,
                                              m_tool->tolerance());
            ETCS_LOG("PaintInput", "fill at " << x << "," << y << " -> " << n << " px");
        }
        else if (kind == PaintToolKind::Eyedrop)
        {
            /*
         * THE COLOUR UNDER THE PRESS, as seen -- every visible layer composited
         * at that pixel, not the active layer's byte, because what the user
         * pointed at is the picture. Through the wheel when there is one, so the
         * swatch slot the wheel was aimed at takes the colour exactly as a pick
         * on the disc would; otherwise straight onto the tool. Then the kind that
         * was in use comes back: an eyedropper is a one-press detour, not a
         * mode you have to find your way out of.
         */
            float c[4] = { 0, 0, 0, 0 };
            if (m_document->SampleAt(x, y, c))
            {
                if (m_wheel) m_wheel->Apply(c[0], c[1], c[2], c[3] > 0.0f ? 1.0f : 0.0f);
                else         m_tool->SetColor(c[0], c[1], c[2], 1.0f);
                ETCS_LOG("PaintInput", "eyedrop at " << x << "," << y << " -> "
                         << c[0] << ", " << c[1] << ", " << c[2] << " (a " << c[3] << ")");
            }
            if (m_wheel) m_wheel->EndPick();
            else         m_tool->SetKind("brush");
        }
        repaint_view();
    }

    /*
 * ── PLACING AND EDITING A TEXT BOX ───────────────────────────────────────
 *
 * The glyph tool used to prompt on the terminal for a string and rasterise it
 * into the active layer. Two things were wrong with that, and they were the same
 * thing twice: the text stopped being text the instant it landed, and the typing
 * happened somewhere other than where the text was going.
 *
 * Now a drag places a BOX (PaintTextBox, held by the document) and the box takes
 * the keyboard. A press inside an existing box selects it instead of starting a
 * new one, so every box ever placed stays reachable as long as the tool is held.
 *
 * A TINY DRAG IS A CLICK, and a click on nothing is a deselect. Without that
 * there is no way to stop typing into a box except by choosing another tool.
 */
    /*
 * Outlines on while the text tool is held, off otherwise -- and a box stops being
 * selected when the tool is put down, because a selection that survives the tool
 * would swallow keystrokes with nothing on screen to explain why.
 */
    void sync_text_affordance()
    {
        if (!m_document) return;
        const bool want = (m_tool && m_tool->kind() == PaintToolKind::Glyph);
        if (want == m_text_affordance) return;
        m_text_affordance = want;
        m_document->ShowTextBoxes(want);
        if (!want) m_document->SelectTextBox(0);
        repaint_view();
    }

    void place_text_box(int32_t ax, int32_t ay, int32_t bx, int32_t by)
    {
        if (!m_document) return;
        const int32_t x0 = std::min(ax, bx), y0 = std::min(ay, by);
        const int32_t w  = std::abs(bx - ax), h = std::abs(by - ay);

        if (w < 6 || h < 6)
        {
            // Too small to be a box: treat it as a click, which selects whatever
            // is under it and otherwise clears the selection.
            const uint32_t hit = m_document->TextBoxAt(ax, ay);
            m_document->SelectTextBox(hit);
            if (hit) ETCS_LOG("PaintInput", "text box " << hit << " selected for typing");
            else     ETCS_LOG("PaintInput", "text selection cleared");
            repaint_view();
            return;
        }

        /*
     * EMPTY, AND IN THE TOOL'S COLOUR.
     *
     * Empty because the next thing that happens is typing: a box that arrives
     * holding the tool's default string means the first keystroke appends to
     * "Text" instead of starting the caption, and the user has to clear it before
     * saying anything. PaintTool::SetText is still how a script can put a string
     * in one (doc.SetTextBoxText), which is where a default belongs -- with the
     * caller that wants it, not with every box.
     */
        const PaintColor c = m_tool ? m_tool->brush().color
                                    : PaintColor{ 0.08f, 0.08f, 0.10f, 1.0f };
        m_document->Remember();
        const uint32_t id = m_document->AddTextBoxColoured(x0, y0, w, h,
                                                           c.r, c.g, c.b, c.a);
        m_document->SelectTextBox(id);
        repaint_view();
    }

    /*
 * ── DEFINING A SELECTION ─────────────────────────────────────────────────
 *
 * THE PREVIEW IS THE SELECTION. The shape tools draw a stand-in on the view
 * while dragging and commit the real thing on release; a region has no pixels
 * to commit, so there is nothing for a stand-in to stand in for. Every flush
 * simply re-states the region from the anchor and the tip, and the render path
 * draws it (PaintDocument::draw_selection). What you see while dragging is
 * therefore what you will have when you let go, by construction rather than by
 * two drawings agreeing -- and it costs a mask fill per flush, which at ten a
 * second is nothing against the re-composite that follows it.
 *
 * The wand is stated once, at the press, because the drag carries no
 * information it wants; re-flooding the same seed ten times a second would
 * give the same region back each time.
 */
    void shape_selection(int32_t ax, int32_t ay, int32_t bx, int32_t by)
    {
        if (!m_document || !m_tool) return;
        switch (m_tool->mode())
        {
        case PaintSelectMode::Rect:    m_document->SelectRect(ax, ay, bx, by);    break;
        case PaintSelectMode::Ellipse: m_document->SelectEllipse(ax, ay, bx, by); break;
        case PaintSelectMode::Lasso:   m_document->SelectPath(m_tool->points());  break;
        case PaintSelectMode::Wand:    return;
        }
        repaint_view();
    }

    /*
 * A CLICK IS A DESELECT, as it is for the text tool: a drag that went nowhere
 * names no region, and with nothing named the region that was there goes. It
 * is the only way out of a selection with the pointer, so it has to exist.
 *
 * Measured over the whole path rather than anchor-to-release, because a lasso
 * ends where it began -- that is what closing a loop means -- and would read
 * as a click every time.
 */
    void end_selection(int32_t ax, int32_t ay, int32_t bx, int32_t by)
    {
        if (!m_document || !m_tool) return;
        if (m_tool->mode() == PaintSelectMode::Wand) return;   // taken at the press

        int32_t lx = ax, rx = ax, ty = ay, by2 = ay;
        for (const PaintStrokePoint& p : m_tool->points())
        {
            lx = std::min(lx, p.x); rx = std::max(rx, p.x);
            ty = std::min(ty, p.y); by2 = std::max(by2, p.y);
        }
        if (rx - lx < 2 && by2 - ty < 2)
        {
            m_document->ClearSelection();
            ETCS_LOG("PaintInput", "selection cleared");
            return;
        }
        shape_selection(ax, ay, bx, by);
        const PaintSelection& s = m_document->selection();
        ETCS_LOG("PaintInput", "selected " << s.count << " px ("
                 << paint_select_mode_name(m_tool->mode()) << ")");
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
    // Whether the document is currently showing its text-box outlines, so the
    // reconcile above is a comparison rather than a call per event.
    bool m_text_affordance = false;
    // The box being carried, and where inside it the pointer took hold. 0 is
    // "nothing is being carried" -- see the press branch.
    uint32_t m_text_drag   = 0;
    int32_t  m_text_grab_x = 0;
    int32_t  m_text_grab_y = 0;
    // The selection being carried, and where inside it the pointer took hold
    // -- the text box's carry, for a region. See the press branch.
    bool     m_sel_carry  = false;
    int32_t  m_sel_grab_x = 0;
    int32_t  m_sel_grab_y = 0;
    // Whatever leaf claiming Glyphs the script bound -- RenderProvider's
    // TextLabel today. By RID and resolved per use, since it is another
    // module's entity (see place_glyphs).
    ETCS::RID m_glyphs = 0;
    PaintColorWheel* m_wheel = nullptr;
    bool    m_is_wheel_pane = false;
    bool    m_on_panel = false;
    // The right-button pan. Tracked in VIEW pixels because that is the frame a
    // drag is felt in -- see the motion branch.
    bool     m_panning = false;
    uint16_t m_pan_button = 0;
    // The two holds this input can be in, each re-judged per event while live.
    // See still_held. Capacity is deliberately small: see SetHoldCapacity.
    PaintPages*      m_pages = nullptr;   // quick switch -- see BindPages
    HeldCharge m_pan_hold{ 4 };
    HeldCharge m_stroke_hold{ 4 };
    uint16_t   m_stroke_button = 0;
    int32_t  m_pan_from_x = 0;
    int32_t  m_pan_from_y = 0;   // a press landed on the panel; its release is not a stroke

    // Motion coalesce -- interval from PaintTool::motionCoalesceMs().
    bool       m_motion_pending = false;
    MotionKind m_motion_kind    = MotionKind::None;
    int32_t    m_pending_view_x = 0;
    int32_t    m_pending_view_y = 0;
    double     m_last_motion_ms = 0.0;
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


/*
 * ── PaintRepeat ──────────────────────────────────────────────────────────
 *
 * THE FRAME, LENT TO THE PALETTE AS A CLOCK. It draws nothing and has no extent;
 * it exists to be VISITED. While the palette has a stepper held it answers yes
 * to Animating(), which keeps the compositor above it composing every frame,
 * and each of those compositions calls DrawInto -- which is where it asks the
 * palette to Tick(). With nothing held it answers no and costs nothing, and the
 * bar settles like any other still picture.
 *
 * A child of the bar, spawned by the toolbar script beside the buttons it
 * serves. Not a thread, not a timer: there is no clock in this page but the
 * frame, and this is the one way in the tree to be told when it happens.
 */
class PaintRepeat : public Drawable2DBase<PaintRepeat>,
                    public DeletableBase<PaintRepeat>
{
public:
    WIRE_TYPE_IDENTITY(PaintRepeat);

    int32_t m_order = 0;
    bool operator<(const PaintRepeat& o) const { return m_order < o.m_order; }
    int32_t Order() override { return m_order; }
    bool DeleteConcrete() override { return true; }

    void BindPalette(ETCS::RID palette) { m_palette = palette; }

    bool Animating() override
    {
        PaintPalette* p = palette();
        return p && p->holding();
    }

    Rect2D BoundsConcrete() override { return Rect2D{ 0, 0, 0, 0 }; }
    bool ContainsLocalConcrete(int32_t, int32_t) override { return false; }
    WindowSize GetSizeConcrete() override { return WindowSize{ 0, 0 }; }
    // A Drawable2D is a Surface; this one has no pixels and takes no drawing.
    void ClearConcrete(float, float, float, float) override {}
    void DrawRectConcrete(int32_t, int32_t, uint32_t, uint32_t,
                          float, float, float, float) override {}
    void BlitConcrete(Surface_*, int32_t, int32_t, uint32_t, uint32_t, float) override {}
    void DrawIntoConcrete(Surface_*) override
    {
        if (PaintPalette* p = palette()) p->Tick();
    }

private:
    PaintPalette* palette()
    {
        if (m_palette == 0) return nullptr;
        ETCS::Entity* raw = paint_resolve_tag("PaintPalette", m_palette);
        return raw ? static_cast<PaintPalette*>(raw->getTrueType()) : nullptr;
    }
    ETCS::RID m_palette = 0;
};

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
                const bool inside = root && paint_pane_contains(pane.root, at_x, at_y);
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
            if (!paint_pane_contains(m_panes[r.second].root, at_x, at_y)) continue;

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

    /*
 * ── A KEY IS NOT A CLICK ─────────────────────────────────────────────────
 *
 * This router used to turn every key event into ScriptPress/ScriptRelease, so
 * any keystroke drew a dab at wherever the pointer happened to be -- exactly the
 * mistake ontology/InputSource.h warns about in the comment that introduced
 * separate button events ("a paint program ended up drawing on any keystroke
 * instead of on a click"). The key ring was wired to the pointer's meaning.
 *
 * OFFERED TO EVERY PANE, AND THE ONE WITH THE FOCUS TAKES IT. A key carries no
 * position, so containment cannot choose a pane the way it does for a pointer,
 * and the thing that should receive it is whatever is being typed into. Rather
 * than keep a focus pointer here -- a second place for it to be wrong -- the
 * question is asked of each pane in the same top-first order, and the first that
 * says it consumed the key ends the walk. A pane with nothing selected consumes
 * nothing, so with no text box open every key still reaches nobody, which is the
 * behaviour anything not-yet-written depends on.
 */
    void RouteKey(uint16_t key, bool down)
    {
        // A modifier's edges are recorded and go no further -- see
        // PaintModifierKeys. Both edges, which is why this is above the
        // release early-out: ctrl coming UP is the half a chord depends on.
        if (paint_modifiers().Note(key, down)) return;
        if (!down) return;          // nothing else here acts on release yet
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

        for (const auto& r : ranked)
        {
            ETCS::Entity* raw = paint_resolve_tag("PaintInput", m_panes[r.second].input);
            if (!raw) continue;
            auto* in = static_cast<PaintInput*>(raw->getTrueType());
            if (in && in->KeyDown(key)) return;
        }
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

DEFINE_WORK_FUNC_TYPED(PaintTool, SetMotionCoalesceMs, (double, ms))
{
    (void)ctx;
    self.SetMotionCoalesceMs(ms);
}

// SetAlphaPercent <0..100> -- the opacity the tool's colour is laid down at.
DEFINE_WORK_FUNC_TYPED(PaintTool, SetAlphaPercent, (int32_t, pct))
{
    (void)ctx;
    self.SetAlphaPercent(pct);
}

// SetKind <brush|line|rect|ellipse|fill|smudge|ruler|glyph|select>
DEFINE_WORK_FUNC(PaintTool, SetKind)
{
    (void)ctx;
    self.SetKind(data.restAsString());
}

// SetMode <rect|ellipse|wand|lasso> -- how the select tool draws its boundary.
// Read by that kind alone; see PaintSelectMode.
// SetShape <rect|oval|triangle|diamond|star> -- the shape tool's outline.
DEFINE_WORK_FUNC(PaintTool, SetShape)
{
    (void)ctx;
    std::string name; data >> name;
    self.SetShape(name);
}

DEFINE_WORK_FUNC(PaintTool, SetMode)
{
    (void)ctx;
    self.SetMode(data.restAsString());
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

// The glyph provider the text boxes are drawn with -- any leaf claiming Glyphs.
DEFINE_WORK_FUNC_TYPED(PaintDocument, BindGlyphs, (ETCS::RID, glyphs))
{
    (void)ctx;
    self.BindGlyphs(glyphs);
}

// AddTextBox <x> <y> <w> <h>, in document coordinates.
DEFINE_WORK_FUNC_TYPED(PaintDocument, AddTextBox,
                       (int32_t, x), (int32_t, y), (int32_t, w), (int32_t, h))
{
    (void)ctx;
    self.AddTextBox(x, y, w, h);
}

// SetTextBoxText <id> <the rest of the line>. The text is taken raw rather than
// parsed as a field, because a caption contains spaces and commas.
DEFINE_WORK_FUNC(PaintDocument, SetTextBoxText)
{
    (void)ctx;
    uint32_t id = 0;
    data >> id;
    ETCS::Buffer rest;
    data >> rest;
    if (!self.SetTextBoxText(id, rest.toString()))
        ETCS_LOG("PaintDocument", "no text box " << id << " to set text on.");
}

DEFINE_WORK_FUNC_TYPED(PaintDocument, RemoveTextBox, (uint32_t, id))
{
    (void)ctx;
    self.RemoveTextBox(id);
}

DEFINE_WORK_FUNC_TYPED(PaintDocument, ShowTextBoxes, (int32_t, on))
{
    (void)ctx;
    self.ShowTextBoxes(on != 0);
}

DEFINE_WORK_FUNC_TYPED(PaintDocument, SelectTextBox, (uint32_t, id))
{
    (void)ctx;
    self.SelectTextBox(id);
}

// ── the selection ────────────────────────────────────────────────────────
//
// The four ways in, the way out, and the carry, all in document coordinates.
// Verbs so that a script can select and move without a pointer -- which is
// also what makes the carry assertable: MoveSelection then two Reports, and
// the inked count has to have moved with it.

// SelectRect <x0> <y0> <x1> <y1> -- opposite corners, inclusive.
DEFINE_WORK_FUNC_TYPED(PaintDocument, SelectRect,
                       (int32_t, x0), (int32_t, y0), (int32_t, x1), (int32_t, y1))
{
    (void)ctx;
    self.SelectRect(x0, y0, x1, y1);
}

// SelectEllipse <x0> <y0> <x1> <y1> -- inscribed in that box.
DEFINE_WORK_FUNC_TYPED(PaintDocument, SelectEllipse,
                       (int32_t, x0), (int32_t, y0), (int32_t, x1), (int32_t, y1))
{
    (void)ctx;
    self.SelectEllipse(x0, y0, x1, y1);
}

// SelectColor <x> <y> <tolerance> -- the wand, on the active layer. Tolerance
// as PaintTool::SetTolerance takes it, 0..255 per channel.
DEFINE_WORK_FUNC_TYPED(PaintDocument, SelectColor,
                       (int32_t, x), (int32_t, y), (uint32_t, tolerance))
{
    (void)ctx;
    self.SelectColor(x, y, tolerance);
}

// SelectPath <x> <y> <x> <y> ... -- the lasso, closed back to its first point.
// The rest of the line as pairs, since a path has no fixed arity; an odd
// trailing number is dropped rather than paired with nothing.
DEFINE_WORK_FUNC(PaintDocument, SelectPath)
{
    (void)ctx;
    std::vector<PaintStrokePoint> path;
    while (data.read_offset < data.written)
    {
        const size_t before = data.read_offset;
        PaintStrokePoint p{};
        data >> p.x;
        if (data.read_offset >= data.written) break;
        data >> p.y;
        if (data.read_offset == before) break;
        path.push_back(p);
    }
    if (!self.SelectPath(path))
        ETCS_LOG("PaintDocument", "SelectPath needs at least three points inside the page.");
}

DEFINE_WORK_FUNC(PaintDocument, ClearSelection)
{
    (void)ctx; (void)data;
    self.ClearSelection();
}

// MoveSelection <dx> <dy> -- lift the selected pixels off the active layer and
// drop them that far away; the selection goes with them.
// CopySelection / CutSelection / PasteSelection -- the clipboard, as verbs.
// ctrl+c / ctrl+x / ctrl+v are glue onto these (PaintInput::KeyDown).
DEFINE_WORK_FUNC(PaintDocument, CopySelection)
{
    (void)ctx; (void)data;
    ETCS_LOG("PaintDocument", "copy: " << (self.CopySelection() ? "taken" : "nothing selected"));
}
DEFINE_WORK_FUNC(PaintDocument, DeleteSelection)
{
    (void)ctx; (void)data;
    ETCS_LOG("PaintDocument", "delete: " << (self.DeleteSelection() ? "cleared" : "nothing selected"));
}
DEFINE_WORK_FUNC(PaintDocument, CutSelection)
{
    (void)ctx; (void)data;
    ETCS_LOG("PaintDocument", "cut: " << (self.CutSelection() ? "taken" : "nothing selected"));
}
DEFINE_WORK_FUNC(PaintDocument, PasteSelection)
{
    (void)ctx; (void)data;
    ETCS_LOG("PaintDocument", "paste: " << (self.PasteSelection() ? "landed" : "nothing to paste"));
}

// Undo / Redo -- the three-deep placeholder history (PaintDocument::Remember).
DEFINE_WORK_FUNC(PaintDocument, Undo)
{
    (void)ctx; (void)data;
    const bool did = self.Undo();
    ETCS_LOG("PaintDocument", "undo: " << (did ? "stepped" : "nothing") << " ("
             << self.undoDepth() << " back, " << self.redoDepth() << " forward)");
}
DEFINE_WORK_FUNC(PaintDocument, Redo)
{
    (void)ctx; (void)data;
    const bool did = self.Redo();
    ETCS_LOG("PaintDocument", "redo: " << (did ? "stepped" : "nothing") << " ("
             << self.undoDepth() << " back, " << self.redoDepth() << " forward)");
}

DEFINE_WORK_FUNC_TYPED(PaintDocument, MoveSelection, (int32_t, dx), (int32_t, dy))
{
    (void)ctx;
    if (!self.MoveSelection(dx, dy))
        ETCS_LOG("PaintDocument", "MoveSelection with nothing selected, or no active layer.");
}

DEFINE_WORK_FUNC_TYPED(PaintDocument, RenderToSurface,
    (ETCS::RID, target), (int32_t, x), (int32_t, y))
{
    (void)ctx;
    self.RenderToSurface(target, x, y);
}

// ImportImage <path> / ExportImage <path> / ExportLayer <path> -- the rest of
// the line, as SetName takes it, because a path has dots and may have spaces;
// a quoted one is unquoted, since a script that quoted it meant the inside.
// The same three lines from the terminal on either substrate and from the
// page's two buttons (index.html), which write and read the browser's own
// filesystem and call these -- see the PAM note above PaintImage.
static inline std::string paint_path_arg(ETCS::Buffer& data)
{
    std::string s = data.restAsString();
    const char* ws = " \t\r\n";
    const size_t a = s.find_first_not_of(ws);
    if (a == std::string::npos) return std::string();
    const size_t b = s.find_last_not_of(ws);
    s = s.substr(a, b - a + 1);
    if (s.size() >= 2 && (s.front() == '\'' || s.front() == '"') && s.back() == s.front())
        s = s.substr(1, s.size() - 2);
    return s;
}

DEFINE_WORK_FUNC(PaintDocument, ImportImage)
{
    (void)ctx;
    const std::string path = paint_path_arg(data);
    if (path.empty()) { ETCS_LOG("PaintDocument", "ImportImage needs a path."); return; }
    self.ImportImage(path);
}

DEFINE_WORK_FUNC(PaintDocument, ExportImage)
{
    (void)ctx;
    const std::string path = paint_path_arg(data);
    if (path.empty()) { ETCS_LOG("PaintDocument", "ExportImage needs a path."); return; }
    self.ExportImage(path);
}

DEFINE_WORK_FUNC(PaintDocument, ExportLayer)
{
    (void)ctx;
    const std::string path = paint_path_arg(data);
    if (path.empty()) { ETCS_LOG("PaintDocument", "ExportLayer needs a path."); return; }
    self.ExportLayer(path);
}

// Resize <w> <h> <anchor 0..8> -- the page re-stated around its pixels, the
// anchored cell of a 3x3 grid staying put (0 top-left, 4 centre, 8
// bottom-right). New <w> <h> -- the same extent with every layer cleared.
// Neither is a history step: see PaintDocument::Resize. Render the surface
// after either, as after any scripted change.
DEFINE_WORK_FUNC_TYPED(PaintDocument, Resize, (uint32_t, w), (uint32_t, h), (int32_t, anchor))
{
    (void)ctx;
    self.Resize(w, h, anchor);
}

DEFINE_WORK_FUNC_TYPED(PaintDocument, New, (uint32_t, w), (uint32_t, h))
{
    (void)ctx;
    self.New(w, h);
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
// The glyph provider the edge ruler's numbers come from.
DEFINE_WORK_FUNC_TYPED(PaintSurface, BindGlyphs, (ETCS::RID, glyphs))
{
    (void)ctx;
    self.BindGlyphs(glyphs);
}

// ShowEdgeRuler <0|1> -- the marks every 100 px around the drawable pane's edge.
DEFINE_WORK_FUNC_TYPED(PaintSurface, ShowEdgeRuler, (int32_t, on))
{
    (void)ctx;
    self.ShowEdgeRuler(on != 0);
}

// BindRulerFrame <rid> -- the surface the pane is inset in, which is where the
// ruler goes so that it is outside anywhere you can draw.
DEFINE_WORK_FUNC_TYPED(PaintSurface, BindRulerFrame, (ETCS::RID, frame))
{
    (void)ctx;
    self.BindRulerFrame(frame);
}

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

// SetHoldCapacity <frames> -- how long a held stepper keeps stepping with no
// release in sight. 600 by default; see PaintPalette::Tick.
DEFINE_WORK_FUNC_TYPED(PaintPalette, SetHoldCapacity, (int32_t, n))
{
    (void)ctx;
    self.SetHoldCapacity(static_cast<uint16_t>(n < 1 ? 1 : n));
}

// PaintRepeat: BindPalette <rid> -- whose steppers this frame-visit serves.
DEFINE_WORK_FUNC_TYPED(PaintRepeat, BindPalette, (ETCS::RID, palette))
{
    (void)ctx;
    self.BindPalette(palette);
}

// ─────────────────────────────────────────────────────────────────────────────

// Create <doc_rid> <db_rid> -- the database must already be Connected: the
// schema is applied here, and a connection is the script's decision (a path
// is where a page store lives, and only the script knows the substrate).
DEFINE_WORK_FUNC_TYPED(PaintPages, Create, (ETCS::RID, document), (ETCS::RID, database))
{
    (void)ctx;
    self.Create(document, database);
}

// BindSurface <rid> -- the surface repainted when a page is loaded.
DEFINE_WORK_FUNC_TYPED(PaintPages, BindSurface, (ETCS::RID, surface))
{
    (void)ctx;
    self.BindSurface(surface);
}

DEFINE_WORK_FUNC(PaintPages, Save)
{
    (void)ctx; (void)data;
    self.Save();
}

DEFINE_WORK_FUNC_TYPED(PaintPages, Load, (int64_t, id))
{
    (void)ctx;
    self.Load(id);
}

DEFINE_WORK_FUNC(PaintPages, New)
{
    (void)ctx; (void)data;
    self.New();
}

DEFINE_WORK_FUNC(PaintPages, Next)
{
    (void)ctx; (void)data;
    self.Next();
}

DEFINE_WORK_FUNC(PaintPages, Prev)
{
    (void)ctx; (void)data;
    self.Prev();
}

DEFINE_WORK_FUNC(PaintPages, List)
{
    (void)ctx; (void)data;
    self.List();
}

DEFINE_WORK_FUNC_TYPED(PaintPages, Delete, (int64_t, id))
{
    (void)ctx;
    self.Delete(id);
}

// The entity, not a page: Delete takes a page id, so the verb every other
// type spells Delete is Destroy here -- two meanings of one word on one type
// would make `pages.Delete(3)` a coin toss.
DEFINE_WORK_FUNC(PaintPages, Destroy)
{
    (void)ctx; (void)data;
    self.DeleteConcrete();
}

// AddHoverLabel <entry_rid> <label_rid> -- a caption shown while that entry's
// group is hovered, hidden otherwise. See PaintPalette::AddHoverLabel.
DEFINE_WORK_FUNC_TYPED(PaintPalette, AddHoverLabel, (ETCS::RID, entry), (ETCS::RID, label))
{
    (void)ctx;
    self.AddHoverLabel(entry, label);
}

DEFINE_WORK_FUNC_TYPED(PaintPalette, AddRadiusDelta, (ETCS::RID, node), (float, delta))
{
    (void)ctx;
    self.AddRadiusDelta(node, delta);
}

DEFINE_WORK_FUNC_TYPED(PaintPalette, BindWheel, (ETCS::RID, wheel))
{
    (void)ctx;
    self.BindWheel(wheel);
}

// AddWheelArrow <arrow node> <colour entry it picks for>
DEFINE_WORK_FUNC_TYPED(PaintPalette, AddWheelArrow, (ETCS::RID, node), (ETCS::RID, slot))
{
    (void)ctx;
    self.AddWheelArrow(node, slot);
}

// AddModeArrow <arrow node> <the select tool's entry> -- steps the mode.
DEFINE_WORK_FUNC_TYPED(PaintPalette, AddModeArrow, (ETCS::RID, node), (ETCS::RID, slot))
{
    (void)ctx;
    self.AddModeArrow(node, slot);
}

// SetShapeReadout <label> -- the shape slice's current outline, by name.
DEFINE_WORK_FUNC_TYPED(PaintPalette, SetShapeReadout, (ETCS::RID, label))
{
    (void)ctx;
    self.SetShapeReadout(label);
}

DEFINE_WORK_FUNC_TYPED(PaintPalette, SetModeReadout, (ETCS::RID, label))
{
    (void)ctx;
    self.SetModeReadout(label);
}

DEFINE_WORK_FUNC_TYPED(PaintPalette, AddAlphaDelta, (ETCS::RID, node), (float, delta_pct))
{
    (void)ctx;
    self.AddAlphaDelta(node, delta_pct);
}

DEFINE_WORK_FUNC_TYPED(PaintPalette, SetRadiusReadout, (ETCS::RID, label))
{
    (void)ctx;
    self.SetRadiusReadout(label);
}

DEFINE_WORK_FUNC_TYPED(PaintPalette, SetAlphaReadout, (ETCS::RID, label))
{
    (void)ctx;
    self.SetAlphaReadout(label);
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

/*
 * AddCall <node_rid> <target_rid> <Tag.Action> <args...> -- pressing the node
 * calls that verb on that entity with the rest of the line as its argument
 * text, exactly as a script line would. The general entry; see
 * PaintPalette::AddCall for why the older kinds are not spelled with it.
 */
DEFINE_WORK_FUNC(PaintPalette, AddCall)
{
    (void)ctx;
    ETCS::RID node = 0, target = 0;
    std::string action;
    data >> node >> target >> action;
    self.AddCall(node, target, action, data.restAsString());
}

// AddPopup <node_rid> <pane_rid> <input_rid> -- pressing the node opens the
// pane (routed through that input) and pressing anywhere closes it. BindRouter
// <rid> is what it opens into.
DEFINE_WORK_FUNC_TYPED(PaintPalette, AddPopup, (ETCS::RID, node), (ETCS::RID, pane), (ETCS::RID, input))
{
    (void)ctx;
    self.AddPopup(node, pane, input);
}

DEFINE_WORK_FUNC_TYPED(PaintPalette, BindRouter, (ETCS::RID, router))
{
    (void)ctx;
    self.BindRouter(router);
}

DEFINE_WORK_FUNC(PaintPalette, Report)
{
    (void)ctx; (void)data;
    self.Report();
}

DEFINE_WORK_FUNC(PaintRepeat, Delete)
{
    (void)ctx; (void)data;
    self.DeleteConcrete();
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

// OpenAt <x> <y> <slot> -- open the wheel above a control, aimed at one swatch.
DEFINE_WORK_FUNC_TYPED(PaintColorWheel, OpenAt,
                       (int32_t, x), (int32_t, y), (ETCS::RID, slot))
{
    (void)ctx;
    self.OpenAt(x, y, slot);
}

DEFINE_WORK_FUNC_TYPED(PaintColorWheel, BindSurface, (ETCS::RID, surface))
{
    (void)ctx;
    self.BindSurface(surface);
}

DEFINE_WORK_FUNC_TYPED(PaintColorWheel, SetTargetSlot, (ETCS::RID, slot))
{
    (void)ctx;
    self.SetTargetSlot(slot);
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
// Pick <x> <y>, in the pane's own space. Logs which of the three things it was,
// because "nothing happened" and "the brightness moved" look the same otherwise.
DEFINE_WORK_FUNC_TYPED(PaintColorWheel, Pick, (int32_t, x), (int32_t, y))
{
    (void)ctx;
    const PaintPick r = self.Pick(x, y);
    ETCS_LOG("PaintColorWheel", "pick at " << x << "," << y << " -> "
             << (r == PaintPick::Picked   ? "colour"
               : r == PaintPick::Adjusted ? "value strip" : "missed"));
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

// ── PaintCanvasMenu ──────────────────────────────────────────────────────
//
// The menu's look is paint_menu.etcs; every verb here is what one of its
// nodes says when pressed (PaintPalette::AddCall), and every one is equally a
// line a script or the page can type.

DEFINE_WORK_FUNC_TYPED(PaintCanvasMenu, Create, (ETCS::RID, document))
{
    (void)ctx;
    self.Create(document);
}

DEFINE_WORK_FUNC_TYPED(PaintCanvasMenu, BindSurface, (ETCS::RID, surface))
{
    (void)ctx;
    self.BindSurface(surface);
}

// StepWidth <delta> / StepHeight <delta> -- in pixels, clamped to 64..8192.
DEFINE_WORK_FUNC_TYPED(PaintCanvasMenu, StepWidth, (int32_t, delta))
{
    (void)ctx;
    self.StepWidth(delta);
}

DEFINE_WORK_FUNC_TYPED(PaintCanvasMenu, StepHeight, (int32_t, delta))
{
    (void)ctx;
    self.StepHeight(delta);
}

// SetAnchor <0..8> -- which cell of the 3x3 grid stays put on resize.
DEFINE_WORK_FUNC_TYPED(PaintCanvasMenu, SetAnchor, (int32_t, anchor))
{
    (void)ctx;
    self.SetAnchor(anchor);
}

DEFINE_WORK_FUNC_TYPED(PaintCanvasMenu, BindWidthReadout, (ETCS::RID, label))
{
    (void)ctx;
    self.BindWidthReadout(label);
}

DEFINE_WORK_FUNC_TYPED(PaintCanvasMenu, BindHeightReadout, (ETCS::RID, label))
{
    (void)ctx;
    self.BindHeightReadout(label);
}

// BindAnchorCell <index 0..8> <node_rid> -- the cell's fill shows whether it
// is the chosen one, from now on.
DEFINE_WORK_FUNC_TYPED(PaintCanvasMenu, BindAnchorCell, (int32_t, index), (ETCS::RID, node))
{
    (void)ctx;
    self.BindAnchorCell(index, node);
}

DEFINE_WORK_FUNC(PaintCanvasMenu, ApplyResize)
{
    (void)ctx; (void)data;
    self.ApplyResize();
}

DEFINE_WORK_FUNC(PaintCanvasMenu, ApplyNew)
{
    (void)ctx; (void)data;
    self.ApplyNew();
}

DEFINE_WORK_FUNC(PaintCanvasMenu, Save)
{
    (void)ctx; (void)data;
    self.Save();
}

DEFINE_WORK_FUNC(PaintCanvasMenu, Load)
{
    (void)ctx; (void)data;
    self.Load();
}

DEFINE_WORK_FUNC(PaintCanvasMenu, Report)
{
    (void)ctx; (void)data;
    self.Report();
}

DEFINE_WORK_FUNC(PaintCanvasMenu, Delete)
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

// Key <glfw keycode> -- the same edge the key ring drives, reachable from a
// script. Down only: nothing acts on release (PaintRouter::RouteKey).
DEFINE_WORK_FUNC_TYPED(PaintRouter, Key, (int32_t, key))
{
    (void)ctx;
    self.RouteKey(static_cast<uint16_t>(key), true);
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
        // Keys as keys -- see PaintRouter::RouteKey for what this used to do.
        if (ev.action == INPUT_DOWN)      self.RouteKey(ev.key, true);
        else if (ev.action == INPUT_UP)   self.RouteKey(ev.key, false);
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

// SetHoldCapacity <n> -- how many unconfirmed events a held button survives
// before this input releases it itself. 4 for paint; raise it for anything that
// treats a hold as a mode. See HeldCharge (ontology/InputSource.h).
DEFINE_WORK_FUNC_TYPED(PaintInput, SetHoldCapacity, (int32_t, n))
{
    (void)ctx;
    self.SetHoldCapacity(static_cast<uint16_t>(n < 1 ? 1 : n));
}

// BindPages <rid> -- the page store the ctrl+PageUp/PageDown chords switch.
DEFINE_WORK_FUNC(PaintInput, BindPages)
{
    (void)ctx;
    ETCS::RID pages = 0;
    data >> pages;
    self.BindPages(pages);
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
    /*
     * THE TOOL'S STATE, not just the cursor's. This reported two numbers and a
     * flag, which answers almost nothing you would ask it: the questions that
     * actually come up are what is loaded and how it will mark -- which kind,
     * what size, what colour, at what opacity.
     */
    ETCS_LOG("PaintInput::Report", "cursor (" << self.cursorX() << ", " << self.cursorY()
             << ")  stroke " << (self.StrokeActive() ? "ACTIVE" : "idle"));
    if (PaintTool* t = self.tool())
    {
        const PaintBrushState& br = t->brush();
        ETCS_LOG("PaintInput::Report", "  tool " << paint_tool_kind_name(t->kind())
                 << (t->kind() == PaintToolKind::Select
                     ? std::string(" (") + paint_select_mode_name(t->mode()) + ")" : std::string())
                 << " radius " << br.radius_px
                 << " colour " << br.color.r << ", " << br.color.g << ", " << br.color.b
                 << " alpha " << t->alphaPercent() << "% (" << br.color.a << ")");
    }
    else ETCS_LOG("PaintInput::Report", "  no tool bound.");
}

DEFINE_WORK_FUNC(PaintInput, Delete)
{
    (void)ctx; (void)data;
    self.DeleteConcrete();
}

#endif // PAINTPROVIDER_H__
