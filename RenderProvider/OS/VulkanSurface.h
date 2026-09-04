#ifndef VULKAN_SURFACE_H__
#define VULKAN_SURFACE_H__

#include "../../../ontology.h"
#include "VulkanInstance.h"
#include "VulkanPlatform.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <dlfcn.h>
#include <fstream>
#include <atomic>
#include <vector>

static constexpr uint32_t SURFACE_FRAMES_IN_FLIGHT  = 2;
// Distinct blit sources one surface can hold GPU textures for at once --
// a layer stack, in practice. A fixed ceiling rather than a growing pool:
// overflow logs and drops the blit rather than failing the frame.
static constexpr uint32_t SURFACE_MAX_BLIT_SOURCES = 64;

// VulkanSurface -- the window-bound, presentable surface:
// [Surface + Presentable + Resizable + Deletable].
//
// Owns the VkSurfaceKHR/swapchain/render-pass/framebuffers, TWO fixed
// pipelines (solid-colour rects by push constant, and textured quads for
// Blit), per-frame command buffers + sync objects, and a small cache of
// GPU textures keyed by the RID of whichever image surface was blitted
// from. Spawned as a CHILD of an existing WindowProvider::Window
// (make_typed_child) -- Create() reaches its parent via
// getInterfacePointer("Window")/("Resizable") for the native surface
// handle and initial size, and subscribes to the parent's Resizable to
// recreate the swapchain on resize.
//
// Clear/DrawRect/Blit accumulate into ONE ordered list, not three; a
// layered editor composites back-to-front, so the order calls arrive in
// is the order they must be recorded in. Present is the only call that
// touches the queue.
class VulkanSurface : public SurfaceBase<VulkanSurface>,
                       public PresentableBase<VulkanSurface>,
                       public DeletableBase<VulkanSurface>,
                       public LifecycleBase<VulkanSurface>
{
public:
    // The ordering every Surface owes (Orderable, composed by SurfaceBase).
    // Meaningful for a device-side offscreen target sitting in a stack;
    // degenerate but harmless for a window's swapchain, which is normally
    // the only surface of its kind in the picture.
    int32_t m_order = 0;
    bool operator<(const VulkanSurface& o) const { return m_order < o.m_order; }
    WIRE_TYPE_IDENTITY(VulkanSurface);

    VulkanSurface()  = default;
    ~VulkanSurface() { teardown(); }

    // instance: RID of an already-Create()'d VulkanInstance. shader_dir:
    // cwd-relative directory holding rect.vert.spv/rect.frag.spv (see
    // RenderProvider.h's own comment on why this isn't self-locating yet).
    bool Create(VulkanInstance* instance, const std::string& shader_dir)
    {
        if (m_swapchain) return true; // idempotent, same convention as Window::CreateWindowConcrete
        m_instance = instance;
        if (!m_instance)
        {
            ETCS_LOG("VulkanSurface", "Create called with no VulkanInstance.");
            return false;
        }

        ETCS::Entity* parent = this->getParent();
        if (!parent)
        {
            ETCS_LOG("VulkanSurface", "Create called with no parent -- must be spawned as a child of a Window.");
            return false;
        }
        void* winRaw = parent->getInterfacePointer(ETCS::Buffer("Window"));
        Window_* win = static_cast<Window_*>(winRaw);
        if (!win)
        {
            ETCS_LOG("VulkanSurface", "Parent has no Window interface pointer.");
            return false;
        }
        void* resizableRaw = parent->getInterfacePointer(ETCS::Buffer("Resizable"));
        m_parentResizable = static_cast<Resizable_*>(resizableRaw);
        if (!m_parentResizable)
        {
            ETCS_LOG("VulkanSurface", "Parent has no Resizable interface pointer.");
            return false;
        }

        m_surface = CreateSurfaceFromNativeHandle(m_instance->GetInstance(), win->GetNativeSurfaceHandle());
        if (m_surface == VK_NULL_HANDLE) return false;

        VkBool32 presentSupported = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(m_instance->GetPhysicalDevice(), m_instance->GetQueueFamily(),
                                              m_surface, &presentSupported);
        if (!presentSupported)
        {
            ETCS_LOG("VulkanSurface", "Selected queue family cannot present to this surface.");
            teardown();
            return false;
        }

        if (!createShaderPipelineIndependentState()) { teardown(); return false; }
        if (!loadPipeline(shader_dir)) { teardown(); return false; }
        if (!createSyncObjects()) { teardown(); return false; }

        // OnResize fires immediately on registration with the CURRENT size
        // (Resizable_::OnResize's existing semantics) -- this doubles as
        // the initial swapchain build, so no separate first-build call.
        m_parentResizable->OnResize([this](WindowSize sz) { this->queueResize(sz); }, 0);

        this->addTag("active");
        return m_swapchain != VK_NULL_HANDLE;
    }

    // Mirrors GLFWWindow::IsActive and VulkanInstance::IsActive -- the
    // "active" tag is added at the end of Create and removed in teardown,
    // so this is the one predicate the frame clock (ProduceFrames,
    // RenderProvider.h) can wait on before ticking at a swapchain that may
    // not exist yet, and stop on when the surface goes away.
    bool IsActive() const { return this->hasTag("active"); }

    // Every Surface_ entry point goes through this before touching a Vulkan
    // handle. Create() can fail HALFWAY -- it assigns m_instance early, then
    // gives up at loadPipeline if the SPIR-V is not where it was told to
    // look -- which leaves an object that has a device but no pipeline, no
    // descriptor pool and no swapchain. Present() alone used to check
    // (m_swapchain), so Clear/DrawRect/Blit walked straight into that state:
    // Blit reached vkAllocateDescriptorSets with a VK_NULL_HANDLE pool and
    // the driver dereferenced it. A segfault reachable from a script, from
    // nothing worse than a wrong working directory.
    //
    // Complains ONCE. A frame loop calling a dead surface would otherwise
    // bury the real error -- the failed Create above it -- under thousands
    // of identical lines.
    bool ready(const char* what) const
    {
        // Dead is checked FIRST and separately from never-created: the two are
        // different failures with different causes, and a surface that worked
        // until its window vanished should say so rather than claim Create was
        // never called.
        if (m_dead)
        {
            if (!m_complained)
            {
                m_complained = true;
                ETCS_LOG("VulkanSurface", what << " on a surface whose window is gone. "
                         "Ignoring this and every later call on RID:" << getRID() << ".");
            }
            return false;
        }
        if (m_swapchain != VK_NULL_HANDLE) return true;
        if (!m_complained)
        {
            m_complained = true;
            ETCS_LOG("VulkanSurface", what << " on a surface that is not created -- Create() "
                     "either was never called or failed (look for an earlier error above this "
                     "one). Ignoring this and every later call on RID:" << getRID() << ".");
        }
        return false;
    }

    // --- Surface_ dispatch (SurfaceBase.h) ---

    // CLEAR IS THE COMPOSITION BOUNDARY, and it is the only one.
    //
    // It used to be that ANY draw call restarted the composition if the last
    // one had been presented -- Clear, DrawRect and Blit alike. That is fine
    // while a composition is one or two calls, and wrong the moment it is
    // hundreds: a Present landing mid-batch marks the composition presented,
    // and the very next DrawRect in the SAME batch then throws away
    // everything emitted before it. What reaches the screen is the tail of
    // the batch, cut at a different point every frame -- shapes that appear
    // for the frame that proposed them and are gone by the next.
    //
    // Reproduced as soon as PolygonDrawable2D arrived, because a scanline
    // fill emits one DrawRect per row and a scene is easily a thousand calls
    // against a 60Hz presenter on another thread.
    //
    // So the boundary is explicit and it is this call. Clear starts a
    // picture; DrawRect and Blit only ever APPEND to it; Present snapshots
    // whatever is there. A caller that wants a fresh picture says so, which
    // it was already saying, and a batch can now take as long as it likes
    // without a concurrent presenter being able to truncate it.
    void ClearConcrete(float r, float g, float b, float a) override
    {
        if (!ready("Clear")) return;
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_pendingDraws.clear();
        m_composed   = false;
        m_clearColor = { r, g, b, a };
    }

    /*
 * COMPOSE ROOT -- the other half of the retained model, for trees that move.
 *
 * Retained composition answers "a script draws once and blocks in Run", and
 * it is right for that: the picture stays up rather than blinking out on
 * frame two. What it cannot answer is a tree that CHANGES, because retaining
 * the draw calls is not the same as re-walking the thing that produced them.
 * A camera whose scene moved has a new image to make, and nothing in a list
 * of past calls will ask it to.
 *
 * So a surface may instead be handed the RID of a Drawable root, and the
 * frame edge re-walks it per frame (ConsumeFrames, RenderProvider.h). That is
 * not the expensive option it sounds like: the walk is exactly where every
 * dirty flag in this system finally pays off. A settled tree costs one
 * DrawInto per node, no recomposition, no projection, and BlitConcrete's own
 * TakeDirty declines the upload -- so a still frame is a snapshot of
 * unchanged descriptor sets, and a frame where one node moved re-walks only
 * the path that node marked.
 *
 * The two modes are exclusive by construction rather than by a flag: binding
 * a root means every frame starts with Clear, so anything a script retained
 * is discarded on the next tick. Bind zero to go back to retained.
 */
    void SetComposeRoot(ETCS::RID root)
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_composeRoot = root;
    }
    ETCS::RID ComposeRoot() const { return m_composeRoot; }

    /*
 * The frame rate this surface is actually sustaining, as a rolling average.
 *
 * Sampled at PRESENT, which is the only instant that means anything: it is
 * the point the frame reached the screen, so the interval between two of them
 * is a frame, with acquire, record, submit and the walk all inside it. A rate
 * computed anywhere else is a rate for part of the pipeline.
 *
 * Exponential rather than a window, because a window needs a buffer and a
 * decision about how long it is, and the only consumer is a human reading a
 * number off the screen. The smoothing constant is what stops it flickering
 * between two integers while saying nothing about the actual variance.
 */
    float Fps() const { return m_fps; }

    // Re-walk the bound root into a fresh composition. Returns false when
    // nothing is bound -- the retained path -- or when the root has stopped
    // resolving, which is how a deleted scene stops being drawn instead of
    // being dereferenced.
    /*
 * Lifecycle_: let go of the tree, whichever way this surface is dying.
 *
 * THE COMPOSE ROOT IS THE WHOLE REASON this type claims the family. A bound
 * root means the frame edge re-walks somebody else's entity tree every tick
 * -- and when a closure ends, that tree is being reclaimed while the walk is
 * still running. Unbinding here is the difference between a frame edge that
 * stops looking and one that is racing the arena for the same nodes.
 *
 * Marking it dead as well, so any tick already in flight takes ready()'s
 * refusal instead of touching a swapchain about to be destroyed. Neither of
 * these could be done from ~VulkanSurface: by then the frame thread has
 * already had its chance to be wrong.
 */
    void ReleaseConcrete()
    {
        ETCS_LOG("VulkanSurface", "release: unbinding the compose root and marking the "
                 "surface dead before teardown (RID:" << getRID() << ").");
        SetComposeRoot(0);
        m_dead = true;
    }

    // Has this surface stopped being a thing worth walking a tree for? The
    // question the frame edge asks BEFORE the walk, as against ready(), which
    // asks whether a Vulkan call may proceed. Released is the graph answer and
    // dead is the window answer; a walk is invalid under either.
    bool Retired() const { return m_dead || Released(); }

    bool RecomposeBound()
    {
        /*
     * REFUSED ONCE THE SURFACE IS RETIRED, and this is a crash guard rather
     * than an optimisation.
     *
     * Recomposing resolves a root RID and dispatches virtuals down somebody
     * else's entity tree. That is only meaningful while the graph is intact,
     * and on the teardown paths it is not: an X connection dropping calls
     * exit() from wherever the error surfaced, static destructors then run --
     * on a POOL WORKER, since that is where the exit came from -- and the
     * frame edge is still going round while the objects underneath it come
     * apart. The symptom is "pure virtual method called": a dispatch that
     * reached an object whose derived part has already been destroyed.
     *
     * Checking here rather than only in Present is the point. Present was
     * already guarded (ready(), m_dead) and it was not enough, because the
     * walk happens FIRST and it is the walk that touches other entities.
     *
     * This closes the window between a surface being retired and the frame
     * edge noticing. It does not close the general case -- nothing can stop a
     * body mid-walk, which is exactly the gap IWireThread::Halt is declared
     * for (core/InterfaceWire.h) -- so this is a guard at one known site, not
     * the shutdown ordering the pool still lacks.
     */
        if (Retired()) return false;

        ETCS::RID root_rid;
        std::array<float, 4> clear;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            root_rid = m_composeRoot;
            clear    = m_clearColor;
        }
        if (root_rid == 0) return false;

        /*
     * HELD, not merely resolved, and this walk is the reason the hold exists.
     *
     * Re-resolving every frame makes a composed reference safe against a root
     * that has ALREADY gone. It cannot make it safe against one going now,
     * because a bare pointer is an answer about the instant it was handed
     * over and this walk lasts a whole tree. Deleting a bound compositor mid
     * frame therefore freed the subtree out from under DrawInto -- reproduced
     * about one run in three, and gone under a debugger, which is exactly the
     * shape of a race with a delete.
     *
     * The hold makes the Delete event wait for this walk instead
     * (Entity::tryHold, core/Entity.h). Falsy now covers being deleted RIGHT
     * NOW as well as being gone already, and both mean the same thing here:
     * unbind and show what was last composed.
     *
     * NOTHING INSIDE THIS SCOPE MAY EMIT AN ORDERED EVENT -- the Delete
     * waiting on the hold may be ahead of it in the queue. DrawInto draws;
     * that is the whole of what is allowed.
     */
        ETCS::Held<Drawable_> root = ETCS::resolve_held<Drawable_>("Drawable", root_rid);
        if (!root)
        {
            ETCS_LOG("VulkanSurface", "compose root RID:" << root_rid
                     << " is gone or going -- unbinding.");
            // Outside the hold by construction: it is already falsy, so there
            // is nothing held and SetComposeRoot is free to take its lock.
            SetComposeRoot(0);
            return false;
        }

        // Clear first, and through the ordinary call: it is the composition
        // boundary (see ClearConcrete), so starting the frame with it is what
        // makes this a new picture rather than an append to the last one.
        ClearConcrete(clear[0], clear[1], clear[2], clear[3]);
        root->DrawInto(this);
        return true;
    }

    void DrawRectConcrete(int32_t x, int32_t y, uint32_t w, uint32_t h,
                           float r, float g, float b, float a) override
    {
        if (!ready("DrawRect")) return;
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_composed = false;          // appends only -- see ClearConcrete
        m_pendingDraws.push_back({ PendingDraw::Kind::Rect, x, y, w, h, r, g, b, a, 0 });
    }

    // The source is reached as Pixels_ (ontology/Pixels.h), never as a
    // concrete type -- this is the whole reason Pixels is its own family.
    // A PintaProvider layer, a test image surface, anything that owns CPU
    // pixels and registers the interface pointer can be blitted here with
    // no compile-time relationship to this module.
    //
    // w/h of 0 mean "the source's own size", which is what a 1:1 canvas
    // composite wants and saves every caller restating it.
    void BlitConcrete(Surface_* source, int32_t x, int32_t y,
                       uint32_t w, uint32_t h, float opacity) override
    {
        if (!ready("Blit")) return;
        if (!source) { ETCS_LOG("VulkanSurface", "Blit called with no source."); return; }
        // Whole body under the lock, not just the push_back: this mutates
        // m_textures and writes into mapped staging memory, both of which
        // the frame consumer reads (ConsumeFrames, RenderProvider.h).
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_composed = false;          // appends only -- see ClearConcrete
        // Every Surface_ is an Entity (virtually), so the source's own
        // identity and its other interfaces are both reachable from here.
        // A device-side offscreen source would be read by image copy at this
        // point instead; today only CPU-backed sources can be uploaded.
        Pixels_* px = static_cast<Pixels_*>(source->getInterfacePointer(ETCS::Buffer("Pixels")));
        if (!px)
        {
            ETCS_LOG("VulkanSurface", "Blit source RID:" << source->getRID()
                     << " has no Pixels interface -- only a CPU-backed surface can be blitted from yet.");
            return;
        }
        if (px->PixelWidth() == 0 || px->PixelHeight() == 0) return;
        if (w == 0) w = px->PixelWidth();
        if (h == 0) h = px->PixelHeight();

        const ETCS::RID rid = source->getRID();
        if (!ensureTexture(rid, *px)) return;

        // TakeDirty CONSUMES the flag, so a layer blitted twice in one
        // frame uploads once. everUploaded covers the first blit of an
        // image whose writer never marked it dirty.
        BlitTexture& tex = m_textures[rid];
        if (px->TakeDirty() || !tex.everUploaded)
        {
            std::memcpy(tex.stagingMapped, px->PixelData(), px->PixelBytes());
            tex.needsUpload = true;
        }

        m_pendingDraws.push_back({ PendingDraw::Kind::Blit, x, y, w, h, 0.0f, 0.0f, 0.0f, opacity, rid });
    }

    // --- Presentable_ dispatch (PresentableBase.h) ---

    // Present is the ONLY call here that touches the queue, and with the
    // frame edge (RenderProvider.h's ProduceFrames/ConsumeFrames) it runs
    // on a different thread from the Clear/DrawRect/Blit calls feeding it.
    // So it takes a SNAPSHOT of everything it needs under the state lock and
    // then does all the Vulkan work without holding it -- vkQueuePresentKHR
    // can block for most of a frame on a vsync'd present mode, and stalling
    // the caller's DrawRect for that long would make the lock the slowest
    // thing in the editor.
    void PresentConcrete() override
    {
        if (!ready("Present")) return;

        /*
 * THE RESIZE LANDS HERE, on this thread, and that is the whole point.
 *
 * The resize callback used to call recreateSwapchain directly -- which
 * meant vkDeviceWaitIdle, destroying the swapchain, its image views and
 * its framebuffers, all running on the POLL thread while this thread was
 * mid-frame using them. It contradicts this file's own stated invariant
 * three lines above ("Present is the ONLY call here that touches the
 * queue"), and it only ever fires when something actually delivers resize
 * events: a window manager mapping, focusing or resizing the window. A
 * headless Xvfb with no WM never sends one, which is exactly why it
 * survived every test here and shows up on a real desktop as a picture
 * that will not stay on screen, or a segfault out of a script that polls.
 *
 * So the callback now records the extent and this thread acts on it, at a
 * frame boundary, where no command buffer is in flight. Every Vulkan call
 * on this surface is back on one thread, which is what the design said it
 * wanted.
 *
 * A surface with no presenter never processes a resize -- correct, and not
 * a gap: a surface nothing presents is showing nothing to resize.
 */
        if (m_resizePending.exchange(false, std::memory_order_acquire))
        {
            WindowSize sz;
            {
                std::lock_guard<std::mutex> lock(m_stateMutex);
                sz = m_pendingExtent;
            }
            recreateSwapchain(sz);
            if (m_swapchain == VK_NULL_HANDLE) return;
        }

        FrameSnapshot frame;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            if (m_extent.width == 0 || m_extent.height == 0)   // minimized
            {
                m_composed = true;
                return;
            }
            takeSnapshotLocked(frame);
        }

        VkFence frameFence = m_inFlight[m_currentFrame];
        vkWaitForFences(m_instance->GetDevice(), 1, &frameFence, VK_TRUE, UINT64_MAX);

        uint32_t imageIndex = 0;
        VkResult acquire = vkAcquireNextImageKHR(m_instance->GetDevice(), m_swapchain, UINT64_MAX,
                                                  m_imageAvailable[m_currentFrame], VK_NULL_HANDLE, &imageIndex);
        if (acquire == VK_ERROR_OUT_OF_DATE_KHR)
        {
            recreateSwapchain({ m_extent.width, m_extent.height });
            return;   // snapshot is dropped; the retained composition redraws next tick
        }
        if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR)
        {
            /*
     * AN EXPLICIT FAIL STATE, because the alternative is an infinite loop
     * that looks like a hang.
     *
     * VK_ERROR_SURFACE_LOST_KHR means the window this surface was created
     * from is gone -- destroyed out from under us, or its display
     * disconnected. Nothing about that recovers: the swapchain cannot be
     * rebuilt on a surface that no longer exists, so every subsequent
     * acquire fails the same way. Returning and letting the frame edge tick
     * again produces exactly what it produced here, several times a second,
     * for as long as the process lives -- a log filling with one line and a
     * program that will not exit.
     *
     * So the surface goes NOT READY. ready() then refuses every later call
     * with its own one-shot complaint, the frame edge's Present becomes a
     * no-op, and the script's own teardown proceeds instead of racing a loop
     * that will never finish. A dead surface reporting that it is dead is
     * strictly better than a live one that can never draw.
     *
     * Every OTHER acquire failure is left alone: OUT_OF_DATE and SUBOPTIMAL
     * are handled above as ordinary resizes, and a transient device error is
     * worth one dropped frame rather than a permanent shutdown.
     */
            if (acquire == VK_ERROR_SURFACE_LOST_KHR)
            {
                ETCS_LOG("VulkanSurface", "vkAcquireNextImageKHR: SURFACE LOST -- the window "
                         "this surface was made from is gone. Marking the surface dead; it "
                         "cannot be rebuilt, and retrying would spin here forever.");
                m_dead = true;
                SetComposeRoot(0);
                return;
            }
            ETCS_LOG("VulkanSurface", "vkAcquireNextImageKHR failed: " << acquire
                     << " -- dropping this frame.");
            return;
        }

        vkResetFences(m_instance->GetDevice(), 1, &frameFence);

        VkCommandBuffer cmd = m_commandBuffers[m_currentFrame];
        vkResetCommandBuffer(cmd, 0);
        recordFrame(cmd, imageIndex, frame);

        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit{};
        submit.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.waitSemaphoreCount   = 1;
        submit.pWaitSemaphores      = &m_imageAvailable[m_currentFrame];
        submit.pWaitDstStageMask    = &waitStage;
        submit.commandBufferCount   = 1;
        submit.pCommandBuffers      = &cmd;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores    = &m_renderFinished[m_currentFrame];
        vkQueueSubmit(m_instance->GetQueue(), 1, &submit, frameFence);

        VkPresentInfoKHR present{};
        present.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores    = &m_renderFinished[m_currentFrame];
        present.swapchainCount     = 1;
        present.pSwapchains        = &m_swapchain;
        present.pImageIndices      = &imageIndex;
        VkResult presentResult = vkQueuePresentKHR(m_instance->GetQueue(), &present);
        if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR)
            recreateSwapchain({ m_extent.width, m_extent.height });

        m_currentFrame = (m_currentFrame + 1) % SURFACE_FRAMES_IN_FLIGHT;
        notePresent();
    }

    // --- Resizable_ dispatch (ResizableBase, composed into SurfaceBase) ---

    WindowSize GetSizeConcrete() override
    {
        return { m_extent.width, m_extent.height };
    }

    // --- Deletable_ dispatch ---

    bool DeleteConcrete() override
    {
        std::string conjugate_key = getSourceModule().toString() + ":" + getSourceTag().toString();
        ETCS_LOG("VulkanSurface", "firing self-DestroyEvent for RID:" << getRID());
        return ETCS::DestroyEvent{conjugate_key.c_str(), this}();
    }

    // Manual-verification convenience ONLY -- not a primitive downstream
    // modules should call. .etcs scripts have no loop construct today
    // (see RenderProvider.h's own comment), so a per-frame Clear/DrawRect/
    // Present cycle has to happen inside one work func for now.
    void RunDemo(ETCS::Entity* windowEntity, uint32_t frameCount)
    {
        Window_* win = static_cast<Window_*>(windowEntity->getInterfacePointer(ETCS::Buffer("Window")));
        if (!win) { ETCS_LOG("VulkanSurface", "RunDemo: parent has no Window interface pointer."); return; }

        for (uint32_t i = 0; i < frameCount && !win->ShouldClose(); ++i)
        {
            win->PollEvents();
            ClearConcrete(0.05f, 0.05f, 0.08f, 1.0f);
            DrawRectConcrete(40, 40, 200, 120, 0.85f, 0.2f, 0.2f, 1.0f);
            DrawRectConcrete(260, 160, 150, 150, 0.2f, 0.7f, 0.3f, 1.0f);
            PresentConcrete();
        }
    }

