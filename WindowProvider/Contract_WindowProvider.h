#ifndef WINDOWPROVIDER_CONTRACT__

// GLFW is the windowing backend on desktop *and* under emscripten (GLFW's
// own emscripten platform). No separate WASMWindow typedef: the same
// GLFWWindow type is Window everywhere this module builds, with platform
// differences handled inside OS/GLFWWindow.h behind __EMSCRIPTEN__.
#if defined(_WIN32) || defined(__linux__) || defined(__EMSCRIPTEN__)
    #define WINDOWPROVIDER_CONTRACT__
    #include "OS/GLFWWindow.h"
    typedef GLFWWindow Window;

#else
    #warning "WindowProvider_Contract: Platform not detected (expects win/linux/emscripten). Check preprocessor definitions."
    #error "Unsupported platform"
#endif

// auto generated hashes of headers:
#include "../../ETCS.h"
#include "module_hashes.h"

#endif // WINDOWPROVIDER_CONTRACT__ definition
