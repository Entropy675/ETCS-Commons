#ifndef RENDERPROVIDER_H__
#define RENDERPROVIDER_H__

#define ETCS_DLL_EXPORTS
#include "../../core_defs.h"
#include "../../ontology.h"
#include "Contract_RenderProvider.h"

#include <chrono>
#include <string>
#include <thread>

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

// Source resolution for Blit: a RID in, a Surface_* out, through the
// ontology family aggregate.
//
// This is the general form -- ETCS::resolve_in_family (core/Entity.h) finds
// the entity in the "Surface" family list whoever built it, and hands back
// the correctly-adjusted Surface_* interface pointer. So a script can blit
// from ANY module's surface, not just one RenderProvider spawned, and this
// module needs no compile-time knowledge of what the source concretely is.
//
// It replaces a module-local row scan that existed only because the family
// aggregates were published but never populated -- fixed in core by
// etcs_supertype_fanout, whose own comment carries the history.

// The one lookup that is a single tag rather than a family: Instance is
// flat by design (no ontology supertype but Deletable), so its tag row is
// the only place it appears.
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
// VulkanSurface::SetComposeRoot for why a tree that moves needs the other one.
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
// The frame pump, as a produce/consume pair -- the same shape
// Window.ProduceEvents/ConsumeEvents already uses for input, applied to
// output. This is what lets a renderer run somewhere other than the thread
// that owns the window's poll loop (scripts/render_frames.etcs).
//
// THE SPLIT, and why it is this way round: ProduceFrames is a CLOCK and
// nothing else, ConsumeFrames does every Vulkan call.
//
// The tempting split -- acquire on the produce side, record/submit/present
// on the consume side -- puts two threads on the same VkQueue and
// VkSwapchainKHR, both of which Vulkan requires the application to
// externally synchronise. That buys nothing: the goal is only to get frames
// OFF the poll thread, not to parallelise a single surface's submission. So
// every queue-touching call stays on the consume thread, which makes this
// surface single-threaded from Vulkan's point of view, and the edge carries
// a tick rather than a half-built frame.
//
// Where the thread ledger lands (see render_script_streamed.etcs):
//   produce -> a ThreadPool worker, held for the surface's lifetime
//   consume -> the detached script thread, blocked on the stream
//
// Draws still arrive from whatever thread calls Clear/DrawRect/Blit -- the
// script's -- so the surface's own state is mutex-guarded and Present works
// off a snapshot. See VulkanSurface::PresentConcrete.
struct RenderFrameTick { uint64_t index; };

// Default pacing, in milliseconds, when the stream config says nothing.
// ~60Hz, a placeholder for asking the swapchain about its present mode,
// which is where real pacing belongs.
static constexpr uint32_t RENDER_FRAME_INTERVAL_MS = 16;

