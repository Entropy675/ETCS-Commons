#ifndef SOCKETCONNECTIONSTATE_H__
#define SOCKETCONNECTIONSTATE_H__
#include "../../../ontology.h"
#include "PicoHTTPParser.h"
#include <array>
#include <cstring>
#include <unistd.h>
// SocketConnectionState — concrete ConnectionState_ leaf for NetworkProvider.
// Spawned exclusively via Entity::addTag<SocketConnectionState>() from a
// ConnectionManager's own accept completion (see ConnectionManager.h) — never
// top-level `spawn`ed on its own, though it IS independently listable/
// addressable once it exists (`list NetworkProvider::SocketConnectionState`),
// since it's registered as a full tag block like any other type.
//
// Its parent is the ConnectionManager that accepted it, which is what makes
// the whole set reachable by strong reference: a subscriber handed this
// connection's RID resolves the manager, then getTypedChild's its way back in.
// The RID is a weak reference crossing OUT of the manager's causal domain;
// everything inside that domain is parent/child.
//
// PARSER OWNERSHIP NOTE: each connection owns its own PicoHTTPParser rather
// than sharing one mutable parse-state instance. This used to be phrased as
// "rather than sharing the parent HTTPParser's" — no longer true in either
// direction: HTTPParser is now purely a Parser_ leaf with no server role, and
// this type's parent is a ConnectionManager, which owns no parser at all.
//
// ARENA OWNERSHIP NOTE: every genuinely large piece of this connection's
// state (recv_buf_, send_buf_, and parser_'s own accum_) is allocated
// from THIS entity's own getArena() — never left as an inline member of
// the struct itself. addTag<SocketConnectionState>() bump-allocates the
// OUTER SHELL of this object (now small — a handful of pointers and
// scalars) into the CALLING entity's arena (HTTPParser's own), same as
// any addTag<T> child; if the large buffers were ALSO inline members,
// they'd ride along as part of that same outer-shell allocation, into
// HTTPParser's arena, where a bump allocator can never individually
// reclaim them — DeleteConcrete()'s own delete_children=true only
// reclaims THIS entity's own local_arena_, never bytes that were bump-
// allocated directly into its PARENT's. Routing the large buffers
// through getArena() instead means they live in the one arena that
// genuinely IS torn down, in full, the instant this specific connection
// is deleted — not accumulating in HTTPParser's arena for its entire
// running life regardless of connection churn.
//
// parser_ specifically needs its own arena bracketed via
// Entity::setPendingParentArena (see the guard pair immediately
// surrounding its own declaration below) so its OWN local_arena_ becomes
// a genuine CHILD of THIS entity's own arena, rather than an orphan of
// the global singleton. This used to be handled differently -- construct
// parser_ as an ordinary orphan, then explicitly reclaim its arena early
// via an evokeDestructor() call inside ~SocketConnectionState()'s own
// body. That was unsafe: C++ guarantees a destructor's own body always
// finishes before any member's own implicit destruction begins, so
// tearing down parser_'s own backing arena from THIS destructor's own
// body always ran too early -- before parser_'s own ~Entity() had a
// chance to safely destruct its own tags/flags_/interface_pointers_,
// which live in that same arena. A real, reproduced SIGSEGV this session
// traced to exactly that ordering (a use-after-free reading
// interface_pointers_'s own node storage from pages already unmapped by
// the explicit reclaim one call earlier). The redirect fixes this
// structurally: parser_'s own local_arena_ now gets torn down
// automatically, safely, as part of THIS entity's own normal arena
// teardown (evokeDestructor(&e->getArena()), called by registerDtor<T>'s
// own lambda strictly AFTER ~SocketConnectionState() -- including
// parser_'s own implicit destruction -- has already fully completed).
// LIFETIME: Ephemeral_ only, deliberately NOT Deletable_. A connection has no
// lifetime independent of the manager that accepted it -- nothing else can
// reach one, and it cannot outlive its parent. Claiming Deletable_ forced a
// DestroyEvent (loader ordering thread) per connection on both accept and
// teardown; dropping it makes the manager's pool the lifetime authority and
// Reset the return path.
class SocketConnectionState : 
    public ConnectionStateBase<SocketConnectionState>, public EphemeralBase<SocketConnectionState>
{
private:
    // Free / Serving / Draining, and the third is load-bearing. Reset is
    // ASYNCHRONOUS: it cancels and closes, but outstanding io_uring
    // submissions still have to retire before this object can be handed to a
    // new client. Draining is "not reusable yet, not serving anyone" -- with
    // only two states a reused connection could receive a completion belonging
    // to the previous client, which is a use-after-free against a live object
    // and so crashes nothing while corrupting both requests.
    //
    // Because the drain waits for io_inflight_ to reach zero before publishing
    // Free, no stale completion can exist at reacquire time -- which is why
    // there is no generation counter here.
    enum class Phase : uint8_t { Free, Serving, Draining };
    std::atomic<Phase> phase_{Phase::Free};
    std::atomic<int>   io_inflight_{0};

    // Manager's in-use count. Decremented by finalizeIfDraining, which is the
    // only place a connection becomes reusable. Raw pointer rather than a back
    // reference to the manager: this type has no business knowing what a
    // ConnectionManager is.
    std::atomic<int>*  pool_counter_ = nullptr;
    std::chrono::steady_clock::time_point last_activity_ = std::chrono::steady_clock::now();
    static constexpr int TIMEOUT_SECONDS = 10;

    // Capacities kept as named constants (not sizeof(recv_buf_) etc,
    // which would now just be sizeof(char*) = 8) — same fix PicoHTTPParser's
    // own accum_ needed for the identical reason.
    static constexpr size_t kRecvBufSize = ETCS_NETWORK_MAX_HEADER_SIZE;
    static constexpr size_t kSendBufSize = ETCS_NETWORK_MAX_HEADER_SIZE * 4;

    // MutableByteSpan — trivial non-owning (pointer, length) pair, returned
    // BY VALUE from RecvBuffer()/SendBuffer() below. Exists purely so every
    // existing call site (conn->RecvBuffer().data(), conn->RecvBuffer().size())
    // keeps compiling and behaving identically after recv_buf_/send_buf_
    // stopped being std::array members with their own .data()/.size() --
    // a raw char* alone has neither. Deliberately NOT std::span (this
    // codebase's own build flags aren't confirmed C++20, and nothing else
    // here uses it) -- a two-line struct is simpler than adding that
    // dependency for one call site's worth of convenience.
    struct MutableByteSpan
    {
        char*  ptr;
        size_t len;
        char*  data() const { return ptr; }
        size_t size() const { return len; }
    };

public:
    WIRE_TYPE_IDENTITY(SocketConnectionState);

    SocketConnectionState()
    {
        recv_buf_ = static_cast<char*>(getArena().allocateRaw(static_cast<long long>(kRecvBufSize)));
        send_buf_ = static_cast<char*>(getArena().allocateRaw(static_cast<long long>(kSendBufSize)));
        std::memset(recv_buf_, 0, kRecvBufSize);
        std::memset(send_buf_, 0, kSendBufSize);

        // Replace parser_'s own default self-allocated accum_ buffer
        // with one drawn from THIS entity's own arena directly -- still
        // correct and safe without this (parser_'s own local_arena_ is
        // now a genuine child, not an orphan -- see the guard pair
        // around parser_'s own declaration below), just one fewer
        // nested-arena hop for this specific, large, hot-path buffer.
        char* accum_buf = static_cast<char*>(
            getArena().allocateRaw(static_cast<long long>(PicoHTTPParser::kAccumCapacity)));
        parser_.BindAccumBuffer(accum_buf);

        parser_.SetMode(PicoHTTPParser::Mode::Request);
    }
    virtual ~SocketConnectionState()
    {
        CloseConnection();

        // parser_'s own local_arena_ no longer needs (or safely CAN
        // have) an explicit reclaim here -- see this class's own
        // file-level comment for the full reasoning. It's a genuine
        // child of this entity's own arena now, torn down automatically,
        // safely, once this destructor (including parser_'s own
        // implicit destruction, which runs after this body returns) has
        // fully completed.
    }
    void markActive() { last_activity_ = std::chrono::steady_clock::now(); }
    bool checkTimeout() 
    {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_activity_).count() > TIMEOUT_SECONDS)
        {
            ETCS_LOG("Timeout reached, recycling RID: " << getRID());
            return ResetConcrete();
        }
        return false;
    }

    // --- Pool surface ---

    // Free -> Serving, atomically. Two accept completions can run on different
    // pool workers at once (submitAccept re-arms BEFORE onConnection), so the
    // claim has to be a CAS rather than a check-then-set.
    bool TryClaim(int fd)
    {
        Phase expect = Phase::Free;
        if (!phase_.compare_exchange_strong(expect, Phase::Serving,
                                            std::memory_order_acq_rel)) return false;
        SetClientFdConcrete(fd);
        markActive();
        return true;
    }

    void SetPoolCounter(std::atomic<int>* c) { pool_counter_ = c; }

    // Bracket every IOSubmission that names this connection's fd. NoteComplete
    // must be the LAST thing a completion callback does: it can publish this
    // object as reusable, and anything touching it afterwards is racing a new
    // client's request.
    void NoteSubmit() { io_inflight_.fetch_add(1, std::memory_order_acq_rel); }
    void NoteComplete()
    {
        if (io_inflight_.fetch_sub(1, std::memory_order_acq_rel) == 1)
            finalizeIfDraining();
    }

    // --- ConnectionState_ / EphemeralBase concrete surface ---

    // Begins the drain; does NOT complete it. Returns true once the transition
    // is under way, idempotent on an already-draining or free connection.
    // IsActive() stays true until the last completion retires.
    bool ResetConcrete()
    {
        Phase expect = Phase::Serving;
        if (!phase_.compare_exchange_strong(expect, Phase::Draining,
                                            std::memory_order_acq_rel))
            return true;

        // Cancel surfaces pending recv/send as -ECANCELED so their callbacks
        // run promptly instead of the drain waiting on a peer that may never
        // send again. Skipped during arena teardown for the same reason
        // ConnectionManager::CloseConcrete skips it -- constructing an
        // IOSubmission allocates, and allocation after teardown throws through
        // a destructor into std::terminate.
        //
        // NO CLOSE HERE. io_uring_prep_cancel_fd resolves the fd when the
        // KERNEL processes the SQE, not when we submit it. Closing now returns
        // the number to the kernel immediately, the next accept is handed the
        // same fd, Serve submits a recv on it, and the cancel then lands on the
        // NEW connection's recv. The close moves to finalizeIfDraining, which
        // already waits for io_inflight_ to reach zero -- so the number is only
        // released once nothing names it.
        if (io_inflight_.load(std::memory_order_acquire) > 0
            && client_fd_ != -1
            && !ETCS::MemoryArena::getInstance().isTearingDown())
        {
            ETCS::IOSubmission cancel;
            cancel.op = ETCS::IOOp::Cancel;
            cancel.fd = client_fd_;
            ETCS::ThreadPool::getInstance().submit(std::move(cancel));
        }

        open_ = false;   // stop do_recv re-arming; the fd itself closes at finalize
        finalizeIfDraining();
        return true;
    }

    // Serving OR Draining. A draining connection is not reusable, so the pool
    // must not see it as free -- this is the distinction IsConnectionOpen()
    // (fd validity) deliberately does not make.
    bool IsActiveConcrete() const
    { return phase_.load(std::memory_order_acquire) != Phase::Free; }
    int  GetClientFdConcrete() const { return client_fd_; }
    void SetClientFdConcrete(int fd) { client_fd_ = fd; open_ = (fd >= 0); }
    ETCS::RID GetPageRIDConcrete() const       { return page_rid_; }
    void       SetPageRIDConcrete(ETCS::RID r) { page_rid_ = r; }
    bool IsConnectionOpenConcrete() const { return open_; }
    // --- Implementation surface used by NetworkProvider.h's consumer ---
    // Additional public methods beyond the ConnectionState_ ontology
    // contract — same pattern MbedTLSContext uses for ParseConcrete/
    // loadCACert beyond its own ontology surface.
    void CloseConnection()
    {
        if (client_fd_ != -1) { ::close(client_fd_); client_fd_ = -1; }
        open_ = false;
    }
    PicoHTTPParser& GetParser() { return parser_; }
    MutableByteSpan RecvBuffer() { return MutableByteSpan{recv_buf_, kRecvBufSize}; }
    MutableByteSpan SendBuffer() { return MutableByteSpan{send_buf_, kSendBufSize}; }
    int  GetSendLen() const  { return send_len_; }
    void SetSendLen(int len) { send_len_ = len; }
    void SetRecvLen(int len) { recv_len_ = len; }
