#include "ChessProvider.h"

// Four types. CChess's own ChessGame (and ChessGameLinux/Windows) WAS the
// loader: it owned the Board, ran the input loop, and pushed state at a View. In
// ETCS that role is RegisterDynamicLoader plus the runtime's UI/Network
// providers, so what remains here is the board and the session beside it.
ETCS_MODULE_EXPORT_MAIN(ChessProvider, "ChessGame ChessShare ChessBoard ChessTable")

// A move exchange is a stream: a follower produces its proposals (Emit),
// the host's board consumes them (Judge) and the record, and every board
// consumes the record (Absorb).
ETCS_TAG_BLOCK_HYBRID(ChessGame,
    (Act, Move, Fen, LoadFen, Status, Chat, Tail, History, Seats, Hash, Reset, IsActive, Delete),
    (Emit, Absorb, Judge))

ETCS_TAG_BLOCK_HYBRID(ChessShare,
    (Attach, Host, Serve, Join, Tick, Leave, Who, Delete),
    (Roster, Judge))

// The picture and the seat. The board rides the frame edge (Animated) and
// needs no verbs to be drawn; the table takes the window's two streams.
ETCS_TAG_BLOCK_BASIC(ChessBoard,
    Create, Bind, SetPosition, SetOrder, SetHidden, SetFlip, Select, SquareAt, Delete)

ETCS_TAG_BLOCK_HYBRID(ChessTable,
    (Create, BindGame, BindShare, BindBoard, BindStatus, BindBars, BindLine, ChatRow,
     Click, Key, Type, Submit, Do, Toggle, Step, Report, Delete),
    (ConsumePointer, ConsumeKeys))
