#ifndef PAINTPROVIDER_PAINTBRUSH_H__
#define PAINTPROVIDER_PAINTBRUSH_H__

#include "../../../core_defs.h"
#include "../../../ontology.h"
#include "PaintColor.h"   // in order: everything above this in the module is visible here

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
    /*
     * READ THROUGH THE HOLD, DROP IT, THEN ACT -- the order
     * ETCS_ASSERT_NO_LIFETIME_HOLD asks for. The verb may raise a flag
     * (SetHidden does), and a flag change is ordered and blocks; emitted from
     * inside a hold it could wait on a Delete that is waiting on the hold. So
     * the hold covers only what is read here, the type, and the call goes to
     * the entity found AGAIN by that type and RID: a resolution is the liveness
     * check, which is all the hold was for.
     */
    std::string tag;
    {
        ETCS::Held<Drawable2D_> held = ETCS::resolve_held<Drawable2D_>("Drawable2D", node);
        if (!held) return false;
        ETCS::Entity* e = static_cast<ETCS::Entity*>(held.get());
        if (!e) return false;
        tag = e->getSourceTag().toString();
    }
    const char* which = verb;
    if (surface_verb && tag.find("PolygonDrawable2D") == std::string::npos)
        which = surface_verb;
    ETCS::Buffer action;
    action.write((tag + "." + which).c_str());
    ETCS::Buffer payload;
    payload.write(args.c_str());
    const ETCS::Buffer key(tag.c_str());
    {
        ETCS::Entity* e = ETCS::etcs_resolve_by_key(key, node);
        if (!e) return false;
        try { e->call(action, payload); } catch (...) { return false; }
    }
    // And again for the mark: the call may have been the one that retired it.
    if (ETCS::Entity* e = ETCS::etcs_resolve_by_key(key, node))
        for (ETCS::Entity* n = e; n; n = n->getParent())
            etcs_mark_observed(n);
    return true;
}

// The four the module actually asks for, so a call site reads as the intent and
// not as the mechanism. Hidden is neither drawn nor picked (Drawable2D_::
// PickAt), which is what "this row has no delete" has to mean -- a transparent
// button still takes the press. It is the `hidden` flag, so this is a recorded
// state change, not a paint property (ontology/DrawableBase.h).
static inline bool paint_node_hidden(ETCS::RID node, bool hidden)
{
    return paint_node_verb(node, "SetHidden", hidden ? "1" : "0");
}

/*
 * IS A POINT OVER THIS WINDOW -- and a hidden window is over nothing.
 *
 * For the wheel, which carries a delta instead of a position and so is never
 * PICKED: PaintInput::RouteEvent hands the notch to the first window whose
 * rectangle holds the last routed point. That path consults neither PickAt nor
 * the router's ranking, so nothing about the order can reach it -- the three
 * copies of this test read Bounds() alone, and a hidden pane keeps its bounds.
 * The closed animation window went on taking every notch over the patch of
 * canvas it used to cover.
 *
 * `at` is in the window's PARENT's space, as Bounds() is.
 */
