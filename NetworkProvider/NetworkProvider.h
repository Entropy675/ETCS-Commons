#ifndef NETWORKPROVIDER_H__
#define NETWORKPROVIDER_H__
#define ETCS_DLL_EXPORTS
#include "../../core_defs.h"
#include "../../ontology.h"
#include "Contract_NetworkProvider.h"
#include <algorithm>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Internal helper — allocates a SharedPage from the module-local arena and
// creates a Pipe pair. Used for in-module producer/consumer chains that drive
// ParseConcrete directly, not through the entity call() interface.
// Lifetime of the page is bounded to the calling function's frame.
// ---------------------------------------------------------------------------
struct LocalPipePair
{
    ETCS::MirrorBuffer  producer;
    ETCS::MirrorBuffer  consumer;
    ETCS::SharedPage*   page = nullptr;

    explicit LocalPipePair(const ETCS::SignalContext& ctx)
    {
        producer.bindContext(ctx);
        consumer.bindContext(ctx);
        page = ETCS::SharedPage::allocate(ETCS::MemoryArena::getInstance(), 0);
        ETCS::MirrorBuffer::makePair<ETCS::StrategyPipe, ETCS::SharedPage>(
            producer, consumer, page, 0, 0);
    }

    ~LocalPipePair()
    {
        ETCS::MirrorBuffer::teardownPair<ETCS::StrategyPipe, ETCS::SharedPage>(
            producer, consumer, page);
    }

    LocalPipePair(const LocalPipePair&)            = delete;
    LocalPipePair& operator=(const LocalPipePair&) = delete;
};

// ===========================================================================
// HttpServer — the root-level type. Config plus Start/Stop, per its own
// class comment (HttpServer.h).
// ===========================================================================

DEFINE_WORK_FUNC(HttpServer, SetPort)
{
    (void)ctx;
    int port = 8080;
    data >> port;
    bool ok = self.SetPort(port);
    if (ok)
        ETCS_LOG("HttpServer::SetPort", "port=" << port << " on RID:" << self.getRID());
    // Refusal is logged by SetPort itself, with the reason -- not repeated
    // here. The result goes back through data so a trace can see it.
    data.reset();
    data << ok;
}

// AddHandler <rid> <Action> — registers an out-of-tree recipient for
// connections. This is the ONE place a RID crosses into this structure from
// outside, and it is explicit for exactly that reason: everything downstream
// (server -> manager -> connections -> pages) is parent/child.
//
// Persists across Stop/Start: a handler is configuration, not generated state.
// AddHandler <rid> <Action> [<filter_rid> <FilterAction>] — registers a
// gate-level recipient: something that owns a whole connection, as opposed
// to AddRoute's per-path recipients. The optional filter is the same
// convention AddRoute already uses one layer up, forwarded straight through
// to ConnectionManager::RegisterConsumer -- this is what lets a script give
// a filtered gate-level consumer (a tarpit matching scan-shaped paths, say)
// to a server without ever needing to name the ConnectionManager the
// server mints internally on Start.
DEFINE_WORK_FUNC(HttpServer, AddHandler)
{
    (void)ctx;
    ETCS::RID   rid = 0;
    std::string action;
    data >> rid;
    data >> action;

    if (rid == 0 || action.empty())
    {
        ETCS_LOG("HttpServer::AddHandler",
                 "expected '<rid> <Action> [<filter_rid> <FilterAction>]' -- got: "
                 << data.buf);
        return;
    }

    // Both extractions are no-ops on an exhausted buffer, so the two-argument
    // form parses as unfiltered with no sentinel needed -- same shape
    // AddRoute's own parse uses.
    ETCS::RID   filter_rid = 0;
    std::string filter_action;
    data >> filter_rid;
    data >> filter_action;
    if ((filter_rid == 0) != filter_action.empty())
    {
        ETCS_LOG("HttpServer::AddHandler", "half a filter given -- registering unfiltered.");
        filter_rid = 0; filter_action.clear();
    }

    self.AddHandler(rid, action, filter_rid, filter_action);
    ETCS_LOG("HttpServer::AddHandler", "RID:" << rid << " ." << action
             << (filter_rid ? " filtered by RID:" + std::to_string(filter_rid)
                               + " ." + filter_action
                             : std::string(" (unfiltered)"))
             << " on RID:" << self.getRID());
}

// AddRoute <rid> <Action> [<filter_rid> <FilterAction>] — registers a
// request-level recipient. Unlike AddHandler (which decides who owns a
// connection), this decides who answers a PATH, which is the only layer where
// a key carried in the request can be seen at all.
DEFINE_WORK_FUNC(HttpServer, AddRoute)
{
    (void)ctx;
    ETCS::RID   rid = 0;
    std::string action;
    data >> rid;
    data >> action;

    if (rid == 0 || action.empty())
    {
        ETCS_LOG("HttpServer::AddRoute",
                 "expected '<rid> <Action> [<filter_rid> <FilterAction>]' -- got: "
                 << data.buf);
        return;
    }

    // Both extractions are no-ops on an exhausted buffer, so the two-argument
    // form parses as catch-all with no sentinel needed.
    ETCS::RID   filter_rid = 0;
    std::string filter_action;
    data >> filter_rid;
    data >> filter_action;
    if ((filter_rid == 0) != filter_action.empty())
    {
        ETCS_LOG("HttpServer::AddRoute", "half a filter given -- registering catch-all.");
        filter_rid = 0; filter_action.clear();
    }
    self.AddRoute(rid, action, filter_rid, filter_action);
}

DEFINE_WORK_FUNC(HttpServer, ClearRoutes)
{
    (void)data; (void)ctx;
    self.ClearRoutes();
}

DEFINE_WORK_FUNC(HttpServer, ClearHandlers)
{
    (void)data; (void)ctx;
    self.ClearHandlers();
}

