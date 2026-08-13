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
class SocketConnectionState : 
    public ConnectionStateBase<SocketConnectionState>, public EphemeralBase<SocketConnectionState>,
    public DeletableBase<SocketConnectionState>
{
private:
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
            ETCS_LOG("Timeout reached, pruning RID: " << getRID());
            return DeleteConcrete(); // Fires the unload event
        }
        return false;
    }
    // --- ConnectionState_ / EphemeralBase concrete surface ---
    bool DeleteConcrete()
    {
        std::string conjugate_key = getSourceModule().toString() + ":" + getSourceTag().toString();
        ETCS_LOG("Delete: firing self-DestroyEvent for RID:"
                 << getRID() << " (" << conjugate_key << ")");
        return ETCS::DestroyEvent{conjugate_key.c_str(), this, true}();
    }
    
    bool ResetConcrete()
    {
        CloseConnection();
        page_rid_ = 0;
        send_len_ = 0;
        parser_.ResetConcrete();
        std::memset(recv_buf_, 0, kRecvBufSize);
        std::memset(send_buf_, 0, kSendBufSize);
        return true;
    }
    bool IsActiveConcrete() const { return open_; }
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
private:
    int         client_fd_ = -1;
    bool        open_      = false;
    ETCS::RID   page_rid_  = 0;
    int         send_len_  = 0;

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
