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
    self.SetPort(port);
    ETCS_LOG("HttpServer::SetPort", "port=" << port << " on RID:" << self.getRID());
}

// AddHandler <rid> <Action> — registers an out-of-tree recipient for
// connections. This is the ONE place a RID crosses into this structure from
// outside, and it is explicit for exactly that reason: everything downstream
// (server -> manager -> connections -> pages) is parent/child.
//
// Persists across Stop/Start: a handler is configuration, not generated state.
DEFINE_WORK_FUNC(HttpServer, AddHandler)
{
    (void)ctx;
    ETCS::RID   rid = 0;
    std::string action;
    data >> rid;
    data >> action;

    if (rid == 0 || action.empty())
    {
        ETCS_LOG("HttpServer::AddHandler", "expected '<rid> <Action>' -- got: " << data.buf);
        return;
    }
    self.AddHandler(rid, action);
    ETCS_LOG("HttpServer::AddHandler", "RID:" << rid << " ." << action
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

    auto& pool = self.getThreadPool();

    // Recursive lambda held by value in its own capture -- the connection
    // stays in recv until a full request has been parsed, which may take
    // several completions for a request split across packets.
    // shared_ptr, not a by-value self-capture. `[do_recv]` copied the
    // std::function at LAMBDA CONSTRUCTION -- before the assignment completed
    // -- so the captured copy was empty and the re-entry below threw
    // bad_function_call. Only reachable when a request needs more than one
    // recv, i.e. when it arrives split across packets, which is why small GETs
    // never showed it. The throw unwound into the worker's catch, so the
    // connection was never recycled and its fd never closed.
    auto do_recv = std::make_shared<std::function<void(SocketConnectionState*)>>();
    *do_recv = [&pool, ctx, do_recv, &self](SocketConnectionState* c) mutable
    {
        if (c->checkTimeout()) return;
        if (!c->IsConnectionOpen()) return;     // draining; do not re-arm
        if (ctx.isInterrupted() || ctx.isTerminated()) { c->Reset(); return; }

        ETCS::IOSubmission sub;
        sub.op         = ETCS::IOOp::Recv;
        sub.fd         = c->GetClientFd();
        sub.buffer     = c->RecvBuffer().data();
        sub.buffer_len = c->RecvBuffer().size();
        sub.priority   = static_cast<int>(ETCS::Priority::Medium);
        sub.ctx        = ctx;

        // Registered against the SERVER, not the connection: this is what
        // makes a destroy of the server actually wait for in-flight
        // per-connection I/O. Without it the outer Serve call's own scope
        // clears the instant this function returns -- which is immediately,
        // since it only submits -- while every lambda it launched keeps
        // running untracked, free to touch an entity concurrently with
        // whatever is destroying it. shared_ptr because std::function needs a
        // copy-constructible target and ScopeTag's copy ctor is deleted.
        auto scope = std::make_shared<ETCS::ScopeTag>(&self, "conn_io", ctx);
        sub.callback = [&pool, ctx, c, do_recv, &self, scope = std::move(scope)]
                       (ETCS::IOCompletion comp) mutable
        {
            // NoteComplete is the LAST statement on every path out: it can
            // publish this connection as reusable, and anything touching c
            // afterwards races a new client's request.
            if (comp.result <= 0) { c->Reset(); c->NoteComplete(); return; }
            c->markActive();

            size_t incoming = static_cast<size_t>(comp.result);
            c->SetRecvLen(comp.result);
            bool complete = c->GetParser().FeedRaw(c->RecvBuffer().data(), incoming);

            if (c->GetParser().GetState() == PicoHTTPParser::State::Error)
            {
                ETCS_LOG("HttpServer::Serve", "Parse error on fd=" << c->GetClientFd());
                c->Reset();
                c->NoteComplete();
                return;
            }
            if (!complete || c->GetParser().GetState() != PicoHTTPParser::State::Complete)
            { (*do_recv)(c); c->NoteComplete(); return; }

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

                send_len = snprintf(c->SendBuffer().data(), c->SendBuffer().size(),
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: %s\r\n"
                    "Content-Length: %zu\r\n"
                    "Connection: %s\r\n"
                    "Keep-Alive: timeout=%d\r\n"
                    "\r\n%.*s",
                    asset.mime_type.c_str(), asset.length, conn_hdr,
                    SocketConnectionState::TIMEOUT_SECONDS,
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

            // SEND UNTIL IT IS ALL GONE. A TCP send returns how many bytes
            // the socket ACCEPTED, not how many were asked for -- for a large
            // response that is whatever fits the send buffer, which varies with
            // window and memory pressure. One submission therefore delivers a
            // prefix, and the previous code treated the completion as the whole
            // thing: a page larger than the socket buffer arrived truncated at
            // a different point on every reload.
            //
            // Same self-reference shape as do_recv, and for the same reason --
            // a by-value capture of a std::function still being assigned is
            // empty when the callback finally runs.
            auto do_send = std::make_shared<std::function<void(size_t)>>();
            // The function holds itself WEAKLY and hands a STRONG reference to
            // each callback. Both halves of the lifetime problem, and neither
            // one alone is enough:
            //   - strong in the function == a cycle, one leaked std::function
            //     per response;
            //   - weak in the callback == freed when Serve's frame returns,
            //     which is long before the send completes.
            // The in-flight callback is what keeps it alive, so the chain holds
            // exactly as long as there are bytes left, and frees on the last one.
            std::weak_ptr<std::function<void(size_t)>> send_w = do_send;
            *do_send = [&pool, ctx, c, keep, do_recv, send_w, &self](size_t offset) mutable
            {
                const size_t total = static_cast<size_t>(c->GetSendLen());
                if (total == 0 || offset >= total)
                {
                    if (keep && c->IsConnectionOpen())
                    { c->RecycleForNextRequest(); (*do_recv)(c); }
                    else c->Reset();
                    return;
                }

                auto self_fn = send_w.lock();   // strong, for the callback to hold

                ETCS::IOSubmission send_sub;
                send_sub.op         = ETCS::IOOp::Send;
                send_sub.fd         = c->GetClientFd();
                send_sub.buffer     = c->SendBuffer().data() + offset;
                send_sub.buffer_len = total - offset;
                send_sub.priority   = static_cast<int>(ETCS::Priority::Medium);
                send_sub.ctx        = ctx;

                auto send_scope = std::make_shared<ETCS::ScopeTag>(&self, "conn_io", ctx);
                send_sub.callback = [c, keep, do_recv, self_fn, offset, total,
                                     send_scope = std::move(send_scope)]
                                    (ETCS::IOCompletion comp) mutable
                {
                    // <= 0 covers both error and a zero-byte accept; treating 0
                    // as progress would spin this loop forever on a dead peer.
                    if (comp.result <= 0) { c->Reset(); c->NoteComplete(); return; }

                    const size_t now = offset + static_cast<size_t>(comp.result);
                    if (now < total) { (*self_fn)(now); c->NoteComplete(); return; }

                    // Only a connection whose response fully landed can be
                    // reused -- otherwise the next request would parse our own
                    // leftover bytes as its request line.
                    if (keep && c->IsConnectionOpen())
                    { c->RecycleForNextRequest(); (*do_recv)(c); }
                    else c->Reset();
                    c->NoteComplete();
                };

                c->NoteSubmit();
                if (!pool.submit(std::move(send_sub)))
                {
                    ETCS_LOG("HttpServer::Serve", "send refused (SQ full) at offset "
                             << offset << "/" << total << " -- dropping fd="
                             << c->GetClientFd());
                    c->NoteComplete();   // undo the submit that never happened
                    c->Reset();
                }
            };

            (*do_send)(0);
            c->NoteComplete();       // this recv is done
        };

        c->NoteSubmit();
        if (!pool.submit(std::move(sub)))
        {
            ETCS_LOG("HttpServer::Serve", "recv refused (SQ full) -- dropping fd="
                     << c->GetClientFd());
            c->NoteComplete();
            c->Reset();
        }
    };

    (*do_recv)(conn);
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

#endif
