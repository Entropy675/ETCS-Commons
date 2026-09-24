#ifndef RENDERPROVIDER_HOSTSURFACE_H__
#define RENDERPROVIDER_HOSTSURFACE_H__

#include "../../../ontology.h"
#include "DeviceFrame.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#include <emscripten/em_asm.h>
#include <emscripten/threading.h>
#include "WebGpuPresenter.h"
typedef WebGpuPresenter PlatformPresenter;
#else
#include "VulkanPresenter.h"
typedef VulkanPresenter PlatformPresenter;
#endif

/*
 * HostSurface -- the window surface, on every platform.
 *
 * [Surface + Pixels + Presentable + Resizable + Deletable + Lifecycle + Threaded]
 *
 * A HOST RASTER FIRST, because there is always a CPU (ontology/Device.h).
 * Clear, DrawRect and Blit write a back buffer of host bytes, Present
 * snapshots it and hands it to the window: putImageData in a browser,
 * XPutImage on X11, StretchDIBits on Windows. That floor needs no device at
 * all, so a machine with no GPU -- or a browser with no WebGPU -- still gets
 * its window.
 *
 * AND A DEVICE WHEN IT HAS ONE, the same way a camera does: a ready Device
 * child (RenderProvider::Device, naming an Instance) switches this surface to
 * drawing through that device, and losing it switches back. The effective
 * sink is derived per frame from the graph -- a ready Device under this
 * surface, the standing preference (UseDevice, yes by default), and a backend
 * that has not failed -- so it can never disagree with what the graph says.
 * The backend is VulkanPresenter natively and WebGpuPresenter in a browser
 * (DeviceFrame.h).
 *
 * EVERY DRAW IS RECORDED, on the host as well, as one ordered list the way the
 * device backends always kept it; the host also rasterises. So a switch in
 * either direction keeps the picture: to the device, the list is the frame
 * and any source it has not seen is sent; back to the host, the list is
 * rasterised once into the buffer before it presents. A bound compose root
 * redraws every tick anyway; a script that drew once and stopped keeps its
 * picture across the switch because of this.
 *
 * In device mode the host bytes are not kept current -- the frame never
 * exists in host memory, which is the point -- so a reader of this surface's
 * Pixels_ sees the last host frame.
 *
 * CLEAR IS THE COMPOSITION BOUNDARY: Clear starts a picture, DrawRect and Blit
 * append, Present snapshots. Draws do not mark observed -- a mark per draw
 * would wake the frame edge mid-batch and present a half-composed frame (the
 * drag flicker); the code that finishes a logical frame marks it.
 */
