#ifndef NETWORKPROVIDER_WEBSOCKET_H__
#define NETWORKPROVIDER_WEBSOCKET_H__
#include "../../../ontology.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <strings.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#endif
#if !defined(__EMSCRIPTEN__)
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/error.h"
#include "mbedtls/psa_util.h"
#include "TLSServerContext.h"
#endif

/*
 * THE WIRE UNDER A LINK: a WebSocket, everywhere.
 *
 * A browser can open exactly one kind of socket to anywhere, so the channel
 * between two runtimes is a WebSocket whoever is on either end. What rides
 * on it is a byte stream -- MirrorBuffer frames ([u32 len][payload],
 * core/MirrorBuffer.h) -- and message boundaries on the WebSocket mean
 * nothing: a runtime reads bytes, the same as from a pipe.
 *
 * In the browser, emscripten's socket layer (SOCKFS) IS the WebSocket: a
 * connect() opens one, send() and recv() move binary messages as a byte
 * stream, and the MirrorBuffer sits directly on that fd (link_dial below).
 *
 * Natively the WebSocket framing (and TLS under it) is done here, by a pump
 * thread moving bytes between the WebSocket and one end of a socketpair; the
 * MirrorBuffer sits on the other end. So both builds hand the layer above a
 * plain connected fd, and nothing above this file knows which it has.
 *
 * Not a Wrapper_ chain (ontology/Wrapper.h), though that is where the
 * framing was once imagined: a wrap stage transforms one frame, and a TLS
 * record or a WebSocket frame does not line up with one -- a read can hold
 * half of either, and pings arrive between them. A pump owns the whole
 * connection instead.
 */
namespace etcs_ws
{

// ── SHA-1 and base64, for the handshake's Sec-WebSocket-Accept only ─────────
inline void sha1(const uint8_t* data, size_t len, uint8_t out[20])
{
    uint32_t h[5] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u };
    auto rol = [](uint32_t v, int s) { return (v << s) | (v >> (32 - s)); };
    std::vector<uint8_t> m(data, data + len);
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    const uint64_t bits = static_cast<uint64_t>(len) * 8;
    for (int i = 7; i >= 0; --i) m.push_back(static_cast<uint8_t>(bits >> (i * 8)));
    for (size_t off = 0; off < m.size(); off += 64)
    {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(m[off + 4 * i]) << 24) | (uint32_t(m[off + 4 * i + 1]) << 16)
                 | (uint32_t(m[off + 4 * i + 2]) << 8) | uint32_t(m[off + 4 * i + 3]);
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i)
        {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);           k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDCu; }
            else             { f = b ^ c ^ d;                    k = 0xCA62C1D6u; }
            const uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 4; ++j) out[4 * i + j] = static_cast<uint8_t>(h[i] >> (24 - 8 * j));
}

inline std::string base64(const uint8_t* p, size_t n)
{
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < n; i += 3)
    {
        const uint32_t v = (uint32_t(p[i]) << 16) | (i + 1 < n ? uint32_t(p[i + 1]) << 8 : 0)
                         | (i + 2 < n ? uint32_t(p[i + 2]) : 0);
        out += t[(v >> 18) & 63];
        out += t[(v >> 12) & 63];
        out += i + 1 < n ? t[(v >> 6) & 63] : '=';
        out += i + 2 < n ? t[v & 63] : '=';
    }
    return out;
}

inline std::string accept_key(const std::string& key)
{
    const std::string s = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    uint8_t d[20];
    sha1(reinterpret_cast<const uint8_t*>(s.data()), s.size(), d);
    return base64(d, 20);
}

// ── ws[s]://host[:port]/path ────────────────────────────────────────────────
struct Url
{
    bool        tls  = false;
    std::string host;
    int         port = 0;
    std::string path = "/";

