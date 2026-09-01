#ifndef RENDERPROVIDER_CONTRACT__

#if defined(__EMSCRIPTEN__)
    // No WASM backend yet -- WebGPU would be the natural target, and it is
    // a different enough surface that it wants its own concrete types
    // rather than a #define around these ones.

#elif defined(_WIN32) || defined(__linux__)
    #define RENDERPROVIDER_CONTRACT__
    #include "OS/VulkanInstance.h"
    #include "OS/VulkanSurface.h"
    typedef VulkanInstance Instance;
    typedef VulkanSurface  Surface;

#else
    #warning "RenderProvider_Contract: Platform not detected (expects win/linux). Check preprocessor definitions."
    #error "Unsupported platform"
#endif

// Not typedef'd and not under OS/: ImageSurface is CPU-only (its buffer and
// raster live in ontology/Pixels.h), so there is no per-platform concrete
// type to select between -- the class IS the tag type on every platform, the
// same way ChessProvider's OS-invariant types are.
#include "ImageSurface.h"

// auto generated hashes of headers:
#include "../../ETCS.h"
#include "module_hashes.h"

#endif // RENDERPROVIDER_CONTRACT__
