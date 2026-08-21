#include "ChessProvider.h"

// Three types. CChess's own ChessGame (and ChessGameLinux/Windows) WAS the
// loader: it owned the Board, ran the input loop, and pushed state at a View. In
// ETCS that role is RegisterDynamicLoader plus the runtime's UI/Network
// providers, so what remains here is the types and their edges.
ETCS_MODULE_EXPORT_MAIN(ChessProvider, "ChessGame ChessLobby ChessNode")

// BASIC everywhere: no stream functions yet. When move exchange becomes a
// Stream (produce-to-send, consume-to-validate -- the consume side being the
// same re-simulation applyMoveLocked already performs), those tags become
// HYBRID. That is also the point at which two peers stop sharing one board.
ETCS_TAG_BLOCK_BASIC(ChessGame,
    Move, Fen, LoadFen, Status, Chat, History, Leave, Request, Key, Seats,
    Accept, Reset, IsActive, Delete, Filter)

// A self is reached through its node, so it carries only the two verbs every
// entity needs: a predicate for routing and a way to be destroyed.
ETCS_TAG_BLOCK_BASIC(ChessLobby,
    Filter, Delete)

ETCS_TAG_BLOCK_BASIC(ChessNode,
    Request, Players, Rooms, Mount, Filter, Delete)