    static bool parse(const std::string& s, Url& u)
    {
        size_t at = 0;
        if      (s.rfind("wss://", 0) == 0) { u.tls = true;  at = 6; }
        else if (s.rfind("ws://", 0) == 0)  { u.tls = false; at = 5; }
        else return false;
        const size_t slash = s.find('/', at);
        const std::string hostport = s.substr(at, slash == std::string::npos ? std::string::npos : slash - at);
        u.path = slash == std::string::npos ? "/" : s.substr(slash);
        const size_t colon = hostport.rfind(':');
        if (colon != std::string::npos && hostport.find(']') == std::string::npos)
        {
            u.host = hostport.substr(0, colon);
            u.port = std::atoi(hostport.c_str() + colon + 1);
        }
        else u.host = hostport;
        if (u.port == 0) u.port = u.tls ? 443 : 80;
        return !u.host.empty() && u.port > 0 && u.port < 65536;
    }
    std::string str() const
    {
        return std::string(tls ? "wss://" : "ws://") + host + ":" + std::to_string(port) + path;
    }
};

// ── Exact reads and whole writes on a raw fd ───────────────────────────────
//
// For the few bytes that must be read WITHOUT reading past them: a channel's
// first frame says what the channel is for, and whatever follows it belongs
// to the MirrorBuffer that takes the fd over next. A MirrorBuffer reads in
// chunks, so it cannot be the one to read that header.
inline bool wait_fd(int fd, short ev, int timeout_ms, const std::atomic<bool>* stop = nullptr)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (true)
    {
        if (stop && stop->load(std::memory_order_acquire)) return false;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            end - std::chrono::steady_clock::now()).count();
        if (left <= 0) return false;
        pollfd p{ fd, ev, 0 };
        const int r = ::poll(&p, 1, static_cast<int>(left < 100 ? left : 100));
        if (r > 0) return true;
        if (r < 0 && errno != EINTR) return false;
    }
}

inline bool write_all(int fd, const char* p, size_t n, int timeout_ms = 10000,
                      const std::atomic<bool>* stop = nullptr)
{
    size_t off = 0;
    while (off < n)
    {
        const ssize_t w = ::send(fd, p + off, n - off, MSG_NOSIGNAL);
        if (w > 0) { off += static_cast<size_t>(w); continue; }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        {
            if (!wait_fd(fd, POLLOUT, timeout_ms, stop)) return false;
            continue;
        }
        return false;
    }
    return true;
}

inline bool read_exact(int fd, char* p, size_t n, int timeout_ms,
                       const std::atomic<bool>* stop = nullptr)
{
    size_t off = 0;
    while (off < n)
    {
        const ssize_t r = ::recv(fd, p + off, n - off, 0);
        if (r > 0) { off += static_cast<size_t>(r); continue; }
        if (r == 0) return false;
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        {
            if (!wait_fd(fd, POLLIN, timeout_ms, stop)) return false;
            continue;
        }
        return false;
    }
    return true;
}

// One MirrorBuffer-shaped frame, exactly -- see read_exact.
inline bool send_frame(int fd, const std::string& payload)
{
    const uint32_t len = static_cast<uint32_t>(payload.size());
    std::string f(reinterpret_cast<const char*>(&len), 4);
    f += payload;
    return write_all(fd, f.data(), f.size());
}
inline bool recv_frame(int fd, std::string& out, int timeout_ms, size_t max = 4091)
{
    uint32_t len = 0;
    if (!read_exact(fd, reinterpret_cast<char*>(&len), 4, timeout_ms)) return false;
    if (len > max) return false;
    out.assign(len, '\0');
    return len == 0 || read_exact(fd, &out[0], len, timeout_ms);
}

