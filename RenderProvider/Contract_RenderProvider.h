#ifndef RENDERPROVIDER_CONTRACT__

#if defined(__EMSCRIPTEN__)
    // No WASM backend yet -- WebGPU would be the natural target, and it is
    // a different enough surface that it wants its own concrete types
    // rather than a #define around these ones.

#elif defined(_WIN32) || defined(__linux__)
    #define RENDERPROVIDER_CONTRACT__
    #include "OS/VulkanInstance.h"
    #include "OS/VulkanTarget.h"
    typedef VulkanInstance Instance;
    typedef VulkanTarget   Target;

#else
    #warning "RenderProvider_Contract: Platform not detected (expects win/linux). Check preprocessor definitions."
    #error "Unsupported platform"
#endif

// auto generated hashes of headers:
#include "../../ETCS.h"
#include "module_hashes.h"

#endif // RENDERPROVIDER_CONTRACT__