// EnableTLS <cert_path> <key_path> — configures TLS termination for this
// server's gate. Deferred: this only stores the paths, and Start is what
// forwards them (see HttpServer::EnableTLS and StartConcrete for why that
// is the only point at which it can be done without racing the first
// client). Call it BEFORE Start; calling it after affects the next Start.
//
// If the cert or key cannot be loaded, Start REFUSES rather than falling
// back to plaintext -- so a typo in a path is a server that does not come
// up, not a server quietly serving HTTPS traffic in the clear.
DEFINE_WORK_FUNC(HttpServer, EnableTLS)
{
    (void)ctx;
    std::string cert_path;
    std::string key_path;
    data >> cert_path;
    data >> key_path;

    if (cert_path.empty() || key_path.empty())
    {
        ETCS_LOG("HttpServer::EnableTLS",
                 "expected '<cert_path> <key_path>' -- got: " << data.buf);
        data.reset();
        data << false;
        return;
    }

    bool ok = self.EnableTLS(cert_path, key_path);
    if (ok)
        ETCS_LOG("HttpServer::EnableTLS", "TLS configured on RID:" << self.getRID()
                 << " (cert='" << cert_path << "' key='" << key_path
                 << "') -- applied at Start.");
    // Refusal is logged by EnableTLS itself, with the reason.
    data.reset();
    data << ok;
}

// ReloadCerts — rotates the certificate on a running server, in place, with
// no dropped connections and no process restart. Takes no arguments: it
// re-reads the paths EnableTLS already stored, which is what a renewal
// rewrites in place.
//
// Nothing changes unless the new certificate loads cleanly, so a failed
// renewal leaves the server serving its existing one rather than taking the
// site down. Connections already established finish against the certificate
// they handshook with; only new ones see the new chain.
DEFINE_WORK_FUNC(HttpServer, ReloadCerts)
{
    (void)data; (void)ctx;
    bool ok = self.ReloadCerts();
    if (ok)
        ETCS_LOG("HttpServer::ReloadCerts", "certificate reloaded on RID:"
                 << self.getRID() << " -- live, no connections dropped.");
    // Failures are logged in full by the layer that knows why.
    data.reset();
    data << ok;
}

DEFINE_WORK_FUNC(HttpServer, Start)
{
    (void)data;
    // The context this server runs under, threaded down to the gate and from
    // there into every IOSubmission and every subscriber dispatch. Captured at
    // Start rather than construction: a server started from a detached script
    // should answer to that script's own signal scope, not to whatever
    // happened to be in scope when the entity was first created.
    self.SetRunContext(ctx);
    self.Start();
}

DEFINE_WORK_FUNC(HttpServer, Stop)
{
    (void)data; (void)ctx;
    self.Stop();
}

DEFINE_WORK_FUNC(HttpServer, IsStarted)
{
    (void)ctx;
    bool started = self.IsStarted();
    data.reset();
    data << started;
}

DEFINE_WORK_FUNC(HttpServer, ListPaths)
{
    // Logged, not returned through data -- a real tree's path list exceeds a
    // fixed-capacity Buffer long before it would be useful to a caller. Walks
    // every page child, so this reports what the SERVER would actually serve,
    // not what any one page holds.
    (void)ctx; (void)data;
    ETCS_LOG("HttpServer::ListPaths", "Paths served by RID:" << self.getRID() << ":");

    std::vector<std::pair<ETCS::Buffer, ETCS::RID>> children;
    self.getTypedChildren(children);
    for (auto& [tag, rid] : children)
    {
        ETCS::Entity* child = self.getTypedChild(tag, rid);
        if (!child) continue;
        if (!child->getInterfacePointer(ETCS::Buffer("HtmlPage"))) continue;

        ETCS_LOG("HttpServer::ListPaths", "  [" << tag.toString() << " RID:" << rid << "]");
        // FileHtmlPage is the only page type that enumerates; a static page
        // answers three fixed paths and has nothing to walk.
        //
        // Tag comparison, not dynamic_cast: getTypedChildren already handed
        // back the tag alongside the RID, and in this runtime the tag IS the
        // type -- so the concrete type is established before the pointer is,
        // and asking RTTI to re-derive it would be re-deciding something the
        // ontology already decided. getTrueType() then yields the
        // most-derived address, which is what makes the static_cast correct
        // under this family's virtual Entity inheritance where a plain
        // downcast would not be.
        if (tag == ETCS::Buffer("FileHtmlPage"))
        {
            FileHtmlPage* tree = static_cast<FileHtmlPage*>(child->getTrueType());
            for (const std::string& path : tree->ListAllPaths())
                ETCS_LOG("HttpServer::ListPaths", "    " << path);
        }
    }
}

DEFINE_WORK_FUNC(HttpServer, Delete)
{
    (void)data; (void)ctx;
    self.Delete();
}

