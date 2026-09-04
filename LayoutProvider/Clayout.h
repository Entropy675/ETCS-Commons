#ifndef LAYOUTPROVIDER_CLAYOUT_H__
#define LAYOUTPROVIDER_CLAYOUT_H__

#include "../../core_defs.h"
#include "../../ontology.h"

/*
 * THE ONLY PLACE clay.h IS INCLUDED, and the only place it CAN be -- nothing
 * else in this module or any other reaches past this file. Clay computes
 * boxes; it does not draw, does not own a window, and has never heard of a
 * Drawable2D. See clay/VENDORED.md.
 *
 * ── THE INCLUDE IS SILENT, AND EVERYTHING BELOW IT IS NOT ─────────────────
 *
 * The list is long because it is the whole list. A build that prints warnings
 * is a build nobody reads warnings from, so the standard to hold is zero, and
 * the only way to have that AND -Wall -Wextra on our own code is to say
 * exactly which diagnostics a file we do not write is exempt from.
 *
 * Every one of these is a fine thing to write in C and a warning only because
 * a C++ compiler is reading it: unused debug locals behind Clay's own
 * CLAY_DEBUG paths, an unused parameter on a callback that keeps a uniform
 * signature, int/uint32 comparisons in loop bounds, compound literals that
 * leave trailing members zeroed. None is actionable here; all of them would be
 * actionable in Clayout's own code, which is why the pop is immediate.
 *
 * If an upgrade adds a new warning, the fix is another line HERE with a note,
 * never -w on the module.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#define CLAY_IMPLEMENTATION
#include "clay/clay.h"
#pragma GCC diagnostic pop

#include <cstdlib>
#include <string>
#include <vector>

/*
 * ── Clayout ──────────────────────────────────────────────────────────────
 *
 * WHERE THE NUMBERS COME FROM.
 *
 * A Drawable2D knows how to draw itself and where it sits relative to its
 * parent. What it could never work out is what those positions and sizes
 * SHOULD BE once the window stops matching the numbers a script typed in --
 * which is why a resize moved the root and left the toolbar hanging off the
 * bottom of it. That gap is layout, and layout is a solved problem we were
 * about to solve again badly.
 *
 * So this holds a tree of boxes, hands it to Clay, and writes the answers back
 * through the family verbs the nodes already have: Drawable2D_::MoveTo and
 * Resizable_::ResizeTo. Nothing about drawing changes, nothing about the
 * ontology changes, and the 2D side gains grow/fixed/percent sizing, padding,
 * gaps, alignment and nesting without any of it being written here.
 *
 * IT NAMES NO RENDERER, WHICH IS WHY IT IS ITS OWN PROVIDER. Everything below
 * reaches its subjects by family name through the ontology -- "Drawable2D",
 * "Resizable" -- so it arranges RenderProvider's compositors, and would
 * arrange a second backend's, and depends on neither. A layout solver that had
 * to be linked against a renderer would be a layout solver that had picked
 * one.
 *
 * A CONTROLLER, NOT A NODE. It is not a Drawable2D and never appears in the
 * tree it arranges. The alternative -- a layout container that is also a node
 * -- forces every arrangement to become a nesting level in the DRAW tree,
 * which is a different tree with different reasons for its shape. Keeping them
 * apart means a script can lay out three siblings that are not siblings, or
 * rearrange without reparenting anything.
 *
 * RESIZABLE ITSELF, which is the whole point: `layout.FollowResize(@window)`
 * and every box it owns tracks the window from then on, with the re-solve
 * happening where the size arrives rather than on a frame tick.
 *
 * Sizes are declared per axis, and the four kinds are Clay's:
 *
 *     fixed <n>     exactly n pixels
 *     grow          share what is left over with the other growers
 *     fit           as small as the contents allow
 *     percent <f>   a fraction of the parent
 */
class Clayout : public DeletableBase<Clayout>, public ResizableBase<Clayout>
{
public:
    WIRE_TYPE_IDENTITY(Clayout);

    Clayout() = default;

    ~Clayout()
    {
        // The arena is ours and Clay holds no other resource, so this is the
        // whole of teardown. Freed rather than leaked because a script can
        // build and drop layouts as freely as any other entity.
        if (m_memory) { std::free(m_memory); m_memory = nullptr; }
    }

    bool DeleteConcrete() override { return true; }

