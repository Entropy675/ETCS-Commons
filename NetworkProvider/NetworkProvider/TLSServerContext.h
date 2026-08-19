#ifndef TLSSERVERCONTEXT_H__
#define TLSSERVERCONTEXT_H__
#include "../../../ontology.h"
#include "TLSServerConfig.h"
#include <cstring>
#include <memory>
#include "mbedtls/ssl.h"
#include "mbedtls/error.h"

// TLSServerContext — per-connection server-side TLS state, driven by the
// io_uring COMPLETION callback chain in ConnectionManager.h (handshake)
// and HttpServer::Serve's do_recv/do_send (NetworkProvider.h, steady-state
// application data), never by a MirrorBuffer pull the way MbedTLSContext.h
// (the client) is. This is the "new type" half of the carry-forward doc's
// "new type or a substantial mode-split" decision -- see TLSServerConfig.h
// for the shared, server-mode mbedtls_ssl_config this wraps a connection's
// own mbedtls_ssl_context against.
//
// NOT an Entity, and deliberately so: nothing outside this connection's own
// recv/send driving loop ever addresses "the TLS state" as a script-visible
// thing -- it has no dispatch surface, no RID, no tags. It is composed
// plumbing, the same relationship SocketConnectionState.h's own
// MutableByteSpan already has to its owner.
//
// WHAT THIS CLASS DOES NOT DO: it never submits an IOSubmission itself.
// The client-side MbedTLSContext.h's bioSend/bioRecv each submit their own
// io_uring op directly, which the carry-forward doc calls out as wrong for
// a server integrated into the completion-driven accept path -- a BIO
// callback fires synchronously and possibly SEVERAL times inside one call
// to mbedtls_ssl_handshake()/mbedtls_ssl_read()/mbedtls_ssl_write(), and an
// io_uring submission has to be a single, explicit, NoteSubmit/NoteComplete-
// bracketed act visible at its own call site (per this session's own
// landmine list) -- not something buried inside a callback that might fire
// zero, one, or several times per outer call.
//
// So the split is: this class owns the mbedtls_ssl_context and two fixed
// CIPHERTEXT staging buffers (in and out), and its BIO callbacks only ever
// touch those buffers -- bioRecv drains CipherIn() and returns
// MBEDTLS_ERR_SSL_WANT_READ when it's empty; bioSend appends to CipherOut()
// and returns WANT_WRITE if there is no room left (should not happen in
// practice: see DriveOnce's own comment on why CipherOut is always fully
// flushed before the next mbedtls call that could grow it). The actual
// io_uring Recv/Send submissions that fill/drain those two buffers live in
// ConnectionManager.h (handshake phase) and NetworkProvider.h (steady-state
// HttpServer::Serve do_recv/do_send), each wrapped in the caller's own
// visible NoteSubmit()/NoteComplete() pair, exactly like every other
// IOSubmission in this module.
class TLSServerContext
{
public:
    enum class Phase : uint8_t { Idle, Handshaking, Established, Error };

    // Trivial non-owning (pointer, length, cursor) view over one of the two
    // fixed staging buffers -- same rationale as SocketConnectionState's own
    // MutableByteSpan (see that class's own comment): the buffer is arena-
    // owned by SocketConnectionState, this type is just a cursor into it.
    struct CipherSpan
    {
        char*  ptr;
        size_t cap;      // total capacity
        size_t len   = 0; // bytes currently valid, starting at ptr
        size_t off   = 0; // bytes already consumed, [off, len) is unread/unflushed

        size_t writable()  const { return cap - len; }
        size_t unread()    const { return len - off; }
        void   reset()     { len = 0; off = 0; }
        // Slides [off, len) down to 0 so writable() is maximized again. Only
        // safe to call when the caller knows nothing else holds a raw
        // pointer into this span across the call (true at every call site
        // below -- see each one's own comment).
        void   compact()
        {
            if (off == 0) return;
            if (off < len) std::memmove(ptr, ptr + off, len - off);
            len -= off;
            off  = 0;
        }
    };

    TLSServerContext() = default;
    ~TLSServerContext() { Free(); }

    TLSServerContext(const TLSServerContext&)            = delete;
    TLSServerContext& operator=(const TLSServerContext&) = delete;

