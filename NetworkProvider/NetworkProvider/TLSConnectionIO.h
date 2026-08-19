#ifndef TLSCONNECTIONIO_H__
#define TLSCONNECTIONIO_H__
#include "../../../ontology.h"
#include "SocketConnectionState.h"
#include "TLSServerContext.h"
#include <functional>
#include "mbedtls/ssl.h"
#include "mbedtls/error.h"

// TLSConnectionIO -- the actual io_uring submission points for server-side
// TLS, shared by ConnectionManager.h (handshake phase, before subscriber
// dispatch) and NetworkProvider.h's HttpServer::Serve (steady-state
// application data, after dispatch). TLSServerContext.h itself never
// submits I/O (see its own top comment); everything here does, over the
// SAME ETCS::IOSubmission / NoteSubmit / NoteComplete / ScopeTag discipline
// every other submission in this module already follows.
//
// ── WHICH THREAD POOL ────────────────────────────────────────────────────
//
// conn->getThreadPool(), NOT ETCS::ThreadPool::getInstance().
//
// They are not interchangeable. A module is a separately loaded object with
// its own statics, so getInstance() resolves to the pool belonging to
// WHICHEVER MODULE THE CALL WAS COMPILED INTO -- which is only the right
// pool by coincidence, namely when that module also happens to own the fd.
// getThreadPool() on an entity resolves to the pool of the module that owns
// THAT ENTITY, which for a SocketConnectionState is always the module whose
// ConnectionManager minted and pooled it.
//
// That distinction has teeth here specifically because these functions are
// reachable from a subscriber, and a subscriber is explicitly allowed to be
// out-of-tree and in another module (ConnectionManager.h's own subscriber
// comment: the RID crossing is the whole point). If an out-of-module
// subscriber drove a connection through these helpers and they submitted on
// getInstance(), the ciphertext I/O for a NetworkProvider-owned fd would be
// queued on the SUBSCRIBER's ring while NetworkProvider's own drain, cancel
// and isDrained() checks watched a different one entirely. Binding the
// submission to the connection's own pool makes that structurally
// impossible rather than a convention someone has to remember.
//
// ── THE REFERENCE-OWNERSHIP CONTRACT, ONE TIME, FOR ALL FOUR FUNCTIONS ──
//
// Every function below is entered holding EXACTLY ONE io_inflight_
// reference (the same discipline do_recv/do_send in NetworkProvider.h
// already rely on) and returns a bool:
//
//   true  -- that entry reference has ALREADY been fully accounted for by
//            the time this call returns, because the CONTINUATION was
//            invoked and the continuation owns it (see below). The caller
//            of a `true`-returning call does NOTHING further -- no
//            NoteComplete, nothing.
//
//   false -- a NEW ciphertext IOSubmission is now in flight (its own
//            NoteSubmit already ran), and the ENTRY reference is still
//            outstanding, now spare relative to that new one. The caller
//            MUST call conn->NoteComplete() exactly once, immediately, as
//            its very next statement -- the identical
//            `(*do_recv)(c); c->NoteComplete();` hand-off shape used
//            throughout this module, just returned as a bool instead of
//            written out by hand at every call site.
//
// THE CONTINUATION OWNS THE REFERENCE. Every continuation parameter
// (on_drained / on_settled / on_plaintext / on_progress) is invoked with a
// live reference in hand and is responsible for releasing it exactly once,
// on every path out of itself. That is not a new rule invented here: it is
// exactly what ConnectionManager::dispatchToSubscribers already does with
// its own trailing NoteComplete(), and what HttpServer::Serve's plaintext
// handler already does on each of its branches. Stating it uniformly is
// what lets these four functions nest through each other -- synchronously,
// asynchronously, and recursively -- without any of them needing to know
// how deep it is or who called it.
//
// A CONSEQUENCE WORTH SPELLING OUT: a continuation is invoked on EVERY
// terminal path, including failures -- a refused submission, a dead peer, a
// short read. An earlier draft of this file released the reference directly
// on those paths and simply never called the continuation, which balanced
// the count but silently stranded the caller's own state machine (the
// handshake phase would never learn it had failed, and anything the caller
// had incremented on the way in would never be decremented). Failures are
// delivered, not swallowed.
//
// ── WHY EVERY ENTRY POINT RE-CHECKS THE SIGNAL CONTEXT ───────────────────
//
// THE PRIMARY REASON IS THAT AN INTERRUPT HAS TO ACTUALLY STOP THIS. Each
// individual callback here is short -- it makes one mbedtls call, submits
// one op, returns -- so no single one of them sits around long enough to
// look suspicious. The hazard is the CHAIN: every completion re-arms the
// next round-trip, so without a check the sequence keeps submitting fresh
// I/O forever after someone has killed the scope it runs under. The kill
// reports success, the scope stays busy, and nothing ever converges. That
// is the same shape as the inert `kill accept 0` ConnectionManager's own
// accept_scope_ comment describes -- a signal set somewhere nothing
// consults -- and the fix is the same: consult it, at the one place the
// chain can actually be stopped, which is before arming the next op.
//
// THE WATCHDOG IS THE BACKSTOP YOU DO NOT WANT TO REACH. ThreadPool's
// watchdog starts a 500ms clock when a signal appears on a running task's
// context and escalates if the task neither finishes nor notices
// (ThreadPool::watchdog_loop). That escalation is deliberately severe and
// its exact scope is a live design question -- today it takes the whole
// process down; it may narrow to killing and reloading just the offending
// module, replaying the ETCS trace that targeted it in sequence after the
// previous one. Either way the blast radius is far larger than one
// connection, so the cost of a missing check here is not a leaked pool
// slot -- it is everything else that was running at the time.
//
// Both reasons point at the same cheap guard, and it is the same one
// do_recv already opens with in NetworkProvider.h. Recursion re-enters
// through these entry points rather than looping internally, so one check
// at the top of each covers every subsequent round-trip too.
namespace TLSIO
{

// Drains TLSServerContext::CipherOut() to the wire, submitting as many
// Sends as the partial-write semantics require -- the carry-forward doc's
// own landmine: mbedtls_ssl_write's ciphertext output needs this exact
// same "send until it is all gone" accounting HttpServer::Serve's
// plaintext do_send already has, but as its own, separate loop. The
// existing plaintext offset loop does not and must not cover this: the two
// count different bytes (plaintext consumed by mbedtls vs ciphertext
// accepted by the socket) and neither is derivable from the other.
//
// on_drained(true)  -- CipherOut() reached empty.
// on_drained(false) -- the peer died or the ring refused the submission;
//                       the caller's own failure path decides what that
//                       means for it (whether to Reset, what to report).
inline bool FlushCipherOut(SocketConnectionState* conn, ETCS::Entity* owner,
                            const ETCS::SignalContext& ctx,
                            std::function<void(bool)> on_drained)
{
    // Re-checked on every recursion, not just at the start of the drain:
    // a large response is many Sends, and an interrupt landing halfway
    // through should stop arming more of them. See this file's own
    // watchdog note.
    if (ctx.isInterrupted() || ctx.isTerminated()) { on_drained(false); return true; }

    auto& out = conn->GetTLS().CipherOut();
    if (out.unread() == 0)
    {
        out.reset();
        on_drained(true);
        return true;
    }

    ETCS::IOSubmission sub;
    sub.op         = ETCS::IOOp::Send;
    sub.fd         = conn->GetClientFd();
    sub.buffer     = out.ptr + out.off;
    sub.buffer_len = out.unread();
    sub.priority   = static_cast<int>(ETCS::Priority::Medium);
    sub.ctx        = ctx;

    // Registered against `owner`, not against the connection -- same
    // reasoning HttpServer::Serve's own conn_io scope comment gives, and
    // the reason owner is a parameter here at all: the entity that should
    // be made to WAIT for this I/O is the one driving the connection (the
    // gate during the handshake phase, the server once it is serving), not
    // the connection itself, whose whole lifecycle is already governed by
    // the io_inflight_ count. Named tls_io rather than conn_io so the two
    // phases stay distinguishable in the shell's scope listing and can be
    // killed independently.
    auto scope = std::make_shared<ETCS::ScopeTag>(owner, "tls_io", ctx);
    sub.callback = [conn, owner, ctx, on_drained, scope = std::move(scope)]
                   (ETCS::IOCompletion comp) mutable
    {
        // <= 0 covers both error and a zero-byte accept, exactly like every
        // other send completion in this module -- treating 0 as progress
        // would spin this loop forever on a dead peer.
        if (comp.result <= 0) { on_drained(false); return; }
        conn->GetTLS().CipherOut().off += static_cast<size_t>(comp.result);
        if (!FlushCipherOut(conn, owner, ctx, on_drained))
            conn->NoteComplete();
    };
    conn->NoteSubmit();
    if (!conn->getThreadPool().submit(std::move(sub)))
    {
        ETCS_LOG("TLSConnectionIO", "FlushCipherOut: send refused (SQ full) on fd="
                 << conn->GetClientFd());
        conn->NoteComplete();   // undo the NoteSubmit -- that submission never happened
        on_drained(false);
        return true;
    }
    return false;
}

// Drives the TLS handshake to completion or failure. Called once from
// ConnectionManager::onConnection (holding the dispatch reference TryClaim
// seeded) and, recursively, from the completion callbacks of whatever
// ciphertext I/O it submits along the way.
//
// on_settled(true)  -- handshake Established. The caller hands the
//                       connection to ordinary subscriber dispatch here.
// on_settled(false) -- handshake failed: protocol error, dead peer,
//                       refused submission, or ciphertext staging
//                       exhausted. conn is NOT Reset() by this function on
//                       that path -- the caller decides, matching every
//                       other "declined connection" path in this module
//                       (e.g. onConnection's own no-subscribers branch).
inline bool DriveTLSHandshake(SocketConnectionState* conn, ETCS::Entity* owner,
                               const ETCS::SignalContext& ctx,
                               std::function<void(bool)> on_settled)
{
    // Before any mbedtls work, not after: a handshake is several round
    // trips with no request in it yet, and this is the phase most able to
    // outrun the watchdog's 500ms budget against a slow peer. See this
    // file's own watchdog note. IsConnectionOpen covers a Reset that
    // landed concurrently (reapIdle's own timeout sweep, or a manager
    // teardown) -- re-arming I/O on a draining connection would submit
    // against an fd finalizeIfDraining is about to close.
    if (ctx.isInterrupted() || ctx.isTerminated()) { on_settled(false); return true; }
    if (!conn->IsConnectionOpen())                 { on_settled(false); return true; }

    int ret = conn->GetTLS().DriveHandshake();

    // Hard failure first -- whatever is staged is not worth sending.
    if (ret != 0 && !TLSServerContext::IsWouldBlock(ret)) { on_settled(false); return true; }

    // FLUSH ON "THERE IS OUTPUT", NEVER ON "mbedtls SAID WANT_WRITE".
    //
    // This distinction is the whole handshake. bioSend does not write to a
    // socket -- it copies into CipherOut and returns the byte count, which
    // mbedtls reads as "sent". So mbedtls considers the flight delivered and
    // moves straight on to waiting for the peer's reply, returning WANT_READ.
    // WANT_WRITE only ever appears when bioSend REFUSES bytes, i.e. when the
    // staging buffer is full, which on a normal handshake never happens.
    //
    // So keying the flush off WANT_WRITE meant the flush effectively never
    // ran: the server parsed the ClientHello, produced its entire ServerHello
    // flight into CipherOut, got WANT_READ, and armed a recv -- with the
    // flight still sitting in the buffer. Client waits for a server that is
    // waiting for the client, until the 30s reaper closes the socket and the
    // client reports a bare EOF (PR_END_OF_FILE_ERROR in Firefox).
    //
    // Draining whenever anything is staged is the correct rule, and it has to
    // happen before BOTH remaining outcomes: before parking on a read (or the
    // peer never gets what it is answering) and before reporting success (or
    // subscribers start serving a client that never received the server's
    // Finished).
    if (conn->GetTLS().CipherOut().unread() > 0)
    {
        const bool finished = (ret == 0);
        return FlushCipherOut(conn, owner, ctx,
            [conn, owner, ctx, on_settled, finished](bool ok)
        {
            if (!ok)      { on_settled(false); return; }
            if (finished) { on_settled(true);  return; }
            if (!DriveTLSHandshake(conn, owner, ctx, on_settled))
                conn->NoteComplete();
        });
    }

    if (ret == 0) { on_settled(true); return true; }

    // WANT_READ with nothing staged: park on the peer. (WANT_WRITE cannot
    // reach here -- it is only produced by a full CipherOut, which the branch
    // above would have drained.) Compacted before every fresh submission so
    // writable() stays maximal rather than
    // stays maximal rather than shrinking permanently as off creeps toward
    // len -- safe precisely here because no mbedtls call is in progress and
    // nothing holds a raw pointer into the span across this point.
    auto& in = conn->GetTLS().CipherIn();
    in.compact();
    if (in.writable() == 0)
    {
        ETCS_LOG("TLSConnectionIO", "DriveTLSHandshake: ciphertext staging exhausted "
                 "on fd=" << conn->GetClientFd() << " without completing -- aborting.");
        on_settled(false);
        return true;
    }

    ETCS::IOSubmission sub;
    sub.op         = ETCS::IOOp::Recv;
    sub.fd         = conn->GetClientFd();
    sub.buffer     = in.ptr + in.len;
    sub.buffer_len = in.writable();
    sub.priority   = static_cast<int>(ETCS::Priority::Medium);
    sub.ctx        = ctx;

    auto scope = std::make_shared<ETCS::ScopeTag>(owner, "tls_io", ctx);
    sub.callback = [conn, owner, ctx, on_settled, scope = std::move(scope)]
                   (ETCS::IOCompletion comp) mutable
    {
        if (comp.result <= 0) { on_settled(false); return; }
        conn->GetTLS().CipherIn().len += static_cast<size_t>(comp.result);
        if (!DriveTLSHandshake(conn, owner, ctx, on_settled))
            conn->NoteComplete();
    };
    conn->NoteSubmit();
    if (!conn->getThreadPool().submit(std::move(sub)))
    {
        ETCS_LOG("TLSConnectionIO", "DriveTLSHandshake: recv refused (SQ full) on fd="
                 << conn->GetClientFd());
        conn->NoteComplete();
        on_settled(false);
        return true;
    }
    return false;
}

// Steady-state read, once Established. Mirrors an ordinary IOCompletion's
// own contract as closely as possible so HttpServer::Serve's plaintext
// handler barely has to change: on_plaintext is called with > 0 (that many
// decrypted bytes now sitting in conn->RecvBuffer(), ready for
// GetParser().FeedRaw exactly as today), 0 (clean EOF), or < 0 (error).
// conn is NOT Reset() by this function on the <= 0 paths -- the handler's
// own existing `if (result <= 0) { Reset(); NoteComplete(); }` branch
// already does the right thing, unchanged, for both transports.
inline bool SubmitTLSRecv(SocketConnectionState* conn, ETCS::Entity* owner,
                           const ETCS::SignalContext& ctx,
                           std::function<void(int)> on_plaintext)
{
    // Mirrors do_recv's own opening guards (NetworkProvider.h) so the TLS
    // branch declines to re-arm under exactly the conditions the raw
    // branch already does. See this file's own watchdog note.
    if (ctx.isInterrupted() || ctx.isTerminated()) { on_plaintext(-1); return true; }
    if (!conn->IsConnectionOpen())                 { on_plaintext(-1); return true; }

    auto& tls = conn->GetTLS();
    unsigned char* out = reinterpret_cast<unsigned char*>(conn->RecvBuffer().data());
    size_t out_cap     = conn->RecvBuffer().size();
    int ret = tls.ReadPlain(out, out_cap);

    // Same "flush on output present, not on WANT_WRITE" rule DriveTLSHandshake
    // documents at length -- see there for why the return code is the wrong
    // thing to key on. A read can legitimately produce ciphertext (a session
    // ticket acknowledgement, a close_notify reply, renegotiation traffic),
    // and none of it would ever leave the buffer if only WANT_WRITE triggered
    // a drain. Flushed before the result is delivered, so ordering on the
    // wire matches ordering in the state machine.
    if (tls.CipherOut().unread() > 0)
    {
        const int staged_ret = ret;
        return FlushCipherOut(conn, owner, ctx,
            [conn, owner, ctx, on_plaintext, staged_ret](bool ok)
        {
            if (!ok) { on_plaintext(-1); return; }
            if (staged_ret > 0) { on_plaintext(staged_ret); return; }
            if (staged_ret == 0 || staged_ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
            { on_plaintext(0); return; }
            if (!TLSServerContext::IsWouldBlock(staged_ret)) { on_plaintext(-1); return; }
            if (!SubmitTLSRecv(conn, owner, ctx, on_plaintext))
                conn->NoteComplete();
        });
    }

    if (ret > 0)                                       { on_plaintext(ret); return true; }
    if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
                                                        { on_plaintext(0);  return true; }
    if (!TLSServerContext::IsWouldBlock(ret))           { on_plaintext(-1); return true; }

    // WANT_READ, nothing staged.
    auto& in = tls.CipherIn();
    in.compact();
    if (in.writable() == 0)
    {
        ETCS_LOG("TLSConnectionIO", "SubmitTLSRecv: ciphertext staging exhausted on fd="
                 << conn->GetClientFd() << " -- a single TLS record exceeded the "
                 "staging window.");
        on_plaintext(-1);
        return true;
    }

    ETCS::IOSubmission sub;
    sub.op         = ETCS::IOOp::Recv;
    sub.fd         = conn->GetClientFd();
    sub.buffer     = in.ptr + in.len;
    sub.buffer_len = in.writable();
    sub.priority   = static_cast<int>(ETCS::Priority::Medium);
    sub.ctx        = ctx;

    auto scope = std::make_shared<ETCS::ScopeTag>(owner, "tls_io", ctx);
    sub.callback = [conn, owner, ctx, on_plaintext, scope = std::move(scope)]
                   (ETCS::IOCompletion comp) mutable
    {
        // Passed through rather than flattened to -1: result 0 is a clean
        // EOF and the handler distinguishes it from an error exactly as it
        // does for a raw recv completion.
        if (comp.result <= 0) { on_plaintext(static_cast<int>(comp.result)); return; }
        conn->GetTLS().CipherIn().len += static_cast<size_t>(comp.result);
        if (!SubmitTLSRecv(conn, owner, ctx, on_plaintext))
            conn->NoteComplete();
    };
    conn->NoteSubmit();
    if (!conn->getThreadPool().submit(std::move(sub)))
    {
        ETCS_LOG("TLSConnectionIO", "SubmitTLSRecv: recv refused (SQ full) on fd="
                 << conn->GetClientFd());
        conn->NoteComplete();
        on_plaintext(-1);
        return true;
    }
    return false;
}

// Steady-state write, once Established. offset/total are PLAINTEXT offsets
// into conn->SendBuffer(), exactly the offsets HttpServer::Serve's own
// do_send already tracks. This is the "own equivalent accounting" the
// carry-forward doc calls for: mbedtls_ssl_write has partial-write
// semantics over plaintext (it may consume less than requested per call,
// same as a raw send()), and each chunk it DOES consume must have its
// resulting ciphertext fully flushed before the next chunk is handed to
// it, since CipherOut() is a bounded window rather than unbounded.
//
// on_progress(total) -- everything sent and flushed.
// on_progress(-1)    -- failed; conn has ALREADY been Reset() here, since
//                        unlike a read failure there is no meaningful
//                        "clean EOF" reading of a half-written response and
//                        nothing downstream can do anything but drop it.
inline bool SubmitTLSSend(SocketConnectionState* conn, ETCS::Entity* owner,
                           const ETCS::SignalContext& ctx,
                           size_t offset, size_t total,
                           std::function<void(long long)> on_progress)
{
    if (offset >= total) { on_progress(static_cast<long long>(total)); return true; }

    // See this file's own watchdog note. Reset before reporting, matching
    // every other failure path in this function -- a half-written response
    // has no clean reading and nothing downstream can do with it.
    if (ctx.isInterrupted() || ctx.isTerminated())
    { conn->Reset(); on_progress(-1); return true; }
    if (!conn->IsConnectionOpen())
    { conn->Reset(); on_progress(-1); return true; }

    auto& tls = conn->GetTLS();
    const unsigned char* plain =
        reinterpret_cast<const unsigned char*>(conn->SendBuffer().data()) + offset;
    int ret = tls.WritePlain(plain, total - offset);

    // One drain covers every outcome that can leave ciphertext staged: a
    // successful write (the encrypted record itself), a WANT_WRITE (staging
    // filled mid-record), and a WANT_READ that still emitted an alert. Keyed
    // on output present rather than on the return code, for the reason
    // DriveTLSHandshake documents at length.
    if (tls.CipherOut().unread() > 0)
    {
        const size_t new_offset = (ret > 0) ? offset + static_cast<size_t>(ret) : offset;
        const int    staged_ret = ret;
        return FlushCipherOut(conn, owner, ctx,
            [conn, owner, ctx, new_offset, total, on_progress, staged_ret](bool ok)
        {
            if (!ok) { conn->Reset(); on_progress(-1); return; }
            if (staged_ret <= 0 && !TLSServerContext::IsWouldBlock(staged_ret))
            { conn->Reset(); on_progress(-1); return; }
            if (!SubmitTLSSend(conn, owner, ctx, new_offset, total, on_progress))
                conn->NoteComplete();
        });
    }

    if (ret > 0)
    {
        // Consumed plaintext without emitting anything yet (mbedtls buffering
        // a partial record) -- carry on from the new offset.
        // Return value propagated verbatim: the recursive call inherits this
        // call's entry reference, so whatever it reports about that reference
        // is exactly what our own caller must act on.
        return SubmitTLSSend(conn, owner, ctx, offset + static_cast<size_t>(ret),
                             total, on_progress);
    }
    if (!TLSServerContext::IsWouldBlock(ret))
    {
        conn->Reset();
        on_progress(-1);
        return true;
    }

    // WANT_READ, nothing staged -- e.g. a ticket/alert exchange needs incoming
    // before mbedtls will accept more outbound plaintext.
    auto& in = tls.CipherIn();
    in.compact();
    if (in.writable() == 0)
    {
        ETCS_LOG("TLSConnectionIO", "SubmitTLSSend: ciphertext staging exhausted on fd="
                 << conn->GetClientFd());
        conn->Reset();
        on_progress(-1);
        return true;
    }

    ETCS::IOSubmission sub;
    sub.op         = ETCS::IOOp::Recv;
    sub.fd         = conn->GetClientFd();
    sub.buffer     = in.ptr + in.len;
    sub.buffer_len = in.writable();
    sub.priority   = static_cast<int>(ETCS::Priority::Medium);
    sub.ctx        = ctx;

    auto scope = std::make_shared<ETCS::ScopeTag>(owner, "tls_io", ctx);
    sub.callback = [conn, owner, ctx, offset, total, on_progress, scope = std::move(scope)]
                   (ETCS::IOCompletion comp) mutable
    {
        if (comp.result <= 0) { conn->Reset(); on_progress(-1); return; }
        conn->GetTLS().CipherIn().len += static_cast<size_t>(comp.result);
        if (!SubmitTLSSend(conn, owner, ctx, offset, total, on_progress))
            conn->NoteComplete();
    };
    conn->NoteSubmit();
    if (!conn->getThreadPool().submit(std::move(sub)))
    {
        ETCS_LOG("TLSConnectionIO", "SubmitTLSSend: recv refused (SQ full) on fd="
                 << conn->GetClientFd());
        conn->NoteComplete();
        conn->Reset();
        on_progress(-1);
        return true;
    }
    return false;
}

} // namespace TLSIO

#endif // TLSCONNECTIONIO_H__
