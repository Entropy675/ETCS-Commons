#ifndef VULKAN_SURFACE_H__
#define VULKAN_SURFACE_H__

// The one platform-conditional seam in RenderProvider: builds a
// VkSurfaceKHR straight from a Window_'s NativeSurfaceHandle
// (ontology/Window.h) using Vulkan's own platform-surface extensions --
// never GLFW. See ontology/Window.h's NativeSurfaceHandle comment and
// WindowProvider/OS/GLFWWindow.h's populateNativeSurfaceHandle() for why:
// GLFW is vendored static into WindowProvider.so alone, and a second,
// independently-linked copy in this module would carry its own
// never-initialized platform state.

#include "../../../ontology.h"
#include "VulkanPlatform.h"

#include <iostream>

// Returns VK_NULL_HANDLE on failure (bad platform tag, or the Vulkan call
// itself failing) -- caller logs/handles, this stays a plain function.
inline VkSurfaceKHR CreateSurfaceFromNativeHandle(VkInstance instance, const NativeSurfaceHandle& native)
{
#if defined(_WIN32)
    if (native.platform != NativeSurfacePlatform::Win32)
    {
        ETCS_LOG("VulkanSurface", "NativeSurfaceHandle is not Win32 on a Win32 build.");
        return VK_NULL_HANDLE;
    }
    VkWin32SurfaceCreateInfoKHR ci{};
    ci.sType     = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    ci.hinstance = static_cast<HINSTANCE>(native.win32.hinstance);
    ci.hwnd      = static_cast<HWND>(native.win32.hwnd);

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (vkCreateWin32SurfaceKHR(instance, &ci, nullptr, &surface) != VK_SUCCESS)
    {
        ETCS_LOG("VulkanSurface", "vkCreateWin32SurfaceKHR failed.");
        return VK_NULL_HANDLE;
    }
    return surface;
#else
    if (native.platform != NativeSurfacePlatform::X11)
    {
        ETCS_LOG("VulkanSurface", "NativeSurfaceHandle is not X11 on a Linux build.");
        return VK_NULL_HANDLE;
    }
    VkXlibSurfaceCreateInfoKHR ci{};
    ci.sType  = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
    ci.dpy    = static_cast<Display*>(native.x11.display);
    ci.window = static_cast<Window>(native.x11.window);

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (vkCreateXlibSurfaceKHR(instance, &ci, nullptr, &surface) != VK_SUCCESS)
    {
        ETCS_LOG("VulkanSurface", "vkCreateXlibSurfaceKHR failed.");
        return VK_NULL_HANDLE;
    }
    return surface;
#endif
}

#endif
