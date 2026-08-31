#ifndef VULKAN_PLATFORM_H__
#define VULKAN_PLATFORM_H__

// The single place RenderProvider's window-system (WSI) selection is made.
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

#endif
