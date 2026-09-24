#ifndef RENDERPROVIDER_H__
#define RENDERPROVIDER_H__

#define ETCS_DLL_EXPORTS
#include "../../core_defs.h"
#include "../../ontology.h"
#include "Contract_RenderProvider.h"

#include <chrono>
#include <string>
#include <thread>

// RenderProvider -- the surfaces, and the device they can draw through:
//
//   Instance     -- the GPU: Vulkan natively, WebGPU in a browser. Flat (no
//                   ontology supertype but Deletable); nothing addresses "an
//                   instance" generically.
//   Device       -- "this entity can reach that Instance", as a child of the
//                   entity. Attaching one is the switch (ontology/Device.h).
//   Surface      -- the window surface [Surface + Pixels + Presentable +
//                   Resizable]: a host raster on every platform, drawing and
//                   presenting through the device while a ready Device is
//                   under it (OS/HostSurface.h). Spawned as a CHILD of a
//                   WindowProvider::Window; reaches its parent through the
//                   generic interface-pointer surface, never through a
//                   compile-time dependency on WindowProvider.
//   ImageSurface -- an offscreen CPU-backed surface
//                   [Surface + Pixels + Resizable]. A layer.
//
// Every surface answers Clear/DrawRect/Blit, and a caller composing a layer
// stack writes the same calls whether the window under it rasterises on the
// CPU or records for the GPU. That is the surface PaintProvider projects onto.
//
// Deliberately absent, all flagged rather than forgotten:
//   - validation layers (VulkanInstance::Create's own comment)
//   - a script-driven frame loop: .etcs has no loop construct, so the frame
//     edge is Surface.RunFrames, a work function a script detaches (the
//     frame-edge note below), and Surface.RunDemo drives frames internally
//     the way Window.Run does.
//   - resampling on blit (Pixels_::Composite's own comment)

// Source resolution for Blit: a RID in, a Surface_* out, through the
// ontology family aggregate.
//
// This is the general form -- ETCS::resolve_in_family (core/Entity.h) finds
// the entity in the "Surface" family list whoever built it, and hands back
// the correctly-adjusted Surface_* interface pointer. So a script can blit
// from ANY module's surface, not just one RenderProvider spawned, and this
// module needs no compile-time knowledge of what the source concretely is.

// rp_resolve_tag now lives in Contract_RenderProvider.h -- see its comment
// there for why it had to move up.

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

// ── Device ───────────────────────────────────────────────────────────────
//
// Takes the Instance's RID and nothing else: what this type says is which
// device is reachable from whatever it is a child of, and that is one fact.

DEFINE_WORK_FUNC_TYPED(Device, Create, (ETCS::RID, instance_rid))
{
    (void)ctx;
    if (!self.Create(instance_rid))
        ETCS_LOG("Device::Create", "RID:" << instance_rid
                 << " is not a Created RenderProvider::Instance -- whatever owns "
                    "this keeps working on the host.");
}

DEFINE_WORK_FUNC(Device, Delete)
{
    (void)ctx; (void)data;
    self.Delete();
}

// ── Surface (window-bound, presentable) ──────────────────────────────────

/*
 * Create [<instance_rid> [<shader_dir>]] -- the host raster needs neither. An
 * Instance given here is the old spelling of attaching a device, kept because
 * every script before devices were children says it: the surface comes up on
 * the host, and a Device child naming that Instance is spawned under it, so
 * the switch is the same child a script would have spawned itself
 * (RenderProvider/Device.h). An Instance that failed to come up spawns
 * nothing, and the window is simply on the host.
 *
 * The RID is parsed off the raw buffer rather than declared as a typed field,
 * matching HttpServer's own AddRoute convention for a script-supplied @name.
 * shader_dir overrides where the Vulkan backend looks for its SPIR-V.
 */
DEFINE_WORK_FUNC(Surface, Create)
{
    (void)ctx;
    ETCS::RID   instance_rid = 0;
    std::string shader_dir;
    data >> instance_rid;
    data >> shader_dir;

    if (!self.Create(shader_dir))
    {
        ETCS_LOG("Surface::Create", "surface bring-up failed.");
        return;
    }
    if (instance_rid == 0) return;

    // Resolved through this module's own "Instance" tag list: Instance is
    // flat by design, so its tag list is the only place it appears.
    // getTrueType(), not a static_cast off Entity* -- Entity is a virtual base.
    ETCS::Entity* raw = rp_resolve_tag("Instance", instance_rid);
    Instance* instance = raw ? static_cast<Instance*>(raw->getTrueType()) : nullptr;
    if (!instance || !instance->Usable())
    {
        ETCS_LOG("Surface::Create", "RID:" << instance_rid << " is not a usable RenderProvider::Instance "
                 "-- this surface stays on the host.");
        return;
    }
    Device* dev = self.addTag<Device>();
    if (!dev || !dev->Create(instance_rid))
        ETCS_LOG("Surface::Create", "could not attach a Device for RID:" << instance_rid
                 << " -- this surface stays on the host.");
}

/*
 * UseDevice 0|1 -- the standing preference: 1 (the default) draws through a
 * ready Device child whenever there is one, 0 keeps this surface on the host
 * with the device still attached. The camera's SetDeviceProjection, for a
 * window.
 */
DEFINE_WORK_FUNC_TYPED(Surface, UseDevice, (uint32_t, on))
{
    (void)ctx;
    self.UseDevice(on != 0);
    ETCS_LOG("Surface::UseDevice", (on ? "a ready Device child is used from the next frame."
                                       : "held on the host from the next frame."));
}

/*
 * WHICH OUTPUT THIS SURFACE PRESENTS TO, by name.
 *
 * One session, several surfaces, several destinations: in the browser the name is
 * a canvas element's id and a page may hold as many as it likes, which is what
 * makes a toolbar strip beside the main view possible at all
 * (HostSurface::SetTarget explains why this belongs to the surface and not to
 * the window). A native window has one output and says so rather than storing
 * a name nothing reads.
 */
DEFINE_WORK_FUNC_TYPED(Surface, SetTarget, (std::string, element_id))
{
    (void)ctx;
    self.SetTarget(element_id);
}

/*
 * An explicit size, which a surface following its window does not have. Stating
 * one makes this surface a REGION of the page rather than the whole frame, and on
 * the browser backend it also stops the follow -- being told and following cannot
 * both be live (HostSurface::ResizeTo).
 */
