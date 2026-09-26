#ifndef NETWORKPROVIDER_EDGE_H__
#define NETWORKPROVIDER_EDGE_H__
#include "../../../ontology.h"
#include "WebSocket.h"

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

/*
 * THE EDGE: everything between two runtimes, on ONE MirrorBuffer.
 *
 * However many surfaces one runtime binds on another, however many verbs it
 * runs and streams it opens, there is one socket between them (WebSocket.h's
 * link_dial, or a channel a LinkHub handed a Room) and one StrategySocket
 * pair on it. Everything is a frame on that pair:
 *
 *   'B' id name module tag hash manifest     bind a surface to an export
 *   'b' id ok far-rid state | reason         state: the node's EnvironmentState
 *   'C' id rid hash verb wrapped-payload      run a verb on a bound export
 *   'c' id ok wrapped-answer | reason
 *   'O' id ch name verb config dir hash manifest   open a stream channel
 *   'o' id ok | reason
 *   'D' ch bytes                              a channel's bytes
 *   'E' ch                                    its producer finished
 *   'X' ch                                    its consumer is gone
 *
 * A STREAM IS A CHANNEL, NOT A CONNECTION. Each side of a remote pair runs
 * its half on a local pipe with a StrategySocket MirrorBuffer on it -- the
 * ordinary pair machinery, wrap chain included -- and the edge carries that
 * pipe's bytes, already framed and wrapped, as 'D' frames under the
 * channel's number. The frames inside ride along the edge's frames; nothing
 * here reads them. Channel numbers are odd from the side that dialled, even
 * from the side that accepted, so both may open channels at once.
 *
 * THE AUTHORITY LAYER IS CHECKED ON EVERY OPENING. A bind and a stream both
 * name the manifest of the asker's surface -- its Wrapper children on the
 * network scope, with their code hashes (MirrorBuffer::manifestOf) -- and
 * the answering side refuses unless its node carries exactly that. Verbs
 * pass the chain too: wrapped by the surface's stages here, unwrapped by the
 * node's there, and the same for the answer, so a stage that refuses
 * (wire_refuse) refuses the verb.
 *
 * SYMMETRIC. Either side may export and either may ask; a Room and a Peer
 * differ only in who dialled.
 *
 * Requests from the far side run on the edge's reader, in arrival order --
 * a verb holds the edge until it answers, as a local verb holds its caller.
 * A stream's bytes are handed to a per-channel writer, so a slow consumer
 * delays only its own channel until 4 MiB are queued for it.
 */
namespace etcs_link
{

// Little-endian fields in a string -- what the frames above are made of.
struct Writer
{
    std::string s;
    Writer& u8(uint8_t v)   { s += static_cast<char>(v); return *this; }
    Writer& u32(uint32_t v) { s.append(reinterpret_cast<const char*>(&v), 4); return *this; }
    Writer& u64(uint64_t v) { s.append(reinterpret_cast<const char*>(&v), 8); return *this; }
    Writer& str(const std::string& v) { u32(static_cast<uint32_t>(v.size())); s += v; return *this; }
    Writer& raw(const char* p, size_t n) { s.append(p, n); return *this; }
};
struct Reader
{
    const std::string& s;
    size_t at = 0;
    bool   ok = true;
    explicit Reader(const std::string& x, size_t start = 0) : s(x), at(start) {}
    template <typename T> T num()
    {
        T v{};
        if (at + sizeof(T) > s.size()) { ok = false; return v; }
        std::memcpy(&v, s.data() + at, sizeof(T));
        at += sizeof(T);
        return v;
    }
    std::string str()
    {
        const uint32_t n = num<uint32_t>();
        if (!ok || at + n > s.size()) { ok = false; return {}; }
        std::string v = s.substr(at, n);
        at += n;
        return v;
    }
    std::string rest() { std::string v = at < s.size() ? s.substr(at) : std::string(); at = s.size(); return v; }
};

inline uint64_t random_token()
{
    static std::atomic<uint64_t> n{ static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count()) * 0x9E3779B97F4A7C15ull };
    uint64_t x = n.fetch_add(0xD1B54A32D192ED03ull);
    x ^= x >> 31; x *= 0x7FB5D329728EA185ull; x ^= x >> 27; x *= 0x81DADEF4BC2DD44Dull; x ^= x >> 33;
    return x ? x : 1;
}

