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
    data.writeString(self.ApplyMove(data.restAsString()).c_str());
}

// Request -- THE http entry point, and the only route this board registers.
// Receives the full request path and decides the verb itself, because it is
// also the thing that decided the path was its own (Filter). One owner for
// "is this mine" and "what did they ask for" means the two can never disagree.
DEFINE_WORK_FUNC(ChessBoard, Request)
{
    data.writeString(self.Request(data.restAsString()).c_str());
}

// Fen -- serialize the position. Both the render source (the UI draws from
// this) and the sync source (a joining or re-simulating peer loads it).
DEFINE_WORK_FUNC(ChessBoard, Fen)
{
    data.writeString(self.game().toFENString().c_str());
}

// LoadFen -- restore a position: resume, replay, or sync to a peer's state.
// Board::loadFEN is the inverse of toFENString (it parses CChess's own ep
// convention -- the pushed pawn's square, not the square behind it).
DEFINE_WORK_FUNC(ChessBoard, LoadFen)
{
    std::string fen = data.restAsString();
    bool ok = self.game().loadFEN(fen);
    data.writeString(ok ? self.game().toFENString().c_str() : "INVALID");
}

// Status -- what the View's status bar showed: side to move, check, terminal.
DEFINE_WORK_FUNC(ChessBoard, Status)
{
    data.writeString(self.StatusLine().c_str());
}

// ── Trait-provided verbs, exported ────────────────────────────────────────────
// EphemeralBase gives ChessBoard Reset()/IsActive() and DeletableBase gives
// Delete(), but inheriting a trait only supplies the C++ method -- the dispatch
// surface is built from work functions, so each still needs a thin forwarder
// here to be reachable from .etcs or the REPL. These call the base's PROVIDED
// method (Reset/Delete), never the *Concrete implementation directly, so
// whatever ceremony the base wraps around it still runs.
DEFINE_WORK_FUNC(ChessBoard, Reset)
{
    bool ok = self.Reset();
    data.writeString(ok ? self.game().toFENString().c_str() : "FAILED");
}

DEFINE_WORK_FUNC(ChessBoard, IsActive)
{
    data.writeString(self.IsActive() ? "active" : "finished");
}

DEFINE_WORK_FUNC(ChessBoard, Delete)
{
    data.writeString(self.Delete() ? "deleted" : "FAILED");
}

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
    self.TakeSeat();   // the seat is claimed HERE, not in the filter -- this is
                       // the crossing actually happening, not a speculative ask
    ETCS_LOG("ChessBoard", "Accept: connection joined (" << who << ") -- seats now "
             << self.Seats() << "/2, key '" << self.MatchKey() << "'");
    data.writeString(self.game().toFENString().c_str()); // hand back the position
}

// Filter -- the Filter_ predicate, exported so a router can consult it. Same
// forwarding shape as Reset/Delete: the trait supplies Accepts(), the dispatch
// surface needs a work function to reach it.
DEFINE_WORK_FUNC(ChessBoard, Filter)
{
    self.Accepts(data);   // empty result == declined, by Filter_'s own contract
}

// Key -- read or set this board's equivalence class. A board with no key
// accepts nothing, so this is what a matchmaker calls to open a game for
// whoever holds the invite.
DEFINE_WORK_FUNC(ChessBoard, Key)
{
    std::string k = data.restAsString();
    if (!k.empty()) self.SetMatchKey(k);
    data.writeString(self.MatchKey().c_str());
}

#endif // CHESSPROVIDER_H__