// ---------------------------------------------------------------------------
// Serve — THE connection handler, and the default one. Invoked by the
// ConnectionManager once per accepted connection, with (manager_rid,
// connection_rid) in the payload.
//
// This single function replaces TestPageOld, TestPage, StartWebserver and
// ServeTree, which were four near-identical recv/parse/route/send loops
// differing only in how they resolved a path -- a hardcoded string, an
// if/else over three fixed names, and a tree walk. Routing now dispatches
// through HtmlPage_::Resolve, so this body is transport and protocol only,
// and a new page type needs no change here at all.
//
// Both RIDs are weak references crossing into this server from the manager's
// domain; resolving them is the boundary check, and everything afterward is
// strong (getTypedChild on a manager we hold a real pointer to).
// ---------------------------------------------------------------------------
DEFINE_WORK_FUNC(HttpServer, Serve)
{
    ETCS::RID manager_rid = 0;
    ETCS::RID conn_rid    = 0;
    data >> manager_rid;
    data >> conn_rid;

    ConnectionManager* mgr = self.GetManager();
    if (!mgr || mgr->getRID() != manager_rid)
    {
        ETCS_LOG("HttpServer::Serve", "manager RID:" << manager_rid
                 << " is not this server's gate -- refusing.");
        return;
    }

    ETCS_LOG("HttpServer::Serve", "invoked with manager RID:" << manager_rid
         << " conn RID:" << conn_rid);

    ETCS::Entity* found = mgr->getTypedChild(ETCS::Buffer("SocketConnectionState"), conn_rid);
    if (!found) return; // client gone between accept and dispatch

    SocketConnectionState* conn = static_cast<SocketConnectionState*>(found->getTrueType());
    if (!conn) return;

    // on_request -- what to do once ReadUntilParsed (ConnectionRecvLoop.h,
    // now shared with ConnectionManager's own gate-level pre-parse; see its
    // own top comment for why the recv/parse loop moved out of this
    // function entirely) hands back a connection whose one request is fully
    // parsed: build the response and send it.
    //
    // Self-referencing shared_ptr for the same reason the pre-shared-loop
    // do_recv needed one: do_send's own keep-alive re-arm calls back into
    // this closure by name, and a std::function cannot capture itself at
    // the point it is still being assigned.
    auto on_request = std::make_shared<std::function<void(SocketConnectionState*)>>();
    *on_request = [ctx, &self, on_request](SocketConnectionState* c) mutable
    {
        // This function is entered holding one io_inflight_ reference --
        // handed off, disarmed, by ReadUntilParsed's own on_complete
        // contract (ConnectionRecvLoop.h). Disarmed further down once
        // do_send takes it over.
        ConnRef entry = ConnRef::Wrap(c);

        std::string path(c->GetParser().GetPath(), c->GetParser().GetPathLen());

        // A query string is part of the request TARGET, not the path.
        // picohttpparser hands back the target verbatim, so "/?as=alice"
        // arrives here as the path and matches neither a route nor a page
        // -- the landing page 404s the moment anyone appends a parameter.
        // Strip it once, here, so every consumer below (route filters,
        // ResolvePath, the page tree) sees the same normalized path.
        //
        // The query itself is deliberately DISCARDED rather than parsed:
        // nothing in this server reads parameters, and the one thing that
        // wanted an identity carries it in the path instead
        // (/<mount>/<key>/<token>/<verb>), which is the shape a peer with
        // no server can also use.
        const size_t qpos = path.find('?');
        if (qpos != std::string::npos)
        {
            ETCS_LOG("HttpServer", "stripping query from '" << path << "'");
            path.erase(qpos);
        }

        // Routes first, pages second. A route is a live entity answering a
        // path; a page is stored content. route_body must outlive the send
        // below, since asset.data points into it rather than copying.
        ETCS::Buffer route_body;
        HtmlPage_::ResolvedAsset asset;
        if (self.DispatchRoute(path, route_body, ctx))
        {
            asset.matched   = true;
            asset.data      = route_body.buf;
            asset.length    = route_body.written;
            asset.mime_type = "text/plain";
        }
        else
        {
            asset = self.ResolvePath(path);
        }

        // HTTP/1.1 is persistent by default; only close when the client
        // asked, or when this connection has served its budget. Closing
        // per request is what filled the client's ephemeral port range
        // with TIME-WAIT and stalled connect() -- the visible symptom was
        // a hung page against an idle server with an empty backlog.
        const bool keep = c->GetParser().isPersistentConnection() && c->CanKeepAlive();
        const char* conn_hdr = keep ? "keep-alive" : "close";
        ETCS_LOG("HttpServer::Serve", "keepalive: minor_ver=" << c->GetParser().GetMinorVer()
             << " num_headers=" << c->GetParser().GetNumHeaders()
             << " persistent=" << c->GetParser().isPersistentConnection()
             << " served=" << c->Served()
             << " can_keep=" << c->CanKeepAlive()
             << " keep=" << keep);

        int send_len = 0;
        if (asset.matched)
        {
            // SendBuffer() is fixed (ETCS_NETWORK_MAX_HEADER_SIZE * 4).
            // Anything larger is clipped by snprintf's own bound rather
            // than corrupting memory, but is still a broken response.
            // Flagged loudly; the real fix is a chunked send loop feeding
            // several IOSubmission::Send calls, not yet wired here.
            if (asset.length + 256 > c->SendBuffer().size())
            {
                ETCS_LOG("HttpServer::Serve", "WARNING: asset '" << path << "' ("
                         << asset.length << " bytes) exceeds SendBuffer capacity ("
                         << c->SendBuffer().size() << ") -- response TRUNCATED.");
            }

            // Only the true fallback -- an extension MimeForExtension has
            // no explicit case for -- downloads instead of opening in-tab.
            // Every filtered type (html, css, js, images, fonts, wasm,
            // json, text/plain -- see MimeForExtension) is inline, so a
            // type only ever needs one line added there to be viewable
            // rather than downloaded; nothing here has to change. This
            // also means style.css/app.js still work when pulled in by
            // index.html regardless -- browsers only consult
            // Content-Disposition on a top-level navigation or explicit
            // fetch-and-save, never on a <link>/<script>/<img> subresource
            // load. Filename comes from the last path segment; '"' and
            // any stray CR/LF are stripped since `path` is
            // attacker-controlled input landing straight in a header.
            std::string disposition_hdr;
            if (asset.mime_type == "application/octet-stream")
            {
                size_t slash = path.find_last_of('/');
                std::string filename = (slash == std::string::npos)
                    ? path : path.substr(slash + 1);
                filename.erase(std::remove_if(filename.begin(), filename.end(),
                    [](unsigned char ch) { return ch == '"' || ch == '\\' || ch == '\r' || ch == '\n'; }),
                    filename.end());
                if (filename.empty()) filename = "download";
                disposition_hdr = "Content-Disposition: attachment; filename=\"" + filename + "\"\r\n";
            }

            send_len = snprintf(c->SendBuffer().data(), c->SendBuffer().size(),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: %s\r\n"
                "Content-Length: %zu\r\n"
                "Connection: %s\r\n"
                "Keep-Alive: timeout=%d\r\n"
                "%s"
                "\r\n%.*s",
                asset.mime_type.c_str(), asset.length, conn_hdr,
                SocketConnectionState::TIMEOUT_SECONDS,
                disposition_hdr.c_str(),
                static_cast<int>(asset.length), asset.data);
        }
        else
        {
            const char* err = "404 Not Found";
            send_len = snprintf(c->SendBuffer().data(), c->SendBuffer().size(),
                "HTTP/1.1 404 Not Found\r\nConnection: %s\r\n"
                "Content-Length: %zu\r\n\r\n%s",
                conn_hdr, std::strlen(err), err);
        }
        c->SetSendLen(send_len);

        ETCS_LOG("HttpServer::Serve", "Serving: "
                 << std::string(c->GetParser().GetMethod(), c->GetParser().GetMethodLen())
                 << " " << path << " (" << (asset.matched ? "200" : "404") << ")");

        // SEND UNTIL IT IS ALL GONE. A TCP send returns how many bytes the
        // socket ACCEPTED, not how many were asked for -- for a large
        // response that is whatever fits the send buffer, which varies with
        // window and memory pressure. One submission therefore delivers a
        // prefix, and the previous code treated the completion as the whole
        // thing: a page larger than the socket buffer arrived truncated at
        // a different point on every reload.
        //
        // do_send re-enters ITSELF (via its own captured shared_ptr, same
        // self-reference shape as on_request) once the response is fully
        // sent, rather than special-casing "just finished" separately at
        // every completion site -- its own top guard (`offset >= total`)
        // is the ONE place keep-alive re-arm vs close is decided, instead
        // of that logic being repeated once per completion path (raw
        // success, raw refused-submission never reaches it, TLS success).
        // Only the raw (non-TLS) branch below submits its own Send;
        // the TLS branch calls TLSIO::SubmitTLSSend directly, which already
        // loops internally over mbedtls's own partial-write semantics (see
        // TLSConnectionIO.h's own comment on why that is a separate loop
        // from this offset loop).
        auto do_send = std::make_shared<std::function<void(SocketConnectionState*, size_t)>>();
        *do_send = [ctx, keep, on_request, &self, do_send](SocketConnectionState* c, size_t offset) mutable
        {
            // Same reference-ownership contract as ReadUntilParsed and
            // every TarpitNode function: entered holding one reference,
            // released on every path out unless handed to new async work.
            ConnRef entry = ConnRef::Wrap(c);

            const size_t total = static_cast<size_t>(c->GetSendLen());
            if (total == 0 || offset >= total)
            {
                // Only a connection whose response fully landed can be
                // reused -- otherwise the next request would parse our own
                // leftover bytes as its request line.
                if (keep && c->IsConnectionOpen())
                {
                    c->RecycleForNextRequest();
                    entry.disarm();
                    ReadUntilParsed(c, &self, ctx,
                        [on_request](SocketConnectionState* cc) { (*on_request)(cc); });
                }
                else c->Reset();
                return;
            }

            if (c->GetTLS().IsEstablished())
            {
                // TLSIO::SubmitTLSSend owns the WHOLE plaintext offset loop
                // internally (its own ciphertext flush after every chunk
                // mbedtls_ssl_write actually consumes -- see its own
                // comment), so this call site is entered once, at the
                // current offset, and on_progress fires exactly once more:
                // with the final byte count on success, or a value <= 0 on
                // error (conn already Reset() by SubmitTLSSend in that case
                // -- see its own comment). Same Acquire-then-branch-on-
                // return-value shape as every other TLSIO call site.
                ConnRef work = ConnRef::Acquire(c);
                bool settled = TLSIO::SubmitTLSSend(c, &self, ctx, offset, total,
                    [c, keep, ctx, on_request, &self](long long progress)
                    {
                        ConnRef ref = ConnRef::Wrap(c);
                        if (progress > 0)
                        {
                            if (keep && c->IsConnectionOpen())
                            {
                                c->RecycleForNextRequest();
                                ref.disarm();
                                ReadUntilParsed(c, &self, ctx,
                                    [on_request](SocketConnectionState* cc) { (*on_request)(cc); });
                                return;
                            }
                            c->Reset();
                        }
                        // progress <= 0: SubmitTLSSend has ALREADY Reset()
                        // conn itself on that path -- nothing left to do but
                        // let `ref` release on the way out.
                    });
                if (settled) work.disarm();
                return;
            }

            ETCS::IOSubmission send_sub;
            send_sub.op         = ETCS::IOOp::Send;
            send_sub.fd         = c->GetClientFd();
            send_sub.buffer     = c->SendBuffer().data() + offset;
            send_sub.buffer_len = total - offset;
            send_sub.priority   = static_cast<int>(ETCS::Priority::Medium);
            send_sub.ctx        = ctx;

            auto send_scope = std::make_shared<ETCS::ScopeTag>(&self, "conn_io", ctx);
            send_sub.callback = [c, offset, total, do_send, send_scope = std::move(send_scope)]
                                (ETCS::IOCompletion comp) mutable
            {
                ConnRef ref = ConnRef::Wrap(c);
                // <= 0 covers both error and a zero-byte accept; treating 0
                // as progress would spin this loop forever on a dead peer.
                if (comp.result <= 0) { c->Reset(); return; }

                // Re-enter do_send at the new offset either way -- when
                // `now == total` this immediately hits its own top guard
                // and handles keep-alive/close from the one place that
                // decides it, rather than repeating that decision here.
                const size_t now = offset + static_cast<size_t>(comp.result);
                ref.disarm();
                (*do_send)(c, now);
            };

            ConnRef work = ConnRef::Acquire(c);
            if (c->getThreadPool().submit(std::move(send_sub))) { work.disarm(); }
            else
            {
                ETCS_LOG("HttpServer::Serve", "send refused (SQ full) at offset "
                         << offset << "/" << total << " -- dropping fd="
                         << c->GetClientFd());
                c->Reset();
            }
        };

        entry.disarm();
        (*do_send)(c, 0);
    };

    // A reference of Serve's OWN, acquired here and handed straight to
    // ReadUntilParsed -- which wraps it on entry and releases it on every
    // path out (its contract, ConnectionRecvLoop.h). Deliberately NOT
    // dispatchToSubscribers's dispatch reference: that one is owned by
    // dispatch's own `entry` ConnRef and released once this call returns,
    // whatever async work it started. Spending it here instead was a double
    // release on every dispatched connection -- io_inflight_ going negative,
    // and, because finalizeIfDraining only fires at exactly 0, a pool slot
    // that could never drain again.
    ConnRef work = ConnRef::Acquire(conn);
    work.disarm();          // ReadUntilParsed owns it from here
    ReadUntilParsed(conn, &self, ctx,
        [on_request](SocketConnectionState* c) { (*on_request)(c); });
}
// ===========================================================================
// ConnectionManager — the Gate_. All four actions forward to the concrete
// surface; the accept chain itself is internal (ConnectionManager.h).
// ===========================================================================

