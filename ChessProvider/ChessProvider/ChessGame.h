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
#include <atomic>
#include <chrono>
#include <unordered_map>

class ChessGame;
class ChessLobby;
class ChessNode;

// ── Serialization ─────────────────────────────────────────────────────────────
// Work functions dispatch on ThreadPool threads and a route can be driven by ANY
// connection, so unlike a type reached only through its own IO chain, two
// requests genuinely execute here at once. Two concurrent Requests both entered
// ResetConcrete and raced inside setStartingBoard: one deleted the Piece objects
// while the other was still walking them (a reproduced SIGSEGV in ~Piece,
// surfacing as a glibc double-free).
//
// An EventStream rather than a mutex, because here the ORDER is the semantics,
// not merely a safety property. Two connections submitting moves need a globally
// agreed sequence regardless of memory safety; a lock would give arbitrary
// non-corrupting interleaving, while the stream gives the actual causal chain --
// the same sequence a peer would replay to verify the game. It also keeps pool
// threads free: work queues instead of stalling every other connection behind a
// held lock.
//
// The mechanism is simply that the ordering thread is a SINGLE thread. Nothing
// reached from on_event needs a lock, which is why every *Locked body below has
// no synchronisation of its own -- and it is also what lets a game call straight
// into its lobby to report an outcome without any handshake.

// ── The wire event ────────────────────────────────────────────────────────────
// The struct that actually crosses the ring, mirroring DLInEvent: a flat POD of
// inputs plus POINTERS to the caller's own completion slots. Nothing here owns
// anything -- arg/tok point into the caller's strings and result_out/done point
// at members of the ChessOpEvent below, all of which the blocking wait in
// operator() keeps alive for the whole dispatch.
struct ChessInEvent
{
    enum class Kind : uint8_t { Request, Move, LoadFen, Fen, Status, Chat, Say,
                                Key, Reset, IsActive,
                                LobbyRequest, LobbyList, LobbyJoin, LobbyPlayers };

    Kind        kind;
    void*       target = nullptr;   // ChessGame* or ChessLobby*, per kind

    const char* arg = nullptr;      // path / uci / fen / chat text / key
    const char* tok = nullptr;      // who, when the caller knows

    std::string*       result_out = nullptr;
    std::atomic<bool>* done       = nullptr;
};

// Only the POINTER rides the ring. LBuffer is 32 bytes and the LMAX adapter is a
// fast hand-off, so the event itself stays on the caller's stack -- putting the
// payload type in the stream directly instead made enqueue memcpy the whole
// struct through that 32-byte slot and silently overrun it.
struct ChessInEventPtr
{
    ChessInEvent* ptr;
};

// Nothing is shared between events -- all state lives on the entity each event
// points at -- so the stream's own State is empty.
struct ChessState {};

struct ChessStream : ETCS::EventStream<ChessStream, ChessState, ChessInEventPtr>
{
    // Defined at the bottom of ChessNode.h, not here: it dispatches to BOTH
    // types and so needs both complete.
    ETCS::DispatchResult on_event(ChessState&, const ChessInEventPtr& evt, uint64_t seq);

    // Never reached: every event is handled synchronously inside on_event and
    // returns Drop, so no gap slot is acquired and no completion is posted.
    void on_completion(ChessState&, ETCS::WorkResult*, uint64_t) {}
    void on_emit(ChessState&, ETCS::GapSlot&)                   {}

    // NO getInstance() ANYMORE. A stream is owned by a ChessNode -- see
    // ChessNode's own comment for why the node is the ordering domain rather
    // than the module or the board.
    //
    // What this type deliberately no longer does is name a particular stream. A
    // state machine now REQUIRES an output target rather than reaching for a
    // module-global, so the identical type code serves per-node, per-board, or
    // N-boards-hashed-to-k wiring with no source change. Granularity became a
    // deployment decision instead of a release decision.
    //
    // The old comment justified one stream for every game in the module as
    // "ordering across unrelated games is harmless (a chess move is not
    // latency-critical) and a thread per game would be absurd". Both halves
    // were answering the wrong question:
    //
    //   - Harmless is true of a peer running one game and false of a server
    //     running many. Two boards share no causal relation, so a shared
    //     sequence asserts one that does not exist, and charges for the
    //     assertion in head-of-line blocking proportional to MODULE-WIDE
    //     traffic rather than to any one game's. Every ChessOpEvent waiter is
    //     a parked ThreadPool thread, so that queue depth converts directly
    //     into pool starvation.
    //
    //   - "A thread per game would be absurd" answers a proposal nobody had to
    //     make. An ordering domain needs a serialization point, not a thread;
    //     the two were only ever collapsed by getInstance() pairing them. Per
    //     NODE is one thread in the deployment that actually exists, since
    //     nodes are few and games are many.
};

