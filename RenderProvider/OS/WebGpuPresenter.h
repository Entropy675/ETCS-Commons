#ifndef RENDERPROVIDER_WEBGPUPRESENTER_H__
#define RENDERPROVIDER_WEBGPUPRESENTER_H__

#include "../../../ontology.h"
#include "DeviceFrame.h"
#include "WebGpuInstance.h"
#include "WebGpuJs.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Textures one surface keeps on the device before the least recently drawn
// one is evicted -- the same bound the Vulkan backend uses.
static constexpr uint32_t WEBGPU_PRESENTER_MAX_SOURCES = 64;

/*
 * WebGpuPresenter -- a browser surface's frames, drawn by WebGPU into an
 * overlay canvas laid over the surface's own (WebGpuJs.h says why a second
 * canvas). The device half of the browser window surface (HostSurface.h),
 * as VulkanPresenter is natively.
 *
 * The C++ side only packs the frame: ops into two flat arrays, uploads into
 * a third, textures named by small slot numbers the page can key a table
 * with (a RID is 64 bits, which JavaScript numbers do not hold exactly).
 * One proxied call per frame hands the page the addresses; the page reads
 * the shared heap and draws. Present runs on the frame thread and the call
 * blocks until the page has drawn, so the arrays are stable for it.
 */
class WebGpuPresenter : public DevicePresenter
{
public:
    WebGpuPresenter(WebGpuInstance* instance, const std::string& target)
        : m_instance(instance), m_target(target), m_id(next_id()) {}
    ~WebGpuPresenter() override
    {
#if defined(__EMSCRIPTEN__)
        MAIN_THREAD_EM_ASM({
            if (Module.etcsGpu) Module.etcsGpu.drop($0);
        }, m_id);
#endif
    }

    WebGpuPresenter(const WebGpuPresenter&) = delete;
    WebGpuPresenter& operator=(const WebGpuPresenter&) = delete;

    uint64_t DeviceKey() const override
    { return m_instance ? reinterpret_cast<uint64_t>(m_instance->GetDevice()) : 0; }

    bool Has(ETCS::RID source, uint32_t w, uint32_t h) const override
    {
        auto it = m_slots.find(source);
        return it != m_slots.end() && it->second.w == w && it->second.h == h;
    }

    // The element a surface presents to can change (SetTarget); the overlay
    // follows by being made again beside the new one.
    void SetTarget(const std::string& target)
    {
        if (target == m_target) return;
        m_target = target;
#if defined(__EMSCRIPTEN__)
        MAIN_THREAD_EM_ASM({ if (Module.etcsGpu) Module.etcsGpu.drop($0); }, m_id);
#endif
        m_slots.clear();
    }

