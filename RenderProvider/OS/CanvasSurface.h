#ifndef RENDERPROVIDER_CANVASSURFACE_H__
#define RENDERPROVIDER_CANVASSURFACE_H__

#include "../../../ontology.h"
#include "CanvasInstance.h"

#include <array>
#include <chrono>
#include <mutex>
#include <string>

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
 *   - Clear, DrawRect and Blit are the SAME PixelsBase raster calls ImageSurface
 *     makes. No shaders, no pipelines, no staging buffers, no descriptor sets,
 *     and no swapchain. Composition is already written.
 *
 *   - Blit accepts any Pixels_ source, so a PaintProvider layer or a Camera3D's
 *     projection composites onto the window with the same call it uses to
 *     composite onto another layer.
 *
 *   - Present is the only browser-specific code in the file: the raster goes to
 *     the canvas and nothing else happens.
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
                 << "x" << size.height << " -- presenting to the page canvas.");
        return true;
    }

    // ── Surface_ dispatch ────────────────────────────────────────────────────
    //
    // Straight to the raster. Unlike the device backend there is no pending-draw
    // list, because there is no frame to record into: a call has happened by the
    // time it returns, and Present is a copy of whatever the raster holds.

    void ClearConcrete(float r, float g, float b, float a) override
    {
        std::lock_guard<std::mutex> lock(m_rasterMutex);
        m_clearColor = { r, g, b, a };
        ClearTo(r, g, b, a);
    }

    void DrawRectConcrete(int32_t x, int32_t y, uint32_t w, uint32_t h,
                           float r, float g, float b, float a) override
    {
        std::lock_guard<std::mutex> lock(m_rasterMutex);
        FillRect(x, y, w, h, r, g, b, a);
    }

    // w/h are accepted for signature parity with the family and ignored: this
    // path does not resample (Pixels_::Composite).
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
        Composite(*px, x, y, opacity);
    }

    // ── Presentable_ dispatch ────────────────────────────────────────────────
    /*
     * The raster to the canvas, and the only place the browser appears.
     *
     * MAIN_THREAD_EM_ASM, not EM_ASM: the canvas belongs to the page, and a work
     * func runs on whichever thread called it -- a script at the REPL prompt runs
     * on a Worker, and a Worker's JS scope has no document. This proxies the
     * putImageData to the page's own scope. The pointer crosses unchanged because
     * a -pthread build's memory is shared.
     *
     * SYNCHRONOUS, and measured rather than assumed. The async form removes a
     * round trip per frame and decouples the frame rate from main-thread latency,
     * which is the obvious win -- and it makes the browser's remaining
     * intermittent proxying fault MORE frequent (four loads in six against one in
     * three), as well as letting a Recompose tear a frame the page has not read
     * yet. So it stays synchronous until that fault is understood: the cost is a
     * blocked frame thread, and the thing it buys is a frame the page is finished
     * with before the raster can move under it.
     *
     * Resize is picked up HERE rather than on a resize callback, because a frame
     * boundary is the only point at which reallocating the raster cannot tear a
     * composition somebody is midway through building.
     */
    void PresentConcrete() override
    {
        if (Retired()) return;

        PollResize();

        std::lock_guard<std::mutex> lock(m_rasterMutex);
        const uint8_t* data = PixelData();
        const uint32_t w = PixelWidth();
        const uint32_t h = PixelHeight();
        if (!data || w == 0 || h == 0) return;

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
            var canvas = (typeof Module !== 'undefined' && Module.canvas)
                       ? Module.canvas : document.getElementById('canvas');
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
        }, data, w, h);
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

    // One mutex over the raster. Every entry point either writes pixels or reads
    // them all at once, so there is nothing finer to lock.
    std::mutex            m_rasterMutex;
    std::array<float, 4>  m_clearColor { 0.0f, 0.0f, 0.0f, 1.0f };
    ETCS::RID             m_composeRoot = 0;

    bool   m_dead        = false;
    float  m_fps         = 0.0f;
    double m_lastPresent = 0.0;
};

#endif // RENDERPROVIDER_CANVASSURFACE_H__
