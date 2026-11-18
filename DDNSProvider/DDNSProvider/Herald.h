#ifndef DDNSPROVIDER_HERALD_H__
#define DDNSPROVIDER_HERALD_H__

#include "../../../core_defs.h"
#include "../../../ontology.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/*
 * Herald -- announces our address abroad: keeps a far Directory's adverts true.
 *
 * Directory's own rule is that an entry lives only while its advertiser is
 * reachable, and inside ETCS the link edge keeps that rule: when the edge
 * closes, the advert goes. A public name server has no edge. When the address
 * moves, the old entry stays and names a door that no longer reaches us. The
 * Herald is the repair: on a clock (Switchable), it asks witnesses what
 * address we appear as from outside, and when enough of them agree on one
 * address it re-advertises every listed name of its kind that names a
 * different one.
 *
 * NOTHING HERE TOUCHES THE NETWORK. The directory and every witness are RIDs
 * the script hands in, resolved per call through the loader and invoked by
 * qualified action name (ForumNode's database edge, for the same reason): the
 * calls go through work functions, so the trace sees them, and this module
 * links against no other module. The directory must claim Directory and
 * export Advertise ("name kind info" -> bool) and List ("kind" -> "name kind
 * info" per line), which is Lobby's surface as it stands. A witness is any
 * entity with an action answering an address with no arguments.
 *
 * QUORUM is the fraction of ALL witnesses (silent ones included) that must
 * name the same address. 1 (the default): any witness that disagrees or stays
 * silent holds the sweep. 1/2: the sweep holds only if more than half fail.
 * Kept as a ratio of integers; a plurality tie at the top always holds,
 * because two addresses with equal standing are an argument, not an answer.
 *
 * HOLDOFF: after an address is advertised, the same address is not sent again
 * until the holdoff passes, so resolver caches and propagation that still show
 * the old entry don't turn every sweep into another update. A different
 * address is sent at once.
 *
 * Configuration is refused while started: the sweep thread reads it without a
 * lock, and a change mid-sweep would mix two configurations in one decision.
 * Start and Stop come from the script's thread, one at a time.
 */
class Herald :
    public SwitchableBase<Herald>, public DeletableBase<Herald>
{
public:
    WIRE_TYPE_IDENTITY(Herald);

    Herald() = default;
    virtual ~Herald() { StopConcrete(); }

    // --- Switchable_ ---------------------------------------------------------

    bool StartConcrete() override
    {
        if (running_) { ETCS_LOG("Herald", "Start: already started."); return false; }
        if (directory_ == 0) { ETCS_LOG("Herald", "Start: no directory tracked."); return false; }
        if (witnesses_.empty()) { ETCS_LOG("Herald", "Start: no witnesses."); return false; }
        {
            std::lock_guard<std::mutex> lock(mu_);
            stopping_ = false;
        }
        running_ = true;
        thread_  = std::thread([this] { loop(); });
        ETCS_LOG("Herald", "Start: every " << interval_s_ << " s, quorum " << num_ << "/" << den_
                 << " of " << witnesses_.size() << " witness(es), kind '" << kind_
                 << "' (RID:" << getRID() << ").");
        return true;
    }

    bool StopConcrete() override
    {
        if (!running_) return true;
        {
            std::lock_guard<std::mutex> lock(mu_);
            stopping_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) thread_.join();
        running_ = false;
        ETCS_LOG("Herald", "Stop: stopped (RID:" << getRID() << ").");
        return true;
    }

    bool IsStartedConcrete() const override { return running_; }

    // --- Deletable_ ----------------------------------------------------------

    bool DeleteConcrete() override
    {
        StopConcrete();
        std::string conjugate_key = getSourceModule().toString() + ":" + getSourceTag().toString();
        ETCS_LOG("Herald", "Delete: firing self-DestroyEvent for RID:" << getRID()
                 << " (" << conjugate_key << ")");
        return ETCS::DestroyEvent{conjugate_key.c_str(), this}();
    }

    // --- Configuration (refused while started) -------------------------------

    bool Track(ETCS::RID rid)
    {
        if (refuseStarted("Track")) return false;
        ETCS::Entity* e = resolve(rid);
        if (!e) { ETCS_LOG("Herald", "Track: RID:" << rid << " does not resolve."); return false; }
        if (!e->getInterfacePointer(ETCS::Buffer("Directory")))
        {
            ETCS_LOG("Herald", "Track: RID:" << rid << " (" << e->getSourceTag().toString()
                     << ") is not a Directory.");
            return false;
        }
        directory_ = rid;
        ETCS_LOG("Herald", "Track: directory RID:" << rid << " (" << e->getSourceTag().toString() << ").");
        return true;
    }

    bool Witness(ETCS::RID rid, const std::string& action)
    {
        if (refuseStarted("Witness")) return false;
        if (rid == 0 || action.empty())
        {
            ETCS_LOG("Herald", "Witness: expected '<rid> <Action>'.");
            return false;
        }
        witnesses_.push_back(Seer{ rid, action });
        ETCS_LOG("Herald", "Witness: RID:" << rid << " ." << action << " (" << witnesses_.size()
                 << " witness(es)).");
        return true;
    }

    // "1", "0.5", "2/3": a ratio in (0, 1], parsed to integers.
    bool SetQuorum(const std::string& text)
    {
        if (refuseStarted("SetQuorum")) return false;
        uint64_t n = 0, d = 0;
        if (!parseRatio(text, n, d) || n == 0 || n > d)
        {
            ETCS_LOG("Herald", "SetQuorum: '" << text << "' is not a fraction in (0, 1].");
            return false;
        }
        num_ = n; den_ = d;
        ETCS_LOG("Herald", "SetQuorum: " << num_ << "/" << den_ << ".");
        return true;
    }

    bool SetKind(const std::string& kind)
    {
        if (refuseStarted("SetKind")) return false;
        if (kind.empty() || kind.find_first_of(" \n") != std::string::npos) return false;
        kind_ = kind;
        return true;
    }

    bool SetInterval(int seconds)
    {
        if (refuseStarted("SetInterval")) return false;
        if (seconds < kMinIntervalS)
        {
            ETCS_LOG("Herald", "SetInterval: " << seconds << " s is under the " << kMinIntervalS
                     << " s floor.");
            return false;
        }
        interval_s_ = seconds;
        return true;
    }

    bool SetHoldoff(int seconds)
    {
        if (refuseStarted("SetHoldoff")) return false;
        if (seconds < 0) return false;
        holdoff_s_ = seconds;
        return true;
    }

    void SetRunContext(const ETCS::SignalContext& ctx) { run_ctx_ = ctx; }

    // One sweep, now, on the caller's thread. Refused while the clock runs.
    bool SweepNow()
    {
        if (running_) { ETCS_LOG("Herald", "Sweep: refused while started."); return false; }
        return sweep();
    }

    std::string Info() const
    {
        return std::string(running_ ? "started" : "stopped")
             + " quorum=" + std::to_string(num_) + "/" + std::to_string(den_)
             + " witnesses=" + std::to_string(witnesses_.size())
             + " kind=" + kind_
             + " last=" + (last_sent_.empty() ? std::string("-") : last_sent_);
    }

private:
    struct Seer { ETCS::RID rid; std::string action; };

    static constexpr int kMinIntervalS = 30;

    // --- the clock -----------------------------------------------------------

    void loop()
    {
        while (true)
        {
            if (run_ctx_.isInterrupted() || run_ctx_.isTerminated()) break;
            sweep();
            std::unique_lock<std::mutex> lock(mu_);
            if (cv_.wait_for(lock, std::chrono::seconds(interval_s_), [this] { return stopping_; })) break;
        }
    }

    // --- one sweep -----------------------------------------------------------

    // True when the directory was checked (whether or not anything was sent).
    bool sweep()
    {
        // The votes. A silent witness is a vote for nothing and still counts
        // in the total, so silence weighs against the quorum like dissent.
        std::map<std::string, size_t> votes;
        for (const Seer& s : witnesses_)
        {
            std::string addr;
            if (ask(s.rid, s.action, "", addr) && !addr.empty() && addr.find(' ') == std::string::npos)
                ++votes[addr];
            else
                ETCS_LOG("Herald", "witness RID:" << s.rid << " ." << s.action << " gave no address.");
        }

        std::string top;
        size_t top_n = 0;
        bool tied = false;
        for (const auto& [addr, n] : votes)
        {
            if (n > top_n) { top = addr; top_n = n; tied = false; }
            else if (n == top_n) tied = true;
        }

        const uint64_t total = witnesses_.size();
        if (top_n == 0) { ETCS_LOG("Herald", "hold: no witness answered."); return false; }
        if (tied)
        {
            ETCS_LOG("Herald", "hold: witnesses split evenly at " << top_n << " vote(s) each.");
            return false;
        }
        if (static_cast<uint64_t>(top_n) * den_ < num_ * total)
        {
            ETCS_LOG("Herald", "hold: " << top_n << "/" << total << " named " << top
                     << ", under the quorum " << num_ << "/" << den_ << ".");
            return false;
        }

        std::string listing;
        if (!ask(directory_, "List", kind_, listing))
        {
            ETCS_LOG("Herald", "hold: directory RID:" << directory_ << " gave no listing.");
            return false;
        }

        const bool recent = (top == last_sent_)
            && std::chrono::steady_clock::now() - last_sent_at_ < std::chrono::seconds(holdoff_s_);

        size_t stale = 0, sent = 0;
        for (const Entry& e : parseListing(listing))
        {
            if (e.kind != kind_ || e.info == top) continue;
            ++stale;
            if (recent) continue;
            std::string ok;
            if (ask(directory_, "Advertise", e.name + " " + kind_ + " " + top, ok) && ok == "1")
            {
                ++sent;
                ETCS_LOG("Herald", "advertised '" << e.name << "' " << kind_ << " " << top
                         << " (was " << e.info << ").");
            }
            else
                ETCS_LOG("Herald", "advertise '" << e.name << "' " << kind_ << " " << top << " refused.");
        }

        if (sent)
        {
            last_sent_    = top;
            last_sent_at_ = std::chrono::steady_clock::now();
        }
        else if (stale && recent)
            ETCS_LOG("Herald", stale << " entr" << (stale == 1 ? "y" : "ies")
                     << " still show the old address; within the holdoff for " << top << ".");
        return true;
    }

    // --- the edges -----------------------------------------------------------

    static ETCS::Entity* resolve(ETCS::RID rid)
    {
        return rid ? ETCS::etcs_resolve_rid_anywhere(&ETCS::getLoader(), rid) : nullptr;
    }

    // "<SourceTag>.<action>" on the RID; the answer is what the action left in
    // the buffer, trimmed.
    bool ask(ETCS::RID rid, const std::string& action, const std::string& args, std::string& out)
    {
        out.clear();
        ETCS::Entity* target = resolve(rid);
        if (!target) { ETCS_LOG("Herald", "RID:" << rid << " no longer resolves."); return false; }
        const ETCS::Buffer tag = target->getSourceTag();
        ETCS::Buffer qualified;
        qualified.write(tag.c_str());
        qualified.write(".");
        qualified.write(action.c_str());
        ETCS::Buffer data;
        if (!args.empty()) data.writeString(args.c_str());
        try { target->call(qualified, data, run_ctx_); }
        catch (const std::exception& ex)
        {
            ETCS_LOG("Herald", "RID:" << rid << " ." << action << " threw: " << ex.what());
            return false;
        }
        out = data.toString();
        while (!out.empty() && (out.back() == '\0' || out.back() == '\n' || out.back() == ' ')) out.pop_back();
        while (!out.empty() && out.front() == ' ') out.erase(0, 1);
        return true;
    }

    struct Entry { std::string name, kind, info; };

    // "name kind info" per line. A listing cut at the buffer's edge ends in
    // "..." (Lobby's List): that last line is dropped, never acted on, since a
    // cut name would be advertised as a name that was never listed.
    static std::vector<Entry> parseListing(const std::string& text)
    {
        std::vector<Entry> out;
        size_t at = 0;
        while (at < text.size())
        {
            size_t end = text.find('\n', at);
            const bool last = (end == std::string::npos);
            if (last) end = text.size();
            std::string line = text.substr(at, end - at);
            at = end + 1;
            if (last && line.size() >= 3 && line.compare(line.size() - 3, 3, "...") == 0)
            {
                ETCS_LOG("Herald", "listing was cut at the buffer edge; last line dropped.");
                break;
            }
            const size_t a = line.find(' ');
            if (a == std::string::npos) continue;
            const size_t b = line.find(' ', a + 1);
            if (b == std::string::npos) continue;
            out.push_back(Entry{ line.substr(0, a), line.substr(a + 1, b - a - 1), line.substr(b + 1) });
        }
        return out;
    }

    // "n", "n.ddd" or "n/d", digits only.
    static bool parseRatio(const std::string& t, uint64_t& n, uint64_t& d)
    {
        constexpr uint64_t kCap = 1000000000ull;
        auto digits = [](const std::string& s, uint64_t& v) {
            if (s.empty() || s.size() > 9) return false;
            v = 0;
            for (char c : s) { if (c < '0' || c > '9') return false; v = v * 10 + static_cast<uint64_t>(c - '0'); }
            return true;
        };
        const size_t slash = t.find('/');
        if (slash != std::string::npos)
            return digits(t.substr(0, slash), n) && digits(t.substr(slash + 1), d) && d != 0;
        const size_t dot = t.find('.');
        if (dot == std::string::npos) { d = 1; return digits(t, n); }
        uint64_t whole = 0, frac = 0;
        const std::string w = t.substr(0, dot), f = t.substr(dot + 1);
        if (!(w.empty() || digits(w, whole)) || !digits(f, frac)) return false;
        d = 1;
        for (size_t i = 0; i < f.size(); ++i) d *= 10;
        if (whole > kCap / d) return false;
        n = whole * d + frac;
        return true;
    }

    bool refuseStarted(const char* what) const
    {
        if (!running_) return false;
        ETCS_LOG("Herald", what << ": refused while started.");
        return true;
    }

    ETCS::RID         directory_  = 0;
    std::vector<Seer> witnesses_;
    uint64_t          num_        = 1;
    uint64_t          den_        = 1;
    std::string       kind_       = "A";
    int               interval_s_ = 300;
    int               holdoff_s_  = 900;

    std::string                           last_sent_;
    std::chrono::steady_clock::time_point last_sent_at_{};

    ETCS::SignalContext     run_ctx_;
    std::thread             thread_;
    std::mutex              mu_;
    std::condition_variable cv_;
    bool                    stopping_ = false;
    std::atomic<bool>       running_{false};
};

#endif // DDNSPROVIDER_HERALD_H__