inline void set_nonblocking(int fd)
{
    const int fl = ::fcntl(fd, F_GETFL, 0);
    if (fl != -1) ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

// ── Frames ──────────────────────────────────────────────────────────────────
enum Op : uint8_t { Cont = 0x0, Text = 0x1, Binary = 0x2, Close = 0x8, Ping = 0x9, Pong = 0xA };

// A client masks what it sends and a server must not (RFC 6455 5.1); the key
// only has to be unpredictable to the page's own script, which it never sees.
inline void encode(std::string& out, uint8_t op, const char* p, size_t n, bool mask)
{
    out += static_cast<char>(0x80 | op);
    const uint8_t mbit = mask ? 0x80 : 0;
    if (n < 126) out += static_cast<char>(mbit | n);
    else if (n < 65536) { out += static_cast<char>(mbit | 126); out += char(n >> 8); out += char(n & 0xFF); }
    else
    {
        out += static_cast<char>(mbit | 127);
        for (int i = 7; i >= 0; --i) out += static_cast<char>((static_cast<uint64_t>(n) >> (8 * i)) & 0xFF);
    }
    if (!mask) { out.append(p, n); return; }
    static std::atomic<uint32_t> seed{ static_cast<uint32_t>(
        std::chrono::steady_clock::now().time_since_epoch().count()) };
    uint32_t k = seed.fetch_add(0x9E3779B9u) * 2654435761u;
    char key[4] = { char(k), char(k >> 8), char(k >> 16), char(k >> 24) };
    out.append(key, 4);
    const size_t base = out.size();
    out.append(p, n);
    for (size_t i = 0; i < n; ++i) out[base + i] = static_cast<char>(out[base + i] ^ key[i & 3]);
}

// Incremental: bytes in, whole frames out. Fragmented data frames are just
// more bytes of the stream, so FIN is not tracked.
struct Decoder
{
    std::string in;
    // 1: a frame; 0: need more; -1: protocol error (oversized, reserved bits).
    int next(uint8_t& op, std::string& payload, size_t max = (1u << 20))
    {
        if (in.size() < 2) return 0;
        const uint8_t b0 = static_cast<uint8_t>(in[0]), b1 = static_cast<uint8_t>(in[1]);
        if (b0 & 0x70) return -1;
        op = b0 & 0x0F;
        const bool masked = b1 & 0x80;
        uint64_t n = b1 & 0x7F;
        size_t at = 2;
        if (n == 126)
        {
            if (in.size() < 4) return 0;
            n = (uint64_t(uint8_t(in[2])) << 8) | uint8_t(in[3]);
            at = 4;
        }
        else if (n == 127)
        {
            if (in.size() < 10) return 0;
            n = 0;
            for (int i = 0; i < 8; ++i) n = (n << 8) | uint8_t(in[2 + i]);
            at = 10;
        }
        if (n > max) return -1;
        char key[4] = { 0, 0, 0, 0 };
        if (masked)
        {
            if (in.size() < at + 4) return 0;
            std::memcpy(key, in.data() + at, 4);
            at += 4;
        }
        if (in.size() < at + n) return 0;
        payload.assign(in.data() + at, static_cast<size_t>(n));
        if (masked) for (size_t i = 0; i < payload.size(); ++i) payload[i] ^= key[i & 3];
        in.erase(0, at + static_cast<size_t>(n));
        return 1;
    }
};

// One header's value out of an HTTP head, case-insensitively by name.
inline std::string header_value(const std::string& head, const char* name)
{
    const size_t nl = std::strlen(name);
    size_t pos = 0;
    while ((pos = head.find("\r\n", pos)) != std::string::npos)
    {
        pos += 2;
        if (head.size() >= pos + nl + 1 && strncasecmp(head.c_str() + pos, name, nl) == 0
            && head[pos + nl] == ':')
        {
            size_t v = pos + nl + 1;
            while (v < head.size() && head[v] == ' ') ++v;
            const size_t e = head.find("\r\n", v);
            return head.substr(v, e == std::string::npos ? std::string::npos : e - v);
        }
    }
    return {};
}

/*
 * EVERY LINK THREAD -- pumps, session readers, stream halves -- in one place
 * the module can stop and join. They run this module's code, so none may
 * outlive the module; and they are started from wherever a connection
 * happened to arrive, so nothing else owns them all.
 * Finished ones are joined as new ones start, and the rest when the module
 * goes (the static's destructor).
 */
class Pumps
{
public:
    static Pumps& get() { static Pumps p; return p; }
    const std::atomic<bool>& stopping() const { return stop_; }

    void start(std::function<void()> body)
    {
        std::lock_guard<std::mutex> lock(mu_);
        reapLocked();
        auto done = std::make_shared<std::atomic<bool>>(false);
        threads_.push_back({ std::thread([body = std::move(body), done]() { body(); done->store(true); }), done });
    }
    ~Pumps()
    {
        stop_.store(true);
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& t : threads_) if (t.th.joinable()) t.th.join();
    }
private:
    struct Entry { std::thread th; std::shared_ptr<std::atomic<bool>> done; };
    void reapLocked()
    {
        for (auto it = threads_.begin(); it != threads_.end(); )
        {
            if (it->done->load()) { it->th.join(); it = threads_.erase(it); }
            else ++it;
        }
    }
    std::mutex         mu_;
    std::vector<Entry> threads_;
    std::atomic<bool>  stop_{ false };
};

