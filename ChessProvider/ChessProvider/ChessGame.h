#ifndef CHESSGAME_H__
#define CHESSGAME_H__
#include "../../../ontology.h"

// The CChess engine, a folder inside this module's own type folder so this
// header has somewhere to live beside it. Plain, dependency-free C++: no ETCS
// types, and no platform split (the ncurses/Windows fork lived entirely in the
// View, which the runtime replaces).
#include "CChess/src/Board.h"
#include <vector>
#include <string>
#include <chrono>

// ── ChessGame ─────────────────────────────────────────────────────────────────
// Renamed from ChessBoard, because it stopped being one. A board is a position;
// this owns seats, turn ownership, spectators, liveness and abandonment policy,
// and only DELEGATES the position to CChess. The name now says which of those
// it is.
//
// Bases:
//   EphemeralBase — resettable, with a real notion of being finished.
//   DeletableBase — destroyed on demand, via the self-DestroyEvent protocol.
//   FilterBase    — decides for itself which request paths are its own.
// Still NOT Gate_/Switchable_: no open/closed or started/stopped axis of its
// own. That belongs to the server in front of it.
//
// ── Identity, and why it is a token rather than a connection ─────────────────
// The obvious design is "a side belongs to a connection". It does not work
// here, and the log says why: this server answers with Connection: close, so
// SocketConnectionState is minted per REQUEST and deleted the moment the
// response is sent. There is no connection that outlives a single fetch, so
// there is nothing stable to bind a side to.
//
// So identity is a client-generated TOKEN carried in the path. That is not a
// workaround -- it is the same shape the P2P version needs, where there is no
// server holding connection state at all, and later the same slot an ACE
// identity key drops into. The token is the player; the connection is a
// courier.
//
// ── Claiming, and why first-mover ────────────────────────────────────────────
// A seat is claimed by MOVING, not by arriving. Arriving is free and unlimited
// (that is what a spectator is), so a claim has to cost something only a player
// would spend -- and the first legal move for a side is exactly that. It also
// means no lobby state exists before the game is real: two people opening the
// link are two spectators until one of them plays.
class ChessGame :
    public EphemeralBase<ChessGame>,
    public DeletableBase<ChessGame>,
    public FilterBase<ChessGame>
{
public:
    WIRE_TYPE_IDENTITY(ChessGame);

    using Clock = std::chrono::steady_clock;

    ChessGame()  { board.setStartingBoard(true); refreshTerminal(); }
    virtual ~ChessGame() = default;

    // ── EphemeralBase ─────────────────────────────────────────────────────
    bool ResetConcrete()
    {
        board.setStartingBoard(true);
        last = ChessStatus::SUCCESS;
        white_.clear(); black_.clear();
        seen_.clear();
        refreshTerminal();
        return true;
    }

    // Reads the CACHED terminal state rather than recomputing. Two reasons, and
    // the second is the real one: isCheckmate()/isStalemate() are non-const in
    // CChess and cannot honestly be made const (they simulate a move by mutating
    // the board and restoring it -- see Board.h), so a const query recomputing
    // them would force this whole class to hold a `mutable` Board. Caching at
    // the one moment the answer can change (a move landing) is both cheaper and
    // const-honest, and it is what lets `board` below be an ordinary member.
    bool IsActiveConcrete() const { return active_; }

    // ── DeletableBase ─────────────────────────────────────────────────────
    bool DeleteConcrete()
    {
        std::string conjugate_key = this->getSourceModule().toString() + ":"
                                   + this->getSourceTag().toString();
        ETCS_LOG("ChessGame", "Delete: firing self-DestroyEvent for RID:"
                 << getRID() << " (" << conjugate_key << ")");
        ETCS::DestroyEvent{conjugate_key.c_str(), this, true}();
        return true;
    }

    // ── FilterBase ────────────────────────────────────────────────────────
    // Whoever routes crossings asks this before handing one over. `io` arrives
    // with the candidate descriptor and, on acceptance, leaves with THIS game's
    // key -- which is what makes two candidates carrying the same key resolve to
    // the same game without the router knowing what chess is.
    //
    // NOTE the seat check is gone. It used to decline once the game was full,
    // which silently made spectating impossible: a third viewer's path stopped
    // matching and fell through to a 404. Fullness is a question about MOVING,
    // and it is answered in ApplyMove where the move actually happens.
    bool AcceptsConcrete(ETCS::Buffer& io) const
    {
        // Filter_'s contract is "empty io means declined", so a decline MUST
        // clear the buffer. Returning false while leaving the descriptor in
        // place reads to the caller as an acceptance whose key happens to be
        // the descriptor -- which silently accepts every crossing.
        if (match_key_.empty()) { io.reset(); return false; }

        const std::string desc = io.restAsString();
        bool found = false;
        for (size_t i = 0; !found && i < desc.size(); )
        {
            size_t j = desc.find('/', i);
            if (j == std::string::npos) j = desc.size();
            if (desc.compare(i, j - i, match_key_) == 0) found = true;
            i = j + 1;
        }
        if (!found) { io.reset(); return false; }
        io.writeString(match_key_.c_str());
        return true;
    }

    const std::string& MatchKey() const    { return match_key_; }
    void SetMatchKey(const std::string& k) { match_key_ = k; }

    // ── Seats ─────────────────────────────────────────────────────────────
    const std::string& White() const { return white_; }
    const std::string& Black() const { return black_; }
    int  Seats() const { return (white_.empty() ? 0 : 1) + (black_.empty() ? 0 : 1); }

    // "white" | "black" | "viewer" -- what this token is allowed to be right
    // now. A token that holds no seat is a viewer, which is the default and
    // needs no registration.
    std::string RoleOf(const std::string& tok) const
    {
        if (!tok.empty() && tok == white_) return "white";
        if (!tok.empty() && tok == black_) return "black";
        return "viewer";
    }

    // ── Verbs ─────────────────────────────────────────────────────────────
    // Here rather than in the work functions because each has two callers: the
    // ontology action (reached by name from .etcs or the REPL) and Request's
    // own path dispatch. Two copies of "parse a UCI string and apply it" is the
    // drift that ends with the HTTP path accepting a move the scripted path
    // rejects, which would break the one claim this type exists to make.
    //
    // tok empty == the trusted local caller (a script, the REPL). It bypasses
    // seat ownership deliberately: that path is already inside the trust
    // boundary, and an operator being unable to move a piece on their own
    // server would be a strange kind of security.
    std::string ApplyMove(const std::string& mv, const std::string& tok = "")
    {
        if (mv.size() < 4) return "ILLEGAL";

        if (!tok.empty())
        {
            const bool white_to_move = board.isWhiteTurn();
            std::string& seat = white_to_move ? white_ : black_;
            const std::string& other = white_to_move ? black_ : white_;

            // Claim by moving. The seat for the side to move is free, and this
            // token does not already hold the OTHER seat -- so playing both
            // colours from one browser is refused, which is what stops a single
            // visitor from quietly becoming the whole game.
            if (seat.empty())
            {
                if (tok == other) return "NOT YOUR TURN";
                seat = tok;
                ETCS_LOG("ChessGame", "seat claimed: "
                         << (white_to_move ? "white" : "black") << " -> " << tok);
            }
            else if (seat != tok)
            {
                // Held by someone else. Distinguish the two reasons, because
                // they mean different things to a client: a spectator should be
                // told they have no seat, a player should be told to wait.
                return (tok == other) ? "NOT YOUR TURN" : "NOT YOUR SEAT";
            }
        }

        Pos from(mv[0] - 'a', 8 - (mv[1] - '0'));
        Pos to  (mv[2] - 'a', 8 - (mv[3] - '0'));

        ChessStatus st = board.movePiece(from, to);
        last = st;
        if (st == ChessStatus::PROMOTE && mv.size() >= 5)
        {
            std::string promo(1, mv[4]);
            board.registerPromotion(promo);
        }
        if (st == ChessStatus::FAIL) return "ILLEGAL";

        started_ = true;
        refreshTerminal();
        return board.toFENString();
    }

    // Cached side-to-move/terminal summary, plus this token's own role, so one
    // request tells a client everything it needs to render its controls.
    std::string StatusLine(const std::string& tok = "") const
    {
        std::string s = board.isWhiteTurn() ? "w" : "b";
        if      (checkmate_)                 s += " checkmate";
        else if (stalemate_)                 s += " stalemate";
        else if (board.sideToMoveInCheck())  s += " check";
        else                                 s += " ok";
        s += " " + RoleOf(tok);
        s += white_.empty() ? " open" : " taken";
        s += black_.empty() ? " open" : " taken";
        return s;
    }

    // ── Liveness / abandonment ────────────────────────────────────────────
    // HTTP gives no disconnect signal, so presence is inferred from traffic:
    // the page polls, and every poll is a heartbeat. Reap runs on each request
    // rather than on a timer -- a game nobody is polling has nobody to notice
    // it, so there is nothing to do until someone shows up.
    void Touch(const std::string& tok)
    {
        if (tok.empty()) return;
        seen_[tok] = Clock::now();
    }

    // Drops seats whose holder has gone quiet, and resets the position if the
    // game was abandoned before it ever really started. A game WITH moves is
    // kept: the position is worth more than the seat, and whoever returns with
    // the same token reclaims their side (their seat is only released after the
    // grace period, and reclaiming is just the empty-seat path in ApplyMove).
    void ReapStale(int grace_seconds = 30)
    {
        const auto now = Clock::now();
        auto gone = [&](const std::string& tok)
        {
            if (tok.empty()) return false;
            auto it = seen_.find(tok);
            if (it == seen_.end()) return true;
            return std::chrono::duration_cast<std::chrono::seconds>(
                       now - it->second).count() >= grace_seconds;
        };

        if (gone(white_)) { ETCS_LOG("ChessGame", "white seat released (idle): " << white_); white_.clear(); }
        if (gone(black_)) { ETCS_LOG("ChessGame", "black seat released (idle): " << black_); black_.clear(); }

        if (!started_ && white_.empty() && black_.empty() && !seen_.empty())
        {
            ETCS_LOG("ChessGame", "abandoned before first move -- resetting.");
            ResetConcrete();
        }
    }

    // ── Chat ──────────────────────────────────────────────────────────────
    // A bounded ring of lines, deliberately trivial: chat is presence, not
    // history, and anything durable belongs in a database provider rather than
    // in the game's own arena footprint.
    void Say(const std::string& tok, const std::string& text)
    {
        if (text.empty()) return;
        std::string who = RoleOf(tok);
        chat_.push_back(who + ": " + text);
        if (chat_.size() > kChatLines) chat_.erase(chat_.begin());
    }

    std::string ChatLog() const
    {
        std::string out;
        for (const auto& line : chat_) { out += line; out += "\n"; }
        return out;
    }

    // Split a request path into segments, dropping empties (so a leading or
    // doubled slash costs nothing).
    static void splitPath(const std::string& path, std::vector<std::string>& out)
    {
        for (size_t i = 0; i < path.size(); )
        {
            size_t j = path.find('/', i);
            if (j == std::string::npos) j = path.size();
            if (j > i) out.emplace_back(path, i, j - i);
            i = j + 1;
        }
    }

    // Route a request path this game has already claimed via Accepts. Shape is
    //   /<anything>/<key>/<token>/<verb>[/<arg>]
    // The token sits between key and verb: the key says WHICH game, the token
    // says WHO, and the verb says WHAT -- read left to right, narrowing. The
    // verb is located relative to the key rather than at a fixed depth, so two
    // games can sit under different mount points.
    std::string Request(const std::string& path)
    {
        std::vector<std::string> seg;
        splitPath(path, seg);

        size_t k = seg.size();
        for (size_t i = 0; i < seg.size(); ++i)
            if (seg[i] == match_key_) { k = i; break; }
        if (k == seg.size()) return "NOT FOUND";

        const std::string tok  = (k + 1 < seg.size()) ? seg[k + 1] : "";
        const std::string verb = (k + 2 < seg.size()) ? seg[k + 2] : "";
        const std::string arg  = (k + 3 < seg.size()) ? seg[k + 3] : "";

        Touch(tok);      // every request is a heartbeat
        ReapStale();     // and an opportunity to notice someone else's absence

        if (verb == "move")   return ApplyMove(arg, tok);
        if (verb == "fen" || verb.empty()) return board.toFENString();
        if (verb == "status") return StatusLine(tok);
        if (verb == "say")    { Say(tok, arg); return ChatLog(); }
        if (verb == "chat")   return ChatLog();
        if (verb == "reset")  { ResetConcrete(); return board.toFENString(); }
        return "NOT FOUND";
    }

    // ── Engine access for this module's own work functions ────────────────
    ::Board&       game()       { return board; }
    const ::Board& game() const { return board; }

    ChessStatus lastStatus() const    { return last; }
    void setLastStatus(ChessStatus s) { last = s; }

private:
    static constexpr size_t kChatLines = 40;

    // Recompute the terminal flags. Called ONLY where the answer can change --
    // construction, reset, and a move landing -- which is what keeps every
    // query const without making the Board mutable.
    void refreshTerminal()
    {
        checkmate_ = board.isCheckmate();
        stalemate_ = board.isStalemate();
        active_    = !(checkmate_ || stalemate_);
    }

    ::Board     board;                       // the engine, by value, private
    std::string match_key_;                  // this game's equivalence class ("" = unclaimed)
    ChessStatus last = ChessStatus::SUCCESS; // last move result (PROMOTE follow-up)

    std::string white_, black_;              // seat holders, by token ("" = open)
    bool        started_   = false;          // has any legal move landed
    bool        checkmate_ = false;
    bool        stalemate_ = false;
    bool        active_    = true;

    std::unordered_map<std::string, Clock::time_point> seen_; // token -> last heartbeat
    std::vector<std::string> chat_;
};

#endif // CHESSGAME_H__
