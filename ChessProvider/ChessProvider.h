#ifndef CHESSPROVIDER_H__
#define CHESSPROVIDER_H__


#define ETCS_DLL_EXPORTS
#include "../../core_defs.h"
#include "../../ontology.h"
#include "Contract_ChessProvider.h"

// Contract_ChessProvider.h pulls in ChessProvider/ChessNode.h, which pulls in
// ChessLobby.h and ChessGame.h in turn. Three types, three roles:
//
//   ChessGame  -- a board. Knows verbs, not URLs.
//   ChessLobby -- a SELF. My edges, my record. Reached through my node.
//   ChessNode  -- the host. Owns selves and boards, parses paths, routes.
//
// The node is the centralization artifact: many lobbies makes it a server, one
// lobby makes it a peer, and nothing else about the arrangement differs.
//
// Buffer API note: ETCS::Buffer reads with restAsString() and writes with
// writeString(const char*), so every write below goes through .c_str().

// ── ChessGame ─────────────────────────────────────────────────────────────────
// The scripted/REPL surface. Every one of these serializes on the chess
// ordering stream, so they are safe to call from a pool thread and cannot
// interleave with an HTTP-driven move.

// No token, so this is the TRUSTED caller (script, REPL) and bypasses seat
// ownership by design: an operator unable to move a piece on their own server
// would be a strange kind of security. Seat enforcement lives on the request
// path, where the caller is a stranger.
DEFINE_WORK_FUNC(ChessGame, Move)
{
    (void)ctx;
    data.writeString(self.ApplyMove(data.restAsString()).c_str());
}

DEFINE_WORK_FUNC(ChessGame, Fen)
{
    (void)ctx;
    data.writeString(self.Fen().c_str());
}

DEFINE_WORK_FUNC(ChessGame, LoadFen)
{
    (void)ctx;
    data.writeString(self.LoadFenStr(data.restAsString()).c_str());
}

DEFINE_WORK_FUNC(ChessGame, Status)
{
    (void)ctx;
    data.writeString(self.StatusLine().c_str());
}

DEFINE_WORK_FUNC(ChessGame, Chat)
{
    (void)ctx;
    data.writeString(self.ChatLog().c_str());
}

// Standalone routing, for a board spawned with NO node in front of it (the
// single fixed game chess_server.etcs opens). Under a node this is unused --
// the node parses paths and calls the game's verb surface directly.
DEFINE_WORK_FUNC(ChessGame, Request)
{
    (void)ctx;
    data.writeString(self.Request(data.restAsString()).c_str());
}

DEFINE_WORK_FUNC(ChessGame, Key)
{
    (void)ctx;
    data.writeString(self.KeyVerb(data.restAsString()).c_str());
}

// Occupancy, for the shell. Tokens are opaque, so this reports whether a seat
// is held rather than who holds it.
DEFINE_WORK_FUNC(ChessGame, Seats)
{
    (void)ctx;
    std::string s = "white:" + std::string(self.White().empty() ? "open" : "taken")
                  + " black:" + std::string(self.Black().empty() ? "open" : "taken");
    data.writeString(s.c_str());
}

// The subscriber ConnectionManager calls per accepted connection. Takes only
// Buffer data, never a NetworkProvider type: this module does not link against
// another module's types, which is what keeps ChessProvider loadable with no
// NetworkProvider present at all.
//
// Seats are NOT claimed here. A connection is per-request under
// Connection: close, so arriving means nothing durable -- claiming happens on
// the first legal move, where a token is actually present.
DEFINE_WORK_FUNC(ChessGame, Accept)
{
    (void)ctx;
    ETCS_LOG("ChessGame", "Accept: connection joined, match '" << self.MatchKey()
             << "' seats " << self.Seats() << "/2");
    data.writeString(self.Fen().c_str());
}

// Trait-provided verbs, exported. Inheriting a trait supplies the C++ method;
// the dispatch surface is built from work functions, so each still needs a thin
// forwarder. These call the base's PROVIDED method, never the *Concrete impl.
DEFINE_WORK_FUNC(ChessGame, Reset)
{
    (void)ctx;
    data.writeString(self.Reset() ? self.Fen().c_str() : "FAILED");
}

DEFINE_WORK_FUNC(ChessGame, IsActive)
{
    (void)ctx;
    data.writeString(self.IsActive() ? "active" : "finished");
}

DEFINE_WORK_FUNC(ChessGame, Delete)
{
    (void)ctx;
    data.writeString(self.Delete() ? "deleted" : "FAILED");
}

DEFINE_WORK_FUNC(ChessGame, Filter)
{
    (void)ctx;
    self.Accepts(data);   // empty result == declined, by Filter_'s own contract
}

// ── ChessLobby ────────────────────────────────────────────────────────────────
// A SELF exports no verbs of its own. It is reached through its node, which is
// also the P2P shape: your lobby is the one your own node hosts, and there is
// nobody else to address it from. Adding Request/List here would make a self
// addressable two ways, parsing two different URL shapes for the same thing --
// exactly the divergence the node-owns-routing split was made to close.
DEFINE_WORK_FUNC(ChessLobby, Filter)
{
    (void)ctx;
    self.Accepts(data);
}

DEFINE_WORK_FUNC(ChessLobby, Delete)
{
    (void)ctx;
    data.writeString(self.Delete() ? "deleted" : "FAILED");
}

// ── ChessNode ─────────────────────────────────────────────────────────────────
// THE http entry point. One route registers against this pair (Request +
// Filter) and everything else is resolved behind it by path.
DEFINE_WORK_FUNC(ChessNode, Request)
{
    (void)ctx;
    data.writeString(self.Request(data.restAsString()).c_str());
}

// Every self on this node: "name W L D locked|open games idle". The operator
// view and the leaderboard, same call -- and the only way to see selves at all,
// since a lobby exists only once somebody has been here.
DEFINE_WORK_FUNC(ChessNode, Players)
{
    (void)ctx;
    data.writeString(self.Players().c_str());
}

// Every match: "match occupants seats state". Matches are created on demand, so
// this is likewise the only enumeration that exists -- there is no static list
// of legal paths to inspect.
DEFINE_WORK_FUNC(ChessNode, Rooms)
{
    (void)ctx;
    data.writeString(self.Rooms().c_str());
}

DEFINE_WORK_FUNC(ChessNode, Mount)
{
    (void)ctx;
    std::string m = data.restAsString();
    if (!m.empty()) self.SetMount(m);
    data.writeString(self.MountPath().c_str());
}

DEFINE_WORK_FUNC(ChessNode, Filter)
{
    (void)ctx;
    self.Accepts(data);
}

DEFINE_WORK_FUNC(ChessNode, Delete)
{
    (void)ctx;
    data.writeString(self.Delete() ? "deleted" : "FAILED");
}

#endif // CHESSPROVIDER_H__