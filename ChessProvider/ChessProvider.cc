#include "ChessProvider.h"

// One tag: ChessBoard. The old ChessGame (and ChessGameLinux/Windows) WAS the
// loader -- it created the Board, ran the input loop, and pushed state at a
// View. In ETCS that role is RegisterDynamicLoader plus the runtime's UI /
// Network providers; what remains in this module is purely the type and its
// edges. NcView is gone entirely: the runtime renders, we just serve state.
ETCS_MODULE_EXPORT_MAIN(ChessProvider, "ChessBoard")

// Hybrid, not Basic: the local edges are Work, and the network story pulls the
// board toward a Stream once moves flow peer-to-peer (a produced move to send,
// a consumed move to validate+apply -- the consume side being the same
// anti-cheat re-simulation as Move). Until that lands these are all Work; the
// block is written Hybrid so adding the move Stream later is a one-line change
// rather than a re-shape.
//
// If ETCS_TAG_BLOCK_HYBRID's stream tuple can't be empty, fall back to:
//   ETCS_TAG_BLOCK_BASIC(ChessBoard, Move, Fen, Status, Reset, LoadFen, Serve, Accept)
ETCS_TAG_BLOCK_BASIC(ChessBoard, Move, Fen, LoadFen, Status, Accept, Filter, Key, Request, Reset, IsActive, Delete)
