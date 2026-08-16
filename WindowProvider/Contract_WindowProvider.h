#ifndef WINDOWPROVIDER_CONTRACT__

#if defined(__EMSCRIPTEN__)
    #define WINDOWPROVIDER_CONTRACT__
    //#include "Web/WASMWindow.h"
    //typedef WASMWindow Window

#elif defined(_WIN32) || defined(__linux__)
    #define WINDOWPROVIDER_CONTRACT__
    #include "OS/GLFWWindow.h"
    typedef GLFWWindow Window;
    
#else
    #warning "WindowProvider_Contract: Platform not detected (expects win/linux/wasm). Check preprocessor definitions."
    #error "Unsupported platform"
#endif

// auto generated hashes of headers:
#include "../../ETCS.h"
#include "module_hashes.h" 

#endif // WINDOWPROVIDER_CONTRACT__ definition

