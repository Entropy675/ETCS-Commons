#ifndef VULKAN_PLATFORM_H__
#define VULKAN_PLATFORM_H__

// The single place RenderProvider's window-system (WSI) selection is made,
// and -- since it is already the one platform-conditional file -- the
// place the VkSurfaceKHR factory lives too.
//
// Defining VK_USE_PLATFORM_* before <vulkan/vulkan.h> is what makes that
// header pull in both the platform's own types (Display*/Window/VisualID,
// HWND/HINSTANCE) and the matching surface extension -- which is where
// VK_KHR_XLIB_SURFACE_EXTENSION_NAME / vkCreateXlibSurfaceKHR and their
// Win32 counterparts actually come from. Including <vulkan/vulkan.h> bare
// gets neither, so every file in this module goes through this header
// rather than including Vulkan directly.
//
// Note this lands X11's `typedef XID Window` at file scope in every TU
// that reaches it. Safe HERE (RenderProvider names windows only as the
// ontology's Window_, never as its own bare `Window` typedef) but NOT in
// WindowProvider, whose Contract typedefs `Window` itself -- see the
// namespace wrapper in WindowProvider/OS/GLFWWindow.h for that side.

#if defined(_WIN32)
    #define VK_USE_PLATFORM_WIN32_KHR
#else
    #define VK_USE_PLATFORM_XLIB_KHR
#endif

#include <vulkan/vulkan.h>

#include "../../../ontology.h"
#include <iostream>

// Builds a VkSurfaceKHR straight from a Window_'s NativeSurfaceHandle
// (ontology/Window.h) using Vulkan's own platform-surface extensions --
// never GLFW. See ontology/Window.h's NativeSurfaceHandle comment and
// WindowProvider/OS/GLFWWindow.h's populateNativeSurfaceHandle() for why:
// GLFW is vendored static into WindowProvider.so alone, and a second,
// independently-linked copy in this module would carry its own
// never-initialized platform state.
//
// Returns VK_NULL_HANDLE on failure (bad platform tag, or the Vulkan call
// itself failing) -- caller logs/handles, this stays a plain function.
inline VkSurfaceKHR CreateSurfaceFromNativeHandle(VkInstance instance, const NativeSurfaceHandle& native)
{
#if defined(_WIN32)
    if (native.platform != NativeSurfacePlatform::Win32)
    {
        ETCS_LOG("VulkanPlatform", "NativeSurfaceHandle is not Win32 on a Win32 build.");
        return VK_NULL_HANDLE;
    }
    VkWin32SurfaceCreateInfoKHR ci{};
    ci.sType     = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    ci.hinstance = static_cast<HINSTANCE>(native.win32.hinstance);
    ci.hwnd      = static_cast<HWND>(native.win32.hwnd);

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (vkCreateWin32SurfaceKHR(instance, &ci, nullptr, &surface) != VK_SUCCESS)
    {
        ETCS_LOG("VulkanPlatform", "vkCreateWin32SurfaceKHR failed.");
        return VK_NULL_HANDLE;
    }
    return surface;
#else
    if (native.platform != NativeSurfacePlatform::X11)
    {
        ETCS_LOG("VulkanPlatform", "NativeSurfaceHandle is not X11 on a Linux build.");
        return VK_NULL_HANDLE;
    }
    VkXlibSurfaceCreateInfoKHR ci{};
    ci.sType  = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
    ci.dpy    = static_cast<Display*>(native.x11.display);
    ci.window = static_cast<Window>(native.x11.window);

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (vkCreateXlibSurfaceKHR(instance, &ci, nullptr, &surface) != VK_SUCCESS)
    {
        ETCS_LOG("VulkanPlatform", "vkCreateXlibSurfaceKHR failed.");
        return VK_NULL_HANDLE;
    }
    return surface;
#endif
}

#endif