    bool Present(DeviceFrame& frame) override
    {
#if defined(__EMSCRIPTEN__)
        if (!m_instance || !m_instance->GetDevice()) return false;
        ++m_frameNo;

        std::unordered_set<ETCS::RID> used;
        for (const DeviceOp& op : frame.ops)
            if (op.kind == DeviceOp::Kind::Blit) used.insert(op.source);

        m_ups.clear();
        for (const DeviceUpload& up : frame.uploads)
        {
            if (up.w == 0 || up.h == 0 || up.bytes.size() < static_cast<size_t>(up.w) * up.h * 4) continue;
            const int32_t slot = slotFor(up.source, used);
            if (slot < 0) continue;
            Slot& s = m_slots[up.source];
            s.w = up.w; s.h = up.h; s.lastUsed = m_frameNo;
            m_ups.push_back(slot);
            m_ups.push_back(static_cast<int32_t>(up.w));
            m_ups.push_back(static_cast<int32_t>(up.h));
            m_ups.push_back(static_cast<int32_t>(reinterpret_cast<uintptr_t>(up.bytes.data())));
        }

        m_opsF.clear();
        m_opsI.clear();
        for (const DeviceOp& op : frame.ops)
        {
            int32_t slot = -1;
            if (op.kind == DeviceOp::Kind::Blit)
            {
                auto it = m_slots.find(op.source);
                if (it == m_slots.end()) continue;          // never sent: nothing to draw
                it->second.lastUsed = m_frameNo;
                slot = it->second.slot;
            }
            m_opsF.insert(m_opsF.end(), { static_cast<float>(op.x), static_cast<float>(op.y),
                                          static_cast<float>(op.w), static_cast<float>(op.h),
                                          op.c[0], op.c[1], op.c[2], op.c[3] });
            m_opsI.push_back(op.kind == DeviceOp::Kind::Blit ? 1 : 0);
            m_opsI.push_back(slot);
        }

        const int32_t hdr[10] = {
            m_id,
            static_cast<int32_t>(frame.width), static_cast<int32_t>(frame.height),
            static_cast<int32_t>(m_opsI.size() / 2),
            static_cast<int32_t>(reinterpret_cast<uintptr_t>(m_opsF.data())),
            static_cast<int32_t>(reinterpret_cast<uintptr_t>(m_opsI.data())),
            static_cast<int32_t>(m_ups.size() / 4),
            static_cast<int32_t>(reinterpret_cast<uintptr_t>(m_ups.data())),
            static_cast<int32_t>(reinterpret_cast<uintptr_t>(frame.clear.data())),
            static_cast<int32_t>(reinterpret_cast<uintptr_t>(m_target.c_str()))
        };
        const int ok = MAIN_THREAD_EM_ASM_INT({
            return Module.etcsGpu ? Module.etcsGpu.present($0) : 0;
        }, hdr);
        if (!ok)
        {
            ETCS_LOG("WebGpuPresenter", "the page could not draw with WebGPU (device lost, or "
                     "it threw -- see the browser console); this surface goes back to the host.");
            m_slots.clear();
            return false;
        }
        return true;
#else
        (void)frame;
        return false;
#endif
    }

private:
    struct Slot { int32_t slot = -1; uint32_t w = 0, h = 0; uint64_t lastUsed = 0; };

    static int32_t next_id()
    {
        static std::atomic<int32_t> n{ 0 };
        return ++n;
    }

    // The slot a source's texture lives in, made if new; full, the least
    // recently drawn source not in this frame gives its slot up.
    int32_t slotFor(ETCS::RID rid, const std::unordered_set<ETCS::RID>& used)
    {
        auto it = m_slots.find(rid);
        if (it != m_slots.end()) return it->second.slot;
        int32_t slot = -1;
        if (m_slots.size() >= WEBGPU_PRESENTER_MAX_SOURCES)
        {
            auto victim = m_slots.end();
            for (auto v = m_slots.begin(); v != m_slots.end(); ++v)
                if (!used.count(v->first) && (victim == m_slots.end() || v->second.lastUsed < victim->second.lastUsed))
                    victim = v;
            if (victim == m_slots.end())
            {
                ETCS_LOG("WebGpuPresenter", "more than " << WEBGPU_PRESENTER_MAX_SOURCES
                         << " sources in one frame -- dropping RID:" << rid);
                return -1;
            }
            slot = victim->second.slot;
#if defined(__EMSCRIPTEN__)
            MAIN_THREAD_EM_ASM({ if (Module.etcsGpu) Module.etcsGpu.evict($0, $1); }, m_id, slot);
#endif
            m_slots.erase(victim);
        }
        else slot = m_nextSlot++;
        m_slots[rid].slot = slot;
        return slot;
    }

    WebGpuInstance* m_instance = nullptr;
    std::string     m_target;
    int32_t         m_id       = 0;
    int32_t         m_nextSlot = 0;
    uint64_t        m_frameNo  = 0;
    std::unordered_map<ETCS::RID, Slot> m_slots;
    // Kept between frames so a steady frame allocates nothing.
    std::vector<float>   m_opsF;
    std::vector<int32_t> m_opsI;
    std::vector<int32_t> m_ups;
};

#endif // RENDERPROVIDER_WEBGPUPRESENTER_H__
