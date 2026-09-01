#ifndef POLYGONDRAWABLE2D_H__
#define POLYGONDRAWABLE2D_H__

#include "../../core_defs.h"
#include "../../ontology.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// ---------------------------------------------------------------------------
// PolygonDrawable2D — the first concrete leaf of the Drawable lineage.
//
// An arbitrary 2D polygon: give it corner points and it is a shape. A
// triangle, a quad, a star, the screen-sized rectangle at the root of a scene
// -- all the same type, differing only in how many points they were handed.
// The root is not special (ontology/Drawable2D.h says why); it is the polygon
// nobody nested inside anything else.
//
// TWO HALVES OF ONE CONTRACT, and this class exists to make both structural
// rather than conventional:
//
//   DOWNWARD -- drawing a parent draws its children. DrawIntoConcrete fills
//               its own shape and then calls drawChildren(), which is the
//               family's own ordered walk. There is no separate "render the
//               scene" pass and no list of things to remember to draw: a
//               subtree is reachable exactly when its root is drawn.
//
//   UPWARD   -- a child's coordinates mean something only relative to its
//               parent. Points come in in the PARENT's space, Bounds() is
//               stated there, and the position on the destination surface is
//               derived by composing origins up the chain at draw time
//               (parentAbsoluteOrigin below). Nothing stores an absolute
//               position, so moving a node moves its whole subtree with
//               nothing to invalidate and no second copy of the answer.
//
// IT OWNS NO PIXELS. A polygon is not a buffer, it is a region of its
// parent's space plus a rule for which points of that region it occupies. So
// it draws THROUGH the destination surface, using only the three verbs every
// Surface already owes -- Clear, DrawRect, Blit -- which is what lets the
// same scene realise onto a window's swapchain, an offscreen CPU layer, or
// anything a future provider registers under "Surface", with no branch here
// for which one it got.
//
// FILLED BY SPANS. Scanline even-odd fill, one DrawRect per span. That is the
// honest generic implementation given the primitives available, and it is
// deliberately not a fast one: a 400px-tall polygon emits ~400 draws per
// frame. A device-side leaf would tessellate and hand the GPU a triangle fan
// instead, and would still be the same family answering the same calls --
// which is the point of the primitives being where they are.
// ---------------------------------------------------------------------------
class PolygonDrawable2D : public Drawable2DBase<PolygonDrawable2D>,
                          public DeletableBase<PolygonDrawable2D>
{
public:
    WIRE_TYPE_IDENTITY(PolygonDrawable2D);

    // --- Orderable_ (required by Surface, which Drawable refines) ---
    //
    // Declared here and nowhere else: the other five comparisons are derived
    // from it (ontology/OrderableBase.h), and this is also what orders this
    // polygon among its siblings in the parent's typed-children list.
    int32_t m_order = 0;
    bool operator<(const PolygonDrawable2D& o) const { return m_order < o.m_order; }

    // The cross-type ordering, for a parent whose children are of several
    // concrete types -- see Drawable.h's note on why that is a scalar
    // question and the pairwise operator above is not.
    int32_t Order() override { return m_order; }

    PolygonDrawable2D()  = default;
    ~PolygonDrawable2D() = default;

    // ── geometry ─────────────────────────────────────────────────────────
    //
    // Points are in the PARENT's coordinate space, which is the upward half
    // of the contract stated plainly: you say where the corners are ON the
    // thing that contains this one, and the polygon's own space falls out of
    // that as the bounding box of what you gave it. One source of truth --
    // there is no second, normalised copy of the points to drift.
    struct Vertex { int32_t x; int32_t y; };

    bool Create()
    {
        this->addTag("active");
        return true;
    }

    void AddPoint(int32_t x, int32_t y) { m_points.push_back(Vertex{x, y}); markCompositorsDirty(); }
    void ClearPoints()                  { m_points.clear(); m_ops.clear(); markCompositorsDirty(); }
    void SetFill(float r, float g, float b, float a)
    { m_fill = {r, g, b, a}; m_filled = true; markCompositorsDirty(); }
    void SetOrder(int32_t z)            { m_order = z; Reorder(); markCompositorsDirty(); }

    // ── Drawable2D_ dispatch ─────────────────────────────────────────────

    // The bounding box, in the parent's space. Degenerate (zero-sized) until
    // there are at least two distinct points, which is the honest answer for
    // a polygon that has not been given a shape yet.
    Rect2D BoundsConcrete() override
    {
        if (m_points.empty()) return Rect2D{0, 0, 0, 0};
        int32_t minx = m_points[0].x, maxx = m_points[0].x;
        int32_t miny = m_points[0].y, maxy = m_points[0].y;
        for (const Vertex& v : m_points)
        {
            minx = std::min(minx, v.x); maxx = std::max(maxx, v.x);
            miny = std::min(miny, v.y); maxy = std::max(maxy, v.y);
        }
        return Rect2D{ minx, miny,
                       static_cast<uint32_t>(maxx - minx),
                       static_cast<uint32_t>(maxy - miny) };
    }

    // Even-odd point-in-polygon. Local coordinates in, so the inherited
    // ToLocal/ToParent (which translate by Bounds()'s origin) compose with
    // this correctly and a caller never has to know which space it is in.
    //
    // This is the ONLY thing separating a triangle from the screen-sized
    // rectangle at the root of the scene, which is the claim the family
    // makes and this is where it is either true or it is not.
    bool ContainsLocalConcrete(int32_t lx, int32_t ly) override
    {
        if (m_points.size() < 3) return false;
        const Rect2D b = BoundsConcrete();
        const double px = static_cast<double>(lx + b.x) + 0.5;
        const double py = static_cast<double>(ly + b.y) + 0.5;

        bool inside = false;
        const size_t n = m_points.size();
        for (size_t i = 0, j = n - 1; i < n; j = i++)
        {
            const double xi = m_points[i].x, yi = m_points[i].y;
            const double xj = m_points[j].x, yj = m_points[j].y;
            if (((yi > py) != (yj > py))
             && (px < (xj - xi) * (py - yi) / (yj - yi) + xi))
                inside = !inside;
        }
        return inside;
    }

    // ── Surface_ dispatch: drawing INTO this polygon's own space ─────────
    //
    // A polygon has no pixels, so these RETAIN rather than rasterise, and
    // DrawIntoConcrete replays them clipped to the shape. Same retained model
    // VulkanSurface uses, for the same reason: the thread that decides what
    // to draw is not the thread that draws it.

    void ClearConcrete(float r, float g, float b, float a) override
    {
        m_fill   = {r, g, b, a};
        m_filled = true;
        m_ops.clear();          // Clear is a new composition, as everywhere else
        markCompositorsDirty();
    }

    void DrawRectConcrete(int32_t x, int32_t y, uint32_t w, uint32_t h,
                          float r, float g, float b, float a) override
    {
        m_ops.push_back(Op{Op::Kind::Rect, x, y, w, h, r, g, b, a, 0, 1.0f});
        markCompositorsDirty();
    }

    // Placed in this polygon's space and clipped to its BOUNDING BOX, not to
    // its shape -- Surface_::Blit takes one rectangle and there is no way to
    // express a span-clipped blit through it. Said out loud rather than left
    // for someone to discover: a blit into a triangle fills the triangle's
    // box. Clipping it properly needs either a Clippable stage on the
    // destination or a masked blit primitive, and inventing either one
    // silently here would be worse than the limitation.
    void BlitConcrete(Surface_* source, int32_t x, int32_t y,
                      uint32_t w, uint32_t h, float opacity) override
    {
        if (!source) return;
        m_ops.push_back(Op{Op::Kind::Blit, x, y, w, h, 0, 0, 0, 0,
                           source->getRID(), opacity});
        markCompositorsDirty();
    }

    // ── Drawable_ dispatch: realise onto a destination ───────────────────
    //
    // Fill, then the retained local ops, then the children -- which is the
    // downward half of the contract, and the reason a scene needs no render
    // list: drawing the root reaches everything nested under it, in Order().
    void DrawIntoConcrete(Surface_* dst) override
    {
        if (!dst) return;
        const Point2D base = parentAbsoluteOrigin();
        const Rect2D  b    = BoundsConcrete();

        if (m_filled && m_points.size() >= 3)
            fillShape(dst, base, m_fill);

        for (const Op& op : m_ops)
        {
            if (op.kind == Op::Kind::Rect)
            {
                // Local -> parent -> absolute, then clipped to the shape one
                // row at a time so a rect drawn "on" a triangle stays on it.
                clipRowsToShape(dst, base,
                                op.x + b.x, op.y + b.y, op.w, op.h,
                                {op.r, op.g, op.b, op.a});
            }
            else
            {
                // Resolved through the family aggregate, not through any
                // module-local table: the blit source may live in a provider
                // this one has never heard of. Resolved at REPLAY, not at the
                // Blit call, so a source deleted between the two is simply
                // skipped instead of dereferenced.
                Surface_* src = ETCS::resolve_in_family<Surface_>("Surface", op.source);
                if (!src) continue;
                dst->Blit(src,
                          base.x + b.x + op.x, base.y + b.y + op.y,
                          op.w, op.h, op.opacity);
            }
        }

        drawChildren(dst);
    }

    // ── Resizable_ dispatch ──────────────────────────────────────────────
    WindowSize GetSizeConcrete() override
    {
        const Rect2D b = BoundsConcrete();
        return WindowSize{ b.w, b.h };
    }

    // ── Deletable_ dispatch ──────────────────────────────────────────────
    bool DeleteConcrete() override
    {
        std::string conjugate_key = getSourceModule().toString() + ":" + getSourceTag().toString();
        ETCS_LOG("PolygonDrawable2D", "firing self-DestroyEvent for RID:" << getRID());
        return ETCS::DestroyEvent{conjugate_key.c_str(), this}();
    }

private:
    struct Colour { float r, g, b, a; };
    struct Op
    {
        enum class Kind { Rect, Blit } kind;
        int32_t  x, y;
        uint32_t w, h;
        float    r, g, b, a;
        ETCS::RID source;
        float    opacity;
    };

    std::vector<Vertex> m_points;
    std::vector<Op>     m_ops;
    Colour              m_fill{1.0f, 1.0f, 1.0f, 1.0f};
    bool                m_filled = false;

    /*
 * Where this polygon's PARENT sits on the destination, composed from every
 * Drawable2D ancestor's own origin. This is the upward half of the contract
 * doing its work: no node stores an absolute position, so a node moving
 * moves its subtree, and a subtree grafted onto a different parent lands
 * wherever that parent is with nothing rewritten.
 *
 * Stops at the first ancestor that is not a Drawable2D -- a Window, an
 * Instance, whatever else an entity may be nested under. That boundary is
 * exactly "the outermost drawable", which is the node whose coordinates are
 * the destination's own.
 *
 * AND AT THE FIRST ANCESTOR THAT OWNS PIXELS, without adding its origin. A
 * node with a buffer is a COORDINATE ORIGIN: its children state their points
 * in its space, and its buffer IS that space, so the offset between them is
 * zero. Whatever that node is nested inside is its own problem, resolved
 * once, when it is blitted (CompositeDrawable2D). Without this rule a
 * composited subtree would be drawn at its screen position inside a buffer
 * that starts at its own top-left, which is the same picture translated by
 * however deep the tree happened to be.
 *
 * Walked per draw rather than cached. It is O(depth) on a chain that is
 * three or four deep in practice, and a cached transform is a second copy
 * of the answer -- the thing this whole arrangement exists to not have.
 */
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

    /*
 * Tell every pixel-owning ancestor that its composition is stale.
 *
 * This is the upward half of the dirty flag, and it is why a compositor can
 * skip a whole subtree safely: a node that changes is responsible for saying
 * so, and it says so to exactly the nodes that cached it -- the compositors
 * on the path from here to the root. Nothing else is touched, so a change
 * deep in one branch does not invalidate a sibling branch's cache.
 *
 * Reached through the family interface pointer, so this file needs no
 * knowledge of CompositeDrawable2D whatsoever: it marks anything that owns
 * pixels, which is exactly the set of things that could have cached it.
 *
 * Walks past a compositor rather than stopping at it -- a compositor nested
 * in a compositor caches this node too, transitively, and both must be told.
 * That is the difference between this walk and the coordinate one above,
 * which stops at the first: coordinates are relative to the NEAREST origin,
 * staleness propagates to EVERY cache.
 */
    void markCompositorsDirty()
    {
        for (ETCS::Entity* node = getParent(); node; node = node->getParent())
        {
            void* px = node->getInterfacePointer(ETCS::Buffer("Pixels"));
            if (px) static_cast<Pixels_*>(px)->MarkDirty();
        }
    }

    // Even-odd scanline crossings for one row, in PARENT space, sorted.
    void rowSpans(int32_t y, std::vector<int32_t>& xs) const
    {
        xs.clear();
        const size_t n = m_points.size();
        if (n < 3) return;
        const double py = static_cast<double>(y) + 0.5;
        for (size_t i = 0, j = n - 1; i < n; j = i++)
        {
            const double xi = m_points[i].x, yi = m_points[i].y;
            const double xj = m_points[j].x, yj = m_points[j].y;
            if ((yi > py) != (yj > py))
                xs.push_back(static_cast<int32_t>(
                    std::lround((xj - xi) * (py - yi) / (yj - yi) + xi)));
        }
        std::sort(xs.begin(), xs.end());
    }

    void fillShape(Surface_* dst, Point2D base, Colour c)
    {
        const Rect2D b = BoundsConcrete();
        std::vector<int32_t> xs;
        for (int32_t y = b.y; y < b.y + static_cast<int32_t>(b.h); ++y)
        {
            rowSpans(y, xs);
            for (size_t k = 0; k + 1 < xs.size(); k += 2)
            {
                const int32_t x0 = xs[k], x1 = xs[k + 1];
                if (x1 <= x0) continue;
                dst->DrawRect(base.x + x0, base.y + y,
                              static_cast<uint32_t>(x1 - x0), 1,
                              c.r, c.g, c.b, c.a);
            }
        }
    }

    // One retained rect, intersected row by row with the polygon's spans, so
    // what lands on the destination is the part of the rect that is actually
    // inside the shape. Same traversal as fillShape; a rect on a triangle is
    // a triangle-shaped rect.
    void clipRowsToShape(Surface_* dst, Point2D base,
                         int32_t rx, int32_t ry, uint32_t rw, uint32_t rh,
                         Colour c)
    {
        std::vector<int32_t> xs;
        const int32_t y_end = ry + static_cast<int32_t>(rh);
        const int32_t x_end = rx + static_cast<int32_t>(rw);
        for (int32_t y = ry; y < y_end; ++y)
        {
            rowSpans(y, xs);
            for (size_t k = 0; k + 1 < xs.size(); k += 2)
            {
                const int32_t x0 = std::max(xs[k], rx);
                const int32_t x1 = std::min(xs[k + 1], x_end);
                if (x1 <= x0) continue;
                dst->DrawRect(base.x + x0, base.y + y,
                              static_cast<uint32_t>(x1 - x0), 1,
                              c.r, c.g, c.b, c.a);
            }
        }
    }
};

#endif