DEFINE_WORK_FUNC(ConnectionManager, Open)
{
    // The context this gate runs under -- see HttpServer::Start's own comment.
    // Set here too so a manager opened directly from a script (rather than by
    // a server's Start) still inherits that script's signal authority.
    self.SetOpenContext(ctx);
    bool ok = self.Open(data);
    data.reset();
    data << ok;
}

DEFINE_WORK_FUNC(ConnectionManager, Close)
{
    (void)ctx;
    bool ok = self.Close();
    data.reset();
    data << ok;
}

DEFINE_WORK_FUNC(ConnectionManager, IsOpen)
{
    (void)ctx;
    bool open = self.IsOpen();
    data.reset();
    data << open;
}

DEFINE_WORK_FUNC(ConnectionManager, RegisterConsumer)
{
    (void)ctx;
    ETCS::RID   rid = 0;
    std::string action;
    data >> rid;
    data >> action;
    ETCS_LOG("ConnectionManager", "RegisterConsumer for '" << action
         << "' on RID:" << rid);
         
    if (rid == 0 || action.empty())
    {
        ETCS_LOG("ConnectionManager::RegisterConsumer",
                 "expected '<rid> <Action> [<filter_rid> <FilterAction>]' -- got: "
                 << data.buf);
        return;
    }

    // Optional trailing pair. Both extractions are no-ops on an exhausted
    // buffer (numeric operator>> returns with val untouched, the std::string
    // one clears it), so the two-argument form parses as "unfiltered" without
    // needing a sentinel.
    ETCS::RID   filter_rid = 0;
    std::string filter_action;
    data >> filter_rid;
    data >> filter_action;
    if ((filter_rid == 0) != filter_action.empty())
    {
        ETCS_LOG("ConnectionManager::RegisterConsumer",
                 "half a filter given (rid=" << filter_rid << " action='"
                 << filter_action << "') -- registering UNFILTERED.");
        filter_rid = 0; filter_action.clear();
    }
    self.RegisterConsumer(rid, action, filter_rid, filter_action);
}

