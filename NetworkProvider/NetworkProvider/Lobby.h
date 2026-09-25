#ifndef NETWORKPROVIDER_LOBBY_H__
#define NETWORKPROVIDER_LOBBY_H__
#include "../../../ontology.h"
#include "Edge.h"

#include <map>
#include <mutex>
#include <string>

/*
 * Lobby -- a Directory (ontology/Directory.h): who is reachable, by name.
 *
 * Published in a Room like any node, so hosts reach it the way guests reach
 * anything -- a surface on their side, `lobby.Advertise(...)` running here.
 * An advert remembers the edge it arrived on and goes when that edge closes
 * (etcs_link::EdgeWatch): a host whose tab closed drops out of the listing
 * the moment its link does, with nothing polled. One made locally stays
 * until withdrawn. A name belongs to whoever advertised it while their link
 * lives; nobody else may replace or withdraw it.
 *
 * What an entry says is where to go (`info`, usually a room address); the
 * lobby takes no part in what happens there.
 */
class Lobby : public DirectoryBase<Lobby>, public DeletableBase<Lobby>
{
public:
    WIRE_TYPE_IDENTITY(Lobby);

    Lobby()
    {
        watch_ = etcs_link::EdgeWatch::get().add([this](uint64_t edge) { dropEdge(edge); });
    }
    ~Lobby() { etcs_link::EdgeWatch::get().remove(watch_); }

    bool AdvertiseConcrete(const std::string& name, const std::string& kind, const std::string& info) override
    {
        if (name.empty() || name.find_first_of(" \n") != std::string::npos) return false;
        const uint64_t edge = etcs_link::caller().edge;
        std::lock_guard<std::mutex> lock(mu_);
        auto it = entries_.find(name);
        if (it != entries_.end() && it->second.edge != edge) return false;   // someone else's
        entries_[name] = Entry{ kind.empty() ? "-" : kind, info, edge, etcs_link::caller().name };
        ETCS_LOG("Lobby", "'" << name << "' (" << kind << ") advertised"
                 << (edge ? " by '" + etcs_link::caller().name + "'" : std::string(" here")));
        return true;
    }
    bool WithdrawConcrete(const std::string& name) override
    {
        const uint64_t edge = etcs_link::caller().edge;
        std::lock_guard<std::mutex> lock(mu_);
        auto it = entries_.find(name);
        if (it == entries_.end() || it->second.edge != edge) return false;
        entries_.erase(it);
        return true;
    }
    std::string ListingConcrete(const std::string& kind) const override
    {
        std::lock_guard<std::mutex> lock(mu_);
        std::string out;
        for (auto& [n, e] : entries_)
            if (kind.empty() || e.kind == kind) out += n + " " + e.kind + " " + e.info + "\n";
        return out;
    }

    bool DeleteConcrete() override
    {
        std::string key = getSourceModule().toString() + ":" + getSourceTag().toString();
        return ETCS::DestroyEvent{ key.c_str(), this }();
    }

private:
    struct Entry { std::string kind, info; uint64_t edge = 0; std::string by; };
    void dropEdge(uint64_t edge)
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto it = entries_.begin(); it != entries_.end(); )
        {
            if (it->second.edge != edge) { ++it; continue; }
            ETCS_LOG("Lobby", "'" << it->first << "' withdrawn -- its link closed.");
            it = entries_.erase(it);
        }
    }
    mutable std::mutex           mu_;
    std::map<std::string, Entry> entries_;
    int                          watch_ = 0;
};

#endif // NETWORKPROVIDER_LOBBY_H__
