#ifndef RENDERPROVIDER_H__
#define RENDERPROVIDER_H__

#define ETCS_DLL_EXPORTS
#include "../../core_defs.h"
#include "../../ontology.h"
#include "Contract_RenderProvider.h"

#include <string>

// RenderProvider -- Vulkan, three tags:
//
//   Instance     -- the VkInstance/device/queue/command pool. Flat (no
//                   ontology supertype but Deletable): there is one Vulkan
//                   backend, and nothing addresses "an instance"
//                   generically.
//   Surface      -- the window-bound presentable surface
//                   [Surface + Presentable + Resizable]. Spawned as a
//                   CHILD of a WindowProvider::Window; reaches its parent
//                   through the generic interface-pointer surface, never
//                   through a compile-time dependency on WindowProvider.
//   ImageSurface -- an offscreen CPU-backed surface
//                   [Surface + Pixels + Resizable]. A layer.
//
// The pair is the point: both answer Clear/DrawRect/Blit, one composites
// on the CPU into its own bytes and the other uploads and draws on the
// GPU, and a caller composing a layer stack writes the same calls either
// way. That is the surface PintaProvider projects onto.
//
// Deliberately absent, all flagged rather than forgotten:
//   - validation layers (VulkanInstance::Create's own comment)
//   - a script-driven frame loop: .etcs has no loop construct today, so
//     Surface.RunDemo drives frames internally the way Window.Run already
//     does. The real fix is a ProduceFrames/ConsumeFrames stream pair, at
//     which point this tag becomes HYBRID -- see RenderProvider.cc.
//   - resampling on blit (Pixels_::Composite's own comment)

// Resolves a script-supplied RID against THIS module's own registry rows.
//
// It would read better as a lookup in the "Pixels" FAMILY aggregate --
// ETCS_SUPERTYPE_BASE publishes exactly such a row per family, and Blit
// wants "whatever entity owns these pixels", not "whichever of my tags it
// happens to be". That does not work today: those family rows are
// published but never populated. The only invoke_insert call sites in the
// runtime (core/Entity.h) are reparenting, an entity's typed_children_, and
// the module row keyed by T::CONTRACT_TAG -- nothing ever fans an instance
// INTO its families' aggregates, even though destroyImpl (DynamicLoader.h)
// already contains the matching fan-OUT and its comment describes fan-in as
// existing. Verified by hitting it: a spawned ImageSurface is absent from
// the "Pixels" row while carrying the Pixels type tag.
//
// So this scans this module's rows instead: invoke_get returns null for a
// RID a row does not hold, so the first hit is the owner. The consequence
// to know about is that a script can only blit from a surface THIS module
// spawned -- the C++ seam below has no such limit (a caller holding an
// Entity* passes it straight to Blit, and any module's Pixels_ answers), so
// PintaProvider reaching in from C++ is unaffected. Making the script path
// equally general needs the family fan-in, and that needs the interface
// pointer rather than the Entity* (a Pixels_* and an Entity* for the same
// object are different addresses under multiple inheritance), which is a
// core decision rather than something to improvise here.
static inline ETCS::Entity* rp_resolve_local(ETCS::RID rid)
{
    if (rid == 0) return nullptr;
    for (auto& [key, handle] : ETCS::EventNode::getInstance().ridMap)
    {
        (void)key;
        if (ETCS::Entity* e = handle.invoke_get(rid)) return e;
    }
    return nullptr;
}

// The one row lookup that IS a single tag: Instance is flat by design, so
// its tag row is the only place it appears.
static inline ETCS::Entity* rp_resolve_tag(const char* tag, ETCS::RID rid)
{
    if (rid == 0) return nullptr;
    auto& ridMap = ETCS::EventNode::getInstance().ridMap;
    auto it = ridMap.find(ETCS::Buffer(tag));
    if (it == ridMap.end()) return nullptr;
    return it->second.invoke_get(rid);
}

// ── Instance ─────────────────────────────────────────────────────────────

DEFINE_WORK_FUNC(Instance, Create)
{
    (void)ctx; (void)data;
    if (!self.Create())
        ETCS_LOG("Instance::Create", "Vulkan instance bring-up failed.");
}

DEFINE_WORK_FUNC(Instance, Delete)
{
    (void)ctx; (void)data;
    self.DeleteConcrete();
}

// ── Surface (window-bound, presentable) ──────────────────────────────────

