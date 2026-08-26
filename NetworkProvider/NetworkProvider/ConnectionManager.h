#ifndef CONNECTIONMANAGER_H__
#define CONNECTIONMANAGER_H__
#include "../../../ontology.h"
#include "SocketConnectionState.h"
#include "TLSServerConfig.h"
#include "TLSConnectionIO.h"
#include "ConnectionRecvLoop.h"
#include "ConnRef.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <vector>
#include <string>
#include <mutex>
#include <memory>
#include <thread>
#include <cstring>

// ConnectionManager — the Gate_ for an inbound TCP listener. Owns the
// listening fd, mints one SocketConnectionState per accepted connection as an
// addTag'd child, and hands each one to whatever registered to receive them.
//
// It does NOT interpret connections. Deciding what a connection is for
// belongs to a subscriber; this type only observes crossings and publishes
// them. That separation is the whole reason this exists: NetworkProvider
// previously fused accepting and serving into HTTPParser::Listen, which is
// why four near-identical consumers had to each re-implement the connection
// lifecycle, and why the parse state machine and the server shared one type
// that could only serve concurrent connections by giving every connection its
// own private parser anyway.
//
// SUBSCRIBERS are (RID, action) pairs, re-resolved on EVERY connection rather
// than cached as pointers. That is the correct discipline for a reference
// crossing out of this entity's own causal domain: a subscriber may die at any
// time, and a stale Entity* would be a live pointer into recycled arena bytes
// (see MemoryArena::reclaimEntity -- an entity's outer shell is zeroed and
// handed to the next same-type allocation, so a stale pointer is not
// dangling-and-crashing but silently valid and WRONG). Re-resolving makes the
// list self-healing: a subscriber whose RID no longer resolves is dropped on
// the spot, with no unregister call required. The cost is one unordered_map
// scan per accepted connection, which is nothing beside the syscalls
// surrounding it.
//
// Everything INSIDE this entity's domain is a strong reference by contrast --
// connections are typed children, reached by getTypedChild, never by RID
// lookup. A RID crossing marks a genuine causal boundary; using one within a
// single domain would be both slower and a lie about the structure.
//
// The Gate_ contract's cross-thread Close() obligation is met by closing
// listen_fd_: a thread parked in accept() observes no SignalContext flag (it
// executes no code of ours), and closing the descriptor is what actually
// returns it. See Gate.h for why that obligation exists at all.
class ConnectionManager :
    public GateBase<ConnectionManager>, public DeletableBase<ConnectionManager>,
    public EphemeralBase<ConnectionManager>
{
public:
    WIRE_TYPE_IDENTITY(ConnectionManager);

    ConnectionManager() = default;
    virtual ~ConnectionManager() { CloseConcrete(); }

    // --- Gate_ concrete surface ---

    // config: "<port>". Binds, listens, and starts the accept chain. Every
    // failure returns false with a log rather than throwing -- Open is a work
    // action a script calls directly, so a bad port should be a visible failed
    // line, not an exception unwinding through the executor.
    bool OpenConcrete(const ETCS::Buffer& config)
    {
        if (listen_fd_ != -1)
        {
            ETCS_LOG("ConnectionManager", "Open: already open on port " << port_
                     << " (RID:" << getRID() << ") -- close it first.");
            return false;
        }

        int port = 8080;
        {
            ETCS::Buffer cfg = config;
            if (cfg.written > 0) cfg >> port;
        }

        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { ETCS_LOG("ConnectionManager", "Open: socket() failed."); return false; }

        int opt = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port        = htons(static_cast<uint16_t>(port));

        if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0)
        {
            ETCS_LOG("ConnectionManager", "Open: bind() failed on port " << port << ".");
            ::close(fd);
            return false;
        }
        if (::listen(fd, kBacklog) < 0)
        {
            ETCS_LOG("ConnectionManager", "Open: listen() failed on port " << port << ".");
            ::close(fd);
            return false;
        }

        listen_fd_ = fd;
        port_      = port;
        stopping_.store(false, std::memory_order_release);

        // Registered BEFORE the first submission, so the scope exists for the
        // entire time the chain does -- there is no window where an accept is
        // outstanding with nothing registered to name or interrupt it. Same
        // construct-before-enqueue discipline DEFINE_STREAM_FUNC_PRODUCE uses
        // for its own guard.
        accept_scope_ = std::make_unique<ETCS::ScopeTag>(this, "accept", open_ctx_);
        accept_ctx_   = accept_scope_->ctx();

        ETCS_LOG("ConnectionManager", "Open: listening on port " << port
                 << " (RID:" << getRID() << ").");

        growPool();   // mint the initial block before the first accept lands

        // A WINDOW of accepts, not one. With a single outstanding accept every
        // connection costs two thread handoffs in strict series -- cqe ->
        // io_completion_loop -> enqueue -> notify_one -> worker -> callback ->
        // submitAccept -> io_uring_submit -- so the accept rate is bounded by
        // that round trip regardless of how idle the pool is. Past the backlog
        // the kernel drops SYNs, which clients see as the server being slow.
        // Each completion re-arms exactly one, so the window stays constant.
        for (size_t i = 0; i < kAcceptWindow; ++i) submitAccept();
        startMaintenance();
        return true;
    }

    // Idempotent, and safe from a thread other than one blocked in accept() --
    // both are Gate_ contract requirements (see Gate.h). stopping_ is set
    // BEFORE the close so an in-flight completion that fires during teardown
    // sees it and declines to resubmit, rather than racing to re-arm a chain
    // against a descriptor that is about to be (or already has been) closed.
    bool CloseConcrete()
    {
        if (listen_fd_ == -1) return true;

        stopping_.store(true, std::memory_order_release);

        // Joined BEFORE the drain: it captures `this` bare and both of its
        // jobs touch the pool, so it has to be gone before anything below
        // starts tearing that down. stopping_ is already set, so it exits at
        // its next check rather than sleeping out a full tick.
        if (maintain_running_.exchange(false, std::memory_order_acq_rel)
            && maintain_thread_.joinable())
            maintain_thread_.join();

        // THE FD NUMBER IS RETIRED HERE, BUT THE DESCRIPTOR IS CLOSED AFTER
        // THE DRAIN -- see the close below. Taking it out of listen_fd_ now
        // keeps Close idempotent (a second call returns at the top) and stops
        // submitAccept re-arming, while leaving the descriptor itself open
        // for as long as anything still names it.
        //
        // That split is the fix for a real hang. This function used to
        // ::close(listen_fd_) immediately after submitting the cancel below,
        // and io_uring_prep_cancel_fd resolves its fd when the KERNEL
        // processes the SQE, not when we submit it -- so the close raced
        // ahead, the cancel matched nothing, and every outstanding accept
        // stayed pending forever. io_uring holds its own reference to the
        // file, so closing the descriptor does NOT retire them either. With
        // a 32-deep accept window that is 32 references that never come
        // back. The old 5s ceiling hid it as a pause; without the ceiling it
        // is a hang, which is how it was finally found.
        //
        // SocketConnectionState::ResetConcrete already documents this exact
        // hazard and already solves it this exact way ("NO CLOSE HERE ...
        // the close moves to finalizeIfDraining, which already waits for
        // io_inflight_ to reach zero -- so the number is only released once
        // nothing names it"). This gate simply never got the same treatment.
        const int closing_fd = listen_fd_;
        listen_fd_ = -1;

        // The close is what actually unparks anything waiting on this fd and
        // is the Gate_ contract's real obligation (see Gate.h). The cancel is
        // what retires the accept window promptly rather than leaving it
        // parked on a descriptor nobody will ever connect to again.
        //
        // Deliberately skipped during arena teardown. Constructing an
        // IOSubmission allocates (its SignalContext carries Buffers), and by
        // the time this runs from ~MemoryArena's own dtor-chain walk,
        // isTeardown_ is already set -- so the allocation throws
        // "Allocation after teardown", and that exception unwinds straight
        // through ~IOSubmission into std::terminate. A real, reproduced abort
        // on every Ctrl+C with the server running.
        //
        // Nothing is lost by skipping it there: the pool's own threads are
        // already joined by that point in shutdown, so there is no
        // outstanding submission left for a cancel to reach, and the OS
        // reclaims the descriptor at process exit regardless.
        // Whether the mechanism that retires the accept window actually got
        // armed. The drain below is causal -- it waits for a count to reach
        // zero rather than for time to pass -- so it may only wait when
        // something is actually capable of driving that count down. If the
        // cancel was skipped (teardown) or refused (ring full), the pending
        // accepts have nothing left to retire them and waiting on them would
        // be waiting on an event that is never coming.
        bool cancel_armed = false;
        if (!ETCS::MemoryArena::getInstance().isTearingDown())
        {
            ETCS::IOSubmission cancel;
            cancel.op  = ETCS::IOOp::Cancel;
            cancel.fd  = closing_fd;
            cancel.ctx = open_ctx_;
            cancel_armed = ETCS::ThreadPool::getInstance().submit(std::move(cancel));
            if (!cancel_armed)
                ETCS_LOG("ConnectionManager", "Close: cancel submission REFUSED (SQ full) -- "
                         << inflight_.load() << " accept(s) cannot be retired and will be "
                            "abandoned rather than waited on.");
        }

        // Retire connections still MID-HANDSHAKE before draining on them.
        //
        // This is what makes the causal drain below actually converge rather
        // than merely refuse to lie. The cancel above names listen_fd_, which
        // says nothing about any connection's own fd, and the maintenance
        // thread that would otherwise reap a stalled handshake through
        // checkTimeout was joined further up -- so a peer that completes TCP
        // and then says nothing has an outstanding recv that nobody is left
        // to retire. Waiting on that count would be waiting on an event that
        // is never coming. Reset submits that connection's own cancel, which
        // surfaces its recv as -ECANCELED, which runs the completion, which
        // settles the handshake as failed and decrements the count. Forcing
        // the event beats timing out on its absence.
        //
        // ONLY handshaking connections, deliberately. Close stops listening;
        // it does not evict established peers, and an in-flight keep-alive
        // request is none of its business -- that distinction between Close
        // and Delete is the whole reason both verbs exist. A handshaking
        // connection is different in kind: its I/O was issued under
        // accept_ctx_, which this function is about to release, so it is
        // this gate's own outstanding work rather than a subscriber's.
        //
        // Snapshot under the lock, act outside it -- Reset can run finalize,
        // and none of that should happen while an accept is blocked waiting
        // for pool_mutex_. Same discipline reapIdle already documents.
        {
            std::vector<SocketConnectionState*> handshaking;
            {
                std::lock_guard<std::mutex> lock(pool_mutex_);
                for (SocketConnectionState* c : pool_)
                    if (c && c->IsActive()
                        && c->GetTLS().GetPhase() == TLSServerContext::Phase::Handshaking)
                        handshaking.push_back(c);
            }
            if (!handshaking.empty())
                ETCS_LOG("ConnectionManager", "Close: retiring " << handshaking.size()
                         << " connection(s) still mid-handshake so their references "
                            "can actually retire.");
            for (SocketConnectionState* c : handshaking) c->Reset();
        }

        // Drain: do not report closed while a completion could still fire and
        // touch this entity.
        //
        // CAUSAL, NOT TIMED. This loop used to carry a 5s ceiling, and the
        // ceiling was the bug: a deadline answers "has enough time passed"
        // when the question is "can the remaining count still converge",
        // and those are different questions whose answers only coincide by
        // luck. Worse, a ceiling that fires proceeds anyway -- releasing
        // accept_scope_ and returning "closed" while a completion is still
        // live and still able to touch this entity, which is precisely the
        // state the drain exists to rule out. Timing out is not a safe
        // fallback here; it is the unsafe outcome wearing a warning label.
        //
        // So the loop now ends on exactly two things, both of them facts
        // about whether convergence is still POSSIBLE, neither of them a
        // clock:
        //
        //   1. The counts reach zero -- the wait succeeded.
        //   2. Convergence is provably impossible, for one of two reasons
        //      the pool can actually tell us about (below).
        //
        // If neither holds we keep waiting, indefinitely and on purpose. An
        // in-flight completion that can still arrive WILL arrive: every path
        // that could otherwise strand one already submits a Cancel to force
        // it out as -ECANCELED (ResetConcrete, and this function's own
        // cancel above), so the counts converge on their own without anyone
        // timing them. A genuine hang here means an invariant is already
        // broken, and hanging visibly at the point of breakage is a better
        // outcome than continuing past it -- the wait is what makes the
        // breakage a stoppable event rather than a silent corruption that
        // surfaces somewhere unrelated later.
        //
        // tls_handshakes_ is drained alongside inflight_, not after it: both
        // name work issued under accept_ctx_, and accept_scope_ below must
        // outlive every last piece of it. See tls_handshakes_'s own comment
        // for why the accept count alone stopped being sufficient once the
        // handshake phase made onConnection asynchronous.
        int spin = 0;
        while (cancel_armed
               && (inflight_.load(std::memory_order_acquire) > 0
                   || tls_handshakes_.load(std::memory_order_acquire) > 0))
        {
            ETCS::ThreadPool& pool = ETCS::ThreadPool::getInstance();

            // TERMINATION 1 -- nothing is left alive to complete anything.
            // At process exit ThreadPool's own static destructor may already
            // have joined its io and worker threads; ordering between two
            // independent Meyers singletons is unspecified, and the observed
            // sequence is precisely that: pool drained, THEN this Close
            // reached, with one accept still outstanding that nothing was
            // left alive to complete.
            //
            // Breaking out is safe rather than a concession: if the pool is
            // drained, no callback can be running or ever start, which is
            // the exact property the wait exists to establish. This is a
            // causal fact about the pool, not an elapsed-time guess.
            if (pool.isDrained())
            {
                ETCS_LOG("ConnectionManager", "Close: pool already drained -- "
                         << inflight_.load() << " accept + " << tls_handshakes_.load()
                         << " handshake submission(s) can no longer "
                            "complete; ending the wait.");
                break;
            }

            // TERMINATION 2 -- a worker was force-cancelled, so the counts
            // are known-corrupt rather than merely slow. This is THE reason
            // the old ceiling existed, named directly instead of inferred
            // from elapsed time: ThreadPool's watchdog sets this flag
            // immediately before it cancels a thread, and a cancelled worker
            // never runs the NoteComplete it was holding, so whatever it
            // owned is permanently unaccounted for. Observing the event that
            // breaks the invariant is causal; waiting 5s and assuming it
            // must have happened is not.
            //
            // NOTE that getLastError CONSUMES the flag -- it is a one-slot
            // channel shared with every other reader in the process. Logged
            // in full here rather than swallowed, so consuming it does not
            // destroy the only record that it happened.
            std::string cancelled_tag;
            if (pool.getLastError(cancelled_tag))
            {
                ETCS_LOG("ConnectionManager", "Close: pool reported a force-cancelled task ('"
                         << cancelled_tag << "') -- " << inflight_.load() << " accept + "
                         << tls_handshakes_.load() << " handshake reference(s) can never be "
                            "released by it. Ending the wait; this gate's accounting for "
                            "those submissions is now permanently short.");
                break;
            }

            // Pure backoff, and deliberately not a decision: nothing about
            // how long this has spun is allowed to end the loop. Yields
            // rather than spinning hot for the reason ChessGame's own wait
            // documents -- this can run on a ThreadPool thread, and a hot
            // spin there starves the very completions being waited on, which
            // would turn a finite wait into a real deadlock.
            ETCS::LMAXSequentialSharedPage::progressiveYield(spin);
        }

        // NOW the descriptor goes back to the OS -- once the drain above has
        // established that nothing still names it. Closing any earlier is
        // what broke the cancel; see the closing_fd comment above.
        ::close(closing_fd);

        accept_scope_.reset();
        accept_ctx_ = ETCS::SignalContext{};

        ETCS_LOG("ConnectionManager", "Close: stopped listening on port " << port_
                 << " (RID:" << getRID() << ").");
        return true;
    }

    bool IsOpenConcrete() const { return listen_fd_ != -1; }

    // --- Ephemeral_ / Deletable_ concrete surface ---

    bool ResetConcrete()
    {
        CloseConcrete();
        {
            std::lock_guard<std::mutex> lock(pool_mutex_);
            for (SocketConnectionState* c : pool_) if (c) c->Reset();
        }
        std::lock_guard<std::mutex> lock(subs_mutex_);
        subscribers_.clear();
        return true;
    }

    bool IsActiveConcrete() const { return listen_fd_ != -1; }

    bool DeleteConcrete()
    {
        CloseConcrete();
        std::string conjugate_key = getSourceModule().toString() + ":" + getSourceTag().toString();
        ETCS_LOG("ConnectionManager", "Delete: firing self-DestroyEvent for RID:"
                 << getRID() << " (" << conjugate_key << ")");
        // Cascades to every live SocketConnectionState child -- the default
        // now, and correct: a manager going away takes its connections with
        // it, since nothing else has any way to reach them.
        return ETCS::DestroyEvent{conjugate_key.c_str(), this}();
    }

    // --- Subscriber surface (implementation, not ontology) ---
    //
    // action is a bare action name ("Serve"), qualified against the
    // subscriber's own source tag at dispatch time -- the subscriber knows its
    // own tag, and requiring a caller to spell "HttpServer.Serve" would mean
    // two places to keep in agreement.
    void RegisterConsumer(ETCS::RID rid, const std::string& action,
                          ETCS::RID filter_rid = 0, const std::string& filter_action = "")
    {
        if (rid == 0 || action.empty()) return;
        std::lock_guard<std::mutex> lock(subs_mutex_);
        for (auto& s : subscribers_)
            if (s.rid == rid && s.action == action) return; // already registered
        subscribers_.push_back(Subscriber{rid, action, filter_rid, filter_action});
        // Kept in lockstep with subscribers_ at every mutation site (here,
        // UnregisterConsumer, and dispatchToSubscribers's own dead-entry
        // sweep) so beginDispatch can answer "is there a filtered
        // subscriber at all" with one relaxed atomic load instead of taking
        // subs_mutex_ and scanning the list on every accepted connection --
        // see hasFilteredSubscriber's own comment.
        if (filter_rid != 0 && !filter_action.empty())
            filtered_subscriber_count_.fetch_add(1, std::memory_order_relaxed);
        ETCS_LOG("ConnectionManager", "RegisterConsumer: RID:" << rid
                 << " ." << action
                 << (filter_rid ? " filtered by RID:" + std::to_string(filter_rid)
                                  + " ." + filter_action
                                : std::string(" (unfiltered)"))
                 << " on RID:" << getRID());
    }

    void UnregisterConsumer(ETCS::RID rid)
    {
        std::lock_guard<std::mutex> lock(subs_mutex_);
        for (auto it = subscribers_.begin(); it != subscribers_.end(); )
        {
            if (it->rid != rid) { ++it; continue; }
            if (it->filtered())
                filtered_subscriber_count_.fetch_sub(1, std::memory_order_relaxed);
            it = subscribers_.erase(it);
        }
    }

    size_t SubscriberCount() const
    {
        std::lock_guard<std::mutex> lock(subs_mutex_);
        return subscribers_.size();
    }

    int  GetListenFd() const { return listen_fd_; }
    int  GetPort()     const { return port_; }

    // The SignalContext this gate was opened under -- threaded into every
    // IOSubmission the accept chain makes, and handed to each subscriber
    // dispatch so a connection's own work inherits the same authority the
    // gate itself runs under.
    void SetOpenContext(const ETCS::SignalContext& ctx) { open_ctx_ = ctx; }

    // Turns on server-side TLS termination for every connection accepted
    // from this point forward. Builds the ONE shared TLSServerConfig
    // (TLSServerConfig.h) once; every SocketConnectionState's own
    // TLSServerContext (TLSServerContext.h) is set up against it fresh, per
    // connection, in onConnection below -- cheap per-connection state
    // against expensive per-config state, the standard mbedTLS pattern.
    //
    // REFUSED WHILE OPEN. An earlier version of this allowed a mid-flight
    // call and documented that it "only affects connections accepted from
    // that point on" -- which is a gate serving plaintext and TLS
    // simultaneously on one port, with which mode a given connection got
    // decided by whether it happened to arrive before or after this call.
    // Nothing can reason about that, least of all anyone trying to
    // establish whether traffic on this port is encrypted. A transport is a
    // property of the listener, so it is settled before the listener exists
    // or not at all.
    //
    // Repeatable before Open: each call builds a FRESH config and installs
    // it only on success, so calling it twice with different paths simply
    // takes the second, and a failed call leaves any previously-working
    // config exactly as it was.
    bool EnableTLS(const std::string& cert_path, const std::string& key_path)
    {
        if (listen_fd_ != -1)
        {
            ETCS_LOG("ConnectionManager", "EnableTLS: already open on port " << port_
                     << " (RID:" << getRID() << ") -- REFUSING. A listener's "
                        "transport is fixed for its lifetime; close it first. "
                        "To rotate a certificate on a RUNNING gate, use "
                        "ReloadCerts.");
            return false;
        }
        return installConfig(cert_path, key_path, "EnableTLS");
    }

    // Rotates the certificate on a RUNNING gate, with no dropped
    // connections and no rebind. This is what a renewal hook calls.
    //
    // Allowed while open, unlike EnableTLS, and the difference is not
    // arbitrary. EnableTLS decides whether this listener speaks TLS at all,
    // which is a property of the listener and must be settled before it
    // exists. ReloadCerts changes only WHICH certificate an already-TLS
    // gate presents -- it cannot turn plaintext into TLS, and it refuses to
    // try.
    //
    // Nothing is disturbed unless the new certificate actually loads: the
    // new config is built and validated in full BEFORE it is installed, so
    // a botched renewal (expired file, wrong path, unreadable key) leaves
    // the running gate serving its existing certificate. Failing closed
    // here would mean a fumbled cron job takes the site down while a
    // perfectly valid certificate was still on disk.
    //
    // Connections already established keep the config they handshook
    // against until they close -- see TLSServerContext::Init. Only
    // connections accepted after this point see the new one.
    bool ReloadCerts(const std::string& cert_path, const std::string& key_path)
    {
        if (!IsTLSActive())
        {
            ETCS_LOG("ConnectionManager", "ReloadCerts: TLS is not enabled on RID:"
                     << getRID() << " -- refusing. ReloadCerts rotates a certificate; "
                        "it does not turn a plaintext gate into a TLS one.");
            return false;
        }
        return installConfig(cert_path, key_path, "ReloadCerts");
    }

    // Same rotation, against the paths this gate is ALREADY using -- which
    // is the ordinary case, because a renewal rewrites files in place rather
    // than moving them. certbot's live/ directory is symlinks precisely so
    // the path a server was configured with stays the path the new
    // certificate appears at.
    //
    // That makes the no-argument form the SAFER one to reach for, not merely
    // the shorter: a hook that repeats the paths can drift from the ones the
    // gate actually loaded -- someone edits the trace, or a second cert gets
    // provisioned -- and reload silently starts serving a certificate nobody
    // configured. Asking the gate what it is using cannot drift.
    bool ReloadCerts()
    {
        std::string cert, key;
        {
            std::lock_guard<std::mutex> lock(tls_mutex_);
            cert = tls_cert_path_;
            key  = tls_key_path_;
        }
        if (cert.empty() || key.empty())
        {
            ETCS_LOG("ConnectionManager", "ReloadCerts: no certificate paths recorded on RID:"
                     << getRID() << " -- this gate was never given any, so there is "
                        "nothing to reload. Use EnableTLS before Open.");
            return false;
        }
        // Released the lock before this: installConfig parses files from
        // disk, and holding a mutex that every accept contends on across
        // file I/O would stall the accept path for the duration of a
        // certificate parse.
        return ReloadCerts(cert, key);
    }

    // Whether this gate terminates TLS at all. Cheap enough to take the
    // lock -- it is consulted once per accepted connection, next to two
    // syscalls and a pool claim.
    bool IsTLSActive() const
    {
        std::lock_guard<std::mutex> lock(tls_mutex_);
        return tls_conf_ != nullptr;
    }