    // Arena-backed staging buffers, allocated and owned by
    // SocketConnectionState (its ctor, alongside recv_buf_/send_buf_) and
    // bound here once -- same discipline PicoHTTPParser::BindAccumBuffer
    // already establishes for a composed non-Entity helper's own large
    // buffer. Called once, from SocketConnectionState's own constructor;
    // NOT re-bound on every claim/recycle.
    void BindCipherBuffers(char* in_buf, size_t in_cap, char* out_buf, size_t out_cap)
    {
        cipher_in_.ptr  = in_buf;
        cipher_in_.cap  = in_cap;
        cipher_out_.ptr = out_buf;
        cipher_out_.cap = out_cap;
    }

    // Called once per claim (SocketConnectionState::TryClaim, via
    // finalizeIfDraining having already called Free() on the PREVIOUS
    // occupant -- see this class's own Free()/finalizeIfDraining split
    // note below).
    //
    // SHARED OWNERSHIP, not a borrowed pointer. This connection holds its
    // own reference to the config for as long as its session lives, which
    // is what makes certificate reload safe: ConnectionManager::ReloadCerts
    // installs a NEW config for connections accepted after it, while every
    // connection already mid-session keeps the exact config its
    // mbedtls_ssl_context was set up against, until it closes. The old
    // config is destroyed when the last connection using it lets go.
    //
    // mbedtls_ssl_setup stores a pointer to the config INSIDE ssl_, so the
    // config must outlive ssl_ -- that requirement is why this is a
    // shared_ptr rather than a raw pointer, and why Free() below releases
    // it only after mbedtls_ssl_free has run.
    bool Init(std::shared_ptr<TLSServerConfig> shared_conf)
    {
        if (!shared_conf || !shared_conf->IsReady()) return false;
        mbedtls_ssl_init(&ssl_);
        int ret = mbedtls_ssl_setup(&ssl_, shared_conf->Get());
        if (ret != 0)
        {
            mbedtls_ssl_free(&ssl_);
            phase_ = Phase::Error;
            return false;
        }
        conf_ = std::move(shared_conf);
        mbedtls_ssl_set_bio(&ssl_, this, &TLSServerContext::bioSend, &TLSServerContext::bioRecv, nullptr);
        cipher_in_.reset();
        cipher_out_.reset();
        phase_ = Phase::Handshaking;
        return true;
    }

    // finalizeIfDraining()'s counterpart -- frees the mbedtls_ssl_context
    // and every internal secret it holds, and returns to Idle. This is the
    // ONLY place a live TLS session may be torn down: RecycleForNextRequest
    // (keep-alive, same peer, same session) must never reach this -- see
    // SocketConnectionState.h's own split for the two functions this
    // mirrors. Idempotent: safe to call on an already-Idle context (e.g. a
    // plaintext-only connection whose TLS was never Init'd at all).
    void Free()
    {
        if (phase_ == Phase::Idle) return;
        mbedtls_ssl_free(&ssl_);
        // Wipe what was actually written, then reset the cursors -- reset()
        // alone only moves len/off back to zero and leaves the bytes
        // sitting there for the NEXT peer to claim this pooled connection.
        // Bounded by len rather than a blind memset of both full spans, the
        // same reasoning (and the same cost argument) finalizeIfDraining
        // already applies to recv_buf_/send_buf_ -- see its own comment
        // there. These hold ciphertext rather than plaintext, so this is
        // hygiene rather than a plugged leak, but it is the same hygiene
        // the buffers either side of it already get.
        if (cipher_in_.len  > 0 && cipher_in_.ptr)  std::memset(cipher_in_.ptr,  0, cipher_in_.len);
        if (cipher_out_.len > 0 && cipher_out_.ptr) std::memset(cipher_out_.ptr, 0, cipher_out_.len);
        cipher_in_.reset();
        cipher_out_.reset();
        // AFTER mbedtls_ssl_free, never before: ssl_ holds an internal
        // pointer to the config, so releasing our reference first could
        // drop the last one and free the config out from under the very
        // call that is tearing ssl_ down. This is also the moment a
        // superseded config (one replaced by ReloadCerts while this
        // connection was mid-session) finally becomes free-able.
        conf_.reset();
        phase_ = Phase::Idle;
    }

    bool  IsActive()     const { return phase_ != Phase::Idle; }
    bool  IsEstablished() const { return phase_ == Phase::Established; }
    Phase GetPhase()      const { return phase_; }

