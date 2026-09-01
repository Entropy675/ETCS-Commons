#ifndef FORUMTHREAD_H__
#define FORUMTHREAD_H__
#include "../../../ontology.h"

#include <vector>
#include <string>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cctype>
#include <ctime>
#include <unordered_map>

class ForumThread;
class ForumSelf;
class ForumNode;

// ── Serialization ─────────────────────────────────────────────────────────────
// Same reasoning as ChessProvider's, and for the same reason it is not merely a
// safety property: a thread IS an ordered sequence, so two posts arriving at
// once need a globally agreed sequence, not merely a non-corrupting interleave.
// A mutex would give the second; only a single ordering thread gives the first,
// and the hash chain below makes the difference observable -- two peers that
// disagree about order produce different heads.
//
// The mechanism is that the ordering thread is a SINGLE thread. Nothing reached
// from on_event needs a lock, which is why every *Locked body has no
// synchronisation of its own, and why a thread can call straight into its node
// to persist a post with no handshake.

struct ForumInEvent
{
    enum class Kind : uint8_t {
        // ForumThread targets
        Read, Post, Title, Lock, Tomb, Status, Head, Verb,
        // ForumNode targets
        NodeRequest, NodeIndex, NodeSelves, NodeNew, NodeLoadRow, NodeFlush
    };

    Kind        kind;
    void*       target = nullptr;   // ForumThread* or ForumNode*, per kind

    const char* arg = nullptr;      // path / body / title / cursor / row
    const char* tok = nullptr;      // who, when the caller knows

    std::string*       result_out = nullptr;
    std::atomic<bool>* done       = nullptr;
};

// Only the POINTER rides the ring, exactly as in ChessProvider: LBuffer is 32
// bytes and enqueueing the payload type directly memcpys the whole struct
// through that slot.
struct ForumInEventPtr { ForumInEvent* ptr; };

// All state lives on the entity each event points at, so the stream's own State
// is empty.
struct ForumState {};

struct ForumStream : ETCS::EventStream<ForumStream, ForumState, ForumInEventPtr>
{
    // Defined at the bottom of ForumNode.h: it dispatches to both types and so
    // needs both complete.
    ETCS::DispatchResult on_event(ForumState&, const ForumInEventPtr& evt, uint64_t seq);

    void on_completion(ForumState&, ETCS::WorkResult*, uint64_t) {}
    void on_emit(ForumState&, ETCS::GapSlot&)                    {}

    // NO getInstance(). A stream is OWNED by a ForumNode -- see ForumNode's own
    // comment for why the node is the ordering domain. What matters here is
    // that this type names no particular one: a state machine REQUIRES an
    // output target rather than reaching for a module-global, so the same type
    // code serves per-node, per-thread, or N-hashed-to-k wiring with no source
    // change. Granularity becomes a deployment decision instead of a release
    // decision.
    //
    // The version this replaced was a Meyers singleton shared by every thread
    // in the module, justified as "ordering across unrelated threads is
    // harmless". That is true of a peer with one thread and false of a server
    // with many: it asserts a causal relation between conversations that have
    // none, and pays for the assertion in head-of-line blocking proportional
    // to total traffic rather than to any one thread's.
};

// ── The caller-facing event ───────────────────────────────────────────────────
// Constructed on the caller's stack, invoked, and read: the object IS the
// completion slot. One event type with a Kind rather than one struct per verb,
// because every forum operation is (target, arg, tok) -> string.
struct ForumOpEvent
{
    // The target ordering domain, passed in rather than looked up. Null is a
    // legitimate state (an entity with no node), and it is REFUSED rather than
    // falling back to a direct call: the direct call is exactly the
    // unsynchronised path that serialization exists to prevent, so a fallback
    // would make the dangerous case the quiet one.
    ForumStream*       stream;
    ForumInEvent::Kind kind;
    void*              target;
    std::string        arg;
    std::string        tok;

