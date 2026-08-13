#ifndef NETWORKPROVIDER_CONTRACT__
#if defined(__EMSCRIPTEN__)
    //#define NETWORKPROVIDER_CONTRACT__
    //#include "Web/WASMNetworkProviderType.h"
    //typedef WASMNetworkProviderType NetworkProviderType;
    // We don't support WASM globally yet... 
#elif defined(_WIN32)
    //#define NETWORKPROVIDER_CONTRACT__
    //#include "Win/WinNetworkProviderType.h"
    //typedef WinNetworkProviderType NetworkProviderType;
#elif defined(__linux__)
    #define NETWORKPROVIDER_CONTRACT__
    #include "NetworkProvider/MbedTLSContext.h"
    #include "NetworkProvider/PicoHTTPParser.h"
    #include "NetworkProvider/StaticHtmlPage.h"
    #include "NetworkProvider/FileHtmlPage.h"
    #include "NetworkProvider/SocketConnectionState.h"
    #include "NetworkProvider/ConnectionManager.h"
    #include "NetworkProvider/HttpServer.h"
    
    typedef MbedTLSContext TLSContext;
    typedef PicoHTTPParser HTTPParser;
    // StaticHtmlPage, FileHtmlPage, SocketConnectionState, ConnectionManager
    // and HttpServer are too simple to be typedef'd -- no cross-platform alias
    // indirection applies, so the concrete type name IS the contract tag.
    //
    // NOTE on HTTPParser: it is now a Parser_ leaf and nothing else. The
    // accept/serve role it used to carry moved to ConnectionManager (Gate_)
    // and HttpServer (Switchable_) -- see ConnectionManager.h's own comment
    // for why fusing them into one type forced every consumer to
    // re-implement the connection lifecycle.
#else
    #warning "NetworkProvider_Contract: Platform not detected. Check preprocessor definitions."
    #error "Unsupported platform"
#endif
// Please typedef your modules types here! Define them for each platform in the blocks above!
// Once you define your types, make sure to export them in NetworkProvider.cc by adding to both the:
//   - ETCS_MODULE_EXPORT_MAIN(NetworkProvider, "") <==== this string list, space separated declaring all tags
//   - And you must add either a ETCS_TAG_BLOCK_HYBRID or ETCS_TAG_BLOCK_BASIC block mapping the tag to your functions.
// Beware! You must pass causal exhaustion for every OS path to be verified and sellable on the marketplace.
// auto generated hashes of headers:
#include "../../ETCS.h"
#include "module_hashes.h"
#endif // NETWORKPROVIDER_CONTRACT__
