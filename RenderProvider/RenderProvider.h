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

#endif