    std::string        result;
    std::atomic<bool>  done{false};

    ForumOpEvent(ForumStream* st, ForumInEvent::Kind k, void* t,
                 std::string a = "", std::string s = "")
        : stream(st), kind(k), target(t), arg(std::move(a)), tok(std::move(s)) {}

    std::string operator()();
};

inline std::string ForumOpEvent::operator()()
{
    // Guards being called FROM an ordering thread, where waiting on a stream
    // that may be ordered behind you is a deadlock. It cannot catch re-entry
    // from this stream's own thread, but on_event only ever calls the *Locked
    // bodies, never back through here.
    // NOTE: with one stream per node this check is now conservative. It asks
    // whether the caller is on AN ordering thread, not whether it is on THIS
    // one -- so a future node-to-node call would trip it spuriously, and the
    // genuine A->B-while-B->A deadlock it used to make structurally impossible
    // is no longer impossible. Cross-node calls need to be non-blocking; the
    // assert needs a stream identity before that rule can be enforced here.
    ETCS_ASSERT_NOT_ORDERING_THREAD("ForumOpEvent");

    if (!stream) return "NO NODE";

    ForumInEvent in;
    in.kind       = kind;
    in.target     = target;
    in.arg        = arg.c_str();
    in.tok        = tok.c_str();
    in.result_out = &result;
    in.done       = &done;

    if (!stream->enqueue(ForumInEventPtr{&in}))
        return "BUSY";

    // progressiveYield, not a bare spin: this runs on a ThreadPool thread and
    // fires on every HTTP request, so a hot spin burns a core per in-flight
    // request and can starve the very ordering thread it is waiting on.
    int retry = 0;
    while (!done.load(std::memory_order_acquire))
        ETCS::LMAXSequentialSharedPage::progressiveYield(retry);
    return result;
}

// ── Wire budget ───────────────────────────────────────────────────────────────
// THE constraint this whole module is shaped by. A route handler's reply is one
// ETCS::Buffer -- HttpServer::Serve takes route_body.buf/.written directly as
// the response body -- so no request can ever return more than bufsize bytes.
// Chess never noticed (a FEN is 70 bytes); a forum would be unusable.
//
// The answer is not to widen the buffer here but to make EVERY read a bounded
// window over a deterministic rendering, with an explicit cursor. One mechanism
// for thread lists, thread bodies and post text alike, rather than three
// hand-rolled paginations that would each need their own off-by-one.
//
// It also happens to be the honest protocol: no request returns "the thread",
// only a bounded observation of it plus where to continue. A reader that wants
// the whole history has to actually walk it, and the walk is visible.
namespace ForumWire
{
    // 200, not bufsize: leaves room for the cursor line and the terminating
    // NUL that Buffer::writeString appends. Deliberately a round number rather
    // than bufsize-minus-arithmetic, so nobody has to re-derive it after a
    // Buffer change.
    static constexpr size_t kWire = 200;

    // EVERY response opens with a one-line CURSOR HEADER, and the payload
    // after it is byte-exact. Three forms:
    //
    //   @.        this is the tail, nothing follows it
    //   @+N       resume at N; this chunk ended on a whole line
    //   @~N       resume at N; this chunk was CUT MID-LINE, concatenate
    //
    // The header goes FIRST and is unconditional, which is the whole point. A
    // trailing marker has to be distinguished from payload, and the only way to
    // do that is to inject a separator the source never contained -- so a
    // client reassembling a long post byte-exactly gets a newline nobody wrote.
    // Found by reassembling a 430-byte body across four windows and diffing.
    //
    // Two markers rather than one, because the client needs to know whether it
    // is resuming at a line boundary or mid-token. A single marker forces a
    // guess, and guessing wrong silently splices two posts together.
    inline std::string window(const std::string& full, size_t cursor)
    {
        if (cursor >= full.size()) return "@.\n";

        size_t take = full.size() - cursor;
        if (take <= kWire) return "@.\n" + full.substr(cursor);

        take = kWire;
        // Prefer a line boundary. rfind over the candidate chunk only, and the
        // >= cursor guard, so a newline BEFORE the cursor cannot be selected.
        const size_t nl = full.rfind('\n', cursor + take - 1);
        if (nl != std::string::npos && nl >= cursor)
        {
            const size_t line_take = nl - cursor + 1;
            return "@+" + std::to_string(cursor + line_take) + "\n"
                 + full.substr(cursor, line_take);
        }
        // No newline in range: a single line longer than the window. Cut it and
        // say so, rather than truncating and pretending the record ended.
        return "@~" + std::to_string(cursor + take) + "\n" + full.substr(cursor, take);
    }

