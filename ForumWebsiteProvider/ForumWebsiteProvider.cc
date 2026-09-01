#include "ForumWebsiteProvider.h"

// Three types, the same three ChessProvider has, because a forum and a chess
// server are the same arrangement with a different artifact in the middle: a
// host that routes, a self that holds edges and a record, and the thing the
// edges point at.
//
// What differs is which one outlives the other. In chess the artifact dies with
// the players, so the node reaps rooms nobody holds an edge to and keeps no
// database. In a forum the artifact outlives every self that touched it, which
// is the whole reason this module has a database edge and ChessProvider does
// not.
ETCS_MODULE_EXPORT_MAIN(ForumWebsiteProvider, "ForumThread ForumSelf ForumNode")

// BASIC: no stream functions on a thread yet. When post exchange becomes a
// Stream (produce-to-send, consume-to-verify -- the consume side being the same
// chain recomputation renderVerifyLocked already performs), this becomes
// HYBRID. That is also the point at which two peers stop sharing one thread and
// each replay into their own, checking heads against each other.
ETCS_TAG_BLOCK_BASIC(ForumThread,
    Read, Post, Title, Lock, Tomb, Status, Head, Key,
    Reset, IsActive, Delete, Filter)

// A self is reached through its node, so it carries only the two verbs every
// entity needs: a predicate for routing and a way to be destroyed.
ETCS_TAG_BLOCK_BASIC(ForumSelf,
    Filter, Delete)

// HYBRID, and the stream half is the load path: LoadRows is the consuming end
// of a pair with LocalDatabase's QueryProduce. There is deliberately no
// producing half here -- this module writes through ExecuteRaw by RID, one
// statement at a time, so a write is durable before it is acknowledged.
ETCS_TAG_BLOCK_HYBRID(ForumNode,
    (Request, Index, Selves, Verify, Mount, Db, InitDb, Accept, Filter, Delete),
    (LoadRows))
