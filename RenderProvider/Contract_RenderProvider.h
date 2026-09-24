#ifndef RENDERPROVIDER_CONTRACT__

/*
 * TWO THINGS PER PLATFORM, and only the first is platform code:
 *
 *   Instance -- the GPU, when there is one: Vulkan natively, WebGPU in a
 *               browser. A Device child names it (RenderProvider/Device.h).
 *   Surface  -- the window surface: a host raster on every platform
 *               (OS/HostSurface.h), presented by the window system's own
 *               call, that draws and presents through the Instance's device
 *               while a ready Device is under it.
 *
 * So the device is never what a window NEEDS, only what it uses when it can:
 * an Instance that fails to come up leaves every surface on the host, and a
 * script says nothing different either way (no line is conditional).
 */
#if defined(__EMSCRIPTEN__)
    #define RENDERPROVIDER_CONTRACT__
    #include "OS/WebGpuInstance.h"
    typedef WebGpuInstance Instance;

#elif defined(_WIN32) || defined(__linux__)
    #define RENDERPROVIDER_CONTRACT__
    #include "OS/VulkanInstance.h"
    typedef VulkanInstance Instance;

#else
    #warning "RenderProvider_Contract: Platform not detected (expects win/linux). Check preprocessor definitions."
    #error "Unsupported platform"
#endif

// The device capability attachment. Beside the Instance rather than under
// OS/: what varies per platform is the Instance it names, already selected
// above -- this type only says "reachable from here", the same statement
// everywhere. BEFORE the surface, which builds its backend from one.
#include "RenderProvider/Device.h"

#include "OS/HostSurface.h"
typedef HostSurface Surface;

#include "RenderProvider/ImageSurface.h"
#include "RenderProvider/PolygonDrawable2D.h"
#include "RenderProvider/CompositeDrawable2D.h"

// The 3D pair, and CPU-only for the same reason ImageSurface is: the
// projection is arithmetic and a depth test, with no device object anywhere in
// it. Scene3D fills a camera's Pixels_ and Camera3D owns those pixels, so the
// result reaches the window by the route every other CPU-side surface already
// takes -- one Blit. With a Device under the camera it instead records the
// spans it would have written and replays them onto the destination as rects,
// which a surface on a device draws there (Camera3D.h).
#include "RenderProvider/Scene3D.h"
#include "RenderProvider/Camera3D.h"

// The Glyphs leaf. AFTER the surfaces, because a label bound to a frame rate
// reads it off the Presentable family of whatever it names.
#include "RenderProvider/TextLabel.h"

// AFTER TextLabel, and that is a hard order rather than a tidy one: the
// throbber owns a TextLabel child and calls its Glyphs verbs directly, so the
// type has to be complete here. Not by family name, because the font belongs to
// that one type and reaching it generically would mean a caller could hand the
// throbber a Glyphs leaf that is not a drawable child it can own.
#include "RenderProvider/Throbber.h"

// auto generated hashes of headers:
#include "../../ETCS.h"
#include "module_hashes.h"

#endif // RENDERPROVIDER_CONTRACT__