    /*
 * The root box, and the arena Clay solves in.
 *
 * Clay sizes its own memory from a maximum element count, so this is the one
 * number that has to be decided up front. The default ceiling is generous for
 * a UI assembled by hand from scripts and the allocation is one block.
 */
    bool Create(uint32_t w, uint32_t h)
    {
        if (w == 0 || h == 0)
        {
            ETCS_LOG("Clayout", "Create with a zero dimension (" << w << "x" << h << ").");
            return false;
        }
        if (!m_memory)
        {
            const uint32_t need = Clay_MinMemorySize();
            m_memory = std::malloc(need);
            if (!m_memory)
            {
                ETCS_LOG("Clayout", "could not allocate " << need << " bytes for Clay.");
                return false;
            }
            Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(need, m_memory);
            m_context = Clay_Initialize(arena,
                                        Clay_Dimensions{ static_cast<float>(w), static_cast<float>(h) },
                                        Clay_ErrorHandler{ &Clayout::onClayError, this });
        }
        m_size = WindowSize{ w, h };
        this->addTag("active");
        return true;
    }

    // ── declaring the tree ───────────────────────────────────────────────
    //
    // Parent-relative like everything else here: a box names its parent by
    // RID, and the root is the box whose parent is 0. Declared once by a
    // script; re-solved as often as the size changes.

    enum class Sizing : uint8_t { Fit = 0, Grow = 1, Fixed = 2, Percent = 3 };

    void AddBox(ETCS::RID node, ETCS::RID parent,
                Sizing w_kind, float w_value,
                Sizing h_kind, float h_value)
    {
        if (node == 0) return;
        for (Box& b : m_boxes)
            if (b.node == node)   // re-declaring a box replaces it
            { b = Box{ node, parent, w_kind, w_value, h_kind, h_value, b.dir, b.padding, b.gap }; return; }
        m_boxes.push_back(Box{ node, parent, w_kind, w_value, h_kind, h_value, 1, 0, 0 });
    }

    // 0 = left to right, 1 = top to bottom. Only meaningful for a box with
    // children; a leaf's direction is nobody's business.
    void SetDirection(ETCS::RID node, int dir)
    { if (Box* b = find(node)) b->dir = dir ? 1 : 0; }

    void SetPadding(ETCS::RID node, float px)
    { if (Box* b = find(node)) b->padding = px < 0.0f ? 0.0f : px; }

    void SetGap(ETCS::RID node, float px)
    { if (Box* b = find(node)) b->gap = px < 0.0f ? 0.0f : px; }

    /*
 * BECOME THIS SIZE, AND MOVE EVERYTHING ACCORDINGLY.
 *
 * The Resizable_ verb, so a layout can follow a window directly
 * (FollowResize, ontology/Resizable.h) and a resize turns into a re-solve
 * with nothing in between. This is the piece the tree was missing: the root
 * could track the window, and its children could not track the root.
 */
    bool ResizeTo(WindowSize size) override
    {
        if (size.width == 0 || size.height == 0) return false;
        m_size = size;
        return Solve();
    }

    WindowSize GetSizeConcrete() override { return m_size; }

    /*
 * Run one pass and write the answers back.
 *
 * Two walks, and the order matters: Clay is told the whole tree first,
 * because a grow box's size is a function of its siblings, and only then is
 * anything moved. Writing back as we went would resize a node whose own
 * children had not been solved yet.
 *
 * SetPosition before ResizeTo, so a node never exists at the new size in the
 * old place -- a compositor that recomposed between the two would blit a
 * correctly-sized box to the wrong coordinates, which reads as a flicker
 * nobody can reproduce.
 */
    bool Solve()
    {
        if (!m_context || m_boxes.empty()) return false;

        Clay_SetCurrentContext(m_context);
        Clay_SetLayoutDimensions(Clay_Dimensions{ static_cast<float>(m_size.width),
                                                  static_cast<float>(m_size.height) });
        Clay_BeginLayout();
        for (const Box& b : m_boxes)
            if (b.parent == 0) declare(b);
        Clay_EndLayout(0.0f);

        size_t moved = 0;
        for (const Box& b : m_boxes)
        {
            Clay_ElementData data = Clay_GetElementData(idFor(b.node));
            if (!data.found) continue;

            // Clay answers in the ROOT's space; a Drawable2D is stated in its
            // parent's. The difference is the parent's own origin, which the
            // same pass just computed -- so the conversion is a subtraction
            // rather than another walk.
            float ox = 0.0f, oy = 0.0f;
            if (b.parent != 0)
            {
                Clay_ElementData p = Clay_GetElementData(idFor(b.parent));
                if (p.found) { ox = p.boundingBox.x; oy = p.boundingBox.y; }
            }

            ETCS::Held<Drawable2D_> node = ETCS::resolve_held<Drawable2D_>("Drawable2D", b.node);
            if (!node) continue;

            node->MoveTo(Point2D{ static_cast<int32_t>(data.boundingBox.x - ox),
                                  static_cast<int32_t>(data.boundingBox.y - oy) });

            ETCS::Held<Resizable_> sizeable = ETCS::resolve_held<Resizable_>("Resizable", b.node);
            if (sizeable)
                sizeable->ResizeTo(WindowSize{ static_cast<uint32_t>(data.boundingBox.width),
                                               static_cast<uint32_t>(data.boundingBox.height) });
            ++moved;
        }

        ETCS_LOG("Clayout", "solved " << m_size.width << "x" << m_size.height
                 << " over " << m_boxes.size() << " box(es), placed " << moved << ".");
        return moved > 0;
    }

