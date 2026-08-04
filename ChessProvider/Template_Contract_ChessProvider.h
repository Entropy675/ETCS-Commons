#ifndef CHESSPROVIDER_CONTRACT__

#if defined(__EMSCRIPTEN__)
    //#define CHESSPROVIDER_CONTRACT__
    //#include "Web/WASMChessProviderType.h"
    //typedef WASMChessProviderType ChessProviderType;
    // We don't support WASM globally yet... 

#elif defined(_WIN32)
    #define CHESSPROVIDER_CONTRACT__
    #include "Win/WinChessProviderType.h"
    typedef WinChessProviderType ChessProviderType;

#elif defined(__linux__)
    #define CHESSPROVIDER_CONTRACT__
    #include "Linux/LinChessProviderType.h"
    typedef LinChessProviderType ChessProviderType;

#else
    #warning "ChessProvider_Contract: Platform not detected. Check preprocessor definitions."
    #error "Unsupported platform"
#endif


// Please typedef your modules types here! Define them for each platform in the blocks above!
// Once you define your types, make sure to export them in ChessProvider.cc by adding to both the:
//   - ETCS_MODULE_EXPORT_MAIN(ChessProvider, "") <==== this string list, space separated declaring all tags
//   - And you must add either a ETCS_TAG_BLOCK_HYBRID or ETCS_TAG_BLOCK_BASIC block mapping the tag to your functions.

// Beware! You must pass causal exhaustion for every OS path to be verified and sellable on the marketplace.

// auto generated hashes of headers:
#include "../../ETCS.h"
#include "module_hashes.h"

#endif // CHESSPROVIDER_CONTRACT__
