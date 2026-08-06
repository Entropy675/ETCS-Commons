#ifndef CHESSPROVIDER_H__
#define CHESSPROVIDER_H__


#define ETCS_DLL_EXPORTS
#include "../../core_defs.h"
#include "../../ontology.h"
#include "Contract_ChessProvider.h"

// Contract_ChessProvider.h pulls in ChessProvider/ChessGame.h, the ontology
// leaf these dispatch on. Nothing here touches ::Board directly -- it all goes
// through ChessGame's own verbs, which are shared with Request's path dispatch
// so the scripted and HTTP paths can never diverge.
//
// Buffer API note: ETCS::Buffer reads with restAsString() and writes with
// writeString(const char*), so every write below goes through .c_str().

// ── Local edges ───────────────────────────────────────────────────────────────

// Move -- "e2e4", or "e2e4q" to promote. No token, so this is the TRUSTED
// caller (script/REPL) and bypasses seat ownership by design: an operator
// unable to move a piece on their own server would be a strange security
// property. Seat enforcement lives on the HTTP path, where the caller is a
// stranger.
DEFINE_WORK_FUNC(ChessGame, Move)
{
    data.writeString(self.ApplyMove(data.restAsString()).c_str());
}

DEFINE_WORK_FUNC(ChessGame, Fen)
{
    data.writeString(self.game().toFENString().c_str());
}

DEFINE_WORK_FUNC(ChessGame, LoadFen)
{
    std::string fen = data.restAsString();
    bool ok = self.game().loadFEN(fen);
    data.writeString(ok ? self.game().toFENString().c_str() : "INVALID");
}

DEFINE_WORK_FUNC(ChessGame, Status)
{
    data.writeString(self.StatusLine().c_str());
}

// Request -- THE http entry point, and the only route this game registers.
// Receives the full request path and decides the verb itself, because it is
// also the thing that decided the path was its own (Filter). One owner for
// "is this mine" and "what did they ask for" means the two cannot disagree.
DEFINE_WORK_FUNC(ChessGame, Request)
{
    data.writeString(self.Request(data.restAsString()).c_str());
}

// Key -- read or set this game's equivalence class. A game with no key accepts
// nothing, so this is what a matchmaker calls to open a game for whoever holds
// the invite.
DEFINE_WORK_FUNC(ChessGame, Key)
{
    std::string k = data.restAsString();
    if (!k.empty()) self.SetMatchKey(k);
    data.writeString(self.MatchKey().c_str());
}

// Seats -- who holds what, for the shell. Tokens are opaque, so this reports
// occupancy rather than identity.
DEFINE_WORK_FUNC(ChessGame, Seats)
{
    (void)ctx;
    std::string s = "white:" + std::string(self.White().empty() ? "open" : "taken")
                  + " black:" + std::string(self.Black().empty() ? "open" : "taken");
    data.writeString(s.c_str());
}

DEFINE_WORK_FUNC(ChessGame, Chat)
{
    (void)ctx;
    data.writeString(self.ChatLog().c_str());
}

// ── The network edge ──────────────────────────────────────────────────────────
// Accept -- the subscriber ConnectionManager calls per accepted connection.
// Takes only Buffer data, never a NetworkProvider type: this module does not
// link against another module's types, which is what keeps ChessProvider
// loadable with no NetworkProvider present at all.
//
// Seats are NOT claimed here. A connection is per-request under
// Connection: close, so arriving means nothing durable -- claiming happens on
// the first legal move, in ApplyMove, where a token is actually present.
DEFINE_WORK_FUNC(ChessGame, Accept)
{
    (void)ctx;
    ETCS_LOG("ChessGame", "Accept: connection joined, key '" << self.MatchKey()
             << "' seats " << self.Seats() << "/2");
    data.writeString(self.game().toFENString().c_str());
}

// ── Trait-provided verbs, exported ────────────────────────────────────────────
// Inheriting a trait supplies the C++ method; the dispatch surface is built
// from work functions, so each still needs a thin forwarder to be reachable.
// These call the base's PROVIDED method, never the *Concrete implementation.
DEFINE_WORK_FUNC(ChessGame, Reset)
{
    bool ok = self.Reset();
    data.writeString(ok ? self.game().toFENString().c_str() : "FAILED");
}

DEFINE_WORK_FUNC(ChessGame, IsActive)
{
    data.writeString(self.IsActive() ? "active" : "finished");
}

DEFINE_WORK_FUNC(ChessGame, Delete)
{
    data.writeString(self.Delete() ? "deleted" : "FAILED");
}

DEFINE_WORK_FUNC(ChessGame, Filter)
{
    self.Accepts(data);   // empty result == declined, by Filter_'s own contract
}

#endif // CHESSPROVIDER_H__