class HostSurface : public SurfaceBase<HostSurface>,
                    public PixelsBase<HostSurface>,
                    public PresentableBase<HostSurface>,
                    public DeletableBase<HostSurface>,
                    public LifecycleBase<HostSurface>,
                    public ThreadedBase<HostSurface>
{
public:
    int32_t m_order = 0;
    bool operator<(const HostSurface& o) const { return m_order < o.m_order; }
    WIRE_TYPE_IDENTITY(HostSurface);

    HostSurface()  = default;
    ~HostSurface() { m_presenter.reset(); closeHostPresenter(); }

    /*
     * The parent link IS the binding between a surface and the window it draws
     * into, which is why a Surface is a typed CHILD of a Window. shader_dir is
     * the Vulkan backend's override for where its SPIR-V is; nothing else
     * reads it. No Instance here: the host raster needs none, and a device is
     * a Device child (Surface.Create(@gpu) spawns one for the caller).
     */
    bool Create(const std::string& shader_dir)
    {
        if (PixelWidth() != 0 && PixelHeight() != 0) return true;   // idempotent
        m_shaderDir = shader_dir;

        ETCS::Entity* parent = getParent();
        if (!parent)
        {
            ETCS_LOG("HostSurface", "Create: no parent -- a surface is a typed child of the "
                     "window it draws into.");
            return false;
        }
        m_window = static_cast<Window_*>(parent->getInterfacePointer(ETCS::Buffer("Window")));
        m_parentResizable = static_cast<Resizable_*>(parent->getInterfacePointer(ETCS::Buffer("Resizable")));
        if (!m_window || !m_parentResizable)
        {
            ETCS_LOG("HostSurface", "Create: parent RID:" << parent->getRID()
                     << " is not a resizable Window.");
            return false;
        }
        const WindowSize size = m_parentResizable->GetSize();
        if (size.width == 0 || size.height == 0)
        {
            ETCS_LOG("HostSurface", "Create: the window reports a zero dimension ("
                     << size.width << "x" << size.height << ").");
            return false;
        }
        Allocate(size.width, size.height);
        this->FollowResize(m_parentResizable);
        this->addTag("active");
        ETCS_LOG("HostSurface", "ready (RID:" << getRID() << ") " << size.width << "x" << size.height
                 << " on the host" << targetNote() << " -- a Device child switches it to the GPU.");
        return true;
    }

    /*
     * WHICH OUTPUT THIS SURFACE PRESENTS TO, by name. In a browser the name is a
     * canvas element's id and a page may hold any number of them (a toolbar
     * strip beside the main view) -- emscripten's GLFW owns one canvas, so a
     * second SURFACE, not a second window, is how a page gets a second drawing
     * area. Natively a window has exactly one output, so a name other than the
     * window's own is refused rather than remembered.
     */
    bool SetTarget(const std::string& element_id)
    {
#if defined(__EMSCRIPTEN__)
        if (element_id.empty())
        {
            ETCS_LOG("HostSurface", "SetTarget: an empty id names nothing -- keeping #" << m_target << ".");
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(m_rasterMutex);
            m_target = element_id;
        }
        ETCS_LOG("HostSurface", "target -> #" << m_target
                 << (TargetExists() ? "" : " (NO SUCH ELEMENT on the page)"));
        return true;
#else
        if (element_id.empty() || element_id == "window") return true;
        ETCS_LOG("HostSurface", "SetTarget('" << element_id << "'): a native window has one "
                 "output. Ignoring -- named targets are the browser's.");
        return false;
#endif
    }

    const std::string& Target() const { return m_target; }

    /*
     * AN EXPLICIT SIZE WINS OVER THE WINDOW'S, which is what lets a surface be a
     * REGION rather than the whole frame. Following and being told cannot both
     * be live -- PollResize would snap it back -- so this drops the follow.
     */
    bool ResizeTo(WindowSize sz) override
    {
        if (sz.width == 0 || sz.height == 0)
        {
            ETCS_LOG("HostSurface", "ResizeTo: a zero dimension is not a size ("
                     << sz.width << "x" << sz.height << ").");
            return false;
        }
        if (sz.width == PixelWidth() && sz.height == PixelHeight()) return true;
        std::lock_guard<std::mutex> lock(m_rasterMutex);
        ETCS_LOG("HostSurface", "resize (explicit) " << PixelWidth() << "x" << PixelHeight()
                 << " -> " << sz.width << "x" << sz.height << " -- no longer following the window.");
        m_parentResizable = nullptr;
        Allocate(sz.width, sz.height);
        m_front.clear();
        return true;
    }

    /*
     * THE STANDING PREFERENCE, the camera's toggle for a surface: yes by
     * default, so attaching a Device is the whole of switching; no keeps this
     * surface on the host with a device still attached. Takes effect at the
     * next frame boundary.
     */
    void UseDevice(bool on)
    {
        m_wantDevice.store(on);
        etcs_mark_observed(this);
    }
    bool DeviceWanted() const { return m_wantDevice.load(); }
    // What the last frame was drawn by -- the answer, not the wish.
    bool OnDevice() const { return m_onDevice.load(); }

    // ── Surface_ dispatch ────────────────────────────────────────────────────

    void ClearConcrete(float r, float g, float b, float a) override
    {
        std::lock_guard<std::mutex> lock(m_rasterMutex);
        m_clearColor = { r, g, b, a };
        m_ops.clear();
        if (!m_device) rasterClearLocked(r, g, b, a);
    }

    void DrawRectConcrete(int32_t x, int32_t y, uint32_t w, uint32_t h,
                          float r, float g, float b, float a) override
    {
        std::lock_guard<std::mutex> lock(m_rasterMutex);
        DeviceOp op;
        op.kind = DeviceOp::Kind::Rect;
        op.x = x; op.y = y; op.w = w; op.h = h;
        op.c[0] = r; op.c[1] = g; op.c[2] = b; op.c[3] = a;
        m_ops.push_back(op);
        if (!m_device) rasterRectLocked(x, y, w, h, r, g, b, a);
    }

    /*
     * Any Pixels_ source, from any module: a paint layer, a camera's frame, an
     * ImageSurface. On the host it is composited now; on the device its bytes
     * are copied now if they changed since this surface last took them (asked
     * as THIS surface, so two windows over one image each get their own
     * answer), and drawn at Present from the device's copy.
     */
    void BlitConcrete(Surface_* source, int32_t x, int32_t y,
                      uint32_t w, uint32_t h, float opacity) override
    {
        if (!source) { ETCS_LOG("HostSurface", "Blit called with no source."); return; }
        Pixels_* px = static_cast<Pixels_*>(source->getInterfacePointer(ETCS::Buffer("Pixels")));
        if (!px)
        {
            ETCS_LOG("HostSurface", "Blit source RID:" << source->getRID()
                     << " owns no pixels -- a device-side source cannot be read back here.");
            return;
        }
        if (px == static_cast<Pixels_*>(this))
        {
            ETCS_LOG("HostSurface", "Blit source is this surface -- refusing to composite onto itself.");
            return;
        }
        if (px->RasterEmpty() || opacity <= 0.0f) return;

        std::lock_guard<std::mutex> lock(m_rasterMutex);
        DeviceOp op;
        op.kind = DeviceOp::Kind::Blit;
        op.x = x; op.y = y;
        op.w = w ? w : px->PixelWidth();
        op.h = h ? h : px->PixelHeight();
        op.c[3] = opacity;
        op.source = source->getRID();
        m_ops.push_back(op);
        if (m_device) captureLocked(source, *px);
        else          rasterBlitLocked(*px, x, y, w, h, opacity);
    }

    // ── Presentable_ dispatch ────────────────────────────────────────────────
    /*
     * The frame edge's one call, on its thread. Resize first, then which sink
     * this frame goes to (the graph, asked now), then the frame itself.
     */
    void PresentConcrete() override
    {
        if (Retired()) return;
        PollResize();
        chooseSink();

        if (m_presenter)
        {
            DeviceFrame frame;
            {
                std::lock_guard<std::mutex> lock(m_rasterMutex);
                frame.clear  = m_clearColor;
                frame.ops    = m_ops;
                frame.width  = PixelWidth();
                frame.height = PixelHeight();
                frame.uploads.reserve(m_uploads.size());
                for (auto& [rid, up] : m_uploads)
                {
                    (void)rid;
                    frame.uploads.push_back(std::move(up));
                }
                m_uploads.clear();
            }
            sendMissing(frame);
            if (m_presenter->Present(frame))
            {
                std::lock_guard<std::mutex> lock(m_rasterMutex);
                for (const DeviceUpload& up : frame.uploads) m_sent.insert(up.source);
                return;
            }
            // Finished (lost device, lost window): this key is not tried again
            // until the graph changes, and this very frame goes out on the host.
            m_failedKey = m_presenterKey;
            leaveDevice("the device stopped presenting");
        }
        presentHost();
    }

    // ── Resizable_ dispatch ──────────────────────────────────────────────────

    WindowSize GetSizeConcrete() override { return { PixelWidth(), PixelHeight() }; }

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

    /*
     * HELD, not merely resolved: the walk lasts a whole tree, and a root deleted
     * mid-walk would otherwise be freed under DrawInto. Nothing inside the hold
     * may emit an ordered event -- the Delete waiting on it may be ahead of it
     * in the queue. Refused once retired: on the teardown paths the graph is
     * coming apart while the frame edge is still going round.
     */
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
            ETCS_LOG("HostSurface", "compose root RID:" << root_rid << " is gone or going -- unbinding.");
            SetComposeRoot(0);
            return false;
        }
        ClearConcrete(clear[0], clear[1], clear[2], clear[3]);
        root->DrawInto(this);
        return true;
    }

    // ── Lifecycle_ ───────────────────────────────────────────────────────────
    // Let go of the tree before teardown: a closure ending reclaims the
    // entities a bound root's walk may still be touching.
    void ReleaseConcrete()
    {
        ETCS_LOG("HostSurface", "release: unbinding the compose root and marking the "
                 "surface dead (RID:" << getRID() << ").");
        SetComposeRoot(0);
        m_dead = true;
    }

    // Three ways of being past it, and a walk is invalid under any:
    //   m_dead      the WINDOW answer  -- this surface is going
    //   Released()  the GRAPH answer   -- it has let go of what it held
    //   Halted()    the REQUEST answer -- somebody asked the bodies to stop
    bool Retired() const { return m_dead || Released() || Halted(); }
    bool IsActive() const { return !Retired() && PixelWidth() != 0; }
    bool CanPresentConcrete() override { return !Retired() && IsActive(); }

    // ── Deletable_ ───────────────────────────────────────────────────────────

    bool DeleteConcrete() override
    {
        std::string conjugate_key = getSourceModule().toString() + ":" + getSourceTag().toString();
        ETCS_LOG("HostSurface", "firing self-DestroyEvent for RID:" << getRID());
        return ETCS::DestroyEvent{conjugate_key.c_str(), this}();
    }

    // Manual-verification convenience only: .etcs has no loop construct, so a
    // Clear/DrawRect/Present cycle has to happen inside one work func.
    void RunDemo(ETCS::Entity* windowEntity, uint32_t frameCount)
    {
        Window_* win = static_cast<Window_*>(windowEntity->getInterfacePointer(ETCS::Buffer("Window")));
        if (!win) { ETCS_LOG("HostSurface", "RunDemo: parent has no Window interface pointer."); return; }
        for (uint32_t i = 0; i < frameCount && !win->ShouldClose(); ++i)
        {
            win->PollEvents();
            ClearConcrete(0.05f, 0.05f, 0.08f, 1.0f);
            DrawRectConcrete(40, 40, 200, 120, 0.85f, 0.2f, 0.2f, 1.0f);
            DrawRectConcrete(260, 160, 150, 150, 0.2f, 0.7f, 0.3f, 1.0f);
            Present();
        }
    }

