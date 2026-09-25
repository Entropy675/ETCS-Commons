#ifndef NETWORKPROVIDER_LINKHUB_H__
#define NETWORKPROVIDER_LINKHUB_H__
#include "../../../ontology.h"
#include "ConnectionManager.h"
#include "ConnRef.h"
#include "RouteRequest.h"
#include "WebSocket.h"
#include "Edge.h"
#include "Room.h"

#include <map>
#include <memory>
#include <mutex>
#include <string>

/*
 * LinkHub -- where links to runtimes meet on a server: WebSocket upgrades
 * under one path prefix of an HttpServer it is a child of
 * (`web.spawn(NetworkProvider::LinkHub links)`), found by Serve the way pages
 * are, by walking the server's typed children.
 *
 *   /<prefix>/<room>                  a guest's channel into <room>
 *   /<prefix>/<room>/host             a remote host's control link
 *   /<prefix>/<room>/accept/<id>      a remote host taking guest channel <id>
 *
 * A ROOM IS HOSTED ONE OF TWO WAYS, and the guest cannot tell which:
 *
 *   Here -- `room.Host(@links studio)`: the Room is in this runtime, and a
 *   guest's channel is bridged straight to it.
 *
 *   Elsewhere -- the host dialled /host (Room::HostVia) because it cannot
 *   accept connections (a browser tab, a machine behind NAT). A guest's
 *   channel is parked, the host is told "open <id>" on its control link, and
 *   when it dials /accept/<id> the two WebSockets are SPLICED: bytes in one
 *   are bytes out of the other. The hub reads none of them. That is the whole
 *   of the server's part -- no store, no forward, no copy of the session --
 *   so the host stays the one place its session is ordered, which is what
 *   the old HTTP room could not be.
 *
 * A parked guest waits 10 s for its host to come for it.
 */
class LinkHub : public DeletableBase<LinkHub>
{
public:
    WIRE_TYPE_IDENTITY(LinkHub);

    LinkHub()  = default;
    ~LinkHub() { shutdown(); }

    void SetPrefix(const std::string& p) { std::lock_guard<std::mutex> lock(mu_); prefix_ = p.empty() ? "link" : p; }

    bool HostLocal(const std::string& room, ETCS::RID room_rid)
    {
        if (room.empty() || !room_rid) return false;
        std::lock_guard<std::mutex> lock(mu_);
        Slot& s = rooms_[room];
        if (s.ctl) { ETCS_LOG("LinkHub", "'" << room << "' is already hosted elsewhere."); return false; }
        s.local = room_rid;
        ETCS_LOG("LinkHub", "'" << room << "' hosted here by RID:" << room_rid);
        return true;
    }

    bool Claims(const RouteRequest& req) const
    {
        std::lock_guard<std::mutex> lock(mu_);
        return req.seg.size() >= 2 && req.seg[0] == prefix_;
    }

    void Info()
    {
        std::lock_guard<std::mutex> lock(mu_);
        ETCS_LOG("LinkHub", "RID:" << getRID() << " /" << prefix_ << "/ rooms:" << rooms_.size()
                 << " threads:" << threads_.live());
        for (auto& [n, s] : rooms_)
            ETCS_LOG("LinkHub", "  " << n << (s.local ? " here (Room RID:" + std::to_string(s.local) + ")"
                                                      : s.ctl ? " elsewhere" : " (no host)")
                     << " parked:" << s.pending.size());
    }

    /*
     * Takes over an upgraded connection, or answers false and leaves it
     * alone (the caller answers the request as an ordinary one). On true the
     * connection's entry reference is this hub's: the pump releases it when
     * the WebSocket ends (WsEnd::on_done), and the caller must not.
     */
    bool Upgrade(SocketConnectionState* c, ConnectionManager* mgr, const RouteRequest& req)
    {
#if defined(__EMSCRIPTEN__)
        (void)c; (void)mgr; (void)req;
        return false;
#else
        if (!c || !mgr || stop_.load()) return false;
        std::string upgrade, key, proto;
        const PicoHTTPParser& p = c->GetParser();
        for (size_t i = 0; i < p.GetNumHeaders(); ++i)
        {
            const phr_header& h = p.GetHeaders()[i];
            const std::string n(h.name, h.name_len), v(h.value, h.value_len);
            if      (strcasecmp(n.c_str(), "Upgrade") == 0)                upgrade = v;
            else if (strcasecmp(n.c_str(), "Sec-WebSocket-Key") == 0)      key = v;
            else if (strcasecmp(n.c_str(), "Sec-WebSocket-Protocol") == 0) proto = v;
        }
        if (strcasecmp(upgrade.c_str(), "websocket") != 0 || key.empty()) return false;

        enum class Kind { Join, Host, Accept } kind;
        const std::string room = req.at(1);
        if (req.seg.size() == 2) kind = Kind::Join;
        else if (req.seg.size() == 3 && req.seg[2] == "host") kind = Kind::Host;
        else if (req.seg.size() == 4 && req.seg[2] == "accept") kind = Kind::Accept;
        else return false;
        {
            std::lock_guard<std::mutex> lock(mu_);
            reapLocked();
            auto it = rooms_.find(room);
            const bool hosted = it != rooms_.end() && (it->second.local || it->second.ctl);
            if (kind == Kind::Join && !hosted) return false;
            if (kind == Kind::Host && it != rooms_.end() && (it->second.local || it->second.ctl)) return false;
            if (kind == Kind::Accept && (it == rooms_.end() || !it->second.pending.count(req.seg[3]))) return false;
        }
        if (!mgr->Hold(c)) return false;

        std::unique_ptr<etcs_ws::ByteIO> io;
        if (c->GetTLS().IsEstablished()) io = std::make_unique<etcs_ws::ServerTlsIO>(c->GetTLS(), c->GetClientFd());
        else                             io = std::make_unique<etcs_ws::PlainIO>(c->GetClientFd(), false);
        auto ws = std::make_shared<etcs_ws::WsEnd>(std::move(io), false, stop_);
        ws->alive    = [c]() { return c->IsConnectionOpen(); };
        ws->activity = [c]() { c->markActive(); };
        ws->on_done  = [c, mgr]() { c->Reset(); c->NoteComplete(); mgr->Unhold(c); };

        const bool binary = proto.find("binary") != std::string::npos;
        const std::string head = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
            "Connection: Upgrade\r\nSec-WebSocket-Accept: " + etcs_ws::accept_key(key) + "\r\n"
            + (binary ? "Sec-WebSocket-Protocol: binary\r\n" : "") + "\r\n";
        const std::string id = req.at(3);
        threads_.start([this, ws, head, kind, room, id]() mutable
        {
            if (!ws->raw(head)) return;
            switch (kind)
            {
                case Kind::Join:   join(std::move(ws), room); break;
                case Kind::Host:   host(std::move(ws), room); break;
                case Kind::Accept: accept(std::move(ws), room, id); break;
            }
        });
        return true;
#endif
    }