// ── The caller-facing event ───────────────────────────────────────────────────
// Constructed on the caller's stack, invoked, and read: the object IS the
// completion slot, exactly as AddTagEvent's own result/ready members are.
//
// One event type with a Kind rather than one struct per verb (LoadEvent,
// ResolveEvent, ...): the loader's events are separate structs because their
// payloads and return types genuinely differ, while every chess operation is
// (target, arg, tok) -> string. Fourteen identical structs would be ceremony.
struct ChessOpEvent
{
    // The target ordering domain, handed in rather than looked up. Null is a
    // real state -- a board with no node in front of it -- and it is REFUSED
    // rather than falling back to a direct call. A fallback would make the
    // unsynchronised path the quiet one, and that path is precisely the
    // reproduced SIGSEGV in ~Piece this stream was introduced to close.
    ChessStream*       stream;
    ChessInEvent::Kind kind;
    void*              target;
    std::string        arg;
    std::string        tok;

    std::string        result;
    std::atomic<bool>  done{false};

    ChessOpEvent(ChessStream* st, ChessInEvent::Kind k, void* t,
                 std::string a = "", std::string s = "")
        : stream(st), kind(k), target(t), arg(std::move(a)), tok(std::move(s)) {}

    std::string operator()();
};

inline std::string ChessOpEvent::operator()()
{
    // Guards the one genuinely dangerous case: being called from an ordering
    // thread, where waiting on a stream that may be ordered behind you is the
    // deadlock already hit once with the ack round-trip. It cannot catch re-entry
    // from THIS stream's own thread, but on_event only ever calls the *Locked
    // bodies, never back through here.
    // NOTE, and a genuine loosening: with one stream per node this asks whether
    // the caller is on AN ordering thread, not whether it is on THIS one. So a
    // future node-to-node call trips it spuriously, AND the A->B-while-B->A
    // deadlock that a single global stream made structurally impossible is no
    // longer impossible. Cross-node calls have to be non-blocking; enforcing
    // that here needs the assert to carry a stream identity.
    ETCS_ASSERT_NOT_ORDERING_THREAD("ChessOpEvent");

    // A board with no node cannot be driven safely, so it is not driven at all.
    // This is a BEHAVIOUR CHANGE for any script that spawns a bare ChessGame:
    // every verb on it now answers "NO NODE" instead of running. Mint a node.
    if (!stream) return "NO NODE";

    ChessInEvent in;
    in.kind       = kind;
    in.target     = target;
    in.arg        = arg.c_str();
    in.tok        = tok.c_str();
    in.result_out = &result;
    in.done       = &done;

    // false means the stream is tearing down -- nothing will ever service this,
    // so return rather than spin on a flag that cannot flip.
    if (!stream->enqueue(ChessInEventPtr{&in}))
        return "BUSY";

    // progressiveYield, NOT a bare spin. This wait runs on a ThreadPool thread
    // and now fires on every HTTP request, so a hot spin here burns a whole core
    // per in-flight request: with four hardware threads, four concurrent
    // requests starve the very ordering thread they are waiting on, AND leave
    // nobody draining io_uring -- which presents as the listener accepting
    // connections only intermittently, nowhere near the actual cause.
    //
    // The loader's own events spin bare and get away with it because they are
    // rare. Chess events are continuous, so this one has to yield.
    int retry = 0;
    while (!done.load(std::memory_order_acquire))
        ETCS::LMAXSequentialSharedPage::progressiveYield(retry);
    return result;
}

