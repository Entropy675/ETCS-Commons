#ifndef PAINTPROVIDER_PAINTMODIFIERKEYS_H__
#define PAINTPROVIDER_PAINTMODIFIERKEYS_H__

#include "../../../core_defs.h"
#include "../../../ontology.h"
#include "PaintBrush.h"   // in order: everything above this in the module is visible here

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

#endif // PAINTPROVIDER_PAINTMODIFIERKEYS_H__
