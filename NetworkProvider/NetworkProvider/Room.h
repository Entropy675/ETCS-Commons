#ifndef NETWORKPROVIDER_ROOM_H__
#define NETWORKPROVIDER_ROOM_H__
#include "../../../ontology.h"
#include "Edge.h"

#include <memory>
#include <mutex>
#include <set>
#include <string>

/*
 * Room -- the HOST end of links, and so the session's authority.
 *
 * One edge per guest runtime (Edge.h), whichever way it arrived: straight
 * from a LinkHub on the same runtime (a native host serving its own site),
 * or dialled out through one (a host that cannot accept connections -- a
 * browser tab, a machine behind NAT -- keeps a control link open to the hub,
 * and dials back once per guest when told to; the hub only splices bytes,
 * LinkHub.h). Either way the nodes are here, their verbs run here, and the
 * order they run in is this runtime's.
 *
 * WHAT A GUEST CAN REACH is what was published, by name, behind the
 * authority layer each published node carries (its Wrapper children on the
 * network scope): a guest binds a surface only if the surface carries the
 * same layer. So a role is not a table here -- a reader is a guest that can
 * bind what readers are given, a writer one that can also fulfil what guards
 * the rest. Everything else in this runtime is out of reach, except through
 * what reaches it here.
 */
class Room : public DeletableBase<Room>
{
public:
    WIRE_TYPE_IDENTITY(Room);

    Room()  = default;
    ~Room() { shutdown(); }

    bool Publish(const std::string& name, ETCS::RID rid)
    {
        const bool ok = exports_.Publish(name, rid);
        if (ok) ETCS_LOG("Room", "published '" << name << "' -> RID:" << rid << " on RID:" << getRID());
        return ok;
    }
    bool Unpublish(const std::string& name) { return exports_.Unpublish(name); }
    void SetName(const std::string& n) { if (!n.empty()) name_ = n; }

    // A guest's socket, from any source: said hello to, then an edge.
    void AcceptChannel(int fd)
    {
        if (fd < 0) return;
        if (stopping_.load()) { ::close(fd); return; }
        threads_.start([this, fd]()
        {
            std::string guest;
            if (!etcs_link::hello_accept(fd, name_, guest)) { ::close(fd); return; }
            auto e = std::make_shared<etcs_link::Edge>(fd, getArena(), false, guest, &exports_);
            {
                std::lock_guard<std::mutex> lock(mu_);
                if (stopping_.load()) return;          // e's destructor closes fd
                edges_.insert(e);
            }
            ETCS_LOG("Room", "'" << guest << "' linked to RID:" << getRID());
            e->start();
        });
    }

    /*
     * Host THROUGH a hub at `url` (wss://site/link/<room>), for a runtime that
     * cannot listen. The control link is `<url>/host`; each "open <id>" on it
     * is a guest waiting, and dialling `<url>/accept/<id>` becomes that guest's
     * socket.
     */
    bool HostVia(const std::string& url, bool insecure)
    {
        std::string err;
        const int fd = etcs_ws::link_dial(url + "/host", insecure, err);
        if (fd < 0) { ETCS_LOG("Room", "HostVia " << url << ": " << err); return false; }
        via_url_ = url;
        ETCS_LOG("Room", "hosting through " << url << " (RID:" << getRID() << ")");
        threads_.start([this, fd, url, insecure]()
        {
            std::string msg;
            while (!stopping_.load())
            {
                if (!etcs_ws::wait_fd(fd, POLLIN, 250)) { if (!stopping_.load()) continue; break; }
                if (!etcs_ws::recv_frame(fd, msg, 5000)) break;
                if (msg.compare(0, 5, "open ") != 0) continue;
                std::string e;
                const int ch = etcs_ws::link_dial(url + "/accept/" + msg.substr(5), insecure, e);
                if (ch < 0) { ETCS_LOG("Room", "accept: " << e); continue; }
                AcceptChannel(ch);
            }
            ::close(fd);
            ETCS_LOG("Room", "control link to " << url << " closed.");
        });
        return true;
    }

    void Info()
    {
        std::lock_guard<std::mutex> lock(mu_);
        reapLocked();
        ETCS_LOG("Room", "RID:" << getRID() << " '" << name_ << "' guests:" << edges_.size()
                 << (via_url_.empty() ? "" : " via:" + via_url_));
        for (auto& [n, r] : exports_.snapshot()) ETCS_LOG("Room", "  " << n << " -> RID:" << r);
        for (auto& e : edges_)
            ETCS_LOG("Room", "  guest '" << e->farName() << "' channels:" << e->channels());
    }

    bool DeleteConcrete() override
    {
        shutdown();
        std::string key = getSourceModule().toString() + ":" + getSourceTag().toString();
        return ETCS::DestroyEvent{ key.c_str(), this }();
    }

private:
    void reapLocked()
    {
        for (auto it = edges_.begin(); it != edges_.end(); )
            it = (*it)->isOpen() ? std::next(it) : edges_.erase(it);
    }
    void shutdown()
    {
        stopping_.store(true);
        std::set<std::shared_ptr<etcs_link::Edge>> edges;
        { std::lock_guard<std::mutex> lock(mu_); edges.swap(edges_); }
        for (auto& e : edges) e->stop();
        threads_.wait();
    }

    std::mutex                                    mu_;
    etcs_link::Exports                            exports_;
    std::set<std::shared_ptr<etcs_link::Edge>>    edges_;
    std::string                                   name_ = "room";
    std::string                                   via_url_;
    std::atomic<bool>                             stopping_{ false };
    etcs_link::Threads                            threads_;
};

#endif // NETWORKPROVIDER_ROOM_H__
