#ifndef RENDERPROVIDER_CANVASSURFACE_H__
#define RENDERPROVIDER_CANVASSURFACE_H__

#include "../../../ontology.h"
#include "CanvasInstance.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <mutex>
#include <cstring>
#include <string>
#include <vector>

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#include <emscripten/em_asm.h>
#include <emscripten/threading.h>
#endif

/*
 * CanvasSurface -- the window surface in a browser.
 *
 * [Surface + Pixels + Presentable + Resizable + Deletable + Lifecycle + Threaded]
 *
 * PIXELS, NOT RENDERABLE, and that choice is the whole design. The two are
 * exclusive under Raster (ontology/Raster.h): a Renderable keeps its pixels on a
 * device and answers a DeviceKey, a Pixels keeps host bytes. A canvas is reached
 * by handing it host bytes, so this is the Pixels side -- and everything that
 * follows falls out of it:
 *
 *   - Clear, DrawRect and Blit write the host-byte back buffer (same ops
 *     ImageSurface uses). No shaders, no pipelines, no descriptor sets.
 *
 *   - HOST FRONT BUFFER. Present snapshots the back buffer into a front
 *     buffer under the raster lock, then putImageData's the front only.
 *     That is VulkanSurface's composition boundary on a CPU raster: Clear
 *     starts a picture; DrawRect/Blit append; Present snapshots. Draws do
 *     not etcs_mark_observed -- a concurrent Present mid-batch would otherwise
 *     flash a cleared or half-composited frame. Foreign modules never name
 *     this type; they mark when a logical frame is finished.
 *
 *   - Blit accepts any Pixels_ source, so a PaintProvider layer or a Camera3D's
 *     projection composites onto the window with the same call it uses to
 *     composite onto another layer.
 *
 *   - Present is the only browser-specific code in the file: the front buffer
 *     goes to the canvas and nothing else happens.
 *
 * WHY THAT IS CHEAP RATHER THAN A CONCESSION. Pixels_ is 8-bit RGBA in ascending
 * byte order, NOT premultiplied, tightly packed, stride = width * 4, origin
 * top-left (ontology/Pixels.h). That is byte-for-byte the layout of a canvas
 * ImageData, so presenting is one putImageData over the buffer -- no conversion,
 * no repack, no intermediate copy on the C++ side.
 *
 * WHAT IT DOES NOT DO. There is no device here, so a Renderable source cannot be
 * composited (it has no host bytes to read) and nothing is accelerated. A
 * device-backed browser surface is a second concrete type beside this one,
 * selected by the same typedef in Contract_RenderProvider.h; it would claim
 * RenderableBase instead and change nothing above it.
 */