DEFINE_WORK_FUNC_TYPED(Surface, ResizeTo, (uint32_t, w), (uint32_t, h))
{
    (void)ctx;
    if (!self.ResizeTo(WindowSize{ w, h }))
        ETCS_LOG("Surface::ResizeTo", "the surface declined " << w << "x" << h << ".");
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
// source's own size. The source is resolved through the Surface family, so
// it can be any surface from any module.
DEFINE_WORK_FUNC_TYPED(Surface, Blit, (ETCS::RID, source), (int32_t, x), (int32_t, y),
                                       (uint32_t, w), (uint32_t, h), (float, opacity))
{
    (void)ctx;
    Surface_* src = ETCS::resolve_in_family<Surface_>("Surface", source);
    if (!src)
    {
        ETCS_LOG("Surface::Blit", "RID:" << source << " is not a Surface.");
        return;
    }
    self.Blit(src, x, y, w, h, opacity);
}

// Compose <drawable_rid> -- bind a Drawable root that the frame edge re-walks
// every tick, instead of replaying whatever the script last drew. Zero unbinds
// and returns the surface to the retained model. See
// HostSurface::SetComposeRoot for why a tree that moves needs the other one.
DEFINE_WORK_FUNC_TYPED(Surface, Compose, (ETCS::RID, root))
{
    (void)ctx;
    if (root != 0 && !ETCS::resolve_in_family<Drawable_>("Drawable", root))
    {
        ETCS_LOG("Surface::Compose", "RID:" << root << " does not resolve as a Drawable.");
        return;
    }
    self.SetComposeRoot(root);
    ETCS_LOG("Surface::Compose", (root == 0
             ? "unbound -- back to the retained composition."
             : "bound; the frame edge now re-walks this tree each tick."));
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

// ── Surface frame edge ───────────────────────────────────────────────────
//
// ONE TICK, NOT A PAIR. The pacing is a number on the Presentable family and
// the recording is a step on it (ontology/PresentableBase.h, which has the
// argument: a standing produce body never returns its pool worker, so a pair
// gives the pool a minimum size). Every queue-touching call happens on one
// thread, which is the real invariant; it is whichever thread drives the
// family. Splitting the device work instead -- acquire on one thread, submit
// on another -- puts two threads on one VkQueue and one VkSwapchainKHR, both
// of which the application must externally synchronise, and buys nothing.
//
// Draws arrive from whatever thread calls Clear/DrawRect/Blit -- the script's
// -- so the surface's own state is mutex-guarded and Present works off a
// snapshot. See HostSurface::PresentConcrete.
// Default pacing, in milliseconds, when the stream config says nothing.
// ~60Hz, a placeholder for asking the swapchain about its present mode,
// which is where real pacing belongs.
static constexpr uint32_t RENDER_FRAME_INTERVAL_MS = 16;

/*
 * RunFrames [<interval_ms>] -- THE SESSION'S TICK, and the only standing loop
 * on this path. Somebody has to call the Animated family's driver and a
 * session needs exactly one caller: this is it, on a detached script thread
 * the script asked for explicitly rather than a pool worker taken silently
 * (the frame-edge note above on why not a stream pair). It advances EVERY
 * Animated leaf, not just this surface -- the palette's click-and-hold and the
 * layer window's fade ride the same tick -- and a session with no surface at
 * all can drive the family from its own loop instead (ontology/Animated.h).
 *
 * ENDS ON THE SURFACE'S OWN ANSWER: a surface goes retired for reasons this
 * loop knows nothing about, and in the moments between that and the closure
 * ending this is the thing still walking a tree being torn down.
 */
DEFINE_WORK_FUNC(Surface, RunFrames)
{
    uint32_t interval_ms = RENDER_FRAME_INTERVAL_MS;
    {
        std::string cfg = data.restAsString();
        if (!cfg.empty())
        {
            try { interval_ms = static_cast<uint32_t>(std::stoul(cfg)); }
            catch (const std::exception&)
            {
                ETCS_LOG("Surface::RunFrames", "unreadable interval '" << cfg
                         << "' -- using the " << RENDER_FRAME_INTERVAL_MS << "ms default.");
            }
        }
    }
    self.SetFrameInterval(static_cast<double>(interval_ms));
    ETCS_LOG("Surface::RunFrames", "tick started at "
             << (interval_ms == 0 ? std::string("max speed (unpaced)")
                                  : std::to_string(interval_ms) + "ms")
             << " -- one driver for the whole Animated family.");

    /*
 * THE WAIT IS GONE, and that is a consequence rather than an omission. Both old
 * bodies opened with a cooperative-pause loop spinning until IsActive(), which
 * could never terminate on the browser's main thread and needed a log line
 * saying so. A step answers "not ready" in one virtual call and costs nothing,
 * so the readiness question is asked by the family every visit instead of being
 * waited out once by a parked thread.
 */
    uint64_t ticks = 0;
    while (!ctx.isInterrupted() && !ctx.isTerminated())
    {
        if (self.Retired())
        {
            ETCS_LOG("Surface::RunFrames", "surface retired after " << ticks
                     << " ticks -- ending the tick rather than driving a "
                     "torn-down graph.");
            break;
        }
        etcs_advance_animated();
        ++ticks;
        // A floor, not the pacing: the frame interval lives on the family now
        // (PresentableBase). This only stops an unpaced surface from turning the
        // driver into a spin -- and at zero it deliberately does not, which is
        // what "unpaced" has always meant here.
        if (interval_ms != 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
        else
            std::this_thread::yield();
    }

    // This body has left -- see Threaded. The old pair said this twice, once per
    // half; there is one half now.
    self.Stop();
    ETCS_LOG("Surface::RunFrames", "tick stopped after " << ticks << " ticks.");
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
    Surface_* src = ETCS::resolve_in_family<Surface_>("Surface", source);
    if (!src)
    {
        ETCS_LOG("ImageSurface::Blit", "RID:" << source << " is not a Surface.");
        return;
    }
    self.Blit(src, x, y, w, h, opacity);
}

DEFINE_WORK_FUNC(ImageSurface, Delete)
{
    (void)ctx; (void)data;
    self.DeleteConcrete();
}

// ── PolygonDrawable2D ────────────────────────────────────────────────────
//
// The scene-graph leaf. A script builds a shape out of corner points stated
// in the PARENT's space, nests more of them inside it, and realises the whole
// tree onto any Surface with one call.

DEFINE_WORK_FUNC(PolygonDrawable2D, Create)
{
    (void)ctx; (void)data;
    self.Create();
}

// Corner points, in the parent's coordinate space -- which is what makes the
// child's own space a consequence of where you put it rather than a second
// thing to configure.
DEFINE_WORK_FUNC_TYPED(PolygonDrawable2D, AddPoint, (int32_t, x), (int32_t, y))
{
    (void)ctx;
    self.AddPoint(x, y);
}

DEFINE_WORK_FUNC(PolygonDrawable2D, ClearPoints)
{
    (void)ctx; (void)data;
    self.ClearPoints();
}

// SetOval <x> <y> <w> <h> -- an oval filling that box of the parent's space,
// with smooth edges. See PolygonDrawable2D::SetOval.
DEFINE_WORK_FUNC_TYPED(PolygonDrawable2D, SetOval, (int32_t, x), (int32_t, y), (uint32_t, w), (uint32_t, h))
{
    (void)ctx;
    self.SetOval(x, y, w, h);
}

DEFINE_WORK_FUNC_TYPED(PolygonDrawable2D, SetFill,
                       (float, r), (float, g), (float, b), (float, a))
{
    (void)ctx;
    self.SetFill(r, g, b, a);
}

// Restacks among siblings and tells the parent's list its ordering is stale
// -- the explicit seam, since the key moved without membership changing
// (ontology/Orderable.h).
DEFINE_WORK_FUNC_TYPED(PolygonDrawable2D, SetOrder, (int32_t, z))
{
    (void)ctx;
    self.SetOrder(z);
}

// Realise this node and EVERYTHING NESTED UNDER IT onto a surface. One call
// per frame for a whole scene -- the downward half of the contract, which is
// what removes the restatement a script-held scene needs.
//
// The target is resolved through the family aggregate, so it can be this
// module's window surface, its CPU layer, or any Surface a future provider
// registers: this work function has no idea which it got.
DEFINE_WORK_FUNC_TYPED(PolygonDrawable2D, Draw, (ETCS::RID, target))
{
    (void)ctx;
    Surface_* dst = ETCS::resolve_in_family<Surface_>("Surface", target);
    if (!dst)
    {
        ETCS_LOG("PolygonDrawable2D::Draw", "target RID:" << target
                 << " does not resolve as a Surface.");
        return;
    }
    self.DrawInto(dst);
}

DEFINE_WORK_FUNC_TYPED(PolygonDrawable2D, Clear,
                       (float, r), (float, g), (float, b), (float, a))
{
    (void)ctx;
    self.Clear(r, g, b, a);
}

DEFINE_WORK_FUNC_TYPED(PolygonDrawable2D, DrawRect,
                       (int32_t, x), (int32_t, y), (uint32_t, w), (uint32_t, h),
                       (float, r), (float, g), (float, b), (float, a))
{
    (void)ctx;
    self.DrawRect(x, y, w, h, r, g, b, a);
}

DEFINE_WORK_FUNC_TYPED(PolygonDrawable2D, Blit, (ETCS::RID, source),
                       (int32_t, x), (int32_t, y), (uint32_t, w), (uint32_t, h),
                       (float, opacity))
{
    (void)ctx;
    Surface_* src = ETCS::resolve_in_family<Surface_>("Surface", source);
    if (!src)
    {
        ETCS_LOG("PolygonDrawable2D::Blit", "source RID:" << source
                 << " does not resolve as a Surface.");
        return;
    }
    self.Blit(src, x, y, w, h, opacity);
}

DEFINE_WORK_FUNC(PolygonDrawable2D, Delete)
{
    (void)data; (void)ctx;
    self.DeleteConcrete();
}

// ── CompositeDrawable2D ──────────────────────────────────────────────────

DEFINE_WORK_FUNC_TYPED(CompositeDrawable2D, Create, (uint32_t, w), (uint32_t, h))
{
    (void)ctx;
    self.Create(w, h);
}

// Where this buffer sits in its parent's space. Its CONTENTS do not move with
// it -- children are stated in this node's own space, so moving the compositor
// moves the whole merged result and nothing inside it is recomputed.
/*
 * SetHidden <0|1> -- present in the tree, not drawn. The family's own flag
 * (ontology/DrawableBase.h says why it lives there and what it does not mean);
 * this is only the script's way to reach it, and a popup is the caller that needs
 * it: a panel with no way to be hidden is either always on screen or has to be
 * unparented, and both make "is it showing" a second fact.
 */
DEFINE_WORK_FUNC_TYPED(CompositeDrawable2D, SetHidden, (int32_t, hidden))
{
    (void)ctx;
    self.SetHidden(hidden != 0);
}

DEFINE_WORK_FUNC_TYPED(CompositeDrawable2D, SetPosition, (int32_t, x), (int32_t, y))
{
    (void)ctx;
    self.SetPosition(x, y);
}

DEFINE_WORK_FUNC_TYPED(CompositeDrawable2D, SetOrder, (int32_t, z))
{
    (void)ctx;
    self.SetOrder(z);
}

DEFINE_WORK_FUNC_TYPED(CompositeDrawable2D, SetBackground,
                       (float, r), (float, g), (float, b), (float, a))
{
    (void)ctx;
    self.SetBackground(r, g, b, a);
}

// SetRetain <0|1> -- stop clearing to the background on recompose, for a node
// whose pixels something outside the tree also writes. See SetRetain itself.
DEFINE_WORK_FUNC_TYPED(CompositeDrawable2D, SetRetain, (int32_t, on))
{
    (void)ctx;
    self.SetRetain(on != 0);
    ETCS_LOG("CompositeDrawable2D", "retain " << (on ? "ON -- the buffer is the picture"
                                                    : "OFF -- the buffer is derived from the tree"));
}

// SetPassthrough <0|1> -- 1 raises the `passthrough` flag: drawn, and no pick
// lands on the node or on anything inside it. See SetPassthrough itself.
DEFINE_WORK_FUNC_TYPED(CompositeDrawable2D, SetPassthrough, (int32_t, on))
{
    (void)ctx;
    self.SetPassthrough(on != 0);
}

/*
 * MoveTo / ResizeTo -- the FAMILY verbs, exposed so a script can drive by hand
 * exactly what a layout drives automatically.
 *
 * MoveTo does the same work as SetPosition and both stay: SetPosition is what a
 * compositor has always been told, MoveTo is what the family asks of anything
 * placeable (ontology/Drawable2D.h). Naming them apart rather than aliasing one
 * to the other keeps a script that never heard of layout reading the way it
 * always did.
 *
 * ResizeTo REALLOCATES the buffer -- see CompositeDrawable2D.h on what happens
 * to a retained canvas's pixels when it does.
 */
DEFINE_WORK_FUNC_TYPED(CompositeDrawable2D, MoveTo, (int32_t, x), (int32_t, y))
{
    (void)ctx;
    self.MoveTo(Point2D{ x, y });
}

DEFINE_WORK_FUNC_TYPED(CompositeDrawable2D, ResizeTo, (uint32_t, w), (uint32_t, h))
{
    (void)ctx;
    if (!self.ResizeTo(WindowSize{ w, h }))
        ETCS_LOG("CompositeDrawable2D::ResizeTo", "refused " << w << "x" << h << ".");
}

/*
 * FollowResize <rid> -- be whatever size that Resizable is, from now on.
 *
 * The ontology verb (Resizable_::FollowResize), exported here because a
 * full-bleed pane is not a layout question: a box that is simply the window, at
 * the window's origin, needs no solver and cannot be stated as a row or a
 * column. A full-sheet overlay is exactly that -- the ruler's own raster is one
 * -- and before this the only way to have one follow anything was to declare it
 * in a Layout it does not belong in.
 *
 * Pushed, for the reason Layout's is (LayoutProvider.h): a drawable sits in no
 * loop that would ask on its own, and the wake carries no size, so one that
 * lands late still reads the current one.
 */
DEFINE_WORK_FUNC_TYPED(CompositeDrawable2D, FollowResize, (ETCS::RID, source))
{
    (void)ctx;
    ETCS::Held<Resizable_> src = ETCS::resolve_held<Resizable_>("Resizable", source);
    if (!src)
    {
        ETCS_LOG("CompositeDrawable2D::FollowResize", "RID:" << source
                 << " is not a live Resizable -- nothing to follow.");
        return;
    }
    self.FollowResize(src.get(), ResizeDelivery::Pushed);
}

// Recompose if anything beneath changed, then blit once. The same verb
// PolygonDrawable2D answers, doing the same job -- which is what lets a script
// swap one for the other without knowing which it has.
DEFINE_WORK_FUNC_TYPED(CompositeDrawable2D, Draw, (ETCS::RID, target))
{
    (void)ctx;
    Surface_* dst = ETCS::resolve_in_family<Surface_>("Surface", target);
    if (!dst)
    {
        ETCS_LOG("CompositeDrawable2D::Draw", "target RID:" << target
                 << " does not resolve as a Surface.");
        return;
    }
    self.DrawInto(dst);
    ETCS_LOG("CompositeDrawable2D::Draw", "recompositions so far: "
             << self.Recompositions());
}

DEFINE_WORK_FUNC_TYPED(CompositeDrawable2D, Clear,
                       (float, r), (float, g), (float, b), (float, a))
{
    (void)ctx;
    self.Clear(r, g, b, a);
}

DEFINE_WORK_FUNC_TYPED(CompositeDrawable2D, DrawRect,
                       (int32_t, x), (int32_t, y), (uint32_t, w), (uint32_t, h),
                       (float, r), (float, g), (float, b), (float, a))
{
    (void)ctx;
    self.DrawRect(x, y, w, h, r, g, b, a);
}

DEFINE_WORK_FUNC_TYPED(CompositeDrawable2D, Blit, (ETCS::RID, source),
                       (int32_t, x), (int32_t, y), (uint32_t, w), (uint32_t, h),
                       (float, opacity))
{
    (void)ctx;
    Surface_* src = ETCS::resolve_in_family<Surface_>("Surface", source);
    if (!src)
    {
        ETCS_LOG("CompositeDrawable2D::Blit", "source RID:" << source
                 << " does not resolve as a Surface.");
        return;
    }
    self.Blit(src, x, y, w, h, opacity);
}

DEFINE_WORK_FUNC(CompositeDrawable2D, Delete)
{
    (void)data; (void)ctx;
    self.DeleteConcrete();
}

// The turn rate, reported the way it is derived: the setting on one side, the
// lens it is measured against in the middle, and the radians that come out --
// so a value that feels wrong can be traced to which input is wrong rather
// than guessed at.
//
// The turns-per-pass line stays because it is the intuition people carry, but
// it reads as a CONSEQUENCE now. It used to be the setting, and printing it
// next to the sensitivity is the clearest way to say which of the two is
// upstream of the other.
static inline void logTurnRate(Scene3D& self)
{
    ETCS_LOG("Scene3D", "look mapping: the " << self.FrameWidth() << "x" << self.FrameHeight()
             << " frame spans " << self.YawSpanTurns() << " full turn(s) of yaw across its width "
             << "and " << self.PitchSpanDeg() << " degrees of pitch down its height "
             << "(sensitivity " << self.Sensitivity() << "). The pointer's position over the "
             << "frame IS the direction -- nothing accumulates, so putting it back where it was "
             << "puts the view back exactly.");
}

// ── Scene3D ──────────────────────────────────────────────────────────────

DEFINE_WORK_FUNC_TYPED(Scene3D, Create, (float, w), (float, h), (float, d))
{
    (void)ctx;
    self.Create(w, h, d);
}

// The CENTRE of this box, in its parent's space. On the root of a scene this
// is also the whole scene's position -- which is what makes WASD one
// translation rather than a walk (Scene3D.h).
DEFINE_WORK_FUNC_TYPED(Scene3D, SetPosition, (float, x), (float, y), (float, z))
{
    (void)ctx;
    self.SetPosition(x, y, z);
}

DEFINE_WORK_FUNC_TYPED(Scene3D, Move, (float, dx), (float, dy), (float, dz))
{
    (void)ctx;
    self.Move(dx, dy, dz);
}

DEFINE_WORK_FUNC_TYPED(Scene3D, SetColor, (float, r), (float, g), (float, b), (float, a))
{
    (void)ctx;
    self.SetColor(r, g, b, a);
}

DEFINE_WORK_FUNC_TYPED(Scene3D, SetOrder, (int32_t, z))
{
    (void)ctx;
    self.SetOrder(z);
}

// Terminal speed in scene units per SECOND, and how fast motion bleeds off.
// Held keys are an acceleration, not a displacement, so these two are the whole
// of the feel: terminal speed is where impulse and drag balance, and damping
// sets how quickly it gets there and how long it coasts after release.
DEFINE_WORK_FUNC_TYPED(Scene3D, SetSpeed, (float, units_per_sec))
{
    (void)ctx;
    self.SetSpeed(units_per_sec);
}

DEFINE_WORK_FUNC_TYPED(Scene3D, SetDamping, (float, per_sec))
{
    (void)ctx;
    self.SetDamping(per_sec);
}

/*
 * SetSensitivity <span> -- the ONE knob, and its unit is a SPAN: how much of
 * each rotational axis the camera's frame covers. 1.0 means the width is one
 * full turn of yaw and the height is the full pitch range, so every direction
 * is reachable without the pointer leaving the view.
 *
 * A RANGE, NOT A RATE, and that is why it can finally be tuned once. Nothing
 * accumulates in an absolute mapping, so this cannot compound or drift, and it
 * means the same thing after an hour as in the first second. The two rates it
 * replaces both failed for the same underlying reason -- see
 * Scene3D::SetSensitivity, and PointerPosition for why deltas were the wrong
 * primitive on a display that will not move a pointer.
 */
DEFINE_WORK_FUNC_TYPED(Scene3D, SetSensitivity, (float, scale))
{
    (void)ctx;
    self.SetSensitivity(scale);
    logTurnRate(self);
}

// A push, in joules along a direction -- the primitive the input edge drives,
// exposed because a script nudging a scene should go through the same energy
// accounting the keys do rather than around it.
DEFINE_WORK_FUNC_TYPED(Scene3D, Impulse, (float, dx), (float, dy), (float, dz),
                                          (float, joules))
{
    (void)ctx;
    self.Impulse(dx, dy, dz, joules);
}

// All kinetic energy to heat, at once. What a collision with something
// immovable does; the energy stays on the point, it just stops being motion.
DEFINE_WORK_FUNC(Scene3D, Halt)
{
    (void)data; (void)ctx;
    self.Halt();
}

// Read the order vector back: both rows, and the three quantities the relation
// between them defines. The shell's window into what a point is carrying.
// Where the look is pointing and how fast it gets there. Both angles carry
// bounds that applyLookTo enforces and nothing could previously read, and the
// rate is now derived from the camera's lens rather than set -- so all four of
// these are answers rather than settings, and this is the only place to see
// them.
DEFINE_WORK_FUNC(Scene3D, Look)
{
    (void)data; (void)ctx;
    constexpr float DEG = 57.2957795f;
    ETCS_LOG("Scene3D::Look",
             "yaw=" << self.Yaw() << " rad (" << self.Yaw() * DEG << " deg), wrapped to [-pi, pi)"
             "   pitch=" << self.Pitch() << " rad (" << self.Pitch() * DEG << " deg), "
             "the VIEW's elevation, +/-85 deg"
             "\n    the " << self.FrameWidth() << "x" << self.FrameHeight() << " frame spans "
             << self.YawSpanTurns() << " turn(s) of yaw across its width and "
             << self.PitchSpanDeg() << " degrees of pitch down its height (sensitivity "
             << self.Sensitivity() << ", yaw only -- a bounded axis cannot be compressed "
             "without putting its own stops inside the frame). Seeded looking "
             << self.ReferenceElevationDeg() << " deg from the horizon, which the pitch "
             "range is NOT centred on: the range is the world's, so the view cannot run "
             "past the pole at one end while stopping short at the other."
             "\n    Absolute: the pointer's position over the frame is the direction, so "
             "nothing accumulates and nothing drifts.");
}

DEFINE_WORK_FUNC(Scene3D, Order)
{
    (void)data; (void)ctx;
    const OrderVector& o = self.Order4();
    ETCS_LOG("Scene3D::Order",
             "row0 (" << o.x << ", " << o.y << ", " << o.z << ", RID:" << o.rid << ")  "
             "row1 (" << o.ox << ", " << o.oy << ", " << o.oz << ", E=" << o.energy << ")  "
             "kinetic=" << o.KineticEnergy() << " heat=" << o.Heat()
             << " fraction=" << o.KineticFraction()
             << "  emissivity=" << self.Emissivity()
             << " shed-to-environment=" << self.EmittedToEnvironment()
             << "\n    row2 pivot (" << o.fx << ", " << o.fy << ", " << o.fz
             << ", r=" << o.radius << ")  "
             << (o.IsAggregate() ? "aggregate" : "leaf")
             << "   row3 axis (" << o.sx << ", " << o.sy << ", " << o.sz
             << ", theta=" << o.theta << ")"
             << "\n    causal-ticks=" << self.CausalTicks()
             << "\n    last crossing: row0 (" << self.LastEmission().x << ", "
             << self.LastEmission().y << ", " << self.LastEmission().z
             << ", RID:" << self.LastEmission().rid << ")  row1 ("
             << self.LastEmission().ox << ", " << self.LastEmission().oy << ", "
             << self.LastEmission().oz << ", E=" << self.LastEmission().energy << ")"
             << "  heat=" << self.LastEmission().Heat()
             << " interval=" << self.LastEmission().interval
             << " uncertainty=" << std::hex << self.LastEmission().uncertainty
             << std::dec);
}

// How fast this node sheds heat into whatever contains it, per second. Drag
// turns motion into heat; this is where the heat goes. Zero is a perfect
// insulator and a legitimate thing to be.
DEFINE_WORK_FUNC_TYPED(Scene3D, SetEmissivity, (float, per_sec))
{
    (void)ctx;
    self.SetEmissivity(per_sec);
}

DEFINE_WORK_FUNC_TYPED(Scene3D, SetVisible, (int32_t, on))
{
    (void)ctx;
    self.SetVisible(on != 0);
}

// Project into a camera on demand -- the same verb the frame path calls, so a
// script can force one view without a frame edge running at all. Useful in the
// shell: move the scene, Render, look at it.
DEFINE_WORK_FUNC_TYPED(Scene3D, Project, (ETCS::RID, camera))
{
    (void)ctx;
    Camera_* cam = ETCS::resolve_in_family<Camera_>("Camera", camera);
    if (!cam)
    {
        ETCS_LOG("Scene3D::Project", "camera RID:" << camera
                 << " does not resolve as a Camera.");
        return;
    }
    self.Project(cam);
    ETCS_LOG("Scene3D::Project", "projections so far: " << self.Projections());
}

// Depth read-back, per pixel of a camera's frame, in scene units. The family's
// answer made reachable from a script -- negative means nothing of this scene
// is visible there (ontology/Drawable3D.h).
DEFINE_WORK_FUNC_TYPED(Scene3D, DepthAt, (ETCS::RID, camera), (int32_t, x), (int32_t, y))
{
    (void)ctx;
    Camera_* cam = ETCS::resolve_in_family<Camera_>("Camera", camera);
    if (!cam)
    {
        ETCS_LOG("Scene3D::DepthAt", "camera RID:" << camera
                 << " does not resolve as a Camera.");
        return;
    }
    ETCS_LOG("Scene3D::DepthAt", "(" << x << "," << y << ") = " << self.DepthAt(cam, x, y));
}

/*
 * ConsumeInput -- the WASD edge.
 *
 * A stream CONSUMER on the scene, fed by the window's own event producer:
 *
 *     window.ProduceEvents() -> scene.ConsumeInput()
 *
 * The two ends live on different entities in different modules, which is a
 * property of what a stream pair IS here (CommandExecutor.h builds the pair on
 * the consumer and hands the producer in) and not an arrangement this module
 * had to negotiate. WindowProvider does not know a renderer exists; this file
 * does not know GLFW exists. What crosses is an InputEvent, which belongs to
 * neither -- it is the ontology's (ontology/InputSource.h).
 *
 * WHY A BITSET AND A TICK, rather than moving once per event. An event says a
 * key CHANGED; motion depends on what is currently HELD, and those are
 * different questions. Draining every pending event into the bitset and then
 * integrating once per tick answers the second from the first, gives a rate
 * that is the tick's rather than the OS key-repeat's, and makes two keys held
 * at once a diagonal instead of a race between two repeat timers.
 *
 * hasData() is what makes that possible without a second thread: it is a
 * non-advancing liveness check (MirrorBuffer.h), so the drain takes what is
 * there and returns, where readRaw alone would block until the next keypress
 * and freeze the scene mid-stride the moment a key stopped repeating.
 *
 * The edge ends when the stream closes -- which the window does when it is
 * closed -- or when the closure is signalled. Nothing here polls the window
 * or touches GLFW; the producer already owns that.
 */
/*
 * The keyboard edge: W/S forward, A/D strafe, Q/E vertical.
 *
 * Blocking, and doing nothing but recording. It must not integrate -- motion is
 * advanced where it is OBSERVED, in Scene3D::Project, over the interval since
 * the last projection.
 *
 * hasData() is not used: a cross-tag pair resolves to StrategyPipe, whose
 * consumer fd is blocking, so a drain loop built on it spins here and stalls a
 * same-module pair. Blocking readRaw is what every consumer here does.
 */
DEFINE_STREAM_FUNC_CONSUME(Scene3D, ConsumeInput)
{
    (void)data;

    ETCS_LOG("Scene3D::ConsumeInput", "key edge open on RID:" << self.getRID()
             << " (" << self.Speed() << " u/s terminal, damping " << self.Damping() << "/s)");

    while (stream.isOpen())
    {
        if (ctx.isInterrupted() || ctx.isTerminated()) break;

        ETCS::Buffer slot;
        if (!stream.readRaw(slot)) break;

        InputEvent ev{};
        slot.readRaw(&ev, sizeof(InputEvent));
        if (ev.action == INPUT_MOTION || ev.key == 0) continue;

        if (ev.action == INPUT_DOWN) self.KeyDown(ev.key);
        else                         self.KeyUp(ev.key);

        // One mark per key change, no polling: it restarts a settled pipeline,
        // and motion sustains its own frames after that.
        self.WakeObservers();
    }

    ETCS_LOG("Scene3D::ConsumeInput", "key edge closed.");
}

/*
 * The pointer edge: absolute position in, look angle out.
 *
 * SEPARATE FROM THE KEYS on purpose. The two channels have nothing to
 * synchronise -- the look is a function of where the pointer is, and a step is a
 * function of which keys are held -- so sharing a stream would only let each
 * queue behind the other's bursts. See ontology/InputSource.h.
 */
DEFINE_STREAM_FUNC_CONSUME(Scene3D, ConsumeLook)
{
    (void)data;

    ETCS_LOG("Scene3D::ConsumeLook", "pointer edge open on RID:" << self.getRID()
             << " -- the pointer's position over the frame IS the direction.");

    while (stream.isOpen())
    {
        if (ctx.isInterrupted() || ctx.isTerminated()) break;

        ETCS::Buffer slot;
        if (!stream.readRaw(slot)) break;

        InputEvent ev{};
        slot.readRaw(&ev, sizeof(InputEvent));
        if (ev.action != INPUT_MOTION) continue;

        self.PointerPosition(ev.x, ev.y);
        self.WakeObservers();
    }

    ETCS_LOG("Scene3D::ConsumeLook", "pointer edge closed.");
}

DEFINE_WORK_FUNC(Scene3D, Delete)
{
    (void)data; (void)ctx;
    self.DeleteConcrete();
}

// ── Camera3D ─────────────────────────────────────────────────────────────

DEFINE_WORK_FUNC_TYPED(Camera3D, Create, (uint32_t, w), (uint32_t, h))
{
    (void)ctx;
    self.Create(w, h);
}

// Where the VIEW sits in its parent's 2D space -- the camera is a Drawable2D
// like any other, so this is the same call a compositor takes and means the
// same thing.
DEFINE_WORK_FUNC_TYPED(Camera3D, SetPosition, (int32_t, x), (int32_t, y))
{
    (void)ctx;
    self.SetPosition(x, y);
}

// The family verbs, same pair as CompositeDrawable2D's and here for the same
// reason -- a 3D view in a resizing panel is the first thing anyone puts in a
// layout. ResizeTo keeps nothing: the next projection fills the frame anyway.
DEFINE_WORK_FUNC_TYPED(Camera3D, MoveTo, (int32_t, x), (int32_t, y))
{
    (void)ctx;
    self.MoveTo(Point2D{ x, y });
}

DEFINE_WORK_FUNC_TYPED(Camera3D, ResizeTo, (uint32_t, w), (uint32_t, h))
{
    (void)ctx;
    if (!self.ResizeTo(WindowSize{ w, h }))
        ETCS_LOG("Camera3D::ResizeTo", "refused " << w << "x" << h << ".");
}

DEFINE_WORK_FUNC_TYPED(Camera3D, SetOrder, (int32_t, z))
{
    (void)ctx;
    self.SetOrder(z);
}

DEFINE_WORK_FUNC_TYPED(Camera3D, SetBackground,
                       (float, r), (float, g), (float, b), (float, a))
{
    (void)ctx;
    self.SetBackground(r, g, b, a);
}

DEFINE_WORK_FUNC_TYPED(Camera3D, LookAt, (float, ex), (float, ey), (float, ez),
                                          (float, tx), (float, ty), (float, tz))
{
    (void)ctx;
    self.LookAt(ex, ey, ez, tx, ty, tz);
}

DEFINE_WORK_FUNC_TYPED(Camera3D, SetLens, (float, fov_degrees),
                                           (float, near_plane), (float, far_plane))
{
    (void)ctx;
    self.SetLens(fov_degrees, near_plane, far_plane);
}

// Bind what this camera looks at. By RID, and resolved per render, so the
// scene may be deleted, replaced, or live in another module entirely.
DEFINE_WORK_FUNC_TYPED(Camera3D, SetScene, (ETCS::RID, scene))
{
    (void)ctx;
    if (!ETCS::resolve_in_family<Drawable3D_>("Drawable3D", scene))
        ETCS_LOG("Camera3D::SetScene", "RID:" << scene << " does not resolve as a "
                 "Drawable3D today -- bound anyway, it is resolved per render.");
    self.SetScene(scene);
}

/*
 * Ask this camera to project through a device instead of into its own pixels.
 *
 * RARELY NEEDED, because the default is yes: a camera uses a device as soon as
 * one is attached under it (ontology/Camera.h), so this exists to hold a camera
 * on the HOST deliberately -- comparing the two paths, or keeping one camera on
 * the CPU while another uses the GPU.
 *
 * uint32_t, because the script grammar has no bool: 0 is off, anything else on.
 * Same convention Window.CaptureMouse already uses.
 */
DEFINE_WORK_FUNC_TYPED(Camera3D, SetDeviceProjection, (uint32_t, on))
{
    (void)ctx;
    self.SetDeviceProjection(on != 0);
}

DEFINE_WORK_FUNC(Camera3D, Render)
{
    (void)data; (void)ctx;
    if (!self.Render())
        ETCS_LOG("Camera3D::Render", "no view produced -- no scene bound, a scene "
                 "that no longer resolves, or a degenerate frustum.");
}

// Render if stale, then blit once. The same verb, with the same meaning, that
// PolygonDrawable2D and CompositeDrawable2D answer -- which is what lets a
// script put a camera anywhere either of those could go.
DEFINE_WORK_FUNC_TYPED(Camera3D, Draw, (ETCS::RID, target))
{
    (void)ctx;
    Surface_* dst = ETCS::resolve_in_family<Surface_>("Surface", target);
    if (!dst)
    {
        ETCS_LOG("Camera3D::Draw", "target RID:" << target
                 << " does not resolve as a Surface.");
        return;
    }
    self.DrawInto(dst);
    ETCS_LOG("Camera3D::Draw", "renders so far: " << self.Renders());
}

DEFINE_WORK_FUNC_TYPED(Camera3D, Clear,
                       (float, r), (float, g), (float, b), (float, a))
{
    (void)ctx;
    self.Clear(r, g, b, a);
}

DEFINE_WORK_FUNC_TYPED(Camera3D, DrawRect,
                       (int32_t, x), (int32_t, y), (uint32_t, w), (uint32_t, h),
                       (float, r), (float, g), (float, b), (float, a))
{
    (void)ctx;
    self.DrawRect(x, y, w, h, r, g, b, a);
}

DEFINE_WORK_FUNC_TYPED(Camera3D, Blit, (ETCS::RID, source),
                       (int32_t, x), (int32_t, y), (uint32_t, w), (uint32_t, h),
                       (float, opacity))
{
    (void)ctx;
    Surface_* src = ETCS::resolve_in_family<Surface_>("Surface", source);
    if (!src)
    {
        ETCS_LOG("Camera3D::Blit", "source RID:" << source
                 << " does not resolve as a Surface.");
        return;
    }
    self.Blit(src, x, y, w, h, opacity);
}

DEFINE_WORK_FUNC(Camera3D, Delete)
{
    (void)data; (void)ctx;
    self.DeleteConcrete();
}

// ── TextLabel ────────────────────────────────────────────────────────────

DEFINE_WORK_FUNC_TYPED(TextLabel, Create, (uint32_t, size_px))
{
    (void)ctx;
    self.Create(size_px);
}

// The whole rest of the line is the text, so it may contain spaces and commas
// without quoting -- restAsString rather than a typed field, the same shape
// HttpServer's route handlers take a path.
DEFINE_WORK_FUNC(TextLabel, SetText)
{
    (void)ctx;
    self.SetText(data.restAsString());
}

DEFINE_WORK_FUNC_TYPED(TextLabel, SetSize, (uint32_t, size_px))
{
    (void)ctx;
    self.SetSize(size_px);
    ETCS_LOG("TextLabel::SetSize", "cell scale x" << self.Scale()
             << " (a bitmap font only lands on the grid at whole multiples).");
}

DEFINE_WORK_FUNC_TYPED(TextLabel, SetPosition, (int32_t, x), (int32_t, y))
{
    (void)ctx;
    self.SetPosition(x, y);
}

DEFINE_WORK_FUNC_TYPED(TextLabel, SetOrder, (int32_t, z))
{
    (void)ctx;
    self.SetOrder(z);
}

DEFINE_WORK_FUNC_TYPED(TextLabel, SetColor, (float, r), (float, g), (float, b), (float, a))
{
    (void)ctx;
    self.SetColor(r, g, b, a);
}

DEFINE_WORK_FUNC_TYPED(TextLabel, SetBackground,
                       (float, r), (float, g), (float, b), (float, a))
{
    (void)ctx;
    self.SetBackground(r, g, b, a);
}

// SetHidden <0|1> -- the same verb the compositor answers, for the same reason:
// a caption that is only shown on hover is a label somebody else hides and
// shows, and Drawable_::Hidden is where every drawable keeps that.
DEFINE_WORK_FUNC_TYPED(TextLabel, SetHidden, (int32_t, hidden))
{
    (void)ctx;
    self.SetHidden(hidden != 0);
}

DEFINE_WORK_FUNC_TYPED(PolygonDrawable2D, SetHidden, (int32_t, hidden))
{
    (void)ctx;
    self.SetHidden(hidden != 0);
}

DEFINE_WORK_FUNC_TYPED(TextLabel, SetPadding, (uint32_t, px))
{
    (void)ctx;
    self.SetPadding(px);
}

// Show a surface's live frame rate wherever "%f" appears in the text. See
// TextLabel::BindFps for why the substitution happens at draw time and not
// through a setter somebody would have to call every frame.
DEFINE_WORK_FUNC_TYPED(TextLabel, BindFps, (ETCS::RID, surface))
{
    (void)ctx;
    self.BindFps(surface);
    ETCS_LOG("TextLabel::BindFps", (surface == 0
             ? "unbound -- back to fixed text."
             : "bound; '%f' in this label now reads the surface's rate each frame."));
}

// What the run WOULD occupy, without drawing it. The layout half of the
// family, exposed because a script placing captions needs it before it can
// decide where they go.
DEFINE_WORK_FUNC(TextLabel, Measure)
{
    (void)ctx;
    const std::string run = data.restAsString();
    const TextExtent e = self.MeasureText(run.empty() ? self.Text().c_str() : run.c_str(),
                                          0, 0);
    ETCS_LOG("TextLabel::Measure", "'" << (run.empty() ? self.Text() : run)
             << "' -> " << e.width << "x" << e.height
             << " (baseline " << e.baseline << ")");
}

// Rasterize <target_rid> <x> <y> -- draw this label's text into any surface at
// a position, without nesting it. The imperative half, for a script that wants
// one-off text rather than a node that keeps drawing itself.
DEFINE_WORK_FUNC_TYPED(TextLabel, Rasterize, (ETCS::RID, target), (int32_t, x), (int32_t, y))
{
    (void)ctx;
    self.RasterizeText(target, self.Text().c_str(), 0, 0, x, y, 1.0f, 1.0f, 1.0f, 1.0f);
}

// Draw the label as a node -- background, text, then children. The same verb
// every other 2D leaf answers, which is what lets a script put a label
// anywhere a polygon could go.
DEFINE_WORK_FUNC_TYPED(TextLabel, Draw, (ETCS::RID, target))
{
    (void)ctx;
    Surface_* dst = ETCS::resolve_in_family<Surface_>("Surface", target);
    if (!dst)
    {
        ETCS_LOG("TextLabel::Draw", "target RID:" << target
                 << " does not resolve as a Surface.");
        return;
    }
    self.DrawInto(dst);
}

DEFINE_WORK_FUNC(TextLabel, Delete)
{
    (void)data; (void)ctx;
    self.DeleteConcrete();
}

/*
 * ── Throbber ───────────────────────────────────────────────────────────────
 *
 * Two lines gets you one: spawn it into a pane and Create it at a size. The
 * rest are for a caller who wants a different rate, a different accent, or a
 * different word -- each of them a value the type already holds, so none of
 * them is a second fact that can disagree with the first.
 *
 * NO Start/Stop. SetHidden is the switch, for the reason the type's own header
 * gives: a drawable already has exactly one answer to "is it showing", and a
 * running flag beside it would be a second one to keep in step. A throbber
 * hidden is a throbber not advancing, in one verb, and the scripts that hide a
 * pane already spell it this way (boot_paint_panels.etcs).
 */
DEFINE_WORK_FUNC_TYPED(Throbber, Create, (uint32_t, size_px))
{
    (void)ctx;
    self.Create(size_px);
    ETCS_LOG("Throbber::Create", "ring " << self.RingPx() << "px, " << self.Dots()
             << " dots, " << self.Step() << " deg/frame on RID:" << self.getRID());
}

DEFINE_WORK_FUNC_TYPED(Throbber, SetSize, (uint32_t, size_px))
{
    (void)ctx;
    self.SetSize(size_px);
}

/*
 * SetStep <degrees per FRAME>, not per second: the throbber's speed is the frame
 * edge's speed, on purpose (Throbber::AdvanceConcrete). A caller wanting
 * revolutions a second multiplies by the interval they set on RunFrames.
 *
 * Clamped by the setter; the log reports what it settled on rather than what was
 * asked for, because a clamped value that echoes the request is a value you
 * cannot debug.
 */
DEFINE_WORK_FUNC_TYPED(Throbber, SetStep, (float, degrees_per_frame))
{
    (void)ctx;
    self.SetStep(degrees_per_frame);
    ETCS_LOG("Throbber::SetStep", self.Step() << " deg/frame on RID:" << self.getRID());
}

DEFINE_WORK_FUNC_TYPED(Throbber, SetDots, (uint32_t, count))
{
    (void)ctx;
    self.SetDots(count);
    ETCS_LOG("Throbber::SetDots", self.Dots() << " on RID:" << self.getRID());
}

// SetColors <head r g b> <tail r g b> -- the two shades the sweep lerps
// between. Six floats and not two named colours, because a colour is not an
// entity here and inventing one for this would be the only place it existed.
DEFINE_WORK_FUNC_TYPED(Throbber, SetColors,
                       (float, ar), (float, ag), (float, ab),
                       (float, br), (float, bg), (float, bb))
{
    (void)ctx;
    self.SetColors(ar, ag, ab, br, bg, bb);
}

DEFINE_WORK_FUNC_TYPED(Throbber, SetTextColor,
                       (float, r), (float, g), (float, b), (float, a))
{
    (void)ctx;
    self.SetTextColor(r, g, b, a);
}

// SetText <rest of line> -- "ETCS" by default, and an empty argument puts it
// back rather than leaving a throbber with no caption at all.
DEFINE_WORK_FUNC(Throbber, SetText)
{
    (void)ctx;
    self.SetText(data.restAsString());
}

DEFINE_WORK_FUNC_TYPED(Throbber, SetPosition, (int32_t, x), (int32_t, y))
{
    (void)ctx;
    self.SetPosition(x, y);
}

DEFINE_WORK_FUNC_TYPED(Throbber, SetOrder, (int32_t, z))
{
    (void)ctx;
    self.SetOrder(z);
}

DEFINE_WORK_FUNC_TYPED(Throbber, SetHidden, (int32_t, hidden))
{
    (void)ctx;
    self.SetHidden(hidden != 0);
}

// Watch <@entity> <flag> -- shown exactly while that entity carries the
// (lowercase) state tag; see Throbber::Watch. An empty flag unbinds.
DEFINE_WORK_FUNC_TYPED(Throbber, Watch, (ETCS::RID, entity), (std::string, flag))
{
    (void)ctx;
    self.Watch(entity, flag);
    ETCS_LOG("Throbber::Watch", (flag.empty() ? "unbound" : "following '" + flag + "' on RID:" + std::to_string(entity))
                                << " for RID:" << self.getRID());
}

// SetPlate r g b a -- a square behind the ring; alpha 0 (the default) is none.
// CenterOn <@node> -- keep the ring centred on that node as it moves and
// resizes; see Throbber::CenterOn. 0 unbinds.
DEFINE_WORK_FUNC_TYPED(Throbber, CenterOn, (ETCS::RID, node))
{
    (void)ctx;
    self.CenterOn(node);
}

DEFINE_WORK_FUNC_TYPED(Throbber, SetPlate, (float, r), (float, g), (float, b), (float, a))
{
    (void)ctx;
    self.SetPlate(r, g, b, a);
}

DEFINE_WORK_FUNC(Throbber, Delete)
{
    (void)data; (void)ctx;
    self.DeleteConcrete();
}

#endif
