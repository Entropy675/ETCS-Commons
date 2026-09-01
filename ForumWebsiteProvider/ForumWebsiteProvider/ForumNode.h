#ifndef FORUMNODE_H__
#define FORUMNODE_H__
#include "ForumSelf.h"
#include <mutex>

// ── ForumNode: the host ───────────────────────────────────────────────────────
// Owns the selves and the threads, does all path parsing, and holds the ONE
// edge that leaves this module: the database.
//
// This is the CENTRALIZATION ARTIFACT, named as one for the same reason
// ChessNode is: a node hosting many selves is a forum server, and a node
// hosting exactly one self is a peer. Nothing else about the arrangement
// changes between those two cases -- same types, same edges, same routing -- so
// the P2P transition is a change of cardinality plus a change in what an edge
// resolves to, not a restructure.
//
// Path: /<mount>/<self>/<thread>/<verb>[/<rest...>]
// Self comes FIRST because it is the segment that becomes implicit when the
// request arrives at your own machine: a peer's own URL is just
// /<thread>/<verb>.
//
// ── The database edge, and why it is a RID ────────────────────────────────────
// This module must stay loadable with no DatabaseProvider present, exactly as
// ChessProvider stays loadable with no NetworkProvider present. So nothing here
// includes, links against, or names a DatabaseProvider TYPE. The connection is
// a bare ETCS::RID handed in by the script, resolved through the loader's own
// ridMap per call, and invoked by qualified action name.
//
// That is the same mechanism HttpServer::DispatchRoute uses to reach a route
// target in another module, and for the same reason: the loader's EventNode is
// the only map that sees every module, since this module's own per-DSO
// singleton holds only the types this module exports.
//
// The cost is real and stated rather than hidden: the call is by NAME, so a
// rename on the database side is a runtime miss and not a compile error. The
// compensation is that a missing database degrades to a forum that serves fine
// and persists nothing, which is the correct failure mode for an optional edge.
class ForumNode :
    public DeletableBase<ForumNode>,
    public FilterBase<ForumNode>
{
    friend struct ForumStream;
    friend class  ForumThread;
    friend class  ForumSelf;

public:
    WIRE_TYPE_IDENTITY(ForumNode);

    using Kind  = ForumInEvent::Kind;
    using Clock = ForumThread::Clock;

    ForumNode()          = default;
    virtual ~ForumNode() = default;

    // ── THE ordering domain ───────────────────────────────────────────────
    // One stream per node, not one per module and not one per thread.
    //
    // Per-module asserts a causal relation between unrelated conversations and
    // charges head-of-line blocking proportional to total traffic. Per-thread
    // would be the finest correct domain in isolation, but this node touches
    // every thread it hosts -- renderIndexLocked, renderVerifyLocked and
    // reapLocked all walk the registry -- so a boundary between node and
    // threads would turn every one of those into an unsynchronised cross-domain
    // read, requiring a published read model to repair. Putting the boundary AT
    // the node means they stay plain calls on one thread and no read model is
    // needed.
    //
    // The justification that survives the P2P transition is NOT "a host is
    // shared between participants" -- in the peer case it is not shared, each
    // participant runs their own. It is that a node is ONE PARTICIPANT'S LOCAL
    // ORDERING DOMAIN, and therefore also the unit a remote peer syncs against:
    // replay is node-to-node, so the sync unit and the ordering domain have to
    // be the same object or a peer following three threads needs three
    // channels.
    //
    // It follows that contention is proportional to how centralized the
    // deployment is: one self on the node, none; N selves, N-way. The
    // centralization claim becomes a latency term you can measure rather than a
    // paragraph asserting it.
    //
    // Started lazily under call_once rather than in the constructor: two pool
    // threads can reach a freshly created node concurrently, and start() is not
    // idempotent. Lazy also keeps a node that is configured but never used from
    // costing a thread.
    ForumStream& stream() const
    {
        std::call_once(stream_started_, [this]
        {
            stream_.start(ETCS::MemoryArena::getInstance());
        });
        return stream_;
    }

    // One filter for everything under the mount. Deliberately NOT routed
    // through the stream: it runs for every request on the server, including
    // paths that turn out not to be ours, and a round trip would serialize all
    // path matching behind forum logic.
    bool AcceptsConcrete(ETCS::Buffer& io) const
    {
        const std::string desc = io.restAsString();
        size_t i = 0;
        while (i < desc.size() && desc[i] == '/') ++i;
        size_t j = desc.find('/', i);
        if (j == std::string::npos) j = desc.size();
        if (desc.compare(i, j - i, mount_) != 0) { io.reset(); return false; }
        io.writeString(mount_.c_str());
        return true;
    }

    // ── Public surface: serialized on the shared forum stream ─────────────
    std::string Request(const std::string& path) { return op(Kind::NodeRequest, path); }
    std::string Index(const std::string& c = "")  const { return op(Kind::NodeIndex,  c); }
    std::string Selves(const std::string& c = "") const { return op(Kind::NodeSelves, c); }
    std::string LoadRow(const std::string& row)   { return op(Kind::NodeLoadRow, row); }
    std::string InitDb()                          { return op(Kind::NodeFlush, "init"); }
    std::string Verify(const std::string& c = "") const { return op(Kind::NodeFlush, "verify:" + c); }

    const std::string& MountPath() const { return mount_; }
    void SetMount(const std::string& m)  { mount_ = m; }   // setup only

    // Set before anything that would persist. Zero means "no database", which
    // is a supported configuration and not an error: the forum runs, and the
    // threads live exactly as long as the process does.
    void SetDb(ETCS::RID rid, const std::string& action = "ExecuteRaw")
    {
        db_rid_ = rid;
        if (!action.empty()) db_action_ = action;
        ETCS_LOG("ForumNode", "database edge set to RID:" << db_rid_
                 << " ." << db_action_ << (db_rid_ ? "" : " (persistence DISABLED)"));
    }
    ETCS::RID DbRID() const { return db_rid_; }

    // The context persisted calls are made under. Captured on every request
    // that carries one (see the work functions), so a write triggered by
    // reaping -- which has no request of its own -- still runs under the last
    // authority that actually asked for something.
    void SetRunContext(const ETCS::SignalContext& ctx) { run_ctx_ = ctx; have_ctx_ = true; }

    bool DeleteConcrete()
    {
        std::string conjugate_key = this->getSourceModule().toString() + ":"
                                  + this->getSourceTag().toString();
        ETCS_LOG("ForumNode", "Delete: firing self-DestroyEvent for RID:"
                 << getRID() << " (" << conjugate_key << ")");
        ETCS::DestroyEvent{conjugate_key.c_str(), this, true}();
        return true;
    }

private:
    // A session is a session; a thread is the record. Selves expire, threads do
    // not -- see ForumSelf's own comment for why that asymmetry is the reason
    // this module has a database at all.
    static constexpr int    kSession    = 900;   // self dropped this long after last request
    static constexpr int    kReaderIdle = 60;    // counted as "here" for this long
    static constexpr size_t kMaxThreads = 512;   // in-memory cap, see createThreadLocked
    static constexpr size_t kMaxIdent   = 48;    // a token is an identifier, not a payload
    static constexpr size_t kDbChunk    = 64;    // body bytes per forum_body row
    static constexpr size_t kSqlMax     = 240;   // one statement must fit one Buffer

    std::string op(Kind k, const std::string& arg = "") const
    {
        return ForumOpEvent{&stream(), k, const_cast<ForumNode*>(this), arg}();
    }

    // ══ Cross-module invocation ═══════════════════════════════════════════
    // Both of these are lifted from HttpServer, deliberately by COPY rather
    // than by include: they touch nothing but core ETCS API, so duplicating
    // twenty lines is cheaper than making this module depend on NetworkProvider
    // to reach DatabaseProvider. The dependency that would create is exactly
    // the one the copy avoids.

    // RIDs are runtime-unique, so at most one list can hold a given one and the
    // first hit is the only hit. getLoader(), not EventNode::getInstance():
    // from module-compiled code the latter is this DSO's own singleton, whose
    // ridMap holds only the types this module exports.
    static ETCS::Entity* resolveRID(ETCS::RID rid)
    {
        if (rid == 0) return nullptr;
        auto& ridMap = ETCS::getLoader().ridMap;
        for (auto& [key, handle] : ridMap)
            if (ETCS::Entity* e = handle.invoke_get(rid)) return e;
        return nullptr;
    }

    // "<SourceTag>.<Action>". getSourceTag() returns by VALUE, so the temporary
    // is held in a named local rather than having .c_str() dangle off the call.
    static ETCS::Buffer qualifiedAction(ETCS::Entity* target, const std::string& action)
    {
        const ETCS::Buffer tag_buf = target->getSourceTag();
        ETCS::Buffer out;
        out.write(tag_buf.c_str());
        out.write(".");
        out.write(action.c_str());
        return out;
    }

    // ── Ordering thread only, from here down ──────────────────────────────

    // Fire one statement at the database.
    //
    // TRADEOFF, stated because it is the one thing in this module that could
    // surprise someone reading a flame graph: this runs ON the ordering thread
    // and blocks it for the duration of an sqlite exec. Every other post in the
    // module waits behind a disk write.
    //
    // Accepted rather than solved, for now, because the alternative -- queueing
    // statements and draining them from a second thread -- means a post can be
    // acknowledged before it is durable, and a forum that loses posts on a
    // crash is not a forum. The chain makes that loss LOUD (the head a client
    // saw would no longer exist) but loud is not the same as fine.
    //
    // The right fix when this bites is a second EventStream owning the database
    // edge, with the post path emitting to it and the acknowledgement deferred
    // until it completes. That is a genuine restructure, not a tweak, which is
    // why it is named here instead of half-built.
    bool dbExecLocked(const std::string& sql) const
    {
        if (db_rid_ == 0) return false;
        if (!have_ctx_)
        {
            ETCS_LOG("ForumNode", "no signal context captured yet -- skipping persist.");
            return false;
        }
        if (sql.size() >= kSqlMax)
        {
            // Never silently truncate a statement: a half-written INSERT is
            // either a syntax error or, worse, a valid statement that stores
            // the wrong thing.
            ETCS_LOG("ForumNode", "statement of " << sql.size()
                     << " bytes exceeds the " << kSqlMax
                     << "-byte wire budget -- DROPPED: " << sql.substr(0, 80));
            return false;
        }

        ETCS::Entity* target = resolveRID(db_rid_);
        if (!target)
        {
            ETCS_LOG("ForumNode", "database RID:" << db_rid_
                     << " no longer resolves -- persistence is now a no-op.");
            return false;
        }

        ETCS::Buffer payload;
        payload.writeString(sql.c_str());
        ETCS::Buffer action = qualifiedAction(target, db_action_);
        try { target->call(action, payload, run_ctx_); }
        catch (const std::exception& ex)
        {
            ETCS_LOG("ForumNode", "database call threw: " << ex.what());
            return false;
        }
        return true;
    }

    // Double single quotes. sqlite3_mprintf("%Q") would be the right tool and
    // is what DatabaseProvider itself uses, but reaching it means linking
    // sqlite into this module, which would make a forum depend on a database
    // engine at LINK time to avoid depending on it at RUNTIME -- exactly
    // backwards. Every value that reaches here has already been flattened of
    // control characters, so quote-doubling is the whole of the escaping needed.
    static std::string sqlQuote(const std::string& s)
    {
        std::string out = "'";
        for (char c : s) { if (c == '\'') out += '\''; out += c; }
        out += "'";
        return out;
    }

    // The schema, owned by the module that owns the shape. Issued as separate
    // statements rather than handed to InitializeSchema as one blob, because
    // the blob would not fit a single Buffer -- the same wire budget that
    // shapes the read path shapes the DDL.
    //
    // Bodies live in their OWN table, chunked. Not normalization for its own
    // sake: a 512-byte post cannot ride in a 240-byte INSERT, so the choice is
    // chunked rows or a shorter maximum post, and chunked rows keep the cap a
    // property of the wire rather than of what anyone is allowed to say.
    void initSchemaLocked() const
    {
        static const char* kDDL[] = {
            "CREATE TABLE IF NOT EXISTS forum_thread ("
                "tkey TEXT PRIMARY KEY, title TEXT, op TEXT, "
                "locked INTEGER, created INTEGER)",
            "CREATE TABLE IF NOT EXISTS forum_post ("
                "tkey TEXT, seq INTEGER, author TEXT, made INTEGER, tomb INTEGER, "
                "chash TEXT, ihash TEXT, PRIMARY KEY (tkey, seq))",
            "CREATE TABLE IF NOT EXISTS forum_body ("
                "tkey TEXT, seq INTEGER, part INTEGER, chunk TEXT, "
                "PRIMARY KEY (tkey, seq, part))"
        };
        for (const char* stmt : kDDL) dbExecLocked(stmt);
        ETCS_LOG("ForumNode", "schema issued to RID:" << db_rid_);
    }

    void persistThreadHeaderLocked(const ForumThread* t) const
    {
        if (db_rid_ == 0 || !t) return;
        std::string sql = "INSERT OR REPLACE INTO forum_thread VALUES (";
        sql += sqlQuote(t->thread_key_) + ",";
        sql += sqlQuote(t->title_)      + ",";
        sql += sqlQuote(t->op_)         + ",";
        sql += (t->locked_ ? "1," : "0,");
        sql += std::to_string(static_cast<int64_t>(std::time(nullptr))) + ")";
        dbExecLocked(sql);
    }

    // One post, plus its body chunks, in one transaction. Nine autocommits per
    // post would be nine fsyncs; one transaction is one.
    void persistPostLocked(const ForumThread* t, const ForumPost& p) const
    {
        if (db_rid_ == 0 || !t) return;

        dbExecLocked("BEGIN");

        std::string sql = "INSERT OR REPLACE INTO forum_post VALUES (";
        sql += sqlQuote(t->thread_key_) + ",";
        sql += std::to_string(p.seq)    + ",";
        sql += sqlQuote(p.author)       + ",";
        sql += std::to_string(p.made)   + ",";
        sql += (p.tomb ? "1," : "0,");
        sql += sqlQuote(ForumHash::hex16(p.chash)) + ",";
        sql += sqlQuote(ForumHash::hex16(p.ihash)) + ")";
        dbExecLocked(sql);

        // A tombstone REMOVES the body rows and keeps the post row. That is the
        // storage-level statement of the same thing the render says: the
        // content is gone, the record that content of this hash was here is not.
        std::string del = "DELETE FROM forum_body WHERE tkey=" + sqlQuote(t->thread_key_)
                        + " AND seq=" + std::to_string(p.seq);
        dbExecLocked(del);

        if (!p.tomb)
        {
            unsigned part = 0;
            for (size_t off = 0; off < p.body.size(); off += kDbChunk, ++part)
            {
                const std::string chunk = p.body.substr(off, kDbChunk);
                std::string ins = "INSERT OR REPLACE INTO forum_body VALUES (";
                ins += sqlQuote(t->thread_key_) + ",";
                ins += std::to_string(p.seq)    + ",";
                ins += std::to_string(part)     + ",";
                ins += sqlQuote(chunk) + ")";
                dbExecLocked(ins);
            }
        }

        dbExecLocked("COMMIT");
    }

    // ── Lookups ───────────────────────────────────────────────────────────

    ForumSelf* findSelfLocked(const std::string& s) const
    {
        for (const auto& [k, p] : selves_) if (k == s) return p;
        return nullptr;
    }

    ForumThread* findThreadLocked(const std::string& t) const
    {
        for (const auto& [k, p] : threads_) if (k == t) return p;
        return nullptr;
    }

    // addTag from here blocks on the LOADER's ordering thread -- a different
    // thread with no cycle back, so it completes rather than deadlocking. It
    // stalls forum dispatch for the duration, acceptable for something that
    // happens once per self rather than once per request.
    ForumSelf* createSelfLocked(const std::string& s)
    {
        ForumSelf* p = addTag<ForumSelf>();
        if (!p) { ETCS_LOG("ForumNode", "addTag<ForumSelf> failed for '" << s << "'"); return nullptr; }
        p->self_ = s;
        p->node_ = this;
        selves_.emplace_back(s, p);
        ETCS_LOG("ForumNode", "self joined: '" << s << "' RID:" << p->getRID()
                 << " (" << selves_.size() << " live)");
        return p;
    }

    // REFUSES past the cap rather than evicting. Eviction would need a reload
    // path, and there is no synchronous read action on the database side to
    // reload THROUGH -- LocalDatabase's read surface is a stream, driven by a
    // script, not something an entity can pull from mid-request. So a silent
    // eviction would present as a thread that existed a moment ago and now
    // 404s, with the data still on disk and no way to reach it.
    //
    // Refusing is worse for the operator and better for the reader, which is
    // the correct side to err on. The fix is a `Query` work action on
    // LocalDatabase filling a bounded Buffer -- roughly twenty lines there,
    // and the same shape as ExecuteRaw. Named rather than half-built.
    ForumThread* createThreadLocked(const std::string& tkey)
    {
        if (threads_.size() >= kMaxThreads)
        {
            ETCS_LOG("ForumNode", "thread cap " << kMaxThreads
                     << " reached -- REFUSING to open '" << tkey
                     << "'. No eviction: there is no reload path to evict toward.");
            return nullptr;
        }
        ForumThread* t = addTag<ForumThread>();
        if (!t) { ETCS_LOG("ForumNode", "addTag<ForumThread> failed for '" << tkey << "'"); return nullptr; }
        t->thread_key_ = tkey;
        t->node_       = this;
        threads_.emplace_back(tkey, t);
        ETCS_LOG("ForumNode", "thread opened: '" << tkey << "' RID:" << t->getRID()
                 << " (" << threads_.size() << " live)");
        return t;
    }

    ForumSelf* selfLocked(const std::string& s)
    {
        ForumSelf* p = findSelfLocked(s);
        if (!p) p = createSelfLocked(s);
        if (p)  p->touchLocked();
        return p;
    }

    // Called by ForumThread when a post lands. The thread knows its authors
    // (tokens); the node is what maps a token to a self.
    void reportPostLocked(const std::string& tok, const std::string& tkey, uint64_t ihash)
    {
        if (tok.empty()) return;
        ForumSelf* p = findSelfLocked(tok);
        if (!p) p = createSelfLocked(tok);   // a post is worth a self
        if (p)  p->recordPostLocked(tkey, ihash);
    }

    // ── Renderings ────────────────────────────────────────────────────────
    // Each of these builds the FULL deterministic string; the window is applied
    // by the caller. Same discipline as ForumThread::renderLocked, so a cursor
    // means one thing everywhere.

    // "<tkey> <posts> <here> <open|locked> <head16> <title...>"
    // Title last, because it is the only field that can contain spaces.
    std::string renderIndexLocked() const
    {
        std::string out;
        for (const auto& [k, t] : threads_)
        {
            if (!t) continue;
            out += k;
            out += " " + std::to_string(t->posts_.size());
            out += " " + std::to_string(t->liveReadersLocked(kReaderIdle));
            out += t->locked_ ? " locked" : " open";
            out += " " + ForumHash::hex16(t->headLocked());
            out += " " + (t->title_.empty() ? std::string("-") : t->title_);
            out += "\n";
        }
        return out;
    }

    std::string renderSelvesLocked() const
    {
        std::string out;
        for (const auto& [k, p] : selves_) { if (p) { out += p->profileLocked(); out += "\n"; } }
        return out;
    }

    // The integrity report, and the reason the two hashes are separate.
    //
    // Content is checked against chash; the chain is recomputed from scratch
    // and checked against each ihash. A tombstoned post has no content to check
    // and MUST still verify on the chain -- if it did not, a removal would be
    // indistinguishable from a rewrite, and the tombstone would be worthless.
    //
    // This is also the load-path check. A row that came back off disk wrong,
    // or a body whose chunks reassembled short, shows up here rather than
    // silently becoming the new truth.
    std::string renderVerifyLocked() const
    {
        std::string out;
        for (const auto& [k, t] : threads_)
        {
            if (!t) continue;
            std::string fault;
            uint64_t prev = 0;

            for (const auto& p : t->posts_)
            {
                if (!p.tomb && ForumHash::of(p.body) != p.chash)
                { fault = "content seq " + std::to_string(p.seq); break; }

                std::string pre = ForumHash::hex16(prev);
                pre += '\x1F'; pre += std::to_string(p.seq);
                pre += '\x1F'; pre += p.author;
                pre += '\x1F'; pre += ForumHash::hex16(p.chash);
                if (ForumHash::of(pre) != p.ihash)
                { fault = "chain seq " + std::to_string(p.seq); break; }
                prev = p.ihash;
            }

            out += k;
            out += fault.empty() ? (" ok " + ForumHash::hex16(t->headLocked()))
                                 : (" BAD " + fault);
            out += "\n";
        }
        return out;
    }

    // Drop selves whose session has expired. Threads are untouched: they are
    // the record, not the session, and nobody has to be present for one to
    // exist. This is the whole of the divergence from ChessNode::reapLocked,
    // which also closes rooms nobody holds an edge to.
    void reapLocked()
    {
        for (size_t i = 0; i < selves_.size(); )
        {
            ForumSelf* p = selves_[i].second;
            if (p && p->idleSecondsLocked() >= kSession)
            {
                ETCS_LOG("ForumNode", "session expired, dropping self '" << p->self_
                         << "' (" << p->posts_ << " posts; threads untouched)");
                selves_.erase(selves_.begin() + static_cast<long>(i));
                p->Delete();
                continue;
            }
            ++i;
        }
    }

    // ── Load ──────────────────────────────────────────────────────────────
    // Applies ONE row arriving from the database, in DatabaseProvider's own
    // QueryProduce wire format.
    //
    // This parse is the ugliest thing in the module and it is ugly for a
    // structural reason worth naming rather than hiding: QueryProduce emits SQL
    // TEXT, because it was built for database-to-database mirroring where the
    // consumer is another sqlite handle that can just execute it. A consumer
    // that is not a database has to parse SQL back into fields, which is a
    // round trip through a format neither side wanted.
    //
    // It couples this module to DatabaseProvider's WIRE FORMAT, not to its
    // types, so the no-link-dependency property holds. But a format coupling is
    // still a coupling, and the honest fix is a neutral record producer on the
    // database side (fields, not statements) that any consumer can read. Until
    // that exists, this is what "use the existing work functions" costs.
    //
    // Load order is a real precondition, not a suggestion: forum_thread, then
    // forum_post, then forum_body. A body chunk for a post that has not arrived
    // is dropped with a log rather than buffered, because buffering it would
    // mean holding unattributed content of unknown provenance in memory.
    std::string applyRowLocked(const std::string& line)
    {
        if (line.empty()) return "SKIP";

        // Header frame from RowProduce: "#col\tcol\t..." -- the column order
        // for every row that follows, so the mapping is by NAME and a SELECT
        // that reorders or adds columns cannot silently shift fields.
        if (line[0] == '#')
        {
            load_cols_.clear();
            splitTabsLocked(line.substr(1), load_cols_);
            load_table_.clear();
            // Which table this is, inferred from the columns present. The
            // stream carries rows, not a table name, and asking the script to
            // repeat it would be a second place for the two to disagree.
            bool has_part = false, has_seq = false;
            for (const auto& c : load_cols_)
            {
                if (c == "part") has_part = true;
                if (c == "seq")  has_seq  = true;
            }
            if      (has_part) load_table_ = "forum_body";
            else if (has_seq)  load_table_ = "forum_post";
            else               load_table_ = "forum_thread";
            return "SKIP";
        }

        if (load_cols_.empty())
        {
            ETCS_LOG("ForumNode", "row arrived before any header -- the producer "
                     "must send its column frame first.");
            return "BAD";
        }

        std::vector<std::string> vals;
        splitTabsLocked(line, vals);
        if (vals.size() != load_cols_.size()) return "ARITY";

        auto field = [&](const char* name) -> std::string
        {
            for (size_t n = 0; n < load_cols_.size(); ++n)
                if (load_cols_[n] == name) return vals[n];
            return std::string();
        };

        const std::string tkey = field("tkey");
        if (tkey.empty()) return "NO KEY";

        ForumThread* t = findThreadLocked(tkey);
        if (!t) t = createThreadLocked(tkey);
        if (!t) return "FAILED";

        if (load_table_ == "forum_thread")
        {
            t->title_  = field("title");
            t->op_     = field("op");
            t->locked_ = (field("locked") == "1");
            // counter_ is the only piece of node state that is not derivable
            // from a thread's own row, and forgetting it is not a cosmetic
            // loss: after a restart it restarts at 0, newThreadLocked mints
            // "t1" again, and findThreadLocked resolves that to the thread
            // ALREADY loaded under that key -- so a user opening a new thread
            // silently lands in an old one and their posts appear there.
            if (tkey.size() > 1 && tkey[0] == 't')
            {
                const size_t n = ForumWire::parseCursor(tkey.substr(1));
                if (n > counter_) counter_ = static_cast<unsigned>(n);
            }
            return "THREAD";
        }

        const unsigned seq = static_cast<unsigned>(ForumWire::parseCursor(field("seq")));
        if (seq == 0) return "BAD SEQ";

        if (load_table_ == "forum_post")
        {
            ForumPost p;
            p.seq    = seq;
            p.author = field("author");
            p.made   = static_cast<int64_t>(ForumWire::parseCursor(field("made")));
            p.tomb   = (field("tomb") == "1");
            p.chash  = unhexLocked(field("chash"));
            p.ihash  = unhexLocked(field("ihash"));
            // By seq, not appended: a SELECT without an ORDER BY must not be
            // able to reorder history, which would change every ihash below
            // the insertion point and report as corruption on the next Verify.
            insertBySeqLocked(*t, p);
            if (t->next_seq_ <= seq) t->next_seq_ = seq + 1;
            return "POST";
        }

        if (load_table_ == "forum_body")
        {
            const unsigned part = static_cast<unsigned>(ForumWire::parseCursor(field("part")));
            const std::string chunk = field("chunk");
            for (auto& p : t->posts_)
            {
                if (p.seq != seq) continue;
                // Placed at its byte offset rather than appended, so an
                // out-of-order part cannot scramble a body. A gap left by a
                // genuinely missing part stays a gap and fails Verify, which
                // is correct: a silently short body would pass as content
                // nobody wrote.
                const size_t off = static_cast<size_t>(part) * kDbChunk;
                if (p.body.size() < off + chunk.size()) p.body.resize(off + chunk.size(), ' ');
                p.body.replace(off, chunk.size(), chunk);
                return "BODY";
            }
            ETCS_LOG("ForumNode", "body chunk for unknown post " << tkey << "/" << seq
                     << " -- dropped (load order: thread, post, body)");
            return "ORPHAN";
        }

        return "SKIP";
    }

    // Tab-separated, undoing RowProduce's own escaping.
    static void splitTabsLocked(const std::string& in, std::vector<std::string>& out)
    {
        out.clear();
        std::string cur;
        for (size_t i = 0; i < in.size(); ++i)
        {
            if (in[i] == '\\' && i + 1 < in.size())
            {
                const char n = in[++i];
                if      (n == 't')  cur += '\t';
                else if (n == 'n')  cur += '\n';
                else if (n == '\\') cur += '\\';
                else { cur += '\\'; cur += n; }
                continue;
            }
            if (in[i] == '\t') { out.push_back(cur); cur.clear(); continue; }
            cur += in[i];
        }
        out.push_back(cur);
    }

    static void insertBySeqLocked(ForumThread& t, const ForumPost& p)
    {
        for (auto& e : t.posts_) if (e.seq == p.seq) { e = p; return; }
        size_t at = t.posts_.size();
        while (at > 0 && t.posts_[at - 1].seq > p.seq) --at;
        t.posts_.insert(t.posts_.begin() + static_cast<long>(at), p);
    }

    static uint64_t unhexLocked(const std::string& s)
    {
        uint64_t v = 0;
        for (char c : s)
        {
            int d;
            if      (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return 0;
            v = (v << 4) | static_cast<uint64_t>(d);
        }
        return v;
    }


    // ── Routing ───────────────────────────────────────────────────────────

    // Everything from segment `from` onward, rejoined. A post body is one
    // logical argument that may legitimately contain '/', and chess's
    // take-segment-four approach would silently truncate at the first one.
    static std::string remainderLocked(const std::vector<std::string>& seg, size_t from)
    {
        std::string out;
        for (size_t i = from; i < seg.size(); ++i)
        { if (i > from) out += '/'; out += seg[i]; }
        return out;
    }

    // /<mount>/index|selves|verify [/<cursor>]
    // /<mount>/<self>/list [/<cursor>] | me | new/<title>
    // /<mount>/<self>/<tkey>/<verb>[/<rest...>]
    //
    // "index", "selves" and "verify" are reserved selves; "list", "me" and
    // "new" are reserved threads. A token is capped at kMaxIdent, because an
    // identity is an identifier -- letting it be arbitrarily long makes it a
    // second payload channel that bypasses every cap on the first.
    std::string requestLocked(const std::string& path)
    {
        std::vector<std::string> seg;
        ForumThread::splitPath(path, seg);
        if (seg.empty() || seg[0] != mount_) return "NOT FOUND";

        reapLocked();

        const std::string who = (seg.size() > 1) ? seg[1] : "";
        if (who.empty()) return "NOT FOUND";

        const std::string cur = (seg.size() > 2) ? seg[2] : "";
        if (who == "index")  return windowOrEnd(renderIndexLocked(),  cur);
        if (who == "selves") return windowOrEnd(renderSelvesLocked(), cur);
        if (who == "verify") return windowOrEnd(renderVerifyLocked(), cur);

        if (who.size() > kMaxIdent) return "IDENT TOO LONG";

        ForumSelf* me = selfLocked(who);
        if (!me) return "FAILED";

        const std::string tkey = (seg.size() > 2) ? seg[2] : "";
        if (tkey.empty() || tkey == "list")
            return windowOrEnd(me->renderListLocked(), (seg.size() > 3) ? seg[3] : "");
        if (tkey == "me")  return me->profileLocked();
        if (tkey == "new") return newThreadLocked(who, remainderLocked(seg, 3));

        ForumThread* t = findThreadLocked(tkey);
        if (!t) return "NOT FOUND";

        // Visiting a thread IS the edge join. There is no separate step, which
        // is what makes a shared link work: following it puts the thread on
        // your own self without anyone granting you anything.
        me->addEdgeLocked(tkey, t);

        const std::string verb = (seg.size() > 3) ? seg[3] : "";
        const std::string arg  = remainderLocked(seg, 4);
        return t->verbLocked(who, verb, arg);
    }

    // Unlike chess, threads are never auto-created by being visited. A chess
    // match that does not exist yet is harmless to conjure -- it is an empty
    // board. A thread conjured by a typo'd URL is a permanent empty record in a
    // durable index, so creation is an explicit verb and only that.
    std::string newThreadLocked(const std::string& who, const std::string& title)
    {
        // Skip any key already taken. The counter restore above makes this
        // unreachable in the ordinary case, but a collision here is silent and
        // its symptom (posting into someone else's thread) is indistinguishable
        // from a routing bug -- so the invariant is enforced where it is cheap
        // rather than assumed to hold upstream.
        std::string tkey;
        for (int guard = 0; guard < 1024; ++guard)
        {
            tkey = "t" + std::to_string(++counter_);
            if (!findThreadLocked(tkey)) break;
            tkey.clear();
        }
        if (tkey.empty())
        {
            ETCS_LOG("ForumNode", "could not mint an unused thread key in 1024 "
                     "attempts -- counter_ is far behind the loaded set.");
            return "FAILED";
        }
        ForumThread* t = createThreadLocked(tkey);
        if (!t) return "FAILED";

        if (!title.empty()) t->titleLocked(title, who);
        persistThreadHeaderLocked(t);

        ForumSelf* me = selfLocked(who);
        if (me) me->addEdgeLocked(tkey, t);
        return tkey;
    }

    static std::string windowOrEnd(const std::string& full, const std::string& cursor)
    { return ForumWire::window(full, ForumWire::parseCursor(cursor)); }

    std::string mount_ = "forum";
    std::vector<std::pair<std::string, ForumSelf*>>   selves_;
    std::vector<std::pair<std::string, ForumThread*>> threads_;
    unsigned counter_ = 0;

    // Load-time only: the column frame RowProduce sends before its rows, and
    // which table those columns identify.
    std::vector<std::string> load_cols_;
    std::string              load_table_;

    // mutable: stream() is const because every read verb is, and call_once is
    // the mutation. The stream itself is logically part of this node's
    // identity, not part of its observable state.
    mutable ForumStream    stream_;
    mutable std::once_flag stream_started_;

    ETCS::RID           db_rid_    = 0;
    std::string         db_action_ = "ExecuteRaw";
    ETCS::SignalContext run_ctx_;
    bool                have_ctx_  = false;
};

// ── Deferred definitions ──────────────────────────────────────────────────────
// These need both the self and the node complete.

// "<tkey> <posts> <here> <open|locked> <head16> <title...>" per line -- my
// threads, from my side. Read off the shared thread today; once the threads are
// separate the same fields come from the edge itself, which is why it is
// reported per-self rather than as a property of the thread.
inline std::string ForumSelf::renderListLocked() const
{
    std::string out;
    for (const auto& [k, t] : edges_)
    {
        if (!t) continue;
        out += k;
        out += " " + std::to_string(t->posts_.size());
        out += " " + std::to_string(t->liveReadersLocked(60));
        out += t->locked_ ? " locked" : " open";
        out += " " + ForumHash::hex16(t->headLocked());
        out += " " + (t->title_.empty() ? std::string("-") : t->title_);
        out += "\n";
    }
    return out;
}

// The thread reports to the NODE, which owns both the token -> self mapping and
// the database edge. A thread with no node (one spawned standalone by a script)
// simply keeps no records and persists nothing -- there is no self to keep them
// on and no connection to write them to.
inline void ForumThread::persistPostLocked(const ForumPost& p)
{
    if (!node_) return;
    node_->reportPostLocked(p.author, thread_key_, p.ihash);
    node_->persistPostLocked(this, p);
}

// A thread's ordering domain is its node's. Deferred to here because it needs
// ForumNode complete.
inline ForumStream* ForumThread::streamOf() const
{
    return node_ ? &node_->stream() : nullptr;
}

inline void ForumThread::persistHeaderLocked()
{
    if (!node_) return;
    node_->persistThreadHeaderLocked(this);
}

inline ETCS::DispatchResult ForumStream::on_event(ForumState&,
                                                  const ForumInEventPtr& evt,
                                                  uint64_t)
{
    ForumInEvent& e = *evt.ptr;
    const std::string arg = e.arg ? e.arg : "";
    const std::string tok = e.tok ? e.tok : "";
    std::string result;

    switch (e.kind)
    {
        case ForumInEvent::Kind::NodeRequest:
        {
            ForumNode* n = static_cast<ForumNode*>(e.target);
            if (n) result = n->requestLocked(arg);
            break;
        }
        case ForumInEvent::Kind::NodeIndex:
        {
            ForumNode* n = static_cast<ForumNode*>(e.target);
            if (n) result = ForumNode::windowOrEnd(n->renderIndexLocked(), arg);
            break;
        }
        case ForumInEvent::Kind::NodeSelves:
        {
            ForumNode* n = static_cast<ForumNode*>(e.target);
            if (n) result = ForumNode::windowOrEnd(n->renderSelvesLocked(), arg);
            break;
        }
        case ForumInEvent::Kind::NodeNew:
        {
            ForumNode* n = static_cast<ForumNode*>(e.target);
            if (n) result = n->newThreadLocked(tok, arg);
            break;
        }
        case ForumInEvent::Kind::NodeLoadRow:
        {
            ForumNode* n = static_cast<ForumNode*>(e.target);
            if (n) result = n->applyRowLocked(arg);
            break;
        }
        // Two administrative verbs share a Kind rather than each taking one,
        // because both are "do a thing to the node itself and report" and
        // neither is on a hot path. The arg discriminates.
        case ForumInEvent::Kind::NodeFlush:
        {
            ForumNode* n = static_cast<ForumNode*>(e.target);
            if (!n) break;
            if (arg == "init") { n->initSchemaLocked(); result = "OK"; }
            else if (arg.compare(0, 7, "verify:") == 0)
                result = ForumNode::windowOrEnd(n->renderVerifyLocked(), arg.substr(7));
            else result = "NOT FOUND";
            break;
        }

        default:
        {
            ForumThread* t = static_cast<ForumThread*>(e.target);
            if (!t) break;
            switch (e.kind)
            {
                case ForumInEvent::Kind::Read:   result = t->readLocked(arg, tok);       break;
                case ForumInEvent::Kind::Post:   result = t->appendLocked(arg, tok);     break;
                case ForumInEvent::Kind::Title:  result = t->titleLocked(arg, tok);      break;
                case ForumInEvent::Kind::Lock:   result = t->lockLocked(tok);            break;
                case ForumInEvent::Kind::Tomb:   result = t->tombLocked(arg, tok);       break;
                case ForumInEvent::Kind::Status: result = t->statusLocked();             break;
                case ForumInEvent::Kind::Head:   result = ForumHash::hex16(t->headLocked()); break;
                case ForumInEvent::Kind::Verb:   result = t->verbLocked(tok, arg, "");   break;
                default: break;
            }
            break;
        }
    }

    if (e.result_out) *e.result_out = result;
    // Release AFTER writing the result: the waiting thread acquire-loads this
    // flag and then reads result_out, so this store is the happens-before edge.
    if (e.done) e.done->store(true, std::memory_order_release);

    // Drop, not Inline: the work is finished and nothing downstream consumes an
    // emit, so a gap slot would reserve reorder capacity for nothing.
    return { ETCS::DispatchKind::Drop, nullptr };
}

#endif // FORUMNODE_H__