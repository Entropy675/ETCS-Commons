#include "NetworkProvider.h"
ETCS_MODULE_EXPORT_MAIN(NetworkProvider,
    "HttpServer ConnectionManager HTTPParser TLSContext SocketConnectionState StaticHtmlPage FileHtmlPage")

// HttpServer — root-level. A bag of config with Start/Stop; owns its gate and
// its pages as typed children.
ETCS_TAG_BLOCK_BASIC(HttpServer,
    SetPort, AddHandler, ClearHandlers, AddRoute, ClearRoutes,
    EnableTLS,
    Start, Stop, IsStarted,
    Serve, ListPaths, Delete
)

// ConnectionManager — the Gate_. The accept chain is internal; these are the
// controls around it.
ETCS_TAG_BLOCK_BASIC(ConnectionManager,
    Open, Close, IsOpen,
    RegisterConsumer, UnregisterConsumer, Delete,
    EnableTLS
)

// HTTPParser — a Parser_ leaf. No server actions remain.
ETCS_TAG_BLOCK_HYBRID(HTTPParser,
    (Delete, ParseRequest, ParseResponse),
    (ConsumeRequest, ProduceResponse)
)

ETCS_TAG_BLOCK_HYBRID(TLSContext,
    (Handshake, Close, SetupCerts, TestConnection),
    (SendData, ReceiveData)
)

// Reset, not Delete: a connection is Ephemeral_ only now -- the pool owns its
// lifetime, and destroying one out from under the pool would leave a dangling
// entry.
ETCS_TAG_BLOCK_BASIC(SocketConnectionState,
    Reset, Close, IsOpen, SetPage, GetPage
)

ETCS_TAG_BLOCK_BASIC(StaticHtmlPage,
    SetHtmlFromFile, SetHtmlRaw,
    SetCssFromFile, SetCssRaw,
    SetJsFromFile, SetJsRaw,
    LogContent, Delete
)

ETCS_TAG_BLOCK_BASIC(FileHtmlPage,
    LoadFromDisk, MountExternal, EnsureFallback, Resolve, ListPaths, Delete
)