// ProduceFrames [<interval_ms>] -- the stream's config buffer carries the
// pacing, and ZERO means unpaced: emit as fast as the edge accepts, and let
// the consumer's back-pressure set the rate.
//
// That mode is the honest way to ask "how fast can this pipeline actually
// go", because the answer is then measured BY the pipeline rather than by a
// clock on one of its calls -- writeRaw blocks exactly when the consumer is
// behind, so ticks completed over an interval IS throughput, with acquire,
// submit and present all inside it. A fixed sleep here would silently cap
// any such measurement at its own frequency, which is what 16ms did.
//
// Paced stays the default because a normal frame loop should not spin a
// pool worker at 100% to draw a canvas nobody is editing.
DEFINE_STREAM_FUNC_PRODUCE(Surface, ProduceFrames)
{
    uint32_t interval_ms = RENDER_FRAME_INTERVAL_MS;
    {
        std::string cfg = data.restAsString();
        if (!cfg.empty())
        {
            try { interval_ms = static_cast<uint32_t>(std::stoul(cfg)); }
            catch (const std::exception&)
            {
                ETCS_LOG("Surface::ProduceFrames", "unreadable interval '" << cfg
                         << "' -- using the " << RENDER_FRAME_INTERVAL_MS << "ms default.");
            }
        }
    }
    ETCS_LOG("Surface::ProduceFrames", "clock started at "
             << (interval_ms == 0 ? std::string("max speed (back-pressure paced)")
                                   : std::to_string(interval_ms) + "ms"));

    // Same wait ProduceEvents does: the surface is spawned and Create()d by
    // the script, and detaching the pump before that has finished would
    // start ticking at a swapchain that does not exist yet.
    while (!self.IsActive())
    {
        if (ctx.isInterrupted() || ctx.isTerminated()) return;
        std::this_thread::yield();
    }

    uint64_t index = 0;
    bool stream_alive = true;

    while (self.IsActive() && stream_alive)
    {
        if (ctx.isInterrupted() || ctx.isTerminated()) break;

        RenderFrameTick tick{ index++ };
        ETCS::Buffer slot;
        slot.writeRaw(&tick, sizeof(tick));

        if (!stream.writeRaw(slot))
        {
            ETCS_LOG("Surface::ProduceFrames", "writeRaw failed -- stream closed at frame " << tick.index);
            stream_alive = false;
            break;
        }
        if (interval_ms != 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
    }

    if (stream.isOpen())
        stream.closeWrite();

    ETCS_LOG("Surface::ProduceFrames", "clock stopped after " << index << " ticks.");
}

DEFINE_STREAM_FUNC_CONSUME(Surface, ConsumeFrames)
{
    (void)data;

    uint64_t presented = 0;
    // Timed from the FIRST tick, not from entry: the producer waits for the
    // surface to go active, so entry-to-first-tick is setup latency, not
    // frame time, and folding it in would drag the rate down by however
    // long the script took to get here.
    std::chrono::steady_clock::time_point first{};

    while (stream.isOpen())
    {
        if (ctx.isInterrupted() || ctx.isTerminated()) break;

        ETCS::Buffer slot;
        if (!stream.readRaw(slot)) break;

        RenderFrameTick tick{};
        slot.readRaw(&tick, sizeof(tick));
        if (presented == 0) first = std::chrono::steady_clock::now();

        // Everything Vulkan happens here, on this one thread. Present pulls
        // the current composition itself -- retained, so a script that drew
        // once keeps being shown rather than blinking out on frame two.
        //
        // Unless a root is bound (Surface.Compose), in which case the tree is
        // re-walked first and the retained list is what that walk produces.
        // The walk is on THIS thread rather than the producer's for the same
        // reason every other Vulkan call is: Blit uploads into mapped staging
        // memory, and that is frame state.
        self.RecomposeBound();
        self.Present();
        ++presented;
    }

    // The throughput number, reported by the side that actually knows it.
    // With an unpaced producer this IS the pipeline's rate and needs no
    // external clock on any single call: writeRaw blocks exactly when this
    // loop falls behind, so frames completed over the interval is what the
    // whole edge -- acquire, record, submit, present -- sustained. Under a
    // paced producer it just reports the pacing back, which is the correct
    // answer to a different question.
    const double secs = (presented > 1)
        ? std::chrono::duration<double>(std::chrono::steady_clock::now() - first).count()
        : 0.0;
    ETCS_LOG("Surface::ConsumeFrames", "stream closed after presenting " << presented
             << " frames" << (secs > 0.0
                 ? " in " + std::to_string(secs) + "s = "
                   + std::to_string(static_cast<double>(presented) / secs) + " fps"
                 : "") << ".");
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

// Radians of view rotation per pixel of pointer movement. The look arrives on
// the same stream as the keys and is applied to whichever camera projects this
// scene -- see Scene3D::PointerDelta on why the angle lives here rather than
// on the camera.
DEFINE_WORK_FUNC_TYPED(Scene3D, SetSensitivity, (float, rad_per_px))
{
    (void)ctx;
    self.SetSensitivity(rad_per_px);
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
             << "  causal-ticks=" << self.CausalTicks()
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
DEFINE_STREAM_FUNC_CONSUME(Scene3D, ConsumeInput)
{
    (void)data;

    ETCS_LOG("Scene3D::ConsumeInput", "WASD edge open on RID:" << self.getRID()
             << " (W/S forward, A/D strafe, Q/E vertical, mouse looks, "
             << self.Speed() << " u/s terminal, damping " << self.Damping() << "/s)");

    // Blocking, and doing nothing but recording. The one thing this loop must
    // not do is integrate: motion is advanced where it is OBSERVED, in
    // Scene3D::Project, on the interval since the last projection (Scene3D.h
    // on why that is the interpolation rather than an approximation of it).
    //
    // Which is also why hasData() is not used here. A cross-tag stream pair
    // resolves to StrategyPipe (core/Entity.h), whose consumer fd is blocking,
    // so hasData is a non-advancing check on the LMAX side and a blocking read
    // on this one -- a drain loop built on it stalls a same-module pair and
    // spins a cross-module one. Blocking readRaw is the shape every consumer
    // in this codebase already uses, and with the integration moved to the
    // observer there is nothing left for a tick here to do.
    while (stream.isOpen())
    {
        if (ctx.isInterrupted() || ctx.isTerminated()) break;

        ETCS::Buffer slot;
        if (!stream.readRaw(slot)) break;

        InputEvent ev{};
        slot.readRaw(&ev, sizeof(InputEvent));

        // Both kinds, one stream, one handler -- which is the reason they
        // share a record (ontology/InputSource.h). A look and a step that
        // arrived together are one intent, and two streams would let them
        // separate by however much two consumers happened to drift.
        if (ev.action == INPUT_MOTION)
        {
            self.PointerDelta(ev.dx, ev.dy);
        }
        else
        {
            if (ev.key == 0) continue;
            if (ev.action == INPUT_DOWN) self.KeyDown(ev.key);
            else                         self.KeyUp(ev.key);
        }

        // One mark per key change, and no polling anywhere: it restarts a
        // pipeline that had gone quiet, and once moving, the motion marks
        // itself at every projection until it settles.
        self.WakeObservers();
    }

    // Held keys are a fact about a stream that no longer exists. Clearing them
    // is what stops a scene drifting forever because the window closed between
    // a key going down and coming up.
    self.ClearHeld();
    ETCS_LOG("Scene3D::ConsumeInput", "input stream closed.");
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

#endif
