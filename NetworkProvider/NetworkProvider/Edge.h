#ifndef NETWORKPROVIDER_EDGE_H__
#define NETWORKPROVIDER_EDGE_H__
#include "../../../ontology.h"
#include "../../../libs/monocypher.h"
#include "WebSocket.h"

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <cerrno>
#include <sstream>
#include <string>
#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#elif defined(__linux__) && !defined(__EMSCRIPTEN__)
#include <sys/random.h>
#endif

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
 * here reads them -- a stream's messages (MirrorBuffer::writeMessage) cross
 * as the run of frames they already are.
 *
 * EACH OF THE EDGE'S OWN IS A MESSAGE, not a frame (writeMessage/readMessage),
 * so an answer is as big as it needs to be: a bound node's state is the
 * surface's starting point, and a record's is not going to fit 4 KiB. Channel numbers are odd from the side that dialled, even
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
 * SEALED, BETWEEN TWO IDENTITIES. The hello (below) is a handshake: each side
 * makes a fresh exchange key, and each side's LOADER signs the transcript of
 * both (core/Trust.h, vouch) -- its key alone for a local identity, its chain
 * to a root besides for a registered one -- and checks the other's
 * (recognize). The two exchange keys give a key each way, and from then on
 * every message of the edge is sealed under them (XChaCha20-Poly1305, the
 * Envelope's, numbered: one out of order, replayed or altered closes the
 * link). A hub between the two splices bytes and reads none, so it is sealed
 * end to end. What the far side proved is stamped on every verb and stream
 * half run for it (SignalContext::peer) -- its key, not the name it said.
 *
 * Requests from the far side run on the edge's reader, in arrival order --
 * a verb holds the edge until it answers, as a local verb holds its caller,
 * and the asker waits for that answer for as long as the link lives (Pending).
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
    bool bytes(uint8_t* out, size_t n)
    {
        if (!ok || at + n > s.size()) { ok = false; return false; }
        std::memcpy(out, s.data() + at, n);
        at += n;
        return true;
    }
};

inline uint64_t random_token()
{
    static std::atomic<uint64_t> n{ static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count()) * 0x9E3779B97F4A7C15ull };
    uint64_t x = n.fetch_add(0xD1B54A32D192ED03ull);
    x ^= x >> 31; x *= 0x7FB5D329728EA185ull; x ^= x >> 27; x *= 0x81DADEF4BC2DD44Dull; x ^= x >> 33;
    return x ? x : 1;
}