// Joins the threads an owner started, when it goes: they hold `this`.
class Threads
{
public:
    void start(std::function<void()> body)
    {
        live_.fetch_add(1, std::memory_order_acq_rel);
        etcs_ws::Pumps::get().start([this, body = std::move(body)]()
        {
            body();
            live_.fetch_sub(1, std::memory_order_acq_rel);
        });
    }
    void wait() const
    {
        while (live_.load(std::memory_order_acquire) > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    int live() const { return live_.load(std::memory_order_acquire); }
private:
    std::atomic<int> live_{ 0 };
};

// Request/answer matching for whichever side asks.
class Pending
{
public:
    uint32_t open()
    {
        std::lock_guard<std::mutex> lock(mu_);
        const uint32_t id = ++next_;
        slots_[id];
        return id;
    }
    void answer(uint32_t id, std::string body)
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = slots_.find(id);
        if (it == slots_.end()) return;
        it->second.body = std::move(body);
        it->second.done = true;
        cv_.notify_all();
    }
    bool wait(uint32_t id, std::string& body, int timeout_ms)
    {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&]
            { auto it = slots_.find(id); return it == slots_.end() || it->second.done || closed_; });
        auto it = slots_.find(id);
        const bool ok = it != slots_.end() && it->second.done;
        if (ok) body = std::move(it->second.body);
        if (it != slots_.end()) slots_.erase(it);
        return ok;
    }
    void close() { std::lock_guard<std::mutex> lock(mu_); closed_ = true; cv_.notify_all(); }
private:
    struct Slot { bool done = false; std::string body; };
    std::mutex               mu_;
    std::condition_variable  cv_;
    std::map<uint32_t, Slot> slots_;
    uint32_t                 next_   = 0;
    bool                     closed_ = false;
};

/*
 * WHO IS ASKING, while a request from the far side runs here: the edge it
 * arrived on and the name that side gave when it said hello. A Record's
 * author and a Directory entry's liveness are this, not a field the caller
 * wrote (Ledger.h, Lobby.h). Empty for anything run locally.
 */
struct Caller { uint64_t edge = 0; std::string name; };
inline Caller& caller() { static thread_local Caller c; return c; }
struct CallerScope
{
    Caller saved;
    CallerScope(uint64_t e, const std::string& n) : saved(caller()) { caller() = Caller{ e, n }; }
    ~CallerScope() { caller() = saved; }
};

// Told when an edge closes, with its id -- what ties an advert's life to its
// advertiser's link.
class EdgeWatch
{
public:
    static EdgeWatch& get() { static EdgeWatch w; return w; }
    int add(std::function<void(uint64_t)> fn)
    {
        std::lock_guard<std::mutex> lock(mu_);
        fns_[++next_] = std::move(fn);
        return next_;
    }
    void remove(int k) { std::lock_guard<std::mutex> lock(mu_); fns_.erase(k); }
    void fire(uint64_t edge)
    {
        std::map<int, std::function<void(uint64_t)>> copy;
        { std::lock_guard<std::mutex> lock(mu_); copy = fns_; }
        for (auto& [k, fn] : copy) fn(edge);
    }
private:
    std::mutex mu_;
    std::map<int, std::function<void(uint64_t)>> fns_;
    int next_ = 0;
};

// What one side lets the other reach: names, and nothing else.
class Exports
{
public:
    bool Publish(const std::string& name, ETCS::RID rid)
    {
        if (name.empty() || rid == 0) return false;
        std::lock_guard<std::mutex> lock(mu_);
        map_[name] = rid;
        return true;
    }
    bool Unpublish(const std::string& name) { std::lock_guard<std::mutex> lock(mu_); return map_.erase(name) > 0; }
    ETCS::Entity* find(const std::string& name, ETCS::RID* rid_out = nullptr)
    {
        ETCS::RID rid = 0;
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = map_.find(name);
            if (it == map_.end()) return nullptr;
            rid = it->second;
        }
        if (rid_out) *rid_out = rid;
        return ETCS::etcs_resolve_rid_anywhere(&ETCS::getLoader(), rid);
    }
    ETCS::Entity* byRid(ETCS::RID rid)
    {
        {
            std::lock_guard<std::mutex> lock(mu_);
            bool listed = false;
            for (auto& [n, r] : map_) if (r == rid) { listed = true; break; }
            if (!listed) return nullptr;
        }
        return ETCS::etcs_resolve_rid_anywhere(&ETCS::getLoader(), rid);
    }
    std::map<std::string, ETCS::RID> snapshot() { std::lock_guard<std::mutex> lock(mu_); return map_; }
