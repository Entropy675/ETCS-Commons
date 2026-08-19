#ifndef HTTPSERVER_H__
#define HTTPSERVER_H__
#include "../../../ontology.h"
#include "ConnectionManager.h"
#include "StaticHtmlPage.h"
#include "FileHtmlPage.h"
#include <string>
#include <vector>

// HttpServer — the root-level type for a served site. Frankly a bag of
// configuration with Start/Stop attached, and that is the intended shape: it
// holds what a server IS (a port, a set of pages, a set of handlers) and, on
// Start, generates the structure that shape implies.
//
// Owns two kinds of typed children:
//   - Pages (any HtmlPage_ family type). Added by a script BEFORE Start, via
//     `web.add(FileHtmlPage tree)`. Serve walks them in attach order and takes
//     the first that resolves a given path, so several trees can be mounted at
//     once and the ordering is the script's own, visible in typed_child_order_.
//   - A ConnectionManager, minted BY Start and deleted by Stop. Not
//     script-created, because a manager is not shareable: two servers on one
//     gate would be two servers on one port with no defined behavior. One
//     server, one gate, and the gate exists exactly as long as the server is
//     started.
//
// Switchable_ rather than Gate_ (see Switchable.h): this owns no external
// handle. The gate is its child's concern, and Start()'s taking no config is
// what keeps the two families distinct -- a gate is a gate ONTO something and
// must be told what; a switch has no external referent to name. Everything
// configurable here is set beforehand by its own work actions, so each setting
// succeeds or fails on its own visible line.
class HttpServer :
    public SwitchableBase<HttpServer>, public DeletableBase<HttpServer>
{
public:
    WIRE_TYPE_IDENTITY(HttpServer);

    HttpServer() = default;
    virtual ~HttpServer() { StopConcrete(); }

    // --- Switchable_ concrete surface ---

    bool StartConcrete()
    {
        if (manager_) { ETCS_LOG("HttpServer", "Start: already started."); return false; }

        manager_ = addTag<ConnectionManager>();
        manager_->SetOpenContext(run_ctx_);

        // TLS BEFORE Open, not after. The gate begins accepting inside
        // Open -- there is no later moment a script could reach in which
        // the listener exists but has not yet taken a connection, so
        // enabling TLS from outside would always race the first client.
        // This is the same "stored as config, forwarded to the gate on
        // Start, wiring happens once at a known point" shape AddHandler's
        // own comment describes, and for the same reason.
        //
        // A FAILURE HERE FAILS THE START. Falling through would open a
        // port that a script explicitly asked to be TLS and serve it in
        // plaintext instead -- every client would either see garbage or,
        // worse, downgrade silently and send credentials in the clear.
        // A server that cannot honour the security it was configured with
        // must not run: refusing is the only safe reading of a bad cert
        // path, and it surfaces as one visible failed line, exactly like
        // a bad port does.
        if (IsTLSConfigured() && !manager_->EnableTLS(tls_cert_, tls_key_))
        {
            ETCS_LOG("HttpServer", "Start: TLS was configured (cert='" << tls_cert_
                     << "' key='" << tls_key_ << "') but could not be enabled -- "
                        "REFUSING to start in plaintext on port " << port_ << ".");
            manager_->Delete();
            manager_ = nullptr;
            return false;
        }

        ETCS::Buffer cfg;
        cfg << port_;
        if (!manager_->Open(cfg))
        {
            ETCS_LOG("HttpServer", "Start: gate failed to open on port " << port_
                     << " -- not started.");
            manager_->Delete();
            manager_ = nullptr;
            return false;
        }

        // Default handler: with no explicit handlers configured, the server
        // serves its own pages. Keeps the common case one line shorter, and a
        // server with pages but no handler would otherwise accept connections
        // and drop every one of them, which is never what anyone meant.
        if (handlers_.empty())
        {
            manager_->RegisterConsumer(getRID(), "Serve");
            ETCS_LOG("HttpServer", "Start: no handlers configured -- registered "
                     "own Serve as default.");
        }
        else
        {
            for (const auto& h : handlers_)
                manager_->RegisterConsumer(h.rid, h.action);
        }

        started_ = true;
        ETCS_LOG("HttpServer", "Start: serving on port " << port_
                 << " (RID:" << getRID() << ").");
        return true;
    }

    bool StopConcrete()
    {
        if (!manager_) { started_ = false; return true; }

        // Deleting the manager cascades to every connection it holds -- the
        // default now, and right: stopping a server should drop its
        // connections, and nothing else can reach them.
        manager_->Delete();
        manager_ = nullptr;
        started_ = false;
        ETCS_LOG("HttpServer", "Stop: stopped (RID:" << getRID() << ").");
        return true;
    }

    bool IsStartedConcrete() const { return started_; }

    // --- Deletable_ concrete surface ---

    bool DeleteConcrete()
    {
        StopConcrete();
        std::string conjugate_key = getSourceModule().toString() + ":" + getSourceTag().toString();
        ETCS_LOG("HttpServer", "Delete: firing self-DestroyEvent for RID:"
                 << getRID() << " (" << conjugate_key << ")");
        return ETCS::DestroyEvent{conjugate_key.c_str(), this}();
    }

    // --- Configuration surface ---

    // REFUSED WHILE STARTED. The listener is bound once, inside Open, and
    // nothing re-reads port_ afterwards -- so accepting a new value here
    // would leave this entity REPORTING one port while its gate serves
    // another, and a later Stop/Start would then silently come up on a port
    // nobody asked for. Config that cannot be applied live must not pretend
    // it was: same "visible failed line rather than silent divergence"
    // contract OpenConcrete already uses for a double Open.
    //
    // Re-declaring the port it is ALREADY on stays a quiet success, because
    // that is not a state change. Traces that re-bind to a running server do
    // exactly this -- chess_web.etcs opens with `SetPort 8080` whether or not
    // it minted the server it just found -- and turning a genuine no-op into
    // a warning would train people to ignore the warning.
    bool SetPort(int port)
    {
        if (port == port_) return true;
        if (started_)
        {
            ETCS_LOG("HttpServer", "SetPort: already started on port " << port_
                     << " (RID:" << getRID() << ") -- REFUSING to retarget to "
                     << port << ". Stop first.");
            return false;
        }
        port_ = port;
        return true;
    }
    int  GetPort() const   { return port_; }

    // Deferred, exactly like SetPort and AddHandler: this stores paths and
    // nothing else. Nothing is read, parsed or validated until Start hands
    // them to the gate (see StartConcrete above for why that is the only
    // safe point). So a bad path reported here would be a lie -- the
    // failure surfaces on Start, where it can actually refuse to serve.
    //
    // Persists across Stop/Start for the same reason handlers do: it is
    // configuration, not generated state, so restarting a server keeps the
    // security it was configured with rather than quietly dropping to
    // plaintext on the second Start.
    // Refused while started, for the same reason SetPort is and one worse:
    // the gate is already accepting, so a stored change would apply to
    // nothing, and the entity would read as TLS-configured while serving
    // plaintext. That is the single most dangerous thing this class could
    // misreport -- someone checking configuration to decide whether traffic
    // is encrypted would get the wrong answer. Stop first.
    bool EnableTLS(const std::string& cert_path, const std::string& key_path)
    {
        if (started_)
        {
            ETCS_LOG("HttpServer", "EnableTLS: already started on port " << port_
                     << " (RID:" << getRID() << ") -- REFUSING. The gate is "
                        "accepting and its transport cannot change under live "
                        "connections. Stop first.");
            return false;
        }
        tls_cert_ = cert_path;
        tls_key_  = key_path;
        return true;
    }

    bool ClearTLS()
    {
        if (started_)
        {
            ETCS_LOG("HttpServer", "ClearTLS: already started on port " << port_
                     << " (RID:" << getRID() << ") -- REFUSING. Stop first.");
            return false;
        }
        tls_cert_.clear();
        tls_key_.clear();
        return true;
    }

    bool IsTLSConfigured() const { return !tls_cert_.empty() && !tls_key_.empty(); }

    // Re-reads the certificate and key from the paths already configured,
    // on a RUNNING server, without dropping a single connection. This is
    // what a renewal hook should call instead of restarting the process.
    //
    // No arguments by design. The paths are already config (EnableTLS), and
    // a renewal writes new bytes to the SAME paths -- certbot's live/
    // directory is symlinks precisely so the path never changes. Taking
    // paths here would invite a reload that silently points somewhere else
    // than the configured location, which is a difference you would only
    // discover on the next restart.
    //
    // Deliberately preserves runtime state that a process restart destroys:
    // live games, whatever the forum holds in memory, every other entity in
    // the tree. A certificate rotation should not cost any of that.
    bool ReloadCerts()
    {
        if (!started_ || !manager_)
        {
            ETCS_LOG("HttpServer", "ReloadCerts: not started (RID:" << getRID()
                     << ") -- nothing to reload. Start reads the certificate "
                        "from disk anyway.");
            return false;
        }
        if (!IsTLSConfigured())
        {
            ETCS_LOG("HttpServer", "ReloadCerts: no TLS configured on RID:" << getRID()
                     << " -- refusing.");
            return false;
        }
        return manager_->ReloadCerts(tls_cert_, tls_key_);
    }

    void SetRunContext(const ETCS::SignalContext& ctx) { run_ctx_ = ctx; }

    // An out-of-tree recipient for connections. Stored as config and forwarded
    // to the gate on Start, so handlers can be declared in any order relative
    // to everything else and the wiring happens once, at a known point.
    // Persists across Stop/Start -- it is configuration, not generated state.
    void AddHandler(ETCS::RID rid, const std::string& action)
    {
        if (rid == 0 || action.empty()) return;
        for (auto& h : handlers_)
            if (h.rid == rid && h.action == action) return;
        handlers_.push_back(Handler{rid, action});
        if (manager_) manager_->RegisterConsumer(rid, action); // live add
    }

    void ClearHandlers()
    {
        handlers_.clear();
        if (manager_) for (auto& h : handlers_) manager_->UnregisterConsumer(h.rid);
    }

    ConnectionManager* GetManager() const { return manager_; }

    // Routes are configuration, exactly like handlers: declared before Start in
    // any order, and unaffected by Stop/Start since nothing about them is
    // generated state.
    void AddRoute(ETCS::RID rid, const std::string& action,
                  ETCS::RID filter_rid = 0, const std::string& filter_action = "")
    {
        if (rid == 0 || action.empty()) return;
        for (auto& r : routes_)
            if (r.rid == rid && r.action == action) return;
        routes_.push_back(Route{rid, action, filter_rid, filter_action});
        ETCS_LOG("HttpServer", "AddRoute: RID:" << rid << " ." << action
                 << (filter_rid ? " filtered by RID:" + std::to_string(filter_rid)
                                  + " ." + filter_action
                                : std::string(" (catch-all)"))
                 << " on RID:" << getRID());
    }

    void ClearRoutes() { routes_.clear(); }

    // A route target is out-of-tree, so it is addressed weakly by RID and
    // re-resolved per request rather than cached as a pointer.
    //
    // getLoader(), NOT EventNode::getInstance(): from module-compiled code the
    // latter is THIS module's own per-DSO singleton, whose ridMap holds only
    // the types THIS module exports. A route target in another module is
    // invisible there -- resolution returns null, the route is skipped, and the
    // request 404s with nothing logged to say why. The loader's own EventNode
    // is the only map that sees every module: registerLoader absorbs each
    // module's RIDLists into it under "Module:Tag" keys as that module loads.
    // Cross-module routing is the entire point of a route, so the loader's map
    // is the correct one to ask.
    //
    // RIDs are runtime-unique, so at most one list can hold a given one and the
    // first hit is the only hit.
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

    // Ask the routes, in order: filtered first (registration order), then
    // catch-all. Same precedence rule the gate uses, for the same reason -- a
    // catch-all registered early must not swallow traffic a later filtered
    // route was added to claim.
    //
    // On a match, `io` carries whatever the handler wrote, which becomes the
    // response body. Returns false if nothing claimed the path, leaving the
    // caller to fall through to the page tree.
    bool DispatchRoute(const std::string& path, ETCS::Buffer& io,
                       const ETCS::SignalContext& ctx) const
    {
        for (int pass = 0; pass < 2; ++pass)
        {
            for (const auto& r : routes_)
            {
                if (r.filtered() != (pass == 0)) continue;

                ETCS::Entity* target = resolveRID(r.rid);
                if (!target)
                {
                    ETCS_LOG("HttpServer", "route target RID:" << r.rid
                             << " no longer resolves -- skipping (path '" << path << "')");
                    continue;
                }

                if (r.filtered())
                {
                    ETCS::Entity* f = resolveRID(r.filter_rid);
                    if (!f)
                    {
                        ETCS_LOG("HttpServer", "route filter RID:" << r.filter_rid
                                 << " no longer resolves -- declining.");
                        continue;
                    }
                    // Descriptor in, equivalence key out; empty == declined.
                    ETCS::Buffer probe;
                    probe.writeString(path.c_str());
                    ETCS::Buffer faction = qualifiedAction(f, r.filter_action);
                    try { f->call(faction, probe, ctx); }
                    catch (const std::exception& ex)
                    {
                        ETCS_LOG("HttpServer", "route filter RID:" << r.filter_rid
                                 << " threw: " << ex.what() << " -- declining.");
                        continue;
                    }
                    // Two conditions, not one. Empty is the contract (Filter_
                    // declines by clearing), but a filter that forgets to clear
                    // would silently claim every path -- so an UNCHANGED probe
                    // counts as a decline too. Same reasoning as the untouched-
                    // payload check below: this layer cannot distinguish "wrote
                    // nothing" from "never ran" by inspection alone, so it
                    // treats "indistinguishable from what I handed in" as no.
                    if (probe.written == 0 || probe.toString() == path)
                    {
                        ETCS_LOG("HttpServer", "route filter RID:" << r.filter_rid
                                 << " declined '" << path << "'");
                        continue;
                    }
                }

                ETCS::Buffer payload;
                payload.writeString(path.c_str());
                ETCS::Buffer action = qualifiedAction(target, r.action);
                try { target->call(action, payload, ctx); }
                catch (const std::exception& ex)
                {
                    ETCS_LOG("HttpServer", "route RID:" << r.rid << " ." << r.action
                             << " threw: " << ex.what());
                    return false;
                }
                ETCS_LOG("HttpServer", "routed '" << path << "' -> RID:" << r.rid
                         << " ." << r.action);
                io = payload;
                return true;
            }
        }
        return false;
    }

    // Resolve a request path against this server's own pages, in attach order,
    // first match wins. The ENTIRE routing implementation -- previously four
    // near-identical consumers each hardcoded their own, which is what made
    // adding a page type mean editing every server.
    HtmlPage_::ResolvedAsset ResolvePath(const std::string& path) const
    {
        std::vector<std::pair<ETCS::Buffer, ETCS::RID>> children;
        getTypedChildren(children);

        for (auto& [tag, rid] : children)
        {
            ETCS::Entity* child = getTypedChild(tag, rid);
            if (!child) continue;

            // Family membership by interface pointer, not by tag name: any
            // HtmlPage_ answers, including types this module has never heard
            // of. ETCS_MAKE_INSTANCE registers this under the bare family
            // name for every member (see ETCS_API.h).
            void* raw = child->getInterfacePointer(ETCS::Buffer("HtmlPage"));
            if (!raw) continue;

            HtmlPage_* page = static_cast<HtmlPage_*>(raw);
            HtmlPage_::ResolvedAsset asset = page->Resolve(path);
            if (asset.matched) return asset;
        }
        return HtmlPage_::ResolvedAsset{};
    }

private:
    struct Handler
    {
        ETCS::RID   rid;
        std::string action;
    };

    // A ROUTE is the request-level counterpart to a gate subscriber. The gate
    // decides who owns an fd; a route decides who answers a PATH -- and the key
    // a route matches on (an invite code, a game id) only exists once the
    // request has been parsed, which is why this cannot live on the gate.
    // Optional filter, same convention as Subscriber: 0/"" means catch-all.
    struct Route
    {
        ETCS::RID   rid;
        std::string action;
        ETCS::RID   filter_rid    = 0;
        std::string filter_action;
        bool filtered() const { return filter_rid != 0 && !filter_action.empty(); }
    };
    int                  port_    = 8080;
    bool                 started_ = false;
    ConnectionManager*   manager_ = nullptr;

    // Paths only -- see EnableTLS's own comment. Both empty means plaintext,
    // which is the default and stays the default: nothing here changes the
    // behaviour of a server that never calls EnableTLS.
    std::string          tls_cert_;
    std::string          tls_key_;
    std::vector<Handler> handlers_;
    std::vector<Route>   routes_;
    ETCS::SignalContext  run_ctx_;
};

#endif // HTTPSERVER_H__
