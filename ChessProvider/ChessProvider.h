#ifndef CHESSPROVIDER_H__
#define CHESSPROVIDER_H__


#define ETCS_DLL_EXPORTS
#include "../../core_defs.h"
#include "../../ontology.h"
#include "Contract_ChessProvider.h"

// Contract_ChessProvider.h pulls in ChessProvider/ChessBoard.h, the ontology
// leaf these dispatch on. Nothing here touches ::Board directly -- it all goes
// through ChessBoard::game().
//
// Buffer API note: ETCS::Buffer reads with restAsString() and writes with
// writeString(const char*), so every write below goes through .c_str().

// ── Local edges: the View's old calls into the engine ─────────────────────────
// ChessGame was the loader (own the Board, run the input loop, push at a View).
// The runtime is the loader now, so what survives is exactly the set of things
// NcView used to call on the engine, restated as dispatchable work.

// Move -- "e2e4", or "e2e4q" to promote. Returns the resulting FEN, or
// "ILLEGAL" if the engine rejected it.
//
// This rejection IS the anti-cheat, structurally: the same function runs on
// whoever is serving and (compiled to WASM) in the client, so neither side is a
// referee -- both re-simulate, and an illegal move simply has no
// representation. ChessBoard holds its Board privately, so this is the only way
// a move ever reaches the engine.
DEFINE_WORK_FUNC(ChessBoard, Move)
{
    std::string mv = data.restAsString();
    if (mv.size() < 4)
    {
        ETCS_LOG("ChessBoard", "Move: rejected (too short) \"" << mv << "\"");
        data.writeString("ILLEGAL");
        return;
    }

    Pos from(mv[0] - 'a', 8 - (mv[1] - '0'));
    Pos to  (mv[2] - 'a', 8 - (mv[3] - '0'));

    ChessStatus st = self.game().movePiece(from, to);
    self.setLastStatus(st);

    if (st == ChessStatus::PROMOTE && mv.size() >= 5)
    {
        std::string promo(1, mv[4]);
        self.game().registerPromotion(promo);
    }

    if (st == ChessStatus::FAIL)
    {
        ETCS_LOG("ChessBoard", "Move: illegal \"" << mv << "\"");
        data.writeString("ILLEGAL");
    }
    else
    {
        std::string fen = self.game().toFENString();
        ETCS_LOG("ChessBoard", "Move: \"" << mv << "\" -> " << fen);
        data.writeString(fen.c_str());
    }
}

// Fen -- serialize the position. Both the render source (the UI draws from
// this) and the sync source (a joining or re-simulating peer loads it).
DEFINE_WORK_FUNC(ChessBoard, Fen)
{
    std::string fen = self.game().toFENString();
    ETCS_LOG("ChessBoard", "Fen: " << fen);
    data.writeString(fen.c_str());
}

// LoadFen -- restore a position: resume, replay, or sync to a peer's state.
// Board::loadFEN is the inverse of toFENString (it parses CChess's own ep
// convention -- the pushed pawn's square, not the square behind it).
DEFINE_WORK_FUNC(ChessBoard, LoadFen)
{
    std::string fen = data.restAsString();
    bool ok = self.game().loadFEN(fen);
    if (ok)
    {
        std::string result = self.game().toFENString();
        ETCS_LOG("ChessBoard", "LoadFen: loaded \"" << fen << "\" -> " << result);
        data.writeString(result.c_str());
    }
    else
    {
        ETCS_LOG("ChessBoard", "LoadFen: invalid \"" << fen << "\"");
        data.writeString("INVALID");
    }
}

// Status -- what the View's status bar showed: side to move, check, terminal.
DEFINE_WORK_FUNC(ChessBoard, Status)
{
    std::string s = self.game().isWhiteTurn() ? "w" : "b";
    if      (self.game().isCheckmate())       s += " checkmate";
    else if (self.game().isStalemate())       s += " stalemate";
    else if (self.game().sideToMoveInCheck()) s += " check";
    else                                      s += " ok";
    ETCS_LOG("ChessBoard", "Status: " << s);
    data.writeString(s.c_str());
}

// NOTE: no Reset or Delete work function here -- EphemeralBase provides Reset
// (ChessBoard::ResetConcrete is "new game") and DeletableBase provides Delete.
// Re-declaring either would be a second, divergent door onto the same state.

// ── The network edge ──────────────────────────────────────────────────────────
// Accept -- the subscriber ConnectionManager calls per accepted connection,
// registered with RegisterConsumer(rid, "Accept"). Deliberately takes only
// Buffer data, never a NetworkProvider C++ type: this module must not link
// against another module's types, and doesn't -- composition of ChessBoard with
// HttpServer/ConnectionManager happens in .etcs (via .add(...)), which is what
// keeps ChessProvider loadable with no NetworkProvider present at all.
//
// This is also the seam where a seat, a colour, or later a signed ACE identity
// binds to a connection. Today every connection may view and move.
DEFINE_WORK_FUNC(ChessBoard, Accept)
{
    std::string who = data.restAsString();
    std::string fen = self.game().toFENString();
    ETCS_LOG("ChessBoard", "Accept: connection joined (" << who << ") -> " << fen);
    data.writeString(fen.c_str()); // hand back the position
}

#endif // CHESSPROVIDER_H__