private:
    // Exactly once, by CAS. Reset and the last NoteComplete both call this and
    // can race.
    void finalizeIfDraining()
    {
        if (io_inflight_.load(std::memory_order_acquire) != 0) return;

        Phase expect = Phase::Draining;
        if (!phase_.compare_exchange_strong(expect, Phase::Free,
                                            std::memory_order_acq_rel)) return;

        // Last possible moment: no submission names this fd any more.
        CloseConnection();

        page_rid_ = 0;
        parser_.ResetConcrete();
        // Only the bytes actually written. A blind memset of both buffers is
        // 40KB per request on a path otherwise bounded by syscalls; recv is
        // bounded by recv_len_ and send by send_len_, so nothing beyond those
        // is ever read.
        if (recv_len_ > 0) std::memset(recv_buf_, 0, static_cast<size_t>(recv_len_));
        if (send_len_ > 0) std::memset(send_buf_, 0, static_cast<size_t>(send_len_));
        recv_len_ = 0;
        send_len_ = 0;

        if (pool_counter_) pool_counter_->fetch_sub(1, std::memory_order_acq_rel);
    }

    int         client_fd_ = -1;
    bool        open_      = false;
    ETCS::RID   page_rid_  = 0;
    int         send_len_  = 0;
    int         recv_len_  = 0;

    // Brackets parser_'s own construction immediately below -- see this
    // class's own file-level comment for the full reasoning. C++
    // constructs members in DECLARATION order regardless of
    // initializer-list order, so this pair's own placement (immediately
    // before and after parser_) is what actually brackets its
    // construction, not merely documents an intent. _set's own
    // constructor runs first (Entity's own base subobject is already
    // fully constructed by the time ANY member begins, so getArena() is
    // already valid here), redirecting Entity()'s own
    // s_pending_parent_arena_ check to THIS entity's own arena instead
    // of the global singleton; _restore's own constructor runs
    // immediately after parser_'s own construction finishes, putting it
    // back to whatever it was before -- same save/restore discipline
    // addTag<T> itself already uses (Entity.h), just bracketing a
    // composed MEMBER's construction instead of a typed CHILD's.
    struct ArenaRedirectSetGuard
    {
        ETCS::MemoryArena* saved;
        explicit ArenaRedirectSetGuard(ETCS::MemoryArena* target)
            : saved(ETCS::Entity::setPendingParentArena(target)) {}
    } _arena_redirect_set_{ &getArena() };

    PicoHTTPParser parser_;

    struct ArenaRedirectRestoreGuard
    {
        explicit ArenaRedirectRestoreGuard(ETCS::MemoryArena* restore)
        { ETCS::Entity::setPendingParentArena(restore); }
    } _arena_redirect_restore_{ _arena_redirect_set_.saved };

    char* recv_buf_ = nullptr;
    char* send_buf_ = nullptr;
};
#endif // SOCKETCONNECTIONSTATE_H__
