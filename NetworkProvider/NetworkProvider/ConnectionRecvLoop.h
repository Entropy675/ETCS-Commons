#ifndef CONNECTIONRECVLOOP_H__
#define CONNECTIONRECVLOOP_H__
#include "SocketConnectionState.h"
#include "TLSConnectionIO.h"
#include "ConnRef.h"
#include <functional>

// ReadUntilParsed — the ONE recv/parse loop, shared by ConnectionManager's
// own pre-parse (preParseThenDispatch, ConnectionManager.h) and
// HttpServer::Serve's steady-state recv (NetworkProvider.h).
//
// THIS USED TO BE TWO COPIES. The first version of the gate-level pre-parse
// feature (see git log) duplicated do_recv/on_plaintext's recv-issuing
// logic into ConnectionManager.h rather than sharing it, on the reasoning
// that the two were "not causally the same shape" -- but that reasoning
// does not actually hold here the way it does for TarpitNode's own send
// loop (which stays deliberately separate from HttpServer::Serve's do_send
// -- see TarpitNode.h). A tarpit's send and an ordinary response's send
// really do want different things: one paces itself and always closes, the
// other sends as fast as the socket allows and re-arms keep-alive. Reading
// bytes off a connection until one HTTP request is fully parsed has no such
// difference between callers -- ConnectionManager wants exactly what
// HttpServer::Serve already wanted, for the identical reason (there is a
// request here and nothing is usable until it is parsed), and a future
// change to picohttpparser's own state machine, a new guard either caller
// needs, would have had to be kept in sync by hand across two copies for no
// benefit. One function, two callers.
//
// `on_complete` is the CONTINUATION: called once with a connection whose
// parser is State::Complete, meaning "here is a fully parsed request, do
// something with it" -- ConnectionManager's own caller hands this a
// dispatchToSubscribers wrapper; HttpServer::Serve's hands this its own
// route/response-building continuation. Never called on failure (parse
// error, closed connection, refused submission) -- both callers already
// have nothing more to do on that path than what this function itself does
// (Reset and let the reference drop), so there is nothing for a failure
// continuation to add.
//
// REFERENCE-OWNERSHIP CONTRACT: entered holding exactly one io_inflight_
// reference (see ConnRef.h), which this function releases on every path out
// UNLESS it hands the connection to `on_complete` -- at that point
// on_complete owns the reference exactly the way every other continuation
// in this module does (TLSConnectionIO.h's own top-of-file contract; this
// is the same rule, just enforced by ConnRef's destructor instead of a
// hand-written NoteComplete at each branch).
inline void ReadUntilParsed(SocketConnectionState* conn, ETCS::Entity* owner,
                             const ETCS::SignalContext& ctx,
                             std::function<void(SocketConnectionState*)> on_complete)
{
    ConnRef entry = ConnRef::Wrap(conn);

    if (conn->checkTimeout())                      return;   // entry releases; nothing to re-arm
    if (!conn->IsConnectionOpen())                  return;   // draining; do not re-arm
    if (ctx.isInterrupted() || ctx.isTerminated())  { conn->Reset(); return; }

    // Already fully parsed -- a caller can hand this a connection whose one
    // request is already sitting in the parser (ConnectionManager's own
    // pre-parse having already run it once; see preParseThenDispatch's own
    // comment for why that happens). Skip straight to the continuation
    // rather than reading again: same parser, same buffer, no bytes read
    // twice.
    if (conn->GetParser().GetState() == PicoHTTPParser::State::Complete)
    {
        entry.disarm();
        on_complete(conn);
        return;
    }

    // Shared "plaintext bytes have arrived" handler, regardless of whether
    // those bytes came straight off the wire or out of
    // TLSServerContext::ReadPlain via TLSIO::SubmitTLSRecv -- that is the
    // whole point of terminating TLS below this layer rather than above it.
    // `result` mirrors ETCS::IOCompletion::result's own contract exactly
    // (>0 bytes ready in RecvBuffer(), <=0 closed/error).
    auto on_bytes = [conn, owner, ctx, on_complete](int result) mutable
    {
        ConnRef ref = ConnRef::Wrap(conn);
        if (result <= 0) { conn->Reset(); return; }
        conn->markActive();

        size_t incoming = static_cast<size_t>(result);
        conn->SetRecvLen(result);
        bool complete = conn->GetParser().FeedRaw(conn->RecvBuffer().data(), incoming);

        if (conn->GetParser().GetState() == PicoHTTPParser::State::Error)
        {
            ETCS_LOG("ReadUntilParsed", "Parse error on fd=" << conn->GetClientFd());
            conn->Reset();
            return;
        }
        if (!complete || conn->GetParser().GetState() != PicoHTTPParser::State::Complete)
        {
            // Need more data -- recurse. Handing this exact reference to
            // the recursive call rather than adding a new one: its own
            // entry ConnRef picks up right where this one leaves off.
            ref.disarm();
            ReadUntilParsed(conn, owner, ctx, on_complete);
            return;
        }

        ref.disarm();
        on_complete(conn);
    };

    if (conn->GetTLS().IsEstablished())
    {
        // Same NoteSubmit-then-branch-on-return-value shape every TLSIO
        // call site uses (TLSConnectionIO.h's own top-of-file contract):
        // settled==true means on_bytes already ran synchronously to
        // completion and released (or disarmed onward) this reference
        // itself; settled==false means a new ciphertext op is outstanding
        // and `work` must stay armed so its own destructor releases this
        // call's reference immediately, as the contract requires.
        ConnRef work = ConnRef::Acquire(conn);
        bool settled = TLSIO::SubmitTLSRecv(conn, owner, ctx, on_bytes);
        if (settled) work.disarm();
        return;
    }

    ETCS::IOSubmission sub;
    sub.op         = ETCS::IOOp::Recv;
    sub.fd         = conn->GetClientFd();
    sub.buffer     = conn->RecvBuffer().data();
    sub.buffer_len = conn->RecvBuffer().size();
    sub.priority   = static_cast<int>(ETCS::Priority::Medium);
    sub.ctx        = ctx;

    // Registered against `owner` (the server or gate driving this
    // connection), not the connection itself -- same reasoning every other
    // conn_io scope in this module gives: it is what makes a teardown of
    // the owner actually wait for in-flight per-connection I/O.
    auto scope = std::make_shared<ETCS::ScopeTag>(owner, "conn_io", ctx);
    sub.callback = [on_bytes, scope = std::move(scope)](ETCS::IOCompletion comp) mutable
    {
        on_bytes(static_cast<int>(comp.result));
    };

    ConnRef work = ConnRef::Acquire(conn);
    if (conn->getThreadPool().submit(std::move(sub)))
    {
        work.disarm();   // the submitted op's own completion will reclaim this reference
    }
    else
    {
        ETCS_LOG("ReadUntilParsed", "recv refused (SQ full) -- dropping fd="
                 << conn->GetClientFd());
        conn->Reset();
        // `work` stays armed -> releases the just-acquired reference when
        // this function returns. `entry` (declared at the top, never
        // disarmed on this path) releases the original one right alongside
        // it -- both references correctly gone, nothing left outstanding.
    }
}

#endif // CONNECTIONRECVLOOP_H__