#if !defined(__EMSCRIPTEN__)

// ── Byte transports under a native WebSocket ───────────────────────────────
//
// readSome: >0 bytes, 0 nothing now, -1 closed. Drained until 0 each time
// the fd polls readable, so bytes a TLS layer decoded but did not return
// yet are never stranded behind a poll that will not fire again.
class ByteIO
{
public:
    virtual ~ByteIO() = default;
    virtual int  fd() const = 0;
    virtual int  readSome(char* out, size_t cap) = 0;
    virtual bool writeAll(const char* p, size_t n, const std::atomic<bool>& stop) = 0;
};

class PlainIO : public ByteIO
{
public:
    explicit PlainIO(int fd, bool owns = true) : fd_(fd), owns_(owns) { set_nonblocking(fd_); }
    ~PlainIO() override { if (owns_ && fd_ >= 0) ::close(fd_); }
    int fd() const override { return fd_; }
    int readSome(char* out, size_t cap) override
    {
        const ssize_t r = ::recv(fd_, out, cap, 0);
        if (r > 0) return static_cast<int>(r);
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return 0;
        return -1;
    }
    bool writeAll(const char* p, size_t n, const std::atomic<bool>& stop) override
    { return write_all(fd_, p, n, 30000, &stop); }
private:
    int  fd_;
    bool owns_;
};

// A server connection's TLS, driven synchronously through the memory BIO the
// accept path already set up (TLSServerContext.h). The fd and the session
// stay the connection's; this only borrows them while the pump runs.
class ServerTlsIO : public ByteIO
{
public:
    ServerTlsIO(TLSServerContext& tls, int fd) : tls_(tls), fd_(fd) { set_nonblocking(fd_); }
    int fd() const override { return fd_; }
    int readSome(char* out, size_t cap) override
    {
        for (int round = 0; round < 2; ++round)
        {
            const int n = tls_.ReadPlain(reinterpret_cast<unsigned char*>(out), cap);
            flushCipher(nullptr);
            if (n > 0) return n;
            if (n == 0 || !TLSServerContext::IsWouldBlock(n)) return -1;
            if (round == 1) return 0;
            TLSServerContext::CipherSpan& in = tls_.CipherIn();
            in.compact();
            if (in.writable() == 0) return -1;
            const ssize_t r = ::recv(fd_, in.ptr + in.len, in.writable(), 0);
            if (r == 0) return -1;
            if (r < 0) return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
            in.len += static_cast<size_t>(r);
        }
        return 0;
    }
    bool writeAll(const char* p, size_t n, const std::atomic<bool>& stop) override
    {
        size_t off = 0;
        while (off < n)
        {
            const int w = tls_.WritePlain(reinterpret_cast<const unsigned char*>(p + off), n - off);
            if (!flushCipher(&stop)) return false;
            if (w > 0) { off += static_cast<size_t>(w); continue; }
            if (!TLSServerContext::IsWouldBlock(w)) return false;
        }
        return true;
    }
private:
    bool flushCipher(const std::atomic<bool>* stop)
    {
        TLSServerContext::CipherSpan& o = tls_.CipherOut();
        const bool ok = o.unread() == 0 || write_all(fd_, o.ptr + o.off, o.unread(), 30000, stop);
        o.reset();
        return ok;
    }
    TLSServerContext& tls_;
    int               fd_;
};

