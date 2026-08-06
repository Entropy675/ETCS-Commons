#ifndef CHESSBOARD_H__
#define CHESSBOARD_H__
#include "../../../ontology.h"

// The CChess engine, a folder inside this module's own type folder so this
// header has somewhere to live beside it. Plain, dependency-free C++: no ETCS
// types, and no platform split (the ncurses/Windows fork lived entirely in the
// View, which the runtime replaces).
#include "CChess/src/Board.h"

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
    public DeletableBase<ChessBoard>
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

    // ── Engine access for this module's own work functions ────────────────
    ::Board&       game()       { return board; }
    const ::Board& game() const { return board; }

    ChessStatus lastStatus() const    { return last; }
    void setLastStatus(ChessStatus s) { last = s; }

private:
    mutable ::Board board;                   // the engine, by value, private
    ChessStatus last = ChessStatus::SUCCESS; // last move result (PROMOTE follow-up)
};

#endif // CHESSBOARD_H__