    // --- Ciphertext IN staging (filled by the caller's own Recv completion) ---
    // Compacted before every fresh recv submission (the driving loop's own
    // job, not this class's) so writable() stays maximal rather than
    // shrinking permanently as off_ creeps toward len_.
    CipherSpan& CipherIn()  { return cipher_in_; }
    CipherSpan& CipherOut() { return cipher_out_; }

    // --- Handshake ---
    // One call to mbedtls_ssl_handshake(). Returns the RAW mbedtls return
    // code -- 0 (done), MBEDTLS_ERR_SSL_WANT_READ, MBEDTLS_ERR_SSL_WANT_WRITE,
    // or a genuine negative error -- for the caller's own driving loop
    // (ConnectionManager.h) to branch on. Never loops internally: mbedtls's
    // own non-blocking contract is that a BIO callback returning WANT_READ/
    // WANT_WRITE propagates straight out of this call rather than spinning
    // inside it, which is exactly what makes one call here correspond to
    // "make what progress is possible with the ciphertext already staged,
    // then tell the caller what it's waiting on."
    int DriveHandshake()
    {
        int ret = mbedtls_ssl_handshake(&ssl_);
        if (ret == 0) phase_ = Phase::Established;
        else if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
            phase_ = Phase::Error;
        return ret;
    }

    // --- Steady-state application data, once Established ---
    // One call each to mbedtls_ssl_read/mbedtls_ssl_write. Same "one call,
    // return the raw code, never spin" contract as DriveHandshake -- the
    // caller (NetworkProvider.h's do_recv/do_send) is what turns a
    // WANT_READ/WANT_WRITE into an actual io_uring submission and resumes
    // from the completion.
    int ReadPlain(unsigned char* out, size_t max_len)
    {
        int ret = mbedtls_ssl_read(&ssl_, out, max_len);
        if (ret < 0 && ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE
                    && ret != MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
            phase_ = Phase::Error;
        return ret;
    }

    int WritePlain(const unsigned char* in, size_t len)
    {
        int ret = mbedtls_ssl_write(&ssl_, in, len);
        if (ret < 0 && ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
            phase_ = Phase::Error;
        return ret;
    }

    static bool IsWouldBlock(int mbedtls_ret)
    {
        return mbedtls_ret == MBEDTLS_ERR_SSL_WANT_READ
            || mbedtls_ret == MBEDTLS_ERR_SSL_WANT_WRITE;
    }

private:
    // bioSend — appends into CipherOut(); never submits I/O itself (see
    // this class's own top comment). Returns WANT_WRITE if CipherOut is
    // full, which the driving loop avoids in practice by always fully
    // flushing CipherOut (offset loop over the real socket, same
    // partial-write discipline HttpServer::Serve's existing plaintext send
    // loop already uses) before making any mbedtls call that could produce
    // more ciphertext.
    static int bioSend(void* self_, const unsigned char* buf, size_t len)
    {
        auto* self = static_cast<TLSServerContext*>(self_);
        CipherSpan& out = self->cipher_out_;
        size_t room = out.writable();
        if (room == 0) return MBEDTLS_ERR_SSL_WANT_WRITE;
        size_t n = len < room ? len : room;
        std::memcpy(out.ptr + out.len, buf, n);
        out.len += n;
        return static_cast<int>(n);
    }

    // bioRecv — drains CipherIn(); returns WANT_READ when nothing is
    // staged. Never submits I/O itself (see this class's own top comment).
    static int bioRecv(void* self_, unsigned char* buf, size_t len)
    {
        auto* self = static_cast<TLSServerContext*>(self_);
        CipherSpan& in = self->cipher_in_;
        size_t avail = in.unread();
        if (avail == 0) return MBEDTLS_ERR_SSL_WANT_READ;
        size_t n = len < avail ? len : avail;
        std::memcpy(buf, in.ptr + in.off, n);
        in.off += n;
        return static_cast<int>(n);
    }

    mbedtls_ssl_context ssl_{};
    Phase               phase_ = Phase::Idle;
    CipherSpan          cipher_in_;
    CipherSpan          cipher_out_;
    // Keeps THIS session's config alive for exactly as long as the session
    // -- see Init's own comment. Held rather than borrowed so a reload can
    // never pull the config out from under a live handshake.
    std::shared_ptr<TLSServerConfig> conf_;
};

#endif // TLSSERVERCONTEXT_H__
