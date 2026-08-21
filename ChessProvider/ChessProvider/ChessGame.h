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
                                Key, Reset, IsActive, History, Leave,
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

    ChessGame()  { board.setStartingBoard(true); refreshTerminal(); recordHistoryLocked(""); }
    virtual ~ChessGame() = default;

    // ── Public surface: every one of these serializes ─────────────────────
    std::string Request(const std::string& path)      { return op(Kind::Request, path); }
    std::string ApplyMove(const std::string& mv,
                          const std::string& tok = "") { return op(Kind::Move, mv, tok); }
    std::string LoadFenStr(const std::string& fen)    { return op(Kind::LoadFen, fen); }
    std::string Fen() const                            { return op(Kind::Fen); }
    std::string StatusLine(const std::string& t = "") const { return op(Kind::Status, "", t); }
    std::string ChatLog() const                        { return op(Kind::Chat); }
    std::string History(const std::string& from = "0") { return op(Kind::History, from); }
    std::string Leave(const std::string& tok)          { return op(Kind::Leave, "", tok); }
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

    // Decode one path segment after splitPath. Browsers encode content-bearing
    // args with encodeURIComponent; without this, spaces arrive as literal %20.
    static std::string percentDecode(const std::string& in)
    {
        std::string out;
        out.reserve(in.size());
        auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return -1;
        };
        for (size_t i = 0; i < in.size(); ++i)
        {
            if (in[i] == '%' && i + 2 < in.size())
            {
                const int hi = hex(in[i + 1]), lo = hex(in[i + 2]);
                if (hi >= 0 && lo >= 0)
                {
                    out.push_back(static_cast<char>((hi << 4) | lo));
                    i += 2;
                    continue;
                }
            }
            out.push_back(in[i]);
        }
        return out;
    }

