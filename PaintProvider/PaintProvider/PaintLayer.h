#ifndef PAINTPROVIDER_PAINTLAYER_H__
#define PAINTPROVIDER_PAINTLAYER_H__

#include "../../../core_defs.h"
#include "../../../ontology.h"
#include "PaintSelection.h"   // in order: everything above this in the module is visible here

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
        if (order == m_order) return;
        if (face_via_document(FaceOrder, order, 0.0f, "")) return;
        m_order = order;
        this->Reorder();
        touch_document();
    }

    void SetName(const std::string& name)
    {
        if (name == m_name) return;
        if (face_via_document(FaceName, 0, 0.0f, name)) return;
        m_name = name;
        touch_document();
    }
    const std::string& name() const { return m_name; }
    int32_t order() const { return m_order; }

    /*
     * THE LAYER'S SHARED NAME: the same number on every member of a session,
     * where a RID is this runtime's alone and an order is a position that a
     * reorder changes. Given by the document the first time this layer goes
     * into a roster (PaintDocument::ensureKeys) and carried on every roster
     * line, so a member matches an arriving stack to its own layers by key
     * before it falls back to order -- which is what makes a reorder land as
     * a reorder there rather than as three layers swapping names over pixels
     * that stayed put. Zero is "not yet named".
     */
    uint64_t key() const { return m_key; }
    void     SetKey(uint64_t key) { m_key = key; }

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

    /*
     * EVERY CHANGE TO A LAYER'S FACE -- order, name, visibility, opacity -- IS
     * PERFORMED BY ITS DOCUMENT as an entry (PaintDocument::FaceChange): the
     * stack with this one field changed, applied through the same reconcile a
     * replay runs, which is what then sets the field here. Undo walks back
     * over it and a session carries it to every member; before this a hidden
     * layer was hidden here and nowhere else. A layer with no document, or a
     * document applying a stack itself (a replay, a restack), sets the field
     * directly -- face_via_document answers false -- and a view-only page
     * refuses the change (answers true, having done nothing). A change to
     * what is already so is nothing.
     */
    enum FaceField { FaceOrder, FaceName, FaceVisible, FaceOpacity };
    bool face_via_document(FaceField field, int32_t i, float f, const std::string& s);

    void SetVisible(bool visible)
    {
        if (visible == m_visible) return;
        if (face_via_document(FaceVisible, visible ? 1 : 0, 0.0f, "")) return;
        m_visible = visible;
        touch_document();
    }
    void ToggleVisible() { SetVisible(!m_visible); }

    void SetOpacity(float opacity)
    {
        opacity = std::clamp(opacity, 0.0f, 1.0f);
        if (opacity == m_opacity) return;
        if (face_via_document(FaceOpacity, 0, opacity, "")) return;
        m_opacity = opacity;
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
    const PaintSelection* clip() const { return m_clip; }

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

    // A rectangle's bytes out, and back in over what is there -- what a Patch
    // entry carries (PaintOpKind::Patch). Clipped to the raster both ways, so
    // a rectangle that hangs off the edge is the part of it that exists.
    bool ReadRect(int32_t x, int32_t y, uint32_t w, uint32_t h, std::vector<uint8_t>& out) const
    {
        const uint8_t* px = this->PixelData();
        if (!px || w == 0 || h == 0) return false;
        const int32_t pw = static_cast<int32_t>(this->PixelWidth()), ph = static_cast<int32_t>(this->PixelHeight());
        out.assign(static_cast<size_t>(w) * h * 4, 0);
        for (int32_t yy = 0; yy < static_cast<int32_t>(h); ++yy)
        {
            const int32_t sy = y + yy;
            if (sy < 0 || sy >= ph) continue;
            for (int32_t xx = 0; xx < static_cast<int32_t>(w); ++xx)
            {
                const int32_t sx = x + xx;
                if (sx < 0 || sx >= pw) continue;
                ::std::memcpy(&out[(static_cast<size_t>(yy) * w + xx) * 4], px + (static_cast<size_t>(sy) * pw + sx) * 4, 4);
            }
        }
        return true;
    }
    bool WriteRect(int32_t x, int32_t y, uint32_t w, uint32_t h, const std::vector<uint8_t>& in)
    {
        uint8_t* px = this->PixelData();
        if (!px || w == 0 || h == 0 || in.size() != static_cast<size_t>(w) * h * 4) return false;
        const int32_t pw = static_cast<int32_t>(this->PixelWidth()), ph = static_cast<int32_t>(this->PixelHeight());
        for (int32_t yy = 0; yy < static_cast<int32_t>(h); ++yy)
        {
            const int32_t dy = y + yy;
            if (dy < 0 || dy >= ph) continue;
            const int32_t x0 = std::max(0, x), x1 = std::min(pw, x + static_cast<int32_t>(w));
            if (x0 >= x1) continue;
            ::std::memcpy(px + (static_cast<size_t>(dy) * pw + x0) * 4,
                          &in[(static_cast<size_t>(yy) * w + (x0 - x)) * 4],
                          static_cast<size_t>(x1 - x0) * 4);
        }
        etcs_mark_observed(this);
        return true;
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
        if (Pixels_* dpx = etcs_direct_pixels(ETCS::resolve_in_family<Pixels_>("Pixels", target)))
        {
            render_composite_scaled(*dpx, *this, ox, oy, dw, dh, alpha);
            return;
        }

        // A device-backed destination -- or one with a Device under it, whose
        // host bytes are not its picture (etcs_direct_pixels) -- has no address
        // to blend into, so it keeps the old approximation: one DrawRect per
        // sample, coarse by necessity.
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
    uint64_t    m_key  = 0;      // see key()
    // View state, not document state -- see SetDim. 1.0 is "not dimmed", which
    // is what every layer is until a row is hovered.
    float m_dim = 1.0f;
    float m_peek = 0.0f;   // see SetPeek
    // NOT OWNED. The document's selection, bound on the way out of
    // activeLayer() -- see BindClip. Null until a document hands one over,
    // which is also what a layer spawned by a script has until it is drawn on.
    const PaintSelection* m_clip = nullptr;
};

#endif // PAINTPROVIDER_PAINTLAYER_H__