    void Report() const
    {
        ETCS_LOG("Clayout", m_size.width << "x" << m_size.height << ", "
                 << m_boxes.size() << " box(es):");
        for (const Box& b : m_boxes)
            ETCS_LOG("Clayout", "  RID:" << b.node << " parent:" << b.parent
                     << "  w=" << kindName(b.w_kind) << "/" << b.w_value
                     << "  h=" << kindName(b.h_kind) << "/" << b.h_value
                     << "  dir=" << (b.dir ? "column" : "row"));
    }

private:
    struct Box
    {
        ETCS::RID node;
        ETCS::RID parent;
        Sizing    w_kind;  float w_value;
        Sizing    h_kind;  float h_value;
        int       dir;
        float     padding;
        float     gap;
    };

    Box* find(ETCS::RID node)
    {
        for (Box& b : m_boxes) if (b.node == node) return &b;
        return nullptr;
    }

    static const char* kindName(Sizing s)
    {
        switch (s)
        {
            case Sizing::Fit:     return "fit";
            case Sizing::Grow:    return "grow";
            case Sizing::Fixed:   return "fixed";
            case Sizing::Percent: return "percent";
        }
        return "?";
    }

    // The RID IS the id. Clay keys elements on a uint32 hash and never needs
    // the string back, so a truncated RID serves directly -- no name to invent,
    // no table to keep in step with the entities.
    static Clay_ElementId idFor(ETCS::RID rid)
    {
        Clay_ElementId id{};
        id.id = static_cast<uint32_t>(rid ^ (rid >> 32));
        return id;
    }

    static Clay_SizingAxis axis(Sizing kind, float value)
    {
        Clay_SizingAxis a{};
        switch (kind)
        {
            case Sizing::Fixed:
                a.type = CLAY__SIZING_TYPE_FIXED;
                a.size.minMax = Clay_SizingMinMax{ value, value };
                break;
            case Sizing::Percent:
                a.type = CLAY__SIZING_TYPE_PERCENT;
                a.size.percent = value;
                break;
            case Sizing::Fit:
                a.type = CLAY__SIZING_TYPE_FIT;
                break;
            case Sizing::Grow:
            default:
                a.type = CLAY__SIZING_TYPE_GROW;
                break;
        }
        return a;
    }

    /*
 * Built by hand rather than through the CLAY({...}) macro, which is the one
 * part of Clay that needs C++20 (clay/VENDORED.md). Open, configure, recurse,
 * close -- exactly what the macro expands to, minus the designated
 * initializers.
 */
    void declare(const Box& b)
    {
        Clay_ElementDeclaration decl{};
        decl.layout.sizing.width   = axis(b.w_kind, b.w_value);
        decl.layout.sizing.height  = axis(b.h_kind, b.h_value);
        decl.layout.layoutDirection = b.dir ? CLAY_TOP_TO_BOTTOM : CLAY_LEFT_TO_RIGHT;
        decl.layout.childGap        = static_cast<uint16_t>(b.gap);
        const uint16_t p = static_cast<uint16_t>(b.padding);
        decl.layout.padding = Clay_Padding{ p, p, p, p };

        Clay__OpenElementWithId(idFor(b.node));
        Clay__ConfigureOpenElement(decl);
        for (const Box& child : m_boxes)
            if (child.parent == b.node) declare(child);
        Clay__CloseElement();
    }

    static void onClayError(Clay_ErrorData e)
    {
        ETCS_LOG("Clayout", "clay: " << std::string(e.errorText.chars,
                                                       static_cast<size_t>(e.errorText.length)));
    }

    void*         m_memory  = nullptr;
    Clay_Context* m_context = nullptr;
    std::vector<Box> m_boxes;
};

#endif