// ── ChessGame ─────────────────────────────────────────────────────────────────
// Renamed from ChessBoard because it stopped being one. A board is a position;
// this owns seats, turn ownership, spectators, liveness and abandonment policy,
// and only DELEGATES the position to CChess.
//
// Bases: EphemeralBase (resettable, with a real notion of being finished),
// DeletableBase (destroyed on demand), FilterBase (decides which request paths
// are its own). Still NOT Gate_/Switchable_: no open/closed axis of its own --
// that belongs to the server in front of it.
//
// Identity is a client-generated TOKEN carried in the path, not a connection:
// this server answers Connection: close, so a SocketConnectionState is minted
// per REQUEST and deleted when the response is sent. Nothing on the wire
// outlives a single fetch, so there is nothing stable to bind a side to. The
// token is also the shape the P2P version needs, and the slot an ACE identity
// key later drops into.
//
// Seats are claimed by MOVING, not arriving. Arriving is free and unlimited
// (that is what a spectator is), so a claim must cost something only a player
// would spend, and the first legal move for a side is exactly that.
class ChessGame :
    public EphemeralBase<ChessGame>,
    public DeletableBase<ChessGame>,
    public FilterBase<ChessGame>
{
    friend struct ChessStream;  // the only caller of the *Locked bodies
    friend class  ChessLobby;   // holds an edge to this match
    friend class  ChessNode;    // owns the board; same ordering thread

public:
    WIRE_TYPE_IDENTITY(ChessGame);

    using Clock = std::chrono::steady_clock;
    using Kind  = ChessInEvent::Kind;

    ChessGame()  { board.setStartingBoard(true); refreshTerminal(); }
    virtual ~ChessGame() = default;

    // ── Public surface: every one of these serializes ─────────────────────
    std::string Request(const std::string& path)      { return op(Kind::Request, path); }
    std::string ApplyMove(const std::string& mv,
                          const std::string& tok = "") { return op(Kind::Move, mv, tok); }
    std::string LoadFenStr(const std::string& fen)    { return op(Kind::LoadFen, fen); }
    std::string Fen() const                            { return op(Kind::Fen); }
    std::string StatusLine(const std::string& t = "") const { return op(Kind::Status, "", t); }
    std::string ChatLog() const                        { return op(Kind::Chat); }
    std::string KeyVerb(const std::string& k = "")     { return op(Kind::Key, k); }

    // ── EphemeralBase / DeletableBase ─────────────────────────────────────
    bool ResetConcrete() { return op(Kind::Reset) != "BUSY"; }

    // Reads the CACHED terminal flag rather than recomputing. Two reasons, the
    // second load-bearing: isCheckmate()/isStalemate() are non-const in CChess
    // and cannot honestly be made const (they simulate a move by mutating the
    // board and restoring it), so recomputing here would force a mutable Board.
    // Caching at the one moment the answer can change is cheaper and
    // const-honest. A plain bool read races benignly with a move landing --
    // worst case a caller sees the previous answer one request early.
    bool IsActiveConcrete() const { return active_; }

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
    // Deliberately NOT routed through the stream. It reads only match_key_, and
    // is called from route dispatch on a pool thread for EVERY request -- a
    // round trip here would serialize path matching behind game logic for paths
    // that turn out not to be ours at all.
    //
    // The tradeoff: match_key_ must be treated as set-once (Key at setup, not
    // mid-game). If it ever needs to change at runtime, this becomes an event.
    //
    // Filter_'s contract is "empty io means declined", so a decline MUST clear
    // the buffer -- returning false while leaving the descriptor in place reads
    // to the caller as an acceptance whose key happens to be the descriptor,
    // which silently accepts every crossing.
    //
    // No seat check: fullness is a question about MOVING, answered in
    // applyMoveLocked. Declining here once full silently made spectating
    // impossible -- a third viewer's path stopped matching and 404'd.
    bool AcceptsConcrete(ETCS::Buffer& io) const
    {
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
    void SetMatchKey(const std::string& k) { match_key_ = k; }   // setup only

    const std::string& White() const { return white_; }
    const std::string& Black() const { return black_; }
    int  Seats() const { return (white_.empty() ? 0 : 1) + (black_.empty() ? 0 : 1); }

    // Shared with ChessLobby, which splits the same paths.
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

private:
    static constexpr size_t kChatLines = 40;

    // Defined at the bottom of ChessNode.h -- it needs ChessNode complete.
    // A board's ordering domain is its node's, so a board with no node has
    // none and every verb on it refuses.
    ChessStream* streamOf() const;

    // const w.r.t. the caller only: the ordering thread mutates through target.
    std::string op(Kind k, const std::string& arg = "",
                   const std::string& tok = "") const
    {
        return ChessOpEvent{streamOf(), k, const_cast<ChessGame*>(this), arg, tok}();
    }

    // ── Everything below runs ONLY on the ordering thread ─────────────────
    // Single thread, therefore no synchronisation. Nothing here may call op():
    // that would enqueue behind itself and never complete.

    std::string fenLocked() const { return board.toFENString(); }

    // Distinct tokens with recent traffic. This, not Seats(), is what "is this
    // room full" means: seats are claimed by MOVING, so two people staring at
    // the opening position occupy a room while holding zero seats.
    int liveTokensLocked(int grace_seconds) const
    {
        const auto now = Clock::now();
        int n = 0;
        for (const auto& [t, when] : seen_)
            if (std::chrono::duration_cast<std::chrono::seconds>(now - when).count()
                    < grace_seconds) ++n;
        return n;
    }

    bool resetLocked()
    {
        board.setStartingBoard(true);
        last = ChessStatus::SUCCESS;
        white_.clear(); black_.clear();
        seen_.clear();
        over_.clear(); draw_offer_.clear();
        recorded_ = false;
        started_  = false;
        refreshTerminal();
        return true;
    }

    std::string loadFenLocked(const std::string& fen)
    {
        if (!board.loadFEN(fen)) return "INVALID";
        refreshTerminal();
        return board.toFENString();
    }

    std::string keyLocked(const std::string& k)
    {
        if (!k.empty()) match_key_ = k;
        return match_key_;
    }

    bool isActiveLocked() const { return active_; }

    // Report the finished game to the lobby's player records. Defined at the
    // bottom of ChessLobby.h -- it needs the lobby complete, and this call is a
    // plain function call rather than an event precisely because both types live
    // on the same ordering thread.
    //
    // Guarded by recorded_ so an outcome counts exactly once no matter how many
    // times a finished game is polled afterwards.
    void reportOutcomeLocked();

    // tok empty == the trusted local caller (script, REPL). It bypasses seat
    // ownership deliberately: that path is already inside the trust boundary,
    // and an operator unable to move a piece on their own server would be a
    // strange kind of security.
    std::string applyMoveLocked(const std::string& mv, const std::string& tok)
    {
        // A resignation or agreed draw leaves a legal position behind, so
        // nothing about the board itself would refuse this. Checked before the
        // move, not after, so a finished game cannot be quietly continued.
        if (!over_.empty()) return "GAME OVER";
        if (mv.size() < 4)  return "ILLEGAL";

        if (!tok.empty())
        {
            const bool white_to_move = board.isWhiteTurn();
            std::string& seat        = white_to_move ? white_ : black_;
            const std::string& other = white_to_move ? black_ : white_;

            // Claim by moving: the seat for the side to move is free, and this
            // token does not already hold the OTHER seat -- so one browser
            // cannot quietly become both players.
            if (seat.empty())
            {
                if (tok == other) return "NOT YOUR TURN";
                seat = tok;
                ETCS_LOG("ChessGame", "seat claimed: "
                         << (white_to_move ? "white" : "black") << " -> " << tok);
            }
            else if (seat != tok)
            {
                // Distinguish the two reasons: a spectator should be told they
                // have no seat, a player should be told to wait.
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
        // Moving answers a pending offer: playing on IS declining, the ordinary
        // convention, and it saves the offerer waiting for a reply already given.
        draw_offer_.clear();
        refreshTerminal();
        if (!active_) reportOutcomeLocked();
        return board.toFENString();
    }

    // Resigning and offering a draw are seated privileges: a spectator has
    // nothing to give up. Both refuse a finished game rather than overwriting
    // its outcome.
    std::string resignLocked(const std::string& tok)
    {
        const std::string role = roleOfLocked(tok);
        if (role == "viewer") return "NOT YOUR SEAT";
        if (!over_.empty())   return "GAME OVER";
        over_ = "resign-" + role;
        draw_offer_.clear();
        ETCS_LOG("ChessGame", "resignation: " << role << " (" << tok << ")");
        refreshTerminal();
        reportOutcomeLocked();
        return statusLineLocked(tok);
    }

    // One verb for offer AND accept: an offer standing from the OTHER player
    // makes this an acceptance, otherwise it records yours. Re-offering your own
    // is a no-op rather than an error -- a double click should not be a
    // self-agreed draw, which is why the offerer's own token is excluded.
    std::string drawLocked(const std::string& tok)
    {
        const std::string role = roleOfLocked(tok);
        if (role == "viewer") return "NOT YOUR SEAT";
        if (!over_.empty())   return "GAME OVER";

        if (!draw_offer_.empty() && draw_offer_ != tok)
        {
            over_ = "draw";
            draw_offer_.clear();
            ETCS_LOG("ChessGame", "draw agreed");
            refreshTerminal();
            reportOutcomeLocked();
            return statusLineLocked(tok);
        }
        draw_offer_ = tok;
        ETCS_LOG("ChessGame", "draw offered by " << role);
        return statusLineLocked(tok);
    }

    std::string declineLocked(const std::string& tok)
    {
        if (roleOfLocked(tok) == "viewer") return "NOT YOUR SEAT";
        if (!draw_offer_.empty() && draw_offer_ != tok) draw_offer_.clear();
        return statusLineLocked(tok);
    }

    std::string roleOfLocked(const std::string& tok) const
    {
        if (!tok.empty() && tok == white_) return "white";
        if (!tok.empty() && tok == black_) return "black";
        return "viewer";
    }

    // side, state, this token's role, each seat, then the draw offer -- one
    // request tells a client everything it needs to render its own controls,
    // rather than inferring role from a separate call that could disagree.
    std::string statusLineLocked(const std::string& tok) const
    {
        std::string s = board.isWhiteTurn() ? "w" : "b";
        // An agreed outcome outranks the position: after a resignation the board
        // may still read "check", which is true and irrelevant.
        if      (!over_.empty())            s += " " + over_;
        else if (checkmate_)                s += " checkmate";
        else if (stalemate_)                s += " stalemate";
        else if (board.sideToMoveInCheck()) s += " check";
        else                                s += " ok";
        s += " " + roleOfLocked(tok);
        s += white_.empty() ? " open" : " taken";
        s += black_.empty() ? " open" : " taken";
        // Sixth field: the draw offer RELATIVE to whoever is asking, so the
        // client needs no token comparison of its own.
        if      (draw_offer_.empty())  s += " none";
        else if (draw_offer_ == tok)   s += " mine";
        else                           s += " theirs";
        return s;
    }

    // A bounded ring, deliberately trivial: chat is presence, not history, and
    // anything durable belongs in a database provider rather than in the game's
    // own arena footprint.
    void sayLocked(const std::string& tok, const std::string& text)
    {
        if (text.empty()) return;
        chat_.push_back(roleOfLocked(tok) + ": " + text);
        if (chat_.size() > kChatLines) chat_.erase(chat_.begin());
    }

    std::string chatLogLocked() const
    {
        std::string out;
        for (const auto& line : chat_) { out += line; out += "\n"; }
        return out;
    }

    // HTTP gives no disconnect signal, so presence is inferred from traffic:
    // the page polls, and every poll is a heartbeat.
    void touchLocked(const std::string& tok)
    {
        if (tok.empty()) return;
        seen_[tok] = Clock::now();
    }

    // Runs per request rather than on a timer -- a game nobody is polling has
    // nobody to notice it, so there is nothing to do until someone shows up.
    void reapLocked(int grace_seconds)
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

        // Every observer must be STALE, not merely present. The earlier
        // condition asked only whether anyone had EVER been seen -- true the
        // instant the current request calls touchLocked, so a fresh game with a
        // live poller reset itself on every single request (visible in the log
        // as "abandoned before first move" once per connection).
        const bool any_live = (liveTokensLocked(grace_seconds) > 0);

        // A game WITH moves is kept: the position is worth more than the seat,
        // and whoever returns with the same token reclaims their side through
        // the ordinary empty-seat path above.
        if (!started_ && white_.empty() && black_.empty() && !seen_.empty() && !any_live)
        {
            ETCS_LOG("ChessGame", "abandoned before first move -- resetting.");
            resetLocked();
        }
    }

    // The verb surface. The NODE parses paths and calls this; the game itself
    // no longer knows what a URL looks like, which is the separation that lets
    // the same board be driven by an HTTP route today and by a replayed move
    // stream from a peer later. One entry point, so both can never diverge.
    std::string verbLocked(const std::string& tok, const std::string& verb,
                           const std::string& arg)
    {
        touchLocked(tok);
        reapLocked(30);

        if (verb == "move")    return applyMoveLocked(arg, tok);
        if (verb == "fen" || verb.empty()) return fenLocked();
        if (verb == "status")  return statusLineLocked(tok);
        if (verb == "say")     { sayLocked(tok, arg); return chatLogLocked(); }
        if (verb == "chat")    return chatLogLocked();
        if (verb == "resign")  return resignLocked(tok);
        if (verb == "draw")    return drawLocked(tok);
        if (verb == "decline") return declineLocked(tok);
        if (verb == "reset")   { resetLocked(); return fenLocked(); }
        return "NOT FOUND";
    }

    // Standalone routing, for a board spawned with no node in front of it
    // (chess_server.etcs). Path shape is the older key-first one, since without
    // a node there is no self segment to lead with.
    std::string requestLocked(const std::string& path)
    {
        std::vector<std::string> seg;
        splitPath(path, seg);

        size_t k = seg.size();
        for (size_t i = 0; i < seg.size(); ++i)
            if (seg[i] == match_key_) { k = i; break; }
        if (k == seg.size()) return "NOT FOUND";

        return verbLocked((k + 1 < seg.size()) ? seg[k + 1] : "",
                          (k + 2 < seg.size()) ? seg[k + 2] : "",
                          (k + 3 < seg.size()) ? seg[k + 3] : "");
    }

    // Recompute the terminal flags. Called ONLY where the answer can change --
    // construction, reset, a move landing, a resignation, a draw, a FEN load --
    // which is what keeps IsActiveConcrete const without making the Board
    // mutable.
    void refreshTerminal()
    {
        checkmate_ = board.isCheckmate();
        stalemate_ = board.isStalemate();
        active_    = over_.empty() && !(checkmate_ || stalemate_);
    }

    ::Board     board;                       // the engine, by value, private
    std::string match_key_;                  // equivalence class ("" = unclaimed)
    ChessStatus last = ChessStatus::SUCCESS; // last move result (PROMOTE follow-up)

    std::string white_, black_;              // seat holders, by token ("" = open)

    // Agreed outcomes, which the BOARD cannot express: "" | resign-white |
    // resign-black | draw. Kept separate from checkmate_/stalemate_ because
    // those are facts about the position and these are facts about the players.
    std::string over_;
    std::string draw_offer_;                 // token of the offerer ("" = none)

    bool        started_   = false;          // has any legal move landed
    bool        recorded_  = false;          // outcome already counted
    bool        checkmate_ = false;
    bool        stalemate_ = false;
    bool        active_    = true;

    // Set by ChessNode::createGameLocked. Null for a standalone board (the
    // single fixed game chess_server.etcs spawns), which simply keeps no
    // records -- there is no self to keep them on.
    ChessNode* node_ = nullptr;

    std::unordered_map<std::string, Clock::time_point> seen_;  // token -> heartbeat
    std::vector<std::string> chat_;
};

#endif // CHESSGAME_H__