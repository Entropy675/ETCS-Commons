#ifndef LAYOUTPROVIDER_H__
#define LAYOUTPROVIDER_H__

#define ETCS_DLL_EXPORTS
#include "../../core_defs.h"
#include "../../ontology.h"
#include "Contract_LayoutProvider.h"

/*
 * LayoutProvider -- one tag, and deliberately one.
 *
 *   Clayout   -- a layout solver [Resizable + Deletable]. Holds a tree of
 *                boxes, solves it, and writes the answers back through
 *                Drawable2D_::MoveTo and Resizable_::ResizeTo.
 *
 * It arranges nodes it reaches by family name, so it has no link-time
 * relationship with the provider those nodes came from and could arrange a
 * second renderer's tree unchanged. That independence is the reason it is a
 * provider rather than a file inside RenderProvider, and the reason the whole
 * module can be handed to someone else intact.
 *
 * The script-facing surface is small on purpose: declare boxes, say how they
 * size, solve. Everything expressive about a layout is in WHICH boxes you
 * declare and how, not in the number of verbs.
 */

// ── Clayout ──────────────────────────────────────────────────────────────
//
// A layout is DECLARED once and re-solved whenever the size changes, so these
// are setup verbs plus one trigger. Clayout.h has the reasoning on why it is
// not a node in the tree it arranges, and why it names no renderer.

DEFINE_WORK_FUNC_TYPED(Clayout, Create, (uint32_t, w), (uint32_t, h))
{
    (void)ctx;
    if (!self.Create(w, h))
        ETCS_LOG("Clayout::Create", "layout not created.");
}

/*
 * AddBox <node> <parent> <w_kind> <w_value> <h_kind> <h_value>
 *
 * kinds: 0 fit, 1 grow, 2 fixed, 3 percent. Numbers rather than four verbs
 * that differ in one word, and a script says which it means in a comment
 * beside the call -- see scripts/paint_editor.etcs for the shape of that.
 *
 * parent 0 means "this is the root of the layout".
 */
DEFINE_WORK_FUNC_TYPED(Clayout, AddBox,
    (ETCS::RID, node), (ETCS::RID, parent),
    (int32_t, w_kind), (float, w_value),
    (int32_t, h_kind), (float, h_value))
{
    (void)ctx;
    self.AddBox(node, parent,
                static_cast<Clayout::Sizing>(w_kind), w_value,
                static_cast<Clayout::Sizing>(h_kind), h_value);
}

// SetDirection <node> <0 row | 1 column>
DEFINE_WORK_FUNC_TYPED(Clayout, SetDirection, (ETCS::RID, node), (int32_t, dir))
{
    (void)ctx;
    self.SetDirection(node, dir != 0);
}

DEFINE_WORK_FUNC_TYPED(Clayout, SetPadding, (ETCS::RID, node), (float, px))
{
    (void)ctx;
    self.SetPadding(node, px);
}

DEFINE_WORK_FUNC_TYPED(Clayout, SetGap, (ETCS::RID, node), (float, px))
{
    (void)ctx;
    self.SetGap(node, px);
}

// Solve once, now -- for the first pass, and for a script that has just
// changed the declaration. FollowResize does this on every size change.
DEFINE_WORK_FUNC(Clayout, Solve)
{
    (void)ctx; (void)data;
    self.Solve();
}

/*
 * FollowResize <rid> -- track another Resizable and re-solve on every change.
 *
 * The whole type in one line: `layout.FollowResize(@main)` and every box it
 * owns follows the window from then on, with the re-solve happening where the
 * size arrives rather than on a frame tick. The follower is resolved by RID at
 * fire time (ontology/Resizable.h), so deleting the layout mid-drag is fine.
 */
DEFINE_WORK_FUNC_TYPED(Clayout, FollowResize, (ETCS::RID, source))
{
    (void)ctx;
    ETCS::Held<Resizable_> src = ETCS::resolve_held<Resizable_>("Resizable", source);
    if (!src)
    {
        ETCS_LOG("Clayout::FollowResize", "RID:" << source
                 << " is not a live Resizable -- nothing to follow.");
        return;
    }
    self.FollowResize(src.get());
    ETCS_LOG("Clayout::FollowResize", "now tracking RID:" << source << ".");
}

DEFINE_WORK_FUNC(Clayout, Report)
{
    (void)ctx; (void)data;
    self.Report();
}

DEFINE_WORK_FUNC(Clayout, Delete)
{
    (void)ctx; (void)data;
    self.DeleteConcrete();
}

#endif // LAYOUTPROVIDER_H__