    bool DeleteConcrete() override
    {
        shutdown();
        std::string k = getSourceModule().toString() + ":" + getSourceTag().toString();
        return ETCS::DestroyEvent{ k.c_str(), this }();
    }

private:
#if !defined(__EMSCRIPTEN__)
    struct Parked
    {
        std::shared_ptr<etcs_ws::WsEnd>       ws;
        std::chrono::steady_clock::time_point until;
    };
#endif
    struct Slot
    {
        ETCS::RID local = 0;
        bool      ctl   = false;      // a remote host's control link is up
        int       ctl_fd = -1;        // this side of its socketpair
#if !defined(__EMSCRIPTEN__)
        std::map<std::string, Parked> pending;
#else
        std::map<std::string, int>    pending;
#endif
    };

#if !defined(__EMSCRIPTEN__)
    void join(std::shared_ptr<etcs_ws::WsEnd> ws, const std::string& room)
    {
        ETCS::RID local = 0;
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = rooms_.find(room);
            if (it == rooms_.end()) return;
            local = it->second.local;
            if (!local)
            {
                // Park it and ask the host to come for it.
                char hex[17];
                snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(etcs_link::random_token()));
                it->second.pending[hex] = Parked{ ws, std::chrono::steady_clock::now() + std::chrono::seconds(10) };
                if (!etcs_ws::send_frame(it->second.ctl_fd, std::string("open ") + hex))
                    it->second.pending.erase(hex);
                return;
            }
        }
        ETCS::Entity* e = ETCS::etcs_resolve_rid_anywhere(&ETCS::getLoader(), local);
        if (!e || e->getSourceTag().toString() != "Room") return;
        int sv[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return;
        static_cast<Room*>(e->getTrueType())->AcceptChannel(sv[0]);
        etcs_ws::FdEnd local_end(sv[1], stop_);
        etcs_ws::pump(*ws, local_end, stop_);
    }

    void host(std::shared_ptr<etcs_ws::WsEnd> ws, const std::string& room)
    {
        int sv[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return;
        {
            std::lock_guard<std::mutex> lock(mu_);
            Slot& s = rooms_[room];
            if (s.local || s.ctl) { ::close(sv[0]); ::close(sv[1]); return; }
            s.ctl = true;
            s.ctl_fd = sv[0];
        }
        ETCS_LOG("LinkHub", "'" << room << "' hosted elsewhere -- control link up.");
        etcs_ws::FdEnd ctl_end(sv[1], stop_);
        etcs_ws::pump(*ws, ctl_end, stop_);
        std::lock_guard<std::mutex> lock(mu_);
        auto it = rooms_.find(room);
        if (it != rooms_.end())
        {
            ::close(it->second.ctl_fd);
            rooms_.erase(it);           // parked guests go with it
        }
        ETCS_LOG("LinkHub", "'" << room << "' host left.");
    }

    void accept(std::shared_ptr<etcs_ws::WsEnd> ws, const std::string& room, const std::string& id)
    {
        std::shared_ptr<etcs_ws::WsEnd> guest;
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = rooms_.find(room);
            if (it == rooms_.end()) return;
            auto p = it->second.pending.find(id);
            if (p == it->second.pending.end()) return;
            guest = p->second.ws;
            it->second.pending.erase(p);
        }
        etcs_ws::pump(*guest, *ws, stop_);
    }

    void reapLocked()
    {
        const auto now = std::chrono::steady_clock::now();
        for (auto& [n, s] : rooms_)
            for (auto it = s.pending.begin(); it != s.pending.end(); )
                it = it->second.until < now ? s.pending.erase(it) : std::next(it);
    }
#else
    void reapLocked() {}
#endif

    void shutdown()
    {
        stop_.store(true);
        {
            std::lock_guard<std::mutex> lock(mu_);
            for (auto& [n, s] : rooms_) s.pending.clear();
        }
        threads_.wait();
        std::lock_guard<std::mutex> lock(mu_);
        rooms_.clear();
    }

    mutable std::mutex          mu_;
    std::string                 prefix_ = "link";
    std::map<std::string, Slot> rooms_;
    std::atomic<bool>           stop_{ false };
    etcs_link::Threads          threads_;
};

#endif // NETWORKPROVIDER_LINKHUB_H__
