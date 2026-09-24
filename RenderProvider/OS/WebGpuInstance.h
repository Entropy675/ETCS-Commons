#ifndef RENDERPROVIDER_WEBGPUINSTANCE_H__
#define RENDERPROVIDER_WEBGPUINSTANCE_H__

#include "../../../ontology.h"
#include "WebGpuJs.h"

#include <atomic>
#include <cstdint>

/*
 * WebGpuInstance -- the browser's GPU, when it has one.
 *
 * [Deletable], flat, like VulkanInstance: Instance is resolved through this
 * module's own "Instance" tag row (rp_resolve_tag, Device.h), never
 * generically. The Device a script spawns under a surface names one of these,
 * and the surface draws through WebGPU while it is ready (HostSurface.h).
 *
 * ASYNCHRONOUS, AND THAT IS THE ONLY DIFFERENCE FROM THE NATIVE ONE. A
 * browser hands out its adapter and device through promises, so Create only
 * STARTS the request: it answers whether this page has WebGPU at all, and
 * GetDevice stays null until the device arrives. Nothing waits for it --
 * DeviceReady is read per frame (Device.h), so a surface switches on the
 * frame the device shows up, and back to its host raster the frame it is
 * lost. That is the same derivation the camera already makes (ontology/
 * Camera.h), extended over time rather than only over the graph.
 *
 * The state is an int32 the page writes with Atomics.store from the promise
 * callbacks: 0 pending, 1 ready, -1 failed or lost.
 */
class WebGpuInstance : public DeletableBase<WebGpuInstance>
{
public:
    WIRE_TYPE_IDENTITY(WebGpuInstance);

    WebGpuInstance()  = default;
    ~WebGpuInstance() { forget(); }

    bool Create()
    {
        if (m_created) return m_state.load() >= 0;
#if defined(__EMSCRIPTEN__)
        rp_webgpu_install();
        const int ok = MAIN_THREAD_EM_ASM_INT({
            return Module.etcsGpu.open($0);
        }, reinterpret_cast<int32_t*>(&m_state));
        if (!ok)
        {
            ETCS_LOG("WebGpuInstance", "this page has no WebGPU (navigator.gpu) -- surfaces "
                     "that name this Instance keep drawing on the host.");
            m_state.store(-1);
            return false;
        }
        m_created = true;
        this->addTag("active");
        ETCS_LOG("WebGpuInstance", "requested a WebGPU device (RID:" << getRID()
                 << ") -- surfaces switch to it when it arrives.");
        return true;
#else
        ETCS_LOG("WebGpuInstance", "WebGPU exists only in a browser build.");
        return false;
#endif
    }

    // Created and not failed: a Device naming this may still become ready.
    bool Usable() const { return m_created && m_state.load() >= 0; }
    bool IsActive() const { return m_created && m_state.load() > 0; }

    // Device.h compares this for equality and asks nothing else of it.
    const void* GetDevice() const { return m_state.load() > 0 ? static_cast<const void*>(this) : nullptr; }

    bool DeleteConcrete() override
    {
        forget();
        std::string conjugate_key = getSourceModule().toString() + ":" + getSourceTag().toString();
        ETCS_LOG("WebGpuInstance", "firing self-DestroyEvent for RID:" << getRID());
        return ETCS::DestroyEvent{conjugate_key.c_str(), this}();
    }

private:
    // The page must stop writing into this object before it goes.
    void forget()
    {
#if defined(__EMSCRIPTEN__)
        if (!m_created) return;
        m_created = false;
        MAIN_THREAD_EM_ASM({
            if (Module.etcsGpu) Module.etcsGpu.close($0);
        }, reinterpret_cast<int32_t*>(&m_state));
#endif
    }

    // Lock-free and exactly an int32 in memory, so the page's Atomics.store
    // on its address is a store to this.
    std::atomic<int32_t> m_state{ 0 };
    static_assert(sizeof(std::atomic<int32_t>) == sizeof(int32_t), "state is written by the page as an int32");
    bool m_created = false;
};

#endif // RENDERPROVIDER_WEBGPUINSTANCE_H__
