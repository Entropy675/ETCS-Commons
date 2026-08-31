#ifndef VULKAN_INSTANCE_H__
#define VULKAN_INSTANCE_H__

#include "../../../ontology.h"
#include "VulkanPlatform.h"

#include <vector>
#include <cstring>
#include <iostream>

// VulkanInstance -- flat tag, no ontology supertype (DeletableBase only):
// there is exactly one Vulkan backend on the roadmap right now (see the
// readme TODO), so unlike Window/Target this never needs to be addressed
// generically by foreign code the way HtmlPage_/Window_/Target_ do. Same
// flatness as DatabaseProvider's LocalDatabase.
//
// Owns the VkInstance, a single selected physical+logical device pair,
// one graphics+present-capable queue, and one command pool -- everything
// a Target needs handed to it (GetDevice/GetQueue/GetCommandPool) to
// build its own swapchain/pipeline against.
class VulkanInstance : public DeletableBase<VulkanInstance>
{
public:
    WIRE_TYPE_IDENTITY(VulkanInstance);

    VulkanInstance()  = default;
    ~VulkanInstance() { teardown(); }

    // Idempotent, like GLFWWindow::CreateWindowConcrete -- a second call
    // is a silent no-op rather than a leak-and-replace.
    bool Create()
    {
        if (m_instance) return true;

        VkApplicationInfo appInfo{};
        appInfo.sType            = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "ETCS RenderProvider";
        appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.pEngineName      = "ETCS";
        appInfo.engineVersion    = VK_MAKE_VERSION(1, 0, 0);
        appInfo.apiVersion       = VK_API_VERSION_1_0;

        // Surface support only -- one generic extension plus exactly one
        // platform extension, named directly off the Vulkan spec. No GLFW
        // dependency anywhere in this module: see ontology/Window.h's
        // NativeSurfaceHandle comment for why that boundary is drawn here.
        std::vector<const char*> extensions = {
            VK_KHR_SURFACE_EXTENSION_NAME,
#if defined(_WIN32)
            VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
#else
            VK_KHR_XLIB_SURFACE_EXTENSION_NAME,
#endif
        };

        VkInstanceCreateInfo ci{};
        ci.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ci.pApplicationInfo        = &appInfo;
        ci.enabledExtensionCount   = static_cast<uint32_t>(extensions.size());
        ci.ppEnabledExtensionNames = extensions.data();
        // No validation layers in V1 -- deliberately deferred, not an oversight.

        if (vkCreateInstance(&ci, nullptr, &m_instance) != VK_SUCCESS)
        {
            ETCS_LOG("VulkanInstance", "vkCreateInstance failed.");
            return false;
        }

        if (!pickPhysicalDevice()) { teardown(); return false; }
        if (!createLogicalDevice()) { teardown(); return false; }

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = m_queueFamily;
        if (vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPool) != VK_SUCCESS)
        {
            ETCS_LOG("VulkanInstance", "vkCreateCommandPool failed.");
            teardown();
            return false;
        }

        ETCS_LOG("VulkanInstance", "Ready -- queue family " << m_queueFamily);
        this->addTag("active");
        return true;
    }

    bool DeleteConcrete() override
    {
        std::string conjugate_key = getSourceModule().toString() + ":" + getSourceTag().toString();
        ETCS_LOG("VulkanInstance", "firing self-DestroyEvent for RID:" << getRID());
        return ETCS::DestroyEvent{conjugate_key.c_str(), this}();
    }

    bool IsActive() const { return this->hasTag("active"); }

    VkInstance       GetInstance()       const { return m_instance; }
    VkPhysicalDevice GetPhysicalDevice() const { return m_physicalDevice; }
    VkDevice         GetDevice()         const { return m_device; }
    VkQueue          GetQueue()          const { return m_queue; }
    uint32_t         GetQueueFamily()    const { return m_queueFamily; }
    VkCommandPool    GetCommandPool()    const { return m_commandPool; }

private:
    VkInstance       m_instance       = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice         m_device         = VK_NULL_HANDLE;
    VkQueue          m_queue          = VK_NULL_HANDLE;
    uint32_t         m_queueFamily    = 0;
    VkCommandPool    m_commandPool    = VK_NULL_HANDLE;

    // First physical device exposing a queue family with both GRAPHICS and
    // (platform-conditionally checked) presentation support -- V1 doesn't
    // score/prefer discrete over integrated, it just needs one that works.
    bool pickPhysicalDevice()
    {
        uint32_t count = 0;
        vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
        if (count == 0)
        {
            ETCS_LOG("VulkanInstance", "No Vulkan-capable physical devices found.");
            return false;
        }
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(m_instance, &count, devices.data());

        for (VkPhysicalDevice dev : devices)
        {
            uint32_t qcount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(dev, &qcount, nullptr);
            std::vector<VkQueueFamilyProperties> props(qcount);
            vkGetPhysicalDeviceQueueFamilyProperties(dev, &qcount, props.data());

            for (uint32_t i = 0; i < qcount; ++i)
            {
                if (props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
                {
                    m_physicalDevice = dev;
                    m_queueFamily    = i;
                    return true;
                }
            }
        }

        ETCS_LOG("VulkanInstance", "No physical device with a graphics queue family found.");
        return false;
    }

    bool createLogicalDevice()
    {
        float priority = 1.0f;
        VkDeviceQueueCreateInfo qci{};
        qci.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = m_queueFamily;
        qci.queueCount       = 1;
        qci.pQueuePriorities = &priority;

        const std::vector<const char*> deviceExtensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

        VkDeviceCreateInfo ci{};
        ci.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        ci.pQueueCreateInfos       = &qci;
        ci.queueCreateInfoCount    = 1;
        ci.enabledExtensionCount   = static_cast<uint32_t>(deviceExtensions.size());
        ci.ppEnabledExtensionNames = deviceExtensions.data();

        if (vkCreateDevice(m_physicalDevice, &ci, nullptr, &m_device) != VK_SUCCESS)
        {
            ETCS_LOG("VulkanInstance", "vkCreateDevice failed.");
            return false;
        }
        vkGetDeviceQueue(m_device, m_queueFamily, 0, &m_queue);
        return true;
    }

    void teardown()
    {
        if (m_commandPool) { vkDestroyCommandPool(m_device, m_commandPool, nullptr); m_commandPool = VK_NULL_HANDLE; }
        if (m_device)      { vkDestroyDevice(m_device, nullptr);                     m_device      = VK_NULL_HANDLE; }
        if (m_instance)    { vkDestroyInstance(m_instance, nullptr);                 m_instance    = VK_NULL_HANDLE; }
        m_physicalDevice = VK_NULL_HANDLE;
        m_queue          = VK_NULL_HANDLE;
        this->removeTag("active");
    }
};

#endif