DEFINE_WORK_FUNC(ConnectionManager, UnregisterConsumer)
{
    (void)ctx;
    ETCS::RID rid = 0;
    data >> rid;
    self.UnregisterConsumer(rid);
}

DEFINE_WORK_FUNC(ConnectionManager, Delete)
{
    (void)data; (void)ctx;
    self.Delete();
}

// EnableTLS <cert_path> <key_path> — turns on server-side TLS termination
// for this gate (ConnectionManager::EnableTLS, ConnectionManager.h). A
// script calls this once, before Open, on any gate meant to speak TLS;
// every connection accepted afterward goes through the handshake phase
// (TLSConnectionIO.h's DriveTLSHandshake) before reaching a subscriber --
// HttpServer::Serve and everything else registered on this gate keeps
// seeing plaintext, unaware TLS ever happened.
DEFINE_WORK_FUNC(ConnectionManager, EnableTLS)
{
    (void)ctx;
    std::string cert_path;
    std::string key_path;
    data >> cert_path;
    data >> key_path;

    if (cert_path.empty() || key_path.empty())
    {
        ETCS_LOG("ConnectionManager::EnableTLS",
                 "expected '<cert_path> <key_path>' -- got: " << data.buf);
        data.reset();
        data << false;
        return;
    }

    bool ok = self.EnableTLS(cert_path, key_path);
    data.reset();
    data << ok;
}

// ReloadCerts [<cert_path> <key_path>] — gate-level certificate rotation,
// for a script-created bare ConnectionManager. HttpServer.ReloadCerts is
// the one to use for a server.
//
// With NO arguments it reloads the paths this gate already has, which is
// what a renewal wants: the files change, their location does not. Give
// paths only to point the gate somewhere genuinely different.
//
// Half a pair is refused rather than guessed at. Silently pairing a
// supplied cert with a remembered key would be a plausible reading and a
// terrible one -- a mismatched pair fails at handshake time, on real
// clients, rather than here.
DEFINE_WORK_FUNC(ConnectionManager, ReloadCerts)
{
    (void)ctx;
    std::string cert_path;
    std::string key_path;
    data >> cert_path;
    data >> key_path;

    bool ok;
    if (cert_path.empty() && key_path.empty())
    {
        ok = self.ReloadCerts();
    }
    else if (cert_path.empty() || key_path.empty())
    {
        ETCS_LOG("ConnectionManager::ReloadCerts",
                 "expected no arguments (reload the configured paths) or "
                 "'<cert_path> <key_path>' -- got: " << data.buf);
        ok = false;
    }
    else
    {
        ok = self.ReloadCerts(cert_path, key_path);
    }

    data.reset();
    data << ok;
}

