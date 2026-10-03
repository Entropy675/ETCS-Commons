#ifndef RENDERPROVIDER_WEBGPUPRESENTER_H__
#define RENDERPROVIDER_WEBGPUPRESENTER_H__

#include "../../../ontology.h"
#include "../RenderProvider/DeviceFrame.h"
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
 *
 * SCENES AND MESHES TRAVEL THE SAME WAY. A scene's target is a texture slot
 * like any source's -- the page renders into it, colour and depth, and the
 * Blit that follows samples it -- and a mesh is a slot in a second table,
 * sent once with its geometry. Each node's matrix is folded with the
 * scene's lens and view here (device_mul4), so the page's vertex shader is
 * one multiply, as the Vulkan one is.
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
    bool HasMesh(ETCS::RID mesh) const override { return m_meshSlots.count(mesh) != 0; }

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
        m_meshSlots.clear();
    }

    bool Present(DeviceFrame& frame) override
    {
#if defined(__EMSCRIPTEN__)
        if (!m_instance || !m_instance->GetDevice()) return false;
        ++m_frameNo;

        std::unordered_set<ETCS::RID> used;
        for (const DeviceOp& op : frame.ops)
            if (op.kind == DeviceOp::Kind::Blit) used.insert(op.source);
        for (const DeviceScene& sc : frame.scenes) used.insert(sc.target);

        // Mesh 0, the unit box, the page never has to be sent: made here the
        // first time, like the Vulkan backend's.
        std::unordered_set<ETCS::RID> usedMeshes;
        for (const DeviceScene& sc : frame.scenes)
            for (const DeviceMeshOp& op : sc.meshes) usedMeshes.insert(op.mesh);
        m_meshUps.clear();
        if (!m_meshSlots.count(0))
        {
            m_box.vertices.clear(); m_box.indices.clear();
            device_unit_box(m_box.vertices, m_box.indices);
            packMesh(0, m_box, usedMeshes);
        }
        for (const DeviceMeshUpload& up : frame.meshes) packMesh(up.mesh, up, usedMeshes);

        // Scenes: a target slot each, the node ops in one flat list.
        m_scI.clear(); m_scF.clear(); m_mopF.clear(); m_mopI.clear();
        for (const DeviceScene& sc : frame.scenes)
        {
            if (sc.w == 0 || sc.h == 0) continue;
            const int32_t slot = slotFor(sc.target, used);
            if (slot < 0) continue;
            Slot& s = m_slots[sc.target];
            s.w = sc.w; s.h = sc.h; s.lastUsed = m_frameNo;
            const int32_t first = static_cast<int32_t>(m_mopI.size());
            float vp[16];
            device_mul4(sc.proj, sc.view, vp);
            for (const DeviceMeshOp& op : sc.meshes)
            {
                auto mit = m_meshSlots.find(op.mesh);
                if (mit == m_meshSlots.end()) continue;
                mit->second.lastUsed = m_frameNo;
                float mvp[16];
                device_mul4(vp, op.model, mvp);
                m_mopF.insert(m_mopF.end(), mvp, mvp + 16);
                // The rotation as three columns padded to vec4, the way WGSL lays a mat3x3 out.
                for (int c = 0; c < 3; ++c)
                    m_mopF.insert(m_mopF.end(), { op.model[c * 4], op.model[c * 4 + 1], op.model[c * 4 + 2], 0.0f });
                m_mopF.insert(m_mopF.end(), op.color, op.color + 4);
                m_mopI.push_back(mit->second.slot);
            }
            m_scI.insert(m_scI.end(), { slot, static_cast<int32_t>(sc.w), static_cast<int32_t>(sc.h),
                                        first, static_cast<int32_t>(m_mopI.size()) - first });
            m_scF.insert(m_scF.end(), sc.clear.begin(), sc.clear.end());
        }

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

        const int32_t hdr[17] = {
            m_id,
            static_cast<int32_t>(frame.width), static_cast<int32_t>(frame.height),
            static_cast<int32_t>(m_opsI.size() / 2),
            static_cast<int32_t>(reinterpret_cast<uintptr_t>(m_opsF.data())),
            static_cast<int32_t>(reinterpret_cast<uintptr_t>(m_opsI.data())),
            static_cast<int32_t>(m_ups.size() / 4),
            static_cast<int32_t>(reinterpret_cast<uintptr_t>(m_ups.data())),
            static_cast<int32_t>(reinterpret_cast<uintptr_t>(frame.clear.data())),
            static_cast<int32_t>(reinterpret_cast<uintptr_t>(m_target.c_str())),
            static_cast<int32_t>(m_scI.size() / 5),
            static_cast<int32_t>(reinterpret_cast<uintptr_t>(m_scI.data())),
            static_cast<int32_t>(reinterpret_cast<uintptr_t>(m_scF.data())),
            static_cast<int32_t>(reinterpret_cast<uintptr_t>(m_mopF.data())),
            static_cast<int32_t>(reinterpret_cast<uintptr_t>(m_mopI.data())),
            static_cast<int32_t>(m_meshUps.size() / 5),
            static_cast<int32_t>(reinterpret_cast<uintptr_t>(m_meshUps.data()))
        };
        const int ok = MAIN_THREAD_EM_ASM_INT({
            return Module.etcsGpu ? Module.etcsGpu.present($0) : 0;
        }, hdr);
        if (!ok)
        {
            ETCS_LOG("WebGpuPresenter", "the page could not draw with WebGPU (device lost, or "
                     "it threw -- see the browser console); this surface goes back to the host.");
            m_slots.clear();
            m_meshSlots.clear();
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

    // A mesh's slot in the page's geometry table, made if new; full, the
    // least recently drawn mesh not in this frame gives its slot up. The
    // geometry itself is queued for the page to copy off the heap.
    void packMesh(ETCS::RID rid, const DeviceMeshUpload& up, const std::unordered_set<ETCS::RID>& used)
    {
        if (up.vertices.empty() || up.indices.empty()) return;
        auto it = m_meshSlots.find(rid);
        int32_t slot = -1;
        if (it != m_meshSlots.end()) slot = it->second.slot;
        else if (m_meshSlots.size() >= WEBGPU_PRESENTER_MAX_SOURCES)
        {
            auto victim = m_meshSlots.end();
            for (auto v = m_meshSlots.begin(); v != m_meshSlots.end(); ++v)
                if (!used.count(v->first) && (victim == m_meshSlots.end() || v->second.lastUsed < victim->second.lastUsed))
                    victim = v;
            if (victim == m_meshSlots.end())
            {
                ETCS_LOG("WebGpuPresenter", "more than " << WEBGPU_PRESENTER_MAX_SOURCES
                         << " meshes in one frame -- dropping RID:" << rid);
                return;
            }
            slot = victim->second.slot;
            m_meshSlots.erase(victim);
        }
        else slot = m_nextMeshSlot++;
        MeshSlot& m = m_meshSlots[rid];
        m.slot = slot; m.lastUsed = m_frameNo;
        m_meshUps.push_back(slot);
        m_meshUps.push_back(static_cast<int32_t>(up.vertices.size()));
        m_meshUps.push_back(static_cast<int32_t>(reinterpret_cast<uintptr_t>(up.vertices.data())));
        m_meshUps.push_back(static_cast<int32_t>(up.indices.size()));
        m_meshUps.push_back(static_cast<int32_t>(reinterpret_cast<uintptr_t>(up.indices.data())));
    }

    struct MeshSlot { int32_t slot = -1; uint64_t lastUsed = 0; };

    WebGpuInstance* m_instance = nullptr;
    std::string     m_target;
    int32_t         m_id       = 0;
    int32_t         m_nextSlot = 0;
    int32_t         m_nextMeshSlot = 0;
    uint64_t        m_frameNo  = 0;
    std::unordered_map<ETCS::RID, Slot>     m_slots;
    std::unordered_map<ETCS::RID, MeshSlot> m_meshSlots;
    DeviceMeshUpload m_box;      // mesh 0's geometry, alive until the page copied it
    // Kept between frames so a steady frame allocates nothing.
    std::vector<float>   m_opsF;
    std::vector<int32_t> m_opsI;
    std::vector<int32_t> m_ups;
    std::vector<int32_t> m_scI;      // 5 per scene: target slot, w, h, first node op, node op count
    std::vector<float>   m_scF;      // 4 per scene: clear colour
    std::vector<float>   m_mopF;     // 32 per node op: mvp[16], rot as 3 x vec4, colour
    std::vector<int32_t> m_mopI;     // 1 per node op: mesh slot
    std::vector<int32_t> m_meshUps;  // 5 per mesh: slot, float count, address, index count, address
};

#endif // RENDERPROVIDER_WEBGPUPRESENTER_H__