    inline size_t parseCursor(const std::string& s)
    {
        if (s.empty()) return 0;
        size_t v = 0;
        for (char c : s)
        {
            if (c < '0' || c > '9') return 0;   // garbage reads as "from the top"
            v = v * 10 + static_cast<size_t>(c - '0');
        }
        return v;
    }
}

// ── Hashing ───────────────────────────────────────────────────────────────────
// FNV-1a 64. Not a security hash and not claimed as one: it makes ACCIDENTAL
// divergence and casual editing detectable, which is the property this layer
// actually needs, and it costs no dependency. The slot a real hash drops into
// later is exactly this function and nothing else, because every call site goes
// through hex16().
namespace ForumHash
{
    static constexpr uint64_t kOffset = 1469598103934665603ULL;
    static constexpr uint64_t kPrime  = 1099511628211ULL;

    inline uint64_t of(const char* p, size_t n, uint64_t h = kOffset)
    {
        for (size_t i = 0; i < n; ++i)
        { h ^= static_cast<unsigned char>(p[i]); h *= kPrime; }
        return h;
    }
    inline uint64_t of(const std::string& s, uint64_t h = kOffset)
    { return of(s.data(), s.size(), h); }

    inline std::string hex16(uint64_t v)
    {
        static const char* d = "0123456789abcdef";
        std::string out(16, '0');
        for (int i = 15; i >= 0; --i) { out[static_cast<size_t>(i)] = d[v & 0xF]; v >>= 4; }
        return out;
    }
}

// ── One post ──────────────────────────────────────────────────────────────────
// TWO hashes, and the split is the point (it is the same content-vs-identity
// split the trust model uses at the domain boundary, instanced at the smallest
// scale where it still means something):
//
//   chash -- the CONTENT. What was said, and nothing about who or when.
//   ihash -- the IDENTITY of this post's place in this thread: a chain over
//            (previous ihash, seq, author, chash).
//
// Because ihash chains, the last post's ihash commits to the entire visible
// history. A client that saw head H and later sees a thread whose head is not
// H, with no new posts to explain it, has caught a rewrite -- without holding
// the thread, without a signature, and without anyone being trusted to report
// it. That is the property to lead with. The server is not trusted to be
// honest; it is merely unable to be quietly dishonest.
//
// Tombstoning clears `body` and sets `tomb`, and deliberately does NOT touch
// chash or ihash. So the chain still verifies across a removal, and the removal
// is itself observable as a crossing: the content is gone, the fact that
// content of exactly that hash was here is not.
struct ForumPost
{
    unsigned    seq   = 0;
    int64_t     made  = 0;         // epoch seconds
    bool        tomb  = false;
    std::string author;            // token, opaque
    std::string body;              // cleared on tombstone
    uint64_t    chash = 0;
    uint64_t    ihash = 0;
};

