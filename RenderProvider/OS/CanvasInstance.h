#ifndef RENDERPROVIDER_CANVASINSTANCE_H__
#define RENDERPROVIDER_CANVASINSTANCE_H__

#include "../../../ontology.h"

// CanvasInstance -- the browser's answer to "which GPU are we on".
//
// [Deletable], flat, exactly like VulkanInstance: Instance is never addressed
// generically, so it claims no ontology family beyond Deletable and is resolved
// through this module's own "Instance" tag row (rp_resolve_tag, Device.h).
//
// There is nothing to create. A page has one canvas and the runtime already has
// it -- the window owns it, and a CanvasSurface presents to it by name. So this
// type exists to keep the SHAPE the rest of the module expects: Surface::Create
// takes an Instance*, Device resolves one to answer DeviceKey, and a script
// spawns one before a surface. Deleting the concrete backend would otherwise
// mean editing both of those for a platform that has no device object to name.
//
// GetDevice() returns `this` once Created: Device_ only ever compares device
// keys for equality, so any stable non-null identity is a correct answer, and
// "the one canvas" is what that identity means here.
class CanvasInstance : public DeletableBase<CanvasInstance>
{
public:
    WIRE_TYPE_IDENTITY(CanvasInstance);

    CanvasInstance()  = default;
    ~CanvasInstance() = default;

    bool Create()
    {
        if (m_active) return true;
        m_active = true;
        this->addTag("active");
        ETCS_LOG("CanvasInstance", "ready (RID:" << getRID() << ") -- the page's canvas.");
        return true;
    }

    bool IsActive() const { return m_active; }

    // Named GetDevice for parity with VulkanInstance, which is what Device.h
    // calls. See the class comment on why identity is a sufficient answer.
    const void* GetDevice() const { return m_active ? static_cast<const void*>(this) : nullptr; }

    bool DeleteConcrete() override
    {
        m_active = false;
        std::string conjugate_key = getSourceModule().toString() + ":" + getSourceTag().toString();
        ETCS_LOG("CanvasInstance", "firing self-DestroyEvent for RID:" << getRID());
        return ETCS::DestroyEvent{conjugate_key.c_str(), this}();
    }

private:
    bool m_active = false;
};

#endif // RENDERPROVIDER_CANVASINSTANCE_H__