// ===========================================================================
// HTTPParser — a Parser_ leaf and nothing else now. Every server action it
// used to carry (Listen, TestPage, TestPageOld, StartWebserver, StartREPL,
// ServeTree) is gone: accepting belongs to ConnectionManager, serving to
// HttpServer. What remains is what this type actually is -- an HTTP parse
// state machine.
// ===========================================================================

DEFINE_WORK_FUNC(HTTPParser, ParseRequest)
{
    self.SetMode(PicoHTTPParser::Mode::Request);
    LocalPipePair lpp(ctx);
    ETCS::Buffer frame;
    while (data.written > 0)
    {
        size_t chunk = std::min(data.written - data.read_offset, (size_t)ETCS::Buffer::bufsize);
        if (chunk == 0) break;
        std::memcpy(frame.buf, data.buf + data.read_offset, chunk);
        frame.written = chunk;
        lpp.producer.writeRaw(frame);
        data.read_offset += chunk;
    }
    lpp.producer.closeWrite();
    self.ParseConcrete(lpp.consumer, ctx);
}

DEFINE_WORK_FUNC(HTTPParser, ParseResponse)
{
    self.SetMode(PicoHTTPParser::Mode::Response);
    LocalPipePair lpp(ctx);
    ETCS::Buffer frame;
    while (data.written > 0)
    {
        size_t chunk = std::min(data.written - data.read_offset, (size_t)ETCS::Buffer::bufsize);
        if (chunk == 0) break;
        std::memcpy(frame.buf, data.buf + data.read_offset, chunk);
        frame.written = chunk;
        lpp.producer.writeRaw(frame);
        data.read_offset += chunk;
    }
    lpp.producer.closeWrite();
    self.ParseConcrete(lpp.consumer, ctx);
}

DEFINE_WORK_FUNC(HTTPParser, Delete)
{
    (void)data; (void)ctx;
    std::string conjugate_key = self.getSourceModule().toString() + ":"
                              + self.getSourceTag().toString();
    ETCS_LOG("HTTPParser::Delete", "firing self-DestroyEvent for RID:"
             << self.getRID() << " (" << conjugate_key << ")");
    // No stopping-flag dance and no scope wait anymore. Both existed because
    // this type used to host a server whose accept loop and per-connection I/O
    // had to be drained before it could safely die. It hosts nothing now, and
    // DestroyEvent's own drainEntityScopes already covers whatever parse work
    // might be in flight.
    ETCS::DestroyEvent{conjugate_key.c_str(), &self}();
}

DEFINE_STREAM_FUNC_CONSUME(HTTPParser, ConsumeRequest)
{
    (void)data;
    LocalPipePair lpp(ctx);
    self.SetMode(PicoHTTPParser::Mode::Request);

    ETCS::Buffer frame;
    while (stream.readRaw(frame))
    {
        if (ctx.isInterrupted() || ctx.isTerminated()) break;

        lpp.producer.writeRaw(frame);
        self.ParseConcrete(lpp.consumer, ctx);

        if (self.GetState() == PicoHTTPParser::State::Complete)
        {
            ETCS_LOG("HTTPParser::ConsumeRequest",
                     std::string(self.GetMethod(), self.GetMethodLen()) << " "
                     << std::string(self.GetPath(), self.GetPathLen()));
            break;
        }
        if (self.GetState() == PicoHTTPParser::State::Error)
        {
            ETCS_LOG("HTTPParser::ConsumeRequest", "Parse error");
            break;
        }
    }

    lpp.producer.closeWrite();
}

DEFINE_STREAM_FUNC_PRODUCE(HTTPParser, ProduceResponse)
{
    (void)self;
    const char* body     = data.buf;
    size_t      body_len = data.written;

    char response[ETCS::Buffer::bufsize * 4];
    int response_len = snprintf(response, sizeof(response),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%.*s",
        body_len, static_cast<int>(body_len), body);

    size_t offset = 0;
    while (offset < static_cast<size_t>(response_len))
    {
        if (ctx.isInterrupted() || ctx.isTerminated()) return;

        ETCS::Buffer frame;
        size_t chunk = std::min(
            static_cast<size_t>(response_len) - offset, (size_t)ETCS::Buffer::bufsize);
        std::memcpy(frame.buf, response + offset, chunk);
        frame.written = chunk;
        stream.writeRaw(frame);
        offset += chunk;
    }

    stream.closeWrite();
}

// ===========================================================================
// TLSContext — untouched by this restructure. Its handshake/send/receive path
// never went through the connection lifecycle, and folding TLS in properly
// means making it a Wrapper_ on the connection (the wrap-chain machinery),
// which is separate work.
// ===========================================================================

DEFINE_WORK_FUNC(TLSContext, SetupCerts)
{
    (void)ctx;
    if (!self.SetupSystemCerts()) { data.writeString("FAIL"); return; }
    data.writeString("OK");
}

