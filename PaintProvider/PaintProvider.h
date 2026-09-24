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
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <mutex>
#include <random>
#include <thread>
#include <string>
#include <map>
#include <memory>
#include <cctype>
#include <unordered_map>
#include <vector>

// PaintNode's route surface. Header-only POD (a parsed request, and a frame
// that names borrowed bytes) with no NetworkProvider type in it, so including
// it here does not make PaintProvider depend on NetworkProvider being loaded --
// the same property ChessProvider's own "takes only Buffer data, never a
// NetworkProvider type" note protects, reached the same way.
#include "../NetworkProvider/NetworkProvider/RouteRequest.h"

/*
 * THE CODECS. stb_image reads PNG, JPEG, BMP, GIF and TGA; stb_image_write
 * writes PNG. Two public-domain single headers, pinned as a vendor entry in
 * manifests/PaintProvider.json rather than copied into libs/, for the same
 * reason sqlite and glfw are fetched: a codec is somebody else's code at a
 * known revision, not this tree's. Declarations here; the implementation is
 * compiled ONCE, in PaintProvider.cc, which defines the *_IMPLEMENTATION macros
 * before including this header.
 *
 * PAM stays. It is what the page store keeps its rows in (PaintPages), and a
 * blob that is raw bytes behind a text header is the right thing for a
 * database to hold: no decode on load, and the same reader the export uses.
 * PNG is for files a person opens elsewhere.
 */
#include "stb_image.h"
#include "stb_image_write.h"
// Outline fonts for the text boxes (PaintFonts), compiled once in the .cc
// like the codecs above.
#include "stb_truetype.h"

#if defined(__EMSCRIPTEN__)
// For MAIN_THREAD_EM_ASM: the canvas menu's save/load reach the page's own
// file controls by dispatching a DOM event (PaintCanvasMenu::page_event).
#include <emscripten.h>
#include <emscripten/em_asm.h>
// For paint_heap_headroom: the wasm heap is fixed (loaders/Makefile's memory
// note), so a picture has to ask before it allocates.
#include <emscripten/heap.h>
#include <malloc.h>
#include <unistd.h>
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

/*
 * ── the nib ──────────────────────────────────────────────────────────────
 *
 * WHAT IS LOADED, not what the gesture is. PaintToolKind is the shape of the
 * whole interaction and says so; this is the other half that comment names --
 * the thing a kind is holding. Every marking kind holds one, and they all
 * reduce to DrawBrush, so a rectangle outlined with the stylus comes out
 * calligraphic without the shape code hearing about it.
 *
 * Two independent facts on one stepped list, because that is the choice a hand
 * makes: Round and Stylus differ in the stamp's SHAPE, Erase in what the stamp
 * WRITES (blend, below). One arrow instead of two, at the cost of stepping past
 * a shape to reach the eraser.
 */
enum class PaintTipMode : uint8_t
{
    Round = 0,   // the disc paint_stamp_of has always made
    Stylus = 1,  // a chisel: a straight nib held at an angle
    Erase = 2,   // the disc again, taking ink off rather than laying it
};

inline const char* paint_tip_mode_name(PaintTipMode t)
{
    switch (t)
    {
        case PaintTipMode::Round:  return "round";
        case PaintTipMode::Stylus: return "stylus";
        case PaintTipMode::Erase:  return "erase";
    }
    return "round";
}

inline PaintTipMode paint_tip_mode_from(const std::string& s)
{
    if (s == "stylus") return PaintTipMode::Stylus;
    if (s == "erase" || s == "eraser") return PaintTipMode::Erase;
    return PaintTipMode::Round;
}

// The angle a chisel is held at, and how thick the nib is across as a fraction
// of its length. A quarter is what makes a stroke ALONG the nib a hairline and
// one ACROSS it full width, which is the whole visible difference from a disc.
static constexpr float PAINT_STYLUS_COS   = 0.70710678f;   // 45 degrees
static constexpr float PAINT_STYLUS_SIN   = 0.70710678f;
static constexpr float PAINT_STYLUS_THICK = 0.25f;

struct PaintStrokePoint
{
    int32_t x = 0;
    int32_t y = 0;
    uint32_t pressure = 255;
};

struct PaintBrushState
{
    // HOW WIDE THE MARK IS, in document pixels -- not a radius, whatever the
    // verb that sets it is called. The wire name stays PaintTool.SetRadius
    // because scripts and the toolbar already say it (paint_toolbar.etcs);
    // what changed is the arithmetic under it, and the field is named for what
    // it holds so the next reader does not have to rediscover which
    // (paint_stamp_of).
    float size_px = 8.0f;
    float hardness = 0.75f;
    PaintColor color{1.0f, 0.0f, 0.0f, 1.0f};
    PaintBlendMode blend = PaintBlendMode::Normal;
    // The nib. Two fields rather than one because they are two questions --
    // what shape the stamp is, and what it writes -- and only the toolbar's
    // arrow ties them together (PaintTipMode). SetBlendMode still sets blend on
    // its own, for a script that wants Erase under a stylus.
    PaintTipMode tip = PaintTipMode::Round;
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
 * it stops there -- Drawable2DBase::parentAbsoluteOrigin). Here the question
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
/*
 * A HIDDEN PANE CONTAINS NOTHING, for routing as for picking. The router ranks
 * every pane it was given and hands the event to the topmost one whose root
 * contains the point; a pane that is registered while hidden -- the visitors
 * window sits in the router from boot, order 30, and is shown later -- would
 * otherwise take every press inside its rectangle ahead of the menu popup
 * beneath it (order 29) and drop it, since PickAt on a hidden root answers
 * nothing. That was every press on a menu item doing nothing while the popup
 * opened and closed fine. Same rule Drawable2D_::PickAt applies one level
 * down: if you cannot see it, you cannot hit it.
 */
static inline bool paint_pane_contains(ETCS::RID root, int32_t x, int32_t y)
{
    ETCS::Held<Drawable2D_> h = ETCS::resolve_held<Drawable2D_>("Drawable2D", root);
    if (!h) return false;
    if (void* d = static_cast<ETCS::Entity*>(h.get())->getInterfacePointer(ETCS::Buffer("Drawable")))
        if (static_cast<Drawable_*>(d)->Hidden()) return false;
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

/*
 * ── ONE VERB, SENT TO SOMEBODY ELSE'S NODE ───────────────────────────────────
 *
 * Every window in this module drives drawables it did not create: a panel tints
 * a plate a script spawned, a menu hides a label, a picker moves its own pane.
 * The seam for that is `Entity::call("<Tag>.<Work>", args)` -- the same one
 * loaders/etcs.cc's etcs_web_call uses -- and the TAG IS READ OFF THE ENTITY
 * rather than assumed, which is the whole point: a row may be built from
 * whatever leaf the script likes as long as that leaf exports the verb.
 *
 * WHY IT IS ONE FUNCTION. It was twelve. Resolve held, cast, compose
 * "<tag>.<verb>", write the payload, call inside a try -- eight lines, written
 * out once per verb per class: five hiders, three setters of text, two movers,
 * two fillers. Twelve places to fix when the seam changes, and it changed twice
 * already. The SECOND time is what makes this a compression rather than tidying:
 * a row's plate became a compositor (it is the row's pane now, which is what
 * lets one script draw every row), compositors answer SetBackground where
 * polygons answer SetFill, and only ONE of the two fillers learned that. The
 * other refused every plate it was handed, once per row per refresh, silently,
 * because a refused verb is a log line and not an error. A rule that lives in
 * one copy of twelve is not a rule; it is a coincidence that held so far.
 *
 * TWO SPELLINGS FOR ONE IDEA is therefore a parameter and not a special case.
 * `surface_verb`, when given, is what everything that is not a polygon is sent
 * instead: a SHAPE has a fill and a SURFACE has a background, and they are the
 * same instruction. Nothing else in the module needs to know that. The split is
 * on the polygon rather than on the compositor because that is where the tags
 * actually divide -- PolygonDrawable2D exports SetFill, and CompositeDrawable2D,
 * TextLabel and Scene3D all export SetBackground (RenderProvider.cc's tag
 * blocks). Asking "is it a compositor" got a label's plate refused for the same
 * reason the row's did.
 *
 * SILENT ON A MISSING NODE. A row that declared no eye has no eye to colour,
 * and a node deleted underneath us is the script's business, not an error to
 * raise once per frame. The answer says whether the call went out, for the few
 * callers that care.
 *
 * ALWAYS MARKS THE CHAIN from the node up. The call crossed a module boundary,
 * so nothing in the tree saw the change, and a pane whose contents did not
 * change is not recomposed (ontology/Pixels.h). Half the old copies marked and
 * half did not; the ones that did not leaned on a repaint() the caller happened
 * to do next, which is the same seam in a different costume.
 */
static inline bool paint_node_verb(ETCS::RID node, const char* verb,
                                   const std::string& args,
                                   const char* surface_verb = nullptr)
{
    if (node == 0 || verb == nullptr) return false;
    ETCS::Held<Drawable2D_> held = ETCS::resolve_held<Drawable2D_>("Drawable2D", node);
    if (!held) return false;
    ETCS::Entity* e = static_cast<ETCS::Entity*>(held.get());
    if (!e) return false;
    const std::string tag = e->getSourceTag().toString();
    const char* which = verb;
    if (surface_verb && tag.find("PolygonDrawable2D") == std::string::npos)
        which = surface_verb;
    ETCS::Buffer action;
    action.write((tag + "." + which).c_str());
    ETCS::Buffer payload;
    payload.write(args.c_str());
    try { e->call(action, payload); } catch (...) { return false; }
    for (ETCS::Entity* n = e; n; n = n->getParent())
        etcs_mark_observed(n);
    return true;
}

// The four the module actually asks for, so a call site reads as the intent and
// not as the mechanism. Hidden is neither drawn nor picked (Drawable2D_::
// PickAt), which is what "this row has no delete" has to mean -- a transparent
// button still takes the press.
static inline bool paint_node_hidden(ETCS::RID node, bool hidden)
{
    return paint_node_verb(node, "SetHidden", hidden ? "1" : "0");
}

static inline bool paint_node_text(ETCS::RID node, const std::string& text)
{
    return paint_node_verb(node, "SetText", text);
}

static inline bool paint_node_moved(ETCS::RID node, int32_t x, int32_t y)
{
    return paint_node_verb(node, "SetPosition",
                           std::to_string(x) + ", " + std::to_string(y));
}

static inline bool paint_node_fill(ETCS::RID node, float r, float g, float b, float a)
{
    return paint_node_verb(node, "SetFill",
                           std::to_string(r) + " " + std::to_string(g) + " "
                         + std::to_string(b) + " " + std::to_string(a),
                           "SetBackground");
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
 * ── the nib, once, for everything that lays one down ─────────────────────
 *
 * THE SIZE IS THE WIDTH OF THE MARK. It was the RADIUS, and every symptom of
 * that came back to the same arithmetic: size 1 put down a disc of
 * dx^2 + dy^2 <= 1 -- three pixels across -- so the thinnest line the tool
 * could draw was three pixels wide, and a rectangle stroked with it stood a
 * pixel outside the box that was dragged on all four sides. The preview did
 * not: it drew a square of `size` view pixels (preview_line), so the shape
 * grew the moment the button came up. One of the two had to be wrong about
 * what the number meant, and the honest reading is the one every other paint
 * program uses and the toolbar already prints: `size` is how wide the mark is.
 *
 * SO THE FOOTPRINT IS COMPUTED IN ONE PLACE and every nib -- the committed
 * mark (PaintLayer::DrawBrush), the live dab on the view (paint_stamp_surface)
 * and the smudge window -- asks the same object what it covers. A preview that
 * does not agree with its commit is not a fast path, it is a lie about what
 * the tool does, and three separate `r = (int)radius_px` lines is how they
 * disagree.
 *
 * EVEN WIDTHS SIT BETWEEN PIXELS, which is what makes a 2px mark two pixels
 * and not three: the disc's centre goes on the boundary (c = -0.5) and the
 * covered offsets run -w/2 .. w/2-1. Odd widths centre on the pixel. Either
 * way the extent is exactly `w` pixels across, which is the whole point.
 */
struct PaintStamp
{
    int   lo = 0, hi = 0;      // offsets covered on each axis, inclusive
    float c  = 0.0f;           // where the centre sits relative to offset 0
    float r2 = 0.25f;
    // The nib's shape, and ONE predicate still answers for both -- which is the
    // same reason `row` walks `has` instead of solving the disc: two shapes
    // that round apart are a preview that disagrees with its commit.
    PaintTipMode tip = PaintTipMode::Round;
    float half = 0.5f;         // stylus: half the nib's length
    float thick = 0.5f;        // stylus: half its width across

    bool has(int dx, int dy) const
    {
        const float fx = static_cast<float>(dx) - c, fy = static_cast<float>(dy) - c;
        if (tip != PaintTipMode::Stylus) return fx * fx + fy * fy <= r2;
        // Rotated into the nib's own axes: u runs along it, v across.
        const float u =  fx * PAINT_STYLUS_COS + fy * PAINT_STYLUS_SIN;
        const float v = -fx * PAINT_STYLUS_SIN + fy * PAINT_STYLUS_COS;
        return std::fabs(u) <= half && std::fabs(v) <= thick;
    }

    // The inclusive x-span of row dy, false when the row is empty. Walked
    // rather than solved so there is one predicate (has) and not two that can
    // round apart -- the stamp is small, and the callers that want spans want
    // them once per row.
    bool row(int dy, int& x0, int& x1) const
    {
        x0 = hi + 1; x1 = lo - 1;
        for (int dx = lo; dx <= hi; ++dx)
            if (has(dx, dy)) { if (dx < x0) x0 = dx; x1 = dx; }
        return x0 <= x1;
    }
};

static inline PaintStamp paint_stamp_of(float size_px,
                                        PaintTipMode tip = PaintTipMode::Round)
{
    const int w = std::max(1, static_cast<int>(std::lround(size_px)));
    PaintStamp s;
    if (w % 2) { s.lo = -(w - 1) / 2; s.hi = (w - 1) / 2; s.c = 0.0f; }
    else       { s.lo = -w / 2;       s.hi = w / 2 - 1;   s.c = -0.5f; }
    const float rad = static_cast<float>(w) * 0.5f;
    s.r2 = rad * rad;
    s.tip = tip;
    // The chisel is inscribed in the SAME lo..hi square, so `size` still means
    // the extent of the mark and the erase nib stays the disc it replaces.
    s.half  = rad;
    s.thick = std::max(0.5f, rad * PAINT_STYLUS_THICK);
    return s;
}

/*
 * The live dab, and IT HAS TO BE THE SHAPE THE MARK WILL BE -- the same
 * PaintStamp PaintLayer::DrawBrush lays down, so the nib under the pointer and
 * the stroke it becomes are the same discrete shape. This was one DrawRect of
 * 2r x 2r once, a SQUARE nib previewing a round mark, which is the same class
 * of bug the size-is-a-width change above fixes one level down.
 *
 * ROW SPANS EITHER WAY. Where the destination has host bytes that is one
 * FillRect per row rather than a call per pixel; a device-backed view has no
 * address to write and gets the same spans through the family verb, which is
 * what that backend costs.
 */
static inline void paint_stamp_surface(ETCS::RID target, int32_t x, int32_t y,
                                       const PaintBrushState& brush)
{
    const PaintStamp st = paint_stamp_of(brush.size_px, brush.tip);
    int x0 = 0, x1 = 0;

    /*
     * AN ERASER HAS NO COLOUR TO PREVIEW WITH. Drawing the dab in the tool's
     * ink says the mark will be that colour, which is the one thing it will
     * not be. A neutral translucent nib still says the two things the dab is
     * for -- where it is and what shape it is -- and claims nothing else.
     */
    const bool lifting = (brush.blend == PaintBlendMode::Erase);
    const float r = lifting ? 0.85f : brush.color.r;
    const float g = lifting ? 0.85f : brush.color.g;
    const float b = lifting ? 0.88f : brush.color.b;
    const float a = lifting ? 0.35f : brush.color.a;

    if (Pixels_* px = ETCS::resolve_in_family<Pixels_>("Pixels", target))
    {
        for (int dy = st.lo; dy <= st.hi; ++dy)
            if (st.row(dy, x0, x1))
                px->FillRect(x + x0, y + dy, static_cast<uint32_t>(x1 - x0 + 1), 1u,
                             r, g, b, a);
        paint_mark_pixel_path(target);
        return;
    }

    Surface_* surface = ETCS::resolve_in_family<Surface_>("Surface", target);
    if (!surface) return;
    for (int dy = st.lo; dy <= st.hi; ++dy)
        if (st.row(dy, x0, x1))
            surface->DrawRect(x + x0, y + dy, static_cast<uint32_t>(x1 - x0 + 1), 1u,
                              r, g, b, a);
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
 *   ANCHORED     Glyph              drag a box; text is typed into it and wraps
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
 * THE SAME KEY WITH SHIFT HELD, for the one place that takes prose -- a text
 * box. A US layout, because GLFW's printable codes are the US key caps and a
 * layout table is the only way from a key to what its cap says with shift; a
 * name field keeps the plain mapping, since a name wants neither.
 */
static inline char paint_key_to_char_shifted(uint16_t key, bool shift)
{
    const char c = paint_key_to_char(key);
    if (!shift || c == 0) return c;
    if (c >= 'a' && c <= 'z') return static_cast<char>(c - 'a' + 'A');
    static const char* from = "1234567890-=[]\\;',./`";
    static const char* to   = "!@#$%^&*()_+{}|:\"<>?~";
    for (size_t i = 0; from[i]; ++i) if (from[i] == c) return to[i];
    return c;
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
    Move,       // drag the picture under the pointer; marks nothing
    Eyedrop,    // one press: the colour under it becomes the tool's, then the
                // previous kind comes back (PaintColorWheel::BeginPick)
    Animate     // drag a region; it becomes the animation's frame (PaintAnimation)
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
    case PaintToolKind::Move:    return "move";
    case PaintToolKind::Animate: return "anim";
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
    if (name == "move")    return PaintToolKind::Move;
    if (name == "anim")    return PaintToolKind::Animate;
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
        || k == PaintToolKind::Shape || k == PaintToolKind::Animate;
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
    return k != PaintToolKind::Ruler && k != PaintToolKind::Select && k != PaintToolKind::Animate;
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
 *
 * AND THE RULER AND THE SHAPE GET FOUR TIMES THAT, because their previews are
 * the two whose cost grows with the DRAG. The full-view rebuild underneath is
 * the same for every anchored kind, but what is drawn over it is not: a line is
 * a line however long it is, while the ruler lays ticks and numbers along its
 * whole extent and a shape walks every edge of a star or a diamond, stamping a
 * nib-sized rect per step of each. Drag either of them far, or with a wide
 * brush, and the per-sample cost climbs until 100ms stops being an interval the
 * rebuild can keep and starts being a queue. 400ms holds at any extent, and
 * two-and-a-half previews a second is still enough to aim a shape whose corners
 * you can already see.
 *
 * THIS IS A CPU-RASTER NUMBER. The preview is composited and stamped on this
 * thread; once a device backend is drawing it, the rebuild stops scaling with
 * the object and this ladder should come back down to one interval for every
 * anchored kind.
 */
inline double paint_tool_default_coalesce_ms(PaintToolKind k)
{
    if (k == PaintToolKind::Ruler || k == PaintToolKind::Shape) return 400.0;
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

    /*
     * ── combining with what was already selected ─────────────────────────
     *
     * Set is the incremental path and keeps the box and the count as it goes;
     * these two rewrite the mask wholesale, so they end by rederiving both.
     * A gesture is one combine, not one per pixel -- a subtract cannot be
     * expressed pixel-at-a-time through Set at all, since the box can only
     * shrink and Set can only grow it.
     *
     * `other` is the gesture's own region, in the same extent. A mismatched
     * one is refused rather than indexed, because the only way to have one is
     * a resize between the press and the release.
     */
    void Union(const PaintSelection& other)
    {
        if (other.w != w || other.h != h) return;
        for (size_t i = 0; i < mask.size(); ++i) if (other.mask[i]) mask[i] = 1;
        Recount();
    }

    void Subtract(const PaintSelection& other)
    {
        if (other.w != w || other.h != h) return;
        for (size_t i = 0; i < mask.size(); ++i) if (other.mask[i]) mask[i] = 0;
        Recount();
    }

    // The box and the count, from the mask. One walk of the page, per gesture
    // rather than per motion sample -- see the combine callers.
    void Recount()
    {
        count = 0; x0 = y0 = 0; x1 = y1 = -1;
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
            {
                if (!mask[static_cast<size_t>(y) * w + x]) continue;
                ++count;
                if (x1 < x0) { x0 = x1 = static_cast<int32_t>(x); y0 = y1 = static_cast<int32_t>(y); continue; }
                x0 = std::min(x0, static_cast<int32_t>(x)); x1 = std::max(x1, static_cast<int32_t>(x));
                y0 = std::min(y0, static_cast<int32_t>(y)); y1 = std::max(y1, static_cast<int32_t>(y));
            }
    }
};

/*
 * WHAT A SELECTION GESTURE DOES TO THE ONE ALREADY THERE.
 *
 * Replace is a bare drag and the only behaviour there used to be. The other two
 * are the modifiers, read from the key stream at the press (PaintModifierKeys,
 * PaintInput::begin_selection) -- ctrl adds the new region, shift takes it away.
 * Held on the document for the length of the gesture rather than passed to each
 * Select*, because a drag re-runs the Select* on every motion sample and the
 * answer has to be base-combined-with-gesture each time, not last-answer-
 * combined-with-gesture.
 */
enum class PaintSelectOp : uint8_t { Replace = 0, Add = 1, Subtract = 2 };

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
        touch_document();
    }

    void SetName(const std::string& name) { m_name = name; touch_document(); }
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

    /*
 * A LAYER'S OWN PROPERTIES ARE A CHANGE TO ITS DOCUMENT, and until now none of
 * them said so. The layer window re-reads the stack when the document's
 * revision moves (PaintInput's panel hook), so an eye toggled by a script, a
 * rename, a restack or an opacity change from anywhere but the panel's own
 * press left the window showing the old answer -- the rows and the picture
 * disagreeing, which is the one thing a layer window exists to prevent. A
 * comment here used to promise a `touch_document` that was never written.
 *
 * THROUGH THE PARENT, because that is what membership IS here: a layer is its
 * document's typed child (PaintDocument::ImportImage's note), so the document
 * is getParent() and nothing has to be kept in step. Silent when there is no
 * parent -- a layer under test, or one being torn down -- since there is then
 * nothing that could be showing it.
 */
    void touch_document();

    void SetVisible(bool visible) { m_visible = visible; touch_document(); }
    void ToggleVisible()           { m_visible = !m_visible; touch_document(); }

    void SetOpacity(float opacity)
    {
        m_opacity = std::clamp(opacity, 0.0f, 1.0f);
        touch_document();
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
 * A HIDDEN LAYER, SHOWN FOR A MOMENT: the hover over a shut eye.
 *
 * Isolation dims every other layer and leaves the subject at full strength --
 * which for a hidden subject is nothing, so hovering the eye of the one layer
 * you could not see faded out everything you could and showed you an empty
 * page. The peek is how strongly a hidden layer draws while its eye is under
 * the pointer (PaintDocument::IsolateLayer sets it); 0 is hidden, as before.
 *
 * View state like the dim and for the same reason: it reaches the screen and
 * nothing else. Exports, thumbnails and the eyedropper ask visible(), and a
 * peeked layer is still not visible.
 */
    void SetPeek(float peek) { m_peek = std::clamp(peek, 0.0f, 1.0f); }
    float peek() const { return m_peek; }
    bool  onScreen() const { return m_visible || m_peek > 0.0f; }
    // The view's multiplier for this layer: the dim when it is shown, the peek
    // when it is not.
    float viewStrength() const { return m_visible ? m_dim : m_peek; }

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

    /*
     * ── where a mark may land ────────────────────────────────────────────
     *
     * THE SELECTION IS A CLIP, which is the half of it that was missing. A
     * region used to be a thing you could carry and nothing else, so leaving
     * the select tool dropped it -- "a region left standing under the brush is
     * a trap: it looks like it should mask the stroke and it does not"
     * (PaintPalette::Apply, as it was). It does now, so the trap is gone and
     * the selection stays.
     *
     * Bound rather than owned: the selection is the DOCUMENT's (one per
     * picture, however many surfaces show it), and a layer is handed a pointer
     * to it on the way out of PaintDocument::activeLayer. Null, or a selection
     * with nothing in it, is "anywhere" -- so an unselected document costs one
     * null test per pixel and clearing the selection is what gives the whole
     * page back.
     */
    void BindClip(const PaintSelection* clip) { m_clip = clip; }

    bool paintable(int32_t x, int32_t y) const
    {
        return !m_clip || m_clip->empty() || m_clip->at(x, y);
    }

    void DrawPixel(int32_t x, int32_t y, float r, float g, float b, float a)
    {
        uint8_t* px = this->PixelData();
        if (!px) return;
        if (x < 0 || y < 0 ||
            static_cast<uint32_t>(x) >= this->PixelWidth() ||
            static_cast<uint32_t>(y) >= this->PixelHeight())
            return;
        // ONE PLACE FOR THE BRUSH, THE LINE AND EVERY OUTLINE, for the reason
        // DrawBrush gives: they all reduce to this.
        if (!paintable(x, y)) return;

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

    /*
     * The mark, exactly as wide as the brush says (PaintStamp).
     *
     * AND THE ONE PLACE A BLEND MEANS ANYTHING, which is why it is honoured
     * here and nowhere else: every outline in this type strokes this primitive
     * along a path (see the shapes' own comment), so an eraser that works for
     * the freehand stroke works for the line, the rect, the ellipse and the
     * star without any of them being told. DrawPixel REPLACES rather than
     * blends, so erasing is writing the transparent pixel -- no read, no
     * compositing step, nothing the mark path did not already do.
     */
    void DrawBrush(int32_t cx, int32_t cy, const PaintBrushState& brush)
    {
        const PaintStamp st = paint_stamp_of(brush.size_px, brush.tip);
        const bool lift = (brush.blend == PaintBlendMode::Erase);
        for (int dy = st.lo; dy <= st.hi; ++dy)
            for (int dx = st.lo; dx <= st.hi; ++dx)
            {
                if (!st.has(dx, dy)) continue;
                if (lift) DrawPixel(cx + dx, cy + dy, 0.0f, 0.0f, 0.0f, 0.0f);
                else      DrawPixel(cx + dx, cy + dy,
                                    brush.color.r, brush.color.g,
                                    brush.color.b, brush.color.a);
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
 * 30px brush should look like it -- at size 1 that is a one-pixel outline on
 * the dragged box exactly (PaintStamp), and at 30 it is 30 wide and spills 15
 * either side of it, which is what stroking a path means. So every outline
 * reduces to DrawBrush along
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
        // BY POINT, NOT BY INDEX, so the clip can be asked (paintable). The
        // clip is part of MATCHING and not of writing: a run that stopped at
        // the selection's edge but kept seeding past it would walk the whole
        // page to fill a corner of it.
        auto matches = [&](int32_t x, int32_t y) {
            if (!paintable(x, y)) return false;
            const size_t i = (static_cast<size_t>(y) * w + x) * 4;
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
            if (!matches(x, y)) continue;

            int32_t left = x;
            while (left > 0 && matches(left - 1, y)) --left;
            int32_t right = x;
            while (right + 1 < w && matches(right + 1, y)) ++right;

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
                    const bool m = matches(rx2, ny);
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
        // The same nib every other mark uses, so a smudge covers exactly what a
        // stroke of the same size would have (PaintStamp).
        const PaintStamp st = paint_stamp_of(brush.size_px, brush.tip);
        const float k = std::clamp(strength, 0.0f, 1.0f);
        if (k <= 0.0f) return;

        const int side = st.hi - st.lo + 1;
        std::vector<uint8_t> src(static_cast<size_t>(side) * side * 4, 0);
        for (int dy = st.lo; dy <= st.hi; ++dy)
            for (int dx = st.lo; dx <= st.hi; ++dx)
            {
                const int sxp = fx + dx, syp = fy + dy;
                if (sxp < 0 || syp < 0 || sxp >= w || syp >= h) continue;
                ::std::memcpy(&src[((static_cast<size_t>(dy - st.lo) * side) + (dx - st.lo)) * 4],
                              px + (static_cast<size_t>(syp) * w + sxp) * 4, 4);
            }

        for (int dy = st.lo; dy <= st.hi; ++dy)
            for (int dx = st.lo; dx <= st.hi; ++dx)
            {
                if (!st.has(dx, dy)) continue;
                const int dxp = tx + dx, dyp = ty + dy;
                if (dxp < 0 || dyp < 0 || dxp >= w || dyp >= h) continue;
                // The clip is on the WRITE only: a smudge may read from outside
                // the region and carry those pixels in, the way a brush picks
                // up whatever colour is loaded. What it may not do is put any
                // down outside it.
                if (!paintable(dxp, dyp)) continue;
                const uint8_t* sp = &src[((static_cast<size_t>(dy - st.lo) * side) + (dx - st.lo)) * 4];
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
        if (!surface || !onScreen()) return;

        const uint8_t* px = this->PixelData();
        if (!px) return;

        const float alpha = std::clamp(opacity, 0.0f, 1.0f) * m_opacity * viewStrength();
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
    float m_peek = 0.0f;   // see SetPeek
    // NOT OWNED. The document's selection, bound on the way out of
    // activeLayer() -- see BindClip. Null until a document hands one over,
    // which is also what a layer spawned by a script has until it is drawn on.
    const PaintSelection* m_clip = nullptr;
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

/*
 * HOW MANY BYTES A PICTURE MAY STILL TAKE, asked before any raster the size of
 * a page is allocated -- because in the browser the answer to asking too late
 * is not an exception but `Aborted(OOM)` and a dead tab. The wasm heap is
 * fixed at whatever INITIAL_MEMORY the loader was built with (loaders/Makefile
 * explains why growth is off), and the runtime's own arenas take most of it --
 * at 256 MB a resize that looked modest, 1024x768 to 1216x960 with two layers,
 * was the one that went over. Natively the heap is the OS's and a failed `new`
 * throws, so the answer is "as much as you like" and the guard reduces to the
 * side limit above.
 *
 * What is free is what the break has not reached plus what malloc has given
 * back (dlmalloc's mallinfo counts the top chunk in fordblks, so the sum is a
 * slight over-estimate, which is what the margin below is for).
 */
#if defined(__EMSCRIPTEN__)
static inline size_t paint_heap_headroom()
{
    const size_t brk = reinterpret_cast<size_t>(sbrk(0));
    const size_t top = emscripten_get_heap_max();
    const struct mallinfo mi = mallinfo();
    return (top > brk ? top - brk : 0) + static_cast<size_t>(mi.fordblks);
}
#else
static inline size_t paint_heap_headroom() { return SIZE_MAX; }
#endif

// Room for `bytes` more, keeping a margin for everything that is not a raster.
// `why` is set to a sentence a log can end with.
static inline bool paint_heap_can_take(size_t bytes, std::string& why)
{
    static constexpr size_t MARGIN = 8u << 20;
    const size_t room = paint_heap_headroom();
    if (room == SIZE_MAX || bytes + MARGIN <= room) return true;
    why = "needs " + std::to_string(bytes >> 20) + " MB and the page has "
        + std::to_string(room > MARGIN ? (room - MARGIN) >> 20 : 0) + " MB to spare";
    return false;
}

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
    // The raster, plus the layer it becomes (ImportImage copies it in).
    if (!paint_heap_can_take(static_cast<size_t>(w) * h * 8, why)) return false;

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

// Which decoder a file wants, from its first bytes rather than its name: a
// ".png" that is a JPEG is common enough to have a name for.
static inline bool paint_image_is_pam(const uint8_t* p, size_t n)
{ return n >= 2 && p[0] == 'P' && (p[1] == '7' || p[1] == '6'); }

/*
 * Any picture the tree can read, into the one in-memory shape (PaintImage).
 * PAM/PPM through this file's own parser; everything else through stb_image,
 * forced to four channels so a greyscale JPEG and an RGB PNG land as the same
 * RGBA the layers hold. The size guard is applied to both paths: a decoder that
 * will happily allocate what a lying header asks for is the harm, and stb's
 * own limit is generous.
 */
static inline bool paint_image_parse(const uint8_t* p, size_t n, PaintImage& out, std::string& why)
{
    if (paint_image_is_pam(p, n)) return paint_pam_parse(p, n, out, why);
    int w = 0, h = 0, comps = 0;
    if (!stbi_info_from_memory(p, static_cast<int>(n), &w, &h, &comps))
    {
        why = std::string("not a picture this reader knows (") + stbi_failure_reason()
            + "); accepted: PNG, JPEG, BMP, GIF, TGA, PAM (P7), PPM (P6)";
        return false;
    }
    if (w <= 0 || h <= 0 || static_cast<uint32_t>(w) > PAINT_IMAGE_MAX_SIDE
        || static_cast<uint32_t>(h) > PAINT_IMAGE_MAX_SIDE)
    { why = "refusing " + std::to_string(w) + "x" + std::to_string(h) + " -- a side over "
          + std::to_string(PAINT_IMAGE_MAX_SIDE) + " is not a picture anyone paints on here"; return false; }
    // stb's decode, the copy into `out`, and the layer it becomes: three rasters
    // at once at the peak, and asked for before the first is allocated.
    if (!paint_heap_can_take(static_cast<size_t>(w) * h * 12, why)) return false;
    unsigned char* px = stbi_load_from_memory(p, static_cast<int>(n), &w, &h, &comps, 4);
    if (!px) { why = std::string("decode failed: ") + stbi_failure_reason(); return false; }
    out.w = static_cast<uint32_t>(w); out.h = static_cast<uint32_t>(h);
    out.rgba.assign(px, px + static_cast<size_t>(w) * h * 4);
    stbi_image_free(px);
    return true;
}

/*
 * ── GIF, both ways ───────────────────────────────────────────────────────
 *
 * IN through stb_image, which already reads a GIF's frames and their delays
 * (stbi_load_gif_from_memory) -- an animated GIF is the one file format that
 * carries a frame sequence and a rate, so it is how frames arrive from
 * outside (PaintAnimation::ImportGif). OUT through the encoder below, because
 * stb_image_write has none and a frame sequence that can be brought in but
 * not taken out is half a feature.
 *
 * THE ENCODER IS THE SMALL ONE: one global 256-colour table for the whole
 * sequence, found by median cut over a sample of every frame's pixels, and
 * plain LZW at up to 12 bits, which is the format's own ceiling. No per-frame
 * tables, no transparency, no inter-frame difference: every frame is written
 * whole. That is more bytes than a good encoder writes and a fraction of the
 * code, and what a paint program's sprite sheet needs is the file to be a
 * correct GIF that every viewer plays, not a small one.
 */
namespace paint_gif {

struct Box { uint8_t lo[3], hi[3]; std::vector<uint32_t> px; };

// Median cut: split the widest axis of the box with the most pixels until
// there are `want` boxes; each box's average is a palette entry.
static inline void median_cut(std::vector<uint32_t> sample, size_t want, std::vector<uint32_t>& palette)
{
    palette.clear();
    if (sample.empty()) { palette.push_back(0); return; }
    std::vector<Box> boxes(1);
    boxes[0].px = std::move(sample);
    auto bounds = [](Box& b)
    {
        b.lo[0] = b.lo[1] = b.lo[2] = 255; b.hi[0] = b.hi[1] = b.hi[2] = 0;
        for (uint32_t c : b.px)
            for (int k = 0; k < 3; ++k)
            {
                const uint8_t v = static_cast<uint8_t>(c >> (16 - 8 * k));
                b.lo[k] = std::min(b.lo[k], v); b.hi[k] = std::max(b.hi[k], v);
            }
    };
    bounds(boxes[0]);
    while (boxes.size() < want)
    {
        // The box to split: the most pixels among those that can still split.
        size_t at = boxes.size(); size_t most = 1;
        for (size_t i = 0; i < boxes.size(); ++i)
        {
            const Box& b = boxes[i];
            const int span = std::max({ b.hi[0] - b.lo[0], b.hi[1] - b.lo[1], b.hi[2] - b.lo[2] });
            if (span > 0 && b.px.size() > most) { most = b.px.size(); at = i; }
        }
        if (at == boxes.size()) break;
        Box& b = boxes[at];
        int axis = 0; int span = b.hi[0] - b.lo[0];
        for (int k = 1; k < 3; ++k) if (b.hi[k] - b.lo[k] > span) { span = b.hi[k] - b.lo[k]; axis = k; }
        const int shift = 16 - 8 * axis;
        std::sort(b.px.begin(), b.px.end(), [shift](uint32_t a, uint32_t c)
                  { return ((a >> shift) & 255) < ((c >> shift) & 255); });
        Box other;
        const size_t mid = b.px.size() / 2;
        other.px.assign(b.px.begin() + mid, b.px.end());
        b.px.resize(mid);
        bounds(b); bounds(other);
        boxes.push_back(std::move(other));
    }
    for (const Box& b : boxes)
    {
        uint64_t r = 0, g = 0, bl = 0;
        for (uint32_t c : b.px) { r += (c >> 16) & 255; g += (c >> 8) & 255; bl += c & 255; }
        const size_t n = std::max<size_t>(1, b.px.size());
        palette.push_back((static_cast<uint32_t>(r / n) << 16) | (static_cast<uint32_t>(g / n) << 8)
                          | static_cast<uint32_t>(bl / n));
    }
}

// Nearest palette entry, cached on the top five bits of each channel: a
// 1024x768 frame asks this 786k times and the cache answers most of them.
struct Mapper
{
    const std::vector<uint32_t>& pal;
    std::vector<int16_t> cache;
    explicit Mapper(const std::vector<uint32_t>& p) : pal(p), cache(32 * 32 * 32, -1) {}
    uint8_t operator()(uint8_t r, uint8_t g, uint8_t b)
    {
        const size_t key = (static_cast<size_t>(r >> 3) << 10) | (static_cast<size_t>(g >> 3) << 5) | (b >> 3);
        if (cache[key] >= 0) return static_cast<uint8_t>(cache[key]);
        int best = 0; int64_t bd = INT64_MAX;
        for (size_t i = 0; i < pal.size(); ++i)
        {
            const int dr = static_cast<int>((pal[i] >> 16) & 255) - r;
            const int dg = static_cast<int>((pal[i] >> 8) & 255) - g;
            const int db = static_cast<int>(pal[i] & 255) - b;
            const int64_t d = static_cast<int64_t>(dr) * dr + static_cast<int64_t>(dg) * dg + static_cast<int64_t>(db) * db;
            if (d < bd) { bd = d; best = static_cast<int>(i); }
        }
        cache[key] = static_cast<int16_t>(best);
        return static_cast<uint8_t>(best);
    }
};

// LZW with a variable code width, the GIF flavour: clear and end codes after
// the alphabet, the table reset at 4096. Bits are packed least-significant
// first and the stream is cut into sub-blocks of at most 255 bytes.
struct LzwOut
{
    std::vector<uint8_t>& out;
    std::vector<uint8_t> block;
    uint32_t acc = 0; int bits = 0;
    explicit LzwOut(std::vector<uint8_t>& o) : out(o) {}
    void code(uint32_t c, int width)
    {
        acc |= c << bits; bits += width;
        while (bits >= 8) { byte(static_cast<uint8_t>(acc & 255)); acc >>= 8; bits -= 8; }
    }
    void byte(uint8_t b) { block.push_back(b); if (block.size() == 255) flush(); }
    void flush()
    {
        if (block.empty()) return;
        out.push_back(static_cast<uint8_t>(block.size()));
        out.insert(out.end(), block.begin(), block.end());
        block.clear();
    }
    void finish() { if (bits > 0) byte(static_cast<uint8_t>(acc & 255)); flush(); out.push_back(0); }
};

static inline void lzw_encode(const std::vector<uint8_t>& idx, std::vector<uint8_t>& out)
{
    constexpr int MIN = 8;
    constexpr uint32_t CLEAR = 1u << MIN, END = CLEAR + 1;
    out.push_back(MIN);
    LzwOut w(out);
    // prefix code * 256 + next byte -> code; 4096 * 256 entries of int16.
    std::vector<int16_t> table(4096 * 256);
    auto reset = [&]() { std::fill(table.begin(), table.end(), -1); };
    reset();
    uint32_t next = END + 1; int width = MIN + 1;
    w.code(CLEAR, width);
    if (idx.empty()) { w.code(END, width); w.finish(); return; }
    uint32_t prefix = idx[0];
    for (size_t i = 1; i < idx.size(); ++i)
    {
        const uint8_t c = idx[i];
        const size_t key = static_cast<size_t>(prefix) * 256 + c;
        if (table[key] >= 0) { prefix = static_cast<uint32_t>(table[key]); continue; }
        w.code(prefix, width);
        if (next < 4096)
        {
            table[key] = static_cast<int16_t>(next);
            if (next == (1u << width) && width < 12) ++width;
            ++next;
        }
        else
        {
            w.code(CLEAR, width);
            reset();
            next = END + 1; width = MIN + 1;
        }
        prefix = c;
    }
    w.code(prefix, width);
    w.code(END, width);
    w.finish();
}

// The whole file: frames of one size, a delay per frame in hundredths of a
// second, looping forever.
static inline bool encode(const std::vector<PaintImage>& frames, uint32_t delay_cs,
                          std::vector<uint8_t>& out, std::string& why)
{
    if (frames.empty()) { why = "no frames"; return false; }
    const uint32_t w = frames[0].w, h = frames[0].h;
    if (w == 0 || h == 0 || w > 65535 || h > 65535) { why = "bad frame size"; return false; }
    for (const PaintImage& f : frames)
        if (f.w != w || f.h != h || f.rgba.size() < static_cast<size_t>(w) * h * 4)
        { why = "frames differ in size"; return false; }

    // The palette, from every frame, sampled so a long sequence does not
    // cost a sort of every pixel it has.
    std::vector<uint32_t> sample;
    {
        const size_t total = static_cast<size_t>(w) * h * frames.size();
        const size_t step = std::max<size_t>(1, total / 65536);
        size_t k = 0;
        for (const PaintImage& f : frames)
            for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i, ++k)
                if (k % step == 0)
                {
                    const uint8_t* p = f.rgba.data() + i * 4;
                    sample.push_back((static_cast<uint32_t>(p[0]) << 16) | (static_cast<uint32_t>(p[1]) << 8) | p[2]);
                }
    }
    std::vector<uint32_t> palette;
    median_cut(std::move(sample), 256, palette);
    Mapper map(palette);

    out.clear();
    auto u16 = [&](uint32_t v) { out.push_back(static_cast<uint8_t>(v & 255)); out.push_back(static_cast<uint8_t>((v >> 8) & 255)); };
    const char* sig = "GIF89a";
    out.insert(out.end(), sig, sig + 6);
    u16(w); u16(h);
    out.push_back(0xF7);            // global table, 8 bits per colour, 256 entries
    out.push_back(0); out.push_back(0);
    for (size_t i = 0; i < 256; ++i)
    {
        const uint32_t c = i < palette.size() ? palette[i] : 0;
        out.push_back(static_cast<uint8_t>((c >> 16) & 255));
        out.push_back(static_cast<uint8_t>((c >> 8) & 255));
        out.push_back(static_cast<uint8_t>(c & 255));
    }
    // Loop forever (the Netscape extension every viewer honours).
    const uint8_t loop[] = { 0x21, 0xFF, 0x0B, 'N','E','T','S','C','A','P','E','2','.','0', 0x03, 0x01, 0x00, 0x00, 0x00 };
    out.insert(out.end(), loop, loop + sizeof(loop));

    std::vector<uint8_t> idx(static_cast<size_t>(w) * h);
    for (const PaintImage& f : frames)
    {
        const uint8_t gce[] = { 0x21, 0xF9, 0x04, 0x00 };
        out.insert(out.end(), gce, gce + 4);
        u16(delay_cs); out.push_back(0); out.push_back(0);
        out.push_back(0x2C); u16(0); u16(0); u16(w); u16(h); out.push_back(0);
        for (size_t i = 0; i < idx.size(); ++i)
        {
            const uint8_t* p = f.rgba.data() + i * 4;
            idx[i] = map(p[0], p[1], p[2]);
        }
        lzw_encode(idx, out);
    }
    out.push_back(0x3B);
    return true;
}

// The frames of a GIF, and its rate as the mean delay in milliseconds. A
// still GIF is one frame; a file that is not a GIF at all is refused with the
// decoder's reason.
static inline bool decode(const uint8_t* bytes, size_t n, std::vector<PaintImage>& frames,
                          int& delay_ms, std::string& why)
{
    int* delays = nullptr; int w = 0, h = 0, z = 0, comp = 0;
    stbi_uc* px = stbi_load_gif_from_memory(bytes, static_cast<int>(n), &delays, &w, &h, &z, &comp, 4);
    if (!px) { why = std::string("decode failed: ") + stbi_failure_reason(); return false; }
    frames.clear();
    const size_t per = static_cast<size_t>(w) * h * 4;
    int64_t sum = 0;
    for (int i = 0; i < z; ++i)
    {
        PaintImage f; f.w = static_cast<uint32_t>(w); f.h = static_cast<uint32_t>(h);
        f.rgba.assign(px + per * i, px + per * (i + 1));
        frames.push_back(std::move(f));
        if (delays) sum += delays[i];
    }
    delay_ms = (z > 0 && sum > 0) ? static_cast<int>(sum / z) : 100;
    stbi_image_free(px);
    if (delays) stbi_image_free(delays);
    return !frames.empty();
}

} // namespace paint_gif

// How many frames a file has as a GIF: 0 for a file that is not one (by its
// magic, so nothing else is decoded), else the count. Decodes the whole GIF
// to answer, which is the price of stb's one entry point; a reel is decoded
// again on import, and a sprite sheet is small.
static inline size_t paint_gif_frames_in(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return 0;
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() < 6 || std::memcmp(bytes.data(), "GIF8", 4) != 0) return 0;
    std::vector<PaintImage> frames; int delay = 0; std::string why;
    if (!paint_gif::decode(bytes.data(), bytes.size(), frames, delay, why)) return 0;
    return frames.size();
}

static inline bool paint_image_read(const std::string& path, PaintImage& out, std::string& why)
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
    return paint_image_parse(bytes.data(), bytes.size(), out, why);
}

// By EXTENSION on the way out, because a file being written has no bytes to
// sniff and its name is the one thing the caller said. ".png" is PNG; anything
// else is PAM, which is what the export always was.
static inline bool paint_image_write(const std::string& path,
                                     const uint8_t* rgba, uint32_t w, uint32_t h, std::string& why);

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

static inline bool paint_image_write(const std::string& path,
                                     const uint8_t* rgba, uint32_t w, uint32_t h, std::string& why)
{
    const size_t dot = path.rfind('.');
    std::string ext = (dot == std::string::npos) ? "" : path.substr(dot + 1);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext != "png") return paint_pam_write(path, rgba, w, h, why);
    if (!rgba || w == 0 || h == 0) { why = "nothing to write"; return false; }
    // Stride w*4: the layers are packed. Not to a memory buffer first -- the
    // file IS the destination on both substrates.
    if (!stbi_write_png(path.c_str(), static_cast<int>(w), static_cast<int>(h), 4, rgba,
                        static_cast<int>(w) * 4))
    { why = "PNG write to " + path + " failed"; return false; }
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
 * ── FONTS FOR TEXT BOXES ─────────────────────────────────────────────────────
 *
 * A Glyphs provider with more than one face. Font 0 is the pixel font the rest
 * of the sheet is lettered in (RenderProvider::TextLabel, bound with
 * BindPixel), kept because it is the look this program already has; fonts 1
 * and up are TrueType files loaded by path (Load), measured and drawn with
 * stb_truetype at any size, antialiased.
 *
 * FILES, NOT THE BROWSER'S FONTS. A text box in a shared session wraps where
 * the box's width says, and every page in the room has to wrap it at the same
 * words -- which only holds if every page measures with the same outlines. A
 * font from the operating system or the browser would be a different font on
 * each machine. So the faces ship with the program (PaintProvider/fonts, each
 * beside its licence) and are staged like the scripts are.
 *
 * SIZE IS THE LINE: size_px is the height from the highest ascender to the
 * lowest descender, the same meaning the pixel font's size has, so switching a
 * box's font keeps its lines about as tall as they were.
 *
 * Glyph bitmaps are cached per font, size and character: a text box is drawn
 * again on every render of the view, which is every stroke's sample.
 */
class PaintFonts : public GlyphsBase<PaintFonts>,
                   public DeletableBase<PaintFonts>
{
public:
    WIRE_TYPE_IDENTITY(PaintFonts);

    PaintFonts() = default;
    bool DeleteConcrete() override { return true; }

    bool Create() { this->addTag("active"); return true; }

    // Font 0: whatever Glyphs leaf draws the sheet's own lettering.
    void BindPixel(ETCS::RID glyphs) { m_pixel = glyphs; }

    /*
     * A FONT THAT DID NOT LOAD STILL TAKES ITS NUMBER. The number is what a box
     * stores and what the bar's buttons name, so a missing file must not shift
     * every font after it onto the wrong face -- the slot is kept, and a box in
     * it is drawn in the pixel font until the file is there.
     */
    bool Load(const std::string& name, const std::string& path)
    {
        auto face = std::make_unique<Face>();
        face->name = name;
        std::ifstream in(path, std::ios::binary);
        if (!in)
            ETCS_LOG("PaintFonts", "Load " << name << ": cannot open '" << path
                     << "' -- font " << (m_faces.size() + 1) << " draws in the pixel font.");
        else
        {
            face->bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            const int offset = face->bytes.empty() ? -1 : stbtt_GetFontOffsetForIndex(face->bytes.data(), 0);
            face->loaded = offset >= 0 && stbtt_InitFont(&face->info, face->bytes.data(), offset);
            if (face->loaded) stbtt_GetFontVMetrics(&face->info, &face->ascent, &face->descent, &face->gap);
            else ETCS_LOG("PaintFonts", "Load " << name << ": '" << path << "' is not a font stb_truetype can read"
                          " -- font " << (m_faces.size() + 1) << " draws in the pixel font.");
        }
        const bool ok = face->loaded;
        std::lock_guard<std::mutex> g(m_mu);
        m_faces.push_back(std::move(face));
        if (ok) ETCS_LOG("PaintFonts", "font " << m_faces.size() << " '" << name << "' from '" << path << "'.");
        return ok;
    }

    uint32_t count() const { return static_cast<uint32_t>(m_faces.size()) + 1; }
    std::string nameOf(uint32_t font) const
    {
        if (font == 0 || font > m_faces.size()) return "pixel";
        return m_faces[font - 1]->name;
    }

    void Report() const
    {
        ETCS_LOG("PaintFonts", "0 pixel" << (m_pixel ? "" : " (unbound)"));
        for (size_t i = 0; i < m_faces.size(); ++i)
            ETCS_LOG("PaintFonts", (i + 1) << " " << m_faces[i]->name << (m_faces[i]->loaded ? "" : " (not loaded: pixel font)"));
    }

    TextExtent MeasureTextConcrete(const char* text, uint32_t font, uint32_t size_px) override
    {
        Face* f = face(font);
        if (!f) return pixel_measure(text, size_px);
        const float scale = stbtt_ScaleForPixelHeight(&f->info, static_cast<float>(std::max<uint32_t>(1, size_px)));
        float w = 0.0f;
        int prev = 0;
        for (const char* c = text ? text : ""; *c; ++c)
        {
            const int cp = static_cast<unsigned char>(*c);
            int adv = 0, lsb = 0;
            stbtt_GetCodepointHMetrics(&f->info, cp, &adv, &lsb);
            if (prev) w += scale * stbtt_GetCodepointKernAdvance(&f->info, prev, cp);
            w += scale * adv;
            prev = cp;
        }
        const float asc = f->ascent * scale, desc = -f->descent * scale;
        return TextExtent{ static_cast<uint32_t>(std::ceil(w)),
                           static_cast<uint32_t>(std::ceil(asc + desc)),
                           static_cast<uint32_t>(std::ceil(asc)) };
    }

    TextExtent RasterizeTextConcrete(ETCS::RID target, const char* text, uint32_t font, uint32_t size_px,
                                     int32_t x, int32_t y, float r, float g, float b, float a) override
    {
        Face* f = face(font);
        if (!f)
        {
            ETCS::Held<Glyphs_> px = ETCS::resolve_held<Glyphs_>("Glyphs", m_pixel);
            if (!px) return TextExtent{ 0, 0, 0 };
            return px->RasterizeText(target, text, 0, size_px, x, y, r, g, b, a);
        }
        const TextExtent e = MeasureTextConcrete(text, font, size_px);
        Pixels_* dst = ETCS::resolve_in_family<Pixels_>("Pixels", target);
        if (!dst || !dst->PixelData() || !text) return e;
        const float scale = stbtt_ScaleForPixelHeight(&f->info, static_cast<float>(std::max<uint32_t>(1, size_px)));
        const int32_t base = y + static_cast<int32_t>(e.baseline);
        float pen = static_cast<float>(x);
        int prev = 0;
        std::lock_guard<std::mutex> lock(m_mu);
        for (const char* c = text; *c; ++c)
        {
            const int cp = static_cast<unsigned char>(*c);
            if (prev) pen += scale * stbtt_GetCodepointKernAdvance(&f->info, prev, cp);
            const Glyph& gl = glyph(*f, font, size_px, scale, cp);
            blend(*dst, gl, static_cast<int32_t>(std::lround(pen)) + gl.xoff, base + gl.yoff, r, g, b, a);
            pen += gl.advance;
            prev = cp;
        }
        etcs_mark_observed(static_cast<ETCS::Entity*>(dst));
        return e;
    }

private:
    struct Face
    {
        std::string name;
        std::vector<unsigned char> bytes;     // stb_truetype reads the file in place
        bool loaded = false;                  // a kept slot for a file that was not there
        stbtt_fontinfo info{};
        int ascent = 0, descent = 0, gap = 0;
    };
    struct Glyph
    {
        int32_t w = 0, h = 0, xoff = 0, yoff = 0;
        float advance = 0.0f;
        std::vector<uint8_t> cover;
    };

    Face* face(uint32_t font) const
    {
        if (font == 0 || font > m_faces.size()) return nullptr;
        Face* f = m_faces[font - 1].get();
        return f->loaded ? f : nullptr;
    }

    TextExtent pixel_measure(const char* text, uint32_t size_px)
    {
        ETCS::Held<Glyphs_> px = ETCS::resolve_held<Glyphs_>("Glyphs", m_pixel);
        if (!px) return TextExtent{ 0, 0, 0 };
        return px->MeasureText(text, 0, size_px);
    }

    const Glyph& glyph(Face& f, uint32_t font, uint32_t size_px, float scale, int cp)
    {
        const uint64_t key = (static_cast<uint64_t>(font) << 48) | (static_cast<uint64_t>(size_px) << 24)
                           | static_cast<uint64_t>(cp);
        auto it = m_cache.find(key);
        if (it != m_cache.end()) return it->second;
        if (m_cache.size() > 8192) m_cache.clear();   // a bound, not a policy: sizes come and go
        Glyph gl;
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&f.info, cp, &adv, &lsb);
        gl.advance = adv * scale;
        int w = 0, h = 0, xo = 0, yo = 0;
        unsigned char* bm = stbtt_GetCodepointBitmap(&f.info, scale, scale, cp, &w, &h, &xo, &yo);
        if (bm)
        {
            gl.w = w; gl.h = h; gl.xoff = xo; gl.yoff = yo;
            gl.cover.assign(bm, bm + static_cast<size_t>(w) * h);
            stbtt_FreeBitmap(bm, nullptr);
        }
        return m_cache.emplace(key, std::move(gl)).first->second;
    }

    // Source-over, coverage times the colour's alpha, into straight RGBA --
    // the blend every raster in this module uses.
    static void blend(Pixels_& dst, const Glyph& gl, int32_t x0, int32_t y0, float r, float g, float b, float a)
    {
        uint8_t* px = dst.PixelData();
        const int32_t W = static_cast<int32_t>(dst.PixelWidth()), H = static_cast<int32_t>(dst.PixelHeight());
        for (int32_t gy = 0; gy < gl.h; ++gy)
        {
            const int32_t ty = y0 + gy;
            if (ty < 0 || ty >= H) continue;
            for (int32_t gx = 0; gx < gl.w; ++gx)
            {
                const int32_t tx = x0 + gx;
                if (tx < 0 || tx >= W) continue;
                const float sa = a * (gl.cover[static_cast<size_t>(gy) * gl.w + gx] / 255.0f);
                if (sa <= 0.0f) continue;
                uint8_t* d = px + (static_cast<size_t>(ty) * W + tx) * 4;
                const float da = d[3] / 255.0f;
                const float oa = sa + da * (1.0f - sa);
                auto mix = [&](float sc, uint8_t dc)
                { return paint_to_byte((sc * sa + (dc / 255.0f) * da * (1.0f - sa)) / oa); };
                d[0] = mix(r, d[0]); d[1] = mix(g, d[1]); d[2] = mix(b, d[2]);
                d[3] = paint_to_byte(oa);
            }
        }
    }

    ETCS::RID m_pixel = 0;
    std::vector<std::unique_ptr<Face>> m_faces;
    std::unordered_map<uint64_t, Glyph> m_cache;
    std::mutex m_mu;
};


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
 * THE BOX IS THE COLUMN, THE SIZE IS THE TYPE. Text is set in the box's font at
 * its size and wraps where the box's width runs out -- at a space when there is
 * one, inside a word that is wider than the whole box -- and Enter starts a new
 * line. Lines past the bottom of the box are not drawn; drag the corner to make
 * room (PaintInput, the resize handle). Font, size and colour are the box's own,
 * set from the bar that opens over it while it is selected (PaintTextBar).
 */
struct PaintTextBox
{
    int32_t     x = 0, y = 0;       // document space, top-left
    int32_t     w = 1, h = 1;
    std::string text;
    uint32_t    id = 0;             // stable across edits, unlike an index
    /*
     * ITS NAME IN A SHARED SESSION. The id is this document's own counter and
     * means nothing on another page, so a box also carries a key that does:
     * who made it and their id for it ("alice.3"; "-.3" for one made before
     * the page was shared). Every page in the room names the box by it.
     */
    std::string key;
    // The colour it was placed with. On the box rather than read from the tool at
    // draw time, because the tool's colour moves on and this text should not: two
    // captions placed with different colours stay different.
    float       rgba[4] = { 0.08f, 0.08f, 0.10f, 1.0f };
    uint32_t    font = 0;           // a Glyphs font handle -- see PaintFonts
    uint32_t    size = 24;          // the line's height, in document pixels

    bool same_as(const PaintTextBox& o) const
    {
        return x == o.x && y == o.y && w == o.w && h == o.h && text == o.text
            && font == o.font && size == o.size
            && rgba[0] == o.rgba[0] && rgba[1] == o.rgba[1]
            && rgba[2] == o.rgba[2] && rgba[3] == o.rgba[3];
    }
};

/*
 * ── THE NOTEBOOK ─────────────────────────────────────────────────────────────
 *
 * WHAT HAPPENED, IN ORDER, RE-EXECUTABLE.
 *
 * The three-deep snapshot store this replaces said of itself, from the day it
 * was written, that it was a placeholder: "the real history comes from the
 * persistence tag: the input event stream is itself the record of what happened,
 * and undo will be a replay of it -- unbounded, and kept across sessions once
 * saving is in." That is this, and the seam it named -- Remember(), called by
 * every committed change before it lands -- is still the seam. No call site
 * moved.
 *
 * ONE OBJECT, FOUR JOBS, which is the argument for building it now rather than
 * building a fourth thing beside it:
 *
 *   undo/redo   walk the log
 *   sharing     a viewer replays it; a late one replays from a snapshot
 *   saving      a document is its notebook
 *   autosave    the head sequence is the only dirty check anyone needs
 *
 * AN ENTRY HAS A LIFETIME, unlike the snapshot it replaces. Remember() fires at
 * BeginStroke -- deliberately, so an anchored preview that commits nothing does
 * not spend a snapshot -- but a freehand stroke's content accrues through motion
 * afterwards and is complete only at release. So an entry is opened at the seam,
 * appended to while the stroke runs, and sealed at the release. An entry left
 * open by a lost release is sealed by the next seam rather than discarded: the
 * ink is on the layer either way, and a notebook that disagrees with the pixels
 * is worse than a slightly ragged stroke.
 *
 * WHY POINTS AND NOT SAMPLES. A Dab records every point ApplyBrush was actually
 * called with, interpolation included, rather than the raw pointer samples plus
 * a spacing rule. Replay is then the same calls in the same order and cannot
 * drift: re-deriving the spacing at replay time would make the viewer's copy a
 * function of PaintInput::apply_segment's arithmetic, which is exactly the kind
 * of agreement that holds until one side is edited. It costs more points than a
 * sample list -- a one-pixel nib over a thousand pixels is a thousand of them --
 * and that is the price of the guarantee.
 *
 * WHAT IS NOT DESCRIBED gets a Snapshot, and that is not an admission of
 * defeat: a paste carries arbitrary pixels, a lift is a hole plus a floating
 * buffer, and there is no compact descriptor for either that is not just the
 * bytes. Remember() with no describing call in front of it MEANS Snapshot, so a
 * change nobody taught the notebook about is recorded correctly rather than
 * silently missed -- the failure mode of the alternative.
 */
enum class PaintOpKind : uint8_t
{
    Snapshot,   // whole-layer bytes; the only entry that restores without replay
    Dab,        // freehand: brush + every point it was stamped at
    Line, Rect, Ellipse,
    Poly,       // a shape mode's vertex ring, stroked closed
    Fill,
    /*
     * THE LAYER SET ITSELF, and adding it is what makes a merge undoable.
     *
     * Everything above is a change to PIXELS. A merge, a delete and a new layer
     * are changes to STRUCTURE, and a notebook that records only the first kind
     * can put the paint back and not the plane it was on -- which is why a merge
     * used to be a one-way door with a keyframe in front of it.
     *
     * This carries the roster AFTER the change and no rasters: the layers that
     * are about to be disturbed get ordinary keyframes immediately before it, so
     * the bytes to bring one back are already on the chain. Metadata only, so a
     * structural entry costs nothing next to the snapshots around it.
     */
    Layers,
    /*
     * THE WHOLE PAGE, stated rather than changed: its extent and its stack. It
     * begins a baseline (PaintDocument::ExportBaseline) -- one of these, then a
     * keyframe per layer -- and a document that takes one in BECOMES that page
     * (AcceptOp): same size, same layers, nothing else. Never kept in a
     * notebook; it is what a notebook starts from.
     */
    Page,
    /*
     * ONE TEXT BOX, AS IT STANDS AFTER AN EDIT -- or its removal. A box is a
     * string and a place, not pixels, so the entry carries the whole box and
     * replay folds these along the path (PaintDocument::replayTo): the boxes a
     * point in history has are the last word each entry said about its key.
     * One entry per edit, written when the edit ends (SelectTextBox), so undo
     * takes back a whole edit -- the typing, the font, the move -- in one step.
     */
    Text,
    /*
     * AN UNDO IS AN EDGE, NOT A WALK. In a shared session every member's
     * picture is a function of one record, so taking a stroke back has to be
     * something the record SAYS, replayed by everyone the same way -- not a
     * cursor moving in one page's own tree, which the room could only learn
     * about by being re-baselined with that page's whole picture (which is
     * what it used to do, and what wiped everyone else's unsent strokes).
     *
     * Names its target by (author, ordinal): the k-th marking entry that
     * author made, counted along the record. Never by sequence number -- a
     * page numbers its own entries locally and the node renumbers them on the
     * way in, so a sequence means nothing past the page that wrote it. Order
     * is the identity, and the order of one author's entries is the same on
     * every member (PaintNotebook's own rule, one level up).
     *
     * Redo is the same edge in reverse. Both are recorded, so a retraction and
     * its retraction are history like everything else, and "undo, then redo"
     * and "never touched" are different records of the same picture.
     */
    Undo,
    Redo,
};

static inline const char* paint_op_name(PaintOpKind k)
{
    switch (k)
    {
    case PaintOpKind::Snapshot: return "snap";
    case PaintOpKind::Dab:      return "dab";
    case PaintOpKind::Line:     return "line";
    case PaintOpKind::Rect:     return "rect";
    case PaintOpKind::Ellipse:  return "ellipse";
    case PaintOpKind::Poly:     return "poly";
    case PaintOpKind::Fill:     return "fill";
    case PaintOpKind::Layers:   return "layers";
    case PaintOpKind::Page:     return "page";
    case PaintOpKind::Text:     return "text";
    case PaintOpKind::Undo:     return "undo";
    case PaintOpKind::Redo:     return "redo";
    }
    return "snap";
}

static inline PaintOpKind paint_op_from(const std::string& s)
{
    if (s == "dab")     return PaintOpKind::Dab;
    if (s == "line")    return PaintOpKind::Line;
    if (s == "rect")    return PaintOpKind::Rect;
    if (s == "ellipse") return PaintOpKind::Ellipse;
    if (s == "poly")    return PaintOpKind::Poly;
    if (s == "fill")    return PaintOpKind::Fill;
    if (s == "layers")  return PaintOpKind::Layers;
    if (s == "page")    return PaintOpKind::Page;
    if (s == "text")    return PaintOpKind::Text;
    if (s == "undo")    return PaintOpKind::Undo;
    if (s == "redo")    return PaintOpKind::Redo;
    return PaintOpKind::Snapshot;
}

/*
 * ── base64, because a snapshot has to travel as a line ───────────────────────
 *
 * The notebook's wire form is one entry per line, so a viewer can parse what it
 * has without waiting for the rest -- and a line cannot hold a NUL, which a PNG
 * begins with. Sixty-four characters is the price of that, and it is the same
 * price every other line-oriented transport pays.
 *
 * NOT A GENERAL CODEC. No wrapping, no whitespace tolerance beyond what a
 * splitter already removed: this encodes what this decodes and nothing else is
 * ever handed to it.
 */
static inline const char* paint_b64_alphabet()
{
    return "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
}

static inline std::string paint_b64_encode(const uint8_t* p, size_t n)
{
    const char* A = paint_b64_alphabet();
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < n; i += 3)
    {
        const uint32_t v = (uint32_t(p[i]) << 16) | (uint32_t(p[i + 1]) << 8) | p[i + 2];
        out += A[(v >> 18) & 63]; out += A[(v >> 12) & 63];
        out += A[(v >>  6) & 63]; out += A[v & 63];
    }
    if (i + 1 == n)
    {
        const uint32_t v = uint32_t(p[i]) << 16;
        out += A[(v >> 18) & 63]; out += A[(v >> 12) & 63]; out += "==";
    }
    else if (i + 2 == n)
    {
        const uint32_t v = (uint32_t(p[i]) << 16) | (uint32_t(p[i + 1]) << 8);
        out += A[(v >> 18) & 63]; out += A[(v >> 12) & 63]; out += A[(v >> 6) & 63]; out += '=';
    }
    return out;
}

static inline bool paint_b64_decode(const std::string& s, std::vector<uint8_t>& out)
{
    int8_t rev[256];
    std::memset(rev, -1, sizeof(rev));
    const char* A = paint_b64_alphabet();
    for (int k = 0; k < 64; ++k) rev[static_cast<uint8_t>(A[k])] = static_cast<int8_t>(k);

    out.clear();
    out.reserve(s.size() / 4 * 3);
    uint32_t acc = 0;
    int bits = 0;
    for (char ch : s)
    {
        if (ch == '=') break;
        const int8_t v = rev[static_cast<uint8_t>(ch)];
        if (v < 0) return false;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
        }
    }
    return true;
}

/*
 * ONE ENTRY. The brush is carried by VALUE rather than read from the tool at
 * replay time, and that is the whole difference between a log and a rumour: the
 * tool has moved on by the time anything replays this, and on a viewer's machine
 * it was never the same tool.
 *
 * `layer` is a RID and is resolved at replay, not held: a layer can be
 * reordered, renamed or deleted between the entry and its use, and only the last
 * of those is a problem -- which is the same rule the snapshot store already
 * used and the reason it took the RID rather than an index.
 */
struct PaintOp
{
    uint64_t    seq   = 0;
    /*
     * THE ENTRY THIS ONE WAS MADE AFTER, which is what makes the notebook a
     * TREE rather than a list with a cursor on it.
     *
     * A session that never undoes has parent == the previous entry for every
     * one of them, so the tree is a line and nothing about it is visible.
     * Undo moves the cursor to a parent; a change made while the cursor is
     * behind the head becomes a SECOND CHILD instead of erasing the future,
     * and redo picks the most recent child. That is the whole of branching,
     * and it replaced a separate `m_redo_from` high-water mark -- "is there
     * anything to redo" is now "does the cursor have children", which is the
     * same question asked of the structure instead of of a second variable
     * that had to be kept in step with it.
     *
     * LOCAL, AND NEVER ON THE WIRE. A node renumbers every entry it accepts,
     * because two writers numbering their own would collide -- so a parent in
     * this runtime's numbering means nothing after renumbering, and carrying
     * it would make the node understand a field it otherwise copies blind.
     * Branches are a property of YOUR editing history; what a session replays
     * is the canonical path and only that (PaintDocument::ExportOps).
     */
    uint64_t    parent = 0;
    PaintOpKind kind  = PaintOpKind::Snapshot;
    /*
     * WHICH LAYER, SAID TWICE, because the two readers of this entry are not
     * on the same machine. A RID is this runtime's own name for the layer and
     * is exact here -- undo resolves by it and gets the layer it recorded. It
     * is meaningless on a VIEWER, whose layers were spawned separately and
     * carry entirely different RIDs, so the entry also carries the layer's
     * ORDER: the document's own stable, shared name for a position in the
     * stack. ApplyOp tries the RID, falls back to the order, and falls back
     * again to the active layer -- exact locally, correct remotely, and never
     * silently dropping a mark on the floor.
     */
    ETCS::RID   layer = 0;
    int32_t     order = 0;
    // Who made it. Empty is "this page", which is what a session with nobody
    // else in it writes and what a local-only document keeps writing forever.
    std::string author;

    PaintBrushState brush;
    uint32_t        tolerance = 0;          // Fill only

    // x,y pairs. A Dab's path; a Line/Rect/Ellipse's two corners; a Poly's ring;
    // a Fill's one seed point.
    std::vector<int32_t> pts;

    // Snapshot only: the layer's bytes and the extent they are for. Checked at
    // restore rather than trusted -- a snapshot taken before a resize must not
    // be written over a buffer of another size, which is the check RestoreBytes
    // has always made and the reason a stale entry is dropped rather than fatal.
    std::vector<uint8_t> bytes;
    uint32_t w = 0, h = 0;

    // Layers only: what the stack looks like after this entry. ORDER IS THE
    // IDENTITY here, not the RID -- a layer brought back by an undo is a new
    // entity at the same position, and position is what a script names and what
    // a viewer's own stack can be matched against.
    struct Face
    {
        int32_t     order   = 0;
        float       opacity = 1.0f;
        bool        visible = true;
        std::string name;
    };
    std::vector<Face> roster;

    /*
     * A ROSTER THAT MERELY STATES THE STACK, rather than changing it: the one
     * taken BEFORE a structural change, so an undo has somewhere to land. It is
     * a keyframe of structure and is walked over exactly as a pixel keyframe is.
     *
     * Without this flag a merge cost TWO presses of ctrl+z, and the second one
     * did nothing anybody could see -- which reads as a broken key, and is the
     * same failure stepping over pixel keyframes was added to avoid.
     */
    bool keyframe = false;

    // Text only: the box after the edit, or that it went. The key is its name
    // everywhere (PaintTextBox::key); the id means nothing past this document.
    PaintTextBox box;
    bool         removed = false;

    // Undo/Redo only: whose entry, and which of theirs (PaintOpKind::Undo).
    std::string target;
    uint32_t    ordinal = 0;

    // "Did this change the picture." A structural entry that changed the stack
    // did -- undoing it puts a layer back -- so it steps like a mark. A
    // retraction is not itself a mark: it names one, and it is what the
    // ordinals in the record count past.
    bool marks() const { return kind != PaintOpKind::Snapshot && !keyframe && !retraction(); }
    bool retraction() const { return kind == PaintOpKind::Undo || kind == PaintOpKind::Redo; }
    bool structural() const { return kind == PaintOpKind::Layers; }
    void addPoint(int32_t x, int32_t y) { pts.push_back(x); pts.push_back(y); }
    size_t points() const { return pts.size() / 2; }
};

/*
 * THE LOG ITSELF -- append-only, sequence-numbered from 1 so that 0 can mean
 * "before anything", which is what a viewer asking for everything sends.
 *
 * SEQUENCES ARE ASSIGNED HERE and nowhere else. When this notebook is the one a
 * node holds, that makes the node the ordering domain for the session, which is
 * the same answer ChessNode reached for the same reason and by the same
 * argument: the sync unit and the ordering domain have to be one object or a
 * viewer following two documents needs two channels.
 *
 * THE COST OF UNDO IS THE SNAPSHOT INTERVAL. Replay-only undo is O(history) per
 * step, which is precisely why the store this replaces was snapshots; entries
 * with a whole-layer snapshot dropped in every SNAPSHOT_EVERY marks bounds it at
 * that interval instead. The same structure is what a late joiner wants --
 * nearest snapshot plus the tail -- so the interval is one knob for both and
 * neither reader has to know the other exists.
 */
class PaintNotebook
{
public:
    // Marks between snapshots, per layer. Sixteen is a compromise with two
    // readers: undo replays at most this many ops, and a viewer joining late
    // downloads at most this many on top of one snapshot.
    static constexpr size_t SNAPSHOT_EVERY = 16;

    uint64_t head() const { return m_ops.empty() ? 0 : m_ops.back().seq; }
    size_t   size() const { return m_ops.size(); }
    bool     empty() const { return m_ops.empty(); }

    // `parent` is where the document stood when this was made. Passed in
    // rather than read from a member, because the notebook does not own the
    // cursor -- the document does, and a store that kept its own copy of
    // somebody else's position is a second thing to keep in step.
    uint64_t Append(PaintOp op, uint64_t parent)
    {
        op.seq    = m_next++;
        op.parent = parent;
        return push(std::move(op));
    }

    /*
     * KEEP THE NUMBER IT ARRIVED WITH. An entry made here is numbered here;
     * an entry that came from a node was numbered THERE, and renumbering it
     * would quietly break the one thing a viewer depends on -- that "I have
     * read up to N" means the same N on both ends. The node is the ordering
     * domain for a shared session, so on a viewer this notebook is a copy of
     * the node's numbering rather than a numbering of its own.
     *
     * Out of order or repeated is not an error worth refusing over: a
     * duplicate arrives when a poll overlaps a push, and dropping the picture
     * on the floor over it would be a worse answer than drawing the stroke
     * twice. It is logged, applied, and the sequence carries on from the
     * highest seen.
     */
    uint64_t AppendAt(PaintOp op, uint64_t parent)
    {
        if (op.seq == 0) op.seq = m_next;
        if (op.seq < m_next)
            ETCS_LOG("PaintNotebook", "entry " << op.seq << " arrived at or behind "
                     << m_next << " -- kept, in arrival order.");
        m_next = std::max(m_next, op.seq + 1);
        // An arriving entry is a LINE, not a branch: what a session replays is
        // one canonical path, so a viewer's copy of it is linear by
        // construction. Its parent is simply whatever this notebook last held.
        op.parent = parent;
        return push(std::move(op));
    }

    // Is this layer due for a keyframe? Asked by the document before it opens a
    // marking entry, so the snapshot lands BEFORE the mark it protects rather
    // than after it -- a snapshot taken after the change cannot undo it.
    bool snapshotDue(ETCS::RID layer) const
    {
        auto it = m_since_snap.find(layer);
        if (it == m_since_snap.end()) return true;      // never seen: seed one
        return it->second >= SNAPSHOT_EVERY;
    }

    const PaintOp* at(uint64_t seq) const
    {
        for (const PaintOp& o : m_ops) if (o.seq == seq) return &o;
        return nullptr;
    }

    const std::vector<PaintOp>& ops() const { return m_ops; }

    // Everything after `since`, in order. The viewer's whole read.
    void Since(uint64_t since, std::vector<const PaintOp*>& out) const
    {
        out.clear();
        for (const PaintOp& o : m_ops) if (o.seq > since) out.push_back(&o);
    }

    /*
     * ── the tree ────────────────────────────────────────────────────────
     *
     * THE CANONICAL PATH IS THE ONLY THING THAT GETS REPLAYED, and every one
     * of these exists to say what that path is. Sequence order stopped being
     * the answer the moment a second branch could exist: the entries between
     * a snapshot and a target may belong to a sibling, and replaying those
     * paints a picture that was never made.
     */

    // Root-first: the chain of entries from the beginning to `seq`. Empty if
    // `seq` names nothing, which is what a cursor of 0 means -- before
    // anything, and correct rather than an error.
    void ChainTo(uint64_t seq, std::vector<const PaintOp*>& out) const
    {
        out.clear();
        uint64_t walk = seq;
        // Bounded by the log's own size: a cycle cannot form from an append-only
        // store whose parents are always older, but this walks user-visible
        // state and a bound costs nothing next to trusting that.
        for (size_t guard = 0; walk != 0 && guard <= m_ops.size(); ++guard)
        {
            const PaintOp* o = at(walk);
            if (!o) break;
            out.push_back(o);
            walk = o->parent;
        }
        std::reverse(out.begin(), out.end());
    }

    // The children of `seq`, newest first -- which is the order redo wants and
    // the whole of the "branches decided by most recent" rule. Sequences are
    // unique and assigned in the order things happened, so there is no tie to
    // break.
    void ChildrenOf(uint64_t seq, std::vector<const PaintOp*>& out) const
    {
        out.clear();
        for (const PaintOp& o : m_ops) if (o.parent == seq) out.push_back(&o);
        std::sort(out.begin(), out.end(),
                  [](const PaintOp* a, const PaintOp* b) { return a->seq > b->seq; });
    }

    // The newest snapshot of this layer ON THIS CHAIN. Not "at or before seq":
    // an entry with a lower sequence can belong to a branch the target is not
    // on, and restoring from it would be restoring somebody else's past.
    static const PaintOp* SnapshotOnChain(const std::vector<const PaintOp*>& chain,
                                          ETCS::RID layer)
    {
        const PaintOp* best = nullptr;
        for (const PaintOp* o : chain)
        {
            // A structural entry that carries bytes IS a keyframe for the layer
            // it carries them for -- that is how a merge's result reaches the
            // survivor on the way forward (appendRoster's `carries`).
            const bool keyframes_it =
                (o->kind == PaintOpKind::Snapshot || (o->structural() && !o->bytes.empty()))
                && o->layer == layer;
            if (keyframes_it) best = o;
        }
        return best;
    }

    /*
     * ── the path as the record reads it ─────────────────────────────────
     *
     * A retraction (PaintOpKind::Undo) names an entry by (author, ordinal):
     * the ordinal is that author's count of MARKING entries along the path,
     * from one. Counted here, in one place, because the writer that makes the
     * edge and every member that applies it have to count the same way, and
     * they do not share a sequence numbering -- only this order.
     *
     * The effective path is the canonical path with three things taken out:
     * the retraction entries themselves (they say, they do not draw); every
     * entry a retraction names that no later redo put back; and, for each
     * layer, every KEYFRAME taken after the oldest retracted mark on it -- a
     * snapshot holds the pixels of everything before it, retracted or not, so
     * the layer has to be rebuilt from the last keyframe that predates the
     * retraction. replayTo over the result is the same walk it always was.
     */
    struct Ordinal { std::string author; uint32_t ordinal = 0; };

    static void Ordinals(const std::vector<const PaintOp*>& chain,
                         std::vector<Ordinal>& out)
    {
        out.assign(chain.size(), Ordinal{});
        std::unordered_map<std::string, uint32_t> count;
        for (size_t i = 0; i < chain.size(); ++i)
        {
            const PaintOp* o = chain[i];
            if (!o->marks()) continue;
            out[i].author  = o->author;
            out[i].ordinal = ++count[o->author];
        }
    }

    // The entry an (author, ordinal) names on this path, or null.
    static const PaintOp* ByOrdinal(const std::vector<const PaintOp*>& chain,
                                    const std::string& author, uint32_t ordinal)
    {
        uint32_t seen = 0;
        for (const PaintOp* o : chain)
            if (o->marks() && o->author == author && ++seen == ordinal) return o;
        return nullptr;
    }

    // This author's newest marking entry on the path that stands (`retracted`
    // false) or that is retracted (`retracted` true): what an undo and a redo
    // respectively name. Answers its ordinal; zero when there is none.
    static uint32_t Newest(const std::vector<const PaintOp*>& chain,
                           const std::string& author, bool retracted)
    {
        std::vector<bool> gone;
        RetractedMask(chain, gone);
        uint32_t seen = 0, answer = 0;
        for (size_t i = 0; i < chain.size(); ++i)
        {
            const PaintOp* o = chain[i];
            if (!o->marks() || o->author != author) continue;
            ++seen;
            if (gone[i] == retracted) answer = seen;
        }
        return answer;
    }

    // Which entries of `chain` the retractions on it have taken back, as a
    // mask aligned with it. A redo after an undo of the same entry puts it
    // back; the last word along the path wins.
    static void RetractedMask(const std::vector<const PaintOp*>& chain, std::vector<bool>& gone)
    {
        gone.assign(chain.size(), false);
        for (const PaintOp* r : chain)
        {
            if (!r->retraction()) continue;
            uint32_t seen = 0;
            for (size_t i = 0; i < chain.size(); ++i)
            {
                const PaintOp* o = chain[i];
                if (o == r) break;                       // only what came before it
                if (!o->marks() || o->author != r->target) continue;
                if (++seen == r->ordinal) { gone[i] = (r->kind == PaintOpKind::Undo); break; }
            }
        }
    }

    void EffectivePath(uint64_t seq, std::vector<const PaintOp*>& out) const
    {
        std::vector<const PaintOp*> chain;
        ChainTo(seq, chain);
        std::vector<bool> gone;
        RetractedMask(chain, gone);

        // The oldest retracted mark per layer: keyframes of that layer from
        // there on are of a picture that no longer stands.
        std::unordered_map<ETCS::RID, uint64_t> first_gone;
        for (size_t i = 0; i < chain.size(); ++i)
            if (gone[i] && !first_gone.count(chain[i]->layer))
                first_gone[chain[i]->layer] = chain[i]->seq;

        out.clear();
        out.reserve(chain.size());
        for (size_t i = 0; i < chain.size(); ++i)
        {
            const PaintOp* o = chain[i];
            if (o->retraction() || gone[i]) continue;
            const bool keyframes = o->kind == PaintOpKind::Snapshot
                                || (o->structural() && !o->bytes.empty());
            if (keyframes)
            {
                auto it = first_gone.find(o->layer);
                if (it != first_gone.end() && o->seq > it->second) continue;
            }
            out.push_back(o);
        }
    }

    // Drop everything before the newest snapshot of every layer. What keeps a
    // long session bounded, and the reason snapshots exist at all rather than
    // being only an undo optimisation: without a keyframe there is nothing a
    // prefix can be discarded in favour of.
    /*
     * COMPACT ALONG ONE PATH, AND REFUSE IF THERE ARE OTHERS.
     *
     * Dropping by sequence number was right for a list and is wrong for a
     * tree: an entry with a low sequence can be the only thing holding a
     * branch's ancestry, and cutting it orphans every entry above it -- an
     * undo that walks into the gap then finds a parent that is not there.
     *
     * So this keeps the given path's prefix back to its own oldest still-
     * needed keyframe, and only when that path is the whole tree. A document
     * with live branches keeps everything, which is the correct answer at the
     * sizes anybody has actually reached; a session long enough for that to
     * hurt wants a policy for WHICH branches to forget, and inventing one
     * before anyone has hit the problem would be inventing the wrong one.
     */
    size_t Compact(const std::vector<const PaintOp*>& path)
    {
        if (path.empty()) return 0;
        if (path.size() != m_ops.size())
        {
            ETCS_LOG("PaintNotebook", "compact declined: " << (m_ops.size() - path.size())
                     << " entr(ies) sit off this path -- branches are kept whole.");
            return 0;
        }

        uint64_t cut = path.back()->seq;
        std::unordered_map<ETCS::RID, uint64_t> newest;
        for (const PaintOp* o : path)
            if (o->kind == PaintOpKind::Snapshot) newest[o->layer] = o->seq;
        if (newest.empty()) return 0;
        for (const auto& [rid, seq] : newest) { (void)rid; cut = std::min(cut, seq); }

        const size_t before = m_ops.size();
        std::vector<PaintOp> kept;
        kept.reserve(m_ops.size());
        for (PaintOp& o : m_ops) if (o.seq >= cut) kept.push_back(std::move(o));
        m_ops.swap(kept);
        // The oldest survivor is a root now; nothing above it may point past it.
        if (!m_ops.empty()) m_ops.front().parent = 0;
        return before - m_ops.size();
    }

    void Clear() { m_ops.clear(); m_since_snap.clear(); m_next = 1; }

    // Drop the tail from `seq` on. Undo does not use this -- an undo that
    // erased its own future could not be redone -- but a document reloaded from
    // a shorter notebook does.
    void Truncate(uint64_t seq)
    {
        while (!m_ops.empty() && m_ops.back().seq >= seq) m_ops.pop_back();
        m_next = head() + 1;
        m_since_snap.clear();
        for (const PaintOp& o : m_ops)
        {
            if (o.marks()) ++m_since_snap[o.layer];
            else            m_since_snap[o.layer] = 0;
        }
    }

private:
    uint64_t push(PaintOp op)
    {
        if (op.marks()) ++m_since_snap[op.layer];
        else            m_since_snap[op.layer] = 0;
        m_ops.push_back(std::move(op));
        return m_ops.back().seq;
    }

    std::vector<PaintOp> m_ops;
    std::unordered_map<ETCS::RID, size_t> m_since_snap;
    uint64_t m_next = 1;
};

/*
 * ── the notebook on the wire ─────────────────────────────────────────────────
 *
 * ONE ENTRY PER LINE, fields separated by single spaces, numbers in decimal:
 *
 *   <seq> <kind> <author> <layer> <r> <g> <b> <a> <size> <hard> <tip> <blend>
 *         <tol> <npts> <x,y> <x,y> ...
 *   <seq> snap <author> <layer> <w> <h> <base64 png>
 *
 * LINES, not a binary frame, for a reason that outlives the convenience: the
 * thing carrying these is an HTTP body and the thing relaying them is a node
 * that must renumber and re-attribute every entry without understanding any of
 * them. A node parses the first three fields and copies the rest through --
 * which is what lets the SAME node relay an entry kind that was added to
 * PaintProvider after the node was built.
 *
 * WHY THE NODE REWRITES AUTHOR. A writer that could name itself could name
 * somebody else. The token the push arrived with is the only trustworthy
 * statement of who is pushing, so the node puts that name in and drops whatever
 * the line claimed. Same argument as ChessGame refusing a move from a seat's
 * non-holder: the client's own account of who it is has no standing.
 *
 * A snapshot travels as a PNG rather than raw RGBA -- the same encoder an
 * export already uses -- because a 1024x768 layer is 3 MB raw, and the base64
 * of that is 4 MB, which is most of an HttpServer send buffer for one entry.
 */
static inline std::string paint_op_encode(const PaintOp& op)
{
    std::string out;
    out += std::to_string(op.seq);
    out += ' ';
    out += paint_op_name(op.kind);
    out += ' ';
    out += op.author.empty() ? "-" : op.author;
    out += ' ';
    out += std::to_string(op.layer);
    out += ' ';
    out += std::to_string(op.order);

    /*
     * A box: its key first -- the one field past the author the node reads, to
     * refuse a box somebody else is holding -- then its place, its type, its
     * colour, the two flags, and the string in base64 ("-" when empty), since
     * a caption may hold every character the line format uses.
     */
    if (op.kind == PaintOpKind::Text)
    {
        const PaintTextBox& b = op.box;
        out += ' ' + (b.key.empty() ? std::string("-") : b.key);
        out += ' ' + std::to_string(b.x) + ' ' + std::to_string(b.y)
             + ' ' + std::to_string(b.w) + ' ' + std::to_string(b.h)
             + ' ' + std::to_string(b.font) + ' ' + std::to_string(b.size);
        for (float c : b.rgba) out += ' ' + std::to_string(c);
        out += op.removed  ? " 1" : " 0";
        out += op.keyframe ? " 1" : " 0";
        out += ' ';
        out += b.text.empty() ? std::string("-")
                              : paint_b64_encode(reinterpret_cast<const uint8_t*>(b.text.data()), b.text.size());
        return out;
    }

    // A retraction: whose entry and which. Layer and order above are zero.
    if (op.retraction())
    {
        out += ' ' + (op.target.empty() ? std::string("-") : op.target);
        out += ' ' + std::to_string(op.ordinal);
        return out;
    }

    if (op.kind == PaintOpKind::Layers || op.kind == PaintOpKind::Page)
    {
        if (op.kind == PaintOpKind::Page)
            out += ' ' + std::to_string(op.w) + ' ' + std::to_string(op.h);
        out += ' ';
        out += std::to_string(op.roster.size());
        for (const PaintOp::Face& f : op.roster)
        {
            // comma-separated within a layer, space between layers, so the
            // NAME is the one field that must carry neither -- both become '_'
            // on the wire. A collaborator who typed a space sees an underscore,
            // which is honest and costs a line to say; the alternative is a
            // quoting rule in a format whose whole virtue is that a node can
            // copy it through without understanding it.
            std::string nm = f.name.empty() ? std::string("-") : f.name;
            for (char& c : nm) if (c == ' ' || c == ',') c = '_';
            out += ' ' + std::to_string(f.order) + ',' + std::to_string(f.opacity)
                 + ',' + (f.visible ? "1" : "0") + ',' + nm;
        }
        return out;
    }

    if (op.kind == PaintOpKind::Snapshot)
    {
        out += ' '; out += std::to_string(op.w);
        out += ' '; out += std::to_string(op.h);
        out += ' ';
        // Straight from the entry's bytes: they are RGBA at (w,h), packed,
        // which is the layout the encoder wants and the layout SnapshotBytes
        // produced.
        std::vector<uint8_t> png;
        if (op.w && op.h && op.bytes.size() == size_t(op.w) * op.h * 4)
        {
            stbi_write_png_to_func(
                [](void* ctx, void* data, int len)
                {
                    auto* v = static_cast<std::vector<uint8_t>*>(ctx);
                    const uint8_t* b = static_cast<const uint8_t*>(data);
                    v->insert(v->end(), b, b + len);
                },
                &png, static_cast<int>(op.w), static_cast<int>(op.h), 4,
                op.bytes.data(), static_cast<int>(op.w) * 4);
        }
        out += png.empty() ? std::string("-") : paint_b64_encode(png.data(), png.size());
        return out;
    }

    const PaintBrushState& b = op.brush;
    auto num = [](float f) { return std::to_string(f); };
    out += ' ' + num(b.color.r) + ' ' + num(b.color.g) + ' ' + num(b.color.b) + ' ' + num(b.color.a);
    out += ' ' + num(b.size_px) + ' ' + num(b.hardness);
    out += ' ' + std::to_string(static_cast<int>(b.tip));
    out += ' ' + std::to_string(static_cast<int>(b.blend));
    out += ' ' + std::to_string(op.tolerance);
    out += ' ' + std::to_string(op.points());
    for (size_t i = 0; i + 1 < op.pts.size(); i += 2)
        out += ' ' + std::to_string(op.pts[i]) + ',' + std::to_string(op.pts[i + 1]);
    return out;
}

static inline bool paint_op_decode(const std::string& line, PaintOp& out)
{
    std::istringstream in(line);
    std::string kind, author;
    out = PaintOp{};
    if (!(in >> out.seq >> kind >> author >> out.layer >> out.order)) return false;
    out.kind   = paint_op_from(kind);
    out.author = (author == "-") ? std::string() : author;

    if (out.kind == PaintOpKind::Text)
    {
        PaintTextBox& b = out.box;
        int removed = 0, keyframe = 0;
        std::string body;
        if (!(in >> b.key >> b.x >> b.y >> b.w >> b.h >> b.font >> b.size
                 >> b.rgba[0] >> b.rgba[1] >> b.rgba[2] >> b.rgba[3] >> removed >> keyframe >> body))
            return false;
        out.removed  = (removed != 0);
        out.keyframe = (keyframe != 0);
        if (body != "-")
        {
            std::vector<uint8_t> raw;
            if (!paint_b64_decode(body, raw)) return false;
            b.text.assign(raw.begin(), raw.end());
        }
        return true;
    }

    if (out.retraction())
    {
        if (!(in >> out.target >> out.ordinal)) return false;
        if (out.target == "-") out.target.clear();
        return true;
    }

    if (out.kind == PaintOpKind::Layers || out.kind == PaintOpKind::Page)
    {
        if (out.kind == PaintOpKind::Page && !(in >> out.w >> out.h)) return false;
        size_t n = 0;
        if (!(in >> n)) return false;
        for (size_t i = 0; i < n; ++i)
        {
            std::string field;
            if (!(in >> field)) return false;
            PaintOp::Face f;
            size_t a = field.find(','), b = field.find(',', a + 1), c = field.find(',', b + 1);
            if (a == std::string::npos || b == std::string::npos || c == std::string::npos) return false;
            f.order   = std::atoi(field.substr(0, a).c_str());
            f.opacity = static_cast<float>(std::atof(field.substr(a + 1, b - a - 1).c_str()));
            f.visible = (field.substr(b + 1, c - b - 1) != "0");
            f.name    = field.substr(c + 1);
            if (f.name == "-") f.name.clear();
            out.roster.push_back(std::move(f));
        }
        return true;
    }

    if (out.kind == PaintOpKind::Snapshot)
    {
        std::string b64;
        if (!(in >> out.w >> out.h >> b64)) return false;
        if (b64 == "-") return false;
        std::vector<uint8_t> png;
        if (!paint_b64_decode(b64, png)) return false;
        int w = 0, h = 0, comp = 0;
        // Four channels forced, exactly as an import does: a layer is RGBA and
        // a PNG that was greyscale on the way out would otherwise come back a
        // different width in bytes.
        uint8_t* px = stbi_load_from_memory(png.data(), static_cast<int>(png.size()),
                                            &w, &h, &comp, 4);
        if (!px) return false;
        out.bytes.assign(px, px + size_t(w) * size_t(h) * 4);
        out.w = static_cast<uint32_t>(w);
        out.h = static_cast<uint32_t>(h);
        stbi_image_free(px);
        return true;
    }

    int tip = 0, blend = 0;
    size_t n = 0;
    if (!(in >> out.brush.color.r >> out.brush.color.g >> out.brush.color.b >> out.brush.color.a
             >> out.brush.size_px >> out.brush.hardness >> tip >> blend
             >> out.tolerance >> n)) return false;
    out.brush.tip   = static_cast<PaintTipMode>(tip);
    out.brush.blend = static_cast<PaintBlendMode>(blend);
    out.pts.reserve(n * 2);
    for (size_t i = 0; i < n; ++i)
    {
        std::string pair;
        if (!(in >> pair)) return false;
        const size_t c = pair.find(',');
        if (c == std::string::npos) return false;
        out.pts.push_back(std::atoi(pair.substr(0, c).c_str()));
        out.pts.push_back(std::atoi(pair.substr(c + 1).c_str()));
    }
    return true;
}

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
        if (refuse_read_only("restack")) return;
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
 * ── merging two layers into one ──────────────────────────────────────────
 *
 * MERGE DOWN puts this layer's pixels onto the one beneath and drops this one;
 * MERGE UP is the same act read from the other end, and the two are one
 * function because the only thing that differs is which of the pair survives.
 *
 * WHICH DIRECTION THE PIXELS GO IS NOT THE SAME AS WHICH LAYER SURVIVES, and
 * that is the whole subtlety. Merging down, the upper layer goes over the lower
 * and the lower keeps the result -- one composite straight into its bytes.
 * Merging up, the upper still goes over the lower, but the UPPER is what
 * survives, so the result has to be built somewhere else and moved in. Getting
 * this backwards produces a merge that looks right until one of the two has
 * transparency, which is every interesting case.
 *
 * THE SOURCE'S OPACITY IS BAKED IN, because after the merge there is no layer
 * left to carry it. The survivor keeps its own, unspent: it is still a layer
 * and still has one.
 *
 * A HIDDEN SOURCE IS REFUSED. Merging ink nobody can see into a layer they can
 * is a change whose whole effect is invisible until it is too late to undo it
 * cheaply -- and the fix is one click, so saying so beats guessing.
 *
 * UNDO RESTORES THE PIXELS, NOT THE LAYER. Remember() takes the survivor's
 * bytes, so ctrl+z puts the picture back; the layer that was merged away is
 * detached, not deleted (RemoveLayer's own note), and nothing here re-attaches
 * it. Same limitation RemoveLayer has carried all along, stated rather than
 * discovered.
 */
    bool MergeLayer(ETCS::RID layer_rid, int direction)
    {
        if (refuse_read_only("merge")) return false;
        std::vector<PaintLayer*> stack;
        OrderedLayers(stack);
        ETCS::Entity* raw = paint_resolve_tag("PaintLayer", layer_rid);
        if (!raw) return false;
        auto* self_layer = static_cast<PaintLayer*>(raw->getTrueType());
        if (!self_layer) return false;

        auto it = std::find(stack.begin(), stack.end(), self_layer);
        if (it == stack.end()) return false;
        const size_t idx = static_cast<size_t>(it - stack.begin());

        // direction < 0 is "down", toward the base of the stack.
        const size_t other = (direction < 0) ? (idx ? idx - 1 : idx) : idx + 1;
        if ((direction < 0 && idx == 0) || other >= stack.size())
        {
            ETCS_LOG("PaintDocument", "'" << self_layer->name() << "' has nothing "
                     << (direction < 0 ? "below" : "above") << " it to merge with.");
            return false;
        }

        PaintLayer* upper = (direction < 0) ? self_layer : stack[other];
        PaintLayer* lower = (direction < 0) ? stack[other] : self_layer;
        PaintLayer* keep  = (direction < 0) ? lower : upper;
        PaintLayer* gone  = (direction < 0) ? upper : lower;

        if (!gone->visible())
        {
            ETCS_LOG("PaintDocument", "'" << gone->name() << "' is hidden -- show it "
                     "before merging, or its ink lands where nobody asked for it.");
            return false;
        }

        // BOTH RASTERS, THEN THE ROSTER. A merge is a change to structure as
        // much as to pixels: keyframing only the survivor put the paint back on
        // undo and not the plane it came off, which made this a one-way door.
        // recordStructure keyframes every layer and appendRoster (below, after
        // the removal) writes down the stack the undo has to walk back over.
        recordStructure("merge");
        m_active_layer = keep;

        if (keep == lower)
        {
            // Down: the upper goes straight over the survivor's own pixels.
            paint_composite_raw_scaled_bytes(lower->PixelData(), lower->width(), lower->height(),
                                             lower->width() * 4,
                                             upper->PixelData(), upper->width(), upper->height(),
                                             0, 0, upper->width(), upper->height(),
                                             upper->opacity());
        }
        else
        {
            // Up: build lower-then-upper elsewhere, then that IS the survivor.
            std::vector<uint8_t> merged;
            if (!lower->SnapshotBytes(merged)) return false;
            paint_composite_raw_scaled_bytes(merged.data(), lower->width(), lower->height(),
                                             lower->width() * 4,
                                             upper->PixelData(), upper->width(), upper->height(),
                                             0, 0, upper->width(), upper->height(),
                                             upper->opacity());
            // The survivor is the upper, and it must be the lower's size for
            // these bytes to mean anything -- which every layer of one document
            // is, and which RestoreBytes checks rather than trusts.
            if (!upper->RestoreBytes(merged))
            {
                ETCS_LOG("PaintDocument", "merge up: '" << upper->name() << "' and '"
                         << lower->name() << "' are different sizes -- refused.");
                return false;
            }
        }

        const std::string went = gone->name();
        removeLayerQuiet(gone->getRID());
        // The new stack AND the survivor as it now is, on one entry -- see
        // appendRoster for why they cannot be two.
        appendRoster(false, keep);
        SetActiveLayer(keep->getRID());
        etcs_mark_observed(keep);
        ETCS_LOG("PaintDocument", "merged '" << went << "' into '" << keep->name() << "'.");
        return true;
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
        if (refuse_read_only("rename")) return;
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
    /*
 * RECORDED, so it comes back. This used to be the other one-way door: the
 * layer was detached, nothing in the notebook said the stack had changed, and
 * undo could restore every raster on a plane that was no longer there. It is
 * one keyframe pass and one metadata entry, and it buys undo for a delete.
 */
    void RemoveLayer(ETCS::RID layer_rid)
    {
        if (refuse_read_only("remove a layer")) return;
        // The public verb is the recorded one. MergeLayer uses the quiet form
        // below, because it has already recorded the structure for the pair it
        // is collapsing and a second pass would write the same keyframes twice.
        recordStructure("remove layer");
        removeLayerQuiet(layer_rid);
        appendRoster();
    }

    void removeLayerQuiet(ETCS::RID layer_rid)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintLayer", layer_rid);
        if (!raw) return;
        auto* layer = static_cast<PaintLayer*>(raw->getTrueType());
        if (!layer) return;
        // The bottom of the stack is the page's ground and stays: a document
        // with no layer at all has nothing to draw on, and "delete the base"
        // is almost always a slip. ClearLayer is the verb for emptying it.
        std::vector<PaintLayer*> stack;
        OrderedLayers(stack);
        if (!stack.empty() && stack.front() == layer)
        {
            ETCS_LOG("PaintDocument", "layer '" << layer->name()
                     << "' is the base of '" << m_name << "' and is not removable -- ClearLayer empties it.");
            return;
        }
        Touch();
        if (m_active_layer == layer) m_active_layer = nullptr;
        layer->SetDim(1.0f);          // it is nobody's hover target now
        layer->SetPeek(0.0f);
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
        {
            l->SetDim((subject && l == subject) ? 1.0f : (subject ? other : 1.0f));
            // A hidden subject comes up as the rest goes down, to the strength
            // they left: at the panel's 0.25 it shows at 0.75 -- plainly there,
            // and still plainly not a layer that is switched on.
            l->SetPeek((subject && l == subject && !l->visible()) ? 1.0f - other : 0.0f);
        }
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

        // The notebook, in one line: what a test asserts on and what tells an
        // operator whether a shared session is actually recording anything.
        // Marks and snapshots counted apart because the two have completely
        // different costs, and a run whose entries are all snapshots is a run
        // where something is taking the undescribed path every time.
        size_t marks = 0, snaps = 0, points = 0;
        for (const PaintOp& o : m_book.ops())
        {
            if (o.marks()) { ++marks; points += o.points(); } else ++snaps;
        }
        ETCS_LOG("PaintDocument", "  notebook: " << m_book.size() << " entr(ies) to seq "
                 << m_book.head() << " -- " << marks << " mark(s) over " << points
                 << " point(s), " << snaps << " snapshot(s); at " << m_cursor
                 << ", " << undoDepth() << " back / " << redoDepth() << " forward"
                 << (m_open_live ? ", one open" : ""));
    }

    /*
 * THE CARRY LANDS BEFORE THE GROUND MOVES. A lift is cut from the active layer
 * and dropped onto the active layer (LiftSelection / DropSelection), and the
 * whole of that contract is that the two are the same layer. Nothing enforced
 * it: pressing a row in the layer window is a different pane's input, so the
 * canvas's drag state is untouched, and a selection lifted from Ink and then
 * dropped after clicking Paper wrote Ink's pixels into Paper. That is the
 * "select bleeds across layers" case, and it is a property of this verb rather
 * than of the panel -- every caller that can change the active layer has it,
 * including the exported verb and a page load.
 *
 * Dropping rather than refusing: the pixels are somewhere the user put them,
 * so they land where they are, on the layer they came from. Touch() because
 * which row is highlighted is part of what the window shows.
 */
    void SetActiveLayer(ETCS::RID layer_rid)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintLayer", layer_rid);
        if (!raw) return;
        PaintLayer* next = static_cast<PaintLayer*>(raw->getTrueType());
        if (next == m_active_layer) return;
        if (m_sel.lifted()) DropSelection();
        m_active_layer = next;
        Touch();
    }

    void ClearLayer(ETCS::RID layer_rid, float r, float g, float b, float a)
    {
        if (refuse_read_only("clear a layer")) return;
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

    /*
 * A NEW BOX IS AN EDIT THAT HAS NOT ENDED: it is recorded when it is let go
 * (SelectTextBox), with whatever was typed into it, so placing a box and typing
 * a caption is one step of undo -- and a box let go empty is simply dropped,
 * never recorded, because an empty box is a click that missed. Its font and
 * size are the last ones the bar set, so a second caption matches the first.
 */
    uint32_t AddTextBoxColoured(int32_t x, int32_t y, int32_t w, int32_t h,
                                float r, float g, float bl, float a)
    {
        if (refuse_read_only("add text")) return 0;
        Touch();
        PaintTextBox b;
        b.rgba[0] = r; b.rgba[1] = g; b.rgba[2] = bl; b.rgba[3] = a;
        b.x = x; b.y = y;
        b.w = (w < 1) ? 1 : w;
        b.h = (h < 1) ? 1 : h;
        b.font = m_text_font;
        b.size = m_text_size;
        b.id = ++m_text_seq;
        b.key = (m_author.empty() ? std::string("-") : m_author) + "." + std::to_string(b.id);
        m_text.push_back(b);
        m_text_fresh = b.key;
        ETCS_LOG("PaintDocument", "text box " << b.id << " at " << b.x << "," << b.y
                 << " " << b.w << "x" << b.h);
        return b.id;
    }

    PaintTextBox* FindTextBox(uint32_t id)
    {
        for (auto& b : m_text) if (b.id == id) return &b;
        return nullptr;
    }
    const PaintTextBox* FindTextBox(uint32_t id) const
    {
        for (const auto& b : m_text) if (b.id == id) return &b;
        return nullptr;
    }

    // By verb: the whole string at once, recorded as an edit of its own unless
    // the box is open, in which case it is part of that edit.
    bool SetTextBoxText(uint32_t id, const std::string& text)
    {
        if (refuse_read_only("edit text")) return false;
        PaintTextBox* b = FindTextBox(id);
        if (!b) return false;
        b->text = text;
        if (id != m_text_sel) record_text(*b, false);
        Touch();
        return true;
    }

    /*
 * GONE, AS ONE STEP OF UNDO. A box that was never recorded -- placed and not
 * yet let go -- just goes; there is nothing in the history to undo.
 */
    bool RemoveTextBox(uint32_t id)
    {
        if (refuse_read_only("remove text")) return false;
        for (auto it = m_text.begin(); it != m_text.end(); ++it)
            if (it->id == id)
            {
                const PaintTextBox gone = *it;
                const bool was_open = (m_text_sel == id);
                if (was_open) m_text_sel = 0;
                m_text.erase(it);
                if (gone.key == m_text_fresh) m_text_fresh.clear();
                else record_text(gone, true);
                if (was_open && sharing()) text_event("release:" + gone.key);
                Touch();
                return true;
            }
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

    /*
 * ── the open box's type ──────────────────────────────────────────────────
 *
 * What the text bar sets (PaintTextBar). Each changes the box in place -- part
 * of the edit that ends when the box is let go -- and becomes the style the
 * next new box starts with.
 */
    static constexpr uint32_t TEXT_SIZE_MIN = 6, TEXT_SIZE_MAX = 400;

    bool SetTextFont(uint32_t id, uint32_t font)
    {
        PaintTextBox* b = FindTextBox(id);
        if (!b || readOnly()) return false;
        b->font = m_text_font = font;
        Touch();
        return true;
    }
    bool SetTextSize(uint32_t id, uint32_t size)
    {
        PaintTextBox* b = FindTextBox(id);
        if (!b || readOnly()) return false;
        b->size = m_text_size = std::clamp(size, TEXT_SIZE_MIN, TEXT_SIZE_MAX);
        Touch();
        return true;
    }
    bool SetTextColor(uint32_t id, float r, float g, float bl, float a)
    {
        PaintTextBox* b = FindTextBox(id);
        if (!b || readOnly()) return false;
        b->rgba[0] = r; b->rgba[1] = g; b->rgba[2] = bl; b->rgba[3] = a;
        Touch();
        return true;
    }

    // Whatever leaf claiming Glyphs draws them -- the document needs its own,
    // because it is what renders them, and it may be rendered with no input
    // machine attached at all.
    void BindGlyphs(ETCS::RID glyphs) { m_glyphs = glyphs; }
    ETCS::RID glyphs() const { return m_glyphs; }

    /*
 * EDITING AFFORDANCES ARE A VIEW STATE, so they are set from outside rather than
 * inferred here: the document has no opinion about which tool is in hand. The
 * input machine turns this on while the text tool is held (PaintInput), which is
 * what makes every existing box visible and therefore selectable.
 */
    void ShowTextBoxes(bool on)   { m_text_show = on; }

    /*
 * ── OPENING AND LETTING GO OF A BOX ──────────────────────────────────────
 *
 * Selecting a box OPENS it: the keys go into it and the bar comes up over it.
 * Letting go -- Escape, a press elsewhere, the bar's `ok`, another box, the
 * page's idle timer in a session -- ENDS THE EDIT, and that is the moment it
 * is recorded: one Text entry with the box as it now stands, if anything about
 * it changed. So undo takes back a whole edit, and a session sees each edit
 * when it is finished and never half of one.
 *
 * IN A SHARED SESSION, OPEN IS CLAIMED. The page asks the node, which gives
 * each box to the first person who asks and to nobody else until they let go;
 * a claim refused comes back as TextDenied and the box goes back to how it was.
 * Letting go releases the claim after the edit has been pushed.
 */
    void SelectTextBox(uint32_t id)
    {
        if (id == m_text_sel) return;
        const uint32_t was = m_text_sel;
        m_text_sel = 0;
        if (was) end_text_edit(was);
        if (id == 0) return;
        const PaintTextBox* b = FindTextBox(id);
        if (!b) return;
        m_text_sel = id;
        m_text_before = *b;
        if (sharing()) text_event("claim:" + b->key);
    }
    uint32_t selectedTextBox() const { return m_text_sel; }

    // A key went into the open box: the page keeps the claim alive while
    // someone is typing (and lets it lapse when they stop).
    void TextEdited(uint32_t id)
    {
        if (!sharing()) return;
        if (const PaintTextBox* b = FindTextBox(id)) text_event("touch:" + b->key);
    }

    /*
 * THE NODE SAID NO: somebody else is holding this box. It goes back to what it
 * was when it was opened -- anything typed since was typed into a box that was
 * never ours -- and closes, and nothing is recorded or sent.
 */
    void TextDenied(const std::string& key, const std::string& holder)
    {
        PaintTextBox* b = nullptr;
        for (auto& t : m_text) if (t.key == key) b = &t;
        if (!b) return;
        if (b->id == m_text_sel && m_text_before.key == key)
        {
            const uint32_t id = b->id;
            *b = m_text_before;
            b->id = id;
            m_text_sel = 0;
        }
        Touch();
        ETCS_LOG("PaintDocument", "text box " << key << " is " << (holder.empty() ? std::string("someone else") : holder)
                 << "'s until they let go of it.");
    }

    /*
 * ── LAYING A BOX OUT ─────────────────────────────────────────────────────
 *
 * The box's lines, at its own size, as the Glyphs provider measures them:
 * each paragraph (split at the Enters) filled word by word until the next word
 * would pass the box's width, a word wider than the whole box broken where it
 * runs out. In DOCUMENT units, so where a line breaks does not depend on the
 * zoom it is looked at through -- every page in a session wraps a box at the
 * same words because they all measure with the same font files (PaintFonts).
 */
    static void wrap_text(Glyphs_* g, const PaintTextBox& b, std::vector<std::string>& lines)
    {
        lines.clear();
        auto width = [&](const std::string& t)
        { return static_cast<int32_t>(g->MeasureText(t.c_str(), b.font, b.size).width); };
        size_t start = 0;
        while (true)
        {
            const size_t nl = b.text.find('\n', start);
            const std::string para = b.text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
            std::string line;
            size_t i = 0;
            while (i < para.size())
            {
                // The next word, with the spaces in front of it.
                size_t j = i;
                while (j < para.size() && para[j] == ' ') ++j;
                while (j < para.size() && para[j] != ' ') ++j;
                const std::string word = para.substr(i, j - i);
                if (width(line + word) <= b.w || line.empty())
                {
                    if (width(line + word) <= b.w) { line += word; i = j; continue; }
                    // One word wider than the box: as much of it as fits.
                    size_t k = i;
                    std::string part;
                    while (k < j && (part.empty() || width(part + para[k]) <= b.w)) part += para[k++];
                    lines.push_back(part);
                    i = k;
                    continue;
                }
                lines.push_back(line);
                line.clear();
                i = para.find_first_not_of(' ', i);          // a new line starts at its word
                if (i == std::string::npos) i = para.size();
            }
            lines.push_back(line);
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
    }

    // From one line's top to the next's: the size, and a sixth of it between.
    static int32_t line_step(const PaintTextBox& b)
    {
        return static_cast<int32_t>(b.size) + static_cast<int32_t>(b.size) / 6;
    }

    uint32_t textFont() const { return m_text_font; }
    uint32_t textSize() const { return m_text_size; }

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
        combine_selection();
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
        combine_selection();
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
        combine_selection();
        ETCS_LOG("PaintDocument", "wand at " << x << "," << y << " -> " << n << " px");
        return !m_sel.empty();
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
        combine_selection();
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

    // Pixels from outside the document onto the active layer at a place, as
    // one undoable step: a snapshot of the layer lands first, then the bytes
    // go over what is there (PaintLayer::DropPixels). The animation window's
    // "put" is this.
    bool PastePixels(const uint8_t* rgba, uint32_t w, uint32_t h, int32_t x, int32_t y)
    {
        if (refuse_read_only("paste")) return false;
        if (!m_active_layer || !rgba || w == 0 || h == 0) return false;
        appendSnapshot(m_active_layer);
        m_active_layer->DropPixels(rgba, w, h, x, y);
        Touch();
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
 * ── one gesture, three meanings ──────────────────────────────────────────
 *
 * The press says which (PaintInput::begin_selection reads the modifiers), and
 * it holds for the whole drag -- letting go of ctrl halfway through a drag
 * would otherwise turn an add into a replace and lose what was there.
 *
 * The base is the selection AT THE PRESS, kept whole because every motion
 * sample recombines against it. Sized to the page here rather than trusted,
 * since a document with nothing selected has an empty mask and the combine
 * indexes both.
 */
    void BeginSelectionGesture(PaintSelectOp op)
    {
        m_sel_op = op;
        if (op == PaintSelectOp::Replace) { m_sel_base.clear(); return; }
        const size_t need = static_cast<size_t>(m_width) * m_height;
        m_sel_base = m_sel.mask;
        if (m_sel_base.size() != need) m_sel_base.assign(need, 0);
    }

    void EndSelectionGesture()
    {
        m_sel_op = PaintSelectOp::Replace;
        m_sel_base.clear();
    }

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
        if (refuse_read_only("paste")) return false;
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
        if (refuse_read_only("delete")) return false;
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
        if (refuse_read_only("cut")) return false;
        if (!CopySelection()) return false;
        Remember();
        if (!LiftSelection()) return false;
        m_sel.lift.clear();
        return true;
    }

    bool hasClip() const { return !m_clip.lift.empty(); }

    /*
 * ── history, as the notebook ─────────────────────────────────────────────
 *
 * THE SAME SEAM, A DIFFERENT STORE. Remember() is still what every committed
 * change calls before it lands, and every one of its call sites is unchanged.
 * What it records is now an entry in the notebook (PaintNotebook, above)
 * rather than one of three whole-layer snapshots, so the depth cap is gone,
 * undo is a replay, and the same object answers a viewer.
 *
 * Remember() ALONE STILL MEANS SNAPSHOT. A caller that did not describe its
 * change gets the bytes, which is what makes this safe to land before every
 * mutation has a descriptor: an operation nobody has taught the notebook
 * about is recorded correctly and expensively rather than missed.
 *
 * WHAT IT COSTS NOW: one snapshot per SNAPSHOT_EVERY described marks per
 * layer, plus one per undescribed change. At 1024x768 that is 3 MB every
 * sixteen strokes instead of 3 MB every stroke -- and Compact() can throw
 * the prefix away, which the old store could not do at all because three
 * snapshots deep has no prefix to throw.
 */
    void Remember()
    {
        Touch();
        sealOpenOp();
        if (!m_active_layer) return;
        appendSnapshot(m_active_layer);
    }

    /*
 * THE DESCRIBING SEAM. Same moment as Remember(), one fact richer: the caller
 * knows what it is about to do, so the notebook records the operation instead
 * of the pixels.
 *
 * A snapshot still goes in first when the layer is due one, and BEFORE the
 * mark rather than after -- a keyframe taken after the change it is supposed
 * to be undoable past is a keyframe of the wrong picture.
 */
    void RememberOp(PaintOpKind kind, const PaintBrushState& brush,
                    uint32_t tolerance = 0)
    {
        Touch();
        sealOpenOp();
        if (!m_active_layer) return;
        if (m_book.snapshotDue(m_active_layer->getRID()))
            appendSnapshot(m_active_layer);

        m_open = PaintOp{};
        m_open.kind      = kind;
        m_open.layer     = m_active_layer->getRID();
        m_open.order     = m_active_layer->order();
        m_open.author    = m_author;
        m_open.brush     = brush;
        m_open.tolerance = tolerance;
        m_open_live      = true;
    }

    // A point on the open entry. Called by ApplyBrush for a Dab, and by the
    // anchored commits for their corners -- one path, so an entry that was
    // opened and never given a point is an entry that marked nothing.
    void NoteOpPoint(int32_t x, int32_t y)
    {
        if (m_open_live) m_open.addPoint(x, y);
    }

    // Seal the open entry. Called at every stroke release, and again by the
    // next seam, which is what closes one a lost release left in the air.
    void SealOp() { sealOpenOp(); }

    const PaintNotebook& notebook() const { return m_book; }
    PaintNotebook&       notebook()       { return m_book; }

    // Start the record over. The three callers are the three acts that make
    // every existing entry describe a picture that no longer exists: a resize
    // re-states every raster, New clears them all, and DestroyLayers takes them
    // away. Each used to drop two snapshot stacks and now drops one notebook,
    // which is the same sentence with less of it.
    void ClearHistory()
    {
        m_open      = PaintOp{};
        m_open_live = false;
        m_book.Clear();
        m_cursor    = 0;
        m_text_fresh.clear();
        /*
     * THE BOXES THAT OUTLIVE THE HISTORY ARE STATED AT ITS START -- a resize
     * keeps its captions -- as keyframes, which undo steps over. Without them
     * the first undo afterwards would rebuild the boxes from a path that never
     * mentions them, and take every caption away.
     */
        for (const PaintTextBox& b : m_text) record_text(b, false, true);
    }

    // Who authors entries made on this document from now on. Empty means this
    // page, which is what a document nobody is sharing keeps writing.
    void SetAuthor(const std::string& who) { m_author = who; }
    const std::string& author() const { return m_author; }

    /*
 * ── the notebook through a FILE, and why not through the call buffer ─────
 *
 * A page hands the runtime arguments through etcs_web_call, whose payload is
 * an ETCS::Buffer -- 256 bytes. One stroke does not fit, let alone a
 * snapshot. So the notebook crosses the same way an imported image already
 * does: the page writes the bytes into the browser's own filesystem and
 * passes a PATH, and the runtime reads the file.
 *
 * That is not a workaround for this feature, it is the established answer to
 * this exact question in this codebase (PaintCanvasMenu::OfferImport), and
 * reusing it means a shared session needs no new bridge, no new buffer size
 * and no second way for bulk data to reach the runtime.
 *
 * ExportOps writes what happened AFTER `since`, which is the only thing a
 * host has to push and the only thing a caught-up viewer has to read.
 */
    size_t ExportOps(const std::string& path, uint64_t since) const
    {
        std::ofstream o(path, std::ios::binary | std::ios::trunc);
        if (!o)
        {
            ETCS_LOG("PaintDocument", "ExportOps: cannot open '" << path << "'.");
            return 0;
        }
        /*
     * ALONG THE CANONICAL PATH, which is the only thing a session replays.
     * Sequence order would hand a viewer entries from a branch this document
     * abandoned -- strokes that were undone here would appear there, which is
     * the exact opposite of what an undo means.
     *
     * A PATH THAT NO LONGER CONTAINS `since` IS A DIVERGENCE: this document
     * has wound back past what it already sent, so the room is holding
     * strokes that are no longer part of the picture. Re-baselining is the
     * honest answer -- a keyframe of every layer on the path, then the tail --
     * and it costs one snapshot per divergence rather than per stroke. An
     * undo no longer gets here (in a session it is an entry on the path,
     * retract); what does is a page-level change -- New, a resize -- which
     * IS a new page for everyone.
     */
        std::vector<const PaintOp*> chain;
        m_book.ChainTo(m_cursor, chain);

        bool on_path = (since == 0);
        for (const PaintOp* op : chain) if (op->seq == since) { on_path = true; break; }

        size_t n = 0;
        if (!on_path)
        {
            ETCS_LOG("PaintDocument", "ExportOps: " << since << " is not on this path "
                     "any more -- re-baselining the room with this whole page.");
            n = write_baseline(o);
        }
        else
        {
            for (const PaintOp* op : chain)
            {
                if (op->seq <= since) continue;
                /*
             * ONLY WHAT THIS PAGE MADE. Entries that arrived from the session
             * are on this path too -- AcceptOp keeps them, so undo and a late
             * keyframe have the whole picture -- but they are the room's
             * already, and sending them back is how a joiner promoted to
             * writer used to push the host's own history at the host.
             */
                if (!m_author.empty() && op->author != m_author) continue;
                // Not keyframes: they are this page's own cache of its derived
                // picture, and on another member they overwrite what that
                // member derived (AcceptOp keeps its own).
                if (op->kind == PaintOpKind::Snapshot) continue;
                o << paint_op_encode(*op) << "\n";
                ++n;
            }
        }
        if (!o) { ETCS_LOG("PaintDocument", "ExportOps: write to '" << path << "' failed."); return 0; }
        ETCS_LOG("PaintDocument", "ExportOps: " << n << " entr(ies) after " << since
                 << " along a " << chain.size() << "-entry path -> '" << path << "'.");
        return n;
    }

    /*
 * THE WHOLE PAGE, FOR A ROOM THAT HAS NOTHING OF IT YET: a Page entry (extent
 * and stack) and a keyframe of every layer, bottom to top. What a session
 * opens with, and what a divergence re-sends (ExportOps).
 *
 * Every layer, not every layer the notebook touched: a page loaded from the
 * store or opened from a file has pixels no entry describes, and a history
 * pushed from zero sent none of them -- the joiner got the strokes and not the
 * picture under them.
 *
 * Each line carries the cursor as its sequence, so the page that pushed it
 * reads back where the next ExportOps should start from.
 */
    size_t ExportBaseline(const std::string& path) const
    {
        std::ofstream o(path, std::ios::binary | std::ios::trunc);
        if (!o) { ETCS_LOG("PaintDocument", "ExportBaseline: cannot open '" << path << "'."); return 0; }
        const size_t n = write_baseline(o);
        if (!o) { ETCS_LOG("PaintDocument", "ExportBaseline: write to '" << path << "' failed."); return 0; }
        ETCS_LOG("PaintDocument", "ExportBaseline: " << m_width << "x" << m_height << ", "
                 << (n ? n - 1 : 0) << " layer keyframe(s) at " << m_cursor << " -> '" << path << "'.");
        return n;
    }

    /*
 * EVERY LINE IS APPLIED AND KEPT, in the order it arrives. A line that does
 * not parse is skipped and said so rather than aborting the batch: a viewer
 * that drops one entry shows a slightly wrong picture, and a viewer that
 * stops reading shows a frozen one. The first is recoverable by the next
 * snapshot and the second is not recoverable at all.
 */
    size_t ImportOps(const std::string& path, uint64_t since = 0, bool keep_mine = false)
    {
        std::ifstream f(path, std::ios::binary);
        if (!f)
        {
            ETCS_LOG("PaintDocument", "ImportOps: cannot open '" << path << "'.");
            return 0;
        }
        // A read from zero is the record from its start, whatever this page
        // held: the chain starts again with it -- and so does the picture (a
        // Page entry replaces the document, become_page), so this page's OWN
        // lines are applied like everybody's: what it drew is in the record,
        // and nowhere else any more.
        // -- unless this page is the one that just SENT that record (keep_mine:
        // the host reading back the baseline it pushed), in which case its own
        // lines are chained and left alone: the picture here is what they were
        // made from.
        const bool restart = (since == 0);
        if (restart) { m_chain = 0; m_chain_seq = 0; }
        const bool pass_mine = keep_mine || !restart;
        std::string line;
        size_t taken = 0, bad = 0, mine = 0;
        while (std::getline(f, line))
        {
            if (line.empty()) continue;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            /*
             * EVERY LINE IS CHAINED, and only the others' are applied. The
             * page hands over the whole read, its own lines included: a
             * writer applied its strokes as it made them, so taking the
             * copy the node sends back would draw each one twice -- but the
             * chain is over the record as the NODE holds it, own lines and
             * all, or the two could never agree.
             */
            m_chain = XXH3_64bits_withSeed(line.data(), line.size(), m_chain);
            {
                std::istringstream head(line);
                uint64_t seq = 0; std::string kind, author;
                if (head >> seq >> kind >> author)
                {
                    m_chain_seq = seq;
                    if (pass_mine && !m_author.empty() && author == m_author) { ++mine; continue; }
                }
            }
            PaintOp op;
            if (!paint_op_decode(line, op))
            {
                ++bad;
                ETCS_LOG("PaintDocument", "ImportOps: unreadable entry: '"
                         << line.substr(0, 80) << (line.size() > 80 ? "..." : "") << "'");
                continue;
            }
            AcceptOp(std::move(op));
            ++taken;
        }
        ETCS_LOG("PaintDocument", "ImportOps: " << taken << " entr(ies) from '" << path
                 << "'" << (mine ? ", " + std::to_string(mine) + " of this page's own passed over" : "")
                 << (bad ? ", " + std::to_string(bad) + " unreadable and skipped" : "")
                 << "; at " << m_book.head() << ", record chain " << std::hex << m_chain
                 << std::dec << " at " << m_chain_seq << ".");
        return taken;
    }

    uint64_t recordChain()    const { return m_chain; }
    uint64_t recordChainSeq() const { return m_chain_seq; }

    // Whether everything this page made is in the record: no entry of its own
    // past `sent` (the page's last export) still waiting to go. Its picture is
    // only the record's picture when this answers yes -- a stroke is in its
    // maker's picture before it is anywhere else.
    bool settled(uint64_t sent) const
    {
        if (m_author.empty()) return true;
        std::vector<const PaintOp*> chain;
        m_book.ChainTo(m_cursor, chain);
        for (const PaintOp* o : chain)
            if (o->seq > sent && o->author == m_author && o->kind != PaintOpKind::Snapshot) return false;
        return !m_open_live;
    }

    /*
     * WHAT THE PICTURE IS, as one number: the document's own state surface and
     * subtree (Entity::getHash -- the layers as children, their flags), every
     * layer's pixels in stack order, and every text box. Two members at the
     * same record head that answer differently have diverged, whichever of
     * them is right, and that is a question the record chain cannot ask: it
     * says what was received, this says what was made of it.
     *
     * The pixels are hashed whole on every call rather than cached on a dirty
     * edge. A page asks once per presence tick (index.html, SHARE_VIEW_MS),
     * and a few megabytes through XXH3 is a millisecond or two -- cheaper than
     * being wrong about which write paths mark, which is the very thing this
     * exists to catch.
     */
    // The parts of PictureHash, one line per layer, for finding WHICH part two
    // members disagree on. A report, not a hash: it is what a divergence is
    // chased with.
    std::string PictureReport() const
    {
        std::ostringstream o;
        o << "picture " << m_width << "x" << m_height << " boxes " << m_text.size();
        std::vector<PaintLayer*> stack;
        OrderedLayers(stack);
        for (PaintLayer* l : stack)
        {
            const uint8_t* px = l->PixelData();
            o << " | layer RID " << l->getRID() << " order " << l->order() << " '" << l->name() << "' opacity " << l->opacity()
              << " visible " << l->visible() << " " << l->PixelWidth() << "x" << l->PixelHeight()
              << " px " << std::hex << (px ? XXH3_64bits(px, l->PixelBytes()) : 0) << std::dec;
        }
        return o.str();
    }

    uint64_t PictureHash() const
    {
        // NOT OVER Entity::getHash(). The node hash carries identity -- the
        // layers' RIDs, this page's own flags such as `readonly` -- and two
        // members with one picture have different identities by construction.
        // Only what the picture IS goes in: the extent, each layer's place,
        // opacity, visibility and pixels, and the boxes.
        uint64_t h = XXH3_64bits(&m_width, sizeof(m_width));
        h = XXH3_64bits_withSeed(&m_height, sizeof(m_height), h);
        std::vector<PaintLayer*> stack;
        OrderedLayers(stack);
        for (PaintLayer* l : stack)
        {
            const int32_t  order   = l->order();
            const float    opacity = l->opacity();
            const uint8_t  visible = l->visible() ? 1 : 0;
            h = XXH3_64bits_withSeed(&order,   sizeof(order),   h);
            h = XXH3_64bits_withSeed(&opacity, sizeof(opacity), h);
            h = XXH3_64bits_withSeed(&visible, sizeof(visible), h);
            const uint8_t* px = l->PixelData();
            if (px) h = XXH3_64bits_withSeed(px, l->PixelBytes(), h);
        }
        for (const PaintTextBox& b : m_text)
        {
            std::string t = b.key + '\x1f' + b.text + '\x1f' + std::to_string(b.x) + ',' + std::to_string(b.y)
                          + ',' + std::to_string(b.w) + ',' + std::to_string(b.h) + ','
                          + std::to_string(b.font) + ',' + std::to_string(b.size);
            for (float c : b.rgba) t += ',' + std::to_string(c);
            h = XXH3_64bits_withSeed(t.data(), t.size(), h);
        }
        return h;
    }

    /*
 * ── UNDO AND REDO WALK THE TREE ──────────────────────────────────────────
 *
 * Undo is "stand on my parent", redo is "stand on my most recent child", and
 * between them that is the entire model. Neither exchanges buffers with the
 * other -- a snapshot stack had to, because it can only move a state from one
 * pile to another, and a tree can simply name a node.
 *
 * WHAT A BRANCH IS. Draw, undo, draw again: the second stroke's parent is the
 * cursor, which already had a child, so it becomes a SECOND child rather than
 * erasing the first. Nothing is discarded by an undo and nothing is discarded
 * by the change that follows one -- ctrl+z walks back to the fork and ctrl+y
 * comes forward down whichever branch was made most recently.
 *
 * MOST RECENT, WITH NO TIE TO BREAK, because sequences are unique and handed
 * out in the order things actually happened. "Most recent" is `max(seq)` over
 * the children and needs no timestamp and no policy.
 *
 * THIS REPLACED A SEPARATE HIGH-WATER MARK. `m_redo_from` was a second
 * variable recording where the future used to reach, kept in step with the
 * cursor by hand and cleared by every new change -- which is how it expressed
 * "the untaken future is gone", the one thing a tree does not have to say.
 * Now "is there anything to redo" is "does the cursor have children", asked of
 * the structure rather than of a variable beside it.
 *
 * SNAPSHOTS ARE STEPPED OVER, not stopped on. A keyframe is an entry in the
 * chain but not a thing anybody did, so landing on one would make ctrl+z
 * sometimes do nothing visible -- which reads as a broken key, not as a
 * subtlety. Both directions skip to the nearest entry that MARKED something.
 */
    bool Undo()
    {
        if (refuse_read_only("undo")) return false;
        // An open box's edit is a step like any other: ended, and so recorded,
        // before the undo that may take it back.
        if (m_text_sel) SelectTextBox(0);
        sealOpenOp();
        if (shared()) return retract(PaintOpKind::Undo);
        // NOT "0 means the head". Every append sets the cursor, so zero is
        // genuinely "before anything" -- and reading it as the head would make
        // an undo on a fully wound-back document leap to the top and undo the
        // newest entry instead of answering that there is nothing left.
        const uint64_t cur = m_cursor;

        // The newest marking entry at or above the cursor, walking parents --
        // stepping over any keyframes sitting between here and it.
        std::vector<const PaintOp*> chain;
        m_book.ChainTo(cur, chain);
        const PaintOp* mark = nullptr;
        for (auto it = chain.rbegin(); it != chain.rend(); ++it)
            if ((*it)->marks()) { mark = *it; break; }

        if (!mark) { ETCS_LOG("PaintDocument", "nothing to undo"); return false; }
        m_cursor = mark->parent;
        return replayTo(m_cursor, "undo");
    }

    bool Redo()
    {
        if (refuse_read_only("redo")) return false;
        if (m_text_sel) SelectTextBox(0);
        sealOpenOp();
        if (shared()) return retract(PaintOpKind::Redo);
        // Down the most-recent child each time, until something that marked
        // lands under us. A run of keyframes has one child each, so this is
        // one step in every ordinary case.
        uint64_t walk = m_cursor;
        std::vector<const PaintOp*> kids;
        for (size_t guard = 0; guard <= m_book.size(); ++guard)
        {
            m_book.ChildrenOf(walk, kids);
            if (kids.empty()) break;
            walk = kids.front()->seq;                 // newest first
            if (kids.front()->marks())
            {
                m_cursor = walk;
                return replayTo(m_cursor, "redo");
            }
        }
        ETCS_LOG("PaintDocument", "nothing to redo");
        return false;
    }

    // Steps available each way, for the layer panel's readout and the two work
    // functions that print them. Ancestors that marked, and the marking entries
    // reachable forward down the most-recent branch.
    size_t undoDepth() const
    {
        std::vector<const PaintOp*> chain;
        m_book.ChainTo(m_cursor, chain);
        size_t n = 0;
        for (const PaintOp* o : chain) if (o->marks()) ++n;
        return n;
    }

    size_t redoDepth() const
    {
        size_t n = 0;
        uint64_t walk = m_cursor;
        std::vector<const PaintOp*> kids;
        for (size_t guard = 0; guard <= m_book.size(); ++guard)
        {
            m_book.ChildrenOf(walk, kids);
            if (kids.empty()) break;
            if (kids.front()->marks()) ++n;
            walk = kids.front()->seq;
        }
        return n;
    }

    // How many branches fork off the cursor, which is the one thing about the
    // tree a person can otherwise only discover by pressing redo and being
    // surprised. The panel can say "2 futures" with this.
    size_t branchesHere() const
    {
        std::vector<const PaintOp*> kids;
        m_book.ChildrenOf(m_cursor, kids);
        return kids.size();
    }

    /*
 * REPLAY ONE ENTRY ONTO THIS DOCUMENT. The viewer's whole job, and the second
 * half of undo's.
 *
 * Deliberately NOT a second implementation of any mark: every branch here ends
 * in the same PaintLayer primitive the live path calls, with the brush the
 * entry carries. A replayed stroke that drew itself differently from the one
 * it is replaying would be a test of the wrong thing and a viewer showing a
 * different picture.
 */
    bool ApplyOp(const PaintOp& op)
    {
        // A box names no layer; neither does a page.
        if (op.kind == PaintOpKind::Text) { apply_text_state(op.box, op.removed); return true; }
        if (op.kind == PaintOpKind::Page) return true;
        PaintLayer* layer = layerFor(op);
        if (!layer)
        {
            ETCS_LOG("PaintDocument", "replay: no layer for entry " << op.seq
                     << " (RID " << op.layer << ", order " << op.order
                     << ") -- dropped.");
            return false;
        }

        switch (op.kind)
        {
        case PaintOpKind::Snapshot:
            if (!layer->RestoreBytes(op.bytes))
            {
                ETCS_LOG("PaintDocument", "replay: entry " << op.seq
                         << " is " << op.bytes.size() << " bytes for a layer that is not that size"
                         << " -- dropped.");
                return false;
            }
            return true;

        case PaintOpKind::Dab:
            for (size_t i = 0; i + 1 < op.pts.size(); i += 2)
                layer->DrawBrush(op.pts[i], op.pts[i + 1], op.brush);
            return true;

        case PaintOpKind::Line:
            if (op.points() < 2) return false;
            layer->StrokeLine(op.pts[0], op.pts[1], op.pts[2], op.pts[3], op.brush);
            return true;

        case PaintOpKind::Rect:
            if (op.points() < 2) return false;
            layer->DrawRectOutline(op.pts[0], op.pts[1], op.pts[2], op.pts[3], op.brush);
            return true;

        case PaintOpKind::Ellipse:
            if (op.points() < 2) return false;
            layer->DrawEllipseOutline(op.pts[0], op.pts[1], op.pts[2], op.pts[3], op.brush);
            return true;

        case PaintOpKind::Poly:
        {
            const size_t n = op.points();
            if (n < 2) return false;
            for (size_t i = 0; i < n; ++i)
            {
                const size_t j = (i + 1) % n;
                layer->StrokeLine(op.pts[i * 2], op.pts[i * 2 + 1],
                                  op.pts[j * 2], op.pts[j * 2 + 1], op.brush);
            }
            return true;
        }

        case PaintOpKind::Page:          // both answered above
        case PaintOpKind::Text:
            return true;

        case PaintOpKind::Layers:
            // The roster half is applied by reconcileLayers, before any raster.
            // What is left here is the raster half, which only a merge has.
            if (op.bytes.empty()) return true;
            if (!layer->RestoreBytes(op.bytes))
            {
                ETCS_LOG("PaintDocument", "replay: entry " << op.seq
                         << " carries bytes for a layer that is not that size -- dropped.");
                return false;
            }
            return true;

        case PaintOpKind::Fill:
        {
            if (op.points() < 1) return false;
            const PaintColor ink = (op.brush.blend == PaintBlendMode::Erase)
                                   ? PaintColor{ 0.0f, 0.0f, 0.0f, 0.0f }
                                   : op.brush.color;
            layer->FloodFill(op.pts[0], op.pts[1], ink, op.tolerance);
            return true;
        }
        }
        return false;
    }

    // Take an entry somebody else made -- a host's push, a file being reopened
    // -- into this notebook AND onto the picture. The one door for arriving
    // history, so a viewer and a reload are the same code path.
    bool AcceptOp(PaintOp op)
    {
        /*
     * A PAGE ENTRY REPLACES THE DOCUMENT rather than adding to it: the size,
     * the stack and nothing on it, history and text boxes gone. The keyframes
     * that follow it in the same baseline fill the layers. Not appended -- it
     * is where this notebook now starts.
     */
        if (op.kind == PaintOpKind::Page)
        {
            become_page(op);
            return true;
        }
        /*
         * A RETRACTION IS APPLIED BY REPLAY. It names an entry already on this
         * path; appending it and re-deriving the path is what takes the entry
         * out of the picture, on every member alike (PaintNotebook::
         * EffectivePath). Nothing to draw, so nothing goes through ApplyOp.
         */
        if (op.retraction())
        {
            m_cursor = m_book.AppendAt(std::move(op), m_cursor);
            return replayTo(m_cursor, "retraction");
        }
        /*
         * A KEYFRAME OF OUR OWN, when this layer is due one, BEFORE the mark
         * lands -- the same rule RememberOp keeps for a mark made here. The
         * record carries no keyframes past the baseline (ExportOps: a writer's
         * whole-layer snapshot was overwriting whatever the others had drawn
         * on that layer since it was taken), so a member keeps its own, of the
         * picture as IT has derived it, and a replay stays bounded.
         */
        if (op.marks() && shared())
            if (PaintLayer* l = layerFor(op))
                if (m_book.snapshotDue(l->getRID())) appendSnapshot(l);
        // A change to the STACK made elsewhere has to change this stack too;
        // the raster half of the entry (a merge's) lands on the result.
        if (op.structural()) reconcileLayers(&op);
        const bool ok = ApplyOp(op);
        m_cursor = m_book.AppendAt(std::move(op), m_cursor);
        Touch();
        return ok;
    }

    bool shared() const { return !m_author.empty(); }

    /*
     * THE UNDO EDGE (PaintOpKind::Undo). Names this page's newest entry that
     * still stands -- its own, never anybody else's: in a room a stroke is its
     * author's to take back, and an undo that reached across authors would
     * have every writer's ctrl+z erasing whoever drew last. Appended to the
     * path like an arriving one and applied the same way, so what this page
     * shows after its own undo is exactly what the others will show after
     * reading it. Redo names the newest of this page's entries that IS
     * retracted, and puts it back.
     */
    bool retract(PaintOpKind kind)
    {
        std::vector<const PaintOp*> chain;
        m_book.ChainTo(m_cursor, chain);
        const bool undo = (kind == PaintOpKind::Undo);
        const uint32_t k = PaintNotebook::Newest(chain, m_author, !undo);
        if (k == 0)
        {
            ETCS_LOG("PaintDocument", (undo ? "nothing of yours to undo" : "nothing of yours to redo"));
            return false;
        }
        PaintOp edge;
        edge.kind    = kind;
        edge.author  = m_author;
        edge.target  = m_author;
        edge.ordinal = k;
        m_cursor = m_book.AppendAt(std::move(edge), m_cursor);
        Touch();
        return replayTo(m_cursor, undo ? "undo" : "redo");
    }

    /*
 * ── VIEW ONLY ────────────────────────────────────────────────────────────
 *
 * A reader in a shared session sees the host's page and must not change it:
 * the room would never hear of the change, and from then on this picture and
 * everybody else's differ in a way nothing puts right. A lowercase state flag
 * on the document, raised by the page while its role is reader, and every
 * verb that edits refuses while it is up (refuse_read_only). What ARRIVES is
 * not an edit made here -- AcceptOp works below these verbs -- so the room's
 * changes still land.
 */
    void SetReadOnly(bool on)
    {
        if (on) this->addTag("readonly");
        else    this->removeTag(ETCS::Buffer("readonly"));
        const char* said = on ? "view only -- this page follows the session and takes no edits."
                              : "editable.";
        ETCS_LOG("PaintDocument", said);
    }
    bool readOnly() const { return const_cast<PaintDocument*>(this)->hasTag(ETCS::Buffer("readonly")); }

    // Where this document stands in the record, which on a viewer is where it
    // stands in the NODE's record -- the `since` of its next read.
    uint64_t notebookHead() const { return m_book.head(); }

    void RenderToSurface(ETCS::RID target, int32_t x, int32_t y, float zoom = 1.0f)
    {
        Surface_* surface = ETCS::resolve_in_family<Surface_>("Surface", target);
        if (!surface) return;

        std::vector<PaintLayer*> stack;
        OrderedLayers(stack);
        for (auto* layer : stack)
        {
            if (!layer->onScreen()) continue;   // a peek draws a hidden layer -- see PaintLayer::SetPeek
            // 1.0, NOT layer->opacity(): BlitTo multiplies by m_opacity itself
            // (see its alpha), so passing it here drew every layer at opacity
            // SQUARED -- a layer set to 50% showed at 25% on screen while the
            // export, which composites once (CompositeVisible), showed it at
            // 50%. The picture and the file disagreed about the same number.
            layer->BlitTo(target, x, y, 0, 0, 1.0f, zoom);
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
        if (refuse_read_only("import")) return false;
        PaintImage img;
        std::string why;
        if (!paint_image_read(path, img, why))
        {
            ETCS_LOG("PaintDocument", "import " << path << ": " << why);
            return false;
        }
        return import_layer(img, path);
    }

    /*
 * THE IMAGE AS THE PAGE: a new canvas the image's size (New), and the image
 * on it as a layer above the paper. A layer rather than the paper's own
 * pixels, so a picture with transparency keeps it and "undo the import" is
 * still the row's delete; the page's size is what changes, which is what
 * "open this picture" means as opposed to "add it to the one I have"
 * (ImportImage). Both are offered when a file comes in
 * (PaintCanvasMenu::OfferImport).
 */
    bool ImportCanvas(const std::string& path)
    {
        if (refuse_read_only("open as canvas")) return false;
        PaintImage img;
        std::string why;
        if (!paint_image_read(path, img, why))
        {
            ETCS_LOG("PaintDocument", "import " << path << ": " << why);
            return false;
        }
        if (!New(img.w, img.h)) return false;
        return import_layer(img, path);
    }

private:
    bool import_layer(const PaintImage& img, const std::string& path)
    {
        Touch();
        PaintLayer* layer = this->addTag<PaintLayer>();
        if (!layer)
        {
            ETCS_LOG("PaintDocument", "import " << path << ": could not spawn a layer under '"
                     << m_name << "'.");
            return false;
        }
        /*
     * PAGE-SIZED, WITH THE IMAGE DROPPED INTO IT -- not a raster the size of
     * the file.
     *
     * A layer used to be created at the image's extent, on the reasoning that
     * a layer's raster is its own and resampling on the way in would throw
     * pixels away. What that actually bought was a layer whose pixels were
     * second class: every selection operation works in DOCUMENT coordinates
     * and lands through the layer's own raster, so lifting the image and
     * moving it wrote the pixels back outside the raster's bounds, where
     * DropPixels clips -- and the picture vanished. Fill, smudge and paste had
     * the same edge.
     *
     * Nothing is thrown away that was ever going to be shown: the composite
     * clips to the page (CompositeVisible), so anything outside it was already
     * invisible. The path for "keep all of it" is the other answer to the
     * import prompt -- ImportCanvas makes the PAGE the image's size first, and
     * then page-sized is exactly the image.
     */
        layer->Create(m_width, m_height);
        layer->Clear(0.0f, 0.0f, 0.0f, 0.0f);
        layer->DropPixels(img.rgba.data(), img.w, img.h, 0, 0);
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
                 << " order=" << top << ", active, on a " << m_width << "x" << m_height
                 << " raster"
                 << ((img.w > m_width || img.h > m_height)
                     ? " (clipped to the page -- 'new canvas' keeps all of it)" : ""));
        return true;
    }
public:

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
        if (!paint_image_write(path, px.data(), m_width, m_height, why))
        {
            ETCS_LOG("PaintDocument", "export " << path << ": " << why);
            return false;
        }
        ETCS_LOG("PaintDocument", "exported " << path << " " << m_width << "x" << m_height
                 << " -> " << path << ", " << shown << " visible layer(s)"
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
        if (!paint_image_write(path, px.data(), m_active_layer->width(), m_active_layer->height(), why))
        {
            ETCS_LOG("PaintDocument", "export layer " << path << ": " << why);
            return false;
        }
        ETCS_LOG("PaintDocument", "exported layer '" << m_active_layer->name() << "' RID:"
                 << m_active_layer->getRID() << " " << m_active_layer->width() << "x"
                 << m_active_layer->height() << " -> " << path);
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
        if (refuse_read_only("resize")) return false;
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

        ClearHistory();
        ETCS_LOG("PaintDocument", "'" << m_name << "' " << m_width << "x" << m_height
                 << " -> " << w << "x" << h << " anchored at " << anchor
                 << " (pixels moved by " << dx << "," << dy << "), " << stack.size()
                 << " layer(s)" << (paper ? ", paper '" + paper->name() + "' extended" : "")
                 << "; history dropped -- a resize re-states every raster, so no entry taken before it describes a layer that is still that size.");
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
        if (refuse_read_only("new")) return false;
        return renew(w, h);
    }

    // New without the view-only guard: what a session's Page entry does to a
    // follower (become_page), which is the room's change and not an edit here.
    bool renew(uint32_t w, uint32_t h)
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
        ClearHistory();
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
        ClearHistory();
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
    /*
 * A NEW, EMPTY LAYER ABOVE THE ACTIVE ONE, which is what the window's + does.
 *
 * ABOVE THE ACTIVE ONE rather than on top of everything, because "add a layer"
 * while working on layer 2 of 5 means "one to draw on next to this", and a
 * layer that always lands on top is one the user then has to drag back down.
 * Page-sized and transparent: a layer is a sheet over the picture, and its own
 * raster is only ever its own size when a file arrived at that size
 * (ImportImage).
 *
 * The name is the first "layer N" nobody is using, counting from the stack's
 * size, so adding and removing does not produce two layers with one name.
 */
    ETCS::RID NewLayer()
    {
        if (refuse_read_only("add a layer")) return 0;
        recordStructure("new layer");
        PaintLayer* layer = this->addTag<PaintLayer>();
        if (!layer)
        {
            ETCS_LOG("PaintDocument", "could not spawn a layer under '" << m_name << "'.");
            return 0;
        }
        layer->Create(m_width, m_height);
        layer->Clear(0.0f, 0.0f, 0.0f, 0.0f);

        std::vector<PaintLayer*> stack;
        OrderedLayers(stack);
        std::string name;
        for (size_t n = stack.size(); ; ++n)
        {
            name = "layer " + std::to_string(n);
            bool taken = false;
            for (auto* l : stack) if (l != layer && l->name() == name) { taken = true; break; }
            if (!taken) break;
        }
        layer->SetName(name);

        // Straight above the active layer, counting depth from the bottom as
        // MoveLayerTo does; on top when nothing is active.
        int32_t depth = static_cast<int32_t>(stack.size()) - 1;
        for (size_t i = 0; i < stack.size(); ++i)
            if (stack[i] == m_active_layer) depth = static_cast<int32_t>(i) + 1;
        layer->SetOrder(static_cast<int32_t>(stack.size()));
        MoveLayerTo(layer->getRID(), depth);

        if (m_sel.lifted()) DropSelection();
        m_active_layer = layer;
        Touch();
        ETCS_LOG("PaintDocument", "layer '" << name << "' RID:" << layer->getRID()
                 << " added at depth " << depth << ", active");
        appendRoster();
        return layer->getRID();
    }

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
        std::vector<std::string> lines;
        for (const PaintTextBox& b : m_text)
        {
            const int32_t vx = ox + static_cast<int32_t>(b.x * z);
            const int32_t vy = oy + static_cast<int32_t>(b.y * z);
            const int32_t vw = std::max(1, static_cast<int32_t>(b.w * z));
            const int32_t vh = std::max(1, static_cast<int32_t>(b.h * z));
            const bool sel = (b.id == m_text_sel);

            if (m_text_show || sel)
            {
                /*
             * A one-pixel frame, as four thin rects -- that is what a surface can
             * draw. Coloured rather than pale: near-white with low alpha is
             * invisible on the paper it is drawn on, and an affordance you cannot
             * see is not one. The open box is stronger and fully opaque, the rest
             * are dimmer, so "which box has the keyboard" is answerable at a
             * glance -- and it carries the corner handle that resizes it.
             */
                const float r0 = sel ? 0.15f : 0.35f;
                const float g0 = sel ? 0.50f : 0.45f;
                const float b0 = sel ? 0.95f : 0.60f;
                const float a  = sel ? 1.00f : 0.55f;
                surface->DrawRect(vx, vy, static_cast<uint32_t>(vw), 1u, r0, g0, b0, a);
                surface->DrawRect(vx, vy + vh - 1, static_cast<uint32_t>(vw), 1u, r0, g0, b0, a);
                surface->DrawRect(vx, vy, 1u, static_cast<uint32_t>(vh), r0, g0, b0, a);
                surface->DrawRect(vx + vw - 1, vy, 1u, static_cast<uint32_t>(vh), r0, g0, b0, a);
                if (sel)
                    surface->DrawRect(vx + vw - TEXT_HANDLE_PX, vy + vh - TEXT_HANDLE_PX,
                                      static_cast<uint32_t>(TEXT_HANDLE_PX), static_cast<uint32_t>(TEXT_HANDLE_PX),
                                      r0, g0, b0, a);
            }

            if (!g) continue;
            wrap_text(g.get(), b, lines);
            const uint32_t px = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(b.size * z)));
            const int32_t step = line_step(b);
            int32_t caret_x = vx, caret_y = vy;
            for (size_t i = 0; i < lines.size(); ++i)
            {
                const int32_t top = static_cast<int32_t>(i) * step;
                // Lines past the box's bottom are not drawn: the box is the page.
                if (top + static_cast<int32_t>(b.size) > b.h) break;
                const int32_t ly = vy + static_cast<int32_t>(top * z);
                if (!lines[i].empty())
                    g->RasterizeText(target, lines[i].c_str(), b.font, px, vx, ly,
                                     b.rgba[0], b.rgba[1], b.rgba[2], b.rgba[3]);
                caret_x = vx + static_cast<int32_t>(g->MeasureText(lines[i].c_str(), b.font, px).width);
                caret_y = ly;
            }
            // Where the next character goes, on the open box: typing always
            // lands at the end, so the end is the one place a caret can be.
            if (sel)
                surface->DrawRect(caret_x + 1, caret_y, std::max(1u, px / 12u), px,
                                  b.rgba[0], b.rgba[1], b.rgba[2], 0.85f);
        }
    }

    // The open box's corner handle, in VIEW pixels -- a press in it resizes
    // the box rather than moving it (PaintInput).
    static constexpr int32_t TEXT_HANDLE_PX = 8;

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
 * WHAT THE GESTURE JUST DREW, COMBINED WITH WHAT WAS THERE.
 *
 * Called at the end of every Select*: they each build their own region into a
 * mask that fresh_selection wiped, so at this point m_sel holds the GESTURE
 * and m_sel_base holds the selection the gesture started from. Replace is
 * leaving it alone, which is why a bare drag pays nothing for this.
 *
 * The base is snapshotted at the press rather than accumulated, because a drag
 * re-runs the Select* on every motion sample -- accumulating would mean the
 * region grew along the path of the pointer instead of being the rectangle it
 * currently describes.
 */
    void combine_selection()
    {
        if (m_sel_op == PaintSelectOp::Replace) return;
        PaintSelection gesture;
        gesture.mask = m_sel.mask;                // the gesture's own pixels
        gesture.w = m_sel.w; gesture.h = m_sel.h;
        m_sel.mask = m_sel_base;
        if (m_sel_op == PaintSelectOp::Add) m_sel.Union(gesture);
        else                                m_sel.Subtract(gesture);
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
        const float alpha = layer->opacity() * layer->viewStrength();

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
    // THROUGH activeLayer(), not the member: that is where the selection is
    // bound as the clip, and this is the freehand path -- the one every stroke
    // takes and the one that must not be the exception.
    //
    // AND THE POINT GOES IN THE NOTEBOOK HERE, not in PaintInput. This is the
    // one place a freehand mark reaches the document, which makes it the only
    // place a Dab's path can be recorded without the recording and the marking
    // being two different code paths that have to agree. It also means a
    // scripted stroke (PaintInput::ScriptPointer) is recorded exactly as a
    // device's is, because both arrive here.
    void ApplyBrush(int32_t x, int32_t y, const PaintBrushState& brush)
    {
        if (PaintLayer* l = activeLayer())
        {
            l->DrawBrush(x, y, brush);
            NoteOpPoint(x, y);
        }
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
    /*
     * THE LAYER, CLIPPED BY THIS DOCUMENT'S SELECTION.
     *
     * An accessor that writes, deliberately, and this is the argument for it:
     * the alternative is binding the clip at every site that marks -- five in
     * PaintInput, more in the shape commits -- and one of them being forgotten
     * is a tool that ignores the selection while its neighbours honour it,
     * which is worse than any of them getting it wrong together. Every mark
     * reaches a layer through here, so here is where the invariant holds:
     * a layer you got from a document is clipped by that document.
     *
     * Layers are spawned by scripts as often as by this type
     * (boot_paint_panels.etcs), so binding at creation would miss the ones
     * that matter most. The pointer is a member of this object and outlives
     * every layer under it.
     */
    PaintLayer* activeLayer() const
    {
        if (m_active_layer) m_active_layer->BindClip(&m_sel);
        return m_active_layer;
    }

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
    // The open box as it was when it was opened, the key of one placed and not
    // yet recorded, and the style the next new box starts with (SelectTextBox,
    // AddTextBoxColoured).
    PaintTextBox m_text_before;
    std::string  m_text_fresh;
    uint32_t     m_text_font = 0;
    uint32_t     m_text_size = 24;
    bool      m_text_show = false;
    bool      m_text_warned = false;
    ETCS::RID m_glyphs = 0;
    // The one selection, and the pixels it is carrying if any. See PaintSelection.
    uint64_t m_revision = 0;     // climbs on every change -- see Touch
    PaintSelection m_sel;
    // The gesture in progress, and the selection it started from. Both are
    // per-drag and both are cleared by EndSelectionGesture.
    PaintSelectOp m_sel_op = PaintSelectOp::Replace;
    std::vector<uint8_t> m_sel_base;
    PaintSelection m_clip;     // the clipboard: a selection's shape and bytes, kept

    /*
     * ── the notebook, and where in it this document currently stands ─────
     *
     * m_cursor is "the entry this picture is as of", and it is the ONLY
     * position this document keeps -- which is the point of the tree. It is
     * also every new entry's parent, so a change made after an undo forks
     * rather than overwrites, and it is the node redo looks for children of.
     *
     * Zero means "before anything", which an empty notebook and a document
     * wound all the way back both are, and both want the same answer from
     * every walk.
     */
    PaintNotebook m_book;
    PaintOp       m_open;
    bool          m_open_live = false;
    uint64_t      m_cursor    = 0;
    std::string   m_author;
    // The record's chain as this page has read it (ImportOps): XXH3 of every
    // line taken in, seeded with the chain before it -- the node's own
    // arithmetic (PaintNode::Session::chain), so equal heads with different
    // chains means a line this page never took in. Reset by a read from zero.
    uint64_t      m_chain     = 0;
    uint64_t      m_chain_seq = 0;

    void sealOpenOp()
    {
        if (!m_open_live) return;
        m_open_live = false;
        // An entry that marked nothing is not history. A press that never
        // moved, an anchored gesture abandoned before it had two corners,
        // a preview that committed nothing -- all of them open an entry and
        // none of them changed the picture.
        if (m_open.pts.empty()) { m_open = PaintOp{}; return; }
        // The cursor is the parent, which is what makes a change after an undo
        // a BRANCH rather than an overwrite.
        m_cursor = m_book.Append(std::move(m_open), m_cursor);
        m_open = PaintOp{};
    }

    /*
     * KEYFRAME EVERY LAYER THAT IS ABOUT TO BE DISTURBED, then write down what
     * the stack will look like. Called BEFORE the structural change, so the
     * bytes needed to bring a layer back are already on the chain when the
     * roster that no longer mentions it arrives.
     *
     * Every layer, not only the ones this particular act touches: a merge takes
     * two and a delete takes one, but a reorder moves several and the cost of
     * being exact about which is a rule that will be wrong the first time
     * somebody adds a fourth structural verb.
     */
    void recordStructure(const char* why)
    {
        sealOpenOp();
        std::vector<PaintLayer*> stack;
        OrderedLayers(stack);
        for (PaintLayer* l : stack) appendSnapshot(l);
        // AND THE ROSTER AS IT STANDS NOW, which is the entry an undo lands on.
        // Without it, undoing past a NEW layer finds no roster at or before the
        // target and reconciles to nothing -- the layer stays. The pair is
        // "here is the stack before" and, after the change, "here is the stack
        // after"; a walk backwards over the second arrives at the first.
        appendRoster(true);
        ETCS_LOG("PaintDocument", why << ": keyframed " << stack.size() << " layer(s) and the roster.");
    }

    // And the roster AFTER it, which is the entry undo actually walks over.
    bool sharing() const { return !m_author.empty(); }

    // Up to the page, which holds the node: claim, touch, release.
    void text_event(const std::string& what) const
    {
#if defined(__EMSCRIPTEN__)
        MAIN_THREAD_EM_ASM({
            window.dispatchEvent(new CustomEvent('etcs-text', { detail: UTF8ToString($0) }));
        }, what.c_str());
#endif
        ETCS_LOG("PaintDocument", "text " << what);
    }

    // One Text entry on the path -- see PaintOpKind::Text.
    void record_text(const PaintTextBox& b, bool removed, bool keyframe = false)
    {
        sealOpenOp();
        PaintOp op;
        op.kind     = PaintOpKind::Text;
        op.author   = m_author;
        op.box      = b;
        op.box.id   = 0;               // this document's number, not the room's
        op.removed  = removed;
        op.keyframe = keyframe;
        m_cursor = m_book.Append(std::move(op), m_cursor);
    }

    // The edit on box `id` is over: recorded if anything about it changed,
    // dropped if it was placed and never written in, released in a session.
    void end_text_edit(uint32_t id)
    {
        PaintTextBox* b = FindTextBox(id);
        if (!b) return;
        const bool fresh = (b->key == m_text_fresh);
        m_text_fresh.clear();
        const std::string key = b->key;
        if (fresh && b->text.empty())
        {
            for (auto it = m_text.begin(); it != m_text.end(); ++it)
                if (it->id == id) { m_text.erase(it); break; }
            Touch();
        }
        else if (fresh || !b->same_as(m_text_before))
        {
            record_text(*b, false);
            ETCS_LOG("PaintDocument", "text box " << id << " edited: \"" << b->text << "\"");
        }
        if (sharing()) text_event("release:" + key);
    }

    // A box as an entry says it is, by key: made, changed or gone.
    void apply_text_state(const PaintTextBox& s, bool removed)
    {
        for (auto it = m_text.begin(); it != m_text.end(); ++it)
            if (it->key == s.key)
            {
                if (removed)
                {
                    if (m_text_sel == it->id) m_text_sel = 0;
                    m_text.erase(it);
                }
                else { const uint32_t id = it->id; *it = s; it->id = id; }
                Touch();
                return;
            }
        if (removed) return;
        PaintTextBox b = s;
        b.id = ++m_text_seq;
        m_text.push_back(b);
        Touch();
    }

    /*
     * THE BOXES AS OF A POINT IN HISTORY: every Text entry on the path, in
     * order, each the last word on its key. The boxes that stay keep their
     * numbers, so nothing holding one is surprised; the open box closes if its
     * key is gone.
     */
    bool rebuild_text(const std::vector<const PaintOp*>& chain)
    {
        std::vector<PaintTextBox> next;
        for (const PaintOp* o : chain)
        {
            if (o->kind != PaintOpKind::Text) continue;
            auto it = std::find_if(next.begin(), next.end(),
                                   [&](const PaintTextBox& t) { return t.key == o->box.key; });
            if (o->removed) { if (it != next.end()) next.erase(it); }
            else if (it != next.end()) *it = o->box;
            else next.push_back(o->box);
        }
        bool changed = next.size() != m_text.size();
        for (PaintTextBox& b : next)
        {
            const PaintTextBox* was = nullptr;
            for (const auto& t : m_text) if (t.key == b.key) was = &t;
            b.id = was ? was->id : ++m_text_seq;
            if (!was || !was->same_as(b)) changed = true;
        }
        m_text = std::move(next);
        if (m_text_sel && !FindTextBox(m_text_sel)) m_text_sel = 0;
        return changed;
    }

    // True, and said once, when this document is view only (SetReadOnly).
    bool refuse_read_only(const char* what) const
    {
        if (!readOnly()) return false;
        ETCS_LOG("PaintDocument", what << ": view only -- the host has not given this page drawing.");
        return true;
    }

    // The Page entry and the keyframes after it -- see ExportBaseline.
    size_t write_baseline(std::ostream& o) const
    {
        std::vector<PaintLayer*> stack;
        OrderedLayers(stack);
        PaintOp page;
        page.kind   = PaintOpKind::Page;
        page.seq    = m_cursor;
        page.author = m_author;
        page.w = m_width; page.h = m_height;
        for (PaintLayer* l : stack)
            page.roster.push_back(PaintOp::Face{ l->order(), l->opacity(), l->visible(), l->name() });
        o << paint_op_encode(page) << "\n";
        size_t n = 1;
        for (PaintLayer* l : stack)
        {
            PaintOp snap;
            snap.kind   = PaintOpKind::Snapshot;
            snap.seq    = m_cursor;
            snap.layer  = l->getRID();
            snap.order  = l->order();
            snap.author = m_author;
            snap.w      = l->PixelWidth();
            snap.h      = l->PixelHeight();
            if (!l->SnapshotBytes(snap.bytes)) continue;
            o << paint_op_encode(snap) << "\n";
            ++n;
        }
        // The boxes, as statements rather than edits: keyframes, which undo on
        // the other side steps over.
        for (const PaintTextBox& b : m_text)
        {
            PaintOp t;
            t.kind = PaintOpKind::Text;
            t.seq = m_cursor;
            t.author = m_author;
            t.box = b;
            t.box.id = 0;
            t.keyframe = true;
            o << paint_op_encode(t) << "\n";
            ++n;
        }
        return n;
    }

    // Be the page a Page entry describes -- see AcceptOp.
    void become_page(const PaintOp& page)
    {
        if (m_sel.lifted()) DropSelection();
        renew(page.w, page.h);            // clears layers, text and history
        reconcileLayers(&page);
        for (PaintLayer* l : layers())
            if (l->PixelWidth() != page.w || l->PixelHeight() != page.h) l->Allocate(page.w, page.h);
        ETCS_LOG("PaintDocument", "following the session's page: " << page.w << "x" << page.h
                 << ", " << page.roster.size() << " layer(s).");
    }

    /*
     * `carries` is a layer whose RASTER changed as part of this structural act,
     * and a merge is the reason it exists. The merge's pixel effect has to live
     * on the SAME entry as the roster or the two fall either side of the undo
     * that walks over them: a separate keyframe before the roster is what an
     * undo lands on (so the merge appears not to come off), and one after it is
     * never reached by a redo (so the merge comes back with the paint missing).
     * One entry, one step, both halves.
     */
    void appendRoster(bool keyframe = false, PaintLayer* carries = nullptr)
    {
        std::vector<PaintLayer*> stack;
        OrderedLayers(stack);
        PaintOp op;
        op.kind     = PaintOpKind::Layers;
        op.keyframe = keyframe;
        op.author   = m_author;
        if (carries)
        {
            op.layer = carries->getRID();
            op.order = carries->order();
            op.w     = carries->PixelWidth();
            op.h     = carries->PixelHeight();
            carries->SnapshotBytes(op.bytes);
        }
        for (PaintLayer* l : stack)
            op.roster.push_back(PaintOp::Face{ l->order(), l->opacity(), l->visible(), l->name() });
        m_cursor = m_book.Append(std::move(op), m_cursor);
        Touch();
    }

    void appendSnapshot(PaintLayer* layer)
    {
        if (!layer) return;
        PaintOp snap;
        snap.kind   = PaintOpKind::Snapshot;
        snap.layer  = layer->getRID();
        snap.order  = layer->order();
        snap.author = m_author;
        snap.w      = layer->PixelWidth();
        snap.h      = layer->PixelHeight();
        if (!layer->SnapshotBytes(snap.bytes)) return;
        m_cursor = m_book.Append(std::move(snap), m_cursor);
    }

    PaintLayer* layerByRID(ETCS::RID rid) const
    {
        std::vector<PaintLayer*> layers;
        OrderedLayers(layers);
        for (PaintLayer* l : layers) if (l->getRID() == rid) return l;
        return nullptr;
    }

    /*
     * THE RID, THEN THE ORDER, AND THEN ALMOST NEVER ANYTHING ELSE.
     *
     * See PaintOp::layer for why an entry names its layer twice. The last
     * fallback exists for one case -- a viewer with a single layer should show
     * a host's strokes on it rather than show nothing -- and it is fenced to
     * exactly that case, because the general version of it is destructive.
     *
     * WHAT IT COST BEFORE THE FENCE: merge a layer away, then undo. The dead
     * layer's RID stops resolving, its order matches nothing, and its own
     * KEYFRAME -- an empty raster -- was restored onto whatever happened to be
     * active. The survivor came back blank, which looks exactly like undo
     * erasing the picture and is nothing of the kind.
     *
     * So a snapshot never falls back: it names one specific raster and putting
     * it on a different one destroys that one. A mark falls back only where
     * there is no other layer it could have meant.
     */
    PaintLayer* layerFor(const PaintOp& op) const
    {
        if (PaintLayer* l = layerByRID(op.layer)) return l;
        std::vector<PaintLayer*> layers;
        OrderedLayers(layers);
        for (PaintLayer* l : layers) if (l->order() == op.order) return l;
        if (op.kind == PaintOpKind::Snapshot) return nullptr;
        return (layers.size() == 1) ? layers.front() : nullptr;
    }

    /*
     * BE THE PICTURE AS OF seq. For every layer the notebook has touched:
     * restore its newest snapshot at or before seq, then replay that layer's
     * marks from there forward.
     *
     * PER LAYER rather than over the whole log, because a snapshot is a
     * layer's and replaying another layer's marks onto it would be wrong in
     * the one case that matters -- two layers painted alternately, which is
     * ordinary work rather than a corner.
     *
     * A layer with no snapshot at or before seq is left alone and said so:
     * its recorded past begins later than the point being asked for, so
     * there is nothing this function could restore it to that would be more
     * correct than what is already on it.
     */
    /*
     * BE THE STACK THE ROSTER DESCRIBES, before a single raster is restored.
     *
     * ORDER IS THE IDENTITY. A layer brought back by an undo is a new entity
     * with a new RID, because a detached one cannot be re-registered as a typed
     * child -- and that is the right answer rather than a compromise: a RID is a
     * causal position in the runtime, so it does not survive moving to a
     * different position in causal history. Position is what a script names and
     * what an entry's `order` field already carries, which is why the raster
     * phase after this resolves by order without anything having to be rewritten.
     */
    void reconcileLayers(const PaintOp* roster)
    {
        if (!roster) return;

        std::vector<PaintLayer*> stack;
        OrderedLayers(stack);

        // Anything at a position the roster does not mention is not in this
        // picture. Detached rather than deleted, exactly as RemoveLayer does.
        for (PaintLayer* l : stack)
        {
            bool wanted = false;
            for (const PaintOp::Face& f : roster->roster) if (f.order == l->order()) { wanted = true; break; }
            if (wanted) continue;
            if (m_active_layer == l) m_active_layer = nullptr;
            l->detachFromParent();
        }

        // And anything the roster names that is not here comes back -- empty,
        // because the keyframe that fills it is the next phase's job.
        for (const PaintOp::Face& f : roster->roster)
        {
            OrderedLayers(stack);
            PaintLayer* at = nullptr;
            for (PaintLayer* l : stack) if (l->order() == f.order) { at = l; break; }
            if (!at)
            {
                at = this->addTag<PaintLayer>();
                if (!at) continue;
                at->Create(m_width, m_height);
                at->Clear(0.0f, 0.0f, 0.0f, 0.0f);
            }
            at->SetOrder(f.order);
            at->SetName(f.name);
            at->SetOpacity(f.opacity);
            at->SetVisible(f.visible);
        }

        OrderedLayers(stack);
        if (!m_active_layer && !stack.empty()) m_active_layer = stack.back();
    }

    bool replayTo(uint64_t seq, const char* what)
    {
        if (m_sel.lifted()) DropSelection();

        // THE CANONICAL PATH, and only it. Sequence order stopped being the
        // answer when a second branch became possible: entries numbered
        // between a keyframe and the target may belong to a sibling, and
        // replaying those paints a picture nobody ever made.
        // AS THE RECORD READS IT: the canonical path less what retractions on
        // it took back (PaintNotebook::EffectivePath). A local document with no
        // retractions gets the canonical path unchanged.
        std::vector<const PaintOp*> chain;
        m_book.EffectivePath(seq, chain);

        // STRUCTURE FIRST. The rasters below are restored onto the layers this
        // puts back; doing it the other way round restores pixels onto planes
        // that are about to be replaced.
        const PaintOp* roster = nullptr;
        for (const PaintOp* o : chain) if (o->structural()) roster = o;
        reconcileLayers(roster);

        /*
         * BY THE LAYER AN ENTRY RESOLVES TO, not the RID it names. An entry
         * from another member names THEIR layer's RID and lands here by order
         * (layerFor); grouping by the named RID put such entries in a group of
         * their own with no keyframe, and the group that restored the layer
         * they had actually been drawn on replayed only the entries naming it
         * -- so every undo in a room took the others' strokes off the layer
         * with it. Resolve once per entry, up front, and group by the answer.
         */
        std::vector<ETCS::RID> touched;
        std::vector<PaintLayer*> lands(chain.size(), nullptr);
        for (size_t i = 0; i < chain.size(); ++i)
        {
            const PaintOp* o = chain[i];
            // A structural entry names no layer UNLESS it carries one's bytes,
            // and a box never does (rebuild_text, below).
            if (o->structural() && o->bytes.empty()) continue;
            if (o->kind == PaintOpKind::Text) continue;
            lands[i] = layerFor(*o);
            if (!lands[i]) continue;
            const ETCS::RID rid = lands[i]->getRID();
            bool seen = false;
            for (ETCS::RID r : touched) if (r == rid) { seen = true; break; }
            if (!seen) touched.push_back(rid);
        }

        size_t replayed = 0, restored = 0;
        for (ETCS::RID rid : touched)
        {
            const PaintOp* base = nullptr;
            for (size_t i = 0; i < chain.size(); ++i)
            {
                const PaintOp* o = chain[i];
                const bool keyframes_it =
                    (o->kind == PaintOpKind::Snapshot || (o->structural() && !o->bytes.empty()))
                    && lands[i] && lands[i]->getRID() == rid;
                if (keyframes_it) base = o;
            }
            if (!base)
            {
                ETCS_LOG("PaintDocument", what << ": layer " << rid
                         << " has no keyframe on this path -- left as it is.");
                continue;
            }
            if (!ApplyOp(*base)) continue;
            ++restored;
            bool past = false;
            for (size_t i = 0; i < chain.size(); ++i)
            {
                const PaintOp* o = chain[i];
                if (!past) { if (o == base) past = true; continue; }
                if (!lands[i] || lands[i]->getRID() != rid || !o->marks()) continue;
                if (ApplyOp(*o)) ++replayed;
            }
        }

        // THE TEXT, from the same path: every box as its last entry left it.
        const bool text = rebuild_text(chain);

        Touch();
        ETCS_LOG("PaintDocument", what << " to " << seq << ": " << restored
                 << " layer(s) restored, " << replayed << " op(s) replayed along a "
                 << chain.size() << "-entry path" << (text ? ", text boxes changed" : "") << ".");
        return restored > 0 || text;
    }

    // What the paper is cleared to -- the page's convention, stated once here
    // for the two verbs that have to make new paper (Resize, New) rather than
    // read back from a layer that may have been painted on since.
    static constexpr float PAPER_CLEAR[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

    // The same ceiling a file is held to: a page bigger than this is an
    // allocation, not a picture (PAINT_IMAGE_MAX_SIDE). And then whether the
    // allocation fits: every layer at the new size, the mask, and the one old
    // layer Rebase holds while it copies -- asked once here, since a refusal
    // after the first layer has moved would leave the stack two sizes at once.
    bool extent_ok(uint32_t w, uint32_t h, const char* what) const
    {
        if (w == 0 || h == 0 || w > PAINT_IMAGE_MAX_SIDE || h > PAINT_IMAGE_MAX_SIDE)
        {
            ETCS_LOG("PaintDocument", what << " " << w << "x" << h << " refused -- 1.."
                     << PAINT_IMAGE_MAX_SIDE << " on a side.");
            return false;
        }
        std::vector<PaintLayer*> stack;
        OrderedLayers(stack);
        const size_t page = static_cast<size_t>(w) * h;
        const size_t need = page * 4 * (stack.size() + 1) + page;
        std::string why;
        if (paint_heap_can_take(need, why)) return true;
        ETCS_LOG("PaintDocument", what << " " << w << "x" << h << " refused -- " << why
                 << ". A smaller page, or fewer layers, fits.");
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

};

// Out of line because a layer's parent is a PaintDocument, which is declared
// after PaintLayer -- see the note on the declaration.
inline void PaintLayer::touch_document()
{
    ETCS::Entity* parent = this->getParent();
    // By TAG, not by dynamic_cast: getTrueType() hands back void* (the leaf is
    // reached through the wire, not through C++ inheritance), and a layer may
    // sit under something that is not a document in a test.
    if (!parent || !parent->hasTag(ETCS::Buffer("PaintDocument"))) return;
    static_cast<PaintDocument*>(parent->getTrueType())->Touch();
}


class PaintSurface : public DeletableBase<PaintSurface>,
                    public AnimatedBase<PaintSurface>
{
public:
    WIRE_TYPE_IDENTITY(PaintSurface);

    /*
 * ── the view follows the pane ────────────────────────────────────────────
 *
 * A compositor's ResizeTo is DEFERRED: it stages the extent and applies it on
 * its next recompose (CompositeDrawable2D::ResizeTo). So the pane is NOT the
 * new size at the moment anything asked it to be, and every caller that
 * resized it and then re-rendered -- the layout's solve, the boot script after
 * it, the page after a window resize -- drew the view for the extent the pane
 * was about to stop having. The picture itself survives that, because it is
 * re-projected from the document on the next render anyway; the ruler does
 * not. Its band is chrome on a RETAINED raster, so a band drawn against the
 * old extent simply stays where it was: a strip of wood lying across the
 * bottom of the page at the height the pane used to end.
 *
 * Waiting a frame is not a thing a script can say, and guessing an interval
 * from the page is not a thing it should have to. So the surface asks instead.
 * AnimatingConcrete is "the pane is not the size I last drew for"; one step later it
 * is, and the answer goes back to false. On a settled view that is one size
 * read per frame, which is the whole bargain the family offers
 * (ontology/Animated.h).
 */
    bool AnimatingConcrete() override
    {
        const WindowSize s = targetSize();
        return s.width != 0 && s.height != 0
            && (s.width != m_drawn_w || s.height != m_drawn_h);
    }

    void AdvanceConcrete(double) override { Render(); }

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
        // WHAT THIS FRAME IS FOR, recorded before it is drawn -- see AnimatingConcrete.
        {
            const WindowSize ts = targetSize();
            m_drawn_w = ts.width; m_drawn_h = ts.height;
        }
        m_document->RenderToSurface(m_target, m_pan_x, m_pan_y, m_zoom);
        draw_peer_views(view);
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

    /*
 * ── WHERE EVERYONE ELSE IS LOOKING ───────────────────────────────────────
 *
 * PRESENCE, NOT HISTORY, and that distinction is the whole reason this is a
 * list on the surface rather than an entry in the notebook. A camera position
 * changes on every pan and is worthless a second later: it has no causal
 * successor, so recording it would fill the one structure whose value is that
 * everything in it caused something. It is stored once, overwritten in place,
 * and never replayed.
 *
 * THE RECT IS DERIVED, NOT SENT. What crosses the wire is a document-space
 * rectangle; each page draws it through its OWN projection, so two people at
 * different zooms still see each other's frame in the right place on the
 * picture. Sending view-space pixels would mean a frame that is only correct
 * for the sender, which is the opposite of the point.
 *
 * ONE CALL PER PEER (SetPeer), because a roster of eight at forty bytes each
 * is over the 256-byte call buffer and a file for something this small would
 * be a bridge built for one crossing.
 */
    struct PeerView
    {
        std::string name;
        int32_t x = 0, y = 0, w = 0, h = 0;
        float   rgb[3] = { 0.8f, 0.8f, 0.8f };
    };

    void ClearPeers() { m_peers.clear(); }

    // Upsert by name: a peer that pans twice between two reads should move, not
    // appear twice.
    void SetPeer(const std::string& name, int32_t x, int32_t y, int32_t w, int32_t h,
                 float r, float g, float b)
    {
        if (name.empty()) return;
        for (PeerView& p : m_peers)
            if (p.name == name)
            {
                p.x = x; p.y = y; p.w = w; p.h = h;
                p.rgb[0] = r; p.rgb[1] = g; p.rgb[2] = b;
                return;
            }
        PeerView p;
        p.name = name; p.x = x; p.y = y; p.w = w; p.h = h;
        p.rgb[0] = r; p.rgb[1] = g; p.rgb[2] = b;
        m_peers.push_back(std::move(p));
    }

    size_t peerCount() const { return m_peers.size(); }

    // This pane's own visible rectangle IN DOCUMENT SPACE -- what a page sends
    // so everyone else can draw it. Derived from the projection rather than
    // remembered, so it cannot go stale behind a pan.
    void ViewRect(int32_t& x, int32_t& y, int32_t& w, int32_t& h)
    {
        const WindowSize ts = targetSize();
        x = ViewToDocX(0);
        y = ViewToDocY(0);
        w = ViewToDocX(static_cast<int32_t>(ts.width))  - x;
        h = ViewToDocY(static_cast<int32_t>(ts.height)) - y;
    }

private:
    /*
     * AN OUTLINE AND A NAME, not a filled rectangle. A translucent fill over
     * somebody else's frame tints the PICTURE inside it, and the picture is the
     * thing both of you are looking at -- so the one place a presence marker
     * must not be is on top of the work. Four edges and a label at the corner
     * says the same thing and costs the artwork nothing.
     */
    void draw_peer_views(Surface_* view)
    {
        if (!view || m_peers.empty()) return;
        for (const PeerView& p : m_peers)
        {
            const int32_t x0 = DocToViewX(p.x), y0 = DocToViewY(p.y);
            const int32_t x1 = DocToViewX(p.x + p.w), y1 = DocToViewY(p.y + p.h);
            const int32_t vx = std::min(x0, x1), vy = std::min(y0, y1);
            const int32_t vw = std::abs(x1 - x0), vh = std::abs(y1 - y0);
            if (vw <= 1 || vh <= 1) continue;

            const uint32_t uw = static_cast<uint32_t>(vw), uh = static_cast<uint32_t>(vh);
            const float r = p.rgb[0], g = p.rgb[1], b = p.rgb[2];
            view->DrawRect(vx, vy, uw, 2u, r, g, b, 0.85f);
            view->DrawRect(vx, vy + vh - 2, uw, 2u, r, g, b, 0.85f);
            view->DrawRect(vx, vy, 2u, uh, r, g, b, 0.85f);
            view->DrawRect(vx + vw - 2, vy, 2u, uh, r, g, b, 0.85f);

            // The name inside the top-left corner, so it stays with the frame
            // when the frame is half off the pane. Through the same provider the
            // ruler labels use -- a surface has one, and text landing IN the
            // raster rather than beside it as a node is what Glyphs_ is for.
            if (m_glyphs != 0)
                if (ETCS::Held<Glyphs_> gl = ETCS::resolve_held<Glyphs_>("Glyphs", m_glyphs))
                    gl->RasterizeText(m_target, p.name.c_str(), 0, 12,
                                      vx + 4, vy + 4, r, g, b, 0.95f);
        }
    }

    std::vector<PeerView> m_peers;

public:
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

    // THE RASTER THE EDGE RULER IS DRAWN ON: a surface the size of the frame
    // the drawable pane sits inside, at that frame's origin -- the pane's
    // Bounds() are read in the frame's space and used as offsets in this one,
    // so the two spaces must coincide. A node of its own rather than the frame
    // itself, because the frame is a compositor the frame edge rebuilds on its
    // thread while Render writes on the input thread: one buffer with two
    // writers and no order between them, which showed as the band flickering
    // over the toolbar during a drag (boot_paint_panels.etcs, ruler_pane).
    // Bound rather than derived from the tree, because where chrome belongs is
    // a composition choice. Unbound, the marks fall back inside the pane; see
    // draw_edge_ruler.
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
        scaled.size_px = std::max(1.0f, brush.size_px * m_zoom);
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
    // How far out from the pane the margin is cleared each frame, whatever
    // the band measures this frame (draw_edge_ruler's clear says why).
    static constexpr int32_t RULER_CLEAR_MAX = 64;

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

        /*
         * AND THE WHOLE FRAME IS WIPED WHEN THE GEOMETRY MOVES, only then.
         *
         * The per-side clears below cover the band as it is NOW, which was
         * enough while the pane only ever changed EXTENT under a fixed margin:
         * the old marks were inside the new band, so drawing the new one
         * covered them. Once the pane follows the window (the layout, in
         * boot_paint_panels.etcs) it can also GROW, and then the previous
         * band lies inside the new PANE area -- the one region this function
         * must never paint. Nothing clears it and it is chrome on a retained
         * raster stacked over the picture, so it stays: a strip of wood lying
         * across the bottom of the page at the height the pane used to end.
         *
         * ClearTo and not FillRect, because FillRect is source-over and
         * refuses alpha 0 (Pixels_), and transparent is exactly what the
         * pane's own area has to be on this raster.
         */
        if (outside)
        {
            const int32_t geo[6] = { ox, oy, pw, ph, dw, dh };
            if (::std::memcmp(geo, m_ruler_geo, sizeof(geo)) != 0)
            {
                if (Pixels_* rp = ETCS::resolve_in_family<Pixels_>("Pixels", dst_rid))
                    rp->ClearTo(0.0f, 0.0f, 0.0f, 0.0f);
                ::std::memcpy(m_ruler_geo, geo, sizeof(geo));
            }
        }

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
            //
            // CLEARED TO A FIXED WIDTH, NOT THE BAND'S. The band follows the
            // widest label, so it changes with the zoom -- 36px at 100%, 31px at
            // 125% -- and a clear that shrank with it left the previous width's
            // pixels standing beyond the new edge: the "1" of a "1024x768" drawn
            // at the wider band, sitting in front of the same extent drawn five
            // pixels further in. The margin is the ruler's whatever the band
            // measures, up to a cap so a page with a deep margin (the toolbar's
            // bottom) is not painted over to the frame's edge.
            const float* w = m_ruler_bg;
            const int32_t cl = std::clamp(ox, 0, RULER_CLEAR_MAX);
            const int32_t ct = std::clamp(oy, 0, RULER_CLEAR_MAX);
            const int32_t cr = std::clamp(dw - (ox + pw), 0, RULER_CLEAR_MAX);
            const int32_t cb = std::clamp(dh - (oy + ph), 0, RULER_CLEAR_MAX);
            const uint32_t span = static_cast<uint32_t>(cl + pw + cr);
            if (ct > 0) dst->DrawRect(ox - cl, oy - ct, span, static_cast<uint32_t>(ct),
                                      w[0], w[1], w[2], w[3]);
            if (cb > 0) dst->DrawRect(ox - cl, oy + ph, span, static_cast<uint32_t>(cb),
                                      w[0], w[1], w[2], w[3]);
            if (cl > 0) dst->DrawRect(ox - cl, oy, static_cast<uint32_t>(cl),
                                      static_cast<uint32_t>(ph), w[0], w[1], w[2], w[3]);
            if (cr > 0) dst->DrawRect(ox + pw, oy, static_cast<uint32_t>(cr),
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

        // The extent label first, since the top axis has to know where it ends:
        // a tick label that starts under the extent's tail is two numbers in one
        // place (the "150" through the "768"), so a label there is not drawn --
        // the tick still is, and the next label says where the count is.
        // THE DOCUMENT'S extent, not the pane's, because that is the unit the
        // two axes are counting. The pane's size is a fact about the window and
        // is not what anybody reading a scale wants.
        const std::string ext = m_document
            ? std::to_string(m_document->width()) + "x" + std::to_string(m_document->height())
            : std::to_string(pw) + "x" + std::to_string(ph);
        const int32_t ext_x = outside ? ox - bl + 2 : ox + major + 2;
        int32_t ext_end = 0;
        if (glyphs && outside)
            ext_end = ext_x + static_cast<int32_t>(glyphs->MeasureText(ext.c_str(), 0, label_px).width) + 4;

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
                // "0" under it is two numbers fighting for twelve pixels. Nor
                // one that would start under the extent's tail (ext_end).
                if (glyphs && d != 0 && bt >= band_h && cx + 3 >= ext_end)
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
            if (outside && bt >= band_h && bl > 0)
                glyphs->RasterizeText(dst_rid, ext.c_str(), 0, label_px,
                                      ext_x, oy - band_h + 2, r, g, b, a);
            else if (!outside)
                glyphs->RasterizeText(dst_rid, ext.c_str(), 0, label_px,
                                      ext_x, oy + major + 2, r, g, b, a);
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

        paint_node_text(m_zoom_label, std::to_string(pct) + "%");
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
    // The pane-on-frame geometry the band was last drawn for: ox, oy, pw, ph,
    // dw, dh. -1 so the first draw always wipes. See draw_edge_ruler.
    int32_t m_ruler_geo[6] = { -1, -1, -1, -1, -1, -1 };
    // The pane extent the last frame was drawn for -- see AnimatingConcrete.
    uint32_t m_drawn_w = 0, m_drawn_h = 0;

    WindowSize targetSize()
    {
        if (Resizable_* v = ETCS::resolve_in_family<Resizable_>("Resizable", m_target))
            return v->GetSize();
        return WindowSize{ 0, 0 };
    }
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
 * TWO DELETES, AND THE ONE WITH AN ARGUMENT IS THIS TYPE'S OWN: Delete(id)
 * removes a PAGE from the store, while the family's Delete() removes this
 * entity (DeletableBase). Declaring the first hides the second, which the
 * compiler reports on every build as an overloaded virtual going quiet -- so
 * the base's name is pulled back in and the two live as an overload set.
 * Nothing resolves differently: the page verb passes an id, the entity verb
 * passes nothing, and the tag block names them Delete and Destroy so a script
 * never has to know there were ever two.
 */
    using DeletableBase<PaintPages>::Delete;

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
            // THE THUMBNAIL, made at save time and kept beside the page rather
            // than derived from it: a list of pages wants a picture per row,
            // and decoding a page's layers to draw one is 6 MB of PAM per row
            // per refresh. Its own table so an older store gains it on the
            // next save without an ALTER.
            "CREATE TABLE IF NOT EXISTS page_thumbs("
            " page_id INTEGER PRIMARY KEY, width INTEGER NOT NULL, height INTEGER NOT NULL,"
            " rgba BLOB NOT NULL)",
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
        Waiting wait(*this);
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
        {
            std::vector<uint8_t> thumb;
            page_thumb(stack, THUMB_W, THUMB_H, thumb);
            Stmt th(*db, "INSERT OR REPLACE INTO page_thumbs(page_id, width, height, rgba) VALUES(?, ?, ?, ?)");
            if (!th || !th.bind(1, DatabaseValue::integer(m_current)) || !th.bind(2, DatabaseValue::integer(THUMB_W))
                || !th.bind(3, DatabaseValue::integer(THUMB_H))
                || !th.bind(4, DatabaseValue::blob(thumb.data(), thumb.size())) || th.step() < 0)
                return false;
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
        if (m_document && m_document->readOnly())
        {
            ETCS_LOG("PaintPages", "view only -- this page follows a shared session; leave it to switch pages.");
            return false;
        }
        if (!m_document) return false;
        Waiting wait(*this);
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
    // What the store holds, for anything that wants to SHOW the pages rather
    // than step through them -- the gear menu's list (PaintCanvasMenu).
    struct PageInfo { int64_t id = 0; std::string name; int64_t w = 0, h = 0, layers = 0; };

    // The stored thumbnail's size. 4:3 like the default page, and the width a
    // list row can give a picture beside a name.
    static constexpr int32_t THUMB_W = 32;
    static constexpr int32_t THUMB_H = 24;

    // The thumbnail saved with a page, or false for a page saved before there
    // were any (it gains one on its next save).
    bool Thumb(int64_t id, int32_t& w, int32_t& h, std::vector<uint8_t>& rgba)
    {
        ETCS::Held<Database_> db = ETCS::resolve_held<Database_>("Database", m_db);
        if (!db) return false;
        Stmt st(*db, "SELECT width, height, rgba FROM page_thumbs WHERE page_id = ?");
        if (!st || !st.bind(1, DatabaseValue::integer(id)) || st.step() != 1) return false;
        w = static_cast<int32_t>(st.col(0).i);
        h = static_cast<int32_t>(st.col(1).i);
        const DatabaseValue v = st.col(2);
        if (v.kind != DatabaseValue::Blob || !v.p || w <= 0 || h <= 0
            || v.n != static_cast<size_t>(w) * h * 4) return false;
        rgba.assign(static_cast<const uint8_t*>(v.p), static_cast<const uint8_t*>(v.p) + v.n);
        return true;
    }

    // A page's name in the store, and on the document too when it is the one
    // on screen -- otherwise the next save would write the old name back.
    bool Rename(int64_t id, const std::string& name)
    {
        if (name.empty()) return false;
        ETCS::Held<Database_> db = ETCS::resolve_held<Database_>("Database", m_db);
        if (!db) return false;
        Stmt up(*db, "UPDATE pages SET name = ? WHERE id = ?");
        if (!up || !up.bind(1, text(name)) || !up.bind(2, DatabaseValue::integer(id)) || up.step() < 0) return false;
        if (id == m_current && m_document) m_document->SetName(name);
        persist();
        ETCS_LOG("PaintPages", "page " << id << " renamed '" << name << "'");
        return true;
    }

    bool Pages(std::vector<PageInfo>& out)
    {
        out.clear();
        ETCS::Held<Database_> db = ETCS::resolve_held<Database_>("Database", m_db);
        if (!db) return false;
        Stmt st(*db, "SELECT p.id, p.name, p.width, p.height,"
                     " (SELECT COUNT(*) FROM page_layers l WHERE l.page_id = p.id)"
                     " FROM pages p ORDER BY p.id");
        if (!st) return false;
        for (int rc = st.step(); rc == 1; rc = st.step())
        {
            PageInfo info;
            info.id     = st.col(0).i;
            info.name   = str(st.col(1));
            info.w      = st.col(2).i;
            info.h      = st.col(3).i;
            info.layers = st.col(4).i;
            out.push_back(std::move(info));
        }
        return true;
    }

    bool New() { return NewAt(m_document ? m_document->width() : 0,
                              m_document ? m_document->height() : 0); }

    /*
 * A NEW PAGE AT A STATED SIZE. New() keeps whatever the present page is,
 * which is right for ctrl+PageDown off the end of the strip and wrong for the
 * gear menu, where the whole point of the two steppers is to say how big the
 * next canvas should be. Same page otherwise: the present is flushed to its
 * slot first, so "new" adds to the history rather than replacing what was
 * there -- which is what makes more than one page exist to switch between.
 */
    bool NewAt(uint32_t want_w, uint32_t want_h)
    {
        if (m_document && m_document->readOnly())
        {
            ETCS_LOG("PaintPages", "view only -- this page follows a shared session; leave it to switch pages.");
            return false;
        }
        if (!m_document) return false;
        Waiting wait(*this);
        flush();
        const uint32_t w = want_w ? want_w : (m_document->width() ? m_document->width() : 1024);
        const uint32_t h = want_h ? want_h : (m_document->height() ? m_document->height() : 768);
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

    /*
 * BUSY IS A FLAG THIS TYPE RAISES ON ITSELF for as long as a page operation
 * runs. A new page encodes and stores every layer of the one it leaves (6 MB
 * of PAM at 1024x768) and a load decodes as much back, all on the thread the
 * press arrived on -- a second or more in the browser during which the sheet
 * is still and the pointer is ignored, which is indistinguishable from a hang.
 *
 * A state tag rather than a verb at a bound node, because the store's job is
 * to say what state it is in, not to know what shows it. `busy` is a
 * lowercase entry in this entity's own tag store (Entity::addTag), ordered
 * within this module like any tag change; whatever wants to show the wait
 * observes it -- the session's throbber does (Throbber::Watch, bound in
 * boot_paint_panels.etcs), on the frame edge's own thread, and so would
 * anything else that watched. Nothing crosses a module boundary on the input
 * thread to make the indicator appear.
 *
 * Every operation that writes the store raises it, not only the page switch:
 * a Save is the same encode, and a Delete drops a page's megabytes of layer
 * rows -- a second of nothing after pressing an x is the same hang-shaped
 * silence.
 *
 * A depth rather than a bare raise because step() lands in Load() or NewAt(),
 * and the flag should come down when the outermost one does.
 */
    struct Waiting
    {
        PaintPages& p;
        explicit Waiting(PaintPages& pages) : p(pages) { if (p.m_wait_depth++ == 0) p.set_busy(true); }
        ~Waiting() { if (--p.m_wait_depth == 0) p.set_busy(false); }
    };
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

    /*
 * THE PRESENT PUT AWAY AND LET GO OF: saved to its slot if it changed, and
 * then in no slot at all, so whatever replaces it on screen is a new page
 * rather than an overwrite of this one. What joining a shared canvas does
 * first -- the joiner's page is kept, and the session's page arrives into a
 * document that no longer answers to that slot.
 */
    bool Stash()
    {
        if (!m_document) return false;
        if (dirty() && !Save()) return false;
        if (m_current != 0)
            ETCS_LOG("PaintPages", "page " << m_current << " put away; the present is in no slot now.");
        m_current = 0;
        return true;
    }

    // The slot goes; the picture on screen does not. Deleting the current page
    // leaves the present as an unsaved page, which the next Save gives a new
    // row -- the alternative, emptying the document, would make a wrong id
    // typed into Delete cost the work in front of you.
    bool Delete(int64_t id)
    {
        Waiting wait(*this);
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
        {
            Stmt del(*db, "DELETE FROM page_thumbs WHERE page_id = ?");
            if (!del || !del.bind(1, DatabaseValue::integer(id)) || del.step() < 0) return false;
        }
        if (!guard.commit()) return false;
        if (id == m_current) m_current = 0;
        ETCS_LOG("PaintPages", "deleted page " << id
                 << (m_current == 0 ? "; the present is no longer in a slot" : ""));
        return true;
    }

private:
    void set_busy(bool on)
    {
        if (on) this->addTag("busy");
        else    this->removeTag(ETCS::Buffer("busy"));
    }

    PaintDocument* m_document = nullptr;
    ETCS::RID      m_db = 0;
    ETCS::RID      m_surface = 0;      // repainted after a load -- see BindSurface
    int            m_wait_depth = 0;   // see Waiting
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

    /*
 * THE PICTURE OF A PAGE, small. Every visible layer, bottom first, each cell
 * of the thumbnail the average of the source block under it, composited in
 * order at the layer's opacity -- the same arithmetic PaintLayerPanel's
 * paint_thumb does for one layer, done for the stack, over the checker so a
 * transparent page still shows as a page. Written at save time, once.
 */
    static void page_thumb(const std::vector<PaintLayer*>& stack, int32_t tw, int32_t th,
                           std::vector<uint8_t>& out)
    {
        out.assign(static_cast<size_t>(tw) * th * 4, 0);
        std::vector<float> acc(static_cast<size_t>(tw) * th * 3, 0.0f);
        for (int32_t y = 0; y < th; ++y)
            for (int32_t x = 0; x < tw; ++x)
            {
                const bool light = (((x >> 2) + (y >> 2)) & 1) != 0;
                float* c = &acc[(static_cast<size_t>(y) * tw + x) * 3];
                c[0] = c[1] = c[2] = light ? 0.44f : 0.34f;
            }
        for (PaintLayer* l : stack)
        {
            if (!l || !l->visible()) continue;
            const int32_t lw = static_cast<int32_t>(l->width()), lh = static_cast<int32_t>(l->height());
            const uint8_t* src = l->PixelData();
            if (!src || lw <= 0 || lh <= 0) continue;
            const float opacity = std::clamp(l->opacity(), 0.0f, 1.0f);
            for (int32_t y = 0; y < th; ++y)
                for (int32_t x = 0; x < tw; ++x)
                {
                    const int32_t sx0 = x * lw / tw, sx1 = std::max(sx0 + 1, (x + 1) * lw / tw);
                    const int32_t sy0 = y * lh / th, sy1 = std::max(sy0 + 1, (y + 1) * lh / th);
                    const int32_t stepx = std::max(1, (sx1 - sx0) / 8), stepy = std::max(1, (sy1 - sy0) / 8);
                    float ar = 0, ag = 0, ab = 0, aa = 0; int taps = 0;
                    for (int32_t sy = sy0; sy < sy1 && sy < lh; sy += stepy)
                        for (int32_t sx = sx0; sx < sx1 && sx < lw; sx += stepx)
                        {
                            const uint8_t* sp = src + (static_cast<size_t>(sy) * lw + sx) * 4;
                            const float a = sp[3] / 255.0f;
                            ar += (sp[0] / 255.0f) * a; ag += (sp[1] / 255.0f) * a;
                            ab += (sp[2] / 255.0f) * a; aa += a; ++taps;
                        }
                    if (taps == 0 || aa <= 0.0f) continue;
                    const float cover = (aa / taps) * opacity;
                    float* c = &acc[(static_cast<size_t>(y) * tw + x) * 3];
                    c[0] = c[0] * (1.0f - cover) + (ar / aa) * cover;
                    c[1] = c[1] * (1.0f - cover) + (ag / aa) * cover;
                    c[2] = c[2] * (1.0f - cover) + (ab / aa) * cover;
                }
        }
        for (size_t i = 0; i < static_cast<size_t>(tw) * th; ++i)
        {
            out[i * 4 + 0] = paint_to_byte(acc[i * 3 + 0]);
            out[i * 4 + 1] = paint_to_byte(acc[i * 3 + 1]);
            out[i * 4 + 2] = paint_to_byte(acc[i * 3 + 2]);
            out[i * 4 + 3] = 255;
        }
    }

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
        if (m_document && m_document->readOnly())
        {
            ETCS_LOG("PaintPages", "view only -- this page follows a shared session; leave it to switch pages.");
            return false;
        }
        if (!m_document) return false;
        Waiting wait(*this);
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

    bool Contains(Point2D at) const
    {
        ETCS::Held<Drawable2D_> w = ETCS::resolve_held<Drawable2D_>("Drawable2D", m_window);
        if (!w) return false;
        const Rect2D b = w->Bounds();
        return at.x >= b.x && at.y >= b.y
            && at.x < b.x + static_cast<int32_t>(b.w) && at.y < b.y + static_cast<int32_t>(b.h);
    }

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

/*
 * ── PaintAnimation ───────────────────────────────────────────────────────
 *
 * FRAMES CUT FROM THE PICTURE, AND PUT BACK. A region of the page, chosen by
 * the animation tool's drag (PaintInput), and a sequence of frames the size
 * of that region: `snap` takes a frame from what is visible in the region
 * now, `put` lays the current frame back onto the active layer there. Between
 * those two the page is the drawing board and the frames are the reel -- draw,
 * snap, draw, snap, then scrub or play the reel in the window. A GIF comes in
 * as frames (ImportGif, stb's decoder, the region resized to the file's) and
 * the reel goes out as one (ExportGif, the encoder above).
 *
 * THE WINDOW IS THE LAYER WINDOW'S SHAPE AGAIN: rows of [thumb][#][x] from a
 * row script, a preview raster the current frame is fitted into, and controls
 * on the bar that are palette calls (PaintPalette::AddCall) to the verbs
 * below. This type maps the rows to frames and draws none of it.
 *
 * PLAYING IS ANIMATED, the family (ontology/Animated.h): while playing, the
 * frame edge advances the reel at the rate set, and the preview follows. dt
 * is honoured here, unlike the throbber's, because a rate in frames per
 * second is a real duration and "twelve a second" must mean the same on
 * every display.
 *
 * THE OUTLINE is four thin panes on the view (BindOutline), placed from the
 * region through the surface's projection and re-placed when the pan or the
 * zoom moves under them -- checked once a frame, so a drag of the picture
 * carries the outline with it.
 */
class PaintAnimation : public DeletableBase<PaintAnimation>,
                       public AnimatedBase<PaintAnimation>
{
public:
    WIRE_TYPE_IDENTITY(PaintAnimation);

    PaintAnimation() = default;
    bool DeleteConcrete() override { return true; }

    enum class Region : uint8_t { Body, Delete };

    static constexpr int32_t MIN_FPS = 1, MAX_FPS = 60;

    bool Create(ETCS::RID document)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintDocument", document);
        if (!raw) { ETCS_LOG("PaintAnimation", "Create: RID:" << document << " is not a PaintDocument."); return false; }
        m_document = static_cast<PaintDocument*>(raw->getTrueType());
        this->addTag("active");
        return true;
    }

    void BindSurface(ETCS::RID surface)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintSurface", surface);
        if (raw) m_surface = static_cast<PaintSurface*>(raw->getTrueType());
    }

    // The window (shown when a region exists), the preview raster, and the
    // readout label ("12 fps  3/8").
    void BindWindow(ETCS::RID pane)   { m_window = pane; paint_node_hidden(pane, !m_has_region); }
    void BindPreview(ETCS::RID node)  { m_preview = node; }
    void BindReadout(ETCS::RID node)  { m_readout = node; }
    // The four outline bars, in the order top, bottom, left, right.
    void BindOutline(ETCS::RID node)  { if (node && m_outline.size() < 4) m_outline.push_back(node); }

    void BeginRow() { m_rows.push_back(Row{}); }
    void RowNode(const std::string& what, ETCS::RID node)
    {
        if (node == 0) return;
        if (m_rows.empty()) { ETCS_LOG("PaintAnimation", "RowNode before BeginRow -- ignored."); return; }
        const size_t idx = m_rows.size() - 1;
        Row& row = m_rows[idx];
        if      (what == "bg")    { row.bg    = node; m_regions[node] = Hit{ idx, Region::Body }; }
        else if (what == "thumb") { row.thumb = node; m_regions[node] = Hit{ idx, Region::Body }; }
        else if (what == "label") { row.label = node; m_regions[node] = Hit{ idx, Region::Body }; }
        else if (what == "del")   { row.del   = node; m_regions[node] = Hit{ idx, Region::Delete }; }
        else ETCS_LOG("PaintAnimation", "RowNode: '" << what << "' is not a part of a row "
                      "(bg thumb label del) -- RID:" << node << " is attached to nothing.");
    }

    void SetRowColors(float sr, float sg, float sb, float ur, float ug, float ub)
    {
        m_row_sel[0] = sr; m_row_sel[1] = sg; m_row_sel[2] = sb;
        m_row_idle[0] = ur; m_row_idle[1] = ug; m_row_idle[2] = ub;
    }

    // ── the region ───────────────────────────────────────────────────────

    /*
 * The region, in document pixels. Normalised and clipped to the page; a
 * region under 2x2 is a click, not a frame, and clears nothing -- the tool's
 * drag has to mean something before it replaces what was there. A change of
 * SIZE drops the frames, because a frame is the region's size by definition
 * and a reel of mixed sizes is not one reel; a move keeps them.
 */
    bool SetRegion(int32_t x0, int32_t y0, int32_t x1, int32_t y1)
    {
        if (!m_document) return false;
        int32_t lx = std::min(x0, x1), rx = std::max(x0, x1);
        int32_t ty = std::min(y0, y1), by = std::max(y0, y1);
        lx = std::clamp(lx, 0, static_cast<int32_t>(m_document->width()));
        rx = std::clamp(rx, 0, static_cast<int32_t>(m_document->width()));
        ty = std::clamp(ty, 0, static_cast<int32_t>(m_document->height()));
        by = std::clamp(by, 0, static_cast<int32_t>(m_document->height()));
        if (rx - lx < 2 || by - ty < 2) return false;
        const uint32_t w = static_cast<uint32_t>(rx - lx), h = static_cast<uint32_t>(by - ty);
        if (m_has_region && (w != m_w || h != m_h) && !m_frames.empty())
        {
            ETCS_LOG("PaintAnimation", "region resized " << m_w << "x" << m_h << " -> " << w << "x" << h
                     << "; " << m_frames.size() << " frame(s) of the old size dropped.");
            m_frames.clear();
            m_at = 0;
        }
        m_x = lx; m_y = ty; m_w = w; m_h = h;
        m_has_region = true;
        paint_node_hidden(m_window, false);
        place_outline(true);
        Refresh();
        ETCS_LOG("PaintAnimation", "region " << m_w << "x" << m_h << " at " << m_x << "," << m_y);
        return true;
    }

    bool hasRegion() const { return m_has_region; }

    // ── the reel ─────────────────────────────────────────────────────────

    // A frame from what is visible in the region now, after the current one
    // (so snapping in sequence builds the reel in order), and it becomes the
    // current frame.
    bool Snap()
    {
        if (!m_document || !m_has_region) { ETCS_LOG("PaintAnimation", "snap: no region -- drag one with the animation tool."); return false; }
        std::vector<uint8_t> px;
        if (!m_document->CompositeVisible(px)) return false;
        const uint32_t dw = m_document->width();
        PaintImage f; f.w = m_w; f.h = m_h; f.rgba.resize(static_cast<size_t>(m_w) * m_h * 4);
        for (uint32_t y = 0; y < m_h; ++y)
            std::memcpy(f.rgba.data() + static_cast<size_t>(y) * m_w * 4,
                        px.data() + (static_cast<size_t>(m_y + y) * dw + m_x) * 4,
                        static_cast<size_t>(m_w) * 4);
        const size_t at = m_frames.empty() ? 0 : std::min(m_frames.size(), m_at + 1);
        m_frames.insert(m_frames.begin() + static_cast<std::ptrdiff_t>(at), std::move(f));
        m_at = at;
        Refresh();
        ETCS_LOG("PaintAnimation", "snapped frame " << (m_at + 1) << " of " << m_frames.size());
        return true;
    }

    // The current frame onto the active layer, in the region, as one
    // undoable step (PaintDocument::PastePixels).
    bool Put()
    {
        if (!m_document || m_frames.empty() || !m_has_region) return false;
        const PaintImage& f = m_frames[m_at];
        if (!m_document->PastePixels(f.rgba.data(), f.w, f.h, m_x, m_y)) return false;
        if (m_surface) m_surface->Render();
        ETCS_LOG("PaintAnimation", "put frame " << (m_at + 1) << " at " << m_x << "," << m_y);
        return true;
    }

    void Remove(size_t index)
    {
        if (index >= m_frames.size()) return;
        m_frames.erase(m_frames.begin() + static_cast<std::ptrdiff_t>(index));
        if (m_at >= m_frames.size()) m_at = m_frames.empty() ? 0 : m_frames.size() - 1;
        Refresh();
    }

    void Select(size_t index) { if (index < m_frames.size()) { m_at = index; Refresh(); } }
    void Next() { if (!m_frames.empty()) { m_at = (m_at + 1) % m_frames.size(); Refresh(); } }
    void Prev() { if (!m_frames.empty()) { m_at = (m_at + m_frames.size() - 1) % m_frames.size(); Refresh(); } }

    void SetFps(int32_t fps) { m_fps = std::clamp(fps, MIN_FPS, MAX_FPS); Refresh(); }
    void StepFps(int32_t by) { SetFps(m_fps + by); }
    void Play()  { m_playing = !m_frames.empty(); m_clock = 0.0; Refresh(); }
    void Pause() { m_playing = false; Refresh(); }
    void Toggle() { if (m_playing) Pause(); else Play(); }

    /*
 * A GIF's frames become the reel and its size becomes the region's, at the
 * region's corner (or the page's, with no region yet): the file is the
 * authority on its own size. Its delay sets the rate.
 */
    bool ImportGif(const std::string& path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) { ETCS_LOG("PaintAnimation", "import " << path << ": cannot open."); return false; }
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::vector<PaintImage> frames; int delay_ms = 100; std::string why;
        if (!paint_gif::decode(bytes.data(), bytes.size(), frames, delay_ms, why))
        { ETCS_LOG("PaintAnimation", "import " << path << ": " << why); return false; }
        if (!m_document) return false;
        const uint32_t w = frames[0].w, h = frames[0].h;
        const int32_t x = m_has_region ? m_x : 0, y = m_has_region ? m_y : 0;
        if (x + static_cast<int32_t>(w) > static_cast<int32_t>(m_document->width())
            || y + static_cast<int32_t>(h) > static_cast<int32_t>(m_document->height()))
        {
            ETCS_LOG("PaintAnimation", "import " << path << ": " << w << "x" << h << " does not fit the page at "
                     << x << "," << y << " -- resize the page or move the region.");
            return false;
        }
        m_frames = std::move(frames);
        m_at = 0;
        m_x = x; m_y = y; m_w = w; m_h = h; m_has_region = true;
        m_fps = std::clamp(static_cast<int32_t>(std::lround(1000.0 / std::max(1, delay_ms))), MIN_FPS, MAX_FPS);
        paint_node_hidden(m_window, false);
        place_outline(true);
        Refresh();
        ETCS_LOG("PaintAnimation", "imported " << path << ": " << m_frames.size() << " frame(s) " << w << "x" << h
                 << " at " << m_fps << " fps");
        return true;
    }

    // The reel to the user as a file. In the browser that is the page's job
    // -- it reads what ExportGif wrote and hands it to the download -- so this
    // raises the same event the menu's file verbs do (PaintCanvasMenu::
    // page_event) and the page calls ExportGif with a path of its own.
    void Download()
    {
#if defined(__EMSCRIPTEN__)
        MAIN_THREAD_EM_ASM({
            window.dispatchEvent(new CustomEvent('etcs-menu', { detail: 'gif' }));
        });
#else
        ETCS_LOG("PaintAnimation", "download: no file dialog on this substrate. From the terminal: anim.ExportGif(<path>)");
#endif
    }

    bool ExportGif(const std::string& path)
    {
        if (m_frames.empty()) { ETCS_LOG("PaintAnimation", "export " << path << ": no frames."); return false; }
        std::vector<uint8_t> out; std::string why;
        const uint32_t delay_cs = static_cast<uint32_t>(std::max(1, static_cast<int>(std::lround(100.0 / m_fps))));
        if (!paint_gif::encode(m_frames, delay_cs, out, why))
        { ETCS_LOG("PaintAnimation", "export " << path << ": " << why); return false; }
        std::ofstream o(path, std::ios::binary | std::ios::trunc);
        if (!o) { ETCS_LOG("PaintAnimation", "export " << path << ": cannot open for writing."); return false; }
        o.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
        ETCS_LOG("PaintAnimation", "exported " << path << ": " << m_frames.size() << " frame(s) " << m_w << "x" << m_h
                 << ", " << out.size() << " bytes, " << m_fps << " fps");
        return true;
    }

    // ── the rows ─────────────────────────────────────────────────────────

    bool owns(ETCS::RID node) const { return node != 0 && m_regions.find(node) != m_regions.end(); }

    bool Contains(Point2D at) const
    {
        ETCS::Held<Drawable2D_> w = ETCS::resolve_held<Drawable2D_>("Drawable2D", m_window);
        if (!w) return false;
        const Rect2D b = w->Bounds();
        return at.x >= b.x && at.y >= b.y
            && at.x < b.x + static_cast<int32_t>(b.w) && at.y < b.y + static_cast<int32_t>(b.h);
    }

    void Scroll(int32_t delta)
    {
        const int32_t rows = static_cast<int32_t>(m_rows.size());
        const int32_t total = static_cast<int32_t>(m_frames.size());
        const int32_t most = (total > rows) ? (total - rows) : 0;
        m_scroll = std::clamp(m_scroll + delta, 0, most);
        Refresh();
    }

    bool Apply(ETCS::RID node, bool is_press)
    {
        auto it = m_regions.find(node);
        if (it == m_regions.end()) return false;
        if (!is_press) return true;
        const Hit hit = it->second;
        if (hit.row >= m_rows.size()) return true;
        const size_t index = static_cast<size_t>(m_scroll) + hit.row;
        if (index >= m_frames.size()) return true;
        if (hit.region == Region::Delete) Remove(index);
        else                              Select(index);
        return true;
    }

    /*
 * Everything the window says, restated: the rows against the reel from the
 * scroll offset, the current one highlighted, the preview fitted with the
 * current frame, the readout. After any change rather than on a clock --
 * except while playing, when Advance calls it once per frame step.
 */
    void Refresh()
    {
        {
            const int32_t most = (m_frames.size() > m_rows.size())
                               ? static_cast<int32_t>(m_frames.size() - m_rows.size()) : 0;
            m_scroll = std::clamp(m_scroll, 0, most);
        }
        // Keep the current frame in the window while playing.
        if (m_playing && !m_rows.empty())
        {
            if (m_at < static_cast<size_t>(m_scroll)) m_scroll = static_cast<int32_t>(m_at);
            else if (m_at >= static_cast<size_t>(m_scroll) + m_rows.size())
                m_scroll = static_cast<int32_t>(m_at + 1 - m_rows.size());
        }
        for (size_t i = 0; i < m_rows.size(); ++i)
        {
            Row& row = m_rows[i];
            const size_t at = static_cast<size_t>(m_scroll) + i;
            if (at >= m_frames.size())
            {
                paint_node_fill(row.bg, m_row_idle[0], m_row_idle[1], m_row_idle[2], 0.0f);
                paint_node_hidden(row.thumb, true);
                paint_node_hidden(row.del, true);
                paint_node_text(row.label, "");
                continue;
            }
            const float* c = (at == m_at) ? m_row_sel : m_row_idle;
            paint_node_fill(row.bg, c[0], c[1], c[2], 1.0f);
            paint_node_hidden(row.thumb, false);
            paint_node_hidden(row.del, false);
            paint_fit(row.thumb, m_frames[at]);
            paint_node_text(row.label, std::to_string(at + 1));
        }
        if (m_preview)
        {
            if (m_frames.empty()) paint_fit(m_preview, PaintImage{});
            else                  paint_fit(m_preview, m_frames[m_at]);
        }
        if (m_readout)
        {
            std::string t = std::to_string(m_fps) + " fps  ";
            t += m_frames.empty() ? "no frames" : (std::to_string(m_at + 1) + "/" + std::to_string(m_frames.size()));
            if (m_playing) t += "  playing";
            paint_node_text(m_readout, t);
        }
    }

    void Report() const
    {
        ETCS_LOG("PaintAnimation", (m_has_region ? std::to_string(m_w) + "x" + std::to_string(m_h) + " at "
                                    + std::to_string(m_x) + "," + std::to_string(m_y) : std::string("no region"))
                 << ", " << m_frames.size() << " frame(s), at " << (m_frames.empty() ? 0 : m_at + 1)
                 << ", " << m_fps << " fps" << (m_playing ? ", playing" : ""));
    }

    // ── Animated_ ────────────────────────────────────────────────────────

    bool AnimatingConcrete() override { return m_playing && m_frames.size() > 1; }

    // dt honoured: the rate is a duration. The outline is re-placed here too,
    // playing or not -- see the header -- which is why Animating answers true
    // only while playing: one virtual call a frame is the cost of the check.
    void AdvanceConcrete(double dt_ms) override
    {
        if (!m_playing || m_frames.size() < 2) return;
        m_clock += dt_ms;
        const double per = 1000.0 / m_fps;
        bool stepped = false;
        while (m_clock >= per) { m_clock -= per; m_at = (m_at + 1) % m_frames.size(); stepped = true; }
        if (stepped) Refresh();
    }

    // Called by the surface's owner each render (PaintInput::repaint_view) so
    // the outline follows a pan or a zoom.
    void FollowView() { place_outline(false); }

private:
    struct Row { ETCS::RID bg = 0, thumb = 0, label = 0, del = 0; };
    struct Hit { size_t row; Region region; };

    // The frame fitted into a raster, box-averaged, over the checker; an empty
    // frame is the checker alone.
    static void paint_fit(ETCS::RID node, const PaintImage& f)
    {
        if (node == 0) return;
        Pixels_* dst = ETCS::resolve_in_family<Pixels_>("Pixels", node);
        if (!dst) return;
        uint8_t* out = dst->PixelData();
        if (!out) return;
        const int32_t tw = static_cast<int32_t>(dst->PixelWidth()), th = static_cast<int32_t>(dst->PixelHeight());
        if (tw <= 0 || th <= 0) return;
        const int32_t lw = static_cast<int32_t>(f.w), lh = static_cast<int32_t>(f.h);
        const bool have = lw > 0 && lh > 0 && f.rgba.size() >= static_cast<size_t>(lw) * lh * 4;
        const float scale = have ? std::max(static_cast<float>(lw) / tw, static_cast<float>(lh) / th) : 0.0f;
        const int32_t ox = have ? (tw - static_cast<int32_t>(lw / scale)) / 2 : 0;
        const int32_t oy = have ? (th - static_cast<int32_t>(lh / scale)) / 2 : 0;
        for (int32_t y = 0; y < th; ++y)
            for (int32_t x = 0; x < tw; ++x)
            {
                const bool light = (((x >> 2) + (y >> 2)) & 1) != 0;
                float r = light ? 0.44f : 0.34f, g = r, b = r;
                if (have)
                {
                    const int32_t sx0 = static_cast<int32_t>((x - ox) * scale), sy0 = static_cast<int32_t>((y - oy) * scale);
                    const int32_t sx1 = std::min(static_cast<int32_t>((x - ox + 1) * scale), lw);
                    const int32_t sy1 = std::min(static_cast<int32_t>((y - oy + 1) * scale), lh);
                    if (sx0 >= 0 && sy0 >= 0 && sx0 < lw && sy0 < lh && sx1 > sx0 && sy1 > sy0)
                    {
                        const int32_t stepx = std::max(1, (sx1 - sx0) / 8), stepy = std::max(1, (sy1 - sy0) / 8);
                        float ar = 0, ag = 0, ab = 0, aa = 0; int taps = 0;
                        for (int32_t sy = sy0; sy < sy1; sy += stepy)
                            for (int32_t sx = sx0; sx < sx1; sx += stepx)
                            {
                                const uint8_t* sp = f.rgba.data() + (static_cast<size_t>(sy) * lw + sx) * 4;
                                const float a = sp[3] / 255.0f;
                                ar += (sp[0] / 255.0f) * a; ag += (sp[1] / 255.0f) * a; ab += (sp[2] / 255.0f) * a; aa += a; ++taps;
                            }
                        if (taps > 0 && aa > 0.0f)
                        {
                            const float cover = aa / taps;
                            r = r * (1.0f - cover) + (ar / aa) * cover;
                            g = g * (1.0f - cover) + (ag / aa) * cover;
                            b = b * (1.0f - cover) + (ab / aa) * cover;
                        }
                    }
                }
                uint8_t* dp = out + (static_cast<size_t>(y) * tw + x) * 4;
                dp[0] = paint_to_byte(r); dp[1] = paint_to_byte(g); dp[2] = paint_to_byte(b); dp[3] = 255;
            }
        etcs_mark_observed(dst);
    }

    /*
 * The four bars around the region, in the SHEET's space: the view's origin
 * plus the region projected through pan and zoom, clipped to the view so a
 * region panned half off the page does not draw its edge over the ruler.
 * On the sheet and not in the view pane, because the view pane is the
 * surface's raster -- retained and cleared by Render -- and a child of a pane
 * that somebody else clears is drawn only when the tree changes, which is why
 * every pane the surface writes has no children (paint_layers.etcs says the
 * same of the thumbs). Skipped when nothing moved since the last placement
 * unless forced: each bar is three verbs across a module boundary.
 */
    void place_outline(bool force)
    {
        if (m_outline.size() < 4 || !m_surface) return;
        if (!m_has_region) { for (ETCS::RID n : m_outline) paint_node_hidden(n, true); return; }
        Rect2D pane{ 0, 0, 0, 0 };
        {
            ETCS::Held<Drawable2D_> v = ETCS::resolve_held<Drawable2D_>("Drawable2D", m_surface->target());
            if (!v) return;
            pane = v->Bounds();
        }
        const int32_t pw = static_cast<int32_t>(pane.w), ph = static_cast<int32_t>(pane.h);
        const int32_t vx0 = std::clamp(m_surface->DocToViewX(m_x), 0, pw);
        const int32_t vy0 = std::clamp(m_surface->DocToViewY(m_y), 0, ph);
        const int32_t vx1 = std::clamp(m_surface->DocToViewX(m_x + static_cast<int32_t>(m_w)), 0, pw);
        const int32_t vy1 = std::clamp(m_surface->DocToViewY(m_y + static_cast<int32_t>(m_h)), 0, ph);
        if (!force && vx0 == m_ox0 && vy0 == m_oy0 && vx1 == m_ox1 && vy1 == m_oy1
            && pane.x == m_opx && pane.y == m_opy) return;
        m_ox0 = vx0; m_oy0 = vy0; m_ox1 = vx1; m_oy1 = vy1; m_opx = pane.x; m_opy = pane.y;
        const int32_t t = 2;
        const bool visible = (vx1 - vx0) >= t && (vy1 - vy0) >= t;
        const uint32_t w = static_cast<uint32_t>(std::max(t, vx1 - vx0)), h = static_cast<uint32_t>(std::max(t, vy1 - vy0));
        auto bar = [&](ETCS::RID n, int32_t x, int32_t y, uint32_t bw, uint32_t bh)
        {
            paint_node_hidden(n, !visible);
            paint_node_verb(n, "ResizeTo", std::to_string(bw) + ", " + std::to_string(bh));
            paint_node_verb(n, "MoveTo", std::to_string(pane.x + x) + ", " + std::to_string(pane.y + y));
        };
        bar(m_outline[0], vx0, vy0, w, t);
        bar(m_outline[1], vx0, vy1 - t, w, t);
        bar(m_outline[2], vx0, vy0, t, h);
        bar(m_outline[3], vx1 - t, vy0, t, h);
    }

    PaintDocument* m_document = nullptr;
    PaintSurface*  m_surface  = nullptr;
    ETCS::RID m_window = 0, m_preview = 0, m_readout = 0;
    std::vector<ETCS::RID> m_outline;
    int32_t m_ox0 = INT32_MIN, m_oy0 = 0, m_ox1 = 0, m_oy1 = 0, m_opx = 0, m_opy = 0;

    bool     m_has_region = false;
    int32_t  m_x = 0, m_y = 0;
    uint32_t m_w = 0, m_h = 0;

    std::vector<PaintImage> m_frames;
    size_t   m_at = 0;
    int32_t  m_fps = 12;
    bool     m_playing = false;
    double   m_clock = 0.0;

    std::vector<Row> m_rows;
    std::unordered_map<ETCS::RID, Hit> m_regions;
    int32_t  m_scroll = 0;
    float m_row_sel[3]  = { 0.35f, 0.55f, 0.95f };
    float m_row_idle[3] = { 0.15f, 0.16f, 0.11f };
};

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
class PaintPalette : public DeletableBase<PaintPalette>,
                     public AnimatedBase<PaintPalette>
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
        Entry e = entry_of(Kind::Color);
        e.rgba[0] = r; e.rgba[1] = g; e.rgba[2] = b; e.rgba[3] = a;
        e.idle[0] = r; e.idle[1] = g; e.idle[2] = b; e.idle[3] = a;
        m_entries[node] = e;
    }

    void AddSize(ETCS::RID node, float radius)
    {
        if (node == 0 || radius <= 0.0f) return;
        Entry e = entry_of(Kind::Size, radius);
        e.idle[0] = 0.16f; e.idle[1] = 0.16f; e.idle[2] = 0.20f; e.idle[3] = 1.0f;
        m_entries[node] = e;
    }

    void AddRadiusDelta(ETCS::RID node, float delta)
    {
        if (node == 0 || delta == 0.0f) return;
        Entry e = entry_of(Kind::RadiusDelta, delta);
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
        Entry e = entry_of(Kind::AlphaDelta, delta_pct);
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
    /*
 * ── AN ARROW IS AS WIDE AS ITS CELL ──────────────────────────────────────
 *
 * The arrows are authored as three fixed points, and the toolbar's slices are
 * layout boxes that grow. So on a wide window every arrow stayed its authored
 * width and sat against the left edge of a cell several times that size --
 * which reads as a broken layout, and is one: the cell is being driven and the
 * thing inside it is not.
 *
 * REBUILT RATHER THAN SCALED, because a polygon has no transform -- ClearPoints
 * and three AddPoints is the whole of it, and it is exactly the geometry the
 * script would have written if it had known the width.
 *
 * Full width of the cell, shallow: the apex at the middle of the top edge and
 * the base at ARROW_DROP down, which is the shape the toolbar already asks for.
 */
    static constexpr int32_t ARROW_DROP = 12;

    // The cell an arrow lives in is its PARENT -- the script spawns it there
    // (`swatch_ink.spawn(... wheel_arrow_ink)`), so nothing has to be registered
    // for this and a re-laid-out toolbar needs no second binding kept in step.
    static int32_t cell_width_of(ETCS::RID node)
    {
        ETCS::Held<Drawable2D_> held = ETCS::resolve_held<Drawable2D_>("Drawable2D", node);
        if (!held) return 0;
        ETCS::Entity* e = static_cast<ETCS::Entity*>(held.get());
        if (!e) return 0;
        ETCS::Entity* parent = e->getParent();
        if (!parent) return 0;
        void* raw = parent->getInterfacePointer(ETCS::Buffer("Drawable2D"));
        if (!raw) return 0;
        const Rect2D r = static_cast<Drawable2D_*>(raw)->Bounds();
        return r.w;
    }

    bool arrowsNeedStretch() const
    {
        for (const auto& [rid, e] : m_entries)
        {
            if (e.kind != Kind::WheelArrow && e.kind != Kind::ModeArrow) continue;
            const int32_t w = cell_width_of(rid);
            if (w > 0 && w != e.drawn_w) return true;
        }
        return false;
    }

    void stretchArrows()
    {
        for (auto& [rid, e] : m_entries)
        {
            if (e.kind != Kind::WheelArrow && e.kind != Kind::ModeArrow) continue;
            const int32_t w = cell_width_of(rid);
            if (w <= 0 || w == e.drawn_w) continue;

            ETCS::Held<Drawable2D_> held = ETCS::resolve_held<Drawable2D_>("Drawable2D", rid);
            if (!held) continue;
            ETCS::Entity* node = static_cast<ETCS::Entity*>(held.get());
            if (!node) continue;

            const std::string tag = node->getSourceTag().toString();
            ETCS::Buffer none;
            try
            {
                node->call(ETCS::Buffer((tag + ".ClearPoints").c_str()), none,
                           ETCS::RootSignalContext());
                auto pt = [&](int32_t x, int32_t y)
                {
                    ETCS::Buffer p;
                    p.write((std::to_string(x) + ", " + std::to_string(y)).c_str());
                    node->call(ETCS::Buffer((tag + ".AddPoint").c_str()), p,
                               ETCS::RootSignalContext());
                };
                pt(w / 2, 0);
                pt(w, ARROW_DROP);
                pt(0, ARROW_DROP);
            }
            catch (...) { continue; }

            e.drawn_w = w;
            etcs_mark_observed(node);
        }
    }

    void AddWheelArrow(ETCS::RID node, ETCS::RID slot)
    {
        if (node == 0) return;
        Entry e = entry_of(Kind::WheelArrow);
        e.slot = slot;
        e.drawn_w = 0;      // built on the first Advance, at whatever width it has
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
        Entry e = entry_of(Kind::ModeArrow);
        e.slot = slot;
        e.drawn_w = 0;
        m_entries[node] = e;
    }

    /*
 * THE SAME ARROW, ASKED WHAT IT IS ON.
 *
 * Both of the above are "an arrow drawn across the top of a cell, belonging to
 * that cell". Which of the two it is was stated by the caller picking a verb,
 * and the caller was stating something the palette already knew: a cell that is
 * a COLOUR wants a picker, a cell that is a TOOL wants the next mode. So the
 * slot answers it, and the arrow becomes one thing with one declaration.
 *
 * WHY THAT MATTERS MORE THAN THE TWO LINES IT SAVES. The triangle itself was
 * written out nine times in paint_toolbar.etcs -- three points and a fill, per
 * cell -- and since the arrows learned to STRETCH (stretchArrows) those nine
 * authored copies have to agree with a rule that lives in this file. Nine
 * copies of a shape that something else now rebuilds is the eye's problem
 * again: the drawing is a thing, not a paragraph repeated. One verb that needs
 * no literal is what lets the triangle move into a script of its own
 * (paint_cell_arrow.etcs) and be `run` once per cell, because `run` carries
 * RIDs and not words (resolve_run_bindings).
 *
 * THE ORDER IS NOW LOAD-BEARING, which the two-verb form did not require: the
 * cell must be declared (AddColor or AddTool) before its arrow, because the
 * cell is what the arrow is asking. Every caller already did that -- an arrow
 * is spawned as a child of the cell -- and a cell that has not been declared
 * gets no arrow rather than a guessed one.
 */
    void AddArrow(ETCS::RID node, ETCS::RID slot)
    {
        if (node == 0) return;
        auto it = m_entries.find(slot);
        if (it == m_entries.end()) return;
        if (it->second.kind == Kind::Color) { AddWheelArrow(node, slot); return; }
        if (it->second.kind == Kind::Tool)  { AddModeArrow(node, slot);  return; }
    }

    // A third thing a node can mean, alongside a colour and a size: which TOOL
    // it selects. Same mapping, same Apply, so a tool button is a rectangle in
    // the toolbar script exactly as a swatch is.
    void AddTool(ETCS::RID node, const std::string& kind)
    {
        if (node == 0) return;
        Entry e = entry_of(Kind::Tool);
        e.tool = paint_tool_kind_from(kind);
        e.idle[0] = 0.16f; e.idle[1] = 0.16f; e.idle[2] = 0.20f; e.idle[3] = 1.0f;
        m_entries[node] = e;
        // LIT FROM THE MOMENT IT IS DECLARED, if it is the tool in hand. The
        // bar is built before anything has hovered or pressed it, so without
        // this the slice holding the default tool stays idle until the pointer
        // first crosses the bar -- which reads as "the highlight only appears
        // once you touch something", not as a starting state.
        float c[4]; restingColor(e, c);
        paint_node_fill(node, c[0], c[1], c[2], c[3]);
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
        Entry e = entry_of(Kind::Zoom, factor);
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
        Entry e = entry_of(Kind::Call);
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
        Entry e = entry_of(Kind::Popup);
        e.slot   = pane;
        e.target = input;
        m_entries[node] = e;
    }

    // The router a popup joins while open. By RID and called by verb name, for
    // the reason PaintColorWheel gives: PaintRouter is declared below this type.
    void BindRouter(ETCS::RID router) { m_router = router; }

    // The same open and close a press on an AddPopup node performs, for a
    // popup nothing was pressed for -- a question the page has to ask, such as
    // what to do with a file that just arrived (PaintCanvasMenu::OfferImport).
    // One popup at a time still holds: opening this closes whatever was open.
    void OpenPopup(ETCS::RID pane, ETCS::RID input)
    {
        if (pane == 0 || input == 0) return;
        if (m_popup_open == pane) return;      // already up; a press would toggle, a request does not
        open_popup(pane, input);
    }
    void ClosePopup() { close_popup(); }

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
        if (m_hovering != node) paint_node_fill(node, r, g, b, a);
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
            const float next = std::max(1.0f, m_tool->brush().size_px + e.radius);
            m_tool->SetRadius(next);
            paint_node_text(m_radius_readout, std::to_string(static_cast<int>(next + 0.5f)));
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
            paint_node_text(m_alpha_readout, std::to_string(m_tool->alphaPercent()));
            ETCS_LOG("PaintPalette", "alpha -> " << m_tool->alphaPercent() << "%");
        }
        else if (e.kind == Kind::Tool)
        {
            m_tool->SetKind(paint_tool_kind_name(e.tool));
            ETCS_LOG("PaintPalette", "tool -> " << paint_tool_kind_name(e.tool));
            repaintPalette();
            /*
             * THE SELECTION SURVIVES THE TOOL, which is a reversal and the
             * reason for it is the clip. Leaving select used to DROP the
             * region, because "a region left standing under the brush is a
             * trap: it looks like it should mask the stroke and it does not."
             * It does now (PaintLayer::BindClip), so the sentence that argued
             * for dropping it argues for keeping it: a selection is where you
             * may draw, which is a fact about the picture and not about which
             * tool is in hand. The way out of one is a bare click with the
             * select tool, as it always was (end_selection).
             */
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
 * step it on: while the button is held still no input arrives. So the palette
 * claims Animated (ontology/Animated.h) and says "I am not finished, come
 * back"; whatever drives that family steps it, and in this session that is the
 * frame edge (Surface::RunFrames).
 *
 * STATED IN MILLISECONDS, which is a correction. This used to be counted in
 * FRAMES -- 22 before the first repeat, then one every 3 -- on the reasoning
 * that the frame was the only clock and a rate in its own units could not drift
 * from it. That is true of the clock and false of the HAND: a stepper the user
 * holds should step twenty times a second on a 30Hz display and on a 144Hz one,
 * and in frames it steps at a third the speed on one and at twice on the other.
 * AnimatedBase hands over a measured interval for exactly this, so the rate is
 * written the way it is meant.
 *
 * THE HOLD ITSELF is a HeldCharge with a large capacity, and it is spent on the
 * same clock rather than per visit. The charge is a count of ACCESSES, so
 * spending one per visit made "how long an unconfirmed press survives" depend
 * on the display's rate and, with two surfaces driving, on how many windows
 * happened to be open. One access per nominal frame's worth of elapsed time
 * keeps the capacity meaning what its callers set it for: a real release ends
 * the hold, a stated release (the mask) ends it, and a pointer that left the
 * page and never came back ends it after the capacity -- the "much higher
 * threshold" a mode wants, against the four a stroke gets.
 */
    // How far the slice in hand is lifted towards white. Half the hover's 0.60,
    // for the reason restingColor gives.
    static constexpr float SELECTED_LIFT = 0.30f;

    static constexpr double REPEAT_DELAY_MS = 360.0;  // before the first repeat
    static constexpr double REPEAT_EVERY_MS = 50.0;   // then twenty a second
    // One HeldCharge access per nominal frame, so a capacity set in "frames"
    // still means the span of time its callers meant. See SetHoldCapacity.
    static constexpr double HOLD_ACCESS_MS  = 16.0;

    // Called after Apply() accepted a press: a delta entry becomes the held one.
    void Hold(ETCS::RID node)
    {
        auto it = resolve_entry(node);
        if (it == m_entries.end()) return;
        const Kind k = it->second.kind;
        if (k != Kind::RadiusDelta && k != Kind::AlphaDelta && k != Kind::Zoom) return;
        m_held = it->first;
        m_held_ms   = 0.0;
        m_repeat_ms = 0.0;
        m_access_ms = 0.0;
        m_hold.Press();
    }
    void Release() { m_held = 0; m_hold.Release(); }
    bool holding() const { return m_held != 0; }

    // Nothing held is the settled state, and it is what this costs then: one
    // call. See ontology/Animated.h on why the question is asked every visit.
    /*
 * TWO REASONS TO TICK. The held repeat is the old one. The new one is that an
 * arrow's CELL may have changed width: the toolbar's slices are Clay boxes that
 * grow with the window, and an arrow is a polygon with three fixed points, so a
 * wide window left every arrow drawn at the width it was authored for and
 * hugging the left edge of a cell three times that size.
 *
 * ASKED ON A TICK RATHER THAN DRIVEN BY THE SOLVE, because the solver writes
 * MoveTo/ResizeTo straight onto the nodes and nothing downstream of it is told.
 * This is the same shape PaintSurface uses for the same class of problem -- "the
 * pane is not the size I last drew for" -- and it costs one bounds read per
 * arrow per frame, only while a mismatch exists.
 */
    bool AnimatingConcrete() override { return m_held != 0 || arrowsNeedStretch(); }

    /*
 * ONE INTERVAL OF HOLDING.
 *
 * The delay and the repeat are two accumulators rather than one elapsed total
 * compared against a formula, because the second one has to survive a step: a
 * single `m_held_ms` tested modulo the period fires twice on a long frame and
 * skips one on a short one, which is the frame-counted bug in a new spelling.
 *
 * AT MOST ONE STEP PER VISIT, deliberately. A stall the cap did not absorb
 * could otherwise owe several repeats at once and deliver them as a jump; a
 * stepper is a HAND-driven control, and the honest answer to "the page was
 * away for a moment" is that the hand got fewer steps, not that it gets them
 * all back in one frame.
 */
    void AdvanceConcrete(double dt_ms) override
    {
        stretchArrows();                 // cheap, and a no-op once they agree
        // Nothing held: this visit was for the arrows. Spending the charge
        // below would "release" a hold that never began and say so in the log
        // on every tick while a cell is still stretching.
        if (m_held == 0) return;
        m_access_ms += dt_ms;
        while (m_access_ms >= HOLD_ACCESS_MS)
        {
            m_access_ms -= HOLD_ACCESS_MS;
            if (m_hold.Held()) continue;
            ETCS_LOG("PaintPalette", "hold lapsed -- released.");
            Release();
            return;
        }

        m_held_ms += dt_ms;
        if (m_held_ms < REPEAT_DELAY_MS) return;
        m_repeat_ms += dt_ms;
        if (m_repeat_ms < REPEAT_EVERY_MS) return;
        m_repeat_ms = 0.0;
        Apply(m_held);
    }

    // How long an unconfirmed hold survives, in nominal frames. See HeldCharge
    // and HOLD_ACCESS_MS -- the unit is the caller's, the clock is not.
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
        m_hovering = target;
        // The caption of whatever was hovered goes away with the hover; the new
        // target's, if it has one, appears. See AddHoverLabel.
        show_hover_label(target);
        repaintPalette();
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
        paint_node_hidden(label, true);
    }
    // The label that shows the select tool's mode -- written by the arrow, as
    // the radius readout is written by its +/- (see AddModeArrow).
    void SetModeReadout(ETCS::RID label) { m_mode_readout = label; }
    // The shape slice's, written by its arrow the same way. Bind it with
    // AddHoverLabel as well and it shows only while the slice is hovered.
    void SetShapeReadout(ETCS::RID label)
    {
        m_shape_readout = label;
        if (m_tool) paint_node_text(label, paint_shape_mode_name(m_tool->shape()));
    }
    // The brush slice's, which says which NIB is loaded rather than which
    // outline (PaintTipMode). Seeded on bind like the shape's, so the caption
    // is right before the arrow has ever been pressed.
    void SetTipReadout(ETCS::RID label)
    {
        m_tip_readout = label;
        if (m_tool) paint_node_text(label, paint_tip_mode_name(m_tool->tip()));
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
            // An arrow said "radius 0" here, which is what the fallthrough
            // prints for anything it has no line for -- and an arrow is now
            // declared by a script that was never told which kind it is
            // (AddArrow), so "which kind did it become" is exactly the
            // question this report has to be able to answer.
            else if (e.kind == Kind::WheelArrow)
                ETCS_LOG("PaintPalette", "  RID:" << rid << "  wheel arrow for RID:" << e.slot);
            else if (e.kind == Kind::ModeArrow)
                ETCS_LOG("PaintPalette", "  RID:" << rid << "  mode arrow for RID:" << e.slot);
            else if (e.kind == Kind::Tool)
                ETCS_LOG("PaintPalette", "  RID:" << rid << "  tool "
                         << paint_tool_kind_name(e.tool));
            else
                ETCS_LOG("PaintPalette", "  RID:" << rid << "  radius " << e.radius);
        }
    }

    /*
 * HOW THIS TYPE REACHES A NODE IT DOES NOT OWN: by verb name over Entity::call,
 * since the node is another module's drawable (the header note says why there
 * is no other way). That seam is one free function now (paint_node_verb, near
 * the top of this file) rather than three statics here and nine more copies
 * elsewhere; the canvas menu, which used to call these by name, calls the same
 * free ones.
 */

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
    /*
 * EVERY MEMBER CARRIES ITS OWN DEFAULT, and the makers below set only what
 * their kind is about. The first four used to be positional -- Entry e{ kind,
 * rgba, radius, tool } -- which named four of nine members and left clang
 * reporting the other five as missing initializers at every one of the ten
 * call sites, on every build. They were never missing; they had defaults.
 * Saying so here is what makes that true by construction instead of by
 * convention.
 */
    struct Entry {
        Kind kind = Kind::Color;
        float rgba[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        float radius = 0.0f;
        PaintToolKind tool = PaintToolKind::Brush;
        float idle[4] = { 0.16f, 0.16f, 0.20f, 1.0f };
        // The arrows only: the entry this arrow is a control FOR -- a colour
        // for a wheel arrow, a tool for a mode arrow. An arrow belongs to a
        // specific slice, not to "whatever was pressed last". A popup's PANE.
        ETCS::RID slot = 0;
        // A call's target and what to say to it; a popup's input (AddPopup).
        ETCS::RID   target = 0;
        std::string action;
        std::string args;
        // The cell width this node's geometry was last built for -- see
        // stretchArrows. Zero means never, which is also what forces the
        // first build.
        int32_t drawn_w = 0;
    };

    // The two makers every call site goes through, so that adding a member to
    // Entry is one edit here rather than ten at the sites.
    static Entry entry_of(Kind k) { Entry e; e.kind = k; return e; }
    static Entry entry_of(Kind k, float radius) { Entry e; e.kind = k; e.radius = radius; return e; }

    /*
 * WHAT A NODE LOOKS LIKE WITH NOTHING HOVERING IT -- its idle colour, except
 * for the slice whose tool is the one in hand, which stays lit.
 *
 * A bar that lights only what is under the pointer answers "what am I about to
 * press" and never "what am I holding", and between strokes the second is the
 * question you actually have. The lift is half the hover's: findable at a
 * glance, and still leaving the hover somewhere to go.
 */
    void restingColor(const Entry& e, float out[4]) const
    {
        const bool held = (e.kind == Kind::Tool && m_tool && e.tool == m_tool->kind());
        const float t = held ? SELECTED_LIFT : 0.0f;
        for (int i = 0; i < 3; ++i) out[i] = e.idle[i] + (1.0f - e.idle[i]) * t;
        out[3] = e.idle[3];
    }

    /*
 * ── the whole bar's look, in one pass ────────────────────────────────────
 *
 * A slice is one of three things: idle, lit because its tool is the one in
 * hand, or lit further because the pointer is on it. That is ONE answer per
 * node, so it is one write per node -- which is the correction here, and it is
 * not only tidiness.
 *
 * This used to be two passes: reset everything to idle, then tint the hovered
 * group over it. Every pointer move therefore wrote most of the bar twice,
 * once with a value that was already known to be wrong, and the compositor is
 * on another thread. A recompose landing between the two reads the first. The
 * visible failure was a hovered slice that stayed dark while every other slice
 * repainted correctly -- the tint had gone out, and the frame that showed it
 * had already been taken.
 *
 * It also fixes what two passes could not express: a tool change while the
 * pointer is ON the slice. The old refresh skipped the hovered group to avoid
 * dimming it, so the newly held slice was never given its lit resting colour,
 * and since the pointer had not moved there was no hover to restore it either.
 * Selecting a tool left nothing highlighted at all.
 *
 * THE GENERAL ENTRIES ARE NOT RESTYLED. Their look belongs to whoever bound
 * them -- the canvas menu paints its anchor cells to show the chosen one
 * (PaintCanvasMenu::show_anchor) -- and this type never learned an idle colour
 * for them, so writing one back would erase a state it does not know is there.
 * See owns_look, which says the same of the arrows.
 */
    void repaintPalette()
    {
        auto h = (m_hovering != 0) ? m_entries.find(m_hovering) : m_entries.end();
        const bool hovered = (h != m_entries.end());
        // A swatch lifts less than a tool: its idle colour is the colour it
        // MEANS, and washing it toward white says the wrong thing about it.
        const float t = hovered ? ((h->second.kind == Kind::Color) ? 0.40f : 0.60f) : 0.0f;

        for (const auto& [rid, e] : m_entries)
        {
            if (!owns_look(e)) continue;
            float c[4]; restingColor(e, c);
            // OVER THE RESTING COLOUR, not over idle: the slice in hand is
            // already lit, and hovering it has to read as brighter still
            // rather than as the same lift arriving twice.
            if (hovered && (rid == m_hovering || same_hover_group(h->second, e)))
            {
                for (int i = 0; i < 3; ++i) c[i] = c[i] + (1.0f - c[i]) * t;
                c[3] = 1.0f;
            }
            paint_node_fill(rid, c[0], c[1], c[2], c[3]);
        }
    }

    /*
 * Whether this type may restyle the entry at all -- see the note in
 * repaintPalette.
 *
 * AN ARROW IS NOT OURS. AddWheelArrow and AddModeArrow never learned an idle
 * colour, so they carried the default -- 0.16, the slice's own dark -- and
 * every repaint painted the arrow that colour. On the first pointer move over
 * the bar all sixteen arrows vanished into their cells, which looked like they
 * had "started highlighted" and then been turned off. Their look belongs to
 * the script that drew them (SetFill, paint_toolbar.etcs), exactly as a
 * general entry's does.
 */
    static bool owns_look(const Entry& e)
    {
        return e.kind != Kind::Call && e.kind != Kind::Popup
            && e.kind != Kind::WheelArrow && e.kind != Kind::ModeArrow;
    }

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
 * Step the stepped thing on the slice `slot` names -- the select tool's mode,
 * the shape tool's outline, or the brush's NIB (PaintTipMode). Three slices
 * with an arrow, one arrow that knows which by asking what it points at.
 *
 * TAKES THE TOOL UP AS WELL. A wheel pick ends with the picked colour in hand,
 * so the swatch's arrow effectively selects that swatch; an arrow that changed
 * the mode of a tool you were not holding would change a readout and nothing
 * you could see on the canvas, which reads as a control that does nothing.
 */
    void step_mode_for(ETCS::RID arrow, ETCS::RID slot)
    {
        auto sit = m_entries.find(slot);
        const bool named = sit != m_entries.end() && sit->second.kind == Kind::Tool;
        const bool is_select = named && sit->second.tool == PaintToolKind::Select;
        const bool is_shape  = named && sit->second.tool == PaintToolKind::Shape;
        const bool is_brush  = named && sit->second.tool == PaintToolKind::Brush;
        if (!is_select && !is_shape && !is_brush)
        {
            ETCS_LOG("PaintPalette", "mode arrow on RID:" << arrow << " names RID:" << slot
                     << ", which is not the select, shape or brush slice.");
            return;
        }
        // Stepping the mode also takes the tool: an arrow pressed is a choice of
        // what to draw next, and asking for a second press to draw it is a
        // choice nobody meant to make.
        const PaintToolKind want = is_select ? PaintToolKind::Select
                                 : is_shape  ? PaintToolKind::Shape
                                             : PaintToolKind::Brush;
        if (m_tool->kind() != want) { m_tool->SetKind(paint_tool_kind_name(want)); repaintPalette(); }
        if (is_select)
        {
            m_tool->CycleMode();
            paint_node_text(m_mode_readout, paint_select_mode_label(m_tool->mode()));
            ETCS_LOG("PaintPalette", "select mode -> " << paint_select_mode_name(m_tool->mode()));
        }
        else if (is_shape)
        {
            m_tool->CycleShape();
            paint_node_text(m_shape_readout, paint_shape_mode_name(m_tool->shape()));
            ETCS_LOG("PaintPalette", "shape -> " << paint_shape_mode_name(m_tool->shape()));
        }
        else
        {
            m_tool->CycleTip();
            paint_node_text(m_tip_readout, paint_tip_mode_name(m_tool->tip()));
            ETCS_LOG("PaintPalette", "tip -> " << paint_tip_mode_name(m_tool->tip()));
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
 * Hiding is enough: the sheet is rebuilt from its tree by the compose walk, so
 * what the pane was covering comes back with the next frame. The wheel is
 * closed on the way in, because two popups would each have to know about the
 * other to dismiss it.
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
        paint_node_hidden(pane, false);
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
        paint_node_hidden(m_popup_open, true);
        ETCS_LOG("PaintPalette", "popup pane RID:" << m_popup_open << " closed.");
        m_popup_open = 0;
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
        if (m_hover_label_shown != 0) paint_node_hidden(m_hover_label_shown, true);
        if (want != 0) paint_node_hidden(want, false);
        m_hover_label_shown = want;
    }

    std::unordered_map<ETCS::RID, Entry> m_entries;
    std::unordered_map<ETCS::RID, ETCS::RID> m_hover_labels;   // entry -> caption
    ETCS::RID m_hover_label_shown = 0;
    // The held stepper, if any -- see Hold/AdvanceConcrete. 600 accesses at one
    // per nominal frame is ten seconds: a hold whose release was lost to the
    // page ends on its own then, whatever the display is actually doing.
    ETCS::RID  m_held = 0;
    double     m_held_ms   = 0.0;   // since the press -- gates the first repeat
    double     m_repeat_ms = 0.0;   // since the last repeat -- gates the rest
    double     m_access_ms = 0.0;   // remainder owed to the hold charge
    HeldCharge m_hold{ 600 };
    PaintTool* m_tool = nullptr;
    PaintSurface* m_surface = nullptr;
    ETCS::RID m_hovering = 0;
    ETCS::RID m_radius_readout = 0;
    ETCS::RID m_alpha_readout = 0;
    ETCS::RID m_mode_readout = 0;
    ETCS::RID m_shape_readout = 0;
    ETCS::RID m_tip_readout = 0;
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
class PaintLayerPanel : public DeletableBase<PaintLayerPanel>,
                        public AnimatedBase<PaintLayerPanel>
{
public:
    WIRE_TYPE_IDENTITY(PaintLayerPanel);

    PaintLayerPanel() = default;
    bool DeleteConcrete() override { return true; }

    /*
     * Drag is its own region and that is the point of it. A press on the row
     * BODY used to start a restack, and the body is also the second half of the
     * double-click that opens the name -- so on a wide window, where the body is
     * most of the row, a drag and a rename were the same gesture and the text
     * field won. A dotted strip says "hold here", takes the drag, and leaves the
     * body to mean only what it always meant.
     */
    enum class Region : uint8_t
    { Body, Eye, Label, Delete, Title, Add, Drag, MergeUp, MergeDown, View };

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
    /*
 * FIVE NODES NOW, because a row answers five questions. The thumbnail is the
 * new one and it is not a control: it is a picture of the layer, painted into
 * whatever raster the script put there (see paint_thumb), so a row can be told
 * apart by what is ON it rather than by a name somebody has to have chosen
 * well. Picking it means the same as picking the row body -- it is the layer,
 * so it selects the layer.
 */
    /*
 * ── A ROW IS ASSEMBLED, NOT DECLARED ─────────────────────────────────────
 *
 * BeginRow opens one and RowNode attaches a part to it by name. This replaced
 * AddRow's five positional RIDs plus AddRowExtras' four, and the reason is not
 * tidiness: a script CANNOT hand another script a node it spawned, because
 * `run` carries RIDs in and no names out (resolve_run_bindings). So a row whose
 * parts all had to arrive in one call could only ever be written in one file,
 * which is why the eye's fourteen points were copied once per row -- seven
 * places to change a glyph and six to forget.
 *
 * With a row open, any script holding this panel can contribute a part. The eye
 * is its own file now (paint_eye.etcs) and draws itself into the row's pane;
 * the row script does not need to know what an eye is made of, and nothing
 * needs to know both.
 *
 * AN UNKNOWN PART NAME IS REFUSED LOUDLY. It is the one mistake this shape
 * makes possible that the positional form did not, and a part silently not
 * attached is a control that does nothing for a reason nobody can see.
 */
    void BeginRow()
    {
        m_title_open = false;
        m_rows.push_back(Row{});
    }

    void RowNode(const std::string& what, ETCS::RID node)
    {
        if (node == 0) return;
        /*
     * THE TITLE BAR'S EYE arrives through the same script the rows use
     * (paint_eye.etcs registers by RowNode, since a script cannot hand its
     * nodes back), so between BeginTitle and the first BeginRow the eye and
     * its pupil are the window's view toggle rather than a row's control.
     */
        if (m_title_open)
        {
            if      (what == "eye")   { m_view_eye  = node; BindView(node); }
            else if (what == "iris")  { m_view_iris = node; BindView(node); }
            else if (what == "pupil") { m_view_trim.push_back(node); BindView(node); }
            else ETCS_LOG("PaintLayerPanel", "RowNode: '" << what << "' between BeginTitle and BeginRow -- "
                          "only an eye goes on the title bar; RID:" << node << " is attached to nothing.");
            return;
        }
        if (m_rows.empty()) { ETCS_LOG("PaintLayerPanel", "RowNode before BeginRow -- ignored."); return; }
        const size_t idx = m_rows.size() - 1;
        Row& row = m_rows[idx];

        // The parts that are picked, and what a press on each one means.
        if      (what == "bg")    { row.bg    = node; m_regions[node] = Hit{ idx, Region::Body }; }
        else if (what == "eye")   { row.eye   = node; m_regions[node] = Hit{ idx, Region::Eye }; }
        // THE IRIS AND THE PUPIL ARE THE EYE for a press or a hover. They are
        // drawn OVER the oblong, and the pick takes the topmost node -- so with
        // them listed as decoration only, the middle of every eye was the one
        // place on it that did nothing. The iris is tinted with the eye
        // (Refresh); the pupil keeps the colour the script gave it.
        else if (what == "iris")  { row.iris = node; m_regions[node] = Hit{ idx, Region::Eye }; row.trim.push_back(node); }
        else if (what == "pupil") { m_regions[node] = Hit{ idx, Region::Eye }; row.trim.push_back(node); }
        else if (what == "thumb") { row.thumb = node; m_regions[node] = Hit{ idx, Region::Body }; }
        else if (what == "label") { row.label = node; m_regions[node] = Hit{ idx, Region::Label }; }
        else if (what == "del")   { row.del   = node; m_regions[node] = Hit{ idx, Region::Delete }; }
        else if (what == "grip")  { row.grip  = node; m_regions[node] = Hit{ idx, Region::Drag }; }
        // THE DOTS ARE THE GRIP, for the reason the iris is the eye: they are
        // drawn over it and the pick takes the topmost node, so as decoration
        // they made the middle of the handle -- the part that says "hold here"
        // -- the one place on it a press did not start a drag.
        else if (what == "dots")  { m_regions[node] = Hit{ idx, Region::Drag }; row.trim.push_back(node); }
        else if (what == "up")    { row.mup   = node; m_regions[node] = Hit{ idx, Region::MergeUp }; }
        else if (what == "down")  { row.mdn   = node; m_regions[node] = Hit{ idx, Region::MergeDown }; }
        // DECORATION, by any of the names a row script has for it. Not a
        // control: it is listed so that hiding the row hides it too, which a
        // polygon cannot inherit from a parent it does not have. Any number of
        // them, because a row keeps growing ornaments and each one that has no
        // slot is one that stays on screen over an empty panel.
        else if (what == "trim")
            row.trim.push_back(node);
        else
        {
            ETCS_LOG("PaintLayerPanel", "RowNode: '" << what << "' is not a part of a row "
                     "(bg eye iris pupil thumb label del grip up down, or dots/trim) -- RID:" << node
                     << " is attached to nothing.");
        }
    }

    // Opens the title bar for an eye (see RowNode); the first BeginRow closes it.
    void BeginTitle() { m_title_open = true; }

    // The window's body: the backing plate and anything else that is not the
    // title bar, hidden with the rows when the window is collapsed -- a
    // collapsed window IS its title bar, and a plate left behind is a window
    // that only looks collapsed.
    void BindBody(ETCS::RID node) { if (node) m_body.push_back(node); }

    // The + on the title bar. A control of the WINDOW rather than of a row, so
    // it is bound like the title handle is and carries no row index.
    void BindAdd(ETCS::RID node)
    {
        if (node) m_regions[node] = Hit{ SIZE_MAX, Region::Add };
    }

    /*
 * THE VIEW TOGGLE, beside the +. Collapsed, the window is its title bar and
 * nothing else -- every row, and the rows' own controls, are hidden.
 *
 * NOT A CLOSE, and the distinction is the same one the title comment already
 * makes: a layer window with a cross on it is a window somebody closes by
 * accident and then cannot reopen. This leaves the bar on screen, which is both
 * the thing you press to get the list back and the reminder that there is one.
 */
    void BindView(ETCS::RID node)
    {
        if (node) m_regions[node] = Hit{ SIZE_MAX, Region::View };
    }

    bool collapsed() const { return m_collapsed; }

    /*
 * THE TITLE BAR IS THE HANDLE: a press on it and the window follows the
 * pointer until the release. Any number of nodes may be the handle (the bar
 * and the word on it), and the window they move is the pane the script laid
 * the rows out in -- moved by its own SetPosition (DragWindow says why), so the
 * rows, being placed in the pane's space, come along with nothing recomputed.
 *
 * There is deliberately NO close: a layer window with a cross on it is a
 * window the user will close by accident and then have no way to reopen, and
 * the layers are not optional. Rows can go (Region::Delete); the window stays.
 * (The pane moves by its SetPosition verb -- DragWindow says why not MoveTo.)
 */
    void BindTitle(ETCS::RID handle)
    {
        if (handle) m_regions[handle] = Hit{ SIZE_MAX, Region::Title };
    }
    void BindWindow(ETCS::RID pane) { m_window = pane; }

    /*
 * THE VIEW TO PUT BACK, because half of what this window does changes the
 * PICTURE and not just the list. Restacking a layer, hiding one, deleting one
 * or adding one all re-order or re-veil what the composite draws -- and a
 * press on a row returns from the input edge before any tool runs, so nothing
 * else was asking the surface to render. The rows updated and the canvas kept
 * showing the arrangement from before the press until something unrelated
 * happened to repaint it.
 *
 * Bound rather than reached for, the same way the palette and the wheel bind
 * one: a panel does not know what is showing the document, and there may be
 * more than one thing.
 */
    void BindSurface(ETCS::RID surface)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintSurface", surface);
        if (raw) m_surface = static_cast<PaintSurface*>(raw->getTrueType());
    }

    void SetHoverDim(float dim) { m_hover_dim = std::clamp(dim, 0.0f, 1.0f); }

    // Eye fills for the two states, and the row tint for selected / not. Given
    // by the script for the same reason the swatch colours are: this is a look.
    void SetEyeColors(float vr, float vg, float vb, float hr, float hg, float hb)
    {
        m_eye_shown[0] = vr; m_eye_shown[1] = vg; m_eye_shown[2] = vb;
        m_eye_hidden[0] = hr; m_eye_hidden[1] = hg; m_eye_hidden[2] = hb;
    }

    // The iris, open and shut. Open is the page's highlight so an eye that is
    // looking reads as looking; shut sinks into the band, the same way the
    // oblong does, so a closed eye is one dark shape rather than a bright dot
    // on a dim one.
    void SetIrisColors(float vr, float vg, float vb, float hr, float hg, float hb)
    {
        m_iris_open[0] = vr; m_iris_open[1] = vg; m_iris_open[2] = vb;
        m_iris_shut[0] = hr; m_iris_shut[1] = hg; m_iris_shut[2] = hb;
    }

    void SetRowColors(float sr, float sg, float sb, float ur, float ug, float ub)
    {
        m_row_sel[0] = sr; m_row_sel[1] = sg; m_row_sel[2] = sb;
        m_row_idle[0] = ur; m_row_idle[1] = ug; m_row_idle[2] = ub;
    }

    // The colour a row takes while it is being dragged, and its ghost with it.
    void SetDragColor(float r, float g, float b) { m_row_drag[0] = r; m_row_drag[1] = g; m_row_drag[2] = b; }

    /*
 * THE GHOST: a row-sized pane that follows the pointer while a row is dragged,
 * showing the layer being carried (its thumb and name), and the mark: a bar
 * in the gap the layer will land in. Both are the script's nodes, spawned in
 * the window and passed through by the pick (SetPassthrough), so what is under
 * the pointer is still the row it is over. Neither is required -- without them
 * a drag still restacks, it just shows only the colour of the row.
 */
    void BindGhost(ETCS::RID pane) { m_ghost = pane; paint_node_hidden(pane, true); }
    void GhostNode(const std::string& what, ETCS::RID node)
    {
        if      (what == "thumb") m_ghost_thumb = node;
        else if (what == "label") m_ghost_label = node;
        else ETCS_LOG("PaintLayerPanel", "GhostNode: '" << what << "' is not a part of the ghost (thumb label).");
    }
    void BindDropMark(ETCS::RID node) { m_drop_mark = node; paint_node_hidden(node, true); }

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
        // The rows now mean different layers, so whatever was being isolated is
        // not what is under the pointer any more. Straight to full rather than
        // faded: the reason for the dim is gone, not going.
        m_hovering = 0;
        m_subject  = 0;
        m_dim_target = 1.0f;
        m_dim_now    = 1.0f;
        m_document->IsolateLayer(0, 1.0f);
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

        /*
     * COLLAPSED IS A ROW STATE, not a second drawing path. Every row and every
     * control on it goes hidden and the title bar is left; the bar is what
     * re-expands it, so the window can never be lost. Done here rather than in
     * the press because a Refresh from anywhere else -- a page switch, a
     * delete -- must not quietly bring the rows back.
     */
        tint_view_eye();
        if (m_collapsed)
        {
            for (Row& row : m_rows)
            {
                row.layer = 0;
                for (ETCS::RID n : { row.bg, row.eye, row.thumb, row.label,
                                     row.del, row.grip, row.mup, row.mdn })
                    if (n) paint_node_hidden(n, true);
                for (ETCS::RID n : row.trim) paint_node_hidden(n, true);
            }
            for (ETCS::RID n : m_body) paint_node_hidden(n, true);
            return;
        }
        for (ETCS::RID n : m_body) paint_node_hidden(n, false);

        /*
     * THE HOVER DIM CANNOT OUTLIVE THE ROW IT CAME FROM. Hovering a row dims
     * every other layer (IsolateLayer) and only another hover undoes it -- so
     * a row deleted under the pointer, or a scroll that re-binds the rows, or
     * a page switch, left the picture faded with the eye column still saying
     * every layer was visible. Re-asserted here, where the rows are being
     * re-bound anyway: if the layer being isolated is no longer one of them,
     * nobody is, and IsolateLayer(0) is how that is said.
     */
        if (m_hovering != 0)
        {
            bool still_here = false;
            for (auto* l : stack) if (l->getRID() == m_hovering) { still_here = true; break; }
            if (!still_here)
            {
                m_hovering = 0;
                m_subject  = 0;
                m_dim_target = 1.0f;
                m_dim_now    = 1.0f;
                m_document->IsolateLayer(0, 1.0f);
            }
        }

        // The stack may have shrunk under the offset (a delete, a page with
        // fewer layers): clamped here rather than only in Scroll, so a window
        // scrolled to the bottom of a long stack does not show empty rows over
        // a short one.
        const size_t total = stack.size();
        {
            const int32_t most = (total > m_rows.size())
                               ? static_cast<int32_t>(total - m_rows.size()) : 0;
            m_scroll = std::clamp(m_scroll, 0, most);
        }
        for (size_t i = 0; i < m_rows.size(); ++i)
        {
            Row& row = m_rows[i];
            const size_t from_top = i + static_cast<size_t>(m_scroll);
            PaintLayer* layer = (from_top < total)
                              ? stack[total - 1 - from_top] : nullptr;
            row.layer = layer ? layer->getRID() : 0;

            // THE ROW ITSELF COMES BACK FIRST. Collapsing hides the row pane
            // and every part on it; the parts below are unhidden one by one as
            // the row is re-bound, but the pane is their parent and was never
            // unhidden -- so expanding brought back nothing, and the window
            // looked broken until a page switch. An empty slot keeps its pane
            // shown too: it is drawn transparent, not absent.
            paint_node_hidden(row.bg, false);
            paint_node_hidden(row.label, false);
            if (!layer)
            {
                // An empty slot is drawn as nothing rather than hidden: the
                // window is a fixed frame and a gap in it is honest.
                paint_node_fill(row.bg,    m_row_idle[0], m_row_idle[1], m_row_idle[2], 0.0f);
                paint_node_fill(row.eye,   0.0f, 0.0f, 0.0f, 0.0f);
                paint_node_hidden(row.eye, true);
                paint_node_hidden(row.thumb, true);
                paint_node_hidden(row.del, true);
                paint_node_fill(row.del,   0.0f, 0.0f, 0.0f, 0.0f);
                paint_node_hidden(row.grip,  true);
                paint_node_hidden(row.mup,   true);
                paint_node_hidden(row.mdn,   true);
                for (ETCS::RID n : row.trim) paint_node_hidden(n, true);
                paint_node_text(row.label, "");
                continue;
            }

            const bool selected = (m_document->activeLayer() == layer);
            const bool carried  = m_drag_live && row.layer == m_dragging;
            const float* tint = carried ? m_row_drag : (selected ? m_row_sel : m_row_idle);
            paint_node_fill(row.bg, tint[0], tint[1], tint[2], 1.0f);
            // EVERY layer has an eye, including the base one: hiding the paper
            // to see what is under it is exactly what the control is for, and
            // it is the delete that the base refuses, not the eye.
            paint_node_hidden(row.eye, false);
            tint_eye(row.eye, row.iris, layer->visible());
            paint_node_hidden(row.thumb, false);
            paint_thumb(row.thumb, layer);
            // The base layer has no delete: it is the page's ground and the
            // document refuses to remove it (PaintDocument::RemoveLayer), so a
            // button that would only ever be refused is not drawn or picked.
            const bool base = (from_top == total - 1);
            paint_node_hidden(row.del, base);
            paint_node_fill(row.del, 0.75f, 0.28f, 0.30f, base ? 0.0f : 1.0f);
            // The grip is always there -- even the base row can be dragged, it
            // simply has nowhere below to land. The merge arrows are hidden at
            // the ends of the stack for the reason the delete is: an arrow that
            // can only ever be refused is a control that teaches nothing.
            paint_node_hidden(row.grip,  false);
            for (ETCS::RID n : row.trim) paint_node_hidden(n, false);
            paint_node_hidden(row.mup,   from_top == 0);
            paint_node_hidden(row.mdn,   base);
            // The row being typed into shows the BUFFER and a caret, not the
            // name it still has -- otherwise the keys go somewhere invisible
            // and the rename reads as the window ignoring you.
            if (m_editing && m_renaming == row.layer)
                paint_node_text(row.label, (m_edit + "_").c_str());
            else
                paint_node_text(row.label, layer->name().c_str());
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
    bool Apply(ETCS::RID node, bool is_press, Point2D at = Point2D{ 0, 0 })
    {
        auto it = m_regions.find(node);
        if (it == m_regions.end()) return false;
        if (!is_press) return true;                // ours, but a release does nothing

        const Hit hit = it->second;
        if (hit.region == Region::Title)
        {
            // The window's origin at the press and the press itself, both in
            // the pane's parent's space -- the same space `at` arrives in.
            ETCS::Held<Drawable2D_> w = ETCS::resolve_held<Drawable2D_>("Drawable2D", m_window);
            if (!w) return true;
            const Rect2D b = w->Bounds();
            m_moving = true;
            m_grab   = at;
            m_origin = Point2D{ b.x, b.y };
            m_renaming = 0;
            return true;
        }
        if (!m_document) return true;
        if (hit.region == Region::Add)
        {
            end_edit(false);
            m_document->NewLayer();
            Refresh();
            repaint();                 // a carry may have landed on the way in
            return true;
        }
        if (hit.region == Region::View)
        {
            end_edit(false);
            m_collapsed = !m_collapsed;
            Refresh();                 // which is where hidden actually happens
            repaint();
            return true;
        }
        Row& row = m_rows[hit.row];
        if (row.layer == 0) return true;           // an empty slot is still ours

        switch (hit.region)
        {
        /*
     * PRESS TO CHOOSE, PRESS AGAIN TO RENAME. The label itself opens the field
     * on its own press, but a TextLabel is only as wide as the word in it -- a
     * layer called "Ink" is a twenty-pixel target in a two-hundred-pixel row,
     * and everywhere else along the name is the body. So the body answers the
     * same gesture: a press on the row that is ALREADY the active one is the
     * second half of a double click, and opens the name. Choosing a different
     * row still just chooses it.
     */
        case Region::Body:
            if (!m_editing && m_document->activeLayer()
                && m_document->activeLayer()->getRID() == row.layer)
            {
                begin_edit(row.layer);
                m_dragging = 0;
                break;
            }
            end_edit(false);
            m_document->SetActiveLayer(row.layer);
            // A press on the body chooses, and does NOT begin a restack any
            // more: the grip does that (Region::Drag). The two shared this
            // gesture and the rename always won it.
            repaint();                 // SetActiveLayer lands a carry in flight
            break;

        /*
     * THE GRIP. Held as a RID so a restack between the press and the drop
     * cannot leave this pointing at a row that now means another layer.
     * Choosing the row too, because reaching for a row's handle and finding
     * you have moved a different layer than the one you then look at is the
     * kind of surprise that makes people stop using a control.
     */
        case Region::Drag:
            end_edit(false);
            m_document->SetActiveLayer(row.layer);
            m_dragging = row.layer;
            m_drag_live = false;
            m_drag_press = window_local(at);
            m_drag_grab_dy = m_drag_press.y - row_rect(hit.row).y;
            repaint();
            break;

        case Region::MergeUp:
        case Region::MergeDown:
            end_edit(false);
            m_document->MergeLayer(row.layer, hit.region == Region::MergeDown ? -1 : 1);
            if (m_dragging == row.layer) m_dragging = 0;
            Refresh();
            repaint();
            break;

        case Region::Eye:
            if (ETCS::Entity* raw = paint_resolve_tag("PaintLayer", row.layer))
                static_cast<PaintLayer*>(raw->getTrueType())->ToggleVisible();
            end_edit(false);
            repaint();                 // the composite has one layer more or less in it
            break;

        /*
     * SECOND PRESS ON THE SAME LABEL OPENS THE NAME FOR TYPING, which is what
     * a double click is once the panel is the only thing that knows a label
     * was just pressed. No timer: the rule is "pressed twice with nothing else
     * in between", which is stricter than a clock and needs no clock.
     *
     * The field is this panel's, not a text box in the picture: the keys are
     * offered here first while it is open (PaintInput::KeyDown -> KeyIn) and
     * the row draws the buffer with a caret, so what is being typed is on the
     * row it will name.
     */
        case Region::Label:
            if (m_renaming == row.layer) break;    // already open, keep typing
            begin_edit(row.layer);
            m_dragging = 0;                        // typing is not a drag
            break;

        case Region::Delete:
            if (m_renaming == row.layer) end_edit(false);
            m_document->RemoveLayer(row.layer);
            if (m_dragging == row.layer) m_dragging = 0;
            repaint();
            break;

        case Region::Title:
        case Region::Add:
        case Region::View:
            // All answered above, before the row lookup -- they are the
            // window's controls, not a row's. Named here so the switch stays
            // total and a new region cannot be added without deciding what it
            // means.
            break;
        }
        Refresh();
        return true;
    }

    /*
 * ── dragging a row ───────────────────────────────────────────────────────
 *
 * A PRESS ON THE GRIP IS NOT YET A DRAG. It chooses the row (Apply), and only
 * a pointer that has moved DRAG_START_PX from where it went down makes it one:
 * then the row takes the drag colour, the ghost appears under the pointer and
 * the mark shows where the layer would land. A press and release that never
 * moved is a click, and moves nothing.
 *
 * BY POSITION, NOT BY NODE. Where a live drag lands is the row under the
 * pointer's height in the window, clamped to the rows that hold layers -- so a
 * release on a row's dots, in the two-pixel gap between rows, over the title
 * bar or over the empty slots below the stack all land somewhere sensible. By
 * node, each of those was a release over something that was not a row, and
 * each silently cancelled the drag. Released outside the window it is still a
 * cancel: putting a layer somewhere the user did not point at is worse than
 * doing nothing.
 */
    static constexpr int32_t DRAG_START_PX = 4;

    bool draggingRow() const { return m_dragging != 0; }

    // Motion while the grip is held. True when it was the drag's, which is
    // also what keeps it from hovering the eyes it passes over.
    bool DragRow(Point2D at)
    {
        if (m_dragging == 0) return false;
        const Point2D l = window_local(at);
        if (!m_drag_live)
        {
            if (std::abs(l.x - m_drag_press.x) < DRAG_START_PX
                && std::abs(l.y - m_drag_press.y) < DRAG_START_PX) return true;
            m_drag_live = true;
            show_ghost();
            Refresh();                 // the carried row in the drag colour
        }
        const int32_t from = row_of_layer(m_dragging);
        const int32_t last = last_layer_row();
        if (last < 0) return true;
        if (m_ghost)
        {
            const Rect2D first_r = row_rect(0), last_r = row_rect(static_cast<size_t>(last));
            const int32_t gy = std::clamp(l.y - m_drag_grab_dy, first_r.y, last_r.y);
            paint_node_moved(m_ghost, first_r.x, gy);
        }
        place_mark(from, row_at(l.y));
        return true;
    }

    bool Drop(Point2D at)
    {
        if (m_dragging == 0) return false;
        const bool live = m_drag_live;
        const ETCS::RID layer = m_dragging;
        end_row_drag();
        if (!live || !m_document || !Contains(at)) { Refresh(); return true; }

        const int32_t target = row_at(window_local(at).y);
        std::vector<PaintLayer*> stack;
        m_document->OrderedLayers(stack);
        const size_t total = stack.size();
        const size_t from_top = static_cast<size_t>(target) + static_cast<size_t>(m_scroll);
        if (target >= 0 && from_top < total && m_rows[static_cast<size_t>(target)].layer != layer)
        {
            // Depth counts from the BOTTOM (PaintDocument::MoveLayerTo, the
            // order key); rows count from the top. Turned round here, the one
            // place that knows both conventions.
            const int32_t depth = static_cast<int32_t>(total - 1 - from_top);
            m_document->MoveLayerTo(layer, depth);
            ETCS_LOG("PaintLayerPanel", "dropped on row " << target << " -- depth " << depth);
        }
        Refresh();
        repaint();                     // a different stack composites differently
        return true;
    }

    // The button came up somewhere this panel never heard about (another
    // pane took the release): whatever was being carried is put down.
    void CancelRowDrag()
    {
        if (m_dragging == 0) return;
        const bool live = m_drag_live;
        end_row_drag();
        if (live) Refresh();
    }

    /*
 * The window under the pointer while the title bar is held. True while that
 * is so, which tells the input edge the motion was a move, not a hover and
 * not a stroke.
 *
 * BY THE SetPosition VERB, not the family's MoveTo, and the difference is
 * visible: a compositor's MoveTo only STAGES the new origin, to be applied at
 * the top of its own next recompose (CompositeDrawable2D::applyPendingGeometry),
 * and a pane whose contents did not change is not recomposed by the frame --
 * so the window answered "moved" and stayed exactly where it was. SetPosition
 * is what the colour wheel's pane moves by (PaintColorWheel::move_pane) and
 * takes effect on the next compose of the parent.
 */
    bool DragWindow(Point2D at)
    {
        if (!m_moving) return false;
        if (!paint_node_moved(m_window,
                              m_origin.x + (at.x - m_grab.x),
                              m_origin.y + (at.y - m_grab.y)))
        { m_moving = false; return false; }
        return true;
    }

    bool EndWindowDrag()
    {
        if (!m_moving) return false;
        m_moving = false;
        return true;
    }

    // Whether a point in the pane's parent's space is over the window. Asked
    // for a wheel notch, which carries no point of its own and so cannot be
    // picked (PaintInput::RouteEvent) -- the last routed position stands in
    // for it, and the window's bounds are the only thing that has to hold.
    bool Contains(Point2D at) const
    {
        ETCS::Held<Drawable2D_> w = ETCS::resolve_held<Drawable2D_>("Drawable2D", m_window);
        if (!w) return false;
        const Rect2D b = w->Bounds();
        return at.x >= b.x && at.y >= b.y
            && at.x < b.x + static_cast<int32_t>(b.w)
            && at.y < b.y + static_cast<int32_t>(b.h);
    }

    /*
 * HOVER: the layer whose EYE is under the pointer at full strength, every other
 * one dimmed.
 *
 * THE EYE AND NOT THE ROW, which is the whole of the gesture. Isolating on the
 * row meant the picture faded whenever the pointer crossed the window on its
 * way to anything -- choosing a layer, renaming one, reaching the +, or just
 * passing over -- so the answer to "which layer is this" arrived constantly and
 * uninvited, and the thing being looked at was the thing being hidden. The eye
 * is the control that is ABOUT visibility, so hovering it is the one moment
 * where "show me only this layer" is what the hand is already asking; a press
 * there commits the same thing permanently.
 *
 * Stated to the document as one call over the whole stack (PaintDocument::
 * IsolateLayer) rather than as a dim per row, so leaving an eye is the same
 * call with a different subject and there is no per-row bookkeeping to get
 * wrong when the pointer skips one. Any other node -- another part of the row,
 * the title bar, the picture -- clears the isolation, which is what "the
 * pointer is not on an eye" means without needing an exit event.
 */
    void Hover(ETCS::RID node)
    {
        if (!m_document) return;
        auto it = m_regions.find(node);
        const bool on_eye = (it != m_regions.end() && it->second.region == Region::Eye
                             && it->second.row < m_rows.size());
        const ETCS::RID subject = on_eye ? m_rows[it->second.row].layer : 0;
        if (subject == m_hovering) return;          // nothing changed; do not re-walk the stack
        m_hovering = subject;
        /*
     * THE TARGET, NOT THE VALUE. A hover has a DURATION -- the pointer rests on
     * the eye for as long as the question is being asked -- and a snap to the
     * dim is the one thing that throws that away. Set where the isolation is
     * going; Tick walks it there a frame at a time.
     */
        if (subject != 0) m_subject = subject;
        m_dim_target = (subject != 0) ? m_hover_dim : 1.0f;
    }

    /*
 * ── the fade, one frame at a time ────────────────────────────────────────
 *
 * The panel claims Animated (ontology/Animated.h) and is stepped by whatever
 * drives that family, and only while there is somewhere left to go: Animating
 * is what keeps the driver from re-compositing the document sixty times a
 * second for a dim that has arrived.
 *
 * THE SUBJECT SURVIVES THE FADE OUT. On the way in it is the layer whose eye is
 * under the pointer; on the way out there is no layer under the pointer at all,
 * and naming nobody would dim the one that was being shown before bringing it
 * back -- a flash of exactly the wrong thing. So the last subject is held until
 * the dim reaches 1.0 and everything is at full strength again, at which point
 * it means nothing and is dropped.
 */
    bool Fading() const { return m_dim_now != m_dim_target; }

    bool AnimatingConcrete() override { return m_document && Fading(); }

    /*
 * A DURATION, NOT A PER-VISIT STEP. This used to move 0.085 per frame, which
 * read as a fade only because the display happened to be running at sixty --
 * on a 144Hz panel the same code is a flicker and on a 30Hz one it is a slide.
 * The whole swing takes FADE_FULL_MS whatever the rate; the isolation depth is
 * a fraction of that swing, so the actual fade is proportionally shorter and
 * the two ends stay in step with each other, which is what makes leaving an eye
 * feel like the reverse of arriving at one.
 */
    void AdvanceConcrete(double dt_ms) override
    {
        constexpr float FADE_FULL_MS = 150.0f;
        const float step = static_cast<float>(dt_ms) / FADE_FULL_MS;
        if (m_dim_now < m_dim_target) m_dim_now = std::min(m_dim_target, m_dim_now + step);
        else                          m_dim_now = std::max(m_dim_target, m_dim_now - step);
        m_document->IsolateLayer(m_subject, m_dim_now);
        if (m_dim_now >= 1.0f) m_subject = 0;
        repaint();
    }

    // The rename by verb: a caller that already has the whole name -- a script,
    // the page, a test -- does not have to type it a key at a time.
    bool CommitRename(const std::string& name)
    {
        if (m_renaming == 0 || !m_document) return false;
        m_document->RenameLayer(m_renaming, name);
        m_editing = false;
        m_edit.clear();
        m_renaming = 0;
        Refresh();
        return true;
    }

    /*
 * A KEY WHILE A NAME IS OPEN, and nothing otherwise.
 *
 * Returns true when it was consumed, which is the whole of what the input edge
 * needs: while this is open the panel is where typing goes, and every key it
 * takes is one the canvas does not act on. Enter keeps the name, Escape drops
 * it, Backspace edits; anything the key map has no character for is swallowed
 * rather than ignored, so a function key pressed mid-rename does not fall
 * through to a tool.
 *
 * GLFW's codes, named for the same reason PaintInput names them.
 */
    bool KeyIn(uint16_t key)
    {
        if (!m_editing) return false;
        constexpr uint16_t KEY_ESCAPE = 256, KEY_ENTER = 257, KEY_BACKSPACE = 259;
        if (key == KEY_ENTER)     { end_edit(true);  return true; }
        if (key == KEY_ESCAPE)    { end_edit(false); return true; }
        if (key == KEY_BACKSPACE)
        {
            if (!m_edit.empty()) m_edit.pop_back();
            Refresh();
            return true;
        }
        const char ch = paint_key_to_char(key);
        if (ch == 0) return true;
        if (m_edit.size() < 48) m_edit.push_back(ch);
        Refresh();
        return true;
    }

    bool editing() const { return m_editing; }

    void Report() const
    {
        ETCS_LOG("PaintLayerPanel", m_rows.size() << " row(s), scroll " << m_scroll
                 << ", document " << (m_document ? "bound" : "UNBOUND")
                 << (m_renaming ? " [renaming]" : "")
                 << (m_dragging ? " [dragging]" : "")
                 << (m_moving ? " [moving window]" : ""));
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
        repaint();
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
        repaint();
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
        repaint();
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
    // nowhere near this window", which is the call a pane-leave makes. By ROW
    // rather than by node, so a caller that is not a pointer can ask for the
    // same isolation the eye's hover gives (see Hover).
    void HoverRow(int32_t row)
    {
        if (!m_document) return;
        const ETCS::RID subject =
            (row >= 0 && static_cast<size_t>(row) < m_rows.size())
                ? m_rows[static_cast<size_t>(row)].layer : 0;
        if (subject == m_hovering) return;
        m_hovering = subject;
        if (subject != 0) m_subject = subject;
        m_dim_target = (subject != 0) ? m_hover_dim : 1.0f;
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
        CancelRowDrag();
        m_moving = false;
        HoverRow(-1);
    }

    size_t rowCount() const { return m_rows.size(); }
    ETCS::RID rowLayer(size_t i) const { return i < m_rows.size() ? m_rows[i].layer : 0; }
    int32_t scroll() const { return m_scroll; }

    /*
 * IS THIS NODE ONE OF MINE -- asked by the router, answered from the index the
 * press path already builds (m_regions, plus the pane the title bar moves).
 *
 * Exists so somebody else can decide what a press ELSEWHERE means without
 * having to know what this panel is made of. The alternative was exporting the
 * row layout, or re-running the pick; this is the one bit either of those would
 * have been used to compute.
 */
    bool owns(ETCS::RID node) const
    {
        if (node == 0) return false;
        if (node == m_window) return true;
        return m_regions.find(node) != m_regions.end();
    }

    /*
 * CLOSE THE NAME FIELD FROM OUTSIDE, keeping what was typed.
 *
 * KEEPING, not dropping, because that is what a press somewhere else means
 * everywhere else: the text is the user's work and leaving is not a cancel.
 * Escape is still the cancel and is still the only one (KeyIn).
 *
 * Idempotent -- end_edit returns immediately when nothing is open -- so the
 * router may call it on every press without first asking whether it needs to.
 */
    void CloseEdit() { end_edit(true); }

private:
    /*
     * TRIM IS A LIST, and it is a list because there turned out to be more than
     * one of them and no way to tell in advance how many. A row's parts divide
     * into CONTROLS (a press on them means something, so each needs its own
     * name and its own Region) and DECORATION that has no meaning of its own
     * and must simply appear and disappear with the row: the eye's pupil, the
     * grip's dots. `pupil` was a named slot for the first of those, which made
     * the second one -- added in the same patch, four lines away in the row
     * script -- have nowhere to go, so it was never registered, never hidden,
     * and six sets of dots floated over an empty panel. A slot per decoration
     * is a bug waiting for the next decoration; a list is not.
     */
    struct Row { ETCS::RID bg, eye, thumb, label, del; ETCS::RID layer;
                 ETCS::RID grip = 0, mup = 0, mdn = 0;
                 ETCS::RID iris = 0;             // tinted with the eye -- see RowNode
                 std::vector<ETCS::RID> trim; };
    struct Hit { size_t row; Region region; };
    bool m_collapsed = false;
    // The title bar's own eye (BeginTitle), tinted open or shut with the
    // window, and the window's body parts, hidden with the rows.
    bool      m_title_open = false;
    ETCS::RID m_view_eye   = 0;
    ETCS::RID m_view_iris  = 0;
    std::vector<ETCS::RID> m_view_trim;
    std::vector<ETCS::RID> m_body;

    void tint_eye(ETCS::RID eye, ETCS::RID iris, bool open)
    {
        const float* o = open ? m_eye_shown : m_eye_hidden;
        const float* i = open ? m_iris_open : m_iris_shut;
        paint_node_fill(eye,  o[0], o[1], o[2], 1.0f);
        if (iris) paint_node_fill(iris, i[0], i[1], i[2], 1.0f);
    }

    // The title bar's eye says whether the rows are showing; shut, its pupil
    // goes with the iris it sits on.
    void tint_view_eye()
    {
        if (!m_view_eye) return;
        tint_eye(m_view_eye, m_view_iris, !m_collapsed);
        for (ETCS::RID n : m_view_trim) paint_node_hidden(n, m_collapsed);
    }

    float m_iris_open[3] = { 0.35f, 0.55f, 0.95f };
    float m_iris_shut[3] = { 0.106f, 0.110f, 0.078f };
    float m_row_drag[3]  = { 0.86f, 0.60f, 0.22f };

    // ── the row drag's geometry, all in the window's own space ───────────
    ETCS::RID m_ghost = 0, m_ghost_thumb = 0, m_ghost_label = 0, m_drop_mark = 0;
    bool      m_drag_live = false;
    Point2D   m_drag_press{ 0, 0 };
    int32_t   m_drag_grab_dy = 0;      // where in its row the grip was taken

    Point2D window_local(Point2D at) const
    {
        ETCS::Held<Drawable2D_> w = ETCS::resolve_held<Drawable2D_>("Drawable2D", m_window);
        if (!w) return at;
        const Rect2D b = w->Bounds();
        return Point2D{ at.x - b.x, at.y - b.y };
    }

    // A row's plate is its pane, so its box in the window is the row's.
    Rect2D row_rect(size_t i) const
    {
        if (i >= m_rows.size()) return Rect2D{ 0, 0, 0, 0 };
        ETCS::Held<Drawable2D_> r = ETCS::resolve_held<Drawable2D_>("Drawable2D", m_rows[i].bg);
        return r ? r->Bounds() : Rect2D{ 0, 0, 0, 0 };
    }

    int32_t last_layer_row() const
    {
        int32_t last = -1;
        for (size_t i = 0; i < m_rows.size(); ++i) if (m_rows[i].layer) last = static_cast<int32_t>(i);
        return last;
    }

    int32_t row_of_layer(ETCS::RID layer) const
    {
        for (size_t i = 0; i < m_rows.size(); ++i) if (m_rows[i].layer == layer) return static_cast<int32_t>(i);
        return -1;
    }

    // The row a height lands on: the first whose bottom edge is below it, so
    // the gap under a row belongs to that row, and anything past the last
    // layer is the last layer.
    int32_t row_at(int32_t ly) const
    {
        const int32_t last = last_layer_row();
        for (int32_t i = 0; i <= last; ++i)
        {
            const Rect2D r = row_rect(static_cast<size_t>(i));
            if (ly < r.y + static_cast<int32_t>(r.h) + 2) return i;
        }
        return last;
    }

    void show_ghost()
    {
        if (!m_ghost) return;
        PaintLayer* layer = nullptr;
        if (ETCS::Entity* raw = paint_resolve_tag("PaintLayer", m_dragging))
            layer = static_cast<PaintLayer*>(raw->getTrueType());
        if (layer)
        {
            if (m_ghost_thumb) paint_thumb(m_ghost_thumb, layer);
            if (m_ghost_label) paint_node_text(m_ghost_label, layer->name());
        }
        paint_node_fill(m_ghost, m_row_drag[0], m_row_drag[1], m_row_drag[2], 0.92f);
        paint_node_hidden(m_ghost, false);
    }

    // The bar goes in the gap the layer will land in: above the target when
    // it moves up, below it when it moves down, nowhere when it would not move.
    void place_mark(int32_t from, int32_t target)
    {
        if (!m_drop_mark) return;
        if (target < 0 || target == from) { paint_node_hidden(m_drop_mark, true); return; }
        const Rect2D r = row_rect(static_cast<size_t>(target));
        const int32_t y = (from >= 0 && target < from) ? r.y - 2 : r.y + static_cast<int32_t>(r.h);
        paint_node_moved(m_drop_mark, r.x, y);
        paint_node_hidden(m_drop_mark, false);
    }

    void end_row_drag()
    {
        m_dragging = 0;
        m_drag_live = false;
        if (m_ghost) paint_node_hidden(m_ghost, true);
        if (m_drop_mark) paint_node_hidden(m_drop_mark, true);
    }

    /*
 * Closing the field, keeping the name or not. Rename goes through the document
 * (RenameLayer) rather than the layer, because a rename is a change to the
 * page -- it is what Touch and therefore the window's own refresh hang off.
 * An empty name is a cancel: a layer with no name is a row you cannot read.
 */
    void begin_edit(ETCS::RID layer)
    {
        if (m_editing) end_edit(false);
        if (!m_document) return;
        m_document->SetActiveLayer(layer);
        m_renaming = layer;
        m_editing  = true;
        m_edit.clear();
        if (ETCS::Entity* raw = paint_resolve_tag("PaintLayer", layer))
            m_edit = static_cast<PaintLayer*>(raw->getTrueType())->name();
        ETCS_LOG("PaintLayerPanel", "renaming RID:" << layer
                 << " -- type, Enter to keep, Esc to drop it.");
        Refresh();
    }

    void end_edit(bool keep)
    {
        if (!m_editing) { m_renaming = 0; return; }
        const ETCS::RID subject = m_renaming;
        const std::string text = m_edit;
        m_editing = false;
        m_edit.clear();
        m_renaming = 0;
        if (keep && m_document && subject != 0 && !text.empty())
        {
            m_document->RenameLayer(subject, text);
            ETCS_LOG("PaintLayerPanel", "RID:" << subject << " renamed \"" << text << "\"");
        }
        Refresh();
    }

    /*
 * ── the row's picture of its layer ───────────────────────────────────────
 *
 * WHAT IS ON THE LAYER, drawn into whatever raster the script put in the row.
 * A name tells you which layer you MEANT; a thumbnail tells you which one you
 * are looking at, and with two imported images and a paper layer the names are
 * all anybody has to go on otherwise.
 *
 * BY RID, INTO SOMEBODY ELSE'S PIXELS, which is the same seam draw_lift uses:
 * the node is a compositor from another module, reached as Pixels_ and written
 * directly (CompositeDrawable2D's own note says it is reached as Pixels_ and
 * never as a concrete type). It has to be RETAINED in the script, or its next
 * recompose clears what was just written.
 *
 * AVERAGED, NOT SAMPLED, and that is the difference between a thumbnail and a
 * blank square. A page is 1024 wide and a thumb is 24, so one nearest-neighbour
 * sample per destination pixel reads one source pixel in 1800 and a six-pixel
 * stroke across the picture survives in none of them -- measured: a drawn
 * layer's thumbnail came back as bare checker. Each destination pixel now
 * averages its whole source cell, at up to 8x8 evenly spaced taps, so a thin
 * mark arrives faint rather than absent and the cost stays fixed however big
 * the page is.
 *
 * FITTED: the thumb is square and a page is not, so the picture is scaled by
 * the larger axis and centred, which is what makes two layers of one document
 * produce thumbnails that line up. Over a checker,
 * because the common case is a layer that is mostly transparent and a
 * transparent thumbnail drawn on a dark row is indistinguishable from an empty
 * one.
 */
    static void paint_thumb(ETCS::RID node, PaintLayer* layer)
    {
        if (node == 0) return;
        Pixels_* dst = ETCS::resolve_in_family<Pixels_>("Pixels", node);
        if (!dst) return;
        uint8_t* out = dst->PixelData();
        if (!out) return;
        const int32_t tw = static_cast<int32_t>(dst->PixelWidth());
        const int32_t th = static_cast<int32_t>(dst->PixelHeight());
        if (tw <= 0 || th <= 0) return;

        const int32_t lw = layer ? static_cast<int32_t>(layer->width())  : 0;
        const int32_t lh = layer ? static_cast<int32_t>(layer->height()) : 0;
        const uint8_t* src = layer ? layer->PixelData() : nullptr;
        const float scale = (lw > 0 && lh > 0)
            ? std::max(static_cast<float>(lw) / tw, static_cast<float>(lh) / th) : 0.0f;
        const int32_t ox = (lw > 0 && scale > 0.0f)
            ? (tw - static_cast<int32_t>(lw / scale)) / 2 : 0;
        const int32_t oy = (lh > 0 && scale > 0.0f)
            ? (th - static_cast<int32_t>(lh / scale)) / 2 : 0;

        for (int32_t y = 0; y < th; ++y)
            for (int32_t x = 0; x < tw; ++x)
            {
                // The checker under everything, 4px squares.
                const bool light = (((x >> 2) + (y >> 2)) & 1) != 0;
                float r = light ? 0.44f : 0.34f, g = r, b = r;

                if (src && scale > 0.0f)
                {
                    const int32_t sx0 = static_cast<int32_t>((x - ox) * scale);
                    const int32_t sy0 = static_cast<int32_t>((y - oy) * scale);
                    const int32_t sx1 = std::min(static_cast<int32_t>((x - ox + 1) * scale), lw);
                    const int32_t sy1 = std::min(static_cast<int32_t>((y - oy + 1) * scale), lh);
                    if (sx0 >= 0 && sy0 >= 0 && sx0 < lw && sy0 < lh && sx1 > sx0 && sy1 > sy0)
                    {
                        const int32_t stepx = std::max(1, (sx1 - sx0) / 8);
                        const int32_t stepy = std::max(1, (sy1 - sy0) / 8);
                        float ar = 0, ag = 0, ab = 0, aa = 0; int taps = 0;
                        for (int32_t sy = sy0; sy < sy1; sy += stepy)
                            for (int32_t sx = sx0; sx < sx1; sx += stepx)
                            {
                                const uint8_t* sp = src + (static_cast<size_t>(sy) * lw + sx) * 4;
                                const float a = sp[3] / 255.0f;
                                ar += (sp[0] / 255.0f) * a; ag += (sp[1] / 255.0f) * a;
                                ab += (sp[2] / 255.0f) * a; aa += a;
                                ++taps;
                            }
                        if (taps > 0 && aa > 0.0f)
                        {
                            // Premultiplied while summing, so a cell that is
                            // mostly transparent keeps the colour of the part
                            // that is not, at that part's weight.
                            const float cover = aa / taps;
                            r = r * (1.0f - cover) + (ar / aa) * cover;
                            g = g * (1.0f - cover) + (ag / aa) * cover;
                            b = b * (1.0f - cover) + (ab / aa) * cover;
                        }
                    }
                }
                uint8_t* dp = out + (static_cast<size_t>(y) * tw + x) * 4;
                dp[0] = paint_to_byte(r); dp[1] = paint_to_byte(g);
                dp[2] = paint_to_byte(b); dp[3] = 255;
            }
        etcs_mark_observed(dst);
    }

    // What the window changed, shown. Silent with nothing bound: a panel driven
    // from a test or a script has no view to put back.
    void repaint() { if (m_surface) m_surface->Render(); }

    PaintDocument* m_document = nullptr;
    PaintSurface*  m_surface  = nullptr;
    // The pane the title bar moves, and the move in flight: where the window
    // and the pointer were at the press, so each motion is a fresh offset from
    // there rather than a sum of deltas that drifts.
    ETCS::RID m_window = 0;
    // The name being typed, and whether anything is being typed at all --
    // m_renaming alone said "armed", which is not the same as "has the
    // keyboard" (KeyIn).
    // The fade: what is shown at full strength, where the dim on everything
    // else is going, and where it has got to (Tick).
    ETCS::RID   m_subject    = 0;
    float       m_dim_target = 1.0f;
    float       m_dim_now    = 1.0f;
    bool        m_editing = false;
    std::string m_edit;
    bool      m_moving = false;
    Point2D   m_grab{ 0, 0 };
    Point2D   m_origin{ 0, 0 };
    std::vector<Row> m_rows;
    std::unordered_map<ETCS::RID, Hit> m_regions;
    int32_t   m_scroll    = 0;
    ETCS::RID m_renaming  = 0;
    ETCS::RID m_dragging  = 0;
    ETCS::RID m_hovering  = 0;
    float m_hover_dim = 0.25f;
    /*
 * THE DEFAULTS ARE THE RULER'S, so a script that never calls SetEyeColors or
 * SetRowColors gets the panel that belongs to this window rather than one a
 * shade off the canvas background -- see paint_layers.etcs for the argument.
 * Ink and band are PaintSurface::m_ruler_ink and m_ruler_bg; the selected row
 * is the ruler's in-band highlight, the colour a tick takes inside the page.
 */
    float m_eye_shown[3]  = { 0.94f, 0.89f, 0.78f };
    float m_eye_hidden[3] = { 0.35f, 0.36f, 0.28f };
    float m_row_sel[3]    = { 0.35f, 0.55f, 0.95f };
    float m_row_idle[3]   = { 0.15f, 0.16f, 0.11f };
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

    /*
 * BOTH NUMBERS AT ONCE, which the steppers cannot express. They move in 64s,
 * and 1080 is not a multiple of 64 -- so the one extent most people actually
 * want, 1920x1080, was not reachable from this menu at all however long you
 * held the +. The presets in the script call this; the steppers still do the
 * fine work from wherever it lands.
 */
    void SetExtent(int32_t w, int32_t h)
    {
        m_width  = clamp_px(w);
        m_height = clamp_px(h);
        push_readouts();
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

    /*
 * WHICH WAY THE PICTURE GOES, drawn on the cells it is not anchored to.
 *
 * A bright cell says which part of the page stays put, and that is one fact
 * short of the question anyone actually has: where does everything else move?
 * Every other cell now carries an arrow pointing away from the chosen one,
 * which is the direction the new room appears in -- so the grid reads as a
 * diagram of the resize rather than as nine buttons.
 *
 * ASCII, because the glyph table is ASCII (RenderProvider's TextLabel font is
 * 0x20..0x7E). The two diagonals share a stroke each: "\\" is up-left and
 * down-right, "/" is up-right and down-left, which is what those characters
 * already look like.
 */
    void BindAnchorArrow(int32_t index, ETCS::RID label)
    {
        if (index < 0 || index > 8 || label == 0) return;
        m_arrows[index] = label;
        show_anchor();
    }

    void ApplyResize()
    {
        if (!m_document) { ETCS_LOG("PaintCanvasMenu", "resize: no document -- Create first."); return; }
        if (m_document->Resize(static_cast<uint32_t>(m_width), static_cast<uint32_t>(m_height), m_anchor)
            && m_surface)
            m_surface->Render();
    }

    /*
 * NEW IS A NEW PAGE, not a wiped one. It used to call PaintDocument::New,
 * which clears the layers where they stand -- so the picture that was there
 * was gone, the history gained nothing, and the page list stayed empty however
 * many times it was pressed. Through the store instead: the present is saved
 * to its slot first and a fresh page opens at the size the two steppers show,
 * which is what makes a second page exist to switch back to.
 *
 * Without a store bound -- a native session with no database -- it falls back
 * to the document's own New, which is the same picture minus the history.
 */
    void ApplyNew()
    {
        if (!m_document) { ETCS_LOG("PaintCanvasMenu", "new: no document -- Create first."); return; }
        bool ok = false;
        if (m_pages)
            ok = m_pages->NewAt(static_cast<uint32_t>(m_width), static_cast<uint32_t>(m_height));
        else
            ok = m_document->New(static_cast<uint32_t>(m_width), static_cast<uint32_t>(m_height));
        if (!ok) return;
        refresh_pages();
        if (m_surface) m_surface->Render();
    }

    /*
 * SAVE IS THE STORE'S. With a page store bound, the menu's save puts the
 * present page in its slot (PaintPages::Save, the same sqlite the page list
 * below reads) rather than handing a PNG to the browser -- the header's
 * download button is where a file leaves the page, and a second control that
 * did the same thing under the word "save" read as the store not working.
 * Without a store (a native session with no database) it falls back to the
 * page's download, which is then the only place a picture can go.
 *
 * LOAD IS STILL THE PAGE'S: it takes a file, and a path is something the
 * SUBSTRATE produces -- in the browser the upload control's dialog turns one
 * into a file, and there is no dialog on the desktop side yet. So under
 * emscripten this raises a DOM event the page answers with the same code its
 * header button runs; natively it names the verb to type. Loading a STORED
 * page is a press on its row in the list below.
 */
    void Save()
    {
        if (m_pages) { if (m_pages->Save()) refresh_pages(); return; }
        page_event("save");
    }
    void Load() { page_event("load"); }

    /*
 * A FILE HAS ARRIVED, AND THE PAGE ASKS WHAT IT IS FOR: another layer of the
 * picture (PaintDocument::ImportImage) or a new picture the file's size
 * (PaintDocument::ImportCanvas). The question is a popup on the sheet --
 * PaintProvider/scripts/paint_import.etcs, opened through the palette
 * (PaintPalette::OpenPopup) so it dismisses as every popup does -- and the
 * answer is one of the three verbs below, which the prompt's buttons call
 * (PaintPalette::AddCall). Held here rather than asked by the page's JS,
 * because "a file came in" is the same event on every substrate and the choice
 * belongs with the canvas that will act on it.
 *
 * With no prompt bound (a native session driven from the terminal) the file
 * goes in as a layer at once: an unanswerable question is not a wait.
 */
    /*
 * ── the pages ────────────────────────────────────────────────────────────
 *
 * The store keeps every page and ctrl+PageUp/PageDown walks them
 * (PaintPages); the list you can SEE is PaintPagePanel, on this menu's pane,
 * bound to the same store. What this type keeps of the pages is `new` (above)
 * and the two readouts, which have to follow a load made from the list --
 * PageChanged is how the panel says so, by verb, since it is declared before
 * this type.
 */
    void BindPages(ETCS::RID pages)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintPages", pages);
        if (raw) m_pages = static_cast<PaintPages*>(raw->getTrueType());
    }

    // The page on screen changed under the menu: the steppers show its size.
    void PageChanged()
    {
        if (!m_document) return;
        m_width  = std::clamp(static_cast<int32_t>(m_document->width()),  MIN_PX, MAX_PX);
        m_height = std::clamp(static_cast<int32_t>(m_document->height()), MIN_PX, MAX_PX);
        push_readouts();
        refresh_pages();
    }

    // The reel a multi-frame GIF goes to on arrival (OfferImport).
    void BindAnimation(ETCS::RID anim) { m_anim = anim; }

    // The list on this pane, told to re-read the store after this type
    // changed it (a new page, a save). By verb, for the ordering reason above.
    void BindPagePanel(ETCS::RID panel) { m_page_panel = panel; }
    void refresh_pages()
    {
        if (m_page_panel == 0) return;
        if (ETCS::Entity* e = paint_resolve_tag("PaintPagePanel", m_page_panel))
        {
            ETCS::Buffer act; act.write("PaintPagePanel.Refresh");
            ETCS::Buffer arg;
            try { e->call(act, arg); } catch (...) {}
        }
    }

    /*
 * The tool an import leaves selected. Bound here rather than reached through
 * the palette because what happens after a file arrives is this type's
 * business (answer_import), and the palette's tool pointer is its own.
 */
    void BindTool(ETCS::RID tool)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintTool", tool);
        if (raw) m_tool = static_cast<PaintTool*>(raw->getTrueType());
    }

    void BindImportPrompt(ETCS::RID palette, ETCS::RID pane, ETCS::RID input, ETCS::RID caption)
    {
        m_prompt_palette = palette;
        m_prompt_pane    = pane;
        m_prompt_input   = input;
        m_prompt_caption = caption;
    }

    void OfferImport(const std::string& path)
    {
        /*
     * A GIF WITH MORE THAN ONE FRAME IS A REEL, not a picture, and it goes to
     * the animation without asking: the one thing the file can be for is the
     * one thing it is. A still GIF is a picture and takes the ordinary
     * question below. Decided here rather than by the page, because "a file
     * came in" is the same event on every substrate and this is where the
     * canvas decides what a file is for.
     */
        if (m_anim != 0 && paint_gif_frames_in(path) > 1)
        {
            if (ETCS::Entity* a = paint_resolve_tag("PaintAnimation", m_anim))
                static_cast<PaintAnimation*>(a->getTrueType())->ImportGif(path);
            return;
        }
        m_pending = path;
        if (m_prompt_palette == 0 || m_prompt_pane == 0 || m_prompt_input == 0)
        {
            ETCS_LOG("PaintCanvasMenu", "import " << path << ": no prompt bound -- as a layer.");
            ImportAsLayer();
            return;
        }
        paint_node_text(m_prompt_caption, paint_path_stem(path));
        if (ETCS::Entity* p = paint_resolve_tag("PaintPalette", m_prompt_palette))
            static_cast<PaintPalette*>(p->getTrueType())->OpenPopup(m_prompt_pane, m_prompt_input);
    }

    void ImportAsLayer()  { answer_import(false); }
    void ImportAsCanvas() { answer_import(true); }
    void ImportCancel()
    {
        m_pending.clear();
        close_prompt();
    }

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

    // The prompt's answer. Closing first, since the popup's close re-renders
    // the surface and the import's own render should be the last word. The
    // menu's pending extent follows a new canvas so "resize" afterwards starts
    // from the page as it now is.
    void answer_import(bool as_canvas)
    {
        const std::string path = m_pending;
        m_pending.clear();
        close_prompt();
        if (path.empty() || !m_document) return;
        const bool ok = as_canvas ? m_document->ImportCanvas(path) : m_document->ImportImage(path);
        if (ok && as_canvas)
        {
            m_width  = std::clamp(static_cast<int32_t>(m_document->width()),  MIN_PX, MAX_PX);
            m_height = std::clamp(static_cast<int32_t>(m_document->height()), MIN_PX, MAX_PX);
            push_readouts();
        }
        /*
     * THE PICTURE ARRIVES SELECTED, WITH THE TOOL THAT MOVES IT. What anyone
     * does first with an imported image is put it where they want it, and that
     * took three steps nobody was told about: pick select, draw a region around
     * the image, then drag. The import already knows the extent -- the new
     * layer's own raster is the image -- so it states it as the selection and
     * leaves the select tool holding it: press inside and drag, and the first
     * press lifts it (PaintInput's carry).
     *
     * On the layer it just made, which is the active one, so the lift cuts from
     * the image and not from whatever was underneath (SetActiveLayer's note).
     */
        if (ok)
        {
            if (PaintLayer* at = m_document->activeLayer())
                m_document->SelectRect(0, 0,
                    static_cast<int32_t>(at->width()) - 1,
                    static_cast<int32_t>(at->height()) - 1);
            if (m_tool) m_tool->SetKind("select");
        }
        if (ok && m_surface) m_surface->Render();
    }

    void close_prompt()
    {
        if (m_prompt_palette == 0) return;
        if (ETCS::Entity* p = paint_resolve_tag("PaintPalette", m_prompt_palette))
            static_cast<PaintPalette*>(p->getTrueType())->ClosePopup();
    }

    // By verb, since these are somebody else's nodes -- see paint_node_verb.
    void push_readouts()
    {
        paint_node_text(m_w_label, std::to_string(m_width));
        paint_node_text(m_h_label, std::to_string(m_height));
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
            if (m_cells[i] != 0)
            {
                if (i == m_anchor) paint_node_fill(m_cells[i], 0.79f, 0.71f, 0.35f, 1.0f);
                else               paint_node_fill(m_cells[i], 0.22f, 0.22f, 0.27f, 1.0f);
            }
            if (m_arrows[i] == 0) continue;
            // Away from the anchor, by the sign of the difference in grid
            // coordinates -- so a cell two columns over reads the same as one,
            // which is right: it is a direction, not a distance.
            const int dx = (i % 3) - (m_anchor % 3);
            const int dy = (i / 3) - (m_anchor / 3);
            const char* mark = "";
            if      (dx == 0 && dy == 0) mark = "";
            else if (dx == 0)            mark = (dy < 0) ? "^" : "v";
            else if (dy == 0)            mark = (dx < 0) ? "<" : ">";
            else                         mark = ((dx < 0) == (dy < 0)) ? "\\" : "/";
            paint_node_text(m_arrows[i], mark);
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
    ETCS::RID m_cells[9]  = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    ETCS::RID m_arrows[9] = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    // The import prompt (BindImportPrompt) and the file it is asking about.
    ETCS::RID   m_prompt_palette = 0;
    ETCS::RID   m_prompt_pane    = 0;
    ETCS::RID   m_prompt_input   = 0;
    ETCS::RID   m_prompt_caption = 0;
    PaintTool*  m_tool = nullptr;
    PaintPages* m_pages = nullptr;
    ETCS::RID   m_page_panel = 0;        // the list on this pane -- see BindPagePanel
    ETCS::RID   m_anim = 0;              // where a multi-frame GIF goes -- see OfferImport
    std::string m_pending;
};


/*
 * ── PaintNode: a shared session, and who may do what to it ───────────────────
 *
 * THE SAME CENTRALIZATION ARTIFACT ChessNode names itself as, for the same
 * structural reason and with the same consequence: a browser has no listening
 * socket, so a page cannot be a server. A host PUSHES its notebook here and
 * viewers PULL it, and the asymmetry is not a design preference -- it is the
 * only arrangement available until there is a peer link. A node hosting many
 * sessions is a server; a node hosting one is a peer.
 *
 * IT HOLDS LINES, NOT A DOCUMENT. This type never decodes an entry, never owns
 * a PaintDocument and never draws anything. It assigns sequence numbers, checks
 * a token against a role, rewrites one field and stores the rest verbatim --
 * which is precisely what lets a node built today relay an entry kind added to
 * PaintProvider tomorrow. A relay that understood its payload would have to be
 * rebuilt every time the payload grew.
 *
 * IT IS THE ORDERING DOMAIN. Sequence numbers are assigned here and nowhere
 * else, so the host and every viewer agree on what happened in what order by
 * construction rather than by reconciliation. The host draws its own stroke
 * locally the instant it is made -- the input arriving is what drives the
 * picture -- and pushes afterwards, so two writers marking the same pixels can
 * see them settle in different orders on their own screens until the next
 * snapshot. That is the honest cost of local echo and it is stated rather than
 * hidden; the alternative is a round trip before your own ink appears.
 *
 * ── the link IS the right to view ───────────────────────────────────────────
 *
 * There is no knocking and nothing to admit. Holding the session's id is what
 * makes you a reader, because that is what a link means to everyone who has
 * ever been sent one -- and a door that has to be answered is a door somebody
 * has to be sitting at.
 *
 * WHICH MAKES THE SESSION ID A SECRET, and that is not a side effect to be
 * tolerated, it is the whole security model. So it is minted here exactly as a
 * token is -- sixteen characters from random_device, never a name anybody
 * types -- and THERE IS NO LISTING VERB. A node that could enumerate its
 * sessions would be handing out every capability it holds; the one that used to
 * be here was written for chess, where being findable is the point, and it is
 * precisely wrong here.
 *
 * A TOKEN IS STILL A ROLE, and that is the part the host does decide. Arriving
 * by link makes you a reader; writing takes an elevation the host performs by
 * hand in the visitor menu. So the roster is name -> token -> role, the check at
 * each verb is a role check rather than a membership check, and the host's own
 * page holds a token too (role owner) -- one code path in, and no "am I the
 * host" special case anywhere in it.
 *
 * Elevation does not open a second channel -- it lets that name's entries into
 * the one that already exists. Revocation is dropping the token: the next read
 * answers FORBIDDEN and that page falls back to its own local document, which
 * it has had all along.
 *
 * ── the path surface ─────────────────────────────────────────────────────────
 *
 *   /<mount>/<self>/open                              start one; "<session> <token>"
 *   /<mount>/<self>/join/<session>                    the link; answers a token
 *   /<mount>/<self>/<token>/<session>/read/<since>    entries after <since>
 *   /<mount>/<self>/<token>/<session>/head            the newest sequence
 *   /<mount>/<self>/<token>/<session>/push            POST body: entries
 *   /<mount>/<self>/<token>/<session>/who             the roster (owner)
 *   /<mount>/<self>/<token>/<session>/role/<name>/<r> reader|writer|out (owner)
 *   /<mount>/<self>/<token>/<session>/close           end it, and everyone in it
 *
 * `open` and `join` are the only verbs reachable without a token, and both are
 * reserved words in the segment a token would occupy. Tokens and session ids are
 * hex, so the two spaces cannot collide -- the same shape ChessNode's reserved
 * selves and reserved matches already have.
 *
 * NOTE THAT `open` DOES NOT TAKE A NAME. The host cannot choose the id, because
 * an id anybody could choose is an id anybody could guess, and guessing it is
 * the whole of getting in.
 *
 * THIS IS A STRUCTURED ROUTE (HttpServer::AddRequestRoute): it needs the METHOD
 * to tell a push from a read and the BODY to receive one, and it answers `read`
 * by REFERENCE because a batch of entries is not going to fit in 255 bytes.
 * Neither was possible before the NetworkProvider change that went in with it.
 */
/*
 * ── PaintVisitors: who is in the session, drawn on the sheet ─────────────────
 *
 * THE MENU IS IN THE CANVAS BECAUSE IT CAN BE. The page used to hold this, and
 * my reason was that a share panel needs TEXT ENTRY -- a node address, a name, a
 * session to type -- which a PaintCanvasMenu pane has no field for. The link
 * removed all three: the id is minted, the name is remembered, and the node is
 * whoever served the page. What is left is names, roles and buttons, which is
 * exactly what these panes are made of.
 *
 * ROWS ARE DECLARED BY THE SCRIPT, NOT SPAWNED HERE -- the same shape
 * PaintLayerPanel uses, and for the same reason: the LOOK belongs to the script
 * that draws it and this type only says what each row currently MEANS. A fixed
 * pool with the unused rows hidden also means no allocation on a roster change,
 * which happens on a timer.
 *
 * AND THE BUTTONS ARE PALETTE CALLS. Each row's controls are ordinary
 * PaintPalette::AddCall entries naming a verb here with the node pressed as the
 * argument, which is how every control in paint_menu.etcs already works. What
 * the input path does know about this window is the two things a palette call
 * cannot carry: where the pointer is, for dragging it by its title, and the
 * keys, while a name is being typed (PaintInput::BindVisitors).
 *
 * WHAT IT DOES NOT DO: talk to the node. It cannot -- a fetch belongs to the
 * page. So a press raises an event the page listens for, the page performs the
 * verb against the node, and the answer comes back as a fresh roster. One
 * direction each way, and this type never learns what a session is.
 */
class PaintVisitors : public DeletableBase<PaintVisitors>
{
public:
    WIRE_TYPE_IDENTITY(PaintVisitors);

    PaintVisitors() = default;
    bool DeleteConcrete() override { return true; }

    bool Create()
    {
        m_rows.clear();
        this->addTag("active");
        return true;
    }

    /*
 * ── A ROW IS ASSEMBLED, AND ITS BUTTONS NAME THEMSELVES ──────────────────
 *
 * Same shape as PaintLayerPanel's, and the second half is the part that makes
 * one row script serve eight rows: the press verbs take the NODE that was
 * pressed rather than a row index, so a palette call is written once with no
 * number in it. An index would have had to be a literal in the script, which
 * means a file per row, which is the duplication this replaced.
 *
 * The buttons (and the words on them) are the HOST's: every one of them is
 * hidden on a guest's window, and on the host's own row, where they could only
 * ever be refused.
 */
    void BeginRow() { m_rows.push_back(Row{}); }

    void RowNode(const std::string& what, ETCS::RID node)
    {
        if (m_rows.empty()) { ETCS_LOG("PaintVisitors", "RowNode before BeginRow -- ignored."); return; }
        if (node == 0) return;
        Row& row = m_rows.back();
        if      (what == "bg")   row.bg   = node;
        else if (what == "name") row.name = node;
        else if (what == "role") row.role = node;
        else if (what == "chip") row.chip = node;
        else if (what == "up" || what == "down" || what == "out")
        {
            m_buttons[node] = m_rows.size() - 1;
            row.controls.push_back(node);
        }
        // A button's word: the same press (the script calls the same verb from
        // it), and hidden with its button.
        else if (what == "word")
        {
            m_buttons[node] = m_rows.size() - 1;
            row.controls.push_back(node);
        }
        else
            ETCS_LOG("PaintVisitors", "RowNode: '" << what << "' is not a part of a row "
                     "(bg name role chip up down out word) -- RID:" << node << " is attached to nothing.");
    }

    size_t rowOf(ETCS::RID node) const
    {
        auto it = m_buttons.find(node);
        return (it == m_buttons.end()) ? SIZE_MAX : it->second;
    }

    void BindWindow(ETCS::RID pane) { m_window = pane; }

    /*
 * ── THE WINDOW'S OWN PARTS ───────────────────────────────────────────────
 *
 * The title is the handle (the window follows a press on it, as the layer
 * window does -- PaintInput moves it, see PressTitle). A HOST-ONLY node is
 * shown to the owner of the session and nobody else (the link, the end
 * button); a GUEST-ONLY node the reverse (leave). The "you" line is everyone's:
 * the name this page goes by in the room, pressed to rename it, beside the
 * colour its frame is drawn in, with the swatches to change it.
 */
    void BindTitle(ETCS::RID node)     { if (node) m_title.push_back(node); }
    void BindHostOnly(ETCS::RID node)  { if (node) m_host_only.push_back(node); }
    void BindGuestOnly(ETCS::RID node) { if (node) m_guest_only.push_back(node); }
    void BindMe(ETCS::RID name, ETCS::RID chip) { m_me_label = name; m_me_chip = chip; }
    void BindSwatch(ETCS::RID node, float r, float g, float b)
    {
        if (!node) return;
        m_swatches[node] = Rgb{ r, g, b };
        paint_node_fill(node, r, g, b, 1.0f);
    }

    // Row colours and the two role inks, given by the script for the same
    // reason the panel's are: this is a look, and the script drew it.
    void SetRowColors(float r, float g, float b, float a)
    { m_row[0] = r; m_row[1] = g; m_row[2] = b; m_row[3] = a; }
    void SetInk(float wr, float wg, float wb, float rr, float rg, float rb)
    {
        m_ink_writer[0] = wr; m_ink_writer[1] = wg; m_ink_writer[2] = wb;
        m_ink_reader[0] = rr; m_ink_reader[1] = rg; m_ink_reader[2] = rb;
    }

    /*
 * "name role [colour]" per line, exactly as the node answers `who`. Parsed
 * here rather than by the page because the page would then be deciding what a
 * row says, and the row is this window's.
 *
 * The OWNER's line is kept and marked rather than dropped: a host looking at a
 * list of other people has no way to tell whether the list is short because
 * nobody came or because it does not include them.
 */
    void SetRoster(const std::string& text)
    {
        m_who.clear();
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::istringstream ls(line);
            Person p;
            if (!(ls >> p.name >> p.role)) continue;
            ls >> p.hue;
            m_who.push_back(std::move(p));
        }
        // Owner first, then by name, so a row does not move under the pointer
        // every time somebody's idle clock ticks.
        std::stable_sort(m_who.begin(), m_who.end(),
            [](const Person& a, const Person& b)
            {
                if ((a.role == "owner") != (b.role == "owner")) return a.role == "owner";
                return a.name < b.name;
            });
        Refresh();
    }

    /*
 * OPENED AS WHAT THIS PAGE IS in the session -- owner, writer or reader --
 * because that decides which half of the window it gets. Reopened as the
 * role changes; the window stays where it was put.
 */
    void OpenAs(const std::string& role)
    {
        m_role = role.empty() ? std::string("reader") : role;
        show(true);
        const bool host = (m_role == "owner");
        for (ETCS::RID n : m_host_only)  paint_node_hidden(n, !host);
        for (ETCS::RID n : m_guest_only) paint_node_hidden(n, host);
        Refresh();
    }
    void Open()  { OpenAs("owner"); }

    // The window's end button: for the host it ends the session, for a guest
    // it leaves it. Either way the page is who tells the node.
    void Close() { const bool host = (m_role == "owner"); show(false); page_event(host ? "close" : "leave"); }
    // The page's own close, when the session is already over: no event back.
    void Hide()  { end_edit(false); show(false); }
    bool shown() const { return m_shown; }
    size_t count() const { return m_who.size(); }

    void CopyLink() { page_event("copy"); }

    // What this page is called in the room and the colour it is seen in, from
    // the page -- the page holds both (they persist beside each other).
    void SetMe(const std::string& name, const std::string& hex)
    {
        m_me = name;
        m_me_hex = hex;
        if (!m_editing) paint_node_text(m_me_label, name);
        Rgb c;
        if (paint_hex_rgb(hex, c)) paint_node_fill(m_me_chip, c.r, c.g, c.b, 1.0f);
        Refresh();
    }

    // A swatch pressed: the colour goes to the page, which keeps it and tells
    // the room (the next presence line carries it).
    void PickColor(ETCS::RID node)
    {
        auto it = m_swatches.find(node);
        if (it == m_swatches.end()) return;
        const std::string hex = paint_rgb_hex(it->second);
        paint_node_fill(m_me_chip, it->second.r, it->second.g, it->second.b, 1.0f);
        m_me_hex = hex;
        page_event(("hue:" + hex).c_str());
    }

    /*
 * ── RENAMING YOURSELF ────────────────────────────────────────────────────
 *
 * The layer and page lists' field again: a press on your name opens it with
 * the name in it, every key goes to it until Enter (keep) or Escape (drop),
 * and a press anywhere else keeps what was typed. The node decides whether
 * the name is free -- the page asks it, and says SetMe with whichever name
 * this page ends up with.
 */
    void EditName()
    {
        if (m_editing) return;
        m_editing = true;
        m_edit = m_me;
        paint_node_text(m_me_label, m_edit + "_");
    }

    bool editing() const { return m_editing; }
    void CloseEdit() { end_edit(true); }

    bool KeyIn(uint16_t key)
    {
        if (!m_editing) return false;
        constexpr uint16_t KEY_ESCAPE = 256, KEY_ENTER = 257, KEY_BACKSPACE = 259;
        if (key == KEY_ENTER)  { end_edit(true);  return true; }
        if (key == KEY_ESCAPE) { end_edit(false); return true; }
        if (key == KEY_BACKSPACE) { if (!m_edit.empty()) m_edit.pop_back(); }
        else
        {
            const char ch = paint_key_to_char(key);
            // The node's own rule for a name (no space, no slash, 24 at most),
            // and no comma, which would split it in two on the way back in (SetMe).
            if (ch != 0 && ch != ' ' && ch != '/' && ch != ',' && m_edit.size() < 24) m_edit.push_back(ch);
        }
        paint_node_text(m_me_label, m_edit + "_");
        return true;
    }

    /*
 * ── MOVING THE WINDOW ────────────────────────────────────────────────────
 *
 * PaintLayerPanel's arrangement: a press on the title records the window's
 * origin and the press, and every motion until the release puts the window
 * at the origin plus how far the pointer has gone. Points are in the window's
 * PARENT's space. This window is its own routing root, so the pointer would
 * leave it at the first fast flick -- the router holds the pointer on it for
 * the length of the drag (PaintRouter::Route, `capture`).
 */
    bool PressTitle(ETCS::RID node, Point2D at)
    {
        bool mine = false;
        for (ETCS::RID t : m_title) if (t == node) { mine = true; break; }
        if (!mine) return false;
        ETCS::Held<Drawable2D_> w = ETCS::resolve_held<Drawable2D_>("Drawable2D", m_window);
        if (!w) return true;
        const Rect2D b = w->Bounds();
        m_moving = true;
        m_grab = at;
        m_origin = Point2D{ b.x, b.y };
        return true;
    }
    bool moving() const { return m_moving; }
    void DragWindow(Point2D at)
    {
        if (!m_moving) return;
        if (!paint_node_moved(m_window, m_origin.x + (at.x - m_grab.x), m_origin.y + (at.y - m_grab.y)))
            m_moving = false;
    }
    void EndWindowDrag() { m_moving = false; }

    /*
 * THE THREE ROW VERBS, reached as palette calls with the node pressed. Each one
 * only NAMES what the page should ask the node for -- this window changes
 * nothing by itself, because the roster it is drawing is the node's and the
 * next refresh would overwrite any guess it made.
 */
    void Promote(ETCS::RID node) { act(rowOf(node), "writer"); }
    void Demote(ETCS::RID node)  { act(rowOf(node), "reader"); }
    void Remove(ETCS::RID node)  { act(rowOf(node), "out");    }

private:
    struct Rgb    { float r = 0, g = 0, b = 0; };
    struct Row    { ETCS::RID bg = 0, name = 0, role = 0, chip = 0; std::vector<ETCS::RID> controls; };
    struct Person { std::string name, role, hue; };

    static bool paint_hex_rgb(const std::string& hex, Rgb& out)
    {
        std::string h = hex;
        if (!h.empty() && h[0] == '#') h.erase(0, 1);
        if (h.size() < 6) return false;
        for (size_t i = 0; i < 6; ++i) if (!std::isxdigit(static_cast<unsigned char>(h[i]))) return false;
        out.r = std::stoi(h.substr(0, 2), nullptr, 16) / 255.0f;
        out.g = std::stoi(h.substr(2, 2), nullptr, 16) / 255.0f;
        out.b = std::stoi(h.substr(4, 2), nullptr, 16) / 255.0f;
        return true;
    }
    static std::string paint_rgb_hex(const Rgb& c)
    {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%02x%02x%02x",
                      paint_to_byte(c.r), paint_to_byte(c.g), paint_to_byte(c.b));
        return buf;
    }

    void act(size_t row, const char* verb)
    {
        if (m_role != "owner") return;               // the buttons are the host's
        if (row == SIZE_MAX || row >= m_who.size()) return;
        if (m_who[row].role == "owner") return;      // the host is not theirs to change
        page_event((std::string(verb) + ":" + m_who[row].name).c_str());
    }

    void end_edit(bool keep)
    {
        if (!m_editing) return;
        m_editing = false;
        std::string want = m_edit;
        m_edit.clear();
        paint_node_text(m_me_label, m_me);          // until the page says otherwise
        if (keep && !want.empty() && want != m_me) page_event(("name:" + want).c_str());
    }

    void show(bool up)
    {
        m_shown = up;
        paint_node_hidden(m_window, !up);
    }

    static void set_ink(ETCS::RID rid, const float* c)
    {
        ETCS::Entity* raw = paint_resolve_tag("TextLabel", rid);
        if (!raw) return;
        ETCS::Buffer b;
        b.writeString((std::to_string(c[0]) + ", " + std::to_string(c[1]) + ", "
                       + std::to_string(c[2]) + ", 1.0").c_str());
        raw->call(ETCS::Buffer("TextLabel.SetColor"), b, ETCS::RootSignalContext());
    }

    // Rows beyond the roster are HIDDEN rather than blanked: an empty plate
    // still reads as a person who has not loaded yet.
    void Refresh()
    {
        const bool host = (m_role == "owner");
        for (size_t i = 0; i < m_rows.size(); ++i)
        {
            const Row& row = m_rows[i];
            const bool live = (i < m_who.size());
            paint_node_hidden(row.bg,   !live);
            paint_node_hidden(row.name, !live);
            paint_node_hidden(row.role, !live);
            paint_node_hidden(row.chip, !live);
            const bool controls = live && host && m_who[i].role != "owner";
            for (ETCS::RID n : row.controls) paint_node_hidden(n, !controls);
            if (!live) continue;
            const Person& p = m_who[i];
            // Your own row is lit rather than labelled: a "(you)" after a long
            // name ran into the role beside it.
            const float* plate = (p.name == m_me) ? m_row_me : m_row;
            paint_node_fill(row.bg, plate[0], plate[1], plate[2], 1.0f);
            // Sixteen characters is what fits before the role column.
            paint_node_text(row.name, p.name.size() > 16 ? p.name.substr(0, 15) + "~" : p.name);
            paint_node_text(row.role, p.role);
            set_ink(row.role, (p.role == "writer" || p.role == "owner") ? m_ink_writer : m_ink_reader);
            Rgb c;
            if (paint_hex_rgb(p.hue, c)) paint_node_fill(row.chip, c.r, c.g, c.b, 1.0f);
            else                         paint_node_fill(row.chip, 0.30f, 0.31f, 0.26f, 1.0f);
        }
    }

    /*
 * UP TO THE PAGE, because only the page can reach the node. Same proxy and the
 * same reason as PaintCanvasMenu::page_event: this runs on a Worker with no
 * window, so MAIN_THREAD_EM_ASM rather than EM_ASM.
 */
    void page_event(const char* what)
    {
#if defined(__EMSCRIPTEN__)
        MAIN_THREAD_EM_ASM({
            var what = UTF8ToString($0);
            window.dispatchEvent(new CustomEvent('etcs-visitors', { detail: what }));
        }, what);
#endif
        ETCS_LOG("PaintVisitors", what << " -> the page.");
    }

    std::vector<Row>    m_rows;
    std::unordered_map<ETCS::RID, size_t> m_buttons;   // control -> its row
    std::vector<Person> m_who;
    ETCS::RID m_window = 0;
    bool  m_shown = false;
    std::string m_role = "owner";

    std::vector<ETCS::RID> m_title, m_host_only, m_guest_only;
    ETCS::RID m_me_label = 0, m_me_chip = 0;
    std::unordered_map<ETCS::RID, Rgb> m_swatches;
    std::string m_me, m_me_hex;
    bool        m_editing = false;
    std::string m_edit;

    bool    m_moving = false;
    Point2D m_grab{ 0, 0 }, m_origin{ 0, 0 };

    float m_row[4]        = { 0.14f, 0.15f, 0.11f, 0.96f };
    float m_row_me[3]     = { 0.22f, 0.24f, 0.16f };
    float m_ink_writer[3] = { 0.79f, 0.71f, 0.35f };
    float m_ink_reader[3] = { 0.55f, 0.57f, 0.50f };
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

    // The animation: the tool's drag hands it the region, its window's rows
    // are its, and the outline follows the view (PaintAnimation).
    void BindAnimation(ETCS::RID anim)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintAnimation", anim);
        if (!raw) return;
        m_anim = static_cast<PaintAnimation*>(raw->getTrueType());
    }

    // The page list, for the pane it lives on (the gear menu's): a press on
    // one of its parts is its, the wheel over it scrolls it, and the keys go
    // to it while a name is open. Same seams as the layer panel's.
    void BindPagePanel(ETCS::RID panel)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintPagePanel", panel);
        if (!raw) return;
        m_page_panel = static_cast<PaintPagePanel*>(raw->getTrueType());
    }

    // The bar over an open text box, placed whenever this input repaints the
    // view (PaintTextBar::Follow).
    /*
     * Undo and redo AS THIS PANE DOES THEM -- the document's step, then the
     * view repainted and a carried selection dropped, which is what ctrl+z
     * does (HandleKey) and what a button has to do too. A button wired to the
     * document's own verb stepped the notebook and left the last frame on
     * screen until something else redrew it.
     */
    bool Undo()
    {
        if (!m_document) return false;
        const bool did = m_document->Undo();
        if (did) { m_sel_carry = false; repaint_view(); }
        return did;
    }
    bool Redo()
    {
        if (!m_document) return false;
        const bool did = m_document->Redo();
        if (did) { m_sel_carry = false; repaint_view(); }
        return did;
    }

    void BindTextBar(ETCS::RID bar)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintTextBar", bar);
        if (!raw) return;
        m_text_bar = static_cast<PaintTextBar*>(raw->getTrueType());
    }

    // The sharing window, for the pane it is: its title drags it and the keys
    // go to it while a name is open (PaintVisitors).
    void BindVisitors(ETCS::RID visitors)
    {
        ETCS::Entity* raw = paint_resolve_tag("PaintVisitors", visitors);
        if (!raw) return;
        m_visitors = static_cast<PaintVisitors*>(raw->getTrueType());
    }

    // True while this input is carrying a window by its title, which is what
    // the router holds the pointer on this pane for (PaintRouter::Route).
    bool wantsCapture() const { return m_visitors && m_visitors->moving(); }

    /*
 * A PRESS LANDED ON ANOTHER PANE. Said by the router to every input it did not
 * give the press to, because a name field open here otherwise hears nothing:
 * the press was never this pane's, the field keeps the keyboard, and typing on
 * the canvas afterwards goes into a name nobody is looking at. Keeping what was
 * typed, as a press elsewhere inside the same pane already does.
 */
    void PressedElsewhere()
    {
        if (m_visitors)   m_visitors->CloseEdit();
        if (m_panel)      m_panel->CloseEdit();
        if (m_page_panel) m_page_panel->CloseEdit();
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
        if (ev.action == INPUT_SCROLL)
        {
            // Over the layer window the notch is the window's: it scrolls the
            // rows, one per notch, up toward the top of the stack. Anywhere
            // else on the pane it is the zoom (HandleEvent). The position is
            // the last one routed, translated into the pane's space the same
            // way a picked event's is below.
            if ((m_panel || m_page_panel || m_anim) && m_root != 0)
            {
                const Point2D pane_at = paint_root_origin(m_root);
                const Point2D at{ RoutedCursorX() - pane_at.x, RoutedCursorY() - pane_at.y };
                if (m_panel && m_panel->Contains(at))
                {
                    m_panel->Scroll(ev.y > 0 ? -1 : +1);
                    return;
                }
                if (m_page_panel && m_page_panel->Contains(at))
                {
                    m_page_panel->Scroll(ev.y > 0 ? -1 : +1);
                    return;
                }
                if (m_anim && m_anim->Contains(at))
                {
                    m_anim->Scroll(ev.y > 0 ? -1 : +1);
                    return;
                }
            }
            HandleEvent(ev);
            return;
        }

        if (m_root == 0) { HandleEvent(ev); return; }   // unrouted: the old path

        // A window carried by its title follows the pointer wherever it is --
        // over its own pane or not, which is why this is ahead of the pick --
        // until the release puts it down.
        if (m_visitors && m_visitors->moving())
        {
            if (ev.action == INPUT_MOTION) m_visitors->DragWindow(Point2D{ ev.x, ev.y });
            else if (ev.action == INPUT_UP || ev.action == INPUT_BUTTON_UP) m_visitors->EndWindowDrag();
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

        /*
     * HELD FOR THE WALK, AND ONLY THE WALK. PickAt descends somebody else's
     * tree, so the answer has to stay true for the whole descent rather than
     * for the instant it was given (core/Entity.h) -- and then the hold is
     * let go before anything is done about the answer. What follows acts by
     * RID (the palette, the panel, the wheel all resolve their own nodes), and
     * an action may make an entity: the import prompt's answer spawns a layer,
     * and AddTag from inside a hold is refused by the core outright, since the
     * ordering thread that has to see it may be inside a Delete waiting on
     * this very hold.
     */
        ETCS::RID hit_rid = 0;
        Point2D   hit_local{ 0, 0 };
        {
            ETCS::Held<Drawable2D_> root = ETCS::resolve_held<Drawable2D_>("Drawable2D", m_root);
            if (!root)
            {
                ETCS_LOG("PaintInput", "routing root RID:" << m_root
                         << " is gone or going -- dropping the event.");
                return;
            }
            const Pick2D hit = root->PickAt(pane_pt);
            if (!hit) return;                       // outside the tree entirely
            hit_rid   = hit.node->getRID();
            hit_local = hit.local;
        }

        /*
     * A PRESS ON THE PALETTE IS NOT A STROKE, and saying so HERE rather than
     * in the palette is deliberate: what a press means is a property of where
     * it landed, and this is the only place that knows both.
     */
        const bool is_press   = (ev.action == INPUT_DOWN || ev.action == INPUT_BUTTON_DOWN);
        const bool is_release  = (ev.action == INPUT_UP   || ev.action == INPUT_BUTTON_UP);

        /*
     * A BUTTON ANYWHERE BUT THE PANEL CLOSES THE NAME FIELD.
     *
     * While the field is open it takes EVERY key ahead of the chords and the
     * text boxes both (KeyDown, below) -- which is the point, and which without
     * this leaves Enter and Escape as the only ways out: a press on the canvas,
     * the toolbar, the wheel or another window leaves the field open and
     * invisible from there, and ctrl+z is a z. The panel's own press path ends
     * it for a row body, an eye and a delete; this is everywhere else.
     *
     * HERE, because this is the only place that knows both that a button was
     * pressed and what it landed on -- the same reason the palette's
     * not-a-stroke rule is stated here rather than in the palette.
     *
     * AHEAD OF the palette and the wheel, which return early: a press they
     * consume is still a press somewhere that is not the field.
     *
     * NOT ON MOTION, and that is the one liberty taken with "any mouse event":
     * a pointer that drifts one pixel while somebody types is not a gesture,
     * and closing on it would make the field unusable rather than unsticky.
     *
     * m_on_panel is what keeps the release of an in-panel press from counting:
     * press the label, let go having drifted off the row, and the field opened
     * and shut in one gesture without it.
     */
        if ((is_press || is_release) && m_panel && !m_on_panel
            && m_panel->editing() && !m_panel->owns(hit_rid))
            m_panel->CloseEdit();
        if ((is_press || is_release) && m_page_panel
            && m_page_panel->editing() && !m_page_panel->owns(hit_rid))
            m_page_panel->CloseEdit();
        // The sharing window's title: the press that starts carrying it. In the
        // router's space, which is its parent's (the sheet sits at the origin).
        if (is_press && m_visitors && m_visitors->PressTitle(hit_rid, Point2D{ ev.x, ev.y }))
            return;
        if (is_press && m_visitors && m_visitors->editing()) m_visitors->CloseEdit();

        // THE PAGE LIST, ahead of the palette: its rows are on the same pane as
        // the menu's buttons and a press on one is the list's, the release
        // swallowed with it so the sheet under the menu never sees half a click.
        if ((is_press || is_release) && m_page_panel && m_page_panel->Apply(hit_rid, is_press))
            return;
        if ((is_press || is_release) && m_anim && m_anim->Apply(hit_rid, is_press))
            return;

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
            // The list follows the document, not the other way round: an import,
            // a page switch or a resize changes the stack with no press on a
            // row, so the next event that reaches here re-reads it. A revision
            // compare rather than a refresh per event -- Refresh rewrites six
            // fills and three labels, and a pointer sends events by the hundred.
            if (m_document && m_document->revision() != m_panel_rev)
            {
                m_panel_rev = m_document->revision();
                m_panel->Refresh();
            }
            // A window being dragged by its title takes every motion until the
            // release, ahead of the hover: the pointer is over whatever the
            // window is passing, not choosing a row. The move alone is the
            // change; the compose walk redraws what the window was covering.
            if (ev.action == INPUT_MOTION && m_panel->DragWindow(pane_pt)) return;
            if (is_release && m_panel->EndWindowDrag()) { m_on_panel = false; return; }
            // A row being carried takes the motion ahead of the hover, so the
            // eyes it passes over do not isolate their layers on the way. A
            // motion that says the button is up ends a drag whose release some
            // other pane took.
            if (ev.action == INPUT_MOTION && m_panel->draggingRow()
                && input_button_state(ev, PAINT_BUTTON_LEFT) == 0)
                m_panel->CancelRowDrag();
            if (ev.action == INPUT_MOTION && m_panel->DragRow(pane_pt)) return;
            if (ev.action == INPUT_MOTION) m_panel->Hover(hit_rid);
            if (is_release && m_panel->Drop(pane_pt)) { m_on_panel = false; return; }
            if (is_press && m_panel->Apply(hit_rid, true, pane_pt)) { m_on_panel = true; return; }
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
        local.x = static_cast<int16_t>(hit_local.x);
        local.y = static_cast<int16_t>(hit_local.y);
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
        const bool primary_live = (m_tool && m_tool->active()) || m_text_drag != 0 || m_text_resize != 0 || m_sel_carry;
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

        if (ev.action == INPUT_MOTION && m_text_resize != 0 && m_document)
        {
            // Its corner, following the pointer. The width is what the lines
            // wrap to, so the text reflows as it goes.
            PaintTextBox* b = m_document->FindTextBox(m_text_resize);
            if (!b) { m_text_resize = 0; return; }
            b->w = std::max(8, to_doc_x(ev.x) - b->x);
            b->h = std::max(8, to_doc_y(ev.y) - b->y);
            repaint_view();
            return;
        }

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
         * NOTHING STARTS ON A VIEW-ONLY PAGE (PaintDocument::SetReadOnly). The
         * verbs refuse too, but a stroke is drawn as it goes and committed at
         * the end, so refusing here is what keeps the pointer from leaving ink
         * the room never sees. Pan and zoom were handled above and still work.
         */
            if (m_document && m_document->readOnly())
            {
                ETCS_LOG("PaintInput", "view only -- ask the host for drawing.");
                return;
            }
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
                /*
             * THE OPEN BOX'S CORNER RESIZES IT. Measured in view pixels, like the
             * handle it is drawn as, so it is the same target at any zoom.
             */
                if (hit_box != 0 && hit_box == m_document->selectedTextBox() && m_surface)
                    if (const PaintTextBox* b = m_document->FindTextBox(hit_box))
                    {
                        const int32_t cx = m_surface->DocToViewX(b->x + b->w), cy = m_surface->DocToViewY(b->y + b->h);
                        const int32_t vx = m_surface->DocToViewX(m_cursor_x), vy = m_surface->DocToViewY(m_cursor_y);
                        if (vx >= cx - PaintDocument::TEXT_HANDLE_PX - 2 && vy >= cy - PaintDocument::TEXT_HANDLE_PX - 2)
                        {
                            m_text_resize = hit_box;
                            repaint_view();
                            return;
                        }
                    }
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
         *
         * UNLESS A MODIFIER IS HELD, and that exception is what makes add and
         * subtract usable at all. ctrl and shift say "change this region"
         * (PaintSelectOp), and the region being changed is the one you are
         * standing on -- a subtract starts inside what it takes away from
         * almost by definition. Without this the gesture is swallowed whole:
         * the press picks the region up, the drag carries it, and the release
         * drops it somewhere new, which is the one thing that was not asked
         * for. It is also why a subtract appeared to do nothing rather than to
         * do something wrong -- a carry logs no selection at all.
         */
            /*
         * THE MOVE TOOL IS THE PAN, REACHED BY THE LEFT BUTTON. Nothing new
         * under it: it enters the same m_panning state a right or middle drag
         * does and the same motion handler moves the picture.
         *
         * IT EXISTS FOR TOUCH. Pan has always been the right or middle button
         * (paint_is_pan_button), and a phone has neither -- so on a tablet the
         * picture could not be moved at all. A tool that pans on an ordinary
         * drag is the one shape that works with a single contact point.
         *
         * The raw event position, not m_cursor_*: a pan is measured in VIEW
         * pixels, and the cursor has already been projected into the document.
         * Panning by a document delta would move the picture by more or less
         * than the finger, depending on the zoom.
         */
            if (m_tool && m_tool->kind() == PaintToolKind::Move)
            {
                m_panning    = true;
                m_pan_button = 0;               // the left button has no pan id
                m_pan_hold.Press();
                m_pan_from_x = ev.x;
                m_pan_from_y = ev.y;
                m_last_motion_ms = 0;           // allow an immediate first sample
                return;
            }

            if (m_tool && m_cursor_seen && m_document
             && m_tool->kind() == PaintToolKind::Select
             && !paint_modifiers().ctrl() && !paint_modifiers().shift()
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
                // (commit_anchored); doing both here would spend an entry on a
                // preview that may commit nothing.
                //
                // OPENS A Dab ENTRY rather than taking a snapshot: the points
                // arrive afterwards, through PaintDocument::ApplyBrush, and the
                // release seals it. An entry that never gets a point -- a press
                // that did not move and marked nothing -- is dropped at the seal
                // rather than recorded as an empty stroke.
                if (m_document && !paint_kind_is_anchored(m_tool->kind()) && !paint_kind_is_placed(m_tool->kind()))
                    m_document->RememberOp(PaintOpKind::Dab, m_tool->brush());
                m_tool->BeginStroke(m_cursor_x, m_cursor_y);
                m_last_x = m_cursor_x;
                m_last_y = m_cursor_y;

                const PaintToolKind k = m_tool->kind();
                /*
                 * WHICH KIND OF SELECTION GESTURE THIS IS, decided once, here.
                 * ctrl adds the region to what is already selected, shift takes
                 * it away, neither replaces. The modifiers come from the key
                 * stream because the event carries none (PaintModifierKeys).
                 * Read at the press and held for the whole drag: letting go of
                 * ctrl halfway would otherwise turn an add into a replace and
                 * lose the region it was adding to.
                 */
                if (k == PaintToolKind::Select && m_document)
                {
                    m_sel_op = paint_modifiers().ctrl()  ? PaintSelectOp::Add
                             : paint_modifiers().shift() ? PaintSelectOp::Subtract
                                                         : PaintSelectOp::Replace;
                    m_document->BeginSelectionGesture(m_sel_op);
                }
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
            // Letting go of the move tool ends the pan the press began. Before
            // everything else, because a pan started no stroke and has nothing
            // below here to commit.
            if (m_panning && m_tool && m_tool->kind() == PaintToolKind::Move)
            {
                flush_coalesced_motion();       // apply the last pending delta
                m_panning = false;
                m_pan_hold.Release();
                return;
            }
            // Letting go of a carried box. Checked before the commit below,
            // because a carry never began a stroke and there is nothing to commit.
            if (m_text_drag != 0)
            {
                ETCS_LOG("PaintInput", "text box " << m_text_drag << " moved");
                m_text_drag = 0;
                return;
            }
            if (m_text_resize != 0)
            {
                ETCS_LOG("PaintInput", "text box " << m_text_resize << " resized");
                m_text_resize = 0;
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
                // commit_anchored opens its OWN entry now -- it is the only
                // thing here that knows which shape is about to be drawn, and
                // an entry opened before that is known could only be a
                // snapshot. A glyph commit places a box rather than pixels and
                // still takes the undescribed path, inside place_text_box.
                if (m_tool->active() && paint_kind_is_anchored(k) && paint_kind_commits(k))
                    commit_anchored(k, m_tool->anchorX(), m_tool->anchorY(),
                                    m_cursor_x, m_cursor_y);
                else if (m_tool->active() && k == PaintToolKind::Select)
                    end_selection(m_tool->anchorX(), m_tool->anchorY(),
                                  m_cursor_x, m_cursor_y);
                else if (m_tool->active() && k == PaintToolKind::Animate && m_anim)
                    m_anim->SetRegion(m_tool->anchorX(), m_tool->anchorY(),
                                      m_cursor_x, m_cursor_y);
                m_tool->EndStroke();
                /*
                 * THE PREVIEW LIVES ON THE VIEW, so whatever the drag drew
                 * there has to go whether or not anything was committed -- and
                 * that is now true of every kind, not only the anchored ones.
                 *
                 * It was anchored-only while the live dab and the mark were the
                 * same pixels in the same colour: a freehand trail left behind
                 * looked exactly like the stroke it was previewing, so nothing
                 * ever noticed. Two changes made the dab a DIFFERENT picture
                 * from the mark. A selection clips the mark and not the dab, so
                 * the trail ran on past the region while the document stopped
                 * at its edge. And the eraser's dab is a neutral nib over a mark
                 * that is transparent (paint_stamp_surface), so an erased stroke
                 * stayed on screen as a grey smear.
                 *
                 * Both are one bug -- the view disagreeing with the document --
                 * and the release is the moment that has to end with them
                 * agreeing. Once per stroke, against once per motion sample,
                 * so this is not the path the live dab exists to protect.
                 *
                 * AND THE NOTEBOOK ENTRY IS SEALED HERE, for the same reason
                 * and at the same moment: the release is where the stroke's
                 * content is finally complete, so it is where the record of it
                 * stops being open. The two invariants are the same invariant.
                 */
                (void)k;
                if (m_document) m_document->SealOp();
                repaint_view();
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
        /*
     * A NAME BEING TYPED IN THE LAYER WINDOW TAKES EVERY KEY, ahead of the
     * chords and the text boxes both. That is what "captures input" means: while
     * the field is open ctrl+z is a z, Delete is a character and not a verb, and
     * nothing the keyboard does reaches the picture. It closes on Enter or
     * Escape (PaintLayerPanel::KeyIn), and only the panel knows it is open.
     */
        if (m_panel && m_panel->KeyIn(key)) return true;
        if (m_page_panel && m_page_panel->KeyIn(key)) return true;
        if (m_visitors && m_visitors->KeyIn(key)) return true;
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

        /*
     * ESCAPE LETS GO, ENTER STARTS A LINE. The box is a column now, and a
     * caption of two lines is typed, not made of two boxes. Letting go is also
     * the bar's `ok` and any press off the box.
     */
        if (key == KEY_ESCAPE)
        {
            m_document->SelectTextBox(0);
            repaint_view();
            return true;
        }
        if (key == KEY_ENTER)
        {
            b->text.push_back('\n');
            m_document->TextEdited(sel);
            repaint_view();
            return true;
        }
        if (key == KEY_BACKSPACE)
        {
            if (!b->text.empty()) b->text.pop_back();
            m_document->TextEdited(sel);
            repaint_view();
            return true;
        }
        if (key == KEY_DELETE)
        {
            // The box itself, since there is no caret to delete forward from.
            m_document->RemoveTextBox(sel);
            repaint_view();
            return true;
        }

        const char ch = paint_key_to_char_shifted(key, paint_modifiers().shift());
        if (ch == 0) return true;          // consumed: a modifier or a function key
        b->text.push_back(ch);
        m_document->TextEdited(sel);
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
     *
     * THE WINDOW RESTARTS WHEN THE FLUSH ENDS, not when it began. Stamped at
     * the start only, a flush slower than the interval left every motion queued
     * behind it already "due", so each one paid for a full preview of its own
     * and the backlog never collapsed: an anchored drag whose preview took
     * 400ms against a 100ms window handled all twelve samples of a short drag
     * one by one, and the press after it waited two seconds for its turn.
     * Measured from the end, what queued during a slow flush arrives inside
     * the window and only moves the pending point -- the queue drains at
     * routing speed and the next flush draws where the pointer is now.
     */
    void flush_coalesced_motion()
    {
        if (!m_motion_pending) return;
        m_motion_pending = false;
        struct Restamp { PaintInput& in; ~Restamp() { in.m_last_motion_ms = now_ms(); } } restamp{ *this };

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
        const float step = std::max(1.0f, m_tool->brush().size_px * 0.5f);
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
        // ONE statement for the whole outline. Every DrawRect is a FillRect that
        // marks, and unbatched each mark walks to the root (ontology/
        // ObservableBase.h) -- an outline is hundreds of rects, which made the
        // marking, not the pixels, most of what a preview cost.
        etcs_observed_batch outline(static_cast<ETCS::Entity*>(view));

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
        const int w = std::max(1, static_cast<int>(m_tool->brush().size_px * z));
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
        case PaintToolKind::Animate:
        {
            // The frame being chosen, in the page's highlight rather than the
            // brush's colour: this drag marks nothing.
            const PaintColor hi{ 0.35f, 0.55f, 0.95f, 1.0f };
            preview_line(view, vax, vay, vbx, vay, hi, 2);
            preview_line(view, vbx, vay, vbx, vby, hi, 2);
            preview_line(view, vbx, vby, vax, vby, hi, 2);
            preview_line(view, vax, vby, vax, vay, hi, 2);
            break;
        }
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

        /*
     * THE ENTRY IS OPENED HERE, one line before the mark it describes, because
     * this is the first point at which the shape is known: the press knew only
     * that something anchored had begun. open_shape records the kind and the
     * brush, the two corners go in as the entry's points, and seal_shape closes
     * it -- which is the same open/append/seal shape a freehand stroke has,
     * compressed into one call because a shape's content is complete the
     * instant it is committed.
     */
        auto open_shape = [&](PaintOpKind k)
        {
            m_document->RememberOp(k, brush);
            m_document->NoteOpPoint(ax, ay);
            m_document->NoteOpPoint(bx, by);
        };

        switch (kind)
        {
        case PaintToolKind::Line:
            open_shape(PaintOpKind::Line);
            layer->StrokeLine(ax, ay, bx, by, brush);
            break;
        case PaintToolKind::Rect:
            open_shape(PaintOpKind::Rect);
            layer->DrawRectOutline(ax, ay, bx, by, brush);
            break;
        case PaintToolKind::Ellipse:
            open_shape(PaintOpKind::Ellipse);
            layer->DrawEllipseOutline(ax, ay, bx, by, brush);
            break;
        case PaintToolKind::Shape:
            switch (m_tool->shape())
            {
            case PaintShapeMode::Rect:
                open_shape(PaintOpKind::Rect);
                layer->DrawRectOutline(ax, ay, bx, by, brush);
                break;
            case PaintShapeMode::Ellipse:
                open_shape(PaintOpKind::Ellipse);
                layer->DrawEllipseOutline(ax, ay, bx, by, brush);
                break;
            default:
            {
                std::vector<std::pair<int32_t, int32_t>> v;
                paint_shape_vertices(m_tool->shape(), ax, ay, bx, by, v);
                // The RING rather than the shape mode: a replaying viewer must
                // not have to own a second copy of paint_shape_vertices, and a
                // mode added later would then draw as whatever that viewer's
                // build thought the name meant. Vertices are the wire form for
                // exactly the reason points are a stroke's.
                m_document->RememberOp(PaintOpKind::Poly, brush);
                for (const auto& p : v) m_document->NoteOpPoint(p.first, p.second);
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
        // rather than the layer's -- see PaintTextBox and place_text_box. No
        // descriptor: a box is not a mark on a layer, so there is nothing for
        // ApplyOp to replay and place_text_box takes the snapshot path.
        case PaintToolKind::Glyph:   place_text_box(ax, ay, bx, by); break;
        default: break;
        }
        m_document->SealOp();
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
            // Open, seed, seal: a fill's whole content is the point it started
            // from and the brush it started with, both known here.
            m_document->RememberOp(PaintOpKind::Fill, m_tool->brush(), m_tool->tolerance());
            m_document->NoteOpPoint(x, y);
            // The eraser is a blend, so it means the same thing on every tool
            // that marks: fill lays the transparent pixel rather than the ink.
            // ApplyOp derives the same ink from the entry's own brush, so a
            // replayed erase-fill erases rather than painting black.
            const PaintColor ink = (m_tool->brush().blend == PaintBlendMode::Erase)
                                   ? PaintColor{ 0.0f, 0.0f, 0.0f, 0.0f }
                                   : m_tool->brush().color;
            const size_t n = layer->FloodFill(x, y, ink, m_tool->tolerance());
            m_document->SealOp();
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
        // No snapshot in front of it: a box is recorded as a Text entry when it
        // is let go (PaintDocument::SelectTextBox), which is its undo step.
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
        if (m_tool->mode() == PaintSelectMode::Wand) { m_document->EndSelectionGesture(); return; }

        int32_t lx = ax, rx = ax, ty = ay, by2 = ay;
        for (const PaintStrokePoint& p : m_tool->points())
        {
            lx = std::min(lx, p.x); rx = std::max(rx, p.x);
            ty = std::min(ty, p.y); by2 = std::max(by2, p.y);
        }
        if (rx - lx < 2 && by2 - ty < 2)
        {
            // A BARE click is the deselect; a MODIFIED one is not. Holding
            // ctrl or shift says "change this region", and the change a
            // zero-area drag describes is nothing -- so the region stands.
            // Without this, a misjudged press while adding would wipe
            // everything the user had built up, which is the one outcome an
            // additive mode must not have.
            if (m_sel_op == PaintSelectOp::Replace)
            {
                m_document->ClearSelection();
                ETCS_LOG("PaintInput", "selection cleared");
            }
            m_document->EndSelectionGesture();
            return;
        }
        shape_selection(ax, ay, bx, by);
        const PaintSelection& s = m_document->selection();
        ETCS_LOG("PaintInput", "selected " << s.count << " px ("
                 << paint_select_mode_name(m_tool->mode())
                 << (m_sel_op == PaintSelectOp::Add ? ", added"
                   : m_sel_op == PaintSelectOp::Subtract ? ", subtracted" : "") << ")");
        m_document->EndSelectionGesture();
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
        if (m_anim) m_anim->FollowView();
        if (m_text_bar) m_text_bar->Follow();
    }

    // A preview segment, as a run of small rects rather than brush dabs -- the
    // view surface is somebody else's and DrawRect is the primitive every
    // Surface has. Thickness follows the nib so the preview reads as the same
    // weight the commit will land at.
    static void preview_line(Surface_* view, int32_t x0, int32_t y0,
                             int32_t x1, int32_t y1, const PaintColor& c, int w)
    {
        // A horizontal or vertical run is exactly the union of the squares the
        // walk below would stamp along it, so it is drawn as that one rect.
        // Every edge of a rectangle, a text box and the animation frame is one
        // of these, and the walk spends a rect per half-nib -- per PIXEL at the
        // 2px width the frame uses.
        if (x0 == x1 || y0 == y1)
        {
            view->DrawRect(std::min(x0, x1) - w / 2, std::min(y0, y1) - w / 2,
                           static_cast<uint32_t>(std::abs(x1 - x0) + w),
                           static_cast<uint32_t>(std::abs(y1 - y0) + w),
                           c.r, c.g, c.b, c.a);
            return;
        }
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
    PaintPagePanel*  m_page_panel = nullptr;   // see BindPagePanel
    PaintVisitors*   m_visitors   = nullptr;   // see BindVisitors
    PaintTextBar*    m_text_bar   = nullptr;   // see BindTextBar
    uint32_t         m_text_resize = 0;         // a box whose corner is being dragged
    PaintAnimation*  m_anim = nullptr;         // see BindAnimation
    uint64_t         m_panel_rev = 0;      // the document revision the panel last showed
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
    // What the selection drag in progress means -- read from the modifiers at
    // the press and held until the release (see the press handler).
    PaintSelectOp m_sel_op = PaintSelectOp::Replace;
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
     * THE POINTER HELD ON ONE PANE. While an input is carrying a window by its
     * title (PaintInput::wantsCapture) every event goes to that pane, whether
     * or not the pane contains the point: the window moves one motion behind
     * the pointer, and a flick faster than its own title is tall would
     * otherwise leave it behind, stranded with the button still down. The
     * release is the pane's too, and ends the hold.
     */
        if (m_capture != 0)
        {
            const bool positional_c = (ev.action != INPUT_SCROLL);
            if (positional_c) { m_x = ev.x; m_y = ev.y; }
            PaintInput* held = nullptr;
            for (const Pane& pane : m_panes)
                if (pane.root == m_capture)
                    if (ETCS::Entity* raw = paint_resolve_tag("PaintInput", pane.input))
                        held = static_cast<PaintInput*>(raw->getTrueType());
            if (held)
            {
                if (positional_c) held->NoteRoutedCursor(ev.x, ev.y);
                held->RouteEvent(ev);
            }
            if (!held || ev.action == INPUT_BUTTON_UP || ev.action == INPUT_UP || !held->wantsCapture())
                m_capture = 0;
            return;
        }

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
                bool inside = false;
                {
                    ETCS::Held<Drawable2D_> root = ETCS::resolve_held<Drawable2D_>("Drawable2D", pane.root);
                    inside = root && paint_pane_contains(pane.root, at_x, at_y);
                }
                if (pane.inside && !inside)
                {
                    if (ETCS::Entity* raw = paint_resolve_tag("PaintInput", pane.input))
                        static_cast<PaintInput*>(raw->getTrueType())->RouteLeave();
                }
                pane.inside = inside;
            }
        }

        uint32_t left = m_budget;
        std::vector<size_t> delivered;
        for (const auto& r : ranked)
        {
            if (left == 0) break;
            const Pane& pane = m_panes[r.second];

            // Held for the containment test only, not for the pane's handling
            // of the event -- PaintInput::RouteEvent says why a hold must not
            // outlive the walk it was taken for.
            {
                ETCS::Held<Drawable2D_> root = ETCS::resolve_held<Drawable2D_>("Drawable2D", pane.root);
                if (!root) continue;
                if (!paint_pane_contains(m_panes[r.second].root, at_x, at_y)) continue;
            }

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
            if (input->wantsCapture()) m_capture = pane.root;
            delivered.push_back(r.second);
        }

        // Everyone the press did not reach hears that it happened (PaintInput::
        // PressedElsewhere) -- the way a name field there learns to close.
        if (ev.action == INPUT_BUTTON_DOWN || ev.action == INPUT_DOWN)
            for (size_t i = 0; i < m_panes.size(); ++i)
            {
                if (std::find(delivered.begin(), delivered.end(), i) != delivered.end()) continue;
                if (ETCS::Entity* raw = paint_resolve_tag("PaintInput", m_panes[i].input))
                    static_cast<PaintInput*>(raw->getTrueType())->PressedElsewhere();
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
    ETCS::RID m_capture = 0;   // the pane holding the pointer -- see Route
};

class PaintNode : public DeletableBase<PaintNode>, public FilterBase<PaintNode>
{
public:
    WIRE_TYPE_IDENTITY(PaintNode);

    PaintNode()          = default;
    virtual ~PaintNode() = default;

    enum class Role : uint8_t { None, Reader, Writer, Owner };

    static const char* role_name(Role r)
    {
        switch (r)
        {
        case Role::Reader:  return "reader";
        case Role::Writer:  return "writer";
        case Role::Owner:   return "owner";
        default:            return "out";
        }
    }

    // Route-level filter, the same one ChessNode has and for the same reason:
    // one server can carry several mounts and only the node knows which is its.
    bool AcceptsConcrete(ETCS::Buffer& io) const
    {
        const std::string desc = io.restAsString();
        size_t i = 0;
        while (i < desc.size() && desc[i] == '/') ++i;
        size_t j = desc.find('/', i);
        if (j == std::string::npos) j = desc.size();
        if (desc.compare(i, j - i, m_mount) != 0) { io.reset(); return false; }
        io.writeString(m_mount.c_str());
        return true;
    }

    const std::string& MountPath() const { return m_mount; }
    void SetMount(const std::string& m) { if (!m.empty()) m_mount = m; }

    bool DeleteConcrete()
    {
        const std::string key = getSourceModule().toString() + ":" + getSourceTag().toString();
        return ETCS::DestroyEvent{key.c_str(), this}();
    }

    /*
 * ONE LOCK, NOT ONE PER SESSION. Every listing verb walks every session, the
 * reaper walks every session and every roster, and a push touches one session
 * while `sessions` is reading all of them. Cutting the lock finer would make
 * each of those an unsynchronised cross-domain read for no gain a relay can
 * spend -- there is no per-session work here long enough to be worth
 * overlapping, because this type does no drawing at all.
 */
    std::string Request(const RouteRequest& req)
    {
        std::lock_guard<std::mutex> lock(m_mu);
        return requestLocked(req);
    }

    // The answer is held HERE, as this node's own storage, because a route that
    // answers by reference is promising the bytes outlive the send -- the same
    // promise FileHtmlPage makes about a mounted file. One buffer per node and
    // one answer at a time, which the lock above already guarantees.
    const std::string& lastAnswer() const { return m_answer; }
    void setAnswer(std::string s) { m_answer = std::move(s); }

private:
    struct Member
    {
        std::string token;
        /*
         * WHERE THIS MEMBER IS LOOKING, and the colour they chose to be seen
         * in -- "x y w h rrggbb", opaque to this node exactly as an entry's
         * body is.
         *
         * PRESENCE, NOT HISTORY. It is overwritten in place and never appended
         * to anything: a camera position changes on every pan and is worthless
         * a second later, so putting it in the session's entries would fill the
         * one structure whose value is that everything in it caused something.
         * It costs one string per member and nothing per pan.
         */
        std::string view;
        // A push arriving in parts (the `part` verb), held until the last one.
        std::string pending;
        Role        role = Role::Reader;
        // Last time this member was heard from, so a roster does not fill with
        // names that walked away. Refreshed by every verb they reach.
        std::chrono::steady_clock::time_point seen = std::chrono::steady_clock::now();
    };

    /*
     * WHO HAS THEIR HAND ON A TEXT BOX. First to ask gets it; it is theirs until
     * they let go (release) or stop touching it for kClaimSeconds, which is what
     * keeps a person who walked away from locking a box for everyone.
     */
    struct Claim
    {
        std::string who;
        std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
    };
    static constexpr long kClaimSeconds = 20;

    struct Session
    {
        std::string host;
        std::vector<std::string> lines;   // verbatim, one entry each
        uint64_t seq = 0;
        /*
         * THE RECORD'S OWN HASH: XXH3 of each stored line seeded with the
         * hash before it, so it names the whole sequence up to `seq` and moves
         * with every line. Answered beside the head (`head`, `push`, `read`),
         * and a page keeps the same chain over what it has taken in: equal
         * heads with different chains is a page that missed or misordered a
         * line, and it resyncs itself from zero (index.html, readTheirs). The
         * page cannot tell that from silence any other way -- a stroke it never
         * received looks exactly like a stroke nobody made.
         */
        uint64_t chain = 0;
        std::unordered_map<std::string, Member> roster;   // by name
        std::unordered_map<std::string, Claim>  claims;   // text box key -> holder
        std::chrono::steady_clock::time_point opened = std::chrono::steady_clock::now();
    };

    // Long enough that a tab left on another desktop is not evicted mid-session,
    // short enough that a roster is a list of people rather than a guest book.
    static constexpr long kIdleSeconds = 900;

    /*
     * A TOKEN IS A SECRET AND HAS TO LOOK LIKE ONE. Sixteen hex characters
     * from the platform's random device, not from the clock and not from a
     * counter: a token a viewer can guess is an admission control that admits
     * everyone, and the two obvious cheap sources are both guessable by
     * someone who knows roughly when the session opened.
     */
    static std::string mint()
    {
        static std::mutex mu;
        std::lock_guard<std::mutex> g(mu);
        static std::random_device rd;
        static std::mt19937_64 gen(rd());
        static const char* hex = "0123456789abcdef";
        std::uniform_int_distribution<int> d(0, 15);
        std::string out;
        out.reserve(16);
        for (int i = 0; i < 16; ++i) out += hex[d(gen)];
        return out;
    }

    // A name is a path segment and an author field, so it may hold neither a
    // slash nor a space. Sanitised once, here, rather than checked at each of
    // the places it is about to be interpolated into one or the other.
    static std::string clean(const std::string& s, size_t cap = 24)
    {
        std::string out;
        for (char c : s)
        {
            if (c == '/' || c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
            out += c;
            if (out.size() >= cap) break;
        }
        return out;
    }

    Session* find(const std::string& name)
    {
        auto it = m_sessions.find(name);
        return (it == m_sessions.end()) ? nullptr : &it->second;
    }

    // Whose token this is, within this session. Linear because a roster is
    // people: a session with enough members for this to matter has a bigger
    // problem than the scan.
    Member* member(Session& s, const std::string& self, const std::string& token)
    {
        auto it = s.roster.find(self);
        if (it == s.roster.end()) return nullptr;
        if (it->second.token.empty() || it->second.token != token) return nullptr;
        it->second.seen = std::chrono::steady_clock::now();
        return &it->second;
    }

    void reapLocked()
    {
        const auto now = std::chrono::steady_clock::now();
        for (auto sit = m_sessions.begin(); sit != m_sessions.end(); )
        {
            Session& s = sit->second;
            for (auto mit = s.roster.begin(); mit != s.roster.end(); )
            {
                const long idle = std::chrono::duration_cast<std::chrono::seconds>(
                    now - mit->second.seen).count();
                // The owner is not reaped out of their own session: a host who
                // steps away should come back to their session, not to its
                // absence. It goes when they close it or when it empties.
                if (mit->second.role != Role::Owner && idle >= kIdleSeconds)
                {
                    ETCS_LOG("PaintNode", "'" << mit->first << "' idle " << idle
                             << "s -- dropped from session '" << sit->first << "'.");
                    mit = s.roster.erase(mit);
                    continue;
                }
                ++mit;
            }
            if (s.roster.empty())
            {
                ETCS_LOG("PaintNode", "session '" << sit->first << "' has nobody left -- closing ("
                         << s.lines.size() << " entr(ies) discarded).");
                sit = m_sessions.erase(sit);
                continue;
            }
            ++sit;
        }
    }

    /*
     * THERE IS NO LISTING, AND ITS ABSENCE IS THE FEATURE. A verb that
     * enumerated sessions would publish every id on this node, and an id is
     * the right to view -- so the listing chess has, which exists there
     * because being found is the point, is exactly the wrong verb here. If an
     * operator needs to know what a node is carrying, that is a log line on
     * the machine, not a route anyone can call.
     */

    // "name role colour" per member; the colour is the last field of their
    // presence line, "-" until they have sent one.
    std::string whoLocked(const Session& s) const
    {
        std::string out;
        for (const auto& [name, m] : s.roster)
        {
            out += name;
            out += " ";
            out += role_name(m.role);
            // A view is "x y w h hue [head chain picture]" -- the hue is the
            // fifth field, whatever a page appends after it (index.html,
            // pushMyView), not the last.
            std::istringstream v(m.view);
            std::string x, y, w, h, hue;
            v >> x >> y >> w >> h >> hue;
            out += " ";
            out += hue.empty() ? std::string("-") : hue;
            out += "\n";
        }
        return out;
    }

    /*
     * THE PUSH. Every line is renumbered and re-attributed before it is
     * stored, and that is the whole of what a node does to a payload it
     * otherwise does not read.
     *
     * A line is "<seq> <kind> <author> <rest...>": the first two fields are
     * replaced, the third is replaced, the rest is copied. Anything that does
     * not have three fields is refused rather than stored, because a stored
     * line that nobody can parse is an entry every viewer will skip forever.
     */
    size_t pushLocked(Session& s, const std::string& author, const std::string& body)
    {
        size_t taken = 0, bad = 0, held = 0;
        std::istringstream in(body);
        std::string line;
        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;

            std::istringstream ls(line);
            std::string was_seq, kind, was_author;
            if (!(ls >> was_seq >> kind >> was_author)) { ++bad; continue; }
            std::string rest;
            std::getline(ls, rest);            // everything after the author, space included

            // A TEXT BOX HELD BY SOMEBODY ELSE is not theirs to submit: the one
            // line the node reads past the author, and only for this.
            if ((kind == "text" || kind == "untext") && !claimFree(s, key_of(rest), author))
            {
                ++held;
                continue;
            }

            std::string stored = std::to_string(++s.seq);
            stored += " " + kind;
            stored += " " + (author.empty() ? std::string("-") : author);
            stored += rest;
            s.chain = XXH3_64bits_withSeed(stored.data(), stored.size(), s.chain);
            s.lines.push_back(std::move(stored));
            ++taken;
        }
        if (bad)
            ETCS_LOG("PaintNode", "push from '" << author << "': " << bad
                     << " malformed line(s) refused.");
        if (held)
            ETCS_LOG("PaintNode", "push from '" << author << "': " << held
                     << " text box(es) held by somebody else -- refused.");
        return taken;
    }

    // The key of a text line, from what follows its author: "<layer> <order> <key> ...".
    static std::string key_of(const std::string& rest)
    {
        std::istringstream r(rest);
        std::string layer, order, key;
        r >> layer >> order >> key;
        return key;
    }

    // Free for `who`: nobody holds it, they do, or the holder's hand went idle.
    bool claimFree(Session& s, const std::string& key, const std::string& who) const
    {
        auto it = s.claims.find(key);
        if (it == s.claims.end() || it->second.who == who) return true;
        const long idle = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - it->second.last).count();
        return idle >= kClaimSeconds || !s.roster.count(it->second.who);
    }

    static std::string hex64(uint64_t v)
    {
        char b[17];
        std::snprintf(b, sizeof(b), "%016llx", static_cast<unsigned long long>(v));
        return b;
    }
    std::string headLocked(const Session& s) const { return std::to_string(s.seq) + " " + hex64(s.chain); }

    std::string readLocked(const Session& s, uint64_t since) const
    {
        std::string out;
        // The lines are in sequence order and their sequence is their position
        // plus one, so the slice is arithmetic rather than a scan.
        const size_t first = (since >= s.seq) ? s.lines.size() : static_cast<size_t>(since);
        for (size_t i = first; i < s.lines.size(); ++i)
        {
            out += s.lines[i];
            out += "\n";
        }
        // LAST, AND NOT AN ENTRY: where the record stands and what it hashes
        // to, so the page can check what it now holds against what the node
        // holds in the same answer that brought the lines. '=' cannot begin an
        // entry (a sequence number does), so nothing reads it as one.
        out += "= " + headLocked(s) + "\n";
        return out;
    }

    std::string requestLocked(const RouteRequest& req)
    {
        if (req.at(0) != m_mount) return "NOT FOUND";
        reapLocked();

        const std::string self = clean(req.at(1));
        if (self.empty()) return "NOT FOUND";

        const std::string second = req.at(2);
        if (second.empty()) return "NOT FOUND";

        /*
     * ── the two verbs reachable without a token ─────────────────────
     *
     * `open` mints the id as well as the token, so a host cannot pick a
     * name and therefore nobody can guess one. `join` takes the id from
     * the link and hands back a reader's token, with nothing in between:
     * holding the link IS the right to view, and there is no pending
     * state because there is nothing to be pending on.
     */
        if (second == "open")
        {
            Session fresh;
            fresh.host = self;
            Member owner;
            owner.token = mint();
            owner.role  = Role::Owner;
            const std::string token = owner.token;
            fresh.roster[self] = std::move(owner);

            std::string id = mint();
            while (m_sessions.count(id)) id = mint();     // astronomically never
            m_sessions[id] = std::move(fresh);
            ETCS_LOG("PaintNode", "session '" << id << "' opened by '" << self << "'.");
            return id + " " + token;
        }

        if (second == "join")
        {
            const std::string id = clean(req.at(3), 40);
            Session* s = find(id);
            // The SAME answer for an id that never existed and one that has
            // been closed: telling the two apart is telling somebody guessing
            // ids which of their guesses was once real.
            if (!s) return "NO SUCH SESSION";

            /*
             * THE TOKEN IS THE IDENTITY, NOT THE NAME. A reload comes back with
             * the token it was given (the page keeps it per tab) and gets its
             * own membership back at whatever role it now has -- which is also
             * how a page LEARNS it has been elevated: the role travels with the
             * answer. A join WITHOUT a token is a new member however familiar
             * the name, and a name already in the roster is suffixed rather
             * than shared.
             *
             * Matching on the name alone handed a second tab the first tab's
             * token: every tab of one browser reads the same stored name, so a
             * host and two readers on one machine were one member to the node,
             * and each reader dropped every one of the host's lines as its own.
             */
            const std::string had = clean(req.at(4), 40);
            if (!had.empty())
                for (auto& [name, m] : s->roster)
                    if (m.token == had)
                    {
                        m.seen = std::chrono::steady_clock::now();
                        return m.token + " " + role_name(m.role) + " " + name;
                    }

            std::string name = self;
            for (unsigned n = 2; s->roster.count(name); ++n)
                name = self + "-" + std::to_string(n);

            Member m;
            m.token = mint();
            m.role  = Role::Reader;          // THE LINK IS WORTH EXACTLY THIS
            const std::string token = m.token;
            s->roster[name] = std::move(m);
            ETCS_LOG("PaintNode", "'" << name << "' joined '" << id << "' as reader"
                     << (name != self ? " (asked for '" + self + "', which was taken)" : "") << ".");
            return token + " reader " + name;
        }

        // ── everything else carries a token ─────────────────────────────
        const std::string token = second;
        const std::string id    = clean(req.at(3), 40);
        const std::string verb  = req.at(4);
        const std::string arg   = req.at(5);

        Session* s = find(id);
        if (!s) return "NO SUCH SESSION";
        Member* me = member(*s, self, token);
        if (!me) return "FORBIDDEN";

        /*
     * WHERE I AM LOOKING, in one call, carrying the colour with it -- which is
     * why it is one call: the rectangle and the colour are a single statement
     * about how this person should appear, and splitting them would let a page
     * be drawn in last week's colour for one poll.
     *
     * ANY MEMBER, INCLUDING A READER. Saying where you are looking is not
     * writing to the picture; a viewer who could not be seen would be the one
     * participant nobody could follow, which is the opposite of the point.
     */
        if (verb == "view")
        {
            std::string payload = req.from(5);
            for (char& c : payload) if (c == '/') c = ' ';
            me->view = payload;
            return "ok";
        }

        // Everyone's, readable by everyone -- unlike `who`, which is the roster
        // with ROLES on it and stays the host's. Presence is public within a
        // session by nature: it exists to be looked at.
        if (verb == "views")
        {
            std::string out;
            for (const auto& [name, m] : s->roster)
            {
                if (m.view.empty() || name == self) continue;   // not my own frame
                out += name;
                out += " ";
                out += m.view;
                out += "\n";
            }
            return out;
        }

        if (verb == "head") return headLocked(*s);

        /*
     * THE ROSTER, TO EVERYONE IN IT. Names, roles and colours are what the
     * people in a room can see of each other anyway -- the frames on the
     * canvas carry the names -- so it is not the host's secret; the buttons
     * that act on it are, and those stay below.
     */
        if (verb == "who") return whoLocked(*s);

        /*
     * A NEW NAME FOR MYSELF, kept with my token and my role. Refused if
     * somebody here already has it: the roster is keyed by name, and two
     * people answering to one would be one of them unable to be promoted.
     * Lines already pushed keep the name they were pushed under.
     */
        if (verb == "rename")
        {
            const std::string want = clean(arg);
            if (want.empty()) return "BAD NAME";
            if (want == self) return want;
            if (s->roster.count(want)) return "TAKEN";
            Member moved = std::move(*me);
            s->roster.erase(self);
            s->roster[want] = std::move(moved);
            if (s->host == self) s->host = want;
            for (auto& [key, c] : s->claims) if (c.who == self) c.who = want;
            ETCS_LOG("PaintNode", "'" << self << "' is now '" << want << "' in '" << id << "'.");
            return want;
        }
        if (verb == "role" && arg.empty())
        {
            // Ask what I am. A viewer polls this to notice an elevation without
            // having to re-join, which is the one thing the roster cannot tell
            // it -- the roster is the owner's to read.
            return role_name(me->role);
        }
        if (verb == "read")
        {
            const uint64_t since = arg.empty() ? 0
                                 : static_cast<uint64_t>(std::strtoull(arg.c_str(), nullptr, 10));
            return readLocked(*s, since);
        }

        /*
     * claim/<key>: my hand on a text box, if nobody else's is (see Claim). A
     * held box answers with its holder, which is what the page shows. Asked
     * again while typing, which is what keeps the claim from lapsing.
     * release/<key>: letting go, after the box went out with a push.
     */
        if (verb == "claim")
        {
            if (me->role != Role::Writer && me->role != Role::Owner) return "READ ONLY";
            const std::string key = clean(arg, 64);
            if (key.empty()) return "BAD KEY";
            if (!claimFree(*s, key, self)) return "held " + s->claims[key].who;
            Claim& c = s->claims[key];
            c.who  = self;
            c.last = std::chrono::steady_clock::now();
            return "ok";
        }
        if (verb == "release")
        {
            const std::string key = clean(arg, 64);
            auto it = s->claims.find(key);
            if (it != s->claims.end() && it->second.who == self) s->claims.erase(it);
            return "ok";
        }

        if (verb == "push")
        {
            // THE ROLE CHECK, and the only place writing is decided. A reader
            // reaching this is not an error to log loudly -- it is a page whose
            // elevation was taken away between its last stroke and this one,
            // which is exactly what revocation is supposed to feel like.
            if (me->role != Role::Writer && me->role != Role::Owner) return "READ ONLY";
            if (!req.posted()) return "POST REQUIRED";
            const size_t n = pushLocked(*s, self, req.bodyString());
            return std::to_string(s->seq) + " " + std::to_string(n) + " " + hex64(s->chain);
        }

        /*
     * A PUSH IN PARTS: part/<i>/<n>, each body appended to the member's
     * pending buffer, the whole taken as one push when the last part lands.
     *
     * Because a request is bounded (ETCS_NETWORK_MAX_HEADER_SIZE, 64 KB for
     * headers and body together) and one entry is not: a keyframe is a layer's
     * PNG, and a page with a photograph on it is megabytes. A line cannot be
     * split by the page into two entries, so the node joins the bytes back
     * before it reads a single line. Part 0 starts over, so a push abandoned
     * half way leaves nothing behind but the next one's first part.
     */
        if (verb == "part")
        {
            if (me->role != Role::Writer && me->role != Role::Owner) return "READ ONLY";
            if (!req.posted()) return "POST REQUIRED";
            const unsigned long i = std::strtoul(arg.c_str(), nullptr, 10);
            const unsigned long n = std::strtoul(req.at(6).c_str(), nullptr, 10);
            if (n == 0 || i >= n) return "BAD PART";
            if (i == 0) me->pending.clear();
            me->pending += req.bodyString();
            if (i + 1 < n) return "more";
            std::string whole;
            whole.swap(me->pending);
            const size_t taken = pushLocked(*s, self, whole);
            return std::to_string(s->seq) + " " + std::to_string(taken) + " " + hex64(s->chain);
        }

        // ── the host's own verbs ────────────────────────────────────────
        if (me->role != Role::Owner) return "FORBIDDEN";
        if (verb == "close")
        {
            // CLOSING IS THE KICK. Every other token in this session stops
            // resolving on the next request, each page keeps the picture it
            // already has, and nothing has to be told anything -- which is the
            // only shape available anyway, since none of them can be called.
            ETCS_LOG("PaintNode", "session '" << id << "' closed by '" << self
                     << "' -- " << (s->roster.size() - 1) << " other(s) dropped.");
            m_sessions.erase(id);
            return "closed";
        }
        if (verb == "role")
        {
            const std::string guest = clean(arg);
            const std::string want  = req.at(6);
            auto it = s->roster.find(guest);
            if (it == s->roster.end()) return "NO SUCH MEMBER";
            if (it->second.role == Role::Owner) return "REFUSED";   // not even by themselves

            if (want == "writer")      it->second.role = Role::Writer;
            else if (want == "reader") it->second.role = Role::Reader;
            else if (want == "out")
            {
                // Dropping the token IS the removal. They keep their canvas and
                // can come back through the link as a reader, which is the
                // right amount of undo for a mis-click.
                ETCS_LOG("PaintNode", "'" << guest << "' removed from '" << id << "'.");
                s->roster.erase(it);
                return "out";
            }
            else return "BAD ROLE";

            ETCS_LOG("PaintNode", "'" << guest << "' is now " << role_name(it->second.role)
                     << " in '" << id << "'.");
            return role_name(it->second.role);
        }

        return "NOT FOUND";
    }

    mutable std::mutex m_mu;
    std::string m_mount = "art";
    std::unordered_map<std::string, Session> m_sessions;
    std::string m_answer;
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

// SetTip <round|stylus|erase> -- which nib is loaded, which is the stamp's
// shape and what it writes at once (PaintTipMode).
DEFINE_WORK_FUNC(PaintTool, SetTip)
{
    (void)ctx;
    self.SetTip(data.restAsString());
}

// SetBlendMode <normal|multiply|screen|erase> -- the blend alone, for the
// combination the tip's stepped list has no room for (a stylus that erases).
// Only Normal and Erase are read today; see PaintLayer::DrawBrush.
DEFINE_WORK_FUNC(PaintTool, SetBlendMode)
{
    (void)ctx;
    self.SetBlendMode(data.restAsString());
}

// SetHardness <0..1>. Held on the brush and not yet read by the stamp -- the
// nib's shape is PaintTipMode's business (paint_stamp_of).
DEFINE_WORK_FUNC_TYPED(PaintTool, SetHardness, (float, hardness))
{
    (void)ctx;
    self.SetHardness(hardness);
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

// MergeDown / MergeUp <layer> -- two verbs, one act, named by direction because
// that is how a person means it; the sign is an implementation detail. No answer
// written back: a TYPED work function's arguments come out of the buffer and it
// has none to write into, and the refusals all log their own reason.
DEFINE_WORK_FUNC_TYPED(PaintDocument, MergeDown, (ETCS::RID, layer))
{
    (void)ctx;
    self.MergeLayer(layer, -1);
}

DEFINE_WORK_FUNC_TYPED(PaintDocument, MergeUp, (ETCS::RID, layer))
{
    (void)ctx;
    self.MergeLayer(layer, 1);
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

// ImportCanvas <path> -- a new page the image's size, with the image on it.
// See PaintDocument::ImportCanvas.
// NewLayer -- an empty, page-sized layer above the active one, made active.
DEFINE_WORK_FUNC(PaintDocument, NewLayer)
{
    (void)ctx; (void)data;
    self.NewLayer();
}

DEFINE_WORK_FUNC(PaintDocument, ImportCanvas)
{
    (void)ctx;
    const std::string path = paint_path_arg(data);
    if (path.empty()) { ETCS_LOG("PaintDocument", "ImportCanvas needs a path."); return; }
    self.ImportCanvas(path);
}

DEFINE_WORK_FUNC(PaintDocument, ExportImage)
{
    (void)ctx;
    const std::string path = paint_path_arg(data);
    if (path.empty()) { ETCS_LOG("PaintDocument", "ExportImage needs a path."); return; }
    self.ExportImage(path);
}

// ExportOps <path> [since] -- the notebook's entries after `since`, as lines.
// The path is first because every other file verb here puts it first; `since`
// is optional and zero means everything, which is what a first push sends.
DEFINE_WORK_FUNC(PaintDocument, ExportOps)
{
    (void)ctx;
    std::istringstream in(data.restAsString());
    std::string path;
    uint64_t since = 0;
    in >> path >> since;
    if (path.empty()) { ETCS_LOG("PaintDocument", "ExportOps needs a path."); data.writeString("0"); return; }
    const size_t n = self.ExportOps(path, since);
    // The COUNT and the HEAD, because the page needs both: one to know whether
    // there was anything to send, the other to know what to ask for next time.
    data.writeString((std::to_string(n) + " " + std::to_string(self.notebookHead())).c_str());
}

// ExportBaseline <path> -- the whole page as a session's starting point
// (PaintDocument::ExportBaseline). Answers the line count.
DEFINE_WORK_FUNC(PaintDocument, ExportBaseline)
{
    (void)ctx;
    std::istringstream in(data.restAsString());
    std::string path;
    in >> path;
    if (path.empty()) { ETCS_LOG("PaintDocument", "ExportBaseline needs a path."); data.writeString("0"); return; }
    data.writeString(std::to_string(self.ExportBaseline(path)).c_str());
}

// TextDenied <key>, <holder> -- the node gave that box to somebody else
// (PaintDocument::TextDenied).
DEFINE_WORK_FUNC_TYPED(PaintDocument, TextDenied, (std::string, key), (std::string, holder))
{
    (void)ctx;
    self.TextDenied(key, holder);
}


// SetTextFont <id> <font> / SetTextSize <id> <px> / SetTextColor <id> <r> <g> <b> <a>
// -- the open box's type, as the text bar sets it (PaintDocument::SetTextFont).
DEFINE_WORK_FUNC_TYPED(PaintDocument, SetTextFont, (uint32_t, id), (uint32_t, font))
{
    (void)ctx;
    self.SetTextFont(id, font);
}

DEFINE_WORK_FUNC_TYPED(PaintDocument, SetTextSize, (uint32_t, id), (uint32_t, size))
{
    (void)ctx;
    self.SetTextSize(id, size);
}

DEFINE_WORK_FUNC_TYPED(PaintDocument, SetTextColor, (uint32_t, id), (float, r), (float, g), (float, b), (float, a))
{
    (void)ctx;
    self.SetTextColor(id, r, g, b, a);
}

// SetReadOnly <0|1> -- view only while a shared session says so.
DEFINE_WORK_FUNC_TYPED(PaintDocument, SetReadOnly, (int32_t, on))
{
    (void)ctx;
    self.SetReadOnly(on != 0);
}

// ImportOps <path> [since] [keep] -- every line of a read, `since` being where
// the read started (zero restarts the record chain, and then this page's own
// lines are applied too, unless `keep`: the host reading its own baseline back). Answers "<taken> <head>
// <chain-hex> <chain-seq>": the page compares the last two with what the node
// said in the same read.
DEFINE_WORK_FUNC(PaintDocument, ImportOps)
{
    (void)ctx;
    std::istringstream in(data.restAsString());
    std::string path, mine;
    uint64_t since = 0;
    in >> path >> since >> mine;
    if (path.empty()) { ETCS_LOG("PaintDocument", "ImportOps needs a path."); data.writeString("0"); return; }
    const size_t n = self.ImportOps(path, since, mine == "keep");
    char chain[17];
    std::snprintf(chain, sizeof(chain), "%016llx", static_cast<unsigned long long>(self.recordChain()));
    data.writeString((std::to_string(n) + " " + std::to_string(self.notebookHead()) + " " + chain
                      + " " + std::to_string(self.recordChainSeq())).c_str());
}

// PictureReport -- PictureHash's parts, one per layer, for chasing a divergence.
DEFINE_WORK_FUNC(PaintDocument, PictureReport)
{
    (void)ctx;
    const std::string r = self.PictureReport();
    ETCS_LOG("PaintDocument", r);
    data.writeString(r.c_str());
}

// PictureHash -- what this page's picture is, as sixteen hex digits. Sent with
// presence so members can tell they have diverged (PaintDocument::PictureHash).
// Answers "<hex> <record-seq>": the picture AND the record position it is the
// picture OF (the last line this page chained, ImportOps), taken together --
// a picture paired with a position read at another moment is what made two
// members at one head disagree about a picture they shared.
// PictureHash [sent] -- and whether the picture is the record's yet (settled:
// nothing of this page's past `sent` is still to be pushed).
DEFINE_WORK_FUNC(PaintDocument, PictureHash)
{
    (void)ctx;
    std::istringstream in(data.restAsString());
    uint64_t sent = 0;
    in >> sent;
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(self.PictureHash()));
    data.writeString((std::string(hex) + " " + std::to_string(self.recordChainSeq())
                      + (self.settled(sent) ? " 1" : " 0")).c_str());
}

// Where this document stands in the record -- the `since` of its next read or
// push. Its own numbering locally; the node's once it is following a session.
DEFINE_WORK_FUNC(PaintDocument, NotebookHead)
{
    (void)ctx;
    data.writeString(std::to_string(self.notebookHead()).c_str());
}

// Who authors entries from now on. Set to the name this page joined a session
// under, so a host's own entries carry the host's name rather than a blank.
DEFINE_WORK_FUNC(PaintDocument, SetAuthor)
{
    (void)ctx;
    self.SetAuthor(data.restAsString());
    data.writeString(self.author().c_str());
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

/*
 * ── the peers' frames ───────────────────────────────────────────────────────
 *
 * ClearPeers then one SetPeer per participant, which is the shape the 256-byte
 * call buffer wants and the reason no file bridge is involved: a peer line is
 * about forty bytes, a roster of eight is over the ceiling, and one call each
 * is under it with room to spare.
 *
 * The rectangle is in DOCUMENT space. Each page draws it through its own
 * projection, so two people at different zooms see each other's frame in the
 * right place on the picture -- view-space pixels would only be correct for
 * whoever sent them.
 */
DEFINE_WORK_FUNC(PaintSurface, ClearPeers)
{
    (void)ctx; (void)data;
    self.ClearPeers();
}

DEFINE_WORK_FUNC_TYPED(PaintSurface, SetPeer,
                       (std::string, name),
                       (int32_t, x), (int32_t, y), (int32_t, w), (int32_t, h),
                       (float, r), (float, g), (float, b))
{
    (void)ctx;
    self.SetPeer(name, x, y, w, h, r, g, b);
}

// This pane's own visible rectangle, in document space -- what the page sends
// to the node so everyone else can draw it. Derived from the projection on
// every call rather than remembered, so it cannot go stale behind a pan.
DEFINE_WORK_FUNC(PaintSurface, ViewRect)
{
    (void)ctx;
    int32_t x = 0, y = 0, w = 0, h = 0;
    self.ViewRect(x, y, w, h);
    data.writeString((std::to_string(x) + " " + std::to_string(y) + " "
                    + std::to_string(w) + " " + std::to_string(h)).c_str());
}

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
// release in sight. 600 by default; see PaintPalette::AdvanceConcrete.
DEFINE_WORK_FUNC_TYPED(PaintPalette, SetHoldCapacity, (int32_t, n))
{
    (void)ctx;
    self.SetHoldCapacity(static_cast<uint16_t>(n < 1 ? 1 : n));
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

// Stash -- save the present if it changed, then leave its slot (PaintPages::Stash).
DEFINE_WORK_FUNC(PaintPages, Stash)
{
    (void)ctx;
    data.writeString(self.Stash() ? "ok" : "failed");
}

DEFINE_WORK_FUNC_TYPED(PaintPages, Load, (int64_t, id))
{
    (void)ctx;
    self.Load(id);
}

// Rename <id> <rest of line> -- the page's name in the store (and on the
// document when it is the page on screen).
DEFINE_WORK_FUNC_TYPED(PaintPages, Rename, (int64_t, id), (std::string, name))
{
    (void)ctx;
    self.Rename(id, name);
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

// AddArrow <arrow node> <the cell it belongs to> -- wheel or mode, decided by
// what the cell already is. What paint_cell_arrow.etcs calls, since a script
// cannot be handed the word "wheel" (run carries RIDs only).
DEFINE_WORK_FUNC_TYPED(PaintPalette, AddArrow, (ETCS::RID, node), (ETCS::RID, slot))
{
    (void)ctx;
    self.AddArrow(node, slot);
}

// SetShapeReadout <label> -- the shape slice's current outline, by name.
DEFINE_WORK_FUNC_TYPED(PaintPalette, SetShapeReadout, (ETCS::RID, label))
{
    (void)ctx;
    self.SetShapeReadout(label);
}

// SetTipReadout <label> -- the brush slice's caption, which nib is loaded.
DEFINE_WORK_FUNC_TYPED(PaintPalette, SetTipReadout, (ETCS::RID, label))
{
    (void)ctx;
    self.SetTipReadout(label);
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

// OpenPopup <pane_rid> <input_rid> / ClosePopup -- the popup nothing was
// pressed for. See PaintPalette::OpenPopup.
DEFINE_WORK_FUNC_TYPED(PaintPalette, OpenPopup, (ETCS::RID, pane), (ETCS::RID, input))
{
    (void)ctx;
    self.OpenPopup(pane, input);
}

DEFINE_WORK_FUNC(PaintPalette, ClosePopup)
{
    (void)ctx; (void)data;
    self.ClosePopup();
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

// ── PaintAnimation ─────────────────────────────────────────────────────────
//
// A region of the page and a reel of frames its size; see PaintAnimation. The
// window's buttons are palette calls to these verbs (paint_anim.etcs).
DEFINE_WORK_FUNC_TYPED(PaintAnimation, Create, (ETCS::RID, document))
{
    (void)ctx;
    self.Create(document);
}

DEFINE_WORK_FUNC_TYPED(PaintAnimation, BindSurface, (ETCS::RID, surface))
{
    (void)ctx;
    self.BindSurface(surface);
}

DEFINE_WORK_FUNC_TYPED(PaintAnimation, BindWindow, (ETCS::RID, pane))
{
    (void)ctx;
    self.BindWindow(pane);
}

DEFINE_WORK_FUNC_TYPED(PaintAnimation, BindPreview, (ETCS::RID, node))
{
    (void)ctx;
    self.BindPreview(node);
}

DEFINE_WORK_FUNC_TYPED(PaintAnimation, BindReadout, (ETCS::RID, node))
{
    (void)ctx;
    self.BindReadout(node);
}

// BindOutline <node> -- four times, top, bottom, left, right: the bars that
// mark the region on the view.
DEFINE_WORK_FUNC_TYPED(PaintAnimation, BindOutline, (ETCS::RID, node))
{
    (void)ctx;
    self.BindOutline(node);
}

DEFINE_WORK_FUNC_TYPED(PaintAnimation, SetRowColors,
    (float, sr), (float, sg), (float, sb), (float, ur), (float, ug), (float, ub))
{
    (void)ctx;
    self.SetRowColors(sr, sg, sb, ur, ug, ub);
}

DEFINE_WORK_FUNC(PaintAnimation, BeginRow)
{
    (void)ctx; (void)data;
    self.BeginRow();
}

DEFINE_WORK_FUNC_TYPED(PaintAnimation, RowNode, (std::string, what), (ETCS::RID, node))
{
    (void)ctx;
    self.RowNode(what, node);
}

// SetRegion x0 y0 x1 y1 -- the frame, in document pixels; what the tool's
// drag calls, and a script's way of saying the same.
DEFINE_WORK_FUNC_TYPED(PaintAnimation, SetRegion, (int32_t, x0), (int32_t, y0), (int32_t, x1), (int32_t, y1))
{
    (void)ctx;
    self.SetRegion(x0, y0, x1, y1);
}

DEFINE_WORK_FUNC(PaintAnimation, Snap)   { (void)ctx; (void)data; self.Snap(); }
DEFINE_WORK_FUNC(PaintAnimation, Put)    { (void)ctx; (void)data; self.Put(); }
DEFINE_WORK_FUNC(PaintAnimation, Next)   { (void)ctx; (void)data; self.Next(); }
DEFINE_WORK_FUNC(PaintAnimation, Prev)   { (void)ctx; (void)data; self.Prev(); }
DEFINE_WORK_FUNC(PaintAnimation, Play)   { (void)ctx; (void)data; self.Play(); }
DEFINE_WORK_FUNC(PaintAnimation, Pause)  { (void)ctx; (void)data; self.Pause(); }
DEFINE_WORK_FUNC(PaintAnimation, Toggle) { (void)ctx; (void)data; self.Toggle(); }
DEFINE_WORK_FUNC(PaintAnimation, Refresh){ (void)ctx; (void)data; self.Refresh(); }
DEFINE_WORK_FUNC(PaintAnimation, Download){ (void)ctx; (void)data; self.Download(); }
DEFINE_WORK_FUNC(PaintAnimation, Report) { (void)ctx; (void)data; self.Report(); }
DEFINE_WORK_FUNC(PaintAnimation, Delete) { (void)ctx; (void)data; self.DeleteConcrete(); }

// Remove <index> / Select <index> -- one-based, as the rows show them.
DEFINE_WORK_FUNC_TYPED(PaintAnimation, Remove, (int32_t, index))
{
    (void)ctx;
    if (index >= 1) self.Remove(static_cast<size_t>(index - 1));
}

DEFINE_WORK_FUNC_TYPED(PaintAnimation, Select, (int32_t, index))
{
    (void)ctx;
    if (index >= 1) self.Select(static_cast<size_t>(index - 1));
}

DEFINE_WORK_FUNC_TYPED(PaintAnimation, SetFps, (int32_t, fps))
{
    (void)ctx;
    self.SetFps(fps);
}

DEFINE_WORK_FUNC_TYPED(PaintAnimation, StepFps, (int32_t, by))
{
    (void)ctx;
    self.StepFps(by);
}

DEFINE_WORK_FUNC_TYPED(PaintAnimation, Scroll, (int32_t, delta))
{
    (void)ctx;
    self.Scroll(delta);
}

// ImportGif <path> / ExportGif <path> -- the reel in and out as a GIF.
DEFINE_WORK_FUNC(PaintAnimation, ImportGif)
{
    (void)ctx;
    self.ImportGif(data.restAsString());
}

DEFINE_WORK_FUNC(PaintAnimation, ExportGif)
{
    (void)ctx;
    self.ExportGif(data.restAsString());
}

// ── PaintPagePanel ─────────────────────────────────────────────────────────
//
// The store as a list: rows assembled by paint_page_row.etcs, a press on a
// row loads that page, its x deletes it, a second press on the name of the
// page on screen renames it. See PaintPagePanel.
DEFINE_WORK_FUNC(PaintPagePanel, Create)
{
    (void)ctx; (void)data;
    self.Create();
}

DEFINE_WORK_FUNC_TYPED(PaintPagePanel, BindPages, (ETCS::RID, pages))
{
    (void)ctx;
    self.BindPages(pages);
}

DEFINE_WORK_FUNC_TYPED(PaintPagePanel, BindMenu, (ETCS::RID, menu))
{
    (void)ctx;
    self.BindMenu(menu);
}

DEFINE_WORK_FUNC_TYPED(PaintPagePanel, BindWindow, (ETCS::RID, pane))
{
    (void)ctx;
    self.BindWindow(pane);
}

DEFINE_WORK_FUNC_TYPED(PaintPagePanel, SetRowColors,
    (float, sr), (float, sg), (float, sb), (float, ur), (float, ug), (float, ub))
{
    (void)ctx;
    self.SetRowColors(sr, sg, sb, ur, ug, ub);
}

DEFINE_WORK_FUNC(PaintPagePanel, BeginRow)
{
    (void)ctx; (void)data;
    self.BeginRow();
}

DEFINE_WORK_FUNC_TYPED(PaintPagePanel, RowNode, (std::string, what), (ETCS::RID, node))
{
    (void)ctx;
    self.RowNode(what, node);
}

// Scroll <rows> -- negative toward the newest page.
DEFINE_WORK_FUNC_TYPED(PaintPagePanel, Scroll, (int32_t, delta))
{
    (void)ctx;
    self.Scroll(delta);
}

DEFINE_WORK_FUNC(PaintPagePanel, Refresh)
{
    (void)ctx; (void)data;
    self.Refresh();
}

DEFINE_WORK_FUNC(PaintPagePanel, Report)
{
    (void)ctx; (void)data;
    self.Report();
}

DEFINE_WORK_FUNC(PaintPagePanel, Delete)
{
    (void)ctx; (void)data;
    self.DeleteConcrete();
}

// AddRow <bg> <eye> <thumb> <label> <delete> -- top of the window first,
// matching the order a script lays them out in. Any of the five may be 0; the
// thumbnail is a raster the panel paints the layer into (see paint_thumb).
DEFINE_WORK_FUNC(PaintLayerPanel, BeginRow)
{
    (void)ctx; (void)data;
    self.BeginRow();
}

// RowNode <what> <rid> -- attaches one part to the row BeginRow opened. See the
// header note for why a row is assembled rather than declared: a script cannot
// hand another script a node it spawned, so a row that needed all its parts in
// one call could only ever live in one file.
DEFINE_WORK_FUNC_TYPED(PaintLayerPanel, RowNode, (std::string, what), (ETCS::RID, node))
{
    (void)ctx;
    self.RowNode(what, node);
}

DEFINE_WORK_FUNC_TYPED(PaintLayerPanel, BindView, (ETCS::RID, node))
{
    (void)ctx;
    self.BindView(node);
}

// BeginTitle -- until the first BeginRow, an eye registered by RowNode is the
// title bar's view toggle rather than a row's. See RowNode.
DEFINE_WORK_FUNC(PaintLayerPanel, BeginTitle)
{
    (void)ctx; (void)data;
    self.BeginTitle();
}

// BindBody <node> -- hidden with the rows when the window is collapsed.
DEFINE_WORK_FUNC_TYPED(PaintLayerPanel, BindBody, (ETCS::RID, node))
{
    (void)ctx;
    self.BindBody(node);
}

// BindAdd <node> -- pressing it adds a layer above the active one
// (PaintDocument::NewLayer).
// BindSurface <surface> -- what to re-render when the window changes the
// picture: a restack, an eye, a delete (PaintLayerPanel::BindSurface).
DEFINE_WORK_FUNC_TYPED(PaintLayerPanel, BindSurface, (ETCS::RID, surface))
{
    (void)ctx;
    self.BindSurface(surface);
}

DEFINE_WORK_FUNC_TYPED(PaintLayerPanel, BindAdd, (ETCS::RID, node))
{
    (void)ctx;
    self.BindAdd(node);
}

// BindTitle <node> -- a press here drags the window; BindWindow <pane> -- the
// pane it drags (PaintLayerPanel::BindTitle). Several handles may be bound.
DEFINE_WORK_FUNC_TYPED(PaintLayerPanel, BindTitle, (ETCS::RID, handle))
{
    (void)ctx;
    self.BindTitle(handle);
}

DEFINE_WORK_FUNC_TYPED(PaintLayerPanel, BindWindow, (ETCS::RID, pane))
{
    (void)ctx;
    self.BindWindow(pane);
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

DEFINE_WORK_FUNC_TYPED(PaintLayerPanel, SetIrisColors,
    (float, vr), (float, vg), (float, vb), (float, hr), (float, hg), (float, hb))
{
    (void)ctx;
    self.SetIrisColors(vr, vg, vb, hr, hg, hb);
}

DEFINE_WORK_FUNC_TYPED(PaintLayerPanel, SetRowColors,
    (float, sr), (float, sg), (float, sb), (float, ur), (float, ug), (float, ub))
{
    (void)ctx;
    self.SetRowColors(sr, sg, sb, ur, ug, ub);
}

// SetDragColor <r> <g> <b> -- the carried row and its ghost (PaintLayerPanel::DragRow).
DEFINE_WORK_FUNC_TYPED(PaintLayerPanel, SetDragColor, (float, r), (float, g), (float, b))
{
    (void)ctx;
    self.SetDragColor(r, g, b);
}

// BindGhost <pane> / GhostNode <thumb|label> <node> / BindDropMark <node> --
// what a row drag shows (PaintLayerPanel::BindGhost).
DEFINE_WORK_FUNC_TYPED(PaintLayerPanel, BindGhost, (ETCS::RID, pane))
{
    (void)ctx;
    self.BindGhost(pane);
}

DEFINE_WORK_FUNC_TYPED(PaintLayerPanel, GhostNode, (std::string, what), (ETCS::RID, node))
{
    (void)ctx;
    self.GhostNode(what, node);
}

DEFINE_WORK_FUNC_TYPED(PaintLayerPanel, BindDropMark, (ETCS::RID, node))
{
    (void)ctx;
    self.BindDropMark(node);
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

// BindImportPrompt <palette> <pane> <input> <caption> -- the popup that asks
// what an arriving file is for; OfferImport <path> asks it. See
// PaintCanvasMenu::OfferImport.
// BindPages <pages> -- the store `new` adds to and save writes; the list of
// pages is PaintPagePanel's. See PaintCanvasMenu's pages note.
// BindAnchorArrow <index> <label> -- the arrow drawn on that cell, pointing
// away from whichever cell is chosen (PaintCanvasMenu::BindAnchorArrow).
// SetExtent <w> <h> -- both numbers at once, for the presets
// (PaintCanvasMenu::SetExtent).
DEFINE_WORK_FUNC_TYPED(PaintCanvasMenu, SetExtent, (int32_t, w), (int32_t, h))
{
    (void)ctx;
    self.SetExtent(w, h);
}

DEFINE_WORK_FUNC_TYPED(PaintCanvasMenu, BindAnchorArrow,
    (int32_t, index), (ETCS::RID, label))
{
    (void)ctx;
    self.BindAnchorArrow(index, label);
}

DEFINE_WORK_FUNC_TYPED(PaintCanvasMenu, BindPages, (ETCS::RID, pages))
{
    (void)ctx;
    self.BindPages(pages);
}

// BindAnimation <anim> -- where a multi-frame GIF goes on upload.
DEFINE_WORK_FUNC_TYPED(PaintCanvasMenu, BindAnimation, (ETCS::RID, anim))
{
    (void)ctx;
    self.BindAnimation(anim);
}

// BindPagePanel <panel> -- the PaintPagePanel on this pane, re-read after a
// new page or a save.
DEFINE_WORK_FUNC_TYPED(PaintCanvasMenu, BindPagePanel, (ETCS::RID, panel))
{
    (void)ctx;
    self.BindPagePanel(panel);
}

// PageChanged -- the page on screen was swapped by something else (the list);
// the readouts follow it.
DEFINE_WORK_FUNC(PaintCanvasMenu, PageChanged)
{
    (void)ctx; (void)data;
    self.PageChanged();
}

// BindTool <tool> -- what an import leaves selected (PaintCanvasMenu::BindTool).
DEFINE_WORK_FUNC_TYPED(PaintCanvasMenu, BindTool, (ETCS::RID, tool))
{
    (void)ctx;
    self.BindTool(tool);
}

DEFINE_WORK_FUNC_TYPED(PaintCanvasMenu, BindImportPrompt,
    (ETCS::RID, palette), (ETCS::RID, pane), (ETCS::RID, input), (ETCS::RID, caption))
{
    (void)ctx;
    self.BindImportPrompt(palette, pane, input, caption);
}

DEFINE_WORK_FUNC(PaintCanvasMenu, OfferImport)
{
    (void)ctx;
    const std::string path = paint_path_arg(data);
    if (path.empty()) { ETCS_LOG("PaintCanvasMenu", "OfferImport needs a path."); return; }
    self.OfferImport(path);
}

DEFINE_WORK_FUNC(PaintCanvasMenu, ImportAsLayer)
{
    (void)ctx; (void)data;
    self.ImportAsLayer();
}

DEFINE_WORK_FUNC(PaintCanvasMenu, ImportAsCanvas)
{
    (void)ctx; (void)data;
    self.ImportAsCanvas();
}

DEFINE_WORK_FUNC(PaintCanvasMenu, ImportCancel)
{
    (void)ctx; (void)data;
    self.ImportCancel();
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

// BindAnimation <anim> -- the reel the animation tool's drag frames (PaintInput::BindAnimation).
DEFINE_WORK_FUNC_TYPED(PaintInput, BindAnimation, (ETCS::RID, anim))
{
    (void)ctx;
    self.BindAnimation(anim);
}

// BindTextBar <bar> -- the bar over an open text box (PaintInput::BindTextBar).
// Undo / Redo -- the pane's step: the document's, then the view repainted. What
// the undo and redo buttons under the picture call (boot_paint_panels.etcs).
DEFINE_WORK_FUNC(PaintInput, Undo)
{
    (void)ctx; (void)data;
    self.Undo();
}
DEFINE_WORK_FUNC(PaintInput, Redo)
{
    (void)ctx; (void)data;
    self.Redo();
}

DEFINE_WORK_FUNC_TYPED(PaintInput, BindTextBar, (ETCS::RID, bar))
{
    (void)ctx;
    self.BindTextBar(bar);
}

// BindVisitors <visitors> -- the sharing window on this pane (PaintInput::BindVisitors).
DEFINE_WORK_FUNC_TYPED(PaintInput, BindVisitors, (ETCS::RID, visitors))
{
    (void)ctx;
    self.BindVisitors(visitors);
}

// BindPagePanel <panel> -- the page list on this pane (PaintInput::BindPagePanel).
DEFINE_WORK_FUNC_TYPED(PaintInput, BindPagePanel, (ETCS::RID, panel))
{
    (void)ctx;
    self.BindPagePanel(panel);
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
                 << " size " << br.size_px
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

// ── PaintFonts ──────────────────────────────────────────────────────────────

DEFINE_WORK_FUNC(PaintFonts, Create)
{
    (void)ctx; (void)data;
    self.Create();
}

// BindPixel <glyphs> -- font 0, the sheet's own lettering.
DEFINE_WORK_FUNC_TYPED(PaintFonts, BindPixel, (ETCS::RID, glyphs))
{
    (void)ctx;
    self.BindPixel(glyphs);
}

// Load <name> <path> -- a TrueType file, as the next font number.
DEFINE_WORK_FUNC(PaintFonts, Load)
{
    (void)ctx;
    std::istringstream in(data.restAsString());
    std::string name, path;
    in >> name >> path;
    if (!name.empty() && name.back() == ',') name.pop_back();
    const bool ok = self.Load(name, path);
    data.writeString(ok ? std::to_string(self.count() - 1).c_str() : "missing");
}

DEFINE_WORK_FUNC(PaintFonts, Report)
{
    (void)ctx; (void)data;
    self.Report();
}

DEFINE_WORK_FUNC(PaintFonts, Delete)
{
    (void)ctx;
    data.writeString(self.DeleteConcrete() ? "deleted" : "FAILED");
}

// ── PaintTextBar ────────────────────────────────────────────────────────────

DEFINE_WORK_FUNC_TYPED(PaintTextBar, Create, (ETCS::RID, document))
{
    (void)ctx;
    self.Create(document);
}

DEFINE_WORK_FUNC_TYPED(PaintTextBar, BindSurface, (ETCS::RID, surface))
{
    (void)ctx;
    self.BindSurface(surface);
}

DEFINE_WORK_FUNC_TYPED(PaintTextBar, BindWindow, (ETCS::RID, pane))
{
    (void)ctx;
    self.BindWindow(pane);
}

// BindFont <node> <font> -- a button meaning that font.
DEFINE_WORK_FUNC_TYPED(PaintTextBar, BindFont, (ETCS::RID, node), (uint32_t, font))
{
    (void)ctx;
    self.BindFont(node, font);
}

DEFINE_WORK_FUNC_TYPED(PaintTextBar, BindSizeLabel, (ETCS::RID, node))
{
    (void)ctx;
    self.BindSizeLabel(node);
}

DEFINE_WORK_FUNC_TYPED(PaintTextBar, BindColor, (ETCS::RID, node), (float, r), (float, g), (float, b))
{
    (void)ctx;
    self.BindColor(node, r, g, b);
}

DEFINE_WORK_FUNC_TYPED(PaintTextBar, SetFontTints,
                       (float, r0), (float, g0), (float, b0), (float, r1), (float, g1), (float, b1))
{
    (void)ctx;
    self.SetFontTints(r0, g0, b0, r1, g1, b1);
}

DEFINE_WORK_FUNC_TYPED(PaintTextBar, PickFont, (ETCS::RID, node))
{
    (void)ctx;
    self.PickFont(node);
}

DEFINE_WORK_FUNC_TYPED(PaintTextBar, StepSize, (int32_t, by))
{
    (void)ctx;
    self.StepSize(by);
}

DEFINE_WORK_FUNC_TYPED(PaintTextBar, PickColor, (ETCS::RID, node))
{
    (void)ctx;
    self.PickColor(node);
}

DEFINE_WORK_FUNC(PaintTextBar, Done)
{
    (void)ctx; (void)data;
    self.Done();
}

DEFINE_WORK_FUNC(PaintTextBar, Remove)
{
    (void)ctx; (void)data;
    self.Remove();
}

DEFINE_WORK_FUNC(PaintTextBar, Delete)
{
    (void)ctx;
    data.writeString(self.DeleteConcrete() ? "deleted" : "FAILED");
}

// ── PaintVisitors ───────────────────────────────────────────────────────────

DEFINE_WORK_FUNC(PaintVisitors, Create)
{
    (void)ctx; (void)data;
    self.Create();
}

DEFINE_WORK_FUNC(PaintVisitors, BeginRow)
{
    (void)ctx; (void)data;
    self.BeginRow();
}

DEFINE_WORK_FUNC_TYPED(PaintVisitors, RowNode, (std::string, what), (ETCS::RID, node))
{
    (void)ctx;
    self.RowNode(what, node);
}

DEFINE_WORK_FUNC_TYPED(PaintVisitors, BindWindow, (ETCS::RID, pane))
{
    (void)ctx;
    self.BindWindow(pane);
}

DEFINE_WORK_FUNC_TYPED(PaintVisitors, SetRowColors, (float, r), (float, g), (float, b), (float, a))
{
    (void)ctx;
    self.SetRowColors(r, g, b, a);
}

DEFINE_WORK_FUNC_TYPED(PaintVisitors, SetInk,
                       (float, wr), (float, wg), (float, wb),
                       (float, rr), (float, rg), (float, rb))
{
    (void)ctx;
    self.SetInk(wr, wg, wb, rr, rg, rb);
}

// The roster as the node answered it. Small by construction -- "luke reader"
// is twelve bytes -- but this crosses the 256-byte call buffer, so a session
// larger than about twenty is where the page should start sending it the way
// the notebook crosses, as a file.
DEFINE_WORK_FUNC(PaintVisitors, SetRoster)
{
    (void)ctx;
    self.SetRoster(data.restAsString());
    data.writeString(std::to_string(self.count()).c_str());
}

DEFINE_WORK_FUNC(PaintVisitors, Open)
{
    (void)ctx; (void)data;
    self.Open();
}

// OpenAs <owner|writer|reader> -- the window for what this page is in the
// session (PaintVisitors::OpenAs).
DEFINE_WORK_FUNC_TYPED(PaintVisitors, OpenAs, (std::string, role))
{
    (void)ctx;
    self.OpenAs(role);
}

// The session is over and the page knows it: down, with nothing sent back.
DEFINE_WORK_FUNC(PaintVisitors, Hide)
{
    (void)ctx; (void)data;
    self.Hide();
}

DEFINE_WORK_FUNC_TYPED(PaintVisitors, BindTitle, (ETCS::RID, node))
{
    (void)ctx;
    self.BindTitle(node);
}

DEFINE_WORK_FUNC_TYPED(PaintVisitors, BindHostOnly, (ETCS::RID, node))
{
    (void)ctx;
    self.BindHostOnly(node);
}

DEFINE_WORK_FUNC_TYPED(PaintVisitors, BindGuestOnly, (ETCS::RID, node))
{
    (void)ctx;
    self.BindGuestOnly(node);
}

// BindMe <name label> <colour chip> -- the "you" line.
DEFINE_WORK_FUNC_TYPED(PaintVisitors, BindMe, (ETCS::RID, name), (ETCS::RID, chip))
{
    (void)ctx;
    self.BindMe(name, chip);
}

// BindSwatch <node> <r> <g> <b> -- a colour this page can be seen in.
DEFINE_WORK_FUNC_TYPED(PaintVisitors, BindSwatch, (ETCS::RID, node), (float, r), (float, g), (float, b))
{
    (void)ctx;
    self.BindSwatch(node, r, g, b);
}

// SetMe <name> <rrggbb> -- from the page, which holds both.
DEFINE_WORK_FUNC_TYPED(PaintVisitors, SetMe, (std::string, name), (std::string, hex))
{
    (void)ctx;
    self.SetMe(name, hex);
}

DEFINE_WORK_FUNC_TYPED(PaintVisitors, PickColor, (ETCS::RID, node))
{
    (void)ctx;
    self.PickColor(node);
}

DEFINE_WORK_FUNC(PaintVisitors, EditName)
{
    (void)ctx; (void)data;
    self.EditName();
}

DEFINE_WORK_FUNC(PaintVisitors, CopyLink)
{
    (void)ctx; (void)data;
    self.CopyLink();
}

// The end button: the host's ends the session, a guest's leaves it -- the page
// hears which and tells the node. Nothing here knows that, which is the point:
// this raises the event and the page decides what it means.
DEFINE_WORK_FUNC(PaintVisitors, Close)
{
    (void)ctx; (void)data;
    self.Close();
}

DEFINE_WORK_FUNC_TYPED(PaintVisitors, Promote, (ETCS::RID, node))
{
    (void)ctx;
    self.Promote(node);
}

DEFINE_WORK_FUNC_TYPED(PaintVisitors, Demote, (ETCS::RID, node))
{
    (void)ctx;
    self.Demote(node);
}

DEFINE_WORK_FUNC_TYPED(PaintVisitors, Remove, (ETCS::RID, node))
{
    (void)ctx;
    self.Remove(node);
}

DEFINE_WORK_FUNC(PaintVisitors, Delete)
{
    (void)ctx;
    data.writeString(self.DeleteConcrete() ? "deleted" : "FAILED");
}

// ── PaintNode ───────────────────────────────────────────────────────────────

DEFINE_WORK_FUNC(PaintNode, Mount)
{
    (void)ctx;
    const std::string m = data.restAsString();
    if (!m.empty()) self.SetMount(m);
    data.writeString(self.MountPath().c_str());
}

/*
 * THE ROUTE TARGET, and a STRUCTURED one -- registered with AddRequestRoute
 * rather than AddRoute, because this node needs two things a path string cannot
 * carry: the METHOD, to tell a push from a read, and the BODY, to receive one.
 *
 * It answers by REFERENCE (RouteRef), which is the other half of the same
 * change: a read of a session's entries is routinely tens of kilobytes and the
 * work-function buffer is 256 bytes. The bytes it points at are this node's own
 * `m_answer`, held until the next request, which is the lifetime contract
 * DispatchRoute states and the same one a mounted file already lives under.
 */
DEFINE_WORK_FUNC(PaintNode, Request)
{
    (void)ctx;
    uint64_t p = 0;
    data.readRaw(&p, sizeof(p));
    const RouteRequest* req = reinterpret_cast<const RouteRequest*>(
        static_cast<uintptr_t>(p));
    if (!req)
    {
        ETCS_LOG("PaintNode::Request", "no request handed over -- is this route "
                 "registered with AddRoute instead of AddRequestRoute?");
        data.writeString("FAILED");
        return;
    }
    self.setAnswer(self.Request(*req));
    const std::string& body = self.lastAnswer();
    RouteRef::Emit(data, body.data(), body.size(), "text/plain");
}

DEFINE_WORK_FUNC(PaintNode, Filter)
{
    (void)ctx;
    self.Accepts(data);
}

DEFINE_WORK_FUNC(PaintNode, Delete)
{
    (void)ctx;
    data.writeString(self.DeleteConcrete() ? "deleted" : "FAILED");
}

#endif // PAINTPROVIDER_H__
