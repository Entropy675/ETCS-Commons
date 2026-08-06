#ifndef CHESSBOARD_H__
#define CHESSBOARD_H__
#include "../../../ontology.h"

// The CChess engine, a folder inside this module's own type folder so this
// header has somewhere to live beside it. Plain, dependency-free C++: no ETCS
// types, and no platform split (the ncurses/Windows fork lived entirely in the
// View, which the runtime replaces).
#include "CChess/src/Board.h"
#include <vector>
#include <string>

// ── ChessBoard ────────────────────────────────────────────────────────────────
// THE gateway type: the ontology leaf carrying CChess's Board into the runtime.
// The work functions in ChessProvider.h dispatch on THIS, never on ::Board,
// which knows nothing about entities, RIDs, or dispatch.
//
// Bases, and why each is claimed rather than reached for:
//   EphemeralBase — a chess game is resettable and has a real notion of being
//     finished. Reset IS "new game"; IsActive is "not yet checkmate or
//     stalemate". Both required methods are existing engine state, not
//     something invented to satisfy the trait.
//   DeletableBase — a game is destroyed on demand (finished, or abandoned),
//     through the same self-DestroyEvent protocol every deletable leaf uses.
// Deliberately NOT Gate_/Switchable_: the board has no open/closed or
// started/stopped axis of its own. The SERVER in front of it does, and that
// axis already lives on HttpServer/ConnectionManager.
//
// The Board is held BY VALUE and PRIVATE. By value so its lifetime is exactly
// this entity's lifetime -- an arena-resident ChessBoard needs no destructor of
// its own and nothing can dangle past it. Private because the work-function
// surface IS the security boundary: with no side door into the engine, Move is
// the only way a move ever reaches movePiece, which is what makes "both peers
// re-simulate, neither referees" a property of the type rather than a
// convention anyone could route around.
//
// One type, no typedef: with the View gone there is no OS axis left, which is
// why Contract_ChessProvider.h names this concrete type directly.
class ChessBoard :
    public EphemeralBase<ChessBoard>,
    public DeletableBase<ChessBoard>,
    public FilterBase<ChessBoard>
{
public:
    WIRE_TYPE_IDENTITY(ChessBoard);

    ChessBoard()  { board.setStartingBoard(true); }
    virtual ~ChessBoard() = default;

    // ── EphemeralBase ─────────────────────────────────────────────────────
    // Reset IS "new game" -- the trait's provided Reset() is exactly the verb
    // this type wanted, so there is no separate Reset work function.
    bool ResetConcrete()
    {
        board.setStartingBoard(true);
        last = ChessStatus::SUCCESS;
        return true;
    }

    // Active until the game is genuinely over: a finished game is inert, since
    // no legal move exists and nothing further can causally follow from it.
    //
    // `board` is mutable ONLY because CChess declares isCheckmate()/
    // isStalemate() non-const (they compute over the attack/move maps rather
    // than caching). Both are logically const queries, which is exactly what
    // mutable is for -- but the cleaner fix lives upstream: const-qualifying
    // those two in CChess would let this member drop the mutable entirely.
    bool IsActiveConcrete() const
    {
        return !(board.isCheckmate() || board.isStalemate());
    }

    // ── DeletableBase ─────────────────────────────────────────────────────
    // `self` is the WORK-FUNCTION parameter (the Type& DEFINE_WORK_FUNC hands
    // the body) and does not exist in here; mySelf() is this type's own
    // ModuleBundle (tag/owner/actions/ctx), not the entity. The entity is
    // simply `this` -- ChessBoard IS an Entity through Ephemeral_'s virtual
    // base -- so the source identity comes off this->. The this-> is required,
    // not stylistic: every base here is a dependent template base, so unqualified
    // lookup never finds an inherited member at template definition time.
    bool DeleteConcrete()
    {
        std::string conjugate_key = this->getSourceModule().toString() + ":"
                                   + this->getSourceTag().toString();
        ETCS_LOG("ChessBoard", "Delete: firing self-DestroyEvent for RID:"
                 << getRID() << " (" << conjugate_key << ")");
        // `this` converts to Entity* implicitly (derived-to-virtual-base upcast).
        ETCS::DestroyEvent{conjugate_key.c_str(), this, true}();
        return true;
    }

    // ── FilterBase ────────────────────────────────────────────────────────
    // Whoever routes crossings asks this before handing one over. `io` arrives
    // with the candidate's key (whatever travelled in the invite link or QR)
    // and, on acceptance, leaves with THIS board's key -- which is what makes
    // two candidates carrying the same key resolve to the SAME board without
    // the router knowing what a board is.
    //
    // const, and load-bearingly: this is evaluated speculatively against boards
    // that will NOT receive the candidate, so nothing here may claim a seat.
    // The seat is taken in Accept, by whoever actually wins the crossing.
    bool AcceptsConcrete(ETCS::Buffer& io) const
    {
        // Filter_'s contract is "empty io means declined" -- so a decline MUST
        // clear the buffer it was handed. Returning false while leaving the
        // descriptor in place reads to the caller as an acceptance whose key
        // happens to be the descriptor, which silently accepts every crossing.
        if (match_key_.empty()) { io.reset(); return false; } // unclaimed: keyed by a matchmaker first
        if (seats_ >= 2)        { io.reset(); return false; } // this game is full
        
        // Segment match, not equality: at the HTTP layer the descriptor is a
        // request path ("/game/ABC123/move/e2e4"), and this board's key is one
        // segment inside it. Whole-string equality would only ever work for a
        // gate-level descriptor that was nothing but the key.
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
        io.writeString(match_key_.c_str());     // accepted: report the class
        return true;
    }

    const std::string& MatchKey() const          { return match_key_; }
    void SetMatchKey(const std::string& k)       { match_key_ = k; }
    int  Seats() const                            { return seats_; }
    void TakeSeat()                               { if (seats_ < 2) ++seats_; }

    // ── Verbs ─────────────────────────────────────────────────────────────
    // Here rather than in the work functions because there are now TWO callers
    // of each: the ontology action (Move/Status, reached by name from .etcs or
    // the REPL) and Request's own path dispatch. Two copies of "parse a UCI
    // string and apply it" is exactly the drift that ends with the HTTP path
    // accepting a move the scripted path rejects, which would break the one
    // claim this type exists to make.
    std::string ApplyMove(const std::string& mv)
    {
        if (mv.size() < 4) return "ILLEGAL";
        Pos from(mv[0] - 'a', 8 - (mv[1] - '0'));
        Pos to  (mv[2] - 'a', 8 - (mv[3] - '0'));

        ChessStatus st = board.movePiece(from, to);
        last = st;
        if (st == ChessStatus::PROMOTE && mv.size() >= 5)
        {
            std::string promo(1, mv[4]);
            board.registerPromotion(promo);
        }
        return (st == ChessStatus::FAIL) ? "ILLEGAL" : board.toFENString();
    }

    std::string StatusLine() const
    {
        std::string s = board.isWhiteTurn() ? "w" : "b";
        if      (board.isCheckmate())       s += " checkmate";
        else if (board.isStalemate())       s += " stalemate";
        else if (board.sideToMoveInCheck()) s += " check";
        else                                s += " ok";
        return s;
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

    // Route a request path this board has already claimed via Accepts. The
    // shape is /<anything>/<key>/<verb>[/<arg>] -- the verb is the segment
    // AFTER this board's own key, so nothing here depends on a fixed prefix
    // and two boards can sit under different mount points.
    std::string Request(const std::string& path)
    {
        std::vector<std::string> seg;
        splitPath(path, seg);

        size_t k = seg.size();
        for (size_t i = 0; i < seg.size(); ++i)
            if (seg[i] == match_key_) { k = i; break; }
        if (k == seg.size()) return "NOT FOUND";

        const std::string verb = (k + 1 < seg.size()) ? seg[k + 1] : "";
        const std::string arg  = (k + 2 < seg.size()) ? seg[k + 2] : "";

        if (verb == "move")   return ApplyMove(arg);
        if (verb == "fen" || verb.empty()) return board.toFENString();
        if (verb == "status") return StatusLine();
        if (verb == "reset")  { ResetConcrete(); return board.toFENString(); }
        return "NOT FOUND";
    }

    // ── Engine access for this module's own work functions ────────────────
    ::Board&       game()       { return board; }
    const ::Board& game() const { return board; }

    ChessStatus lastStatus() const    { return last; }
    void setLastStatus(ChessStatus s) { last = s; }

private:
    mutable ::Board board;                   // the engine, by value, private
    std::string     match_key_;              // this game's equivalence class ("" = unclaimed)
    int             seats_ = 0;              // occupants; a game seats two
    ChessStatus last = ChessStatus::SUCCESS; // last move result (PROMOTE follow-up)
};

#endif // CHESSBOARD_H__