DEFINE_WORK_FUNC(TLSContext, TestConnection)
{
    const char* hostname = data.buf;
    size_t hostname_len  = std::strlen(hostname) + 1;
    const char* port     = data.buf + hostname_len;

    self.SetHostname(hostname);

    mbedtls_net_context net_ctx;
    mbedtls_net_init(&net_ctx);

    int ret = mbedtls_net_connect(&net_ctx, hostname, port, MBEDTLS_NET_PROTO_TCP);
    if (ret != 0)
    {
        ETCS_LOG("TLSContext::TestConnection", "Connect failed");
        mbedtls_net_free(&net_ctx);
        data.writeString("FAIL: connect");
        return;
    }

    ETCS::SharedPage* page = ETCS::SharedPage::allocate(ETCS::MemoryArena::getInstance(), 0);
    ETCS::MirrorBuffer producer, consumer;
    ETCS::MirrorBuffer::makePair<ETCS::StrategySocket, ETCS::SharedPage>(
        producer, consumer, page,
        static_cast<uint64_t>(net_ctx.fd),
        static_cast<uint64_t>(net_ctx.fd));

    self.ParseConcrete(consumer, ctx);

    if (self.GetState() == MbedTLSContext::State::Complete ||
        self.GetState() == MbedTLSContext::State::Ready)
        data.writeString("OK: handshake complete");
    else
        data.writeString("FAIL: handshake");

    mbedtls_net_free(&net_ctx);
    ETCS::MirrorBuffer::teardownPair<ETCS::StrategySocket, ETCS::SharedPage>(
        producer, consumer, page);
}

DEFINE_WORK_FUNC(TLSContext, Handshake)
{
    const char* hostname = reinterpret_cast<const char*>(data.buf);
    self.SetHostname(hostname);

    size_t hostname_len      = std::strlen(hostname) + 1;
    const unsigned char* pem = reinterpret_cast<const unsigned char*>(data.buf + hostname_len);
    size_t pem_len           = data.written - hostname_len;

    if (!self.LoadCACert(pem, pem_len))
    {
        ETCS_LOG("TLSContext::Handshake", "Failed to load CA cert");
        return;
    }

    ETCS::MirrorBuffer net;
    net.bindContext(ctx);
    ETCS::MBuffer remainder;
    remainder.read_offset = 0;
    std::memcpy(remainder.buf, data.buf + hostname_len + pem_len,
                data.written - hostname_len - pem_len);
    remainder.written = data.written - hostname_len - pem_len;
    net.unpack(remainder);

    self.ParseConcrete(net, ctx);
}

DEFINE_WORK_FUNC(TLSContext, Close)
{
    (void)data; (void)ctx;
    self.ResetConcrete();
}

DEFINE_STREAM_FUNC_PRODUCE(TLSContext, SendData)
{
    if (self.GetState() != MbedTLSContext::State::Ready &&
        self.GetState() != MbedTLSContext::State::Decrypting)
    {
        ETCS_LOG("TLSContext::SendData", "TLS not ready");
        return;
    }

    ETCS::Buffer frame;
    size_t offset = 0;
    while (offset < data.written)
    {
        size_t chunk = std::min(data.written - offset, (size_t)ETCS::Buffer::bufsize);
        std::memcpy(frame.buf, data.buf + offset, chunk);
        frame.written = chunk;
        stream.writeRaw(frame);
        offset += chunk;

        if (ctx.isInterrupted() || ctx.isTerminated()) return;
    }
    stream.closeWrite();
}

DEFINE_STREAM_FUNC_CONSUME(TLSContext, ReceiveData)
{
    (void)data; (void)self;
    LocalPipePair lpp(ctx);
    HTTPParser http;
    http.SetMode(PicoHTTPParser::Mode::Response);

    ETCS::Buffer frame;
    while (stream.readRaw(frame))
    {
        if (ctx.isInterrupted() || ctx.isTerminated()) break;

        lpp.producer.writeRaw(frame);
        http.ParseConcrete(lpp.consumer, ctx);

        if (http.GetState() == PicoHTTPParser::State::Error)
        {
            ETCS_LOG("TLSContext::ReceiveData", "HTTP parse error");
            break;
        }
        if (http.GetState() == PicoHTTPParser::State::Complete)
        {
            ETCS_LOG("TLSContext::ReceiveData", "HTTP " << http.GetStatus() << " received");
            break;
        }
    }

    lpp.producer.closeWrite();
}

// ===========================================================================
// SocketConnectionState — pooled and recycled by its ConnectionManager, not
// created and destroyed per request. Still independently addressable for
// inspection from the shell, e.g.
//   list NetworkProvider::SocketConnectionState
//   NetworkProvider::SocketConnectionState.Close <rid>
// Delete is deliberately gone: the pool owns the lifetime.
// ===========================================================================

DEFINE_WORK_FUNC(SocketConnectionState, Close)
{
    (void)data; (void)ctx;
    self.CloseConnection();
}

// Asynchronous: this returns once the drain has STARTED. IsActive() stays
// true until the last outstanding submission retires, and only then does the
// pool see it as reusable.
DEFINE_WORK_FUNC(SocketConnectionState, Reset)
{
    (void)ctx;
    bool started = self.Reset();
    data.reset();
    data << started;
}

DEFINE_WORK_FUNC(SocketConnectionState, IsOpen)
{
    (void)ctx;
    bool open = self.IsConnectionOpen();
    data.reset();
    data << open;
}

DEFINE_WORK_FUNC(SocketConnectionState, SetPage)
{
    (void)ctx;
    ETCS::RID rid = 0;
    data >> rid;
    self.SetPageRID(rid);
}

DEFINE_WORK_FUNC(SocketConnectionState, GetPage)
{
    (void)ctx;
    ETCS::RID rid = self.GetPageRID();
    data.reset();
    data << rid;
}

// ===========================================================================
// StaticHtmlPage
// ===========================================================================

DEFINE_WORK_FUNC(StaticHtmlPage, SetHtmlFromFile) { (void)ctx; self.SetHtmlFromFile(data); }
DEFINE_WORK_FUNC(StaticHtmlPage, SetHtmlRaw)      { (void)ctx; self.SetHtmlRaw(ETCS::NBuffer(data)); }
DEFINE_WORK_FUNC(StaticHtmlPage, SetCssFromFile)  { (void)ctx; self.SetCssFromFile(data); }
DEFINE_WORK_FUNC(StaticHtmlPage, SetCssRaw)       { (void)ctx; self.SetCssRaw(ETCS::NBuffer(data)); }
DEFINE_WORK_FUNC(StaticHtmlPage, SetJsFromFile)   { (void)ctx; self.SetJsFromFile(data); }
DEFINE_WORK_FUNC(StaticHtmlPage, SetJsRaw)        { (void)ctx; self.SetJsRaw(ETCS::NBuffer(data)); }