// A client's TLS, on its own socket. Verification is on unless the caller
// said otherwise -- `insecure` exists for the self-signed certificate a
// development server runs with, and says so in the log every time.
class ClientTlsIO : public ByteIO
{
public:
    ClientTlsIO(int fd) : fd_(fd)
    {
        mbedtls_ssl_init(&ssl_);
        mbedtls_ssl_config_init(&conf_);
        mbedtls_x509_crt_init(&ca_);
        set_nonblocking(fd_);
    }
    ~ClientTlsIO() override
    {
        if (ready_) mbedtls_ssl_close_notify(&ssl_);
        mbedtls_ssl_free(&ssl_);
        mbedtls_ssl_config_free(&conf_);
        mbedtls_x509_crt_free(&ca_);
        if (fd_ >= 0) ::close(fd_);
    }
    bool handshake(const std::string& host, bool insecure, std::string& err)
    {
        if (psa_crypto_init() != PSA_SUCCESS) { err = "psa_crypto_init failed"; return false; }
        if (mbedtls_ssl_config_defaults(&conf_, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT) != 0)
        { err = "ssl_config_defaults failed"; return false; }
        if (insecure) mbedtls_ssl_conf_authmode(&conf_, MBEDTLS_SSL_VERIFY_NONE);
        else
        {
            if (mbedtls_x509_crt_parse_file(&ca_, "/etc/ssl/certs/ca-certificates.crt") < 0)
            { err = "no system CA bundle at /etc/ssl/certs/ca-certificates.crt"; return false; }
            mbedtls_ssl_conf_authmode(&conf_, MBEDTLS_SSL_VERIFY_REQUIRED);
            mbedtls_ssl_conf_ca_chain(&conf_, &ca_, nullptr);
        }
        if (mbedtls_ssl_setup(&ssl_, &conf_) != 0) { err = "ssl_setup failed"; return false; }
        mbedtls_ssl_set_hostname(&ssl_, host.c_str());
        mbedtls_ssl_set_bio(&ssl_, this, &ClientTlsIO::bioSend, &ClientTlsIO::bioRecv, nullptr);
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (true)
        {
            const int r = mbedtls_ssl_handshake(&ssl_);
            if (r == 0) break;
            if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE)
            {
                char b[160]; mbedtls_strerror(r, b, sizeof b);
                err = std::string("TLS handshake: ") + b;
                return false;
            }
            if (std::chrono::steady_clock::now() > end) { err = "TLS handshake timed out"; return false; }
            pollfd p{ fd_, static_cast<short>(r == MBEDTLS_ERR_SSL_WANT_READ ? POLLIN : POLLOUT), 0 };
            ::poll(&p, 1, 100);
        }
        ready_ = true;
        return true;
    }
    int fd() const override { return fd_; }
    int readSome(char* out, size_t cap) override
    {
        const int r = mbedtls_ssl_read(&ssl_, reinterpret_cast<unsigned char*>(out), cap);
        if (r > 0) return r;
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) return 0;
#ifdef MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
        if (r == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) return 0;
#endif
        return -1;
    }
    bool writeAll(const char* p, size_t n, const std::atomic<bool>& stop) override
    {
        size_t off = 0;
        while (off < n)
        {
            const int w = mbedtls_ssl_write(&ssl_, reinterpret_cast<const unsigned char*>(p + off), n - off);
            if (w > 0) { off += static_cast<size_t>(w); continue; }
            if (w != MBEDTLS_ERR_SSL_WANT_READ && w != MBEDTLS_ERR_SSL_WANT_WRITE) return false;
            if (!wait_fd(fd_, w == MBEDTLS_ERR_SSL_WANT_READ ? POLLIN : POLLOUT, 30000, &stop)) return false;
        }
        return true;
    }
private:
    static int bioSend(void* self, const unsigned char* b, size_t n)
    {
        const ssize_t w = ::send(static_cast<ClientTlsIO*>(self)->fd_, b, n, MSG_NOSIGNAL);
        if (w >= 0) return static_cast<int>(w);
        return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
             ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
    }
    static int bioRecv(void* self, unsigned char* b, size_t n)
    {
        const ssize_t r = ::recv(static_cast<ClientTlsIO*>(self)->fd_, b, n, 0);
        if (r >= 0) return static_cast<int>(r);     // 0 is the peer's EOF, which mbedtls reports
        return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
             ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_NET_RECV_FAILED;
    }
    int                 fd_;
    bool                ready_ = false;
    mbedtls_ssl_context ssl_;
    mbedtls_ssl_config  conf_;
    mbedtls_x509_crt    ca_;
};