private:
    std::mutex                       mu_;
    std::map<std::string, ETCS::RID> map_;
};

// ── The hello, before the edge exists ───────────────────────────────────────
// Exact frames on the bare socket (etcs_ws::send_frame/recv_frame): nothing
// may be read past them, the pair takes the socket over next.
inline bool hello_dial(int fd, const std::string& my_name, std::string& far_name)
{
    std::string w;
    if (!etcs_ws::send_frame(fd, Writer().u8('H').u8(2).str(my_name).s)
        || !etcs_ws::recv_frame(fd, w, 10000) || w.empty() || w[0] != 'W') return false;
    Reader r(w, 1);
    far_name = r.str();
    return r.ok;
}
inline bool hello_accept(int fd, const std::string& my_name, std::string& far_name)
{
    std::string h;
    if (!etcs_ws::recv_frame(fd, h, 10000) || h.size() < 2 || h[0] != 'H' || h[1] != 2) return false;
    Reader r(h, 2);
    far_name = r.str();
    return r.ok && etcs_ws::send_frame(fd, Writer().u8('W').str(my_name).s);
}

class Edge
{
public:
    static constexpr size_t kChunk     = 3072;             // bytes of a channel per 'D' frame
    static constexpr size_t kQueueCap  = 4u << 20;         // queued for one slow consumer
    static constexpr int    kAnswerMs  = 15000;

    Edge(int fd, ETCS::MemoryArena& arena, bool dialer, std::string far_name, Exports* exports)
        : fd_(fd), dialer_(dialer), far_name_(std::move(far_name)), exports_(exports),
          id_(random_token()), next_ch_(dialer ? 1 : 2)
    {
        ctx_.interrupt = &stop_;
        out_.bindContext(ctx_);
        in_.bindContext(ctx_);
        page_ = ETCS::SharedPage::allocate(arena, 0);
        ETCS::MirrorBuffer::makePair<ETCS::StrategySocket, ETCS::SharedPage>(
            out_, in_, page_, static_cast<uint64_t>(fd), static_cast<uint64_t>(fd));
    }
    ~Edge()
    {
        stop();
        ETCS::MirrorBuffer::teardownPair<ETCS::StrategySocket, ETCS::SharedPage>(out_, in_, page_);
    }
    Edge(const Edge&) = delete;
    Edge& operator=(const Edge&) = delete;

    void start() { threads_.start([this]() { run(); }); }

    // Ends the reader, every channel and every half this side was running
    // for the far one, and waits for all of them.
    void stop()
    {
        stop_.store(1, std::memory_order_release);
        ::shutdown(fd_, SHUT_RDWR);
        {
            std::lock_guard<std::mutex> lock(ch_mu_);
            for (auto& [n, c] : chans_) c->kill();
        }
        pending_.close();
        threads_.wait();
    }
    bool isOpen() const { return open_.load(std::memory_order_acquire) && !stop_.load(); }
    uint64_t id() const { return id_; }
    const std::string& farName() const { return far_name_; }
    size_t channels() { std::lock_guard<std::mutex> lock(ch_mu_); return chans_.size(); }

    // ── Asking the far side ─────────────────────────────────────────────────

    // `state`: what the far node says a reflection of it should show
    // (ontology/Environmental.h), packed; empty when it claims no such thing.
    bool bind(const std::string& name, const std::string& module, const std::string& tag,
              uint64_t tag_hash, const std::string& manifest, ETCS::RID& far, std::string& state,
              std::string& why)
    {
        std::string body;
        if (!ask('B', Writer().str(name).str(module).str(tag).u64(tag_hash).str(manifest).s, body))
        { why = "no answer from " + far_name_; return false; }
        Reader r(body);
        if (r.num<uint8_t>() != 1) { why = r.str(); return false; }
        far   = r.num<uint64_t>();
        state = r.str();
        return r.ok;
    }