class CanvasSurface : public SurfaceBase<CanvasSurface>,
                       public PixelsBase<CanvasSurface>,
                       public PresentableBase<CanvasSurface>,
                       public DeletableBase<CanvasSurface>,
                       public LifecycleBase<CanvasSurface>,
                       public ThreadedBase<CanvasSurface>
{
public:
    // The ordering every Surface owes (Orderable, composed by SurfaceBase).
    int32_t m_order = 0;
    bool operator<(const CanvasSurface& o) const { return m_order < o.m_order; }
    WIRE_TYPE_IDENTITY(CanvasSurface);

    CanvasSurface()  = default;
    ~CanvasSurface() = default;

    /*
     * The parent link IS the binding between a surface and the window it draws
     * into -- same contract VulkanSurface states, and the reason a Surface is a
     * typed CHILD of a Window rather than a root spawn.
     *
     * shader_dir is accepted and ignored: there are no shaders on this path, and
     * the signature is the family's rather than this backend's.
     */
    bool Create(CanvasInstance* instance, const std::string& shader_dir)
    {
        (void)shader_dir;
        if (PixelWidth() != 0 && PixelHeight() != 0) return true;   // idempotent

        if (!instance || !instance->IsActive())
        {
            ETCS_LOG("CanvasSurface", "Create needs an active RenderProvider::Instance.");
            return false;
        }
        m_instance = instance;

        ETCS::Entity* parent = getParent();
        if (!parent)
        {
            ETCS_LOG("CanvasSurface", "Create: no parent -- a surface is a typed child of "
                     "the window it draws into.");
            return false;
        }
        m_parentResizable = static_cast<Resizable_*>(
            parent->getInterfacePointer(ETCS::Buffer("Resizable")));
        if (!parent->getInterfacePointer(ETCS::Buffer("Window")) || !m_parentResizable)
        {
            ETCS_LOG("CanvasSurface", "Create: parent RID:" << parent->getRID()
                     << " is not a resizable Window.");
            return false;
        }

        const WindowSize size = m_parentResizable->GetSize();
        if (size.width == 0 || size.height == 0)
        {
            ETCS_LOG("CanvasSurface", "Create: the window reports a zero dimension ("
                     << size.width << "x" << size.height << ").");
            return false;
        }
        Allocate(size.width, size.height);
        this->FollowResize(m_parentResizable);
        this->addTag("active");
        ETCS_LOG("CanvasSurface", "ready (RID:" << getRID() << ") " << size.width
                 << "x" << size.height << " -- presenting to #" << m_target
                 << (TargetExists() ? "." : " (NO SUCH ELEMENT on the page yet)."));
        return true;
    }

    /*
     * WHICH CANVAS THIS SURFACE PRESENTS TO, and the whole of what makes more
     * than one of them possible.
     *
     * A page can hold any number of canvases and a session any number of
     * surfaces; what was missing was the link between a particular surface and a
     * particular element. It belongs on the SURFACE rather than on the window:
     * emscripten's GLFW owns exactly one canvas (`Browser.getCanvas()`), every
     * one of its event handlers drops events whose target is not that canvas, and
     * `glfwCreateWindow` makes each new window the active one -- so a second GLFW
     * window cannot host a second canvas, it can only take the first one's input
     * away. A second SURFACE has none of those problems: presenting is a
     * putImageData, and nothing about it is global.
     *
     * Named, not numbered. An index into the page's canvases would make the
     * script depend on the order elements appear in the HTML; an id is what the
     * page already calls the thing.
     *
     * The Vulkan backend has this verb too, where it names the presentation
     * target and there is currently one -- see VulkanSurface::SetTarget.
     */
    bool SetTarget(const std::string& element_id)
    {
        if (element_id.empty())
        {
            ETCS_LOG("CanvasSurface", "SetTarget: an empty id names nothing -- keeping #"
                     << m_target << ".");
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(m_rasterMutex);
            m_target = element_id;
        }
        ETCS_LOG("CanvasSurface", "target -> #" << m_target
                 << (TargetExists() ? "" : " (NO SUCH ELEMENT on the page)"));
        return true;
    }

    const std::string& Target() const { return m_target; }

    /*
     * AN EXPLICIT SIZE WINS OVER THE WINDOW'S, which is what lets a surface be a
     * REGION rather than the whole frame -- a toolbar strip beside the page it
     * belongs to, for instance.
     *
     * Following and being told are the two ways a surface can get its size and
     * they cannot both be live: PollResize would snap an explicitly sized surface
     * back to the window's dimensions at the next frame boundary. So this drops
     * the follow, which also states the precedence in one place instead of a flag
     * every reader has to correlate.
     */
    bool ResizeTo(WindowSize sz) override
    {
        if (sz.width == 0 || sz.height == 0)
        {
            ETCS_LOG("CanvasSurface", "ResizeTo: a zero dimension is not a size ("
                     << sz.width << "x" << sz.height << ").");
            return false;
        }
        if (sz.width == PixelWidth() && sz.height == PixelHeight()) return true;

        std::lock_guard<std::mutex> lock(m_rasterMutex);
        ETCS_LOG("CanvasSurface", "resize (explicit) " << PixelWidth() << "x"
                 << PixelHeight() << " -> " << sz.width << "x" << sz.height
                 << " -- no longer following the window.");
        m_parentResizable = nullptr;
        Allocate(sz.width, sz.height);
        m_front.clear();
        m_front_w = m_front_h = 0;
        return true;
    }

    // ── Surface_ dispatch ────────────────────────────────────────────────────
    //
    // CLEAR IS THE COMPOSITION BOUNDARY, same rule as VulkanSurface. Draws write
    // the back buffer only and do not mark observed -- Present is what publishes
    // a finished picture. A mark from every FillRect would wake ConsumeFrames
    // mid-batch and putImageData a cleared or half-composited raster (the drag
    // flicker). Foreign code (PaintProvider) still marks once a logical frame is
    // done; that mark is the only edge that should reach Present.

    void ClearConcrete(float r, float g, float b, float a) override
    {
        std::lock_guard<std::mutex> lock(m_rasterMutex);
        m_clearColor = { r, g, b, a };
        uint8_t* data = PixelData();
        if (!data) return;
        const uint8_t px[4] = {
            static_cast<uint8_t>(r <= 0.f ? 0 : r >= 1.f ? 255 : r * 255.f + 0.5f),
            static_cast<uint8_t>(g <= 0.f ? 0 : g >= 1.f ? 255 : g * 255.f + 0.5f),
            static_cast<uint8_t>(b <= 0.f ? 0 : b >= 1.f ? 255 : b * 255.f + 0.5f),
            static_cast<uint8_t>(a <= 0.f ? 0 : a >= 1.f ? 255 : a * 255.f + 0.5f)
        };
        const size_t n = PixelBytes();
        for (size_t i = 0; i < n; i += 4)
            std::memcpy(data + i, px, 4);
        // no etcs_mark_observed -- see class comment
    }

    void DrawRectConcrete(int32_t x, int32_t y, uint32_t w, uint32_t h,
                           float r, float g, float b, float a) override
    {
        std::lock_guard<std::mutex> lock(m_rasterMutex);
        // FillRect marks; write through PixelData instead so Present is not
        // woken on every rubber-band or stamp dab.
        uint8_t* base = PixelData();
        if (!base || a <= 0.0f) return;
        const uint32_t pw = PixelWidth();
        const uint32_t ph = PixelHeight();
        if (pw == 0 || ph == 0) return;
        const int32_t x0 = x < 0 ? 0 : x;
        const int32_t y0 = y < 0 ? 0 : y;
        const int32_t x1 = std::min(static_cast<int32_t>(pw), x + static_cast<int32_t>(w));
        const int32_t y1 = std::min(static_cast<int32_t>(ph), y + static_cast<int32_t>(h));
        if (x0 >= x1 || y0 >= y1) return;
        const uint8_t sr = static_cast<uint8_t>(r <= 0.f ? 0 : r >= 1.f ? 255 : r * 255.f + 0.5f);
        const uint8_t sg = static_cast<uint8_t>(g <= 0.f ? 0 : g >= 1.f ? 255 : g * 255.f + 0.5f);
        const uint8_t sb = static_cast<uint8_t>(b <= 0.f ? 0 : b >= 1.f ? 255 : b * 255.f + 0.5f);
        const uint8_t sa = static_cast<uint8_t>(a <= 0.f ? 0 : a >= 1.f ? 255 : a * 255.f + 0.5f);
        const uint32_t stride = PixelStride();
        for (int32_t py = y0; py < y1; ++py)
        {
            uint8_t* row = base + static_cast<size_t>(py) * stride;
            for (int32_t px = x0; px < x1; ++px)
            {
                uint8_t* d = row + static_cast<size_t>(px) * 4;
                if (sa == 255) { d[0] = sr; d[1] = sg; d[2] = sb; d[3] = 255; continue; }
                const uint32_t inv = 255u - sa;
                const uint32_t da  = d[3];
                const uint32_t oa  = sa + (da * inv) / 255u;
                if (oa == 0) { d[0] = d[1] = d[2] = d[3] = 0; continue; }
                d[0] = static_cast<uint8_t>((sr * sa + d[0] * da * inv / 255u) / oa);
                d[1] = static_cast<uint8_t>((sg * sa + d[1] * da * inv / 255u) / oa);
                d[2] = static_cast<uint8_t>((sb * sa + d[2] * da * inv / 255u) / oa);
                d[3] = static_cast<uint8_t>(oa);
            }
        }
    }

    void BlitConcrete(Surface_* source, int32_t x, int32_t y,
                       uint32_t w, uint32_t h, float opacity) override
    {
        (void)w; (void)h;
        if (!source) { ETCS_LOG("CanvasSurface", "Blit called with no source."); return; }
        Pixels_* px = static_cast<Pixels_*>(source->getInterfacePointer(ETCS::Buffer("Pixels")));
        if (!px)
        {
            ETCS_LOG("CanvasSurface", "Blit source RID:" << source->getRID()
                     << " owns no pixels -- a device-side source cannot be read "
                        "back into a CPU composite.");
            return;
        }
        if (px == static_cast<Pixels_*>(this))
        {
            ETCS_LOG("CanvasSurface", "Blit source is this surface -- refusing to composite onto itself.");
            return;
        }
        std::lock_guard<std::mutex> lock(m_rasterMutex);
        // Composite marks; do the blend here without marking.
        if (PixelBytes() == 0 || px->PixelBytes() == 0 || opacity <= 0.0f) return;
        const uint32_t dw = PixelWidth();
        const uint32_t dh = PixelHeight();
        const uint32_t sw = px->PixelWidth();
        const uint32_t sh = px->PixelHeight();
        const uint32_t dstride = PixelStride();
        const uint32_t sstride = px->PixelStride();
        uint8_t* dst = PixelData();
        const uint8_t* src = px->PixelData();
        if (!dst || !src) return;
        for (uint32_t sy = 0; sy < sh; ++sy)
        {
            int64_t dy = static_cast<int64_t>(y) + sy;
            if (dy < 0 || dy >= static_cast<int64_t>(dh)) continue;
            const uint8_t* srow = src + static_cast<size_t>(sy) * sstride;
            uint8_t*       drow = dst + static_cast<size_t>(dy) * dstride;
            for (uint32_t sx = 0; sx < sw; ++sx)
            {
                int64_t dx = static_cast<int64_t>(x) + sx;
                if (dx < 0 || dx >= static_cast<int64_t>(dw)) continue;
                const uint8_t* s = srow + sx * 4;
                uint8_t* d = drow + static_cast<size_t>(dx) * 4;
                const uint8_t sa = static_cast<uint8_t>(s[3] * (opacity > 1.0f ? 1.0f : opacity));
                if (sa == 0) continue;
                if (sa == 255) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255; continue; }
                const uint32_t inv = 255u - sa;
                const uint32_t da  = d[3];
                const uint32_t oa  = sa + (da * inv) / 255u;
                if (oa == 0) { d[0] = d[1] = d[2] = d[3] = 0; continue; }
                d[0] = static_cast<uint8_t>((s[0] * sa + d[0] * da * inv / 255u) / oa);
                d[1] = static_cast<uint8_t>((s[1] * sa + d[1] * da * inv / 255u) / oa);
                d[2] = static_cast<uint8_t>((s[2] * sa + d[2] * da * inv / 255u) / oa);
                d[3] = static_cast<uint8_t>(oa);
            }
        }
    }

    // ── Presentable_ dispatch ────────────────────────────────────────────────
    /*
     * Snapshot back -> front under the lock, then putImageData the front with
     * the lock released. Holding the mutex across MAIN_THREAD_EM_ASM would stall
     * drawers for a full vsync; the front vector is stable for the proxy once
     * the copy completes.
     */
    void PresentConcrete() override
    {
        if (Retired()) return;

        PollResize();

        uint32_t w = 0, h = 0;
        std::string target;
        {
            std::lock_guard<std::mutex> lock(m_rasterMutex);
            const uint8_t* data = PixelData();
            w = PixelWidth();
            h = PixelHeight();
            if (!data || w == 0 || h == 0) return;
            /*
             * ONE COPY, INTO THE FRONT BUFFER, and it is the front buffer that
             * ships. There were three before: back -> m_front, m_front -> a
             * local snapshot, and the local -> the page. At 1024x768 that is
             * 3 MB copied twice more than the boundary requires, every frame,
             * on a worker -- and nothing ever read m_front, so the middle
             * buffer the comment above describes was not a front buffer at all,
             * only a write nobody consumed.
             *
             * m_front is still the thing handed over, which is what makes the
             * remaining copy load-bearing rather than incidental: the lock is
             * released before the proxy call, so the page must be reading
             * something the drawers cannot move under it.
             */
            m_front.resize(static_cast<size_t>(w) * h * 4);
            std::memcpy(m_front.data(), data, m_front.size());
            m_front_w = w;
            m_front_h = h;
            target = m_target;
        }
        if (m_front.empty()) return;

#if defined(__EMSCRIPTEN__)
        /*
         * ONE DECLARATION PER STATEMENT in the block below, and no bare commas.
         * The braces do not make this one macro argument -- only PARENTHESES
         * protect a comma from the preprocessor's argument split, so
         * `var w = $1, h = $2;` would hand MAIN_THREAD_EM_ASM four arguments and
         * stringify a fragment of the code. Commas inside a call's parentheses
         * (putImageData, subarray) are fine.
         */
        MAIN_THREAD_EM_ASM({
            var w = $1;
            var h = $2;
            var id = UTF8ToString($3);
            // BY ID, never Module.canvas. Module.canvas is the ONE canvas
            // emscripten's own machinery points at, so falling back to it would
            // quietly draw this surface over whichever one that is -- exactly the
            // bug a second canvas exists to avoid. A missing element draws
            // nothing; Create already said so in the log.
            var canvas = document.getElementById(id);
            if (!canvas) return;
            // The framebuffer is authoritative: a canvas whose backing store is a
            // different size would scale the bytes instead of showing them.
            if (canvas.width !== w)  canvas.width  = w;
            if (canvas.height !== h) canvas.height = h;
            var ctx = canvas.getContext('2d');
            if (!ctx) return;   // a WebGL context was taken on this canvas
            var img = ctx.createImageData(w, h);
            img.data.set(HEAPU8.subarray($0, $0 + w * h * 4));
            ctx.putImageData(img, 0, 0);
        }, m_front.data(), w, h, target.c_str());
#endif
        notePresent();
    }

    // ── Resizable_ dispatch ──────────────────────────────────────────────────

    WindowSize GetSizeConcrete() override
    {
        return { PixelWidth(), PixelHeight() };
    }

    // ── the compose root ─────────────────────────────────────────────────────
    //
    // Bind a Drawable root and every frame starts with Clear and re-walks it;
    // bind zero and whatever a script drew is retained. Exclusive by
    // construction rather than by a flag, because a bound root clears first.

    void SetComposeRoot(ETCS::RID root)
    {
        std::lock_guard<std::mutex> lock(m_rasterMutex);
        m_composeRoot = root;
    }
    ETCS::RID ComposeRoot() const { return m_composeRoot; }

    bool RecomposeBound()
    {
        if (Retired()) return false;

        ETCS::RID root_rid;
        std::array<float, 4> clear;
        {
            std::lock_guard<std::mutex> lock(m_rasterMutex);
            root_rid = m_composeRoot;
            clear    = m_clearColor;
        }
        if (root_rid == 0) return false;

        ETCS::Held<Drawable_> root = ETCS::resolve_held<Drawable_>("Drawable", root_rid);
        if (!root)
        {
            ETCS_LOG("CanvasSurface", "compose root RID:" << root_rid
                     << " is gone or going -- unbinding.");
            SetComposeRoot(0);
            return false;
        }

        ClearConcrete(clear[0], clear[1], clear[2], clear[3]);
        root->DrawInto(this);
        return true;
    }

    // The rate this surface is sustaining, sampled at Present because that is
    // the instant a frame reached the screen. Exponential rather than a window:
    // the only consumer is a human reading a number (TextLabel::BindFps).
    float Fps() const { return m_fps; }

    // ── Lifecycle_ ───────────────────────────────────────────────────────────
    //
    // Let go of the tree before teardown. A bound root means the frame edge is
    // walking somebody else's entities, and a closure ending reclaims those
    // while the walk may still be running.
    void ReleaseConcrete()
    {
        ETCS_LOG("CanvasSurface", "release: unbinding the compose root and marking the "
                 "surface dead (RID:" << getRID() << ").");
        SetComposeRoot(0);
        m_dead = true;
    }

    // Three ways of being past it, and a walk is invalid under any:
    //   m_dead      the WINDOW answer  -- this surface is going
    //   Released()  the GRAPH answer   -- it has let go of what it held
    //   Halted()    the REQUEST answer -- somebody asked the bodies to stop
    bool Retired() const { return m_dead || Released() || Halted(); }

    // Stop() is ThreadedBase's and is final there -- the frame edge is stopped by
    // the family, not by this backend.
    bool IsActive() const { return !Retired() && PixelWidth() != 0; }

    // ── Deletable_ ───────────────────────────────────────────────────────────

    bool DeleteConcrete() override
    {
        std::string conjugate_key = getSourceModule().toString() + ":" + getSourceTag().toString();
        ETCS_LOG("CanvasSurface", "firing self-DestroyEvent for RID:" << getRID());
        return ETCS::DestroyEvent{conjugate_key.c_str(), this}();
    }

    // Manual-verification convenience only, matching the device backend's: .etcs
    // has no loop construct yet, so a Clear/DrawRect/Present cycle has to happen
    // inside one work func.
    void RunDemo(ETCS::Entity* windowEntity, uint32_t frameCount)
    {
        Window_* win = static_cast<Window_*>(
            windowEntity->getInterfacePointer(ETCS::Buffer("Window")));
        if (!win) { ETCS_LOG("CanvasSurface", "RunDemo: parent has no Window interface pointer."); return; }

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
    // Reallocate to the window's size at a frame boundary. Allocate is
    // idempotent for an unchanged size, so this is a comparison in the common
    // case; a real change loses the frame in progress, which is what a resize is.
    void PollResize()
    {
        if (!m_parentResizable) return;
        const WindowSize want = m_parentResizable->GetSize();
        if (want.width == 0 || want.height == 0) return;
        if (want.width == PixelWidth() && want.height == PixelHeight()) return;

        std::lock_guard<std::mutex> lock(m_rasterMutex);
        ETCS_LOG("CanvasSurface", "resize " << PixelWidth() << "x" << PixelHeight()
                 << " -> " << want.width << "x" << want.height);
        Allocate(want.width, want.height);
        m_front.clear();
        m_front_w = m_front_h = 0;
    }

    /*
     * Whether the page actually has the element this surface is aimed at. Asked
     * once at Create and once per SetTarget, purely so a typo in an id reads as a
     * named complaint in the log rather than as a canvas that stays blank for no
     * stated reason.
     */
    bool TargetExists() const
    {
#if defined(__EMSCRIPTEN__)
        return MAIN_THREAD_EM_ASM_INT({
            return document.getElementById(UTF8ToString($0)) ? 1 : 0;
        }, m_target.c_str()) != 0;
#else
        return false;
#endif
    }

    void notePresent()
    {
        using clock = ::std::chrono::steady_clock;
        const double now = ::std::chrono::duration<double>(
            clock::now().time_since_epoch()).count();
        if (m_lastPresent > 0.0)
        {
            const double dt = now - m_lastPresent;
            if (dt > 0.0)
            {
                const float inst = static_cast<float>(1.0 / dt);
                m_fps = (m_fps <= 0.0f) ? inst : (m_fps * 0.9f + inst * 0.1f);
            }
        }
        m_lastPresent = now;
    }

    CanvasInstance* m_instance        = nullptr;
    Resizable_*     m_parentResizable = nullptr;
    /*
     * The canvas element this surface presents to. Defaults to "canvas" because
     * that is the id emscripten's own machinery assumes and the id every page in
     * this tree has used so far, so a session that never mentions a target
     * behaves exactly as it did before targets existed.
     *
     * Under m_rasterMutex with the pixels, not beside them: Present reads the
     * bytes and the destination together, and a SetTarget landing between the two
     * would put one surface's raster on another surface's canvas for a frame.
     */
    std::string     m_target          = "canvas";

    // One mutex over the raster. Every entry point either writes pixels or reads
    // them all at once, so there is nothing finer to lock.
    std::mutex            m_rasterMutex;
    std::array<float, 4>  m_clearColor { 0.0f, 0.0f, 0.0f, 1.0f };
    ETCS::RID             m_composeRoot = 0;

    // Front buffer -- last snapshot Present published. Draws never touch it.
    std::vector<uint8_t>  m_front;
    uint32_t              m_front_w = 0;
    uint32_t              m_front_h = 0;

    bool   m_dead        = false;
    float  m_fps         = 0.0f;
    double m_lastPresent = 0.0;
};

#endif // RENDERPROVIDER_CANVASSURFACE_H__