// ── The two kinds of end a pump moves bytes between ────────────────────────
class End
{
public:
    virtual ~End() = default;
    virtual int  fd() const = 0;
    // Append what arrived to `out`; false once this end is finished.
    virtual bool readInto(std::string& out) = 0;
    virtual bool send(const char* p, size_t n) = 0;
    // Per-loop upkeep (pings, idle limits, the owner's liveness); false ends.
    virtual bool tick() { return true; }
    virtual void finish() {}
};

// One end of a socketpair: the runtime side of a native link.
class FdEnd : public End
{
public:
    FdEnd(int fd, const std::atomic<bool>& stop) : fd_(fd), stop_(stop) { set_nonblocking(fd_); }
    ~FdEnd() override { if (fd_ >= 0) ::close(fd_); }
    int fd() const override { return fd_; }
    bool readInto(std::string& out) override
    {
        char buf[16384];
        while (true)
        {
            const ssize_t r = ::recv(fd_, buf, sizeof buf, 0);
            if (r > 0) { out.append(buf, static_cast<size_t>(r)); continue; }
            if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return true;
            return false;
        }
    }
    bool send(const char* p, size_t n) override { return write_all(fd_, p, n, 30000, &stop_); }
private:
    int                      fd_;
    const std::atomic<bool>& stop_;
};

/*
 * A WebSocket over a ByteIO. A server end pings every 10 s and a client end
 * answers; either gives up after 35 s without hearing anything, so a peer
 * that vanished without a close (a laptop lid, a dropped route) is noticed
 * in bounded time rather than when the next write happens to fail.
 */
class WsEnd : public End
{
public:
    WsEnd(std::unique_ptr<ByteIO> io, bool client, const std::atomic<bool>& stop)
        : io_(std::move(io)), client_(client), stop_(stop),
          last_rx_(std::chrono::steady_clock::now()), last_ping_(last_rx_) {}
    ~WsEnd() override { if (on_done) on_done(); }

    std::function<bool()> alive;      // the owner's own liveness, if it has one
    std::function<void()> activity;   // told whenever bytes arrive
    std::function<void()> on_done;    // runs last, from the destructor

    int fd() const override { return io_->fd(); }
    bool readInto(std::string& out) override
    {
        // Everything that arrived is decoded before an EOF is reported: a
        // peer that writes its last frames and closes delivers both in one
        // read, and the frames are still owed to the other end.
        char buf[16384];
        bool eof = false;
        while (true)
        {
            const int n = io_->readSome(buf, sizeof buf);
            if (n < 0) { eof = true; break; }
            if (n == 0) break;
            dec_.in.append(buf, static_cast<size_t>(n));
            last_rx_ = std::chrono::steady_clock::now();
            if (activity) activity();
        }
        uint8_t op = 0;
        std::string payload;
        while (true)
        {
            const int r = dec_.next(op, payload);
            if (r < 0) return false;
            if (r == 0) return !eof;
            switch (op)
            {
                case Cont: case Text: case Binary: out += payload; break;
                case Ping:  control(Pong, payload); break;
                case Pong:  break;
                case Close: control(Close, payload.substr(0, 2)); closed_ = true; return false;
                default:    return false;
            }
        }
    }
    bool send(const char* p, size_t n) override
    {
        std::string f;
        encode(f, Binary, p, n, client_);
        std::lock_guard<std::mutex> lock(write_mu_);
        return io_->writeAll(f.data(), f.size(), stop_);
    }
    bool tick() override
    {
        if (alive && !alive()) return false;
        const auto now = std::chrono::steady_clock::now();
        if (now - last_rx_ > std::chrono::seconds(35)) return false;
        if (!client_ && now - last_ping_ > std::chrono::seconds(10))
        {
            last_ping_ = now;
            return control(Ping, {});
        }
        return true;
    }
    void finish() override { if (!closed_) { control(Close, std::string("\x03\xe8", 2)); closed_ = true; } }