static inline bool paint_window_contains(ETCS::RID window, Point2D at)
{
    {
        ETCS::Held<Drawable_> d = ETCS::resolve_held<Drawable_>("Drawable", window);
        if (!d || d->Hidden()) return false;
    }
    ETCS::Held<Drawable2D_> w = ETCS::resolve_held<Drawable2D_>("Drawable2D", window);
    if (!w) return false;
    const Rect2D b = w->Bounds();
    return at.x >= b.x && at.y >= b.y
        && at.x < b.x + static_cast<int32_t>(b.w) && at.y < b.y + static_cast<int32_t>(b.h);
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

static inline bool paint_node_ordered(ETCS::RID node, int32_t order)
{
    return paint_node_verb(node, "SetOrder", std::to_string(order));
}

// What a node says it is stacked at. Asked rather than remembered, for the
// reason PaintRouter reads Order() fresh per event: a script may restack a pane
// mid-session, and a cached copy would be a second authority on the same fact.
static inline int32_t paint_node_order(ETCS::RID node)
{
    ETCS::Held<Drawable_> h = ETCS::resolve_held<Drawable_>("Drawable", node);
    return h ? h->Order() : 0;
}

// Where a node is and how big, in its parent's space. False when it cannot be
// resolved, so a caller never reads a zeroed rect as a real one.
static inline bool paint_node_bounds(ETCS::RID node, Rect2D& out)
{
    ETCS::Held<Drawable2D_> h = ETCS::resolve_held<Drawable2D_>("Drawable2D", node);
    if (!h) return false;
    out = h->Bounds();
    return true;
}

/*
 * A FLOATING WINDOW STAYS WHERE IT CAN BE REACHED.
 *
 * The windows over the canvas are placed absolutely (SetPosition in the boot
 * script) rather than being layout boxes -- which is what lets them be dragged,
 * and what means nothing puts them back when the pane they float in gets
 * SMALLER. Shrink the window and a panel at x=740 is no longer on the canvas at
 * all: alive, ordered, still consuming the events nobody can aim at it, and
 * unreachable short of a reload.
 *
 * CLAMPED AGAINST ITS OWN PARENT rather than against a stage this has to be
 * told about. The parent IS the pane it floats in, by construction, so there is
 * no second binding to keep in step and no way for the two to disagree.
 *
 * KEEP is how much must stay REACHABLE, not how much must be visible. A window
 * pushed flush to an edge keeps its whole title bar -- the part you drag it
 * back out by. Clamping to zero would leave a window you can see and cannot
 * move, which is the same bug one pixel further on.
 *
 * Moves only when it must, so a window the user put somewhere legal is never
 * quietly tidied.
 */
static inline bool paint_clamp_into_parent(ETCS::RID node, int32_t keep)
{
    ETCS::Held<Drawable2D_> h = ETCS::resolve_held<Drawable2D_>("Drawable2D", node);
    if (!h) return false;
    ETCS::Entity* e = static_cast<ETCS::Entity*>(h.get());
    ETCS::Entity* parent = e ? e->getParent() : nullptr;
    if (!parent) return false;
    void* pd = parent->getInterfacePointer(ETCS::Buffer("Drawable2D"));
    if (!pd) return false;

    const Rect2D in  = static_cast<Drawable2D_*>(pd)->Bounds();
    const Rect2D own = h->Bounds();
    if (in.w == 0 || in.h == 0) return false;   // not laid out yet; nothing to clamp into

    /*
     * WHOLLY INSIDE WHERE IT FITS, and a grabbable strip where it does not.
     * Clamping only to `keep` would leave a window hanging off the edge with
     * most of itself unreachable and look like the bug rather than the fix;
     * clamping only to "fits" has nowhere to put a window bigger than the pane,
     * which a narrow phone viewport makes ordinary rather than exotic.
     */
    int32_t x = own.x, y = own.y;
    const int32_t max_x = (own.w <= in.w) ? static_cast<int32_t>(in.w - own.w)
                                          : static_cast<int32_t>(in.w) - keep;
    const int32_t max_y = (own.h <= in.h) ? static_cast<int32_t>(in.h - own.h)
                                          : static_cast<int32_t>(in.h) - keep;
    if (x > max_x) x = max_x;
    if (y > max_y) y = max_y;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x == own.x && y == own.y) return false;
    paint_node_moved(node, x, y);
    return true;
}

/*
 * A WINDOW FOLDED TO ITS BAR IS ONLY ITS BAR. A compositor is its whole
 * rectangle to a pick (Drawable2D_::PickAt answers the pane itself where no
 * child is), and a pane is routed by its rectangle (PaintRouter::Route) -- so
 * a window that hid its rows and kept its size was still scenery over the
 * rectangle they left: a stroke drawn toward a collapsed layer window stopped
 * at an edge nobody could see, and a press there drew nothing. So the pane
 * itself shrinks to the bar -- which also clips what is under it, drawn and
 * picked -- and grows back to the height it had. `full_h` holds that height
 * while folded; 0 is unfolded.
 */
static inline void paint_window_fold(ETCS::RID pane, bool folded, uint32_t bar_h, uint32_t& full_h)
{
    Rect2D b{ 0, 0, 0, 0 };
    {
        ETCS::Held<Drawable2D_> w = ETCS::resolve_held<Drawable2D_>("Drawable2D", pane);
        if (!w) return;
        b = w->Bounds();
    }
    if (folded && full_h == 0 && bar_h > 0 && bar_h < b.h)
    {
        full_h = b.h;
        paint_node_verb(pane, "ResizeTo", std::to_string(b.w) + ", " + std::to_string(bar_h));
    }
    else if (!folded && full_h != 0)
    {
        paint_node_verb(pane, "ResizeTo", std::to_string(b.w) + ", " + std::to_string(full_h));
        full_h = 0;
    }
}

// The bottom of the lowest of these nodes, in their parent's space: how tall
// a window's bar is, read off the nodes that make it.
static inline uint32_t paint_nodes_bottom(const std::vector<ETCS::RID>& nodes)
{
    int32_t bottom = 0;
    for (ETCS::RID n : nodes)
    {
        ETCS::Held<Drawable2D_> d = ETCS::resolve_held<Drawable2D_>("Drawable2D", n);
        if (!d) continue;
        const Rect2D r = d->Bounds();
        bottom = std::max(bottom, r.y + static_cast<int32_t>(r.h));
    }
    return static_cast<uint32_t>(std::max<int32_t>(bottom, 0));
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

    if (Pixels_* px = etcs_direct_pixels(ETCS::resolve_in_family<Pixels_>("Pixels", target)))
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

#endif // PAINTPROVIDER_PAINTBRUSH_H__
