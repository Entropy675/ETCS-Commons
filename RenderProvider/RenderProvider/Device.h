#ifndef RENDERPROVIDER_DEVICE_H__
#define RENDERPROVIDER_DEVICE_H__

#include "../../../core_defs.h"
#include "../../../ontology.h"
#include "../OS/VulkanInstance.h"

// ---------------------------------------------------------------------------
// Device -- "this entity can reach that GPU".
//
// [Device + Deletable]. A capability attachment, and deliberately not the GPU
// itself: RenderProvider::Instance is the Vulkan device, one per process, and
// making a camera's capability a SECOND Instance would build a second device
// to express a relationship. So this names an existing one by RID and answers
// for it.
//
// WHY IT IS A SEPARATE ENTITY AT ALL, rather than a field on the camera.
// Because the capability is meant to be structural (ontology/Device.h): a
// camera that can reach a device is a camera with one of these under it, which
// puts the fact on the state surface, makes it enumerable by the ordinary
// typed-child walk, and marks the camera's observers when it appears or goes.
// A field would have been a second copy of that, settable to a value the graph
// disagrees with.
//
// It is also why `attach` is not the verb that would have worked here. attach
// is a pure lookup (doc/etcs_specification.md) -- it binds a name, it does not
// re-parent -- so an Instance spawned at the root cannot become a camera's
// child after the fact. Spawning one of these under the camera and pointing it
// at the Instance is how the relation gets INTO the tree, which is where every
// other relation in this system lives.
//
//     eye.spawn(RenderProvider::Device dev)
//     dev.Create(@gpu)
//     eye.SetDeviceProjection(1)
//
// BY RID, RESOLVED AT THE POINT OF USE, like every other cross-entity
// reference here: an Instance deleted out from under this one makes
// DeviceReady() false and the camera falls back to the host, which is exactly
// what "there is always a CPU" buys.
// ---------------------------------------------------------------------------
class Device : public DeviceBase<Device>,
               public DeletableBase<Device>
{
public:
    WIRE_TYPE_IDENTITY(Device);

    Device()  = default;
    ~Device() = default;

    bool Create(ETCS::RID instance)
    {
        if (instance == 0)
        {
            ETCS_LOG("Device", "Create needs the RID of a RenderProvider::Instance.");
            return false;
        }
        m_instance = instance;
        this->addTag("active");
        return DeviceReadyConcrete();
    }

    // ── Device_ dispatch ──────────────────────────────────────────────────

    // The VkDevice handle, which is the same value a Renderable on this device
    // answers (VulkanSurface::DeviceKeyConcrete) -- equality across the two is
    // the whole of what a key is for (ontology/Renderable.h).
    uint64_t DeviceKeyConcrete() const
    {
        VulkanInstance* vi = resolve();
        return vi ? reinterpret_cast<uint64_t>(vi->GetDevice()) : 0;
    }

    // Declared and usable are different answers: an Instance that exists but
    // has not been Created yet has no VkDevice, and a camera must not switch
    // to a device that cannot draw.
    bool DeviceReadyConcrete() const { return DeviceKeyConcrete() != 0; }

    // ── Deletable_ ────────────────────────────────────────────────────────
    bool DeleteConcrete() { return true; }

private:
    /*
     * Resolved per call, never held. The capability is answered from what is
     * true NOW rather than from what was true when the camera was set up --
     * an Instance deleted out from under this one stops resolving, DeviceReady
     * goes false, and the camera is back on the host with nothing to reset.
     *
     * Through the module's own "Instance" tag row rather than a family
     * aggregate, because Instance is flat by design (no ontology supertype but
     * Deletable) and that row is the only place it appears -- the same lookup
     * and the same reason as Surface::Create's, which is why rp_resolve_tag
     * exists rather than being written twice.
     *
     * getTrueType(), not a static_cast off Entity*: Entity is a virtual base
     * here, so a direct downcast from it is ill-formed.
     */
    VulkanInstance* resolve() const
    {
        ETCS::Entity* raw = rp_resolve_tag("Instance", m_instance);
        return raw ? static_cast<VulkanInstance*>(raw->getTrueType()) : nullptr;
    }

    ETCS::RID m_instance = 0;
};

#endif