private:
    // ── the sink ─────────────────────────────────────────────────────────────

    /*
     * The first ready RenderProvider::Device among this surface's children --
     * the camera's walk (ontology/Camera.h), narrowed to this module's own
     * Device because making a backend needs its Instance, which the family
     * does not hand out. First ready one wins, as there.
     */
    Device* readyDevice()
    {
        std::vector<std::pair<ETCS::Buffer, ETCS::RID>> kids;
        this->getTypedChildren(kids);
        for (const auto& entry : kids)
        {
            ETCS::Entity* child = this->getTypedChild(entry.first, entry.second);
            if (!child || !child->getInterfacePointer(ETCS::Buffer("Device"))) continue;
            if (child->getSourceTag() != ETCS::Buffer("Device")) continue;
            Device* d = static_cast<Device*>(child->getTrueType());
            if (d && d->DeviceReady()) return d;
        }
        return nullptr;
    }

    // Which sink this frame goes to, and the transition if it changed.
    void chooseSink()
    {
        Device* d = m_wantDevice.load() ? readyDevice() : nullptr;
        const uint64_t key = d ? d->DeviceKey() : 0;
        if (key == 0) m_failedKey = 0;          // the graph changed: a later device is tried afresh

        if (m_presenter && key != m_presenterKey)
            leaveDevice(key == 0 ? (m_wantDevice.load() ? "the Device is gone or not ready"
                                                        : "UseDevice(0)")
                                 : "a different device");
        if (m_presenter || key == 0 || key == m_failedKey) return;

        std::unique_ptr<PlatformPresenter> p = makePresenter(d->ResolveInstance());
        if (!p)
        {
            m_failedKey = key;
            ETCS_LOG("HostSurface", "RID:" << getRID() << " could not bring up the device "
                     "backend -- staying on the host.");
            return;
        }
        m_presenter    = std::move(p);
        m_presenterKey = key;
        {
            std::lock_guard<std::mutex> lock(m_rasterMutex);
            m_device = true;
            m_uploads.clear();
            m_sent.clear();
        }
        m_onDevice.store(true);
        ETCS_LOG("HostSurface", "RID:" << getRID() << " now draws through the device ("
                 << backendName() << ").");
    }

    std::unique_ptr<PlatformPresenter> makePresenter(Instance* instance)
    {
        if (!instance) return nullptr;
#if defined(__EMSCRIPTEN__)
        std::string target;
        { std::lock_guard<std::mutex> lock(m_rasterMutex); target = m_target; }
        return std::unique_ptr<PlatformPresenter>(new WebGpuPresenter(instance, target));
#else
        if (!m_window) return nullptr;
        std::unique_ptr<PlatformPresenter> p(new VulkanPresenter(instance, m_window->GetNativeSurfaceHandle()));
        if (!p->Create(PixelWidth(), PixelHeight(), m_shaderDir)) return nullptr;
        return p;
#endif
    }

    // Back to the host: the recording becomes host pixels once, so a
    // retained picture survives the switch.
    void leaveDevice(const char* why)
    {
        m_presenter.reset();
        m_presenterKey = 0;
        {
            std::lock_guard<std::mutex> lock(m_rasterMutex);
            m_device = false;
            m_uploads.clear();
            m_sent.clear();
            rasterizeRecordedLocked();
        }
        m_onDevice.store(false);
        ETCS_LOG("HostSurface", "RID:" << getRID() << " back on the host -- " << why << ".");
    }

    static const char* backendName()
    {
#if defined(__EMSCRIPTEN__)
        return "WebGPU";
#else
        return "Vulkan";
#endif
    }

    // Device mode: a source's bytes, copied if the device does not have them
    // as they are now. Observing starts at the first copy, which is also why
    // the first copy is unconditional. Caller holds m_rasterMutex.
    void captureLocked(Surface_* source, const Pixels_& px)
    {
        const ETCS::RID rid = source->getRID();
        ETCS::IWireObservable* obs = etcs_observable_of(static_cast<ETCS::Entity*>(source));
        bool changed = true;
        if (m_observing.insert(rid).second) { if (obs) obs->Observe(getRID()); }
        else if (obs) changed = obs->TakeObserved(getRID());
        if (!changed && m_sent.count(rid)) return;
        DeviceUpload& up = m_uploads[rid];
        up.source = rid;
        up.w = px.PixelWidth();
        up.h = px.PixelHeight();
        up.bytes.assign(px.PixelData(), px.PixelData() + px.PixelBytes());
    }

    /*
     * A source this device has never been sent -- the surface just switched,
     * or the backend was remade -- read now, off the frame thread's own hold
     * on it. Sources the device has already had come through captureLocked
     * whenever they change, so only the unknown ones are resolved here.
     */
    void sendMissing(DeviceFrame& frame)
    {
        std::unordered_set<ETCS::RID> sent;
        { std::lock_guard<std::mutex> lock(m_rasterMutex); sent = m_sent; }
        std::unordered_set<ETCS::RID> queued;
        for (const DeviceUpload& up : frame.uploads) queued.insert(up.source);
        for (const DeviceOp& op : frame.ops)
        {
            if (op.kind != DeviceOp::Kind::Blit || sent.count(op.source) || queued.count(op.source)) continue;
            queued.insert(op.source);
            ETCS::Held<Surface_> src = ETCS::resolve_held<Surface_>("Surface", op.source);
            if (!src) continue;
            Pixels_* px = static_cast<Pixels_*>(src->getInterfacePointer(ETCS::Buffer("Pixels")));
            if (!px || px->RasterEmpty() || !px->PixelData()) continue;
            DeviceUpload up;
            up.source = op.source;
            up.w = px->PixelWidth();
            up.h = px->PixelHeight();
            up.bytes.assign(px->PixelData(), px->PixelData() + px->PixelBytes());
            frame.uploads.push_back(std::move(up));
        }
    }

    // ── the host raster ──────────────────────────────────────────────────────
    // Written through PixelData rather than FillRect/Composite, which mark:
    // see the class comment on why a draw is not a frame here.

    static uint8_t toByte(float v) { return static_cast<uint8_t>(v <= 0.f ? 0 : v >= 1.f ? 255 : v * 255.f + 0.5f); }

    void rasterClearLocked(float r, float g, float b, float a)
    {
        uint8_t* data = PixelData();
        if (!data) return;
        const uint8_t px[4] = { toByte(r), toByte(g), toByte(b), toByte(a) };
        const size_t n = PixelBytes();
        for (size_t i = 0; i < n; i += 4) std::memcpy(data + i, px, 4);
    }

    void rasterRectLocked(int32_t x, int32_t y, uint32_t w, uint32_t h, float r, float g, float b, float a)
    {
        uint8_t* base = PixelData();
        if (!base || a <= 0.0f) return;
        const uint32_t pw = PixelWidth(), ph = PixelHeight();
        if (pw == 0 || ph == 0) return;
        const int32_t x0 = x < 0 ? 0 : x;
        const int32_t y0 = y < 0 ? 0 : y;
        const int32_t x1 = std::min(static_cast<int32_t>(pw), x + static_cast<int32_t>(w));
        const int32_t y1 = std::min(static_cast<int32_t>(ph), y + static_cast<int32_t>(h));
        if (x0 >= x1 || y0 >= y1) return;
        const uint8_t sr = toByte(r), sg = toByte(g), sb = toByte(b), sa = toByte(a);
        const uint32_t stride = PixelStride();
        for (int32_t py = y0; py < y1; ++py)
        {
            uint8_t* row = base + static_cast<size_t>(py) * stride;
            for (int32_t pxl = x0; pxl < x1; ++pxl)
            {
                uint8_t* d = row + static_cast<size_t>(pxl) * 4;
                if (sa == 255) { d[0] = sr; d[1] = sg; d[2] = sb; d[3] = 255; continue; }
                const uint32_t inv = 255u - sa, da = d[3];
                const uint32_t oa = sa + (da * inv) / 255u;
                if (oa == 0) { d[0] = d[1] = d[2] = d[3] = 0; continue; }
                d[0] = static_cast<uint8_t>((sr * sa + d[0] * da * inv / 255u) / oa);
                d[1] = static_cast<uint8_t>((sg * sa + d[1] * da * inv / 255u) / oa);
                d[2] = static_cast<uint8_t>((sb * sa + d[2] * da * inv / 255u) / oa);
                d[3] = static_cast<uint8_t>(oa);
            }
        }
    }

    void rasterBlitLocked(const Pixels_& src, int32_t x, int32_t y, uint32_t w, uint32_t h, float opacity)
    {
        if (PixelBytes() == 0 || src.PixelBytes() == 0 || opacity <= 0.0f) return;
        // w/h are the destination extent when they differ from the source's
        // own size (ontology/ScaledComposite.h).
        if (render_blit_is_scaled(src, w, h))
        {
            render_composite_scaled(*this, src, x, y, w, h, opacity);
            return;
        }
        const uint32_t dw = PixelWidth(), dh = PixelHeight();
        const uint32_t sw = src.PixelWidth(), sh = src.PixelHeight();
        const uint32_t dstride = PixelStride(), sstride = src.PixelStride();
        uint8_t* dst = PixelData();
        const uint8_t* s0 = src.PixelData();
        if (!dst || !s0) return;
        const float op = opacity > 1.0f ? 1.0f : opacity;
        for (uint32_t sy = 0; sy < sh; ++sy)
        {
            const int64_t dy = static_cast<int64_t>(y) + sy;
            if (dy < 0 || dy >= static_cast<int64_t>(dh)) continue;
            const uint8_t* srow = s0 + static_cast<size_t>(sy) * sstride;
            uint8_t*       drow = dst + static_cast<size_t>(dy) * dstride;
            for (uint32_t sx = 0; sx < sw; ++sx)
            {
                const int64_t dx = static_cast<int64_t>(x) + sx;
                if (dx < 0 || dx >= static_cast<int64_t>(dw)) continue;
                const uint8_t* s = srow + sx * 4;
                uint8_t* d = drow + static_cast<size_t>(dx) * 4;
                const uint8_t sa = static_cast<uint8_t>(s[3] * op);
                if (sa == 0) continue;
                if (sa == 255) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255; continue; }
                const uint32_t inv = 255u - sa, da = d[3];
                const uint32_t oa = sa + (da * inv) / 255u;
                if (oa == 0) { d[0] = d[1] = d[2] = d[3] = 0; continue; }
                d[0] = static_cast<uint8_t>((s[0] * sa + d[0] * da * inv / 255u) / oa);
                d[1] = static_cast<uint8_t>((s[1] * sa + d[1] * da * inv / 255u) / oa);
                d[2] = static_cast<uint8_t>((s[2] * sa + d[2] * da * inv / 255u) / oa);
                d[3] = static_cast<uint8_t>(oa);
            }
        }
    }

    // The recording, drawn into the host raster once: leaving the device.
    // Sources are resolved now; one that is gone is simply not drawn.
    void rasterizeRecordedLocked()
    {
        rasterClearLocked(m_clearColor[0], m_clearColor[1], m_clearColor[2], m_clearColor[3]);
        for (const DeviceOp& op : m_ops)
        {
            if (op.kind == DeviceOp::Kind::Rect)
            {
                rasterRectLocked(op.x, op.y, op.w, op.h, op.c[0], op.c[1], op.c[2], op.c[3]);
                continue;
            }
            ETCS::Held<Surface_> src = ETCS::resolve_held<Surface_>("Surface", op.source);
            if (!src) continue;
            Pixels_* px = static_cast<Pixels_*>(src->getInterfacePointer(ETCS::Buffer("Pixels")));
            if (px && px != static_cast<Pixels_*>(this)) rasterBlitLocked(*px, op.x, op.y, op.w, op.h, op.c[3]);
        }
    }

    // Reallocate to the window's size at a frame boundary; Allocate is
    // idempotent for an unchanged size, so the common case is a comparison.
    void PollResize()
    {
        if (!m_parentResizable) return;
        const WindowSize want = m_parentResizable->GetSize();
        if (want.width == 0 || want.height == 0) return;
        if (want.width == PixelWidth() && want.height == PixelHeight()) return;
        std::lock_guard<std::mutex> lock(m_rasterMutex);
        ETCS_LOG("HostSurface", "resize " << PixelWidth() << "x" << PixelHeight()
                 << " -> " << want.width << "x" << want.height);
        Allocate(want.width, want.height);
        m_front.clear();
    }

    // ── the host present, per platform ───────────────────────────────────────
    /*
     * Snapshot back -> front under the lock, then hand the front to the window
     * with the lock released: the window call can take a vsync, and the drawers
     * must not wait for it. The front is what the platform call reads, so the
     * drawers cannot move it underneath.
     */
    void presentHost()
    {
        uint32_t w = 0, h = 0;
        std::string target;
        {
            std::lock_guard<std::mutex> lock(m_rasterMutex);
            const uint8_t* data = PixelData();
            w = PixelWidth();
            h = PixelHeight();
            if (!data || w == 0 || h == 0) return;
            m_front.resize(static_cast<size_t>(w) * h * 4);
            std::memcpy(m_front.data(), data, m_front.size());
            target = m_target;
        }
        if (m_front.empty()) return;
#if defined(__EMSCRIPTEN__)
        /*
         * ONE DECLARATION PER STATEMENT and no bare commas: the block is a macro
         * argument, and only PARENTHESES protect a comma from the split.
         * By id, never Module.canvas -- that is emscripten's one canvas, and
         * drawing there would put this surface over whichever one that is.
         */
        MAIN_THREAD_EM_ASM({
            var w = $1;
            var h = $2;
            var id = UTF8ToString($3);
            var canvas = document.getElementById(id);
            if (!canvas) return;
            if (canvas.width !== w)  canvas.width  = w;
            if (canvas.height !== h) canvas.height = h;
            var ctx = canvas.getContext('2d');
            if (!ctx) return;
            var img = ctx.createImageData(w, h);
            img.data.set(HEAPU8.subarray($0, $0 + w * h * 4));
            ctx.putImageData(img, 0, 0);
        }, m_front.data(), w, h, target.c_str());
#elif defined(_WIN32)
        const NativeSurfaceHandle& nh = m_window->GetNativeSurfaceHandle();
        if (nh.platform != NativeSurfacePlatform::Win32) return;
        toBGRX(w, h, 16, 8, 0);
        HWND hwnd = static_cast<HWND>(nh.win32.hwnd);
        HDC dc = GetDC(hwnd);
        if (!dc) return;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth       = static_cast<LONG>(w);
        bi.bmiHeader.biHeight      = -static_cast<LONG>(h);   // top-down, as the raster is
        bi.bmiHeader.biPlanes      = 1;
        bi.bmiHeader.biBitCount    = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        StretchDIBits(dc, 0, 0, static_cast<int>(w), static_cast<int>(h), 0, 0, static_cast<int>(w), static_cast<int>(h),
                      m_native.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
        ReleaseDC(hwnd, dc);
#else
        if (!openHostPresenter()) return;
        toBGRX(w, h, m_rShift, m_gShift, m_bShift);
        XImage* img = XCreateImage(m_xdpy, m_xvisual, static_cast<unsigned>(m_xdepth), ZPixmap, 0,
                                   reinterpret_cast<char*>(m_native.data()), w, h, 32, static_cast<int>(w * 4));
        if (!img) return;
        XPutImage(m_xdpy, m_xwin, m_xgc, img, 0, 0, 0, 0, w, h);
        img->data = nullptr;      // ours, not Xlib's to free
        XFree(img);
        XFlush(m_xdpy);
#endif
    }

#if !defined(__EMSCRIPTEN__)
    // The front buffer as the window's own pixel layout: one 32-bit word per
    // pixel, each 8-bit channel at its shift. Alpha dropped -- a window is opaque.
    void toBGRX(uint32_t w, uint32_t h, int rs, int gs, int bs)
    {
        const size_t n = static_cast<size_t>(w) * h;
        m_native.resize(n);
        const uint8_t* s = m_front.data();
        for (size_t i = 0; i < n; ++i, s += 4)
            m_native[i] = (static_cast<uint32_t>(s[0]) << rs) | (static_cast<uint32_t>(s[1]) << gs)
                        | (static_cast<uint32_t>(s[2]) << bs);
    }
    std::vector<uint32_t> m_native;
#endif

#if !defined(__EMSCRIPTEN__) && !defined(_WIN32)
    /*
     * A CONNECTION OF OUR OWN to the window's X server, opened from the name of
     * the one the window provider holds. The frame thread draws on it while the
     * window's thread polls events on the other, and two threads on one Xlib
     * connection is the thing Xlib does not promise. Only the window's XID is
     * shared, which is a number, not a connection.
     */
    bool openHostPresenter()
    {
        if (m_xdpy) return true;
        if (m_xfailed || !m_window) return false;
        const NativeSurfaceHandle& nh = m_window->GetNativeSurfaceHandle();
        if (nh.platform != NativeSurfacePlatform::X11 || !nh.x11.display)
        {
            m_xfailed = true;
            ETCS_LOG("HostSurface", "the window gave no X11 handle -- nothing to present to on the host.");
            return false;
        }
        install_x_error_handler();
        m_xdpy = XOpenDisplay(DisplayString(static_cast<Display*>(nh.x11.display)));
        if (!m_xdpy)
        {
            m_xfailed = true;
            ETCS_LOG("HostSurface", "XOpenDisplay failed -- nothing to present to on the host.");
            return false;
        }
        x_ours(m_xdpy, true);
        m_xwin = static_cast<::Window>(nh.x11.window);
        XWindowAttributes attrs{};
        if (!XGetWindowAttributes(m_xdpy, m_xwin, &attrs) || !attrs.visual)
        {
            ETCS_LOG("HostSurface", "the window's attributes could not be read -- nothing to present to.");
            closeHostPresenter();
            m_xfailed = true;
            return false;
        }
        m_xvisual = attrs.visual;
        m_xdepth  = attrs.depth;
        auto shift = [](unsigned long mask) { int s = 0; while (mask && !(mask & 1ul)) { mask >>= 1; ++s; } return s; };
        m_rShift = shift(m_xvisual->red_mask);
        m_gShift = shift(m_xvisual->green_mask);
        m_bShift = shift(m_xvisual->blue_mask);
        if (m_xdepth < 24)
            ETCS_LOG("HostSurface", "the window's visual is " << m_xdepth << "-bit; the host present "
                     "writes 8 bits per channel and colours will be wrong.");
        m_xgc = XCreateGC(m_xdpy, m_xwin, 0, nullptr);
        ETCS_LOG("HostSurface", "host present: XPutImage to window 0x" << std::hex << m_xwin << std::dec
                 << " (depth " << m_xdepth << ").");
        return true;
    }

    /*
     * An X error on OUR connection is logged and survived; Xlib's default is to
     * exit the process, and a window closing between the frame edge's check and
     * its XPutImage is an ordinary race, not a reason to die. Errors on any
     * other connection go to whoever was handling them before.
     */
    static int x_error(Display* dpy, XErrorEvent* e)
    {
        if (x_ours(dpy, false))
        {
            static std::atomic<int> n{ 0 };
            if (n.fetch_add(1) < 5)
                ETCS_LOG("HostSurface", "X error " << static_cast<int>(e->error_code)
                         << " on the host present (request " << static_cast<int>(e->request_code) << ") -- ignored.");
            return 0;
        }
        return x_prev_handler() ? x_prev_handler()(dpy, e) : 0;
    }
    static XErrorHandler& x_prev_handler() { static XErrorHandler h = nullptr; return h; }
    static bool x_ours(Display* dpy, bool add)
    {
        static std::mutex m;
        static std::vector<Display*> ours;
        std::lock_guard<std::mutex> g(m);
        if (add) { ours.push_back(dpy); return true; }
        return std::find(ours.begin(), ours.end(), dpy) != ours.end();
    }
    static void install_x_error_handler()
    {
        static std::once_flag once;
        std::call_once(once, [] { x_prev_handler() = XSetErrorHandler(&HostSurface::x_error); });
    }
#endif

    void closeHostPresenter()
    {
#if !defined(__EMSCRIPTEN__) && !defined(_WIN32)
        if (!m_xdpy) return;
        if (m_xgc) XFreeGC(m_xdpy, m_xgc);
        m_xgc = nullptr;
        XCloseDisplay(m_xdpy);
        m_xdpy = nullptr;
#endif
    }

    std::string targetNote() const
    {
#if defined(__EMSCRIPTEN__)
        return " presenting to #" + m_target + (TargetExists() ? "" : " (NO SUCH ELEMENT on the page yet)");
#else
        return "";
#endif
    }

    // Whether the page has the element this surface is aimed at -- so a typo
    // in an id reads as a named complaint rather than a canvas that stays blank.
    bool TargetExists() const
    {
#if defined(__EMSCRIPTEN__)
        return MAIN_THREAD_EM_ASM_INT({
            return document.getElementById(UTF8ToString($0)) ? 1 : 0;
        }, m_target.c_str()) != 0;
#else
        return true;
#endif
    }

    Window_*    m_window          = nullptr;
    Resizable_* m_parentResizable = nullptr;
    std::string m_shaderDir;
    /*
     * The canvas element this surface presents to in a browser. "canvas" by
     * default: the id emscripten's own machinery assumes. Under m_rasterMutex
     * with the pixels -- Present reads the bytes and the destination together.
     */
    std::string m_target = "canvas";

    // One mutex over the raster and the recording: every entry point writes
    // one or reads both at once.
    std::mutex              m_rasterMutex;
    std::array<float, 4>    m_clearColor { 0.0f, 0.0f, 0.0f, 1.0f };
    ETCS::RID               m_composeRoot = 0;
    std::vector<DeviceOp>   m_ops;                // the composition, in call order
    bool                    m_device = false;     // the sink draws go to -- set only by the frame thread
    std::unordered_map<ETCS::RID, DeviceUpload> m_uploads;   // device mode, not yet presented
    std::unordered_set<ETCS::RID> m_observing;    // sources this surface asked to be told about
    std::unordered_set<ETCS::RID> m_sent;         // sources the current backend holds

    // Front buffer -- the last snapshot the host present handed over.
    std::vector<uint8_t>    m_front;

    // The device backend, frame thread only.
    std::unique_ptr<PlatformPresenter> m_presenter;
    uint64_t                m_presenterKey = 0;
    uint64_t                m_failedKey    = 0;
    std::atomic<bool>       m_wantDevice { true };
    std::atomic<bool>       m_onDevice   { false };

#if !defined(__EMSCRIPTEN__) && !defined(_WIN32)
    Display*  m_xdpy    = nullptr;
    ::Window  m_xwin    = 0;
    Visual*   m_xvisual = nullptr;
    int       m_xdepth  = 0;
    GC        m_xgc     = nullptr;
    int       m_rShift  = 16, m_gShift = 8, m_bShift = 0;
    bool      m_xfailed = false;
#endif

    bool m_dead = false;
};


#endif // RENDERPROVIDER_HOSTSURFACE_H__
