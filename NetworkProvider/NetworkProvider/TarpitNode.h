#ifndef TARPITNODE_H__
#define TARPITNODE_H__
#include "../../../ontology.h"
#include "ConnectionManager.h"
#include "SocketConnectionState.h"
#include "TLSConnectionIO.h"
#include "ConnRef.h"
#include "TarpitFilter.h"
#include "TarpitScheduler.h"
#include <cstdio>
#include <cstring>
#include <string>

// TarpitNode — a self-registering gate-level filter, exactly the same shape
// ChessNode/ChessGame already are for route-level filtering
// (ChessProvider/ChessProvider/ChessNode.h): a script spawns one, points it
// at a server via `web.add(HttpServer AddHandler) @tarpit Request @tarpit
// Filter`, and from that point on HttpServer/ConnectionManager know nothing
// about tarpits at all -- they only know Filter_ and a gate-level consumer,
// the same two things chess and forum have always used. No flag, no special
// case in the types that dispatch to it.
//
// WHY THIS TYPE EXISTS AT ALL, and why it could not simply be another
// AddRoute-level filter like chess's own: a route-level Filter_
// (HttpServer::DispatchRoute) only ever runs for a request HttpServer::Serve
// has ALREADY claimed, read, parsed and is about to answer -- so by the time
// a route filter gets a look, this connection's fate (some HttpServer,
// specifically) is already decided. A tarpit needs to intercept BEFORE that:
// it wants connections HttpServer never sees at all, held open and answered
// slowly instead of joining the normal request/response cycle. That is a
// GATE-level decision (which consumer gets this connection), not a
// route-level one (which handler answers this path) -- which is why this
// registers through ConnectionManager::RegisterConsumer, not
// HttpServer::AddRoute.
//
// The one real wrinkle: a gate-level Filter_ has never been able to see a
// PATH before, because ConnectionManager::askFilter has always run at
// accept time, before a single byte is read. See
// ConnectionManager::preParseThenDispatch (ConnectionManager.h) for the
// conditional pre-parse that exists solely so this type's AcceptsConcrete
// has a parsed request to look at -- gated behind "some filtered subscriber
// is actually registered", so a deployment with no tarpit pays nothing for
// this existing.
//
// AcceptsConcrete's probe is (manager_rid, conn_rid) -- askFilter's own
// unchanged format, the same one every OTHER gate-level Filter_ would get --
// not a path string. A route-level filter (ChessNode::AcceptsConcrete) gets
// handed the path directly because HttpServer::DispatchRoute's own probe
// format is different (it writes the path itself, per-request, because it
// already has one in hand). This type resolves its own path by walking
// (manager_rid, conn_rid) back to the live SocketConnectionState and reading
// GetParser() directly -- the parser is guaranteed populated by the time
// askFilter runs post-pre-parse, so there is nothing to wait on.
//
// ClaimAndDelay's own send loop (sendNow, below) is a DELIBERATE, COMPLETE
// DUPLICATE of HttpServer::Serve's do_send in NetworkProvider.h -- not a
// shared helper, not a refactor of one into the other. The two are not
// causally the same shape and can legitimately diverge: do_send exists to
// serve real responses as fast as possible and re-arms keep-alive; sendNow
// exists to serve exactly one 404, as slowly as delay_ms_ says, and always
// closes. A future change to either (a new field one needs that the other
// never will, a different failure policy) should not have to reason about
// the other.
class TarpitNode :
    public DeletableBase<TarpitNode>, public FilterBase<TarpitNode>
{
public:
    WIRE_TYPE_IDENTITY(TarpitNode);

    TarpitNode()          = default;
    virtual ~TarpitNode() = default;

    // --- Filter_ concrete surface ---
    //
    // io carries (manager_rid, conn_rid) IN -- see this class's own top
    // comment for why that is the probe format rather than a path -- and,
    // on acceptance, an equivalence key OUT: the matched path itself,
    // mirroring the empty-means-decline / non-empty-means-accept shape
    // every other Filter_ in this codebase already uses (Filter.h).
    // ConnectionManager itself never interprets this key; it exists purely
    // for the log line askFilter's own caller already writes.
    // LOGS EVERY OUTCOME, deliberately, unlike most Filter_ implementations.
    // A route-level filter that declines is one of several being polled and
    // its silence is readable against the "routed '<path>' -> RID:..." line
    // that follows. This one is the ONLY gate-level filter, it runs before
    // anything else has said a word about the connection, and a decline here
    // is indistinguishable in a log from "the filter was never consulted at
    // all" -- which is a completely different failure (a registration
    // pointing at the wrong entity, a connection that never reached
    // beginDispatch) needing a completely different fix. Three lines that
    // name which of those actually happened cost one log call on a path that
    // already does a substring scan over a pattern list.
    bool AcceptsConcrete(ETCS::Buffer& io) const
    {
        ETCS::RID manager_rid = 0, conn_rid = 0;
        io >> manager_rid;
        io >> conn_rid;

        SocketConnectionState* conn = resolveConnection(manager_rid, conn_rid);
        if (!conn)
        {
            ETCS_LOG("TarpitNode", "probe (manager RID:" << manager_rid << " conn RID:"
                     << conn_rid << ") resolved no live connection -- declining. The "
                        "gate and the connection are both supposed to still exist at "
                        "this point, so this is a boundary failure, not a miss.");
            io.reset();
            return false;
        }

        std::string path(conn->GetParser().GetPath(), conn->GetParser().GetPathLen());
        if (!tarpit_.Matches(path))
        {
            ETCS_LOG("TarpitNode", "no pattern matched '" << path << "' (against "
                     << tarpit_.PatternCount() << " pattern(s)) -- declining, this "
                        "connection goes to the ordinary handler.");
            io.reset();
            return false;
        }

        ETCS_LOG("TarpitNode", "MATCHED '" << path << "' -- claiming this connection; "
                 "it will be held " << delay_ms_ << "ms before its 404.");
        io.reset();
        io.writeString(path.c_str());
        return true;
    }

    // --- Deletable_ concrete surface ---
    bool DeleteConcrete()
    {
        std::string conjugate_key = getSourceModule().toString() + ":" + getSourceTag().toString();
        ETCS_LOG("TarpitNode", "Delete: firing self-DestroyEvent for RID:"
                 << getRID() << " (" << conjugate_key << ")");
        return ETCS::DestroyEvent{conjugate_key.c_str(), this}();
    }

    // --- Configuration surface ---
    void AddTarpitPattern(const std::string& p) { tarpit_.AddPattern(p); }
    void ClearTarpitPatterns()                   { tarpit_.ClearPatterns(); }
    void LoadDefaultTarpitPatterns()             { tarpit_.LoadDefaultPatterns(); }
    size_t TarpitPatternCount() const            { return tarpit_.PatternCount(); }

    // How long a matched connection is held before its 404 is sent.
    // Clamped well under SocketConnectionState::TIMEOUT_SECONDS: the whole
    // point is to waste a scanner's time WITHOUT the connection getting
    // reaped by ConnectionManager's own idle sweep first, which would just
    // free the slot early and defeat the purpose. -5s of headroom for
    // scheduling jitter (TarpitScheduler's own 200ms tick, thread
    // scheduling) rather than cutting it exactly at the wire.
    void SetTarpitDelayMs(int ms)
    {
        constexpr int kMaxSafeMs = (SocketConnectionState::TIMEOUT_SECONDS - 5) * 1000;
        if (ms < 0) ms = 0;
        if (ms > kMaxSafeMs)
        {
            ETCS_LOG("TarpitNode", "SetTarpitDelayMs: " << ms
                     << "ms exceeds the safe ceiling (" << kMaxSafeMs
                     << "ms, TIMEOUT_SECONDS-5) -- clamping.");
            ms = kMaxSafeMs;
        }
        delay_ms_ = ms;
    }
    int GetTarpitDelayMs() const { return delay_ms_; }

    // --- Request handling: the "Request" action a filtered gate-level
    // registration invokes once AcceptsConcrete above has already matched
    // this connection. Builds the 404 now (cheap, and lets an early Reset
    // never race a half-built SendBuffer) and holds it behind
    // TarpitScheduler rather than sending immediately.
    //
    // Entered holding EXACTLY ONE io_inflight_ reference -- same contract
    // as ReadUntilParsed and every function in this module: a ConnRef owns
    // it, released automatically unless disarmed because something else
    // took it over (see ConnRef.h).
    void ClaimAndDelay(ETCS::RID manager_rid, ETCS::RID conn_rid, const ETCS::SignalContext& ctx)
    {
        SocketConnectionState* conn = resolveConnection(manager_rid, conn_rid);
        if (!conn) return;   // gate or connection gone between Filter and Request --
                              // nothing to construct a ConnRef around
        ConnRef entry = ConnRef::Wrap(conn);

        const char* err = "404 Not Found";
        int send_len = snprintf(conn->SendBuffer().data(), conn->SendBuffer().size(),
            "HTTP/1.1 404 Not Found\r\nConnection: close\r\n"
            "Content-Length: %zu\r\n\r\n%s",
            std::strlen(err), err);
        conn->SetSendLen(send_len);

        // Hand the reference off to whichever of the two paths below
        // actually ends up using it -- the scheduled job's own closure
        // (the common case) or, if scheduling is refused, sendNow called
        // directly right here. Either way the far side reclaims it with
        // its own ConnRef::Wrap as its first statement; nothing here has
        // to remember to release anything afterward.
        entry.disarm();
        bool scheduled = TarpitScheduler::getInstance().ScheduleDelayed(delay_ms_,
            [this, conn, ctx]()
            {
                ConnRef ref = ConnRef::Wrap(conn);
                if (ctx.isInterrupted() || ctx.isTerminated()
                    || !conn->IsConnectionOpen()
                    || ETCS::MemoryArena::getInstance().isTearingDown())
                { conn->Reset(); return; }
                ref.disarm();
                sendNow(conn, ctx, 0);
            });

        if (!scheduled)
        {
            ETCS_LOG("TarpitNode", "at capacity (" << TarpitScheduler::kMaxPending
                     << ") -- sending the 404 immediately instead of holding it, fd="
                     << conn->GetClientFd());
            sendNow(conn, ctx, 0);   // reclaims the reference disarmed above
        }
    }

private:
    // Module-local RID resolution. Same lookup ConnectionManager's own
    // resolveRID (and HttpServer's) already perform, and the same reasoning:
    // the loader's map, not this module's own EventNode, since a manager RID
    // crossing into this type is exactly the kind of cross-module reference
    // that map exists to answer.
    static ETCS::Entity* resolveRID(ETCS::RID rid)
    {
        if (rid == 0) return nullptr;
        auto& ridMap = ETCS::getLoader().ridMap;
        for (auto& [key, handle] : ridMap)
            if (ETCS::Entity* e = handle.invoke_get(rid)) return e;
        return nullptr;
    }

    static SocketConnectionState* resolveConnection(ETCS::RID manager_rid, ETCS::RID conn_rid)
    {
        ETCS::Entity* mgr_entity = resolveRID(manager_rid);
        if (!mgr_entity) return nullptr;
        ConnectionManager* mgr = static_cast<ConnectionManager*>(mgr_entity->getTrueType());
        if (!mgr) return nullptr;
        ETCS::Entity* conn_entity = mgr->getTypedChild(ETCS::Buffer("SocketConnectionState"), conn_rid);
        if (!conn_entity) return nullptr;
        return static_cast<SocketConnectionState*>(conn_entity->getTrueType());
    }

    // The duplicated send loop -- see this class's own top comment for why
    // it is not shared with HttpServer::Serve's do_send. Simpler than
    // do_send in one respect: a tarpit hit always closes, so there is no
    // keep-alive/RecycleForNextRequest branch to carry.
    //
    // Entered holding EXACTLY ONE io_inflight_ reference, same ConnRef
    // contract as every other function in this module (see ConnRef.h):
    // released automatically on every path out unless a NEW reference was
    // separately Acquired for outstanding async work.
    void sendNow(SocketConnectionState* conn, const ETCS::SignalContext& ctx, size_t offset)
    {
        ConnRef entry = ConnRef::Wrap(conn);

        const size_t total = static_cast<size_t>(conn->GetSendLen());
        if (total == 0 || offset >= total) { conn->Reset(); return; }

        if (conn->GetTLS().IsEstablished())
        {
            // Same Acquire-then-branch-on-return-value shape every TLSIO
            // call site in this module uses (TLSConnectionIO.h's own
            // top-of-file contract): settled==true means on_progress
            // already ran synchronously and released `work` itself;
            // settled==false means a new ciphertext op is outstanding and
            // `work` must stay armed so its destructor releases this call's
            // reference immediately, as the contract requires.
            ConnRef work = ConnRef::Acquire(conn);
            bool settled = TLSIO::SubmitTLSSend(conn, this, ctx, offset, total,
                [conn](long long progress)
                {
                    // progress > 0 == everything sent and flushed (see
                    // SubmitTLSSend's own on_progress contract) -- close it
                    // out. progress <= 0 == failure, and SubmitTLSSend has
                    // ALREADY Reset() conn itself on that path, so there is
                    // nothing left to do here but let `ref` release.
                    ConnRef ref = ConnRef::Wrap(conn);
                    if (progress > 0) conn->Reset();
                });
            if (settled) work.disarm();
            return;
        }

        ETCS::IOSubmission send_sub;
        send_sub.op         = ETCS::IOOp::Send;
        send_sub.fd         = conn->GetClientFd();
        send_sub.buffer     = conn->SendBuffer().data() + offset;
        send_sub.buffer_len = total - offset;
        send_sub.priority   = static_cast<int>(ETCS::Priority::Medium);
        send_sub.ctx        = ctx;

        // Registered against THIS entity, same as do_send's own conn_io
        // scope one layer over in NetworkProvider.h -- what makes a
        // teardown of this TarpitNode actually wait for connections it
        // still holds open, rather than releasing them out from under an
        // in-flight send.
        auto send_scope = std::make_shared<ETCS::ScopeTag>(this, "conn_io", ctx);
        send_sub.callback = [this, conn, ctx, offset, total, send_scope = std::move(send_scope)]
                            (ETCS::IOCompletion comp) mutable
        {
            ConnRef ref = ConnRef::Wrap(conn);
            // <= 0 covers both error and a zero-byte accept, same as every
            // other send completion in this module -- treating 0 as
            // progress would spin this loop forever on a dead peer.
            if (comp.result <= 0) { conn->Reset(); return; }

            const size_t now = offset + static_cast<size_t>(comp.result);
            // Re-enter sendNow either way -- when `now == total` this
            // immediately hits its own top guard and closes the
            // connection from the one place that decides it. A tarpit hit
            // never keeps the connection: unlike do_send, there is no
            // keep-alive re-arm branch anywhere in this loop, on purpose --
            // a scanner that got a slow 404 gets nothing more.
            ref.disarm();
            sendNow(conn, ctx, now);
        };

        ConnRef work = ConnRef::Acquire(conn);
        if (conn->getThreadPool().submit(std::move(send_sub))) { work.disarm(); }
        else
        {
            ETCS_LOG("TarpitNode", "send refused (SQ full) at offset " << offset << "/"
                     << total << " -- dropping fd=" << conn->GetClientFd());
            conn->Reset();
        }
    }

    TarpitFilter tarpit_;
    int          delay_ms_ = 6000;
};

#endif // TARPITNODE_H__
