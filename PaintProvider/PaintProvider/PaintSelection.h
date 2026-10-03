#ifndef PAINTPROVIDER_PAINTSELECTION_H__
#define PAINTPROVIDER_PAINTSELECTION_H__

#include "../../../core_defs.h"
#include "../../../ontology.h"
#include "PaintTool.h"   // in order: everything above this in the module is visible here

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

#endif // PAINTPROVIDER_PAINTSELECTION_H__
