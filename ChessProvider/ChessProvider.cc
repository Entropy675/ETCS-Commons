#include "ChessProvider.h"

// One type: ChessGame. Renamed from ChessBoard once it grew seats, turn
// ownership, spectators and liveness -- a board is a position, and this manages
// a game that HAS one.
//
// ChessGame (the CChess class) WAS the loader: it owned the Board, ran the
// input loop, and pushed state at a View. In ETCS that role is
// RegisterDynamicLoader plus the runtime's UI/Network providers, so what
// remains here is the type and its edges.
ETCS_MODULE_EXPORT_MAIN(ChessProvider, "ChessGame")

// BASIC: no stream functions yet. When move exchange becomes a Stream
// (produce-to-send, consume-to-validate -- the consume side being the same
// re-simulation ApplyMove already performs), this becomes HYBRID.
ETCS_TAG_BLOCK_BASIC(ChessGame,
    Move, Fen, LoadFen, Status, Request, Key, Seats, Chat,
    Accept, Filter, Reset, IsActive, Delete)