// Create <instance_rid> [<shader_dir>] -- the RID is parsed off the raw
// buffer rather than declared as a typed field, matching HttpServer's own
// AddHandler/AddRoute convention (NetworkProvider.h) for taking a
// script-supplied @name. shader_dir defaults to "shaders/", resolved
// relative to the process cwd exactly like FileHtmlPage::LoadFromDisk's
// path -- nothing in this codebase locates a module's own install
// directory yet, and inventing that mechanism here would be a bigger
// change than this milestone warrants.
DEFINE_WORK_FUNC(Surface, Create)
{
    (void)ctx;
    ETCS::RID   instance_rid = 0;
    std::string shader_dir;
    data >> instance_rid;
    data >> shader_dir;

    if (instance_rid == 0)
    {
        ETCS_LOG("Surface::Create", "no Instance RID given -- spawn a RenderProvider::Instance, "
                                     "call Create on it, and pass it here.");
        return;
    }

    // Resolved through this module's own "Instance" tag list rather than a
    // family aggregate: Instance is flat by design, so its tag list IS the
    // only place it appears.
    ETCS::Entity* raw = rp_resolve_tag("Instance", instance_rid);
    if (!raw)
    {
        ETCS_LOG("Surface::Create", "RID:" << instance_rid << " is not a RenderProvider::Instance.");
        return;
    }
    // getTrueType(), not a static_cast off Entity*: Entity is a VIRTUAL
    // base here, so a direct downcast from it is ill-formed -- this is the
    // same recovery route the work-func trampolines themselves use.
    Instance* instance = static_cast<Instance*>(raw->getTrueType());

    if (!self.Create(instance, shader_dir))
        ETCS_LOG("Surface::Create", "surface bring-up failed.");
}

DEFINE_WORK_FUNC_TYPED(Surface, Clear, (float, r), (float, g), (float, b), (float, a))
{
    (void)ctx;
    self.Clear(r, g, b, a);
}

DEFINE_WORK_FUNC_TYPED(Surface, DrawRect, (int32_t, x), (int32_t, y), (uint32_t, w), (uint32_t, h),
                                            (float, r), (float, g), (float, b), (float, a))
{
    (void)ctx;
    self.DrawRect(x, y, w, h, r, g, b, a);
}

// Blit <source_rid> <x> <y> <w> <h> <opacity> -- w/h of 0 mean the
// source's own size. Source resolution is module-local for now; see
// rp_resolve_local's comment for what that costs and why.
DEFINE_WORK_FUNC_TYPED(Surface, Blit, (ETCS::RID, source), (int32_t, x), (int32_t, y),
                                       (uint32_t, w), (uint32_t, h), (float, opacity))
{
    (void)ctx;
    ETCS::Entity* src = rp_resolve_local(source);
    if (!src)
    {
        ETCS_LOG("Surface::Blit", "RID:" << source << " is not an entity this module spawned (see rp_resolve_local).");
        return;
    }
    self.Blit(src, x, y, w, h, opacity);
}

DEFINE_WORK_FUNC(Surface, Present)
{
    (void)ctx; (void)data;
    self.Present();
}

DEFINE_WORK_FUNC(Surface, Delete)
{
    (void)ctx; (void)data;
    self.DeleteConcrete();
}

// Manual-verification convenience -- see this file's own header comment.
DEFINE_WORK_FUNC_TYPED(Surface, RunDemo, (uint32_t, frames))
{
    (void)ctx;
    ETCS::Entity* parent = self.getParent();
    if (!parent)
    {
        ETCS_LOG("Surface::RunDemo", "no parent window to poll.");
        return;
    }
    self.RunDemo(parent, frames);
}

// ── ImageSurface (offscreen, CPU-backed) ─────────────────────────────────

DEFINE_WORK_FUNC_TYPED(ImageSurface, Create, (uint32_t, w), (uint32_t, h))
{
    (void)ctx;
    if (!self.Create(w, h))
        ETCS_LOG("ImageSurface::Create", "allocation failed for " << w << "x" << h);
}

DEFINE_WORK_FUNC_TYPED(ImageSurface, Clear, (float, r), (float, g), (float, b), (float, a))
{
    (void)ctx;
    self.Clear(r, g, b, a);
}

DEFINE_WORK_FUNC_TYPED(ImageSurface, DrawRect, (int32_t, x), (int32_t, y), (uint32_t, w), (uint32_t, h),
                                                 (float, r), (float, g), (float, b), (float, a))
{
    (void)ctx;
    self.DrawRect(x, y, w, h, r, g, b, a);
}

// Same call as Surface.Blit, resolved the same way -- this is the CPU
// half of the pair (layer onto layer, rather than layer onto screen).
DEFINE_WORK_FUNC_TYPED(ImageSurface, Blit, (ETCS::RID, source), (int32_t, x), (int32_t, y),
                                            (uint32_t, w), (uint32_t, h), (float, opacity))
{
    (void)ctx;
    ETCS::Entity* src = rp_resolve_local(source);
    if (!src)
    {
        ETCS_LOG("ImageSurface::Blit", "RID:" << source << " is not an entity this module spawned (see rp_resolve_local).");
        return;
    }
    self.Blit(src, x, y, w, h, opacity);
}

DEFINE_WORK_FUNC(ImageSurface, Delete)
{
    (void)ctx; (void)data;
    self.DeleteConcrete();
}

#endif