    // `owner`'s network-scope stages wrap the payload and unwrap the answer.
    bool work(ETCS::RID far, uint64_t hash, const std::string& verb, ETCS::Buffer& data,
              ETCS::Entity* owner, std::string& why)
    {
        ETCS::MBuffer wire;
        if (data.written) wire.writeRaw(data.buf, data.written);
        wrap(owner, wire);
        std::string body;
        if (!ask('C', Writer().u64(far).u64(hash).str(verb).raw(wire.buf, wire.written).s, body) || body.empty())
        { why = "no answer from " + far_name_; data.reset(); return false; }
        if (body[0] != 1) { why = body.substr(1); data.reset(); return false; }
        ETCS::MBuffer ans;
        if (body.size() > 1) ans.writeRaw(body.data() + 1, body.size() - 1);
        if (!unwrap(owner, ans) || ans.written >= ETCS::Buffer::bufsize)
        { why = "the answer did not pass this side's authority layer"; data.reset(); return false; }
        data.reset();
        if (ans.written) data.writeRaw(ans.buf, ans.written);
        return true;
    }

    /*
     * A channel whose far end runs `name.verb(config)` as one half of a pair.
     * The fd returned is this side's end of the local pipe: read it when the
     * far side produces, write it when it consumes -- a StrategySocket half
     * goes on it (Entity::consumeFrom / produceOnto). -1 and `why` if refused.
     */
    int openStream(const std::string& name, const std::string& verb, const std::string& config,
                   bool far_produces, uint64_t hash, const std::string& manifest, std::string& why)
    {
        if (!isOpen()) { why = "the link is closed"; return -1; }
        int p[2];
        if (::pipe(p) != 0) { why = "pipe failed"; return -1; }
        const int mine = far_produces ? p[0] : p[1];   // the half's end
        const int mux  = far_produces ? p[1] : p[0];   // this edge's end
        etcs_ws::set_nonblocking(mux);
        const uint32_t ch = nextChannel();
        auto c = std::make_shared<Channel>(ch, mux, far_produces /* inbound */);
        { std::lock_guard<std::mutex> lock(ch_mu_); chans_[ch] = c; }

        std::string body;
        const bool asked = ask('O', Writer().u32(ch).str(name).str(verb).str(config)
                                   .u8(far_produces ? 1 : 0).u64(hash).str(manifest).s, body);
        if (!asked || body.empty() || body[0] != 1)
        {
            why = !asked ? "no answer from " + far_name_ : body.size() > 1 ? body.substr(1) : "refused";
            dropChannel(ch);
            ::close(mux); ::close(mine);
            return -1;
        }
        runChannel(c);
        return mine;
    }

private:
    struct Channel
    {
        Channel(uint32_t n, int f, bool in) : id(n), fd(f), inbound(in) {}
        uint32_t                id;
        int                     fd;
        bool                    inbound;
        std::mutex              mu;
        std::condition_variable cv;
        std::deque<std::string> q;
        size_t                  bytes = 0;
        bool                    ended = false;
        std::atomic<bool>       dead{ false };
        void kill() { dead.store(true); std::lock_guard<std::mutex> lock(mu); cv.notify_all(); }
    };

    uint32_t nextChannel() { std::lock_guard<std::mutex> lock(ch_mu_); const uint32_t n = next_ch_; next_ch_ += 2; return n; }
    std::shared_ptr<Channel> channel(uint32_t n)
    {
        std::lock_guard<std::mutex> lock(ch_mu_);
        auto it = chans_.find(n);
        return it == chans_.end() ? nullptr : it->second;
    }
    void dropChannel(uint32_t n) { std::lock_guard<std::mutex> lock(ch_mu_); chans_.erase(n); }

    bool send(const std::string& msg)
    {
        std::lock_guard<std::mutex> lock(send_mu_);
        return !stop_.load() && out_.writeFrame(msg.data(), msg.size());
    }
    bool ask(char op, const std::string& body, std::string& answer)
    {
        if (!isOpen()) return false;
        const uint32_t id = pending_.open();
        if (!send(Writer().u8(static_cast<uint8_t>(op)).u32(id).s + body)) return false;
        return pending_.wait(id, answer, kAnswerMs);
    }
    void reply(char op, uint32_t id, const std::string& body)
    { send(Writer().u8(static_cast<uint8_t>(op)).u32(id).s + body); }

