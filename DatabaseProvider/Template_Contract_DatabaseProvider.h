#ifndef DATABASEPROVIDER_CONTRACT__

/*
 * THE SAME ENGINE IN THE BROWSER, over the same interface: a path. sqlite is
 * portable C over a filesystem, and emscripten gives it one -- MEMFS by default,
 * IDBFS where the page mounts it (PaintProvider's page mounts /persist there, and
 * syncs it to IndexedDB), so a database at a path is durable across reloads with
 * no engine of its own for the web. That is the whole reason there is no
 * Web/ leaf: the Sqlite one IS the web one. THREADSAFE=1 and no load-extension
 * (manifests/DatabaseProvider.json) are what the side module needs of it.
 */
#if defined(__EMSCRIPTEN__) || defined(_WIN32) || defined(__linux__)
    #define DATABASEPROVIDER_CONTRACT__
    #include "OS/OSLocalDatabase.h"
    typedef OSLocalDatabase LocalDatabase;

#else
    #warning "DatabaseProvider_Contract: Platform not detected. Check preprocessor definitions."
    #error "Unsupported platform"
#endif


// Please typedef your modules types here! Define them for each platform in the blocks above!
// Once you define your types, make sure to export them in DatabaseProvider.cc by adding to both the:
//   - ETCS_MODULE_EXPORT_MAIN(DatabaseProvider, "") <==== this string list, space separated declaring all tags
//   - And you must add either a ETCS_TAG_BLOCK_HYBRID or ETCS_TAG_BLOCK_BASIC block mapping the tag to your functions.

// Beware! You must pass causal exhaustion for every OS path to be verified and sellable on the marketplace.

// auto generated hashes of headers:
#include "../../ETCS.h"
#include "module_hashes.h"

#endif // DATABASEPROVIDER_CONTRACT__