    // The handshake's own bytes, before any frame: written as they are.
    bool raw(const std::string& s)
    {
        std::lock_guard<std::mutex> lock(write_mu_);
        return io_->writeAll(s.data(), s.size(), stop_);
    }
    ByteIO& io() { return *io_; }

private:
    bool control(uint8_t op, const std::string& payload)
    {
        std::string f;
        encode(f, op, payload.data(), payload.size(), client_);
        std::lock_guard<std::mutex> lock(write_mu_);
        return io_->writeAll(f.data(), f.size(), stop_);
    }
    std::unique_ptr<ByteIO>  io_;
    bool                     client_;
    const std::atomic<bool>& stop_;
    Decoder                  dec_;
    std::mutex               write_mu_;
    bool                     closed_ = false;
    std::chrono::steady_clock::time_point last_rx_, last_ping_;
};

// Moves bytes both ways until either end finishes or `stop` is raised.
inline void pump(End& a, End& b, const std::atomic<bool>& stop)
{
    std::string buf;
    while (!stop.load(std::memory_order_acquire))
    {
        pollfd p[2] = { { a.fd(), POLLIN, 0 }, { b.fd(), POLLIN, 0 } };
        const int r = ::poll(p, 2, 250);
        if (r < 0 && errno != EINTR) break;
        bool ok = true;
        // What a finishing end delivered is still forwarded before the pump
        // stops -- its last bytes and its EOF arrive together.
        auto move = [&buf](End& from, End& to)
        {
            buf.clear();
            const bool open = from.readInto(buf);
            const bool sent = buf.empty() || to.send(buf.data(), buf.size());
            return open && sent;
        };
        if (ok && p[0].revents) ok = move(a, b);
        if (ok && p[1].revents) ok = move(b, a);
        if (ok) ok = a.tick() && b.tick();
        if (!ok) break;
    }
    a.finish();
    b.finish();
}

// A native client WebSocket: TCP, TLS if wss, the upgrade. Blocking, with
// the timeouts above. Returns null and says why in `err`.
inline std::unique_ptr<WsEnd> client_connect(const Url& u, bool insecure, std::string& err)
{
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (::getaddrinfo(u.host.c_str(), std::to_string(u.port).c_str(), &hints, &res) != 0 || !res)
    { err = "cannot resolve " + u.host; return nullptr; }
    int fd = -1;
    for (addrinfo* a = res; a && fd < 0; a = a->ai_next)
    {
        fd = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        set_nonblocking(fd);
        if (::connect(fd, a->ai_addr, a->ai_addrlen) == 0) break;
        int soerr = 0; socklen_t sl = sizeof soerr;
        if (errno == EINPROGRESS && wait_fd(fd, POLLOUT, 10000)
            && ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) == 0 && soerr == 0) break;
        ::close(fd); fd = -1;
    }
    ::freeaddrinfo(res);
    if (fd < 0) { err = "cannot connect to " + u.host + ":" + std::to_string(u.port); return nullptr; }
    const int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    std::unique_ptr<ByteIO> io;
    if (u.tls)
    {
        auto t = std::make_unique<ClientTlsIO>(fd);
        if (!t->handshake(u.host, insecure, err)) return nullptr;
        io = std::move(t);
    }
    else io = std::make_unique<PlainIO>(fd);

    uint8_t nonce[16];
    const uint64_t r1 = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    for (int i = 0; i < 16; ++i) nonce[i] = static_cast<uint8_t>((r1 * (i + 7) * 0x9E3779B97F4A7C15ull) >> 56);
    const std::string key = base64(nonce, 16);
    const std::string req = "GET " + u.path + " HTTP/1.1\r\nHost: " + u.host + ":" + std::to_string(u.port)
        + "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key
        + "\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Protocol: binary\r\n\r\n";
    const std::atomic<bool>& stop = Pumps::get().stopping();
    if (!io->writeAll(req.data(), req.size(), stop)) { err = "upgrade request not sent"; return nullptr; }

    // The head, one byte at a time: the server may send frames right behind
    // it, and those belong to the decoder, not to this parse.
    std::string head;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (head.size() < 8192 && head.find("\r\n\r\n") == std::string::npos)
    {
        char c;
        const int n = io->readSome(&c, 1);
        if (n < 0) { err = "connection closed during upgrade"; return nullptr; }
        if (n == 1) { head += c; continue; }
        if (std::chrono::steady_clock::now() > end) { err = "upgrade timed out"; return nullptr; }
        wait_fd(io->fd(), POLLIN, 100);
    }
    if (head.compare(0, 12, "HTTP/1.1 101") != 0)
    { err = "server refused the upgrade: " + head.substr(0, head.find("\r\n")); return nullptr; }
    if (header_value(head, "Sec-WebSocket-Accept") != accept_key(key))
    { err = "bad Sec-WebSocket-Accept"; return nullptr; }
    return std::make_unique<WsEnd>(std::move(io), true, stop);
}