    void wrap(ETCS::Entity* owner, ETCS::MBuffer& m)
    {
        ETCS::IWireWrapper* chain[ETCS::MAX_WRAP_STAGES] = {};
        const size_t n = ETCS::MirrorBuffer::chainOf(owner, ETCS::WireScope::Socket, chain, ETCS::MAX_WRAP_STAGES);
        for (size_t i = 0; i < n; ++i) chain[i]->Wrap(m, ctx_);
    }
    bool unwrap(ETCS::Entity* owner, ETCS::MBuffer& m)
    {
        ETCS::IWireWrapper* chain[ETCS::MAX_WRAP_STAGES] = {};
        const size_t n = ETCS::MirrorBuffer::chainOf(owner, ETCS::WireScope::Socket, chain, ETCS::MAX_WRAP_STAGES);
        for (size_t i = n; i-- > 0; )
        {
            chain[i]->Unwrap(m, ctx_);
            if (ETCS::wire_refused(m)) return false;
        }
        return true;
    }

    // ── The reader: one frame at a time, in order ──────────────────────────
    void run()
    {
        std::string msg;
        while (!stop_.load())
        {
            ETCS::MBuffer f;
            if (!in_.readFrame(f)) break;
            msg.assign(f.buf, f.written);
            if (msg.size() < 5) continue;
            Reader r(msg, 1);
            const uint32_t id = r.num<uint32_t>();
            switch (msg[0])
            {
                case 'b': case 'c': case 'o': pending_.answer(id, msg.substr(5)); break;
                case 'B': reply('b', id, answerBind(r)); break;
                case 'C': reply('c', id, answerCall(r)); break;
                case 'O': reply('o', id, answerOpen(r)); break;
                case 'D':
                    if (auto c = channel(id)) queue(c, msg.substr(5));
                    break;
                case 'E':
                    if (auto c = channel(id)) { std::lock_guard<std::mutex> lock(c->mu); c->ended = true; c->cv.notify_all(); }
                    break;
                case 'X':
                    if (auto c = channel(id)) c->kill();
                    break;
                default: break;
            }
        }
        open_.store(false);
        stop_.store(1);
        {
            std::lock_guard<std::mutex> lock(ch_mu_);
            for (auto& [n, c] : chans_) c->kill();
        }
        pending_.close();
        EdgeWatch::get().fire(id_);
        ETCS_LOG("Edge", "link with '" << far_name_ << "' closed.");
    }

    ETCS::Entity* exported(const std::string& name, ETCS::RID* rid = nullptr)
    { return exports_ ? exports_->find(name, rid) : nullptr; }

    // A surface binds only to its own type and its own authority layer.
    std::string answerBind(Reader& r)
    {
        const std::string name = r.str(), module = r.str(), tag = r.str();
        const uint64_t hash = r.num<uint64_t>();
        const std::string manifest = r.str();
        Writer w;
        ETCS::RID rid = 0;
        ETCS::Entity* node = r.ok ? exported(name, &rid) : nullptr;
        if (!node) return w.u8(0).str("no export named '" + name + "'").s;
        if (ETCS::MirrorBuffer::localFrame(node))
            return w.u8(0).str("'" + name + "' is of this runtime's local frame and does not cross").s;
        const std::string have_mod = node->getSourceModule().toString(), have_tag = node->getSourceTag().toString();
        if (have_mod != module || have_tag != tag)
            return w.u8(0).str("'" + name + "' is a " + have_mod + "::" + have_tag + ", not a " + module + "::" + tag).s;
        const uint64_t mine = node->tagHash(ETCS::Buffer(tag.c_str()));
        if (mine == 0 || mine != hash)
            return w.u8(0).str("'" + name + "' is a different build of " + module + "::" + tag).s;
        const std::string guard = ETCS::MirrorBuffer::manifestOf(node, ETCS::WireScope::Socket);
        if (guard != manifest)
            return w.u8(0).str("'" + name + "' is guarded by [" + guard + "], the surface carries ["
                               + manifest + "]").s;
        // The far frame's half of Environmental: the named values a reflection
        // starts from. Its tags are the surface's own already -- the same type.
        ETCS::EnvironmentState st;
        if (void* env = node->getInterfacePointer(ETCS::Buffer("Environmental")))
            static_cast<Environmental_*>(env)->CaptureState(st);
        std::string packed = st.pack();
        if (packed.size() > ETCS::MirrorBuffer::MAX_FRAME_PAYLOAD - 64)
        {
            ETCS_LOG("Edge", "'" << name << "': its state (" << packed.size()
                     << " bytes) is more than one frame; the surface starts without it.");
            packed.clear();
        }
        ETCS_LOG("Edge", "'" << far_name_ << "' bound a surface to '" << name << "' (RID:" << rid << ")");
        return w.u8(1).u64(rid).str(packed).s;
    }

