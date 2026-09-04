#ifndef DATABASEPROVIDER_CONTRACT__

#if defined(__EMSCRIPTEN__)
    //#define DATABASEPROVIDER_CONTRACT__
    //#include "Web/WASMDatabaseProvider.h"
    //typedef WASMDatabaseProvider DatabaseProvider;
    // We don't support WASM globally yet... 

#elif defined(_WIN32) || defined(__linux__)
    #define DATABASEPROVIDER_CONTRACT__
    #include "OS/SqliteLocalDatabase.h"

    /*
     * TWO TAGS, ONE FAMILY -- and the family is `Database` (ontology/
     * DatabaseBase.h), which says nothing about where the bytes are.
     *
     * The tag name states the EXPECTED LOCALE, because that is the causal
     * fact a script actually knows at spawn time: is this data on my disk, or
     * on a server somebody else can also reach. Durability, sharing and
     * latency all follow from it, and none of them follow from "sqlite" --
     * which is why the tag is named for the locale and the class is named for
     * the engine.
     *
     * Not a platform fork, unlike every other contract typedef in this tree.
     * There is no machine on which you want only one of these: migrating,
     * mirroring and caching a remote table locally all want both open at once
     * -- mirror_users.etcs and archive_node.etcs are already shaped like the
     * first two. So this header lists tags side by side rather than selecting
     * between them, and that is the first time it has had to.
     */
    typedef SqliteLocalDatabase LocalDatabase;

    /*
     * The postgres leaf goes here, exported alongside rather than instead of:
     *
     *     #include "OS/PostgresRemoteDatabase.h"
     *     typedef PostgresRemoteDatabase RemoteDatabase;
     *
     * It claims the SAME family and adds no ontology of its own. Where it
     * differs is in what it composes: `ETCS::Remote` (core/MirrorBuffer.h) is
     * a marker base that flips its stream edges to a socket transport, which
     * is how a type declares its locale to the runtime today -- detected by
     * is_base_of, no macro changes, no family required.
     *
     * Deliberately a slot and not a stub. The thing being removed from the
     * ontology in this change was dead weight precisely because it made a
     * CONSTRAINT CLAIM nothing could satisfy; a commented block in a module's
     * own contract header claims nothing and is a to-do. Adding it for real
     * brings libpq as a system dependency and a network dependency into a
     * module that has neither, which is its own decision.
     */

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