#endif // !__EMSCRIPTEN__

/*
 * A CONNECTED FD TO A WEBSOCKET URL, in either build -- the one call the
 * layers above make. -1 and `err` on failure.
 *
 * Native: the WebSocket is this module's (client_connect), bridged to one
 * end of a socketpair by a pump; the caller gets the other end. Closing it
 * ends the pump, which closes the WebSocket.
 *
 * Browser: emscripten's socket IS a WebSocket, and the URL it opens is read
 * from SOCKFS.websocketArgs at connect() -- a full URL there is used as it
 * is. So the URL is set, the socket connected, and the setting put back,
 * under one lock so two dials cannot swap URLs. connect() creates the
 * WebSocket synchronously (it is proxied to the main thread), which is what
 * makes restoring right after it safe. The address passed to connect() is
 * never contacted; the certificate is the browser's business, so `insecure`
 * means nothing here.
 */
inline int link_dial(const std::string& url, bool insecure, std::string& err)
{
    Url u;
    if (!Url::parse(url, u)) { err = "not a ws:// or wss:// URL: " + url; return -1; }
#if defined(__EMSCRIPTEN__)
    (void)insecure;
    static std::mutex dial_mu;
    std::lock_guard<std::mutex> lock(dial_mu);
    const std::string full = u.str();
    MAIN_THREAD_EM_ASM({
        SOCKFS.websocketArgs = SOCKFS.websocketArgs || {};
        SOCKFS.__etcsSavedUrl = SOCKFS.websocketArgs['url'];
        SOCKFS.websocketArgs['url'] = UTF8ToString($0);
        SOCKFS.websocketArgs['subprotocol'] = 'binary';
    }, full.c_str());
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    int rc = -1;
    if (fd >= 0)
    {
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port = htons(static_cast<uint16_t>(u.port));
        sa.sin_addr.s_addr = htonl(0x7F000001);
        rc = ::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa);
        if (rc < 0 && errno == EINPROGRESS) rc = 0;
    }
    MAIN_THREAD_EM_ASM({
        if (SOCKFS.__etcsSavedUrl === undefined) delete SOCKFS.websocketArgs['url'];
        else SOCKFS.websocketArgs['url'] = SOCKFS.__etcsSavedUrl;
    });
    if (fd < 0 || rc < 0) { err = "socket/connect failed for " + full; if (fd >= 0) ::close(fd); return -1; }
    // Open, or failed: SOCKFS reports a refused or closed WebSocket as
    // HUP/ERR on poll, and writable once it is open.
    pollfd p{ fd, POLLOUT, 0 };
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < end)
    {
        p.revents = 0;
        if (::poll(&p, 1, 100) > 0) break;
    }
    if (!(p.revents & POLLOUT) || (p.revents & (POLLHUP | POLLERR)))
    { err = "WebSocket did not open: " + full; ::close(fd); return -1; }
    return fd;
#else
    std::unique_ptr<WsEnd> ws = client_connect(u, insecure, err);
    if (!ws) return -1;
    if (insecure && u.tls)
        ETCS_LOG("Link", "dialled " << u.str() << " WITHOUT verifying its certificate (insecure).");
    int sv[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { err = "socketpair failed"; return -1; }
    std::shared_ptr<WsEnd> end(std::move(ws));
    const int far = sv[1];
    Pumps::get().start([end, far]()
    {
        FdEnd local(far, Pumps::get().stopping());
        pump(*end, local, Pumps::get().stopping());
    });
    set_nonblocking(sv[0]);
    return sv[0];
#endif
}

} // namespace etcs_ws

#endif // NETWORKPROVIDER_WEBSOCKET_H__