// ── ForumThread ───────────────────────────────────────────────────────────────
// The shared artifact, and the direct analogue of ChessGame: it knows verbs,
// not URLs. The node parses paths and calls verbLocked, so the same thread can
// be driven by an HTTP route today and by a replayed post stream from a peer
// later, with one entry point so the two cannot diverge.
//
// Bases: EphemeralBase (a thread can be reset, and has a real notion of being
// closed), DeletableBase, FilterBase. Not Gate_/Switchable_: no open/closed
// axis of its own beyond `locked_`, which is a property of the conversation
// rather than of a resource handle.
//
// Identity is a client-generated TOKEN in the path, not a connection: the
// server answers Connection: close, so nothing on the wire outlives a fetch.
// Same shape a peer needs, and the slot an ACE identity key drops into.
//
// Authorship is claimed by POSTING, not by opening -- the same rule as chess
// seats being claimed by moving. Opening a thread is free and unlimited, so the
// claim has to cost something only a participant would spend.
class ForumThread :
    public EphemeralBase<ForumThread>,
    public DeletableBase<ForumThread>,
    public FilterBase<ForumThread>
{
    friend struct ForumStream;
    friend class  ForumSelf;
    friend class  ForumNode;

public:
    WIRE_TYPE_IDENTITY(ForumThread);

    using Clock = std::chrono::steady_clock;
    using Kind  = ForumInEvent::Kind;

    ForumThread()          = default;
    virtual ~ForumThread() = default;

    // ── Public surface: every one of these serializes ─────────────────────
    std::string Read(const std::string& cur = "") const   { return op(Kind::Read, cur); }
    std::string PostBody(const std::string& body,
                         const std::string& tok = "")     { return op(Kind::Post, body, tok); }
    std::string SetTitle(const std::string& t,
                         const std::string& tok = "")     { return op(Kind::Title, t, tok); }
    std::string LockThread(const std::string& tok = "")   { return op(Kind::Lock, "", tok); }
    std::string Tombstone(const std::string& seq,
                          const std::string& tok = "")    { return op(Kind::Tomb, seq, tok); }
    std::string StatusLine() const                        { return op(Kind::Status); }
    std::string HeadHash() const                          { return op(Kind::Head); }

    // ── EphemeralBase / DeletableBase ─────────────────────────────────────
    bool ResetConcrete() { return op(Kind::Verb, "reset") != "BUSY"; }

    // Reads the cached flag rather than recomputing, same as ChessGame: a plain
    // bool read races benignly with a post landing, worst case a caller sees
    // the previous answer one request early.
    bool IsActiveConcrete() const { return !locked_; }

    bool DeleteConcrete()
    {
        std::string conjugate_key = this->getSourceModule().toString() + ":"
                                  + this->getSourceTag().toString();
        ETCS_LOG("ForumThread", "Delete: firing self-DestroyEvent for RID:"
                 << getRID() << " (" << conjugate_key << ")");
        ETCS::DestroyEvent{conjugate_key.c_str(), this, true}();
        return true;
    }

    // ── FilterBase ────────────────────────────────────────────────────────
    // Deliberately NOT routed through the stream. It reads only thread_key_ and
    // is called on a pool thread for EVERY request, including paths that turn
    // out not to be ours -- a round trip would serialize all path matching
    // behind forum logic.
    //
    // Filter_'s contract is "empty io means declined", so a decline MUST clear
    // the buffer: returning false while leaving the descriptor in place reads
    // to the caller as an acceptance whose key happens to be the descriptor.
    //
    // No locked check. Whether a thread accepts new POSTS is a question
    // answered in appendLocked; declining here would make a locked thread
    // unreadable, which is the opposite of what locking means.
    bool AcceptsConcrete(ETCS::Buffer& io) const
    {
        if (thread_key_.empty()) { io.reset(); return false; }

        const std::string desc = io.restAsString();
        bool found = false;
        for (size_t i = 0; !found && i < desc.size(); )
        {
            size_t j = desc.find('/', i);
            if (j == std::string::npos) j = desc.size();
            if (desc.compare(i, j - i, thread_key_) == 0) found = true;
            i = j + 1;
        }
        if (!found) { io.reset(); return false; }
        io.writeString(thread_key_.c_str());
        return true;
    }

    const std::string& ThreadKey() const    { return thread_key_; }
    void SetThreadKey(const std::string& k) { thread_key_ = k; }   // setup only
    const std::string& Title() const        { return title_; }
    const std::string& Op() const           { return op_; }
    size_t PostCount() const                { return posts_.size(); }

    // Shared with the node, which splits the same paths.
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

    // Percent-decoding, which chess does not do and a forum cannot skip. A post
    // body is arbitrary text arriving as ONE path segment, so the client
    // encodeURIComponent's it; '?' in particular must arrive as %3F or
    // HttpServer::Serve strips the rest of the body as a query string before
    // this module ever sees it.
    //
    // Decoded HERE rather than in the server, because the server correctly
    // treats the path as opaque -- only the type that knows a segment is a
    // human's sentence knows it should be decoded.
    static std::string percentDecode(const std::string& in)
    {
        auto hexval = [](char c) -> int
        {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        std::string out;
        out.reserve(in.size());
        for (size_t i = 0; i < in.size(); ++i)
        {
            if (in[i] == '%' && i + 2 < in.size())
            {
                const int hi = hexval(in[i + 1]), lo = hexval(in[i + 2]);
                if (hi >= 0 && lo >= 0)
                { out.push_back(static_cast<char>(hi * 16 + lo)); i += 2; continue; }
            }
            if (in[i] == '+') { out.push_back(' '); continue; }
            out.push_back(in[i]);
        }
        return out;
    }

private:
    // A body is capped and flattened to ONE line. Both are wire facts rather
    // than editorial ones: the rendering below is line-oriented and the window
    // cuts on newlines, so an embedded newline would make a record's line count
    // unbounded in a second dimension for no gain at this size. Longer posts
    // are a chunking problem, and chunking already exists -- it is the cursor.
    static constexpr size_t kMaxBody  = 512;
    static constexpr size_t kMaxTitle = 96;

    // Defined at the bottom of ForumNode.h -- it needs the node complete.
    // A thread with no node has no ordering domain and therefore no safe way to
    // run any verb, so every one of them returns "NO NODE" rather than
    // executing unsynchronised.
    ForumStream* streamOf() const;

    std::string op(Kind k, const std::string& arg = "",
                   const std::string& tok = "") const
    {
        return ForumOpEvent{streamOf(), k, const_cast<ForumThread*>(this), arg, tok}();
    }

    // ── Everything below runs ONLY on the ordering thread ─────────────────
    // Single thread, therefore no synchronisation. Nothing here may call op():
    // that would enqueue behind itself and never complete.

    static std::string flattenLocked(const std::string& in, size_t cap)
    {
        std::string out;
        out.reserve(in.size() < cap ? in.size() : cap);
        for (char c : in)
        {
            if (out.size() >= cap) break;
            // Control characters become spaces rather than being dropped, so
            // the byte count a client sees matches what it sent minus nothing.
            out.push_back((static_cast<unsigned char>(c) < 0x20 || c == 0x7F) ? ' ' : c);
        }
        // Trailing whitespace only; leading is the poster's business.
        while (!out.empty() && out.back() == ' ') out.pop_back();
        return out;
    }

    uint64_t headLocked() const
    { return posts_.empty() ? 0ULL : posts_.back().ihash; }

    void touchLocked(const std::string& tok)
    {
        seen_ = Clock::now();
        if (!tok.empty()) readers_[tok] = Clock::now();
    }

    int idleSecondsLocked() const
    {
        return static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
                   Clock::now() - seen_).count());
    }

    int liveReadersLocked(int grace_seconds) const
    {
        const auto now = Clock::now();
        int n = 0;
        for (const auto& [t, when] : readers_)
            if (std::chrono::duration_cast<std::chrono::seconds>(now - when).count()
                    < grace_seconds) ++n;
        return n;
    }

    // Defined at the bottom of ForumNode.h: it needs the node complete, and it
    // is a plain function call rather than an event precisely because both
    // types live on this same ordering thread.
    void persistPostLocked(const ForumPost& p);
    void persistHeaderLocked();

    bool resetLocked()
    {
        posts_.clear();
        readers_.clear();
        locked_ = false;
        op_.clear();
        next_seq_ = 1;
        touchLocked("");
        return true;
    }

    // The one place a post is created, and therefore the one place the chain is
    // extended. Everything about ordering, authorship and hashing is here so
    // that a replayed post from a peer can enter through the same door.
    std::string appendLocked(const std::string& raw, const std::string& tok)
    {
        if (locked_)     return "LOCKED";
        if (tok.empty()) return "NO IDENTITY";   // unlike chess, a post is always attributed

        const std::string body = flattenLocked(percentDecode(raw), kMaxBody);
        if (body.empty()) return "EMPTY";

        ForumPost p;
        p.seq    = next_seq_++;
        p.made   = static_cast<int64_t>(std::time(nullptr));
        p.author = tok;
        p.body   = body;
        p.chash  = ForumHash::of(body);

        // The chain. Fields are joined with 0x1F (unit separator) rather than a
        // printable delimiter so no field value can forge a boundary -- 0x1F is
        // exactly what flattenLocked has already removed from every body.
        std::string pre = ForumHash::hex16(headLocked());
        pre += '\x1F'; pre += std::to_string(p.seq);
        pre += '\x1F'; pre += p.author;
        pre += '\x1F'; pre += ForumHash::hex16(p.chash);
        p.ihash = ForumHash::of(pre);

        // Authorship claimed by posting, not by opening.
        if (op_.empty())
        {
            op_ = tok;
            ETCS_LOG("ForumThread", "thread '" << thread_key_
                     << "' claimed by first poster " << tok);
        }

        posts_.push_back(p);
        touchLocked(tok);
        persistPostLocked(p);

        return "OK " + std::to_string(p.seq) + " " + ForumHash::hex16(p.ihash);
    }

    // A tombstone is an OBSERVATION of a removal, never a hole. The sequence is
    // not renumbered and the chain is not recomputed, so a reader can always
    // tell that content of a known hash used to be here. Erasing the record
    // instead would make the head change with no evidence of why, which is the
    // exact failure the chain exists to expose.
    std::string tombLocked(const std::string& seq_s, const std::string& tok)
    {
        if (tok.empty()) return "NO IDENTITY";
        const size_t want = ForumWire::parseCursor(seq_s);
        if (want == 0) return "BAD SEQ";

        for (auto& p : posts_)
        {
            if (p.seq != want) continue;
            if (p.tomb) return "ALREADY";
            // The author of the post, or the thread's op. Nobody else, and no
            // server-side override: an operator who wants content gone can stop
            // hosting the thread, which is visible, rather than editing it,
            // which would not be.
            if (tok != p.author && tok != op_) return "NOT YOURS";
            p.tomb = true;
            p.body.clear();
            touchLocked(tok);
            persistPostLocked(p);
            ETCS_LOG("ForumThread", "tombstone on '" << thread_key_ << "' seq "
                     << p.seq << " chash " << ForumHash::hex16(p.chash));
            return "TOMB " + std::to_string(p.seq) + " " + ForumHash::hex16(p.chash);
        }
        return "NOT FOUND";
    }

    std::string titleLocked(const std::string& t, const std::string& tok)
    {
        const std::string want = flattenLocked(percentDecode(t), kMaxTitle);
        if (want.empty()) return title_.empty() ? "-" : title_;
        // Settable by the op, or by anyone while the thread has no op yet --
        // which is the window between creation and the first post.
        if (!op_.empty() && tok != op_) return "NOT YOURS";
        title_ = want;
        touchLocked(tok);
        persistHeaderLocked();
        return title_;
    }

    std::string lockLocked(const std::string& tok)
    {
        if (op_.empty())        return "NO OP";
        if (tok != op_)         return "NOT YOURS";
        locked_ = !locked_;
        touchLocked(tok);
        persistHeaderLocked();
        return locked_ ? "locked" : "open";
    }

    // "<tkey> <title|-> <op|-> open|locked <n> <head16> <readers>"
    // One request tells a client everything it needs to render its controls,
    // rather than inferring state from a second call that could disagree.
    std::string statusLocked() const
    {
        std::string s = thread_key_;
        s += " " + (title_.empty() ? std::string("-") : title_);
        s += " " + (op_.empty()    ? std::string("-") : op_);
        s += locked_ ? " locked" : " open";
        s += " " + std::to_string(posts_.size());
        s += " " + ForumHash::hex16(headLocked());
        s += " " + std::to_string(liveReadersLocked(60));
        return s;
    }

    // THE deterministic rendering. Every read is a window over exactly this
    // string, so the cursor means the same thing to every caller and a client
    // paging through gets a byte-exact concatenation of what a single large
    // response would have been.
    //
    // KNOWN COST, not yet paid down. Rebuilt per request, so paging a thread of
    // n posts at kWire bytes a window is O(n) work repeated O(n) times -- a
    // single reader scrolling one long thread is O(n^2) on this node's ordering
    // thread, blocking every other reader of every other thread on it.
    //
    // Per-node ordering bounds the blast radius to one node; it does not fix
    // the quadratic. The fix is to cache the rendering and invalidate it on
    // mutation (there are exactly four mutators: appendLocked, tombLocked,
    // titleLocked, lockLocked), which is cheap but is a correctness surface of
    // its own and is deliberately not being added in the same change as the
    // ordering restructure.
    std::string renderLocked() const
    {
        std::string out = "#" + statusLocked() + "\n";
        for (const auto& p : posts_)
        {
            out += std::to_string(p.seq);
            out += " " + p.author;
            out += " " + std::to_string(p.made);
            out += " " + ForumHash::hex16(p.chash);
            out += " " + ForumHash::hex16(p.ihash);
            out += " ";
            out += p.tomb ? "!tomb" : p.body;
            out += "\n";
        }
        return out;
    }

    std::string readLocked(const std::string& cursor, const std::string& tok)
    {
        touchLocked(tok);
        return ForumWire::window(renderLocked(), ForumWire::parseCursor(cursor));
    }

    // The verb surface. The node parses paths and calls this; the thread does
    // not know what a URL looks like.
    std::string verbLocked(const std::string& tok, const std::string& verb,
                           const std::string& arg)
    {
        touchLocked(tok);

        if (verb.empty() || verb == "read") return readLocked(arg, tok);
        if (verb == "post")   return appendLocked(arg, tok);
        if (verb == "title")  return titleLocked(arg, tok);
        if (verb == "lock")   return lockLocked(tok);
        if (verb == "tomb")   return tombLocked(arg, tok);
        if (verb == "status") return statusLocked();
        if (verb == "head")   return ForumHash::hex16(headLocked());
        if (verb == "reset")  { resetLocked(); return statusLocked(); }
        return "NOT FOUND";
    }

    std::string thread_key_;                 // equivalence class ("" = unclaimed)
    std::string title_;
    std::string op_;                         // first poster; "" until one lands
    bool        locked_   = false;
    unsigned    next_seq_ = 1;

    std::vector<ForumPost> posts_;

    // Set by ForumNode::createThreadLocked. Null for a standalone thread spawned
    // with no node in front of it, which simply persists nothing -- there is no
    // node to hold the database edge.
    ForumNode* node_ = nullptr;

    Clock::time_point seen_ = Clock::now();
    std::unordered_map<std::string, Clock::time_point> readers_;   // token -> heartbeat
};

#endif // FORUMTHREAD_H__