    std::string answerCall(Reader& r)
    {
        const ETCS::RID rid = r.num<uint64_t>();
        const uint64_t hash = r.num<uint64_t>();
        const std::string verb = r.str();
        const std::string wire = r.rest();
        Writer w;
        ETCS::Entity* node = (r.ok && exports_) ? exports_->byRid(rid) : nullptr;
        if (!node) return w.u8(0).raw("not an export", 13).s;
        const std::string conj = node->getSourceTag().toString() + "." + verb;
        if (node->actionHash(ETCS::Buffer(conj.c_str())) != hash)
            return w.u8(0).raw("different build of this verb", 28).s;
        ETCS::MBuffer m;
        if (!wire.empty()) m.writeRaw(wire.data(), wire.size());
        if (!unwrap(node, m)) return w.u8(0).raw("refused by the node's authority layer", 37).s;
        if (m.written >= ETCS::Buffer::bufsize) return w.u8(0).raw("payload too large", 17).s;
        ETCS::Buffer buf;
        if (m.written) buf.writeRaw(m.buf, m.written);
        try
        {
            CallerScope who(id_, far_name_);
            node->call(ETCS::Buffer(conj.c_str()), buf);
        }
        catch (const std::exception& ex) { return w.u8(0).raw(ex.what(), std::strlen(ex.what())).s; }
        ETCS::MBuffer ans;
        if (buf.written) ans.writeRaw(buf.buf, buf.written);
        wrap(node, ans);
        return w.u8(1).raw(ans.buf, ans.written).s;
    }

    std::string answerOpen(Reader& r)
    {
        const uint32_t ch = r.num<uint32_t>();
        const std::string name = r.str(), verb = r.str(), config = r.str();
        const bool produces = r.num<uint8_t>() != 0;       // this side produces
        const uint64_t hash = r.num<uint64_t>();
        const std::string manifest = r.str();
        Writer w;
        ETCS::Entity* node = r.ok ? exported(name) : nullptr;
        if (!node) return w.u8(0).raw("no such export", 14).s;
        if (ETCS::MirrorBuffer::localFrame(node)) return w.u8(0).raw("of the local frame", 18).s;
        const std::string conj = node->getSourceTag().toString() + "." + verb;
        if (node->actionHash(ETCS::Buffer(conj.c_str())) != hash)
            return w.u8(0).raw("different build of this stream", 30).s;
        if (ETCS::MirrorBuffer::manifestOf(node, ETCS::WireScope::Socket) != manifest)
            return w.u8(0).raw("the authority layer differs", 27).s;
        if (channel(ch) || (ch % 2) == (dialer_ ? 1u : 0u)) return w.u8(0).raw("bad channel", 11).s;
        int p[2];
        if (::pipe(p) != 0) return w.u8(0).raw("pipe failed", 11).s;
        const int mine = produces ? p[1] : p[0];
        const int mux  = produces ? p[0] : p[1];
        etcs_ws::set_nonblocking(mux);
        auto c = std::make_shared<Channel>(ch, mux, !produces /* inbound */);
        { std::lock_guard<std::mutex> lock(ch_mu_); chans_[ch] = c; }
        runChannel(c);
        ETCS::Buffer cfg;
        if (!config.empty()) cfg.write(config.c_str());
        const ETCS::RID rid = node->getRID();
        const std::string who = far_name_;
        ETCS_LOG("Edge", "'" << who << "' opened " << name << "." << verb
                 << (produces ? " (producing here)" : " (consuming here)") << " on channel " << ch);
        threads_.start([this, rid, conj, cfg, mine, produces, who]()
        {
            ETCS::Entity* n = ETCS::etcs_resolve_rid_anywhere(&ETCS::getLoader(), rid);
            if (!n) { ::close(mine); return; }
            CallerScope scope(id_, who);
            if (produces) n->produceOnto(ETCS::Buffer(conj.c_str()), cfg, mine, ctx_);
            else          n->consumeFrom(ETCS::Buffer(conj.c_str()), cfg, mine, ctx_);
        });
        return w.u8(1).s;
    }