private:
    // Raised from 40 because the log now carries narration as well as speech:
    // arrivals, seat claims, every move, and the outcome. A 40-line ring was
    // scrolled clean by roughly twenty plies, which threw away the conversation
    // to make room for the move list -- the opposite of what either is for.
    static constexpr size_t kChatLines    = 200;

    // ── The frame budget ──────────────────────────────────────────────────
    // A route's ENTIRE reply is one ETCS::Buffer: HttpServer::DispatchRoute
    // declares `ETCS::Buffer payload`, calls the work function with it, and
    // assigns it straight to io. So a reply has MAX_TAG_BUFFER_SIZE to live in,
    // and what happens on overflow is worse than truncation:
    //
    //     bool writeString(const char* str)
    //     {
    //         reset();                                   // <-- clears FIRST
    //         if (strlen(str) + 1 > bufsize) return false;  // <-- leaves it EMPTY
    //
    // The buffer is reset before the capacity check, so an oversized reply does
    // not arrive clipped -- it arrives as nothing at all. That is the whole bug
    // report: the chat worked until the pane filled and then every line
    // vanished at once, because the pane filling and the log crossing bufsize
    // are the same event, and the crossing blanks the response rather than
    // shortening it. (It is logged, so the server's own log names it.)
    //
    // Fixed-size verbs were never at risk -- a FEN is 56 bytes, a status line
    // 40 -- which is why this only surfaced once chat started carrying
    // narration and grew several times faster.
    //
    // Widening the buffer is possible (TBuffer<N> takes any N, and NBuffer is
    // already 8K) but it is not the fix, for two reasons: the work function is
    // handed an ETCS::Buffer& by HttpServer, so this module cannot choose the
    // width on its own; and a log with no upper bound reaches any width
    // eventually, so a wider buffer only moves the cliff. Paging removes it.
    //
    // The growing verbs therefore answer in PAGES: a "<base> <next>" header and
    // then as many whole lines as fit. 200 rather than a number derived from
    // bufsize because this module should not encode a constant it does not own
    // -- a build with a bigger Buffer simply gets the same correct answer in
    // the same number of requests, and one with a smaller Buffer is the only
    // case that would need this lowered.
    static constexpr size_t kFrameBudget = 200;

    // Lines dropped off the front of each ring. A page is addressed by ABSOLUTE
    // index, so a client that was reading at 40 can tell the difference between
    // "nothing new" and "the 40 you had are gone" -- without which a full ring
    // silently renumbers under the reader and it re-appends lines it already
    // has.
    size_t chat_base_ = 0, history_base_ = 0;

    // "<base> <next>\n" then src[from - base ...], stopping before the budget.
    // Whole lines only: half a line is not a thing any reader here can use.
    std::string pageLocked(const std::vector<std::string>& src,
                           size_t base, size_t from) const
    {
        if (from < base) from = base;               // caller fell behind the ring
        size_t i = (from - base < src.size()) ? (from - base) : src.size();
        std::string body;
        while (i < src.size() && body.size() + src[i].size() + 1 <= kFrameBudget)
        {
            body += src[i];
            body += "\n";
            ++i;
        }
        // A single line longer than the whole budget would otherwise never be
        // sent and the reader would stall on it forever, re-requesting the same
        // index. Ship it clipped: losing the tail of one over-long line beats
        // losing every line after it.
        if (body.empty() && i < src.size())
        {
            body = src[i].substr(0, kFrameBudget);
            body += "\n";
            ++i;
        }
        return std::to_string(base) + " " + std::to_string(base + i) + "\n" + body;
    }

    // A FEN per ply. Bounded for the same reason chat is: a game is presence,
    // not an archive, and anything durable belongs in a database provider. The
    // cap drops the OLDEST plies, so a very long game loses its opening rather
    // than its recent moves -- which is the half a review pane is actually
    // used for. 600 plies is past the longest recorded tournament game.
    static constexpr size_t kHistoryPlies = 600;

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

    // ── History ───────────────────────────────────────────────────────────
    // One line per ply: "<uci> <fen>", oldest first, index 0 being the position
    // the game STARTED from and carrying "-" for its move. Keeping the root in
    // the same list rather than beside it means a client walking backwards
    // never needs to know what a starting position looks like -- which matters
    // because a game may have been seeded with loadFen and not start from one.
    //
    // Recorded server-side rather than left to the client because a spectator
    // who arrives at move thirty has no snapshots of their own, and neither
    // does a player who reloaded. The client still keeps its own snapshots as
    // a fast path; this is what makes them recoverable.
    void recordHistoryLocked(const std::string& uci)
    {
        history_.emplace_back((uci.empty() ? "-" : uci) + " " + board.toFENString());
        // Dropping the front discards the root line with it, so past the cap the
        // first entry is an ordinary mid-game ply. Harmless: the client renders
        // positions, it does not reconstruct them from the root.
        if (history_.size() > kHistoryPlies) { history_.erase(history_.begin()); ++history_base_; }
    }

    // Paged. A ply line is a uci plus a FEN, about 75 bytes, so a whole game
    // has never fitted in one reply and never will -- this verb was born
    // needing the paging that chat only grew into.
    std::string historyPageLocked(size_t from) const
    { return pageLocked(history_, history_base_, from); }

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
        // The chat SURVIVES a reset while the history does not, and the
        // difference is deliberate: the position is a new game, so replaying the
        // old one under it would be a lie, but the people are the same people
        // and their conversation did not end. The log line below is what joins
        // the two halves.
        history_.clear();
        history_base_ = 0;
        recordHistoryLocked("");
        logLocked("new game -- seats are open");
        return true;
    }

    std::string loadFenLocked(const std::string& fen)
    {
        if (!board.loadFEN(fen)) return "INVALID";
        refreshTerminal();
        // A loaded position is a new root, not a continuation: the plies before
        // it never happened on this board and stepping back into them would
        // show a line that does not lead here.
        history_.clear();
        history_base_ = 0;
        recordHistoryLocked("");
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

        // Hoisted out of the token branch below: the narration after the move
        // lands needs to know which SIDE moved, and by then the board has
        // already flipped the turn. Reading it back from the board afterwards
        // would report the opponent.
        const bool white_moved = board.isWhiteTurn();

        if (!tok.empty())
        {
            const bool white_to_move = white_moved;
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
                // Announced HERE, before the move is validated, because the
                // claim has already happened -- seat is assigned on the line
                // above and is not rolled back if movePiece refuses. Deferring
                // the line until the move is known legal would leave a seat
                // held by someone the log never named.
                logLocked(tok + " sits as " + (white_to_move ? "white" : "black"));
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

        const bool first_move = !started_;
        started_ = true;
        // Moving answers a pending offer: playing on IS declining, the ordinary
        // convention, and it saves the offerer waiting for a reply already given.
        draw_offer_.clear();
        refreshTerminal();
        recordHistoryLocked(mv);

        if (first_move) logLocked("game started");
        // Side, not self: a move is made by white, and which self holds white is
        // already in the seat line above. Suffixed the way a scoresheet is, so
        // the log reads as a game rather than as a request trace.
        logLocked(std::string(white_moved ? "white" : "black") + " plays " + mv
                  + (checkmate_ ? "#" : (board.sideToMoveInCheck() ? "+" : "")));

        if (!active_)
        {
            if      (checkmate_) logLocked(std::string(white_moved ? "white" : "black") + " wins by checkmate");
            else if (stalemate_) logLocked("stalemate -- drawn");
            reportOutcomeLocked();
        }
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
        logLocked(role + " resigns -- " + (role == "white" ? "black" : "white") + " wins");
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
            logLocked(role + " accepts -- drawn by agreement");
            refreshTerminal();
            reportOutcomeLocked();
            return statusLineLocked(tok);
        }
        // Re-offering your own standing offer is a no-op above the log too:
        // without this the log gains a line every time an impatient player
        // clicks the button again.
        if (draw_offer_ != tok) logLocked(role + " offers a draw");
        draw_offer_ = tok;
        ETCS_LOG("ChessGame", "draw offered by " << role);
        return statusLineLocked(tok);
    }

    std::string declineLocked(const std::string& tok)
    {
        if (roleOfLocked(tok) == "viewer") return "NOT YOUR SEAT";
        if (!draw_offer_.empty() && draw_offer_ != tok)
        {
            draw_offer_.clear();
            logLocked(roleOfLocked(tok) + " declines the draw");
        }
        return statusLineLocked(tok);
    }

    // ── Explicit departure ────────────────────────────────────────────────
    // HTTP gives no disconnect signal, which is why presence is inferred from
    // traffic at all -- but "no signal" is not the same as "no statement". A
    // client that KNOWS it is leaving can say so, and this is the verb it says
    // it with: the seat is released now instead of in thirty seconds.
    //
    // The reaper is not replaced by this, it is demoted to the fallback it
    // should always have been. A crashed tab, a closed laptop and a dropped
    // network still say nothing, and those cases are exactly what the grace
    // period exists for. This one closes the case that was ALWAYS reportable
    // and was being handled as though it were not: the sole occupant of a
    // two-player game clicking back to the lobby, after which the room sat
    // half-claimed for half a minute and the next arrival was told the seat was
    // taken by someone who had already gone.
    //
    // Idempotent and unauthenticated in the same sense every other verb here
    // is: the token IS the identity, so a leave can only release the seat that
    // token holds. There is nothing to spoof that moving as that token could
    // not already do.
    std::string leaveLocked(const std::string& tok)
    {
        if (tok.empty()) return "OK";

        // Erase the heartbeat FIRST, so liveTokensLocked stops counting this
        // token immediately -- the room list is what the next arrival reads to
        // decide whether to join or watch, and it must not report a ghost.
        const bool was_here = (seen_.erase(tok) > 0);
        if (draw_offer_ == tok) draw_offer_.clear();

        // Mutually exclusive by the claim rule in applyMoveLocked: one token
        // can never hold both seats.
        if (white_ == tok)
        {
            white_.clear();
            ETCS_LOG("ChessGame", "white seat released (left): " << tok);
            logLocked(tok + " left -- white seat is open");
        }
        else if (black_ == tok)
        {
            black_.clear();
            ETCS_LOG("ChessGame", "black seat released (left): " << tok);
            logLocked(tok + " left -- black seat is open");
        }
        else if (was_here) logLocked(tok + " left");

        return "OK";
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

    // No exceptions on a bad cursor: the argument comes off a URL, so garbage
    // is an ordinary input and "start from the beginning" is a safe reading of
    // it. stoul would throw straight through the ordering thread.
    static size_t parseIndex(const std::string& s)
    {
        size_t n = 0;
        for (char c : s)
        {
            if (c < '0' || c > '9') return 0;
            n = n * 10 + static_cast<size_t>(c - '0');
            if (n > 100000000u) return 0;      // nonsense, not a cursor
        }
        return n;
    }

    // A bounded ring, deliberately trivial: chat is presence, not history, and
    // anything durable belongs in a database provider rather than in the game's
    // own arena footprint.
    void sayLocked(const std::string& tok, const std::string& text)
    {
        const std::string msg = percentDecode(text);
        if (msg.empty()) return;
        // Author is the self that spoke, not the seat colour. Role belongs in
        // /status; chat is who said what.
        const std::string who = tok.empty() ? "viewer" : tok;
        chat_.push_back(who + ": " + msg);
        if (chat_.size() > kChatLines) chat_.erase(chat_.begin());
    }

    // ── Narration ─────────────────────────────────────────────────────────
    // Events go into the SAME ring as speech rather than into a second channel.
    // One channel because the two are read together -- "bob sits as black" is
    // only useful next to what bob then said -- and because a second endpoint
    // would be a second poll, a second merge, and a second thing that can be
    // one request out of date with the first.
    //
    // Marked with a leading "* " so a client can tell narration from speech
    // with no protocol change: a human's line is always "<self>: <text>", and
    // "* " is not a prefix any "<self>:" can produce, since the space cannot be
    // where the colon is. Worth stating because the alternative -- trusting an
    // unforgeable author field -- does not exist here: the self IS client
    // supplied.
    void logLocked(const std::string& text)
    {
        chat_.push_back("* " + text);
        if (chat_.size() > kChatLines) { chat_.erase(chat_.begin()); ++chat_base_; }
    }

    std::string chatPageLocked(size_t from) const
    { return pageLocked(chat_, chat_base_, from); }

    // What a caller with no cursor gets: the TAIL, not the whole log. Used by
    // the shell's Chat verb and by any client that has not been taught to page.
    // The tail rather than the head because the last thing said is the thing
    // worth seeing, and because "the whole log" is the answer that blanks the
    // buffer -- there is no size of chat for which returning all of it is
    // correct, so the no-argument form does not offer it.
    std::string chatLogLocked() const
    {
        size_t from = chat_base_;
        std::string out = pageLocked(chat_, chat_base_, from);
        // Walk forward until the page reaching the end is the one returned.
        while (true)
        {
            size_t next = from + 1;
            if (next >= chat_base_ + chat_.size()) break;
            std::string cand = pageLocked(chat_, chat_base_, next);
            // Stop as soon as advancing no longer reaches further: the last
            // page that still ends at the end of the ring is the tail.
            if (cand.size() < out.size() && next + 1 >= chat_base_ + chat_.size()) { out = cand; break; }
            out = cand;
            from = next;
        }
        return out;
    }

    // HTTP gives no disconnect signal, so presence is inferred from traffic:
    // the page polls, and every poll is a heartbeat.
    void touchLocked(const std::string& tok)
    {
        if (tok.empty()) return;
        // First heartbeat from this token is an arrival. Detected here rather
        // than at the edge join in ChessNode because the edge is added by
        // VISITING a match, including from the lobby's own listing, and
        // announcing an arrival for someone who merely has the game in their
        // list would be narration of something that did not happen.
        if (seen_.find(tok) == seen_.end()) logLocked(tok + " is here");
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

        if (gone(white_)) { ETCS_LOG("ChessGame", "white seat released (idle): " << white_);
                            logLocked(white_ + " timed out -- white seat is open"); white_.clear(); }
        if (gone(black_)) { ETCS_LOG("ChessGame", "black seat released (idle): " << black_);
                            logLocked(black_ + " timed out -- black seat is open"); black_.clear(); }

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
        // leave is handled BEFORE the heartbeat, and has to be: touchLocked
        // would re-register the very presence this verb exists to withdraw, so
        // a leave arriving through the ordinary path would announce a departure
        // and then immediately contradict it. reapLocked still runs after, so a
        // departure that empties the room can trip the abandoned-before-first-
        // move reset in the same request rather than on the next visitor's.
        if (verb == "leave")   { const std::string r = leaveLocked(tok); reapLocked(30); return r; }

        touchLocked(tok);
        reapLocked(30);

        if (verb == "move")    return applyMoveLocked(arg, tok);
        if (verb == "fen" || verb.empty()) return fenLocked();
        if (verb == "status")  return statusLineLocked(tok);
        // say answers OK, not the log: the reply used to be the whole chat,
        // which is the single largest thing this server ever tried to return
        // and the most likely to blank. The speaker's own next poll shows them
        // their line a beat later, which is what every other client already
        // sees anyway.
        if (verb == "say")     { sayLocked(tok, arg); return "OK"; }
        if (verb == "chat")    return arg.empty() ? chatLogLocked()
                                                  : chatPageLocked(parseIndex(arg));
        if (verb == "history") return historyPageLocked(parseIndex(arg));
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

    // "<uci> <fen>" per ply, oldest first, root at index 0 with "-" for its
    // move. A vector rather than the deque the ring behaviour suggests: the cap
    // is hit by roughly no games at all, so the one erase(begin()) it would
    // save is not worth a second container shape in this file.
    std::vector<std::string> history_;
};

#endif // CHESSGAME_H__