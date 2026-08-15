#ifndef CONNECTIONMANAGER_H__
#define CONNECTIONMANAGER_H__
#include "../../../ontology.h"
#include "SocketConnectionState.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <vector>
#include <string>
#include <mutex>
#include <memory>

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

        // The close is what actually unparks anything waiting on this fd and
        // is the Gate_ contract's real obligation (see Gate.h). The cancel is
        // an optimisation on top -- it lets the pool retire an outstanding
        // accept promptly instead of discovering the closed fd on its own.
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
        if (!ETCS::MemoryArena::getInstance().isTearingDown())
        {
            ETCS::IOSubmission cancel;
            cancel.op  = ETCS::IOOp::Cancel;
            cancel.fd  = listen_fd_;
            cancel.ctx = open_ctx_;
            ETCS::ThreadPool::getInstance().submit(std::move(cancel));
        }

        ::close(listen_fd_);
        listen_fd_ = -1;

        // Drain: do not report closed while a completion could still fire and
        // touch this entity. Bounded rather than unbounded -- a stuck pool
        // should surface as a loud warning, not a hang in a destructor.
        //
        // ETCS_SLEEP_MS's return is deliberately ignored: false means the
        // sleep was cut short by a signal (EINTR), which is not a reason to
        // stop waiting for an in-flight completion. The only thing that ends
        // this loop early is the count reaching zero; the ceiling is the only
        // other exit. Conflating the two would let a stray signal report a
        // clean close while a completion was still pending.
        int retries = 0;
        while (inflight_.load(std::memory_order_acquire) > 0)
        {
            // The drain is only meaningful while the pool can still run
            // completions. At process exit ThreadPool's own static destructor
            // may already have joined its io and worker threads -- ordering
            // between two independent Meyers singletons is unspecified, and
            // the observed sequence is precisely that: pool drained, THEN this
            // Close reached, with one accept still outstanding that nothing
            // was left alive to complete. Waiting there spins the full ceiling
            // for nothing, on every exit.
            //
            // Breaking out is safe rather than a concession: if the pool is
            // drained, no callback can be running or ever start, which is the
            // exact property the wait exists to establish.
            if (ETCS::ThreadPool::getInstance().isDrained())
            {
                ETCS_LOG("ConnectionManager", "Close: pool already drained -- "
                         << inflight_.load() << " submission(s) can no longer "
                            "complete; skipping the wait.");
                break;
            }

            ETCS_SLEEP_MS(10);
            if (++retries > 500) // 5s ceiling
            {
                ETCS_LOG("ConnectionManager", "Close: " << inflight_.load()
                         << " accept submission(s) still in flight after 5s -- "
                            "proceeding anyway.");
                break;
            }
        }

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
            it = (it->rid == rid) ? subscribers_.erase(it) : it + 1;
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

private:
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
    static constexpr size_t kAcceptWindow = 8;
    // Was 8. The backlog is what absorbs a burst arriving faster than the
    // window can retire; at 8 it was the binding limit under load.
    static constexpr int    kBacklog      = 128;

    int                 listen_fd_ = -1;
    int                 port_      = 0;
    std::atomic<int>    inflight_{0};
    std::atomic<bool>   stopping_{false};

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
        ETCS::Buffer action = qualifiedAction(f, s.filter_action);
        try { f->call(action, io, accept_ctx_); }
        catch (const std::exception& ex)
        {
            ETCS_LOG("ConnectionManager", "filter RID:" << s.filter_rid
                     << " ." << s.filter_action << " threw: " << ex.what()
                     << " -- declining.");
            return false;
        }
        return io.written > 0;
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

            submitAccept();
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
    void onConnection(int fd)
    {
        SocketConnectionState* conn = acquireConnection(fd);
        if (!conn) { ::close(fd); return; }

        ETCS_LOG("ConnectionManager", "Accepted fd=" << fd
                 << " RID:" << conn->getRID() << " on port " << port_);

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
            conn->NoteComplete();   // release the dispatch reference
            return;
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
                    it = (it->rid == r) ? subscribers_.erase(it) : it + 1;
        }

        if (!delivered)
        {
            ETCS_LOG("ConnectionManager", "No live subscriber took connection RID:"
                     << conn->getRID() << " -- recycling it.");
            conn->Reset();
        }

        // Dispatch is over: whatever Serve submitted now holds its own
        // references, and this one must go or the connection never drains.
        conn->NoteComplete();

        // getTypedChildren walked and copied the whole child list under the
        // lock addTag and getTypedChild both need -- O(live connections) of
        // lock-held work on every accept, to log a count. pool_.size() is the
        // same number.
        ETCS_LOG("CM.mem", "pool=" << pool_.size() << " in_use=" << in_use_.load());
    }
};

#endif // CONNECTIONMANAGER_H__