    // ── Channels ───────────────────────────────────────────────────────────
    void queue(const std::shared_ptr<Channel>& c, std::string bytes)
    {
        std::unique_lock<std::mutex> lock(c->mu);
        // Back-pressure, bounded: past the cap the reader waits for this
        // consumer, which stalls the whole edge -- the price of one socket.
        c->cv.wait(lock, [&] { return c->bytes < kQueueCap || c->dead.load() || stop_.load(); });
        if (c->dead.load()) return;
        c->bytes += bytes.size();
        c->q.push_back(std::move(bytes));
        c->cv.notify_all();
    }

    void runChannel(const std::shared_ptr<Channel>& c)
    {
        if (c->inbound) threads_.start([this, c]() { pumpIn(c); });
        else            threads_.start([this, c]() { pumpOut(c); });
    }

    // Far producer -> local consumer: the queue, written into the pipe.
    void pumpIn(std::shared_ptr<Channel> c)
    {
        while (true)
        {
            std::string next;
            {
                std::unique_lock<std::mutex> lock(c->mu);
                c->cv.wait(lock, [&] { return !c->q.empty() || c->ended || c->dead.load(); });
                if (c->dead.load()) break;
                if (c->q.empty()) break;                         // ended and drained
                next = std::move(c->q.front());
                c->q.pop_front();
                c->bytes -= next.size();
                c->cv.notify_all();
            }
            if (!writeAll(c, next)) { send(Writer().u8('X').u32(c->id).s); break; }
        }
        ::close(c->fd);
        dropChannel(c->id);
    }

    // Local producer -> far consumer: the pipe, read into 'D' frames.
    void pumpOut(std::shared_ptr<Channel> c)
    {
        char buf[kChunk];
        bool eof = false;
        while (!c->dead.load() && !stop_.load())
        {
            pollfd p{ c->fd, POLLIN, 0 };
            if (::poll(&p, 1, 100) <= 0) continue;
            const ssize_t n = ::read(c->fd, buf, sizeof buf);
            if (n > 0) { if (!send(Writer().u8('D').u32(c->id).raw(buf, static_cast<size_t>(n)).s)) break; continue; }
            if (n == 0) { eof = true; break; }
            // An emscripten pipe at EOF answers EAGAIN and says POLLHUP
            // (MirrorBuffer::waitFd): nothing left, and nobody writing.
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            {
                if (p.revents & POLLHUP) { eof = true; break; }
                continue;
            }
            break;
        }
        if (eof) send(Writer().u8('E').u32(c->id).s);
        ::close(c->fd);
        dropChannel(c->id);
    }

    bool writeAll(const std::shared_ptr<Channel>& c, const std::string& s)
    {
        size_t off = 0;
        while (off < s.size())
        {
            if (c->dead.load() || stop_.load()) return false;
            const ssize_t n = ::write(c->fd, s.data() + off, s.size() - off);
            if (n > 0) { off += static_cast<size_t>(n); continue; }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
            {
                pollfd p{ c->fd, POLLOUT, 0 };
                ::poll(&p, 1, 100);
                continue;
            }
            return false;                                     // the local consumer is gone
        }
        return true;
    }

    int                 fd_;
    bool                dialer_;
    std::string         far_name_;
    Exports*            exports_;
    uint64_t            id_;
    ETCS::SignalFlag    stop_{ 0 };
    ETCS::SignalContext ctx_;
    ETCS::SharedPage*   page_ = nullptr;
    ETCS::MirrorBuffer  out_, in_;
    std::mutex          send_mu_;
    std::atomic<bool>   open_{ true };      // until the reader ends
    Pending             pending_;
    Threads             threads_;
    std::mutex          ch_mu_;
    std::map<uint32_t, std::shared_ptr<Channel>> chans_;
    uint32_t            next_ch_;
};

} // namespace etcs_link

#endif // NETWORKPROVIDER_EDGE_H__
