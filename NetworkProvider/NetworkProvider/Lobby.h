#ifndef NETWORKPROVIDER_LOBBY_H__
#define NETWORKPROVIDER_LOBBY_H__
#include "../../../ontology.h"
#include "Edge.h"

#include <condition_variable>
#include <functional>
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
 *
 * WATCHED, NOT POLLED. `lobby.Watch() -> x.Consume()` sends the whole
 * listing as one message whenever it changes (and once to begin with): a
 * roster, or where everyone is looking, is a listing that changes on every
 * arrival and departure, and the departures are the ones a poll gets late.
 * `far.Watch() -> here.Mirror()` keeps a local Lobby as a copy of a far one.
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
        const bool fresh = it == entries_.end();
        entries_[name] = Entry{ kind.empty() ? "-" : kind, info, edge, etcs_link::caller().name };
        if (fresh)
            ETCS_LOG("Lobby", "'" << name << "' (" << kind << ") advertised"
                     << (edge ? " by '" + etcs_link::caller().name + "'" : std::string(" here")));
        changed();
        return true;
    }
    bool WithdrawConcrete(const std::string& name) override
    {
        const uint64_t edge = etcs_link::caller().edge;
        std::lock_guard<std::mutex> lock(mu_);
        auto it = entries_.find(name);
        if (it == entries_.end() || it->second.edge != edge) return false;
        entries_.erase(it);
        changed();
        return true;
    }
    // Mirror's take: this lobby becomes the listing, entries local to here.
    void adopt(const std::string& listing)
    {
        std::lock_guard<std::mutex> lock(mu_);
        entries_.clear();
        size_t at = 0;
        while (at < listing.size())
        {
            const size_t nl = listing.find('\n', at);
            const std::string line = listing.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
            at = nl == std::string::npos ? listing.size() : nl + 1;
            const size_t a = line.find(' ');
            const size_t b = a == std::string::npos ? a : line.find(' ', a + 1);
            if (a == std::string::npos) continue;
            entries_[line.substr(0, a)] = Entry{ b == std::string::npos ? line.substr(a + 1) : line.substr(a + 1, b - a - 1),
                                                 b == std::string::npos ? "" : line.substr(b + 1), 0, "" };
        }
        changed();
    }
    // The listing once it is newer than `seen` (its version on return), or
    // false when the lobby closes or `ctx` is raised.
    bool waitListing(uint64_t& seen, std::string& out, const ETCS::SignalContext& ctx,
                     const std::function<bool()>& gone = {})
    {
        std::unique_lock<std::mutex> lock(mu_);
        while (version_ == seen)
        {
            if (closed_ || ctx.isInterrupted() || ctx.isTerminated() || (gone && gone())) return false;
            cv_.wait_for(lock, std::chrono::milliseconds(100));
        }
        seen = version_;
        out.clear();
        for (auto& [n, e] : entries_) out += n + " " + e.kind + " " + e.info + "\n";
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
        { std::lock_guard<std::mutex> lock(mu_); closed_ = true; cv_.notify_all(); }
        std::string key = getSourceModule().toString() + ":" + getSourceTag().toString();
        return ETCS::DestroyEvent{ key.c_str(), this }();
    }

private:
    struct Entry { std::string kind, info; uint64_t edge = 0; std::string by; };
    void changed() { ++version_; cv_.notify_all(); }     // under mu_
    void dropEdge(uint64_t edge)
    {
        std::lock_guard<std::mutex> lock(mu_);
        bool any = false;
        for (auto it = entries_.begin(); it != entries_.end(); )
        {
            if (it->second.edge != edge) { ++it; continue; }
            ETCS_LOG("Lobby", "'" << it->first << "' withdrawn -- its link closed.");
            it = entries_.erase(it);
            any = true;
        }
        if (any) changed();
    }
    mutable std::mutex           mu_;
    std::condition_variable      cv_;
    std::map<std::string, Entry> entries_;
    uint64_t                     version_ = 1;   // a watcher starting at 0 gets the listing at once
    bool                         closed_  = false;
    int                          watch_ = 0;
};

#endif // NETWORKPROVIDER_LOBBY_H__
