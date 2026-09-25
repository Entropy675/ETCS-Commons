#ifndef NETWORKPROVIDER_PEER_H__
#define NETWORKPROVIDER_PEER_H__
#include "../../../ontology.h"
#include "Edge.h"

#include <map>
#include <memory>
#include <string>

/*
 * Peer -- this runtime's link to a Room in another one: the side that dials.
 *
 * ONE PEER PER ADDRESS, and so one edge between the two runtimes: every
 * surface bound through it, every verb and every stream shares its socket
 * (Edge.h). A second Connect to an address another Peer already holds is
 * refused and names that Peer -- bind through it.
 *
 * A Peer may publish too; the edge is symmetric, so what it publishes the
 * far side can bind the same way.
 */
class Peer : public DeletableBase<Peer>
{
public:
    WIRE_TYPE_IDENTITY(Peer);

    Peer()  = default;
    ~Peer() { Close(); }

    void SetName(const std::string& n) { if (!n.empty()) name_ = n; }

    bool Connect(const std::string& url, bool insecure)
    {
        if (edge_ && edge_->isOpen()) { ETCS_LOG("Peer", "already linked to " << url_ << " -- Close first."); return false; }
        if (ETCS::RID other = holder(url); other && other != getRID())
        {
            ETCS_LOG("Peer", url << " is already linked by Peer RID:" << other << " -- bind through it.");
            return false;
        }
        std::string err;
        const int fd = etcs_ws::link_dial(url, insecure, err);
        if (fd < 0) { ETCS_LOG("Peer", "Connect " << url << ": " << err); return false; }
        std::string host;
        if (!etcs_link::hello_dial(fd, name_, host))
        {
            ETCS_LOG("Peer", "Connect " << url << ": no room answered there.");
            ::close(fd);
            return false;
        }
        edge_ = std::make_shared<etcs_link::Edge>(fd, getArena(), true, host, &exports_);
        edge_->start();
        url_ = url;
        claim(url, getRID());
        ETCS_LOG("Peer", "linked to '" << host << "' at " << url << " (RID:" << getRID() << ")");
        return true;
    }

    void Close()
    {
        if (edge_) edge_->stop();
        edge_.reset();
        if (!url_.empty()) release(url_, getRID());
    }

    bool Publish(const std::string& name, ETCS::RID rid) { return exports_.Publish(name, rid); }
    bool Unpublish(const std::string& name)             { return exports_.Unpublish(name); }

    void Info()
    {
        ETCS_LOG("Peer", "RID:" << getRID() << " '" << name_ << "' "
                 << (edge_ && edge_->isOpen() ? "linked to '" + edge_->farName() + "' at " + url_
                                                + " channels:" + std::to_string(edge_->channels())
                                              : std::string("not linked")));
    }

    // The link, for a Remote bound through this Peer (Remote.h).
    std::shared_ptr<etcs_link::Edge> edge() const { return edge_; }

    bool DeleteConcrete() override
    {
        Close();
        std::string key = getSourceModule().toString() + ":" + getSourceTag().toString();
        return ETCS::DestroyEvent{ key.c_str(), this }();
    }

private:
    // Which Peer holds which address -- what keeps it one edge per pair.
    static std::mutex& reg_mu() { static std::mutex m; return m; }
    static std::map<std::string, ETCS::RID>& reg() { static std::map<std::string, ETCS::RID> m; return m; }
    static ETCS::RID holder(const std::string& url)
    {
        std::lock_guard<std::mutex> lock(reg_mu());
        auto it = reg().find(url);
        if (it == reg().end()) return 0;
        if (!ETCS::etcs_resolve_rid_anywhere(&ETCS::getLoader(), it->second)) { reg().erase(it); return 0; }
        return it->second;
    }
    static void claim(const std::string& url, ETCS::RID rid) { std::lock_guard<std::mutex> lock(reg_mu()); reg()[url] = rid; }
    static void release(const std::string& url, ETCS::RID rid)
    {
        std::lock_guard<std::mutex> lock(reg_mu());
        auto it = reg().find(url);
        if (it != reg().end() && it->second == rid) reg().erase(it);
    }

    std::shared_ptr<etcs_link::Edge> edge_;
    etcs_link::Exports               exports_;
    std::string                      url_;
    std::string                      name_ = "guest";
};

#endif // NETWORKPROVIDER_PEER_H__