private:
    // ONE list for both primitives, in call order -- see this class's own
    // header comment for why they cannot be two lists.
    struct PendingDraw
    {
        enum class Kind { Rect, Blit } kind;
        int32_t   x, y;
        uint32_t  w, h;
        float     r, g, b, a;   // Rect: colour. Blit: `a` carries opacity.
        ETCS::RID source;       // Blit only; 0 for a rect.
    };

    // What one frame needs, copied out from under the state lock so the
    // Vulkan work can run without holding it. Everything here is by value or
    // a handle whose lifetime the surface owns for the whole frame.
    struct FrameDraw
    {
        PendingDraw::Kind kind;
        int32_t   x, y;
        uint32_t  w, h;
        float     r, g, b, a;
        VkDescriptorSet set;    // Blit only; resolved while locked
    };
    struct UploadJob
    {
        VkImage  image;
        VkBuffer staging;
        uint32_t w, h;
        bool     firstUpload;   // UNDEFINED vs SHADER_READ_ONLY as the old layout
    };
    struct FrameSnapshot
    {
        std::array<float, 4>   clearColor{ 0.0f, 0.0f, 0.0f, 1.0f };
        std::vector<FrameDraw> draws;
        std::vector<UploadJob> uploads;
    };

    struct RectPushConstants { float rect[4]; float color[4]; };
    struct BlitPushConstants { float rect[4]; float opacity; float _pad[3]; };

    // GPU-side mirror of one image surface's pixels, kept alive across
    // frames and reused: uploading a full layer every frame is exactly the
    // cost the dirty flag exists to avoid. Keyed by the source's RID, which
    // is stable for that entity's lifetime.
    struct BlitTexture
    {
        VkImage         image        = VK_NULL_HANDLE;
        VkDeviceMemory  memory       = VK_NULL_HANDLE;
        VkImageView     view         = VK_NULL_HANDLE;
        VkBuffer        staging      = VK_NULL_HANDLE;
        VkDeviceMemory  stagingMem   = VK_NULL_HANDLE;
        void*           stagingMapped = nullptr;   // persistently mapped
        VkDescriptorSet set          = VK_NULL_HANDLE;
        uint32_t        w = 0, h = 0;
        bool            needsUpload  = false;
        bool            everUploaded = false;
    };

    VulkanInstance* m_instance         = nullptr;
    Resizable_*     m_parentResizable  = nullptr;

    VkSurfaceKHR   m_surface   = VK_NULL_HANDLE;
    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    VkFormat       m_format    = VK_FORMAT_UNDEFINED;
    VkExtent2D     m_extent    = { 0, 0 };
    std::vector<VkImage>       m_images;
    std::vector<VkImageView>   m_imageViews;
    std::vector<VkFramebuffer> m_framebuffers;

    VkRenderPass     m_renderPass     = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline       m_pipeline       = VK_NULL_HANDLE;

    // Blit path: its own pipeline (same render pass, different shaders and
    // a descriptor set), plus the sampler/pool/layout it binds through.
    // The pipeline is swapchain-dependent like the rect one; the layout,
    // sampler and pool are not, so they live and die with the object.
    VkPipelineLayout      m_blitPipelineLayout = VK_NULL_HANDLE;
    VkPipeline            m_blitPipeline       = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_blitSetLayout      = VK_NULL_HANDLE;
    VkDescriptorPool      m_blitDescriptorPool = VK_NULL_HANDLE;
    VkSampler             m_blitSampler        = VK_NULL_HANDLE;
    VkShaderModule        m_blitVertShader     = VK_NULL_HANDLE;
    VkShaderModule        m_blitFragShader     = VK_NULL_HANDLE;
    std::unordered_map<ETCS::RID, BlitTexture> m_textures;

    std::array<VkCommandBuffer, SURFACE_FRAMES_IN_FLIGHT> m_commandBuffers{};
    std::array<VkSemaphore, SURFACE_FRAMES_IN_FLIGHT>     m_imageAvailable{};
    std::array<VkSemaphore, SURFACE_FRAMES_IN_FLIGHT>     m_renderFinished{};
    std::array<VkFence, SURFACE_FRAMES_IN_FLIGHT>         m_inFlight{};
    uint32_t m_currentFrame = 0;

    // Guards m_clearColor / m_pendingDraws / m_composed / m_textures and the
    // mapped staging memory behind them. Held only for CPU work: see
    // PresentConcrete for why it is never held across a submit or present.
    std::mutex                m_stateMutex;
    mutable bool              m_complained = false;   // ready()'s one-shot
    // Set once, never cleared: a lost surface cannot come back, and pretending
    // it might is what turns one failure into an endless retry loop.
    bool                      m_dead       = false;
    std::array<float, 4>      m_clearColor = { 0.0f, 0.0f, 0.0f, 1.0f };
    std::vector<PendingDraw>  m_pendingDraws;
    // True once a composition has been presented and nothing new has been
    // drawn since. See ClearConcrete for what starts a composition.
    bool                      m_composed   = false;
    // Zero means the retained model. See SetComposeRoot.
    ETCS::RID                 m_composeRoot = 0;
    float                     m_fps = 0.0f;
    bool                      m_fpsPrimed = false;
    std::chrono::steady_clock::time_point m_lastPresent{};

    // Called at the end of every present. Not under the state lock: it is
    // touched by the frame thread only, and a reader getting a stale float is
    // reading a frame rate.
    void notePresent()
    {
        const auto now = std::chrono::steady_clock::now();
        if (m_fpsPrimed)
        {
            const float dt = std::chrono::duration<float>(now - m_lastPresent).count();
            if (dt > 0.0f)
            {
                const float inst = 1.0f / dt;
                m_fps = (m_fps <= 0.0f) ? inst : (m_fps * 0.9f + inst * 0.1f);
            }
        }
        m_lastPresent = now;
        m_fpsPrimed = true;
    }

    // Resize hand-off: written by the poll thread, acted on by the frame
    // thread. See PresentConcrete.
    std::atomic<bool>         m_resizePending{false};
    WindowSize                m_pendingExtent{0, 0};   // guarded by m_stateMutex
    bool                      m_builtOnce  = false;

    // --- setup helpers ---

    // Command pool comes from VulkanInstance -- allocated once here.
    bool createShaderPipelineIndependentState()
    {
        VkCommandBufferAllocateInfo alloc{};
        alloc.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc.commandPool        = m_instance->GetCommandPool();
        alloc.level               = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = SURFACE_FRAMES_IN_FLIGHT;
        if (vkAllocateCommandBuffers(m_instance->GetDevice(), &alloc, m_commandBuffers.data()) != VK_SUCCESS)
        {
            ETCS_LOG("VulkanSurface", "vkAllocateCommandBuffers failed.");
            return false;
        }
        return true;
    }

    bool createSyncObjects()
    {
        VkSemaphoreCreateInfo semInfo{};
        semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

        for (uint32_t i = 0; i < SURFACE_FRAMES_IN_FLIGHT; ++i)
        {
            if (vkCreateSemaphore(m_instance->GetDevice(), &semInfo, nullptr, &m_imageAvailable[i]) != VK_SUCCESS ||
                vkCreateSemaphore(m_instance->GetDevice(), &semInfo, nullptr, &m_renderFinished[i]) != VK_SUCCESS ||
                vkCreateFence(m_instance->GetDevice(), &fenceInfo, nullptr, &m_inFlight[i]) != VK_SUCCESS)
            {
                ETCS_LOG("VulkanSurface", "Sync object creation failed.");
                return false;
            }
        }
        return true;
    }

    static std::vector<uint32_t> readSpirv(const std::string& path)
    {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f) return {};
        std::streamsize size = f.tellg();
        if (size <= 0 || (size % 4) != 0) return {};
        f.seekg(0);
        std::vector<uint32_t> buf(static_cast<size_t>(size) / 4);
        if (!f.read(reinterpret_cast<char*>(buf.data()), size)) return {};
        return buf;
    }

    VkShaderModule loadShaderModule(const std::string& path)
    {
        std::vector<uint32_t> code = readSpirv(path);
        if (code.empty())
        {
            ETCS_LOG("VulkanSurface", "Failed to read SPIR-V: " << path);
            return VK_NULL_HANDLE;
        }
        VkShaderModuleCreateInfo ci{};
        ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        ci.codeSize = code.size() * sizeof(uint32_t);
        ci.pCode    = code.data();
        VkShaderModule mod = VK_NULL_HANDLE;
        if (vkCreateShaderModule(m_instance->GetDevice(), &ci, nullptr, &mod) != VK_SUCCESS)
        {
            ETCS_LOG("VulkanSurface", "vkCreateShaderModule failed for " << path);
            return VK_NULL_HANDLE;
        }
        return mod;
    }

    // Loads the fixed rect pipeline's shaders and pipeline layout -- the
    // pipeline OBJECT itself is built (and rebuilt) in
    // buildSwapchainDependentPipeline, since it's tied to the render
    // pass, which is tied to the swapchain's format/extent.
    /*
 * WHERE THE SHADERS ARE, decided by the module rather than by the caller's
 * working directory.
 *
 * The shader path used to be whatever the script passed, resolved against the
 * process's cwd -- so `view.Create(@gpu, modules/RenderProvider/shaders/)`
 * worked from the ETCS root and failed from anywhere else, including from
 * bin/. And it failed QUIETLY, in the sense that matters: two "Failed to read
 * SPIR-V" lines scroll past during startup, Create returns false, ready()
 * then suppresses every later call on the surface, nothing is ever presented,
 * and what the user sees is a window that never gets painted -- black under a
 * bare X server, WHITE under a compositor. Reproduced exactly that way, and
 * it is the single most likely explanation for a "blank window" report.
 *
 * A module's shaders ship with the module, so the module can find them.
 * dladdr gives this .so's own path at runtime; the candidates below are that
 * directory and the two layouts this repo actually uses, with the caller's
 * argument kept as a first-choice override for a deployment that puts them
 * somewhere else entirely.
 *
 * Loud either way: the chosen directory is logged, and a total miss lists
 * every path tried rather than leaving the reader to guess which cwd it
 * wanted.
 */
    static bool shader_dir_has_pipeline(const std::string& dir)
    {
        std::ifstream probe(dir + "rect.vert.spv", std::ios::binary);
        return probe.good();
    }

    static std::string own_module_dir()
    {
        Dl_info info{};
        // Any symbol defined in THIS shared object will do; a static member
        // function of this class is one that certainly is.
        if (dladdr(reinterpret_cast<const void*>(&VulkanSurface::own_module_dir), &info) == 0
            || info.dli_fname == nullptr)
            return "";
        std::string path = info.dli_fname;
        const size_t slash = path.find_last_of('/');
        return (slash == std::string::npos) ? "" : path.substr(0, slash + 1);
    }

    static std::string resolve_shader_dir(const std::string& requested)
    {
        std::vector<std::string> tried;
        auto consider = [&tried](std::string dir) -> std::string
        {
            if (dir.empty()) return "";
            if (dir.back() != '/') dir += '/';
            tried.push_back(dir);
            return shader_dir_has_pipeline(dir) ? dir : std::string();
        };

        const std::string so_dir = own_module_dir();
        for (const std::string& candidate : {
                 requested,                                        // the caller's override
                 so_dir + "shaders",                               // beside the .so
                 so_dir + "../modules/RenderProvider/shaders",     // bin/ -> repo layout
                 std::string("modules/RenderProvider/shaders"),    // cwd = ETCS root
                 std::string("../modules/RenderProvider/shaders")  // cwd = bin/
             })
        {
            const std::string hit = consider(candidate);
            if (!hit.empty())
            {
                ETCS_LOG("VulkanSurface", "shaders: " << hit);
                return hit;
            }
        }

        std::string all;
        for (const std::string& t : tried) { all += "\n    "; all += t; }
        ETCS_LOG("VulkanSurface", "no SPIR-V found. Tried:" << all
                 << "\n  The surface will not be created, and nothing will be "
                    "presented into its window.");
        return "";
    }

    bool loadPipeline(const std::string& shader_dir)
    {
        const std::string dir = resolve_shader_dir(shader_dir);
        if (dir.empty()) return false;
        m_vertShader = loadShaderModule(dir + "rect.vert.spv");
        m_fragShader = loadShaderModule(dir + "rect.frag.spv");
        if (!m_vertShader || !m_fragShader) return false;

        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pushRange.offset     = 0;
        pushRange.size       = sizeof(RectPushConstants);

        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges    = &pushRange;
        if (vkCreatePipelineLayout(m_instance->GetDevice(), &layoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS)
        {
            ETCS_LOG("VulkanSurface", "vkCreatePipelineLayout failed.");
            return false;
        }

        return loadBlitPipeline(dir);
    }

    // The Blit path's swapchain-INDEPENDENT half: shaders, descriptor set
    // layout, sampler, pool, pipeline layout. The pipeline object itself is
    // built with the rect one in createGraphicsPipeline, since both hang off
    // the render pass.
    bool loadBlitPipeline(const std::string& dir)
    {
        m_blitVertShader = loadShaderModule(dir + "blit.vert.spv");
        m_blitFragShader = loadShaderModule(dir + "blit.frag.spv");
        if (!m_blitVertShader || !m_blitFragShader) return false;

        VkDescriptorSetLayoutBinding binding{};
        binding.binding         = 0;
        binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo setLayoutInfo{};
        setLayoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        setLayoutInfo.bindingCount = 1;
        setLayoutInfo.pBindings    = &binding;
        if (vkCreateDescriptorSetLayout(m_instance->GetDevice(), &setLayoutInfo, nullptr, &m_blitSetLayout) != VK_SUCCESS)
        {
            ETCS_LOG("VulkanSurface", "vkCreateDescriptorSetLayout failed.");
            return false;
        }

        // NEAREST, not LINEAR: this is a pixel editor's canvas. Filtering a
        // layer on the way to the screen would show the user something
        // other than the pixels they are editing.
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter    = VK_FILTER_NEAREST;
        samplerInfo.minFilter    = VK_FILTER_NEAREST;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.borderColor  = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        samplerInfo.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        if (vkCreateSampler(m_instance->GetDevice(), &samplerInfo, nullptr, &m_blitSampler) != VK_SUCCESS)
        {
            ETCS_LOG("VulkanSurface", "vkCreateSampler failed.");
            return false;
        }

        // One set per distinct blit source ever seen by this surface.
        // SURFACE_MAX_BLIT_SOURCES is a hard ceiling rather than a growing
        // pool: exceeding it logs and drops the blit instead of failing the
        // frame, and the number is generous for a layer stack.
        VkDescriptorPoolSize poolSize{};
        poolSize.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSize.descriptorCount = SURFACE_MAX_BLIT_SOURCES;

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets       = SURFACE_MAX_BLIT_SOURCES;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes    = &poolSize;
        poolInfo.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        if (vkCreateDescriptorPool(m_instance->GetDevice(), &poolInfo, nullptr, &m_blitDescriptorPool) != VK_SUCCESS)
        {
            ETCS_LOG("VulkanSurface", "vkCreateDescriptorPool failed.");
            return false;
        }

        VkPushConstantRange blitPush{};
        blitPush.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        blitPush.offset     = 0;
        blitPush.size       = sizeof(BlitPushConstants);

        VkPipelineLayoutCreateInfo blitLayoutInfo{};
        blitLayoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        blitLayoutInfo.setLayoutCount         = 1;
        blitLayoutInfo.pSetLayouts            = &m_blitSetLayout;
        blitLayoutInfo.pushConstantRangeCount = 1;
        blitLayoutInfo.pPushConstantRanges    = &blitPush;
        if (vkCreatePipelineLayout(m_instance->GetDevice(), &blitLayoutInfo, nullptr, &m_blitPipelineLayout) != VK_SUCCESS)
        {
            ETCS_LOG("VulkanSurface", "vkCreatePipelineLayout (blit) failed.");
            return false;
        }
        return true;
    }

    VkShaderModule m_vertShader = VK_NULL_HANDLE;
    VkShaderModule m_fragShader = VK_NULL_HANDLE;

    // --- frame composition ---

    // RETAINED, not immediate. A surface keeps showing its last composition
    // until something composes a new one, and the first draw call AFTER a
    // present is what starts that new one.
    //
    // This is the rule the frame edge needs to exist at all: with
    // ProduceFrames/ConsumeFrames the surface presents on its own clock,
    // and .etcs has no loop construct, so a script issues its draws ONCE and
    // then blocks in Window.Run. Under immediate-mode semantics every frame
    // after the first would present an empty screen. It is also just what a
    // canvas does -- a layer stack does not vanish because a frame went by.
    //
    // Callers that DO redraw every frame (RunDemo, or any future render
    // loop) see no difference: their first draw of each frame clears the
    // retained list, so nothing accumulates across frames.
    // Caller holds m_stateMutex.
    void takeSnapshotLocked(FrameSnapshot& out)
    {
        out.clearColor = m_clearColor;

        out.draws.reserve(m_pendingDraws.size());
        for (const PendingDraw& d : m_pendingDraws)
        {
            VkDescriptorSet set = VK_NULL_HANDLE;
            if (d.kind == PendingDraw::Kind::Blit)
            {
                auto it = m_textures.find(d.source);
                if (it == m_textures.end()) continue;   // source went away since the Blit call
                set = it->second.set;
            }
            out.draws.push_back({ d.kind, d.x, d.y, d.w, d.h, d.r, d.g, d.b, d.a, set });
        }

        // Collected here rather than iterated during recording: another
        // thread's Blit can insert into m_textures at any time, and
        // rehashing invalidates iterators (references to existing elements
        // survive, which is why holding the handles below is fine).
        for (auto& [rid, tex] : m_textures)
        {
            (void)rid;
            if (!tex.needsUpload) continue;
            out.uploads.push_back({ tex.image, tex.staging, tex.w, tex.h, !tex.everUploaded });
            tex.needsUpload  = false;
            tex.everUploaded = true;
        }

        // The composition stays in m_pendingDraws -- it is retained, not
        // consumed. Marking it presented is what lets the next draw call
        // start a new one.
        m_composed = true;
    }

    // --- blit texture cache ---

    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
                       VkBuffer& outBuf, VkDeviceMemory& outMem)
    {
        VkBufferCreateInfo ci{};
        ci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        ci.size        = size;
        ci.usage       = usage;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(m_instance->GetDevice(), &ci, nullptr, &outBuf) != VK_SUCCESS) return false;

        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(m_instance->GetDevice(), outBuf, &req);
        uint32_t typeIndex = m_instance->FindMemoryType(req.memoryTypeBits, props);
        if (typeIndex == UINT32_MAX) return false;

        VkMemoryAllocateInfo alloc{};
        alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize  = req.size;
        alloc.memoryTypeIndex = typeIndex;
        if (vkAllocateMemory(m_instance->GetDevice(), &alloc, nullptr, &outMem) != VK_SUCCESS) return false;
        vkBindBufferMemory(m_instance->GetDevice(), outBuf, outMem, 0);
        return true;
    }

    // SRGB, not UNORM: the swapchain is an sRGB format, so the hardware
    // encodes linear->sRGB on write. Sampling an SRGB texture decodes
    // sRGB->linear on read, and the two cancel -- a pixel handed in as
    // 0x80 comes out of the display as 0x80. A UNORM texture here would
    // skip the decode and every blitted layer would come out visibly
    // brightened.
    bool ensureTexture(ETCS::RID rid, const Pixels_& px)
    {
        auto it = m_textures.find(rid);
        if (it != m_textures.end())
        {
            if (it->second.w == px.PixelWidth() && it->second.h == px.PixelHeight()) return true;
            destroyTexture(it->second);   // resized -- rebuild against the new dimensions
            m_textures.erase(it);
        }
        if (m_textures.size() >= SURFACE_MAX_BLIT_SOURCES)
        {
            ETCS_LOG("VulkanSurface", "blit source cache full (" << SURFACE_MAX_BLIT_SOURCES
                     << ") -- dropping blit from RID:" << rid);
            return false;
        }

        BlitTexture tex{};
        tex.w = px.PixelWidth();
        tex.h = px.PixelHeight();
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(tex.w) * tex.h * 4;

        /*
 * UNORM, NOT SRGB, and the difference is visible rather than theoretical.
 *
 * Pixels_::toByte is a plain linear scale -- 0.13f becomes 33 -- so the bytes
 * in a CPU-backed surface are linear samples, not sRGB-encoded ones. Sampling
 * them through an SRGB view told the device to DECODE them as if they were
 * encoded, and the SRGB swapchain then re-encoded on the way out, so the
 * stored byte appeared on screen literally: 33. Meanwhile DrawRect sends the
 * same 0.13f through the shader to that same SRGB attachment, which encodes
 * it properly: 101.
 *
 * One colour, two answers, decided by which surface happened to draw it. It
 * went unnoticed while CPU layers and device rects were used for different
 * things; CompositeDrawable2D made it impossible to miss, because moving a
 * subtree from the polygon path to the composited path changed the picture
 * while changing nothing about what it was asked to draw.
 *
 * UNORM makes the sample linear, which is what the byte actually is, and lets
 * the attachment do the single encode it was always going to do. Both paths
 * now produce the same pixel for the same float -- verified by rendering the
 * identical scene both ways and comparing the histograms.
 */
        VkImageCreateInfo imageInfo{};
        imageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType     = VK_IMAGE_TYPE_2D;
        imageInfo.format        = VK_FORMAT_R8G8B8A8_UNORM;
        imageInfo.extent        = { tex.w, tex.h, 1 };
        imageInfo.mipLevels     = 1;
        imageInfo.arrayLayers   = 1;
        imageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(m_instance->GetDevice(), &imageInfo, nullptr, &tex.image) != VK_SUCCESS)
        {
            ETCS_LOG("VulkanSurface", "vkCreateImage failed for blit source RID:" << rid);
            destroyTexture(tex);
            return false;
        }

        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(m_instance->GetDevice(), tex.image, &req);
        uint32_t typeIndex = m_instance->FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (typeIndex == UINT32_MAX) { destroyTexture(tex); return false; }

        VkMemoryAllocateInfo alloc{};
        alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize  = req.size;
        alloc.memoryTypeIndex = typeIndex;
        if (vkAllocateMemory(m_instance->GetDevice(), &alloc, nullptr, &tex.memory) != VK_SUCCESS)
        {
            destroyTexture(tex);
            return false;
        }
        vkBindImageMemory(m_instance->GetDevice(), tex.image, tex.memory, 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image            = tex.image;
        viewInfo.viewType         = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format           = VK_FORMAT_R8G8B8A8_UNORM;  // matches the image -- see createTexture
        viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(m_instance->GetDevice(), &viewInfo, nullptr, &tex.view) != VK_SUCCESS)
        {
            destroyTexture(tex);
            return false;
        }

        // Persistently mapped: a layer being painted on is re-uploaded
        // every time it changes, and map/unmap per frame would be pure
        // overhead for a buffer that never moves.
        if (!createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           tex.staging, tex.stagingMem))
        {
            ETCS_LOG("VulkanSurface", "staging buffer allocation failed for blit source RID:" << rid);
            destroyTexture(tex);
            return false;
        }
        vkMapMemory(m_instance->GetDevice(), tex.stagingMem, 0, bytes, 0, &tex.stagingMapped);

        VkDescriptorSetAllocateInfo setAlloc{};
        setAlloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        setAlloc.descriptorPool     = m_blitDescriptorPool;
        setAlloc.descriptorSetCount = 1;
        setAlloc.pSetLayouts        = &m_blitSetLayout;
        if (vkAllocateDescriptorSets(m_instance->GetDevice(), &setAlloc, &tex.set) != VK_SUCCESS)
        {
            ETCS_LOG("VulkanSurface", "vkAllocateDescriptorSets failed for blit source RID:" << rid);
            destroyTexture(tex);
            return false;
        }

        VkDescriptorImageInfo imgInfo{};
        imgInfo.sampler     = m_blitSampler;
        imgInfo.imageView   = tex.view;
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = tex.set;
        write.dstBinding      = 0;
        write.descriptorCount = 1;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo      = &imgInfo;
        vkUpdateDescriptorSets(m_instance->GetDevice(), 1, &write, 0, nullptr);

        m_textures[rid] = tex;
        return true;
    }

    void destroyTexture(BlitTexture& tex)
    {
        VkDevice dev = m_instance ? m_instance->GetDevice() : VK_NULL_HANDLE;
        if (!dev) return;
        if (tex.set)        { vkFreeDescriptorSets(dev, m_blitDescriptorPool, 1, &tex.set); tex.set = VK_NULL_HANDLE; }
        if (tex.stagingMapped) { vkUnmapMemory(dev, tex.stagingMem); tex.stagingMapped = nullptr; }
        if (tex.staging)    { vkDestroyBuffer(dev, tex.staging, nullptr);   tex.staging    = VK_NULL_HANDLE; }
        if (tex.stagingMem) { vkFreeMemory(dev, tex.stagingMem, nullptr);   tex.stagingMem = VK_NULL_HANDLE; }
        if (tex.view)       { vkDestroyImageView(dev, tex.view, nullptr);   tex.view       = VK_NULL_HANDLE; }
        if (tex.image)      { vkDestroyImage(dev, tex.image, nullptr);      tex.image      = VK_NULL_HANDLE; }
        if (tex.memory)     { vkFreeMemory(dev, tex.memory, nullptr);       tex.memory     = VK_NULL_HANDLE; }
    }

    // Layout transition + copy, recorded into the frame's own command
    // buffer BEFORE the render pass begins (a copy inside a render pass is
    // illegal). One barrier pair per texture that changed since last frame.
    void recordPendingUploads(VkCommandBuffer cmd, const std::vector<UploadJob>& uploads)
    {
        for (const UploadJob& job : uploads)
        {
            VkImageMemoryBarrier toDst{};
            toDst.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            toDst.oldLayout           = job.firstUpload ? VK_IMAGE_LAYOUT_UNDEFINED
                                                         : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            toDst.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toDst.image               = job.image;
            toDst.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            toDst.srcAccessMask       = job.firstUpload ? 0 : VK_ACCESS_SHADER_READ_BIT;
            toDst.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd,
                                  job.firstUpload ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                                   : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                  VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toDst);

            VkBufferImageCopy copy{};
            copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copy.imageExtent      = { job.w, job.h, 1 };
            vkCmdCopyBufferToImage(cmd, job.staging, job.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

            VkImageMemoryBarrier toRead = toDst;
            toRead.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toRead.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                  0, 0, nullptr, 0, nullptr, 1, &toRead);
        }
    }

    bool chooseSurfaceFormat(VkSurfaceFormatKHR& out)
    {
        uint32_t count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(m_instance->GetPhysicalDevice(), m_surface, &count, nullptr);
        if (count == 0) return false;
        std::vector<VkSurfaceFormatKHR> formats(count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(m_instance->GetPhysicalDevice(), m_surface, &count, formats.data());
        for (const auto& f : formats)
            if (f.format == VK_FORMAT_B8G8R8A8_SRGB && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            { out = f; return true; }
        out = formats[0];
        return true;
    }

    // Tears down and rebuilds everything downstream of the swapchain
    // (images/views/render pass/framebuffers/pipeline) -- called on
    // initial Create (via the immediate OnResize invocation) and on every
    // real resize thereafter (ontology/Resizable.h's notifyResize fix is
    // what makes the second case actually fire).
    /*
 * Called from the resize callback, on whatever thread polls the window.
 * Records and returns -- see PresentConcrete for why the recreation itself
 * belongs to the frame thread.
 *
 * The FIRST call is different and is taken synchronously: OnResize fires
 * immediately on registration with the current size, and that call IS the
 * initial swapchain build, on the creating thread, before any presenter
 * exists. Create's own return value depends on it having happened.
 */
    void queueResize(WindowSize sz)
    {
        if (!m_builtOnce)
        {
            m_builtOnce = true;
            recreateSwapchain(sz);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_pendingExtent = sz;
        }
        m_resizePending.store(true, std::memory_order_release);
    }

    void recreateSwapchain(WindowSize sz)
    {
        if (!m_instance || m_surface == VK_NULL_HANDLE) return;
        if (sz.width == 0 || sz.height == 0) { m_extent = { 0, 0 }; return; } // minimized -- wait for next real resize

        vkDeviceWaitIdle(m_instance->GetDevice());
        destroySwapchainDependents();

        VkSurfaceCapabilitiesKHR caps{};
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_instance->GetPhysicalDevice(), m_surface, &caps);

        VkSurfaceFormatKHR surfaceFormat{};
        if (!chooseSurfaceFormat(surfaceFormat)) return;
        m_format = surfaceFormat.format;

        VkExtent2D extent;
        if (caps.currentExtent.width != UINT32_MAX)
        {
            extent = caps.currentExtent;
        }
        else
        {
            extent.width  = std::clamp(sz.width,  caps.minImageExtent.width,  caps.maxImageExtent.width);
            extent.height = std::clamp(sz.height, caps.minImageExtent.height, caps.maxImageExtent.height);
        }
        m_extent = extent;

        uint32_t imageCount = caps.minImageCount + 1;
        if (caps.maxImageCount > 0) imageCount = std::min(imageCount, caps.maxImageCount);

        VkSwapchainCreateInfoKHR ci{};
        ci.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        ci.surface          = m_surface;
        ci.minImageCount    = imageCount;
        ci.imageFormat      = surfaceFormat.format;
        ci.imageColorSpace  = surfaceFormat.colorSpace;
        ci.imageExtent      = extent;
        ci.imageArrayLayers = 1;
        ci.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.preTransform     = caps.currentTransform;
        ci.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        ci.presentMode      = VK_PRESENT_MODE_FIFO_KHR; // universally supported, vsync'd -- good enough for V1
        ci.clipped          = VK_TRUE;

        if (vkCreateSwapchainKHR(m_instance->GetDevice(), &ci, nullptr, &m_swapchain) != VK_SUCCESS)
        {
            ETCS_LOG("VulkanSurface", "vkCreateSwapchainKHR failed.");
            m_swapchain = VK_NULL_HANDLE;
            return;
        }

        uint32_t actualCount = 0;
        vkGetSwapchainImagesKHR(m_instance->GetDevice(), m_swapchain, &actualCount, nullptr);
        m_images.resize(actualCount);
        vkGetSwapchainImagesKHR(m_instance->GetDevice(), m_swapchain, &actualCount, m_images.data());

        m_imageViews.resize(actualCount);
        for (uint32_t i = 0; i < actualCount; ++i)
        {
            VkImageViewCreateInfo viewInfo{};
            viewInfo.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image    = m_images[i];
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format   = m_format;
            viewInfo.components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                     VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
            viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            if (vkCreateImageView(m_instance->GetDevice(), &viewInfo, nullptr, &m_imageViews[i]) != VK_SUCCESS)
            {
                ETCS_LOG("VulkanSurface", "vkCreateImageView failed for swapchain image " << i);
                return;
            }
        }

        if (!createRenderPass()) return;
        if (!createFramebuffers()) return;
        if (!createGraphicsPipeline()) return;

        // Re-fire on THIS target's own Resizable interface, so anything
        // subscribed to the target (rather than to the window) -- a future
        // PintaProvider canvas, say -- hears the new size too.
        this->notifyResize({ m_extent.width, m_extent.height });
    }

    bool createRenderPass()
    {
        VkAttachmentDescription colorAttachment{};
        colorAttachment.format         = m_format;
        colorAttachment.samples        = VK_SAMPLE_COUNT_1_BIT;
        colorAttachment.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAttachment.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference colorRef{};
        colorRef.attachment = 0;
        colorRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments    = &colorRef;

        VkSubpassDependency dep{};
        dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass    = 0;
        dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.srcAccessMask = 0;
        dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo ci{};
        ci.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        ci.attachmentCount = 1;
        ci.pAttachments    = &colorAttachment;
        ci.subpassCount    = 1;
        ci.pSubpasses      = &subpass;
        ci.dependencyCount = 1;
        ci.pDependencies   = &dep;

        if (vkCreateRenderPass(m_instance->GetDevice(), &ci, nullptr, &m_renderPass) != VK_SUCCESS)
        {
            ETCS_LOG("VulkanSurface", "vkCreateRenderPass failed.");
            return false;
        }
        return true;
    }

    bool createFramebuffers()
    {
        m_framebuffers.resize(m_imageViews.size());
        for (size_t i = 0; i < m_imageViews.size(); ++i)
        {
            VkImageView attachments[] = { m_imageViews[i] };
            VkFramebufferCreateInfo ci{};
            ci.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            ci.renderPass      = m_renderPass;
            ci.attachmentCount = 1;
            ci.pAttachments    = attachments;
            ci.width           = m_extent.width;
            ci.height          = m_extent.height;
            ci.layers          = 1;
            if (vkCreateFramebuffer(m_instance->GetDevice(), &ci, nullptr, &m_framebuffers[i]) != VK_SUCCESS)
            {
                ETCS_LOG("VulkanSurface", "vkCreateFramebuffer failed for image " << i);
                return false;
            }
        }
        return true;
    }

    // Both pipelines are identical except for their shaders and layout --
    // same render pass, same empty vertex input (corners come from
    // gl_VertexIndex), same alpha blending. Built together because both are
    // swapchain-dependent: viewport/scissor are baked in, so a resize
    // rebuilds both.
    bool createGraphicsPipeline()
    {
        if (!buildPipeline(m_vertShader, m_fragShader, m_pipelineLayout, m_pipeline)) return false;
        if (!buildPipeline(m_blitVertShader, m_blitFragShader, m_blitPipelineLayout, m_blitPipeline)) return false;
        return true;
    }

    bool buildPipeline(VkShaderModule vs, VkShaderModule fs, VkPipelineLayout layout, VkPipeline& out)
    {
        VkPipelineShaderStageCreateInfo vertStage{};
        vertStage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vertStage.stage  = VK_SHADER_STAGE_VERTEX_BIT;
        vertStage.module = vs;
        vertStage.pName  = "main";

        VkPipelineShaderStageCreateInfo fragStage{};
        fragStage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        fragStage.stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        fragStage.module = fs;
        fragStage.pName  = "main";

        VkPipelineShaderStageCreateInfo stages[] = { vertStage, fragStage };

        // No vertex buffers -- the quad's 4 corners come from gl_VertexIndex
        // in rect.vert (see shaders/rect.vert), so vertex input is empty.
        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

        VkViewport viewport{ 0.0f, 0.0f, static_cast<float>(m_extent.width), static_cast<float>(m_extent.height), 0.0f, 1.0f };
        VkRect2D scissor{ { 0, 0 }, m_extent };
        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.pViewports    = &viewport;
        viewportState.scissorCount  = 1;
        viewportState.pScissors     = &scissor;

        VkPipelineRasterizationStateCreateInfo raster{};
        raster.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode    = VK_CULL_MODE_NONE;
        raster.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth   = 1.0f;

        VkPipelineMultisampleStateCreateInfo msaa{};
        msaa.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        msaa.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        blendAttachment.blendEnable    = VK_TRUE;
        blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.colorBlendOp        = VK_BLEND_OP_ADD;
        blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        blendAttachment.alphaBlendOp        = VK_BLEND_OP_ADD;

        VkPipelineColorBlendStateCreateInfo blend{};
        blend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blend.attachmentCount = 1;
        blend.pAttachments    = &blendAttachment;

        VkGraphicsPipelineCreateInfo ci{};
        ci.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        ci.stageCount          = 2;
        ci.pStages             = stages;
        ci.pVertexInputState   = &vertexInput;
        ci.pInputAssemblyState = &inputAssembly;
        ci.pViewportState      = &viewportState;
        ci.pRasterizationState = &raster;
        ci.pMultisampleState   = &msaa;
        ci.pColorBlendState    = &blend;
        ci.layout              = layout;
        ci.renderPass           = m_renderPass;
        ci.subpass              = 0;

        if (vkCreateGraphicsPipelines(m_instance->GetDevice(), VK_NULL_HANDLE, 1, &ci, nullptr, &out) != VK_SUCCESS)
        {
            ETCS_LOG("VulkanSurface", "vkCreateGraphicsPipelines failed.");
            return false;
        }
        return true;
    }

    void recordFrame(VkCommandBuffer cmd, uint32_t imageIndex, const FrameSnapshot& frame)
    {
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(cmd, &begin);

        // Before the render pass: a copy command is illegal inside one.
        recordPendingUploads(cmd, frame.uploads);

        VkClearValue clear{};
        clear.color = { { frame.clearColor[0], frame.clearColor[1], frame.clearColor[2], frame.clearColor[3] } };

        VkRenderPassBeginInfo rpBegin{};
        rpBegin.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpBegin.renderPass        = m_renderPass;
        rpBegin.framebuffer       = m_framebuffers[imageIndex];
        rpBegin.renderArea.extent = m_extent;
        rpBegin.clearValueCount   = 1;
        rpBegin.pClearValues      = &clear;
        vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

        // In call order, switching pipelines only when the kind changes --
        // a layer stack is usually one blit after another, so the common
        // case is a single bind. Order is the correctness requirement here,
        // not the bind count: compositing back-to-front only works if these
        // are recorded exactly as they arrived.
        const float fw = static_cast<float>(m_extent.width);
        const float fh = static_cast<float>(m_extent.height);
        VkPipeline bound = VK_NULL_HANDLE;

        for (const FrameDraw& d : frame.draws)
        {
            // Pixel rect -> NDC. Same conversion for both kinds.
            const float nx = (static_cast<float>(d.x) / fw) * 2.0f - 1.0f;
            const float ny = (static_cast<float>(d.y) / fh) * 2.0f - 1.0f;
            const float nw = (static_cast<float>(d.w) / fw) * 2.0f;
            const float nh = (static_cast<float>(d.h) / fh) * 2.0f;

            if (d.kind == PendingDraw::Kind::Rect)
            {
                if (bound != m_pipeline)
                {
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
                    bound = m_pipeline;
                }
                RectPushConstants pc{};
                pc.rect[0] = nx; pc.rect[1] = ny; pc.rect[2] = nw; pc.rect[3] = nh;
                pc.color[0] = d.r; pc.color[1] = d.g; pc.color[2] = d.b; pc.color[3] = d.a;
                vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                    0, sizeof(pc), &pc);
                vkCmdDraw(cmd, 4, 1, 0, 0);
            }
            else
            {
                if (d.set == VK_NULL_HANDLE) continue;   // source went away between Blit and Present

                if (bound != m_blitPipeline)
                {
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_blitPipeline);
                    bound = m_blitPipeline;
                }
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_blitPipelineLayout,
                                         0, 1, &d.set, 0, nullptr);
                BlitPushConstants pc{};
                pc.rect[0] = nx; pc.rect[1] = ny; pc.rect[2] = nw; pc.rect[3] = nh;
                pc.opacity = d.a;
                vkCmdPushConstants(cmd, m_blitPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                    0, sizeof(pc), &pc);
                vkCmdDraw(cmd, 4, 1, 0, 0);
            }
        }

        vkCmdEndRenderPass(cmd);
        vkEndCommandBuffer(cmd);
    }

    void destroySwapchainDependents()
    {
        VkDevice dev = m_instance ? m_instance->GetDevice() : VK_NULL_HANDLE;
        if (!dev) return;
        if (m_pipeline)       { vkDestroyPipeline(dev, m_pipeline, nullptr);             m_pipeline = VK_NULL_HANDLE; }
        if (m_blitPipeline)   { vkDestroyPipeline(dev, m_blitPipeline, nullptr);         m_blitPipeline = VK_NULL_HANDLE; }
        for (auto fb : m_framebuffers) vkDestroyFramebuffer(dev, fb, nullptr);
        m_framebuffers.clear();
        if (m_renderPass)     { vkDestroyRenderPass(dev, m_renderPass, nullptr);         m_renderPass = VK_NULL_HANDLE; }
        for (auto view : m_imageViews) vkDestroyImageView(dev, view, nullptr);
        m_imageViews.clear();
        m_images.clear();
        if (m_swapchain)      { vkDestroySwapchainKHR(dev, m_swapchain, nullptr);        m_swapchain = VK_NULL_HANDLE; }
    }

    void teardown()
    {
        if (m_instance && m_instance->GetDevice()) vkDeviceWaitIdle(m_instance->GetDevice());
        destroySwapchainDependents();
        VkDevice dev = m_instance ? m_instance->GetDevice() : VK_NULL_HANDLE;
        if (dev)
        {
            // Textures first: their descriptor sets are freed back to the
            // pool, so the pool has to outlive them.
            for (auto& [rid, tex] : m_textures) { (void)rid; destroyTexture(tex); }
            m_textures.clear();

            if (m_pipelineLayout) { vkDestroyPipelineLayout(dev, m_pipelineLayout, nullptr); m_pipelineLayout = VK_NULL_HANDLE; }
            if (m_vertShader)     { vkDestroyShaderModule(dev, m_vertShader, nullptr);        m_vertShader = VK_NULL_HANDLE; }
            if (m_fragShader)     { vkDestroyShaderModule(dev, m_fragShader, nullptr);        m_fragShader = VK_NULL_HANDLE; }

            if (m_blitPipelineLayout) { vkDestroyPipelineLayout(dev, m_blitPipelineLayout, nullptr); m_blitPipelineLayout = VK_NULL_HANDLE; }
            if (m_blitVertShader)     { vkDestroyShaderModule(dev, m_blitVertShader, nullptr);        m_blitVertShader = VK_NULL_HANDLE; }
            if (m_blitFragShader)     { vkDestroyShaderModule(dev, m_blitFragShader, nullptr);        m_blitFragShader = VK_NULL_HANDLE; }
            if (m_blitDescriptorPool) { vkDestroyDescriptorPool(dev, m_blitDescriptorPool, nullptr);  m_blitDescriptorPool = VK_NULL_HANDLE; }
            if (m_blitSetLayout)      { vkDestroyDescriptorSetLayout(dev, m_blitSetLayout, nullptr);  m_blitSetLayout = VK_NULL_HANDLE; }
            if (m_blitSampler)        { vkDestroySampler(dev, m_blitSampler, nullptr);                m_blitSampler = VK_NULL_HANDLE; }
            for (uint32_t i = 0; i < SURFACE_FRAMES_IN_FLIGHT; ++i)
            {
                if (m_imageAvailable[i]) vkDestroySemaphore(dev, m_imageAvailable[i], nullptr);
                if (m_renderFinished[i]) vkDestroySemaphore(dev, m_renderFinished[i], nullptr);
                if (m_inFlight[i])       vkDestroyFence(dev, m_inFlight[i], nullptr);
            }
        }
        m_imageAvailable.fill(VK_NULL_HANDLE);
        m_renderFinished.fill(VK_NULL_HANDLE);
        m_inFlight.fill(VK_NULL_HANDLE);
        if (m_instance && m_surface) { vkDestroySurfaceKHR(m_instance->GetInstance(), m_surface, nullptr); m_surface = VK_NULL_HANDLE; }
        this->removeTag("active");
    }
};

#endif