// The operating system's randomness, whole or not at all: an exchange key.
inline bool random_bytes(uint8_t* out, size_t n)
{
#if defined(__EMSCRIPTEN__)
    for (size_t at = 0; at < n; at += 256)
        if (::getentropy(out + at, n - at < 256 ? n - at : 256) != 0) return false;
    return true;
#elif defined(__linux__)
    size_t at = 0;
    while (at < n)
    {
        const ssize_t r = ::getrandom(out + at, n - at, 0);
        if (r < 0) { if (errno == EINTR) continue; return false; }
        at += static_cast<size_t>(r);
    }
    return true;
#elif defined(_WIN32)
    return BCryptGenRandom(nullptr, out, static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    (void)out; (void)n;
    return false;
#endif
}
inline std::string hex(const uint8_t* p, size_t n)
{
    static const char* d = "0123456789abcdef";
    std::string s(2 * n, '0');
    for (size_t i = 0; i < n; ++i) { s[2 * i] = d[p[i] >> 4]; s[2 * i + 1] = d[p[i] & 15]; }
    return s;
}
inline bool unhex(const std::string& s, uint8_t* out, size_t n)
{
    if (s.size() != 2 * n) return false;
    auto v = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    for (size_t i = 0; i < n; ++i)
    {
        const int hi = v(s[2 * i]), lo = v(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<uint8_t>(hi << 4 | lo);
    }
    return true;
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

/*
 * Request/answer matching for whichever side asks.
 *
 * AN ANSWER IS WAITED FOR AS LONG AS THE LINK LIVES. Whether the far side is
 * still there is the transport's to say (WsEnd: 35 s without a byte from it
 * closes the link, and the close wakes every wait here); how long it takes to
 * answer is not. A far side that is busy answers late, not never -- a browser
 * tab moves its socket's bytes only when its main thread runs -- and a
 * wall-clock deadline here was a guess at what the transport already knows:
 * under load it ran out on a bind the far side had answered, and the surface
 * was never bound. A remote verb now holds its caller as a local one does,
 * until the answer, the link's close, or the asker being interrupted.
 */
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
    // `asker`, if given, is looked at between slices: an interrupted asker
    // stops waiting, and an answer that comes after is dropped (answer()).
    bool wait(uint32_t id, std::string& body, const ETCS::SignalContext* asker)
    {
        std::unique_lock<std::mutex> lock(mu_);
        while (true)
        {
            auto it = slots_.find(id);
            if (it == slots_.end() || it->second.done || closed_) break;
            if (asker && (asker->isInterrupted() || asker->isTerminated())) break;
            cv_.wait_for(lock, std::chrono::milliseconds(kSliceMs));
        }
        auto it = slots_.find(id);
        const bool ok = it != slots_.end() && it->second.done;
        if (ok) body = std::move(it->second.body);
        if (it != slots_.end()) slots_.erase(it);
        return ok;
    }
    void close() { std::lock_guard<std::mutex> lock(mu_); closed_ = true; cv_.notify_all(); }
private:
    static constexpr int kSliceMs = 100;   // how soon an interrupted asker is noticed
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
//
// NEVER DESTROYED, on purpose. Its watchers (Lobby) live in the module's
// MemoryArena, which is torn down at exit; a function-local static built after
// the arena is destroyed BEFORE it, so ~Lobby's remove() ran on a freed map --
// an erase on dead tree nodes, reported by glibc as "corrupted size vs.
// prev_size". A leaked singleton outlives every arena that can hold a watcher.
class EdgeWatch
{
public:
    static EdgeWatch& get() { static EdgeWatch* w = new EdgeWatch; return *w; }
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
/*
 * What a link may reach here, by name -- and, per name, WHICH VERBS. A node
 * published bare answers to everything it has; one published with a verb
 * list answers to those alone, so a record can be published to be read
 * (Head, Since, Follow) while what writes it is published elsewhere, behind
 * its own authority layer. The far side sees a verb not listed as one the
 * node does not have.
 */
class Exports
{
public:
    struct Export { ETCS::RID rid = 0; std::set<std::string> verbs; };   // empty: all

    bool Publish(const std::string& name, ETCS::RID rid, std::set<std::string> verbs = {})
    {
        if (name.empty() || rid == 0) return false;
        std::lock_guard<std::mutex> lock(mu_);
        map_[name] = Export{ rid, std::move(verbs) };
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
            rid = it->second.rid;
        }
        if (rid_out) *rid_out = rid;
        return ETCS::etcs_resolve_rid_anywhere(&ETCS::getLoader(), rid);
    }
    ETCS::Entity* byRid(ETCS::RID rid)
    {
        {
            std::lock_guard<std::mutex> lock(mu_);
            bool listed = false;
            for (auto& [n, e] : map_) if (e.rid == rid) { listed = true; break; }
            if (!listed) return nullptr;
        }
        return ETCS::etcs_resolve_rid_anywhere(&ETCS::getLoader(), rid);
    }
    // Whether `verb` is reachable on what `rid` was published as (any of
    // its names: a node published twice is reachable as the union).
    bool allows(ETCS::RID rid, const std::string& verb)
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& [n, e] : map_)
            if (e.rid == rid && (e.verbs.empty() || e.verbs.count(verb))) return true;
        return false;
    }
    std::map<std::string, Export> snapshot() { std::lock_guard<std::mutex> lock(mu_); return map_; }
private:
    std::mutex                    mu_;
    std::map<std::string, Export> map_;
};

// ── The hello, before the edge exists ───────────────────────────────────────
// Exact frames on the bare socket (etcs_ws::send_frame/recv_frame): nothing
// may be read past them, the pair takes the socket over next.
//
//   dialler   'H' 3 <name> <exchange key>
//   accepter  'W' <name> <exchange key> <its word>      or 'N' <reason>
//   dialler   'P' <its word>
//   accepter  'K'                                        or 'N' <reason>
//
// A word is "<key> <chain|-> <epoch> <signature>" over the transcript -- both
// names and both exchange keys -- and the epoch its loader runs in, as that
// loader gave it (TrustEvent Vouch), and is checked by the other's loader
// (Recognize). A LINK IS BETWEEN RUNTIMES OF ONE EPOCH: a word said in another
// is refused on both sides -- the dialler, who sees it first, says its own
// word all the same, so the accepter's loader learns what the dialler's did
// and each can ask its remotes whether it is the one behind (core/Trust.h,
// recognize). The dialler waits for its welcome
// as long as the socket lives: a host reached through a hub comes for a guest
// when it gets to it (LinkHub), and answers then. The accepting side keeps its
// deadline -- the guest's hello is sent on connecting, so only a guest that
// says nothing runs it out.
struct Link
{
    std::string far_name;
    ETCS::Peer  far;                  // what it proved
    uint8_t     out[32] = {}, in[32] = {};
    ~Link() { etcs_monocypher::crypto_wipe(out, 32); etcs_monocypher::crypto_wipe(in, 32); }
};

namespace hello
{
inline void transcript(const std::string& dialler, const uint8_t dx[32], const std::string& accepter,
                       const uint8_t ax[32], uint8_t t[32])
{
    const std::string m = Writer().raw("ETCSLNK0", 8).str(dialler).raw(reinterpret_cast<const char*>(dx), 32)
                                   .str(accepter).raw(reinterpret_cast<const char*>(ax), 32).s;
    etcs_monocypher::crypto_blake2b(t, 32, reinterpret_cast<const uint8_t*>(m.data()), m.size());
}
// This loader's word on the link, as side `side` ('d' dialled, 'a' accepted).
inline bool vouch(char side, const uint8_t t[32], std::string& word, std::string& why)
{
    ETCS::TrustEvent q(ETCS::TrustEvent::Op::Vouch);
    q.name = std::string(1, side);
    q.a = hex(t, 32);
    const bool ok = q();
    (ok ? word : why) = q.out;
    return ok;
}
// The far side's word, checked by this loader: what it proved, or why not.
// `parted` says the far side runs in another epoch.
inline bool recognize(char side, const uint8_t t[32], const std::string& word, const std::string& name,
                      ETCS::Peer& far, std::string& why, bool* parted = nullptr)
{
    ETCS::TrustEvent q(ETCS::TrustEvent::Op::Recognize);
    q.name = std::string(1, side);
    q.a = word;
    q.b = hex(t, 32);
    const bool ok = q();
    const bool other = !ok && q.out.compare(0, 13, "another epoch") == 0;
    if (parted) *parted = other;
    if (other) { why = "'" + name + "' runs in " + q.out; return false; }
    if (!ok) { why = "'" + name + "' did not prove itself: " + q.out; return false; }
    std::istringstream in(q.out);
    std::string ident;
    int registered = 0, guild = 0;
    in >> ident >> registered >> guild;
    far = ETCS::Peer{};
    if (!unhex(word.substr(0, 64), far.key, 32) || !unhex(ident, far.identity, 32))
    { why = "'" + name + "' answered with an unreadable word"; return false; }
    far.registered = registered ? 1 : 0;
    far.guild = guild ? 1 : 0;
    std::strncpy(far.name, name.c_str(), sizeof far.name - 1);
    return true;
}
// The two keys, one each way, from the exchange and the transcript.
inline bool keys(bool dialler, const uint8_t mine[32], const uint8_t theirs[32], const uint8_t t[32], Link& l)
{
    uint8_t shared[32], d2a[32], a2d[32];
    etcs_monocypher::crypto_x25519(shared, mine, theirs);
    uint8_t any = 0;
    for (uint8_t b : shared) any |= b;
    if (!any) return false;                          // a low-order key: no secret at all
    for (char side : { 'd', 'a' })
    {
        std::string m("ETCSLNK2");
        m.push_back(side);
        m.append(reinterpret_cast<const char*>(shared), 32);
        m.append(reinterpret_cast<const char*>(t), 32);
        etcs_monocypher::crypto_blake2b(side == 'd' ? d2a : a2d, 32, reinterpret_cast<const uint8_t*>(m.data()), m.size());
        etcs_monocypher::crypto_wipe(&m[0], m.size());
    }
    std::memcpy(l.out, dialler ? d2a : a2d, 32);
    std::memcpy(l.in,  dialler ? a2d : d2a, 32);
    etcs_monocypher::crypto_wipe(shared, 32);
    etcs_monocypher::crypto_wipe(d2a, 32);
    etcs_monocypher::crypto_wipe(a2d, 32);
    return true;
}
struct Exchange
{
    uint8_t secret[32], pub[32];
    bool make()
    {
        if (!random_bytes(secret, 32)) return false;
        etcs_monocypher::crypto_x25519_public_key(pub, secret);
        return true;
    }
    ~Exchange() { etcs_monocypher::crypto_wipe(secret, 32); }
};
} // namespace hello

// "<key> <identity> <registered> <guild>", hex and 0/1 -- for a script to read.
inline std::string who(const ETCS::Peer& p)
{
    return hex(p.key, 32) + " " + hex(p.identity, 32) + " " + (p.registered ? "1" : "0") + " " + (p.guild ? "1" : "0");
}
// "key 1a2b...: registered, of this guild" -- for a log line.
inline std::string describe(const ETCS::Peer& p)
{
    char fp[17];
    p.fingerprint(fp);
    return std::string("key ") + fp + ": " + (!p.registered ? "a local identity"
                                            : p.guild ? "registered, of this loader's guild" : "registered elsewhere");
}

inline bool hello_dial(int fd, const std::string& my_name, Link& link, std::string* why = nullptr)
{
    std::string w, word, err;
    uint8_t far_x[32], t[32];
    hello::Exchange x;
    const auto fail = [&](const std::string& m) { if (why) *why = m; return false; };
    if (!x.make()) return fail("no randomness for the exchange");
    if (!etcs_ws::send_frame(fd, Writer().u8('H').u8(3).str(my_name).raw(reinterpret_cast<const char*>(x.pub), 32).s)
        || !etcs_ws::recv_frame(fd, w, etcs_ws::kLive) || w.empty()) return false;
    Reader r(w, 1);
    if (w[0] == 'N') return fail(r.str());
    if (w[0] != 'W') return false;
    link.far_name = r.str();
    r.bytes(far_x, 32);
    const std::string far_word = r.str();
    if (!r.ok) return fail("the welcome is unreadable");
    hello::transcript(my_name, x.pub, link.far_name, far_x, t);
    bool parted = false;
    if (!hello::recognize('a', t, far_word, link.far_name, link.far, err, &parted))
    {
        std::string unsaid;
        if (parted && hello::vouch('d', t, word, unsaid)) etcs_ws::send_frame(fd, Writer().u8('P').str(word).s);
        return fail(err);
    }
    if (!hello::vouch('d', t, word, err)) return fail("this loader would not vouch for the link: " + err);
    if (!etcs_ws::send_frame(fd, Writer().u8('P').str(word).s) || !etcs_ws::recv_frame(fd, w, 10000) || w.empty())
        return fail("'" + link.far_name + "' did not answer this side's word");
    if (w[0] == 'N') { Reader n(w, 1); return fail(n.str()); }
    if (w[0] != 'K') return false;
    if (!hello::keys(true, x.secret, far_x, t, link)) return fail("the exchange gave no secret");
    return true;
}
// `refuse` says why a guest that proved itself may not link here (empty: it
// may) -- a name already linked by another key, say.
inline bool hello_accept(int fd, const std::string& my_name, Link& link,
                         const std::function<std::string(const ETCS::Peer&)>& refuse = {})
{
    std::string h, word, err;
    uint8_t far_x[32], t[32];
    hello::Exchange x;
    const auto no = [&](const std::string& why)
    {
        ETCS_LOG("Edge", "'" << my_name << "' refused a link: " << why);
        etcs_ws::send_frame(fd, Writer().u8('N').str(why).s);
        return false;
    };
    if (!etcs_ws::recv_frame(fd, h, 10000) || h.size() < 2 || h[0] != 'H' || h[1] != 3) return false;
    Reader r(h, 2);
    link.far_name = r.str();
    r.bytes(far_x, 32);
    if (!r.ok) return false;
    if (!x.make()) return no("no randomness for the exchange");
    hello::transcript(link.far_name, far_x, my_name, x.pub, t);
    if (!hello::vouch('a', t, word, err)) return no("'" + my_name + "' would not vouch for the link: " + err);
    if (!etcs_ws::send_frame(fd, Writer().u8('W').str(my_name).raw(reinterpret_cast<const char*>(x.pub), 32).str(word).s)
        || !etcs_ws::recv_frame(fd, h, 10000) || h.size() < 2 || h[0] != 'P') return false;
    Reader p(h, 1);
    const std::string far_word = p.str();
    if (!p.ok) return false;
    if (!hello::recognize('d', t, far_word, link.far_name, link.far, err)) return no(err);
    if (refuse) { const std::string why = refuse(link.far); if (!why.empty()) return no(why); }
    if (!hello::keys(false, x.secret, far_x, t, link)) return no("the exchange gave no secret");
    return etcs_ws::send_frame(fd, Writer().u8('K').s);
}

class Edge
{
public:
    static constexpr size_t kChunk     = 3072;             // bytes of a channel per 'D' frame
    static constexpr size_t kQueueCap  = 4u << 20;         // queued for one slow consumer

    Edge(int fd, ETCS::MemoryArena& arena, bool dialer, const Link& link, Exports* exports)
        : fd_(fd), dialer_(dialer), far_name_(link.far_name), far_(link.far), exports_(exports),
          id_(random_token()), next_ch_(dialer ? 1 : 2)
    {
        std::memcpy(key_out_, link.out, 32);
        std::memcpy(key_in_, link.in, 32);
        ctx_.interrupt = &stop_;
        ctx_.peer = far_;
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
        etcs_monocypher::crypto_wipe(key_out_, 32);
        etcs_monocypher::crypto_wipe(key_in_, 32);
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
    // What the far side proved at the hello: its key, and its identity.
    const ETCS::Peer& far() const { return far_; }
    size_t channels() { std::lock_guard<std::mutex> lock(ch_mu_); return chans_.size(); }

    // ── Asking the far side ─────────────────────────────────────────────────

    // Each ask waits for its answer as long as the link lives (Pending), or
    // until `asker` is interrupted.
    //
    // `state`: what the far node says a reflection of it should show
    // (ontology/Environmental.h), packed; empty when it claims no such thing.
    bool bind(const std::string& name, const std::string& module, const std::string& tag,
              uint64_t tag_hash, const std::string& manifest, ETCS::RID& far, std::string& state,
              std::string& why, const ETCS::SignalContext* asker = nullptr)
    {
        std::string body;
        if (!ask('B', Writer().str(name).str(module).str(tag).u64(tag_hash).str(manifest).s, body, asker))
        { why = unanswered(); return false; }
        Reader r(body);
        if (r.num<uint8_t>() != 1) { why = r.str(); return false; }
        far   = r.num<uint64_t>();
        state = r.str();
        return r.ok;
    }

    // `owner`'s network-scope stages wrap the payload and unwrap the answer.
    bool work(ETCS::RID far, uint64_t hash, const std::string& verb, ETCS::Buffer& data,
              ETCS::Entity* owner, std::string& why, const ETCS::SignalContext* asker = nullptr)
    {
        ETCS::MBuffer wire;
        if (data.written) wire.writeRaw(data.buf, data.written);
        wrap(owner, wire);
        std::string body;
        if (!ask('C', Writer().u64(far).u64(hash).str(verb).raw(wire.buf, wire.written).s, body, asker) || body.empty())
        { why = unanswered(); data.reset(); return false; }
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
                   bool far_produces, uint64_t hash, const std::string& manifest, std::string& why,
                   const ETCS::SignalContext* asker = nullptr)
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
                                   .u8(far_produces ? 1 : 0).u64(hash).str(manifest).s, body, asker);
        if (!asked || body.empty() || body[0] != 1)
        {
            why = !asked ? unanswered() : body.size() > 1 ? body.substr(1) : "refused";
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

    // Every message sealed under this side's key, numbered in the order sent:
    // the number is the nonce, so the far side opens them in that order or
    // not at all.
    bool send(const std::string& msg)
    {
        std::lock_guard<std::mutex> lock(send_mu_);
        if (stop_.load()) return false;
        std::string sealed(kMac + msg.size(), '\0');
        uint8_t nonce[24] = {};
        std::memcpy(nonce, &sent_, 8);
        ++sent_;
        uint8_t* body = reinterpret_cast<uint8_t*>(&sealed[0]);
        etcs_monocypher::crypto_aead_lock(body + kMac, body, key_out_, nonce, nullptr, 0,
                                          reinterpret_cast<const uint8_t*>(msg.data()), msg.size());
        return out_.writeMessage(sealed);
    }
    bool open(std::string& msg)
    {
        if (msg.size() < kMac) return false;
        uint8_t nonce[24] = {};
        std::memcpy(nonce, &got_, 8);
        ++got_;
        uint8_t* body = reinterpret_cast<uint8_t*>(&msg[0]);
        if (etcs_monocypher::crypto_aead_unlock(body + kMac, body, key_in_, nonce, nullptr, 0, body + kMac, msg.size() - kMac) != 0)
            return false;
        msg.erase(0, kMac);
        return true;
    }
    bool ask(char op, const std::string& body, std::string& answer, const ETCS::SignalContext* asker)
    {
        if (!isOpen()) return false;
        const uint32_t id = pending_.open();
        if (!send(Writer().u8(static_cast<uint8_t>(op)).u32(id).s + body)) return false;
        return pending_.wait(id, answer, asker);
    }
    // Why an ask came back empty: there is no waiting out a live link, so
    // it closed, or the asker stopped asking.
    std::string unanswered() const
    {
        return isOpen() ? "the asker was interrupted before " + far_name_ + " answered"
                        : "the link with " + far_name_ + " closed before it answered";
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

    // ── The reader: one message at a time, in order ────────────────────────
    void run()
    {
        std::string msg;
        while (!stop_.load())
        {
            if (!in_.readMessage(msg)) break;
            if (!open(msg))
            {
                ETCS_LOG("Edge", "a message from '" << far_name_ << "' did not open -- altered, replayed or "
                         "out of its order. The link is closed.");
                break;
            }
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
        // The far frame's half of Environmental: the values behind the node's
        // tags, which a reflection starts from. Its tags are the surface's
        // own already -- the same type.
        ETCS::EnvironmentState st;
        if (node->getInterfacePointer(ETCS::Buffer("Environmental"))) etcs_capture_values(node, st);
        std::string packed = st.pack();
        if (packed.size() > ETCS::MirrorBuffer::MAX_MESSAGE - 64)
        {
            ETCS_LOG("Edge", "'" << name << "': its state (" << packed.size()
                     << " bytes) is more than one message; the surface starts without it.");
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
        if (!exports_->allows(rid, verb)) return w.u8(0).raw("not a verb this export answers to", 33).s;
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
            node->call(ETCS::Buffer(conj.c_str()), buf, ctx_);     // ctx_.peer: who asked
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
        ETCS::RID published = 0;
        ETCS::Entity* node = r.ok ? exported(name, &published) : nullptr;
        if (!node) return w.u8(0).raw("no such export", 14).s;
        if (ETCS::MirrorBuffer::localFrame(node)) return w.u8(0).raw("of the local frame", 18).s;
        if (!exports_->allows(published, verb)) return w.u8(0).raw("not a stream this export answers to", 35).s;
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
        bool gone = false;
        while (true)
        {
            std::string next;
            {
                std::unique_lock<std::mutex> lock(c->mu);
                // Waited in slices: a consumer that returned with nothing more
                // arriving is noticed here (its pipe end closed), and the far
                // producer told -- not only when the next bytes fail to land.
                while (!c->cv.wait_for(lock, std::chrono::milliseconds(250),
                                       [&] { return !c->q.empty() || c->ended || c->dead.load(); }))
                {
                    pollfd p{ c->fd, POLLOUT, 0 };
                    if (::poll(&p, 1, 0) > 0 && (p.revents & (POLLERR | POLLHUP | POLLNVAL))) { gone = true; break; }
                }
                if (gone || c->dead.load()) break;
                if (c->q.empty()) break;                         // ended and drained
                next = std::move(c->q.front());
                c->q.pop_front();
                c->bytes -= next.size();
                c->cv.notify_all();
            }
            if (!writeAll(c, next)) { gone = true; break; }
        }
        if (gone) send(Writer().u8('X').u32(c->id).s);
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

    static constexpr size_t kMac = 16;

    int                 fd_;
    bool                dialer_;
    std::string         far_name_;
    ETCS::Peer          far_;
    uint8_t             key_out_[32] = {}, key_in_[32] = {};
    uint64_t            sent_ = 0, got_ = 0;        // the next number each way
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