DEFINE_WORK_FUNC(StaticHtmlPage, LogContent)
{
    (void)data; (void)ctx;
    ETCS_LOG("StaticHtmlPage::LogContent", "html: " << self.GetHtmlContent());
    ETCS_LOG("StaticHtmlPage::LogContent", "css:  " << self.GetCssContent());
    ETCS_LOG("StaticHtmlPage::LogContent", "js:   " << self.GetJsContent());
}

DEFINE_WORK_FUNC(StaticHtmlPage, Delete)
{
    (void)data; (void)ctx;
    self.Delete();
}

// ===========================================================================
// FileHtmlPage
// ===========================================================================

DEFINE_WORK_FUNC(FileHtmlPage, LoadFromDisk)
{
    (void)ctx;
    self.LoadFromDisk(data.toString());
    ETCS_LOG("FileHtmlPage::LoadFromDisk", "Loaded '" << data.toString()
             << "' into RID:" << self.getRID());
}

DEFINE_WORK_FUNC(FileHtmlPage, SetDefaultExtension)
{
    // Optional path-extension fallback for this tree: when a request path
    // segment is not found as written, try it once with this suffix. Empty
    // string clears the fallback (exact-name only). Leading '.' is optional.
    (void)ctx;
    ETCS::Buffer ext;
    data >> ext;
    self.SetDefaultExtension(ext.toString());
    ETCS_LOG("FileHtmlPage::SetDefaultExtension", "RID:" << self.getRID()
             << " default_extension='" << self.GetDefaultExtension() << "'");
}

DEFINE_WORK_FUNC(FileHtmlPage, MountExternal)
{
    (void)ctx;
    std::string segment;
    ETCS::RID   target_rid = 0;
    data >> segment;
    data >> target_rid;
    self.MountChild(segment, target_rid);
    ETCS_LOG("FileHtmlPage::MountExternal", "Mounted RID:" << target_rid
             << " at '" << segment << "' under RID:" << self.getRID());
}

DEFINE_WORK_FUNC(FileHtmlPage, EnsureFallback)
{
    (void)ctx;
    StaticHtmlPage* page = self.EnsureFallbackPage();
    data.reset();
    data << page->getRID();
}

DEFINE_WORK_FUNC(FileHtmlPage, ListPaths)
{
    (void)ctx; (void)data;
    ETCS_LOG("FileHtmlPage::ListPaths", "Paths under RID:" << self.getRID() << ":");
    for (const std::string& path : self.ListAllPaths())
        ETCS_LOG("FileHtmlPage::ListPaths", "  " << path);
}

DEFINE_WORK_FUNC(FileHtmlPage, Resolve)
{
    (void)ctx;
    std::string path = data.toString();
    HtmlPage_::ResolvedAsset asset = self.Resolve(path);
    data.reset();
    if (asset.matched)
        data.writeString(("MATCH " + asset.mime_type + " "
                          + std::to_string(asset.length) + " bytes").c_str());
    else
        data.writeString("NOT FOUND");
}

DEFINE_WORK_FUNC(FileHtmlPage, Delete)
{
    (void)data; (void)ctx;
    self.Delete();
}

// ===========================================================================
// TarpitNode — a self-registering Filter_ + gate-level consumer, the same
// two-action shape ChessNode/ChessGame already use for route-level
// filtering (ChessProvider.h's own Filter/Request pair). Registered via
// HttpServer::AddHandler's optional filter arguments (forwarded straight to
// ConnectionManager::RegisterConsumer), never wired specially into
// HttpServer or ConnectionManager themselves -- see TarpitNode.h's own top
// comment for the full reasoning.
// ===========================================================================

DEFINE_WORK_FUNC(TarpitNode, Filter)
{
    (void)ctx;
    self.Accepts(data);
}

// Payload is (manager_rid, conn_rid) -- ConnectionManager::dispatchToSubscribers'
// own frame, unpacked here exactly as HttpServer::Serve unpacks the same
// shape one type over.
DEFINE_WORK_FUNC(TarpitNode, Request)
{
    ETCS::RID manager_rid = 0;
    ETCS::RID conn_rid    = 0;
    data >> manager_rid;
    data >> conn_rid;
    self.ClaimAndDelay(manager_rid, conn_rid, ctx);
}

DEFINE_WORK_FUNC(TarpitNode, AddTarpitPattern)
{
    (void)ctx;
    self.AddTarpitPattern(data.toString());
}

DEFINE_WORK_FUNC(TarpitNode, ClearTarpitPatterns)
{
    (void)data; (void)ctx;
    self.ClearTarpitPatterns();
}

DEFINE_WORK_FUNC(TarpitNode, LoadDefaultTarpitPatterns)
{
    (void)data; (void)ctx;
    self.LoadDefaultTarpitPatterns();
    ETCS_LOG("TarpitNode::LoadDefaultTarpitPatterns", "RID:" << self.getRID()
             << " -- " << self.TarpitPatternCount() << " pattern(s) loaded.");
}

DEFINE_WORK_FUNC(TarpitNode, SetTarpitDelayMs)
{
    (void)ctx;
    int ms = 0;
    data >> ms;
    self.SetTarpitDelayMs(ms);
    data.reset();
    data << self.GetTarpitDelayMs();
}

DEFINE_WORK_FUNC(TarpitNode, Delete)
{
    (void)data; (void)ctx;
    self.Delete();
}

#endif