private:
    // Builds a FRESH config, validates it fully, and only then installs it.
    // Shared by EnableTLS and ReloadCerts so both get identical validation
    // and identical failure semantics -- the only difference between them
    // is the guard each applies before calling this.
    //
    // A new object every time, never a reload in place: LoadCertAndKey is
    // single-use by construction (see its own guard), and building fresh is
    // also what gives in-flight connections something stable to keep
    // pointing at.
    bool installConfig(const std::string& cert_path, const std::string& key_path,
                       const char* who)
    {
        auto fresh = std::make_shared<TLSServerConfig>();
        if (!fresh->LoadCertAndKey(cert_path, key_path))
        {
            ETCS_LOG("ConnectionManager", who << ": failed to load cert='" << cert_path
                     << "' key='" << key_path << "' (RID:" << getRID()
                     << ") -- nothing changed; the gate keeps whatever it was "
                        "already using.");
            return false;
        }
        {
            // Config and the paths it came from move together, under one
            // lock: the no-argument ReloadCerts reads those paths to decide
            // what to load, so a window where they described a different
            // config than the one installed would let a reload pick up the
            // previous certificate's path and quietly revert.
            std::lock_guard<std::mutex> lock(tls_mutex_);
            tls_conf_      = std::move(fresh);
            tls_cert_path_ = cert_path;
            tls_key_path_  = key_path;
        }
        ETCS_LOG("ConnectionManager", who << ": loaded cert='" << cert_path
                 << "' key='" << key_path << "' (RID:" << getRID()
                 << ") -- in effect for connections accepted from now on; "
                    "sessions already established keep their own until they close.");
        return true;
    }

    // A COPY, taken under the lock. The refcount bump is the whole point:
    // whatever this hands back stays valid for the caller even if
    // ReloadCerts replaces tls_conf_ an instant later.
    std::shared_ptr<TLSServerConfig> currentTLSConfig() const
    {
        std::lock_guard<std::mutex> lock(tls_mutex_);
        return tls_conf_;
    }

    // filter_rid/filter_action are OPTIONAL (0/"" means unfiltered). A filtered
    // subscriber declares a predicate this gate consults before handing over a
    // connection; an unfiltered one takes anything.
    struct Subscriber
    {
        ETCS::RID   rid;
        std::string action;
        ETCS::RID   filter_rid    = 0;
        std::string filter_action;
        bool filtered() const { return filter_rid != 0 && !filter_action.empty(); }
    };

    // Concurrent outstanding accepts. inflight_ already counts them and Close
    // already drains on it, so the window needs no new bookkeeping.
    //
    // 32, not 8. Confirmed by TcpExtListenOverflows climbing (18 over one
    // session): the backlog filled and the kernel DROPPED those SYNs, which a
    // client sees as silence, not refusal -- so curl stalls through its SYN
    // retransmit backoff (1s, 3s, 7s, 15s, 31s) and then succeeds. That is the
    // hang-then-recover shape, and the idle counter reading 64 on recovery is
    // the backoff, not the server.
    //
    // The backlog only drains as fast as accepts retire, and keep-alive made
    // that much slower: a tab now holds a connection for up to
    // TIMEOUT_SECONDS, so a reload opening several at once outruns a window of
    // 8 immediately. 32 outstanding SQEs against a 256-entry ring is free.
    static constexpr size_t kAcceptWindow = 32;
    // How often the maintenance thread runs. Well under TIMEOUT_SECONDS so an
    // expired connection is reaped promptly rather than a whole tick late.
    static constexpr int    kMaintainMs   = 1000;
    // Maintenance ticks between stuck-connection reports (see reapIdle).
    // 30 at kMaintainMs=1000 is one line per stuck connection every ~30s --
    // enough to watch a count fail to move, quiet enough to read around.
    static constexpr int    kStuckReportTicks = 30;
    // Was 8. The backlog is what absorbs a burst arriving faster than the
    // window can retire; at 8 it was the binding limit under load.
    static constexpr int    kBacklog      = 128;

    std::thread        maintain_thread_;
    std::atomic<bool>  maintain_running_{false};
    std::atomic<long>  rearms_{0};
    // Touched only by the maintenance thread, so a plain int is correct --
    // see reapIdle's own stuck report for what it paces.
    int                stuck_report_tick_ = 0;

    int                 listen_fd_ = -1;
    int                 port_      = 0;
    std::atomic<int>    inflight_{0};
    std::atomic<bool>   stopping_{false};

    // TLS termination. Non-null means this gate terminates TLS: onConnection
    // below takes a copy per accepted connection and, if there is one, runs
    // the handshake phase (TLSConnectionIO.h's DriveTLSHandshake) before
    // subscriber dispatch. Null means plaintext straight to
    // dispatchToSubscribers, unchanged from before this feature existed.
    // The CURRENT config new connections are handed. shared_ptr rather than
    // a plain member because a connection outlives the manager's interest
    // in the config it used: after ReloadCerts, sessions still in flight
    // hold the superseded config alive until they close (see
    // TLSServerConfig.h's own LIFETIME note). nullptr means plaintext.
    //
    // Guarded by its own mutex, not by pool_mutex_ or subs_mutex_ -- it is
    // read on the io completion thread once per accept and written from
    // whichever thread ran ReloadCerts, and it has nothing to do with the
    // pool or the subscriber list. Copies are taken out of it rather than
    // referenced (currentTLSConfig below), so the lock is never held across
    // anything that could block.
    mutable std::mutex               tls_mutex_;
    std::shared_ptr<TLSServerConfig> tls_conf_;

    // Where tls_conf_ was loaded FROM, so the no-argument ReloadCerts can
    // reload in place. Guarded by the same mutex and written in the same
    // critical section as tls_conf_ itself -- they are one fact, not two.
    // Empty until the first successful EnableTLS, which is exactly the
    // condition that makes a bare ReloadCerts meaningless and is checked
    // there.
    std::string                      tls_cert_path_;
    std::string                      tls_key_path_;

    // Connections currently between accept and handshake-settled.
    //
    // Needed because the TLS path broke an invariant the accept chain used
    // to hold for free. Before TLS, onConnection dispatched SYNCHRONOUSLY
    // inside the accept completion, so by the time that callback returned
    // the connection already belonged to a subscriber holding its own
    // conn_io scope, and inflight_ reaching zero genuinely meant "nothing
    // this gate started is still running". With TLS, onConnection returns
    // while handshake I/O is still outstanding under accept_ctx_ -- so
    // inflight_ hits zero, CloseConcrete's drain passes, and accept_scope_
    // is released out from under live submissions issued under its own
    // derived context.
    //
    // Draining on this as well closes that window. The per-submission
    // tls_io ScopeTags (TLSConnectionIO.h) name the work; this is what
    // makes Close actually WAIT for it, which a scope alone does not do.
    //
    // Convergence is FORCED, not awaited: CloseConcrete resets every
    // still-handshaking connection before it drains on this, so each one's
    // own cancel surfaces its outstanding recv as -ECANCELED and settles it.
    // Without that step this count would genuinely never reach zero for a
    // peer that completes TCP and then goes silent -- the maintenance thread
    // that would normally reap it via checkTimeout is joined before the
    // drain, and the cancel on listen_fd_ says nothing about a connection's
    // own fd. See that call site for why only handshaking connections are
    // retired and established ones are left alone.
    std::atomic<int>    tls_handshakes_{0};

    // The context this gate was opened under -- the caller's, inherited from
    // whatever started the server. Parent of accept_scope_'s own derived
    // context, so a signal from above still reaches everything below.
    ETCS::SignalContext open_ctx_;

    // ONE long-lived scope for the whole accept chain, held for exactly as
    // long as the gate is open -- not one per submission.
    //
    // Per-submission was wrong in a way the shell made visible: `kill accept
    // 0` set a flag on a scope that had already been replaced by the next
    // accept's own, so the list showed an `accept` entry blinking in and out
    // rather than naming a durable activity. A scope should name a thing that
    // is running, and the thing running here is the chain, not any one
    // acquire. conn_io is genuinely per-submission and correctly transient;
    // this is not.
    //
    // Its derived context (accept_ctx_) is what every accept submission AND
    // every connection dispatch is issued under, so the chain reads:
    //
    //   process root -> script/server ctx -> accept scope -> connection work
    //
    // meaning `kill accept 0` stops accepting AND every connection the gate
    // has published, in one operation, without touching anything else the
    // server owns. That is the whole point of the scope being long-lived:
    // interrupting a transient one could never have reached work it spawned
    // earlier.
    //
    // unique_ptr because ScopeTag has no default ctor (it registers on
    // construction) and is not move-assignable, so it cannot simply be
    // reseated on a later Open.
    std::unique_ptr<ETCS::ScopeTag> accept_scope_;
    ETCS::SignalContext             accept_ctx_;

    mutable std::mutex      subs_mutex_;
    std::vector<Subscriber> subscribers_;

    // Count of subscribers_ entries with filtered()==true, maintained
    // alongside every mutation of subscribers_ itself (RegisterConsumer,
    // UnregisterConsumer, dispatchToSubscribers's own dead-entry sweep) so
    // hasFilteredSubscriber() below never has to touch subs_mutex_ or scan
    // the list on the accept-time hot path. Relaxed ordering is enough: the
    // actual dispatch decision still reads subscribers_ itself under the
    // lock (dispatchToSubscribers's own snapshot), so a stale count here can
    // only ever cost one extra (or one skipped) pre-parse on the connection
    // immediately following a registration change -- never an incorrect
    // dispatch.
    std::atomic<int>        filtered_subscriber_count_{0};

    // ── Connection pool ──────────────────────────────────────────────────
    // Connections are minted once and RECYCLED, not created and destroyed per
    // request. Both ends of the old lifecycle were blocking round-trips to the
    // loader's single ordering thread -- AddTagEvent on accept, DestroyEvent
    // plus a caller-thread scope drain on teardown -- shared with every module
    // in the process. That is what made one slow connection delay unrelated
    // ones. Reuse pays it once per POOL GROWTH instead of once per connection.
    //
    // Sized in powers of two: double at >2/3 in use, halve at <1/3. The gap
    // between the two thresholds is what stops a load hovering at a boundary
    // from growing and shrinking on alternate accepts.
    static constexpr size_t kMinPool = 8;
    static constexpr size_t kMaxPool = 4096;

    mutable std::mutex                  pool_mutex_;
    std::vector<SocketConnectionState*> pool_;
    size_t                              pool_cursor_ = 0;
    std::atomic<int>                    in_use_{0};
    std::atomic<bool>                   growing_{false};

    // Rotating cursor rather than a scan from zero: in the steady state the
    // next slot is free, so the common case is one CAS.
    //
    // NOTHING THAT BLOCKS ON THE LOADER RUNS UNDER pool_mutex_. addTag and
    // DestroyEvent are both blocking round-trips to the loader's single
    // ordering thread; holding this mutex across one serializes every accept
    // behind it, which is the coupling the pool exists to remove. Growth mints
    // outside the lock and splices in; shrink collects victims under the lock
    // and destroys them after releasing it.
    SocketConnectionState* acquireConnection(int fd)
    {
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            SocketConnectionState* claimed = nullptr;
            bool want_grow = false;
            std::vector<SocketConnectionState*> victims;
            {
                std::lock_guard<std::mutex> lock(pool_mutex_);
                const size_t n = pool_.size();
                for (size_t i = 0; i < n; ++i)
                {
                    const size_t idx = (pool_cursor_ + i) % n;
                    SocketConnectionState* c = pool_[idx];
                    if (c && c->TryClaim(fd))
                    {
                        pool_cursor_ = (idx + 1) % n;
                        in_use_.fetch_add(1, std::memory_order_acq_rel);
                        claimed = c;
                        break;
                    }
                }
                rebalanceLocked(want_grow, victims);
            }

            for (SocketConnectionState* v : victims)
                ETCS::DestroyEvent{"NetworkProvider:SocketConnectionState", v, true}();

            if (claimed) { if (want_grow) growPool(); return claimed; }
            if (!growPool()) break;
        }

        ETCS_LOG("ConnectionManager", "pool exhausted at " << pool_.size()
                 << " (cap " << kMaxPool << ") -- refusing fd=" << fd);
        return nullptr;
    }

    // Mints outside pool_mutex_, splices in under it. growing_ keeps two
    // concurrent accepts from each minting a full block.
    bool growPool()
    {
        size_t cur, target;
        {
            std::lock_guard<std::mutex> lock(pool_mutex_);
            cur    = pool_.size();
            target = (cur == 0) ? kMinPool : cur * 2;
            if (target > kMaxPool) return false;
        }
        if (growing_.exchange(true, std::memory_order_acq_rel)) return false;

        std::vector<SocketConnectionState*> fresh;
        for (size_t i = cur; i < target; ++i)
        {
            SocketConnectionState* c = addTag<SocketConnectionState>();
            if (!c) { ETCS_LOG("ConnectionManager", "addTag<SocketConnectionState> failed at " << i); break; }
            c->SetPoolCounter(&in_use_);
            fresh.push_back(c);
        }

        size_t now = 0;
        {
            std::lock_guard<std::mutex> lock(pool_mutex_);
            for (SocketConnectionState* c : fresh) pool_.push_back(c);
            now = pool_.size();
        }
        growing_.store(false, std::memory_order_release);

        ETCS_LOG("ConnectionManager", "pool grown " << cur << " -> " << now
                 << " (in use " << in_use_.load() << ")");
        return !fresh.empty();
    }

    // Reports what to do rather than doing it: both actions block on the
    // loader, and the caller runs them after releasing the lock.
    void rebalanceLocked(bool& want_grow, std::vector<SocketConnectionState*>& victims)
    {
        const size_t n    = pool_.size();
        const int    used = in_use_.load(std::memory_order_acquire);

        if (static_cast<size_t>(used) * 3 >= n * 2) { want_grow = true; return; }
        if (n <= kMinPool) return;
        if (static_cast<size_t>(used) * 3 >= n) return;

        const size_t target = n / 2;
        // Free entries only. A Draining or Clearing connection is skipped
        // rather than waited for -- it will be reclaimed by a later rebalance,
        // and blocking an accept on someone else's drain is the exact coupling
        // this pool exists to remove.
        for (size_t i = pool_.size(); i-- > 0 && pool_.size() > target; )
        {
            SocketConnectionState* c = pool_[i];
            if (!c || c->IsActive()) continue;
            pool_.erase(pool_.begin() + static_cast<long>(i));
            victims.push_back(c);
        }
        if (pool_cursor_ >= pool_.size()) pool_cursor_ = 0;
        if (!victims.empty())
            ETCS_LOG("ConnectionManager", "pool shrunk " << n << " -> " << pool_.size()
                     << " (in use " << used << ")");
    }

    // Module-local RID resolution. Same lookup FileHtmlPage's own
    // resolveMountTarget already performs, and same reasoning: this module's
    // own EventNode singleton holds a RIDList per type it exports, and RIDs
    // are runtime-unique so at most one list can ever hold a given one.
    // Scanning every list rather than requiring the tag means a subscriber can
    // be ANY type -- which is the point, since the manager has no business
    // knowing what consumes its connections.
    static ETCS::Entity* resolveRID(ETCS::RID rid)
    {
        if (rid == 0) return nullptr;
        auto& ridMap = ETCS::getLoader().ridMap;   // loader's map, not this
                                                    // module's -- see
                                                    // HttpServer::resolveRID
        for (auto& [key, handle] : ridMap)
            if (ETCS::Entity* e = handle.invoke_get(rid)) return e;
        return nullptr;
    }

    // "<SourceTag>.<Action>" -- the qualified form call() expects. getSourceTag()
    // returns by VALUE, so the temporary is held in a named local for its whole
    // use rather than having .c_str() read a pointer into a dead temporary.
    static ETCS::Buffer qualifiedAction(ETCS::Entity* target, const std::string& action)
    {
        const ETCS::Buffer tag_buf = target->getSourceTag();
        ETCS::Buffer out;
        out.write(tag_buf.c_str());
        out.write(".");
        out.write(action.c_str());
        return out;
    }

    // Ask a subscriber's filter whether it wants this crossing. `io` carries the
    // descriptor in and, on acceptance, the crossing's equivalence key out --
    // an EMPTY result is the decline (see Filter_'s own comment, Filter.h).
    //
    // Runs on the io_completion thread, inside onConnection: a module-side
    // entity-to-entity call() goes straight through WorkBundle with no event
    // round-trip, so it never touches the loader's ordering thread. A filter
    // that blocked on one would deadlock the accept chain exactly the way a
    // synchronous addTag from this same thread once did -- filters must stay
    // cheap, local and side-effect free.
    bool askFilter(const Subscriber& s, ETCS::Buffer& io)
    {
        ETCS::Entity* f = resolveRID(s.filter_rid);
        if (!f)
        {
            ETCS_LOG("ConnectionManager", "filter RID:" << s.filter_rid
                     << " no longer resolves -- declining for subscriber RID:" << s.rid);
            return false;
        }

        // Snapshot the probe BEFORE the call. Filter_'s own contract
        // (Filter.h) is "probe in, key out -- empty means decline", which
        // is only meaningful once AcceptsConcrete actually RAN. A target
        // resolved to the wrong entity, or given a filter_action its tag
        // does not provide, does not throw here -- call()'s own dispatch
        // logs "Tag: X does not provide requested action: Y" and simply
        // returns, leaving `io` holding exactly the probe it was never
        // given a chance to interpret. That is non-empty, so without this
        // check it reads as an ACCEPT -- exactly backwards, and silently:
        // every connection matches a filter that never ran, and gets
        // dispatched to an action the target doesn't have either (the
        // matched subscriber's own action string is equally unexamined),
        // which the receiving ModuleBundle also logs-and-drops -- so
        // nothing is served AT ALL, not even the unfiltered fallback,
        // for every single connection this gate accepts.
        //
        // A byte-identical, still non-empty `io` is the tell: any genuine
        // AcceptsConcrete either declines by emptying `io` (every Filter_
        // in this codebase does -- TarpitNode::AcceptsConcrete included)
        // or accepts by writing an actual key, which is never simply the
        // untouched probe handed in. Comparing raw bytes rather than
        // reaching for an operator== keeps this independent of whatever
        // equality Buffer may or may not define.
        const size_t probe_len = io.written;
        std::vector<char> probe_bytes(io.buf, io.buf + probe_len);

        ETCS::Buffer action = qualifiedAction(f, s.filter_action);
        try { f->call(action, io, accept_ctx_); }
        catch (const std::exception& ex)
        {
            ETCS_LOG("ConnectionManager", "filter RID:" << s.filter_rid
                     << " ." << s.filter_action << " threw: " << ex.what()
                     << " -- declining.");
            return false;
        }

        if (io.written == probe_len
            && (probe_len == 0 || std::memcmp(io.buf, probe_bytes.data(), probe_len) == 0))
        {
            ETCS_LOG("ConnectionManager", "filter RID:" << s.filter_rid
                     << " ." << s.filter_action << " left the probe untouched -- "
                        "that action likely does not exist on RID:" << s.filter_rid
                     << "'s actual tag (see the ModuleBundle log line just above,"
                        " if one printed) -- declining rather than treating an "
                        "unrun filter as a match.");
            return false;
        }

        return io.written > 0;
    }

    // Bring the outstanding-accept count back up to the window.
    void topUpAccepts()
    {
        for (int live = inflight_.load(std::memory_order_acquire);
             live < static_cast<int>(kAcceptWindow);
             ++live)
            submitAccept();
    }

    // ── Maintenance thread ────────────────────────────────────────────────
    // Two jobs, both of which need a timer and neither of which has one.
    //
    // 1. REAP IDLE CONNECTIONS. checkTimeout() is only ever called from inside
    //    do_recv, which only runs when a completion arrives. Before keep-alive
    //    that was sufficient -- every connection completed or errored within
    //    one request. Now an idle connection sits with a recv armed and
    //    nothing driving it, so a client that vanishes WITHOUT a FIN (suspended
    //    tab, closed laptop, dropped wifi) leaves that recv outstanding forever
    //    and its pool slot claimed forever. Those accumulate, the pool fills
    //    with ghosts, and every new connection is refused on arrival -- which
    //    presents as a server that is idle and unreachable at the same time.
    //
    // 2. RE-ARM A SHRUNKEN ACCEPT WINDOW. Every re-arm otherwise happens inside
    //    a completion, so a submission lost before its callback runs shrinks
    //    the window permanently and silently.
    //
    // Its own thread rather than a Timeout submission or a pool task: if the
    // ring or the pool is what broke, a watchdog living on either dies with it.
    void startMaintenance()
    {
        if (maintain_running_.exchange(true, std::memory_order_acq_rel)) return;
        maintain_thread_ = std::thread([this]
        {
            while (maintain_running_.load(std::memory_order_acquire)
                   && !stopping_.load(std::memory_order_acquire))
            {
                ETCS_SLEEP_MS(kMaintainMs);
                if (stopping_.load(std::memory_order_acquire) || listen_fd_ == -1) break;
                if (accept_ctx_.isInterrupted() || accept_ctx_.isTerminated()) break;

                reapIdle();

                const int live = inflight_.load(std::memory_order_acquire);
                if (live < static_cast<int>(kAcceptWindow))
                {
                    ETCS_LOG("ConnectionManager", "MAINT: accept window at " << live
                             << "/" << kAcceptWindow << " -- topping up (total re-arms "
                             << ++rearms_ << ")");
                    topUpAccepts();
                }
            }
        });
    }

    // Snapshot under the lock, act outside it: checkTimeout can Reset, which
    // submits a cancel and can run finalize, and none of that should happen
    // while an accept is blocked waiting for pool_mutex_.
    void reapIdle()
    {
        std::vector<SocketConnectionState*> live;
        {
            std::lock_guard<std::mutex> lock(pool_mutex_);
            for (SocketConnectionState* c : pool_)
                if (c && c->IsActive()) live.push_back(c);
        }
        int reaped = 0;
        for (SocketConnectionState* c : live) if (c->checkTimeout()) ++reaped;
        if (reaped)
            ETCS_LOG("ConnectionManager", "MAINT: reaped " << reaped
                     << " idle connection(s) past " << SocketConnectionState::TIMEOUT_SECONDS << "s");

        // STUCK REPORT. A connection that is IsActive() but no longer open
        // is mid-drain: Reset has run, but finalizeIfDraining has not, which
        // means io_inflight_ has not reached zero. That is NORMAL for a tick
        // or two while a cancel surfaces the outstanding op as -ECANCELED --
        // and PERMANENT if a reference was leaked, because nothing else will
        // ever drive that count down and the connection's pool slot is gone
        // for the process's lifetime.
        //
        // The two cases are indistinguishable from any other log line this
        // module writes, which is exactly why this exists: it prints the
        // one number that separates them (how many references are actually
        // outstanding) alongside the RID, so a leak can be attributed to a
        // specific connection and a specific count rather than inferred
        // from a pool that quietly stops recycling. Deliberately rate
        // limited -- a stuck connection is stuck forever, so reporting it
        // every tick would reproduce precisely the unbounded-repetition
        // problem checkTimeout's own Serving guard just removed.
        if (++stuck_report_tick_ >= kStuckReportTicks)
        {
            stuck_report_tick_ = 0;
            for (SocketConnectionState* c : live)
                if (!c->IsConnectionOpen())
                    ETCS_LOG("ConnectionManager", "MAINT: connection RID:" << c->getRID()
                             << " is still draining with " << c->InflightCount()
                             << " io reference(s) outstanding -- if this count never "
                                "reaches 0 it is a LEAKED reference, and this "
                                "connection's pool slot is gone for good.");
        }
    }

    void submitAccept(int retry = 0)
    {
        if (stopping_.load(std::memory_order_acquire) || listen_fd_ == -1) return;
        // accept_ctx_, not open_ctx_ -- it inherits open_ctx_ through its own
        // parent link (Scope::registerContext), so this still sees every
        // signal from above WHILE additionally answering to `kill accept 0`.
        // Checking open_ctx_ here was what made that kill inert: the flag was
        // set on a context nothing consulted.
        if (accept_ctx_.isInterrupted() || accept_ctx_.isTerminated()) return;

        inflight_.fetch_add(1, std::memory_order_acq_rel);

        ETCS::IOSubmission sub;
        sub.op       = ETCS::IOOp::Accept;
        sub.fd       = listen_fd_;
        sub.priority = static_cast<int>(ETCS::Priority::High);
        sub.ctx      = accept_ctx_;

        // Registered against THIS entity, and moved into the callback -- the
        // same construct-before-submit-then-move discipline
        // DEFINE_STREAM_FUNC_PRODUCE uses (ETCS_API.h). Without it there is a
        // window between submitting and the completion running where nothing
        // is registered to protect this entity at all, and a teardown checking
        // for in-flight scopes would see none and proceed. shared_ptr because
        // std::function requires a copy-constructible target and ScopeTag's
        // copy ctor is deleted by design.
        // No per-submission ScopeTag anymore -- accept_scope_ covers the whole
        // chain (see its own comment). `this` is safe to capture bare for the
        // same reason that scope exists: CloseConcrete drains inflight_ to
        // zero before releasing it, so no completion can outlive the entity.
        sub.callback = [this](ETCS::IOCompletion comp) mutable
        {
            // Decrement FIRST, on every path out -- Close's drain below can
            // only reach zero if every submission counts itself done
            // regardless of outcome.
            inflight_.fetch_sub(1, std::memory_order_acq_rel);

            // An interrupt on the accept scope means the chain is over, and
            // since the chain is the only thing keeping accept_scope_ alive,
            // the last submission out is what has to release it. Otherwise
            // `kill accept 0` leaves a scope registered forever, marked
            // stopping, with nothing left to stop -- which is what the shell
            // was showing.
            //
            // Distinct from Close(): this stops accepting and lets the gate
            // stay open. Close() releases the descriptor. Collapsing the two
            // would make the finer verb unavailable, and stopping-without-
            // closing is exactly what a fine-grained kill is for.
            if (accept_ctx_.isInterrupted() || accept_ctx_.isTerminated())
            {
                if (comp.result >= 0) ::close(comp.result); // accepted mid-stop
                ETCS_LOG("ConnectionManager", "accept chain interrupted -- closing gate "
                         "(RID:" << getRID() << ").");
                {
                    std::vector<std::pair<ETCS::Buffer, ETCS::RID>> kids;
                    getTypedChildren(kids);
                    ETCS::MemoryArena& a = getArena();
                    ETCS::MemoryArena& g = ETCS::MemoryArena::getInstance();
                    ETCS_LOG("CM.mem", "manager arena: capacity=" << a.getCapacity()
                             << " usage=" << a.getUsage()
                             << " dtor_records=" << a.getDtorRecordCount()
                             << " children=" << kids.size()
                             << " | MODULE ROOT: capacity=" << g.getCapacity()
                             << " usage=" << g.getUsage()
                             << " dtor_records=" << g.getDtorRecordCount());
                }
                CloseConcrete();
                return;
            }

            if (comp.result < 0)
            {
                submitAccept(); // transient error -- re-arm; submitAccept's own
                                // guards handle the shutting-down case
                return;
            }

            int fd = comp.result;

            if (stopping_.load(std::memory_order_acquire)
                || accept_ctx_.isInterrupted() || accept_ctx_.isTerminated())
            {
                ::close(fd); // accepted after shutdown began -- nothing left to serve it
                return;
            }

            // TOP UP to the full window, do not just add one back. One-for-one
            // re-arming means the window can only recover at completion rate,
            // which is exactly the rate that was too slow to drain the backlog
            // in the first place -- a burst that empties the window keeps it
            // empty. Topping up pulls the queue down as fast as the ring allows.
            topUpAccepts();
            ETCS_LOG("ConnectionManager", "Usage pre onConnection(fd)=" << getGlobalArena().getUsage());
            onConnection(fd);
            ETCS_LOG("ConnectionManager", "Usage post onConnection(fd)=" << getGlobalArena().getUsage());
        };
        if (!ETCS::ThreadPool::getInstance().submit(std::move(sub)))
        {
            inflight_.fetch_sub(1, std::memory_order_acq_rel);
            if (retry >= 200)
            {
                ETCS_LOG("ConnectionManager", "CRITICAL: accept submission refused "
                         << retry << " times -- gate RID:" << getRID()
                         << " is no longer accepting.");
                return;
            }
            ETCS_SLEEP_MS(1);
            submitAccept(retry + 1);
        }
    }

    // Mint the child, then publish it. The connection becomes a real, listable,
    // killable entity the instant it is accepted -- not whenever some consumer
    // eventually claims it.
    //
    // On a TLS gate, a freshly claimed connection owes a HANDSHAKE before
    // it owes a subscriber anything -- dispatchToSubscribers (below) must
    // never see a connection whose bytes are still ciphertext, since neither
    // HttpServer nor any other subscriber has ever been taught TLS exists
    // (the carry-forward doc's whole point: termination happens HERE, before
    // dispatch, so every subscriber keeps seeing plaintext forever). So this
    // function now does one of two things with the reference acquireConnection
    // (via TryClaim) seeded onto conn->io_inflight_: either it drives that
    // reference straight into dispatchToSubscribers's own trailing
    // NoteComplete() (plaintext, unchanged), or it hands the reference to
    // DriveTLSHandshake and lets ITS bool-return contract (TLSConnectionIO.h's
    // own top-of-file comment) decide who releases it and when.
    void onConnection(int fd)
    {
        SocketConnectionState* conn = acquireConnection(fd);
        if (!conn) { ::close(fd); return; }

        // TryClaim seeded exactly one io_inflight_ reference -- the dispatch
        // reference. This owns it until something downstream takes it over.
        ConnRef entry = ConnRef::Wrap(conn);

        ETCS_LOG("ConnectionManager", "Accepted fd=" << fd
                 << " RID:" << conn->getRID() << " on port " << port_);

        // One copy, taken once, for this connection's whole session. If a
        // ReloadCerts lands while this handshake is in flight, this
        // connection finishes against the config it started with rather
        // than half of each.
        if (std::shared_ptr<TLSServerConfig> cfg = currentTLSConfig())
        {
            if (!conn->GetTLS().Init(std::move(cfg)))
            {
                ETCS_LOG("ConnectionManager", "TLS Init failed for fd=" << fd
                         << " RID:" << conn->getRID() << " -- dropping.");
                conn->Reset();
                return;                 // entry releases the dispatch reference
            }

            // DriveTLSHandshake's own bool return: true means the entry
            // reference it was handed (the one acquireConnection seeded) is
            // ALREADY fully accounted for -- either the handshake resolved
            // synchronously and on_settled already ran to completion, or it
            // never will resolve synchronously but the reference was still
            // consumed on this path -- so `entry` disarms. false means a new
            // async ciphertext IO is outstanding and the entry reference is
            // spare, which is exactly what leaving `entry` armed settles on
            // return. See TLSConnectionIO.h's top-of-file contract.
            // Counted BEFORE the drive, released on the single settled
            // path below -- see tls_handshakes_'s own comment for why
            // CloseConcrete has to wait on this separately from inflight_.
            tls_handshakes_.fetch_add(1, std::memory_order_acq_rel);
            bool settled = TLSIO::DriveTLSHandshake(conn, this, accept_ctx_,
                [this, conn](bool ok)
                {
                    // Invoked holding a live reference, like every
                    // continuation in this module.
                    ConnRef ref = ConnRef::Wrap(conn);
                    tls_handshakes_.fetch_sub(1, std::memory_order_acq_rel);
                    if (ok) { ref.disarm(); beginDispatch(conn); }
                    else    { conn->Reset(); }   // ref releases
                });
            if (settled) entry.disarm();
            return;
        }

        entry.disarm();     // beginDispatch takes the dispatch reference
        beginDispatch(conn);
    }

    // Whether ANY registered subscriber is filtered. This is the sole gate on
    // whether preParseThenDispatch ever runs at all: with no filtered
    // consumer registered, a path-aware pre-parse would be pure unrequested
    // cost -- one extra recv and one extra parse per connection, paid by a
    // deployment that never asked for path-aware gate-level filtering.
    //
    // A relaxed atomic load, not a locked scan -- this runs once per
    // ACCEPTED CONNECTION, right next to the accept syscall itself, so it is
    // squarely in the hot path onConnection already documents as
    // performance-sensitive (see submitAccept's own comments on why the
    // accept window exists at all). Locking subs_mutex_ here would also
    // contend with RegisterConsumer/UnregisterConsumer on every accept, for
    // a fact (whether any filtered subscriber exists) that changes on the
    // order of "a script ran a config action", not per-connection.
    // filtered_subscriber_count_ is kept exactly in step with subscribers_
    // itself at every mutation site -- see its own comment for why a
    // relaxed read here is still safe.
    bool hasFilteredSubscriber() const
    {
        return filtered_subscriber_count_.load(std::memory_order_relaxed) > 0;
    }

    // The single entry point onConnection now calls instead of
    // dispatchToSubscribers directly. Byte-for-byte identical behavior to
    // before this existed, for the common case: no filtered subscriber means
    // no pre-parse, straight to dispatchToSubscribers exactly as always.
    // Only once a script has actually registered a filtered gate-level
    // consumer (a tarpit or similar) does this pay for a pre-parse -- and
    // only then, because only then does askFilter have anything to gain from
    // one: an unfiltered subscriber's own action (HttpServer::Serve) does its
    // own recv/parse itself and has never needed this.
    void beginDispatch(SocketConnectionState* conn)
    {
        if (!hasFilteredSubscriber())
        {
            // Worth a line rather than a silent branch. This is the single
            // point at which path-aware gate-level filtering is skipped for
            // a connection, and "no filtered subscriber is registered" looks
            // exactly like "the filter ran and declined" from anywhere
            // downstream -- both end with the ordinary handler answering
            // normally. A tar-pit that is registered against the wrong
            // entity, or not registered at all, is invisible without this.
            ETCS_LOG("ConnectionManager", "no filtered subscriber registered -- "
                     "dispatching connection RID:" << conn->getRID()
                     << " without a pre-parse.");
            dispatchToSubscribers(conn);
            return;
        }
        preParseThenDispatch(conn);
    }

    // ConnectionManager's own top-of-file comment says it plainly: "It does
    // NOT interpret connections." This is the one deliberate, narrow
    // exception, and it earns that exception on two grounds. First, it is
    // gated behind hasFilteredSubscriber() above, so the protocol-agnostic
    // default this type has always offered is completely unchanged unless a
    // script opts in by registering a filtered consumer. Second, this module
    // only ever serves HTTP in practice -- PicoHTTPParser is its only
    // Parser_ -- so reading enough bytes to ask "what path is this" is not
    // actually assuming more about the protocol than the rest of this module
    // already does.
    //
    // Why this has to exist at all: HttpServer::DispatchRoute's own
    // route-level Filter_ call (chess, forum) already sees the path, because
    // by the time it runs, HttpServer::Serve's own recv has already read and
    // parsed the request. askFilter's own gate-level call, by contrast, has
    // always run at ACCEPT time -- before a single byte is read -- which is
    // why its probe has only ever carried (manager_rid, conn_rid), never a
    // path. A gate-level filter that wants to match on path needs the parse
    // to have already happened before askFilter runs, and this is where
    // that happens.
    //
    // The recv/parse loop itself is ReadUntilParsed (ConnectionRecvLoop.h),
    // shared with HttpServer::Serve rather than a second copy of it living
    // here -- the two callers want exactly the same thing (read bytes until
    // one request is fully parsed) for the same reason, so a change to
    // either one's needs belongs in one place. What stays HERE, and does
    // NOT move into the shared loop, is only "what to do once parsed":
    // handing the connection to dispatchToSubscribers, so a filtered
    // consumer's own Filter_ (TarpitNode::AcceptsConcrete, say) can finally
    // see a path.
    void preParseThenDispatch(SocketConnectionState* conn)
    {
        ReadUntilParsed(conn, this, accept_ctx_,
            [this](SocketConnectionState* c) { dispatchToSubscribers(c); });
    }

    // The plaintext subscriber-dispatch body onConnection always ran before
    // TLS existed, extracted verbatim so the plaintext path (no config
    // installed) is byte-for-byte unchanged. Called either directly from
    // onConnection (plaintext) or from a TLS handshake's on_settled callback
    // once DriveTLSHandshake reports success (TLSConnectionIO.h) -- in both
    // cases the caller hands over exactly the one io_inflight_ reference
    // that this function's own `entry` ConnRef releases.
    void dispatchToSubscribers(SocketConnectionState* conn)
    {
        // The dispatch reference, owned for this whole function. This is the
        // contract HttpServer::Serve broke once by consuming it a second
        // time (see NetworkProvider.h) -- expressed as ownership rather than
        // as a rule, it is not something a subscriber can quietly spend.
        ConnRef entry = ConnRef::Wrap(conn);

        // Snapshot under the lock, dispatch outside it: a subscriber's action
        // can do anything, including registering or unregistering, and holding
        // this lock across that would be a self-deadlock waiting to happen.
        std::vector<Subscriber> snapshot;
        {
            std::lock_guard<std::mutex> lock(subs_mutex_);
            snapshot = subscribers_;
        }

        if (snapshot.empty())
        {
            ETCS_LOG("ConnectionManager", "No subscribers -- dropping connection RID:"
                     << conn->getRID() << ".");
            conn->Reset();
            return;                 // entry releases the dispatch reference
        }

        // Payload is the connection's own RID: the subscriber is out-of-tree,
        // so this is a genuine causal crossing and a weak reference is exactly
        // right. The subscriber re-enters this manager's strong domain by
        // resolving it back through getTypedChild.
        ETCS::Buffer frame;
        frame << getRID();
        frame << conn->getRID();

        std::vector<ETCS::RID> dead;
        bool delivered = false;

        // FIRST MATCH WINS, filtered before unfiltered, registration order
        // within each pass. A connection has exactly one owner: HttpServer::Serve
        // drives recv/parse/send on the fd and deletes the connection when done,
        // so two subscribers receiving the same fd would corrupt each other's
        // parse. If a target wants to fan out, that is the target's business --
        // this gate publishes each crossing once.
        //
        // Filtered first so an unfiltered subscriber (which accepts everything)
        // is a FALLBACK rather than a swallower: register the general handler
        // however early you like, and a later filtered one still gets its own
        // traffic. Within a pass, earlier registration wins.
        auto try_deliver = [&](const Subscriber& s) -> bool
        {
            ETCS::Entity* target = resolveRID(s.rid);
            if (!target) { dead.push_back(s.rid); return false; }

            ETCS::Buffer key;
            if (s.filtered())
            {
                ETCS::Buffer probe = frame;   // descriptor in, key out
                if (!askFilter(s, probe)) return false;
                key = probe;
            }

            ETCS::Buffer action = qualifiedAction(target, s.action);
            ETCS_LOG("ConnectionManager", "dispatching: action='" << action.toString()
                     << "' to RID:" << s.rid
                     << (s.filtered() ? " (matched key '" + key.toString() + "')"
                                      : std::string(" (unfiltered)")));

            ETCS::Buffer payload = frame;
            try { target->call(action, payload, accept_ctx_); }
            catch (const std::exception& ex)
            {
                ETCS_LOG("ConnectionManager", "Subscriber RID:" << s.rid
                         << " ." << s.action << " threw: " << ex.what());
                return false;
            }
            return true;
        };

        for (const auto& s : snapshot)
            if (s.filtered() && try_deliver(s)) { delivered = true; break; }
        if (!delivered)
            for (const auto& s : snapshot)
                if (!s.filtered() && try_deliver(s)) { delivered = true; break; }

        // Self-healing: a subscriber that no longer resolves is gone for good,
        // so drop it rather than rescanning for it on every future connection.
        if (!dead.empty())
        {
            std::lock_guard<std::mutex> lock(subs_mutex_);
            for (ETCS::RID r : dead)
                for (auto it = subscribers_.begin(); it != subscribers_.end(); )
                {
                    if (it->rid != r) { ++it; continue; }
                    // Kept in lockstep with subscribers_, same as
                    // RegisterConsumer/UnregisterConsumer -- see
                    // filtered_subscriber_count_'s own comment.
                    if (it->filtered())
                        filtered_subscriber_count_.fetch_sub(1, std::memory_order_relaxed);
                    it = subscribers_.erase(it);
                }
        }

        if (!delivered)
        {
            ETCS_LOG("ConnectionManager", "No live subscriber took connection RID:"
                     << conn->getRID() << " -- recycling it.");
            conn->Reset();
        }

        // Dispatch is over: whatever Serve submitted now holds its own
        // references, and this one must go or the connection never drains.
        // Released HERE rather than at scope exit because the log below
        // reports in_use_, which this release can decrement.
        entry.release();

        // getTypedChildren walked and copied the whole child list under the
        // lock addTag and getTypedChild both need -- O(live connections) of
        // lock-held work on every accept, to log a count. pool_.size() is the
        // same number.
        ETCS_LOG("CM.mem", "pool=" << pool_.size() << " in_use=" << in_use_.load());
    }
};

#endif // CONNECTIONMANAGER_H__
