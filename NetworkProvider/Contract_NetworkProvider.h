#ifndef NETWORKPROVIDER_CONTRACT__
#if defined(_WIN32)
    //#define NETWORKPROVIDER_CONTRACT__
    //#include "Win/WinNetworkProviderType.h"
    //typedef WinNetworkProviderType NetworkProviderType;
#elif defined(__EMSCRIPTEN__) || defined(__linux__)
    // One branch for both: this provider is formatting, and the transport under
    // it is core's (ThreadPool's IO). Its socket calls resolve to emscripten's
    // POSIX layer, so every header below compiles unmodified. A browser has no
    // listening socket, so HttpServer.Start is refused at listen() at runtime --
    // the browser's constraint, not a second contract.
    #define NETWORKPROVIDER_CONTRACT__
    #include "NetworkProvider/MbedTLSContext.h"
    #include "NetworkProvider/PicoHTTPParser.h"
    #include "NetworkProvider/StaticHtmlPage.h"
    #include "NetworkProvider/FileHtmlPage.h"
    #include "NetworkProvider/SocketConnectionState.h"
    #include "NetworkProvider/ConnectionManager.h"
    #include "NetworkProvider/HttpServer.h"
    #include "NetworkProvider/TarpitNode.h"
    // Links between runtimes: WebSocket.h is the wire, Edge.h the one
    // multiplexed MirrorBuffer on it, Room the host end, Peer the dialling
    // end, Remote the child that makes a local entity the surface of a far
    // one, LinkHub where they meet on a server. Seal is a Wrapper -- the
    // authority layer a published node can demand -- and Lobby and Ledger
    // are the Directory and Record the lobbies are made of.
    #include "NetworkProvider/Room.h"
    #include "NetworkProvider/Peer.h"
    #include "NetworkProvider/Remote.h"
    #include "NetworkProvider/Seal.h"
    #include "NetworkProvider/Lobby.h"
    #include "NetworkProvider/Ledger.h"

    typedef MbedTLSContext TLSContext;
    typedef PicoHTTPParser HTTPParser;
    // StaticHtmlPage, FileHtmlPage, SocketConnectionState, ConnectionManager,
    // HttpServer, TarpitNode and the link types are too simple to be typedef'd -- no
    // cross-platform alias indirection applies, so the concrete type name IS
    // the contract tag.
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
