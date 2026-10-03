#ifndef NETWORKPROVIDER_LEDGER_H__
#define NETWORKPROVIDER_LEDGER_H__
#include "../../../ontology.h"
#include "Edge.h"

#include <condition_variable>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

/*
 * Ledger -- a Record (ontology/Record.h): numbered lines, chained by hash,
 * in the order this runtime put them.
 *
 * A guest appends through its surface (`book.Append(...)` runs HERE), so
 * every line from every guest is ordered by this one runtime, and its author
 * is the link it came over (etcs_link::caller), not a name the guest typed.
 *
 * FOLLOWING IS A STREAM. `book.Follow(<seq>) -> copy.Mirror()` sends the
 * lines from <seq> and then each new one as it is appended, for as long as
 * the consumer stays; Mirror appends them to another Ledger exactly as they
 * were -- same numbers, same authors -- so the copy's Head is the original's
 * when they agree, which is the whole check. Over a link that stream is a
 * channel on the edge like any other.
 *
 * A line is one MESSAGE (MirrorBuffer::writeMessage): up to kLine bytes of
 * text, no newline. Append as a verb carries what fits a Buffer; a bigger
 * line -- a page, a photograph -- is streamed in: `x.Produce -> book.Take()`,
 * one line per message, authored like an Append.
 *
 * A DOOR ONTO ANOTHER (Feed): a ledger fed into another holds nothing of its
 * own -- what is appended to it is appended THERE, authored the same. It
 * exists to carry an authority layer the record itself does not: the record
 * is published to be read, and a ledger behind a Seal, fed into it, is the
 * way in for those who hold the key (Room.h).
 *
 * FROM THE LAST CHECKPOINT ON (ontology/Record.h). Checkpoint(seq) drops the
 * lines before `seq`: `base_` is how many went, `base_chain_` the chain there.
 * A follower or a Since from before it is told "~ <base> <chain>" first and
 * then gets the lines from there, and Mirror takes that as a new start.
 *
 * ENVIRONMENTAL (ontology/Environmental.h). Its lines are not on its tag
 * surface -- who appended what came from outside the script -- so they are
 * the state it captures: kept by a Persistence child and put back after a
 * replay (RebuildLocal), and handed to a surface of it when one binds
 * (ReflectRemote), so a guest's surface starts out knowing the host's lines.
 */
class Ledger : public RecordBase<Ledger>, public EnvironmentalBase<Ledger>, public DeletableBase<Ledger>
{
public:
    WIRE_TYPE_IDENTITY(Ledger);

    static constexpr size_t kLine   = 8u << 20;
    static constexpr size_t kAuthor = 32;

    // The lines are the value behind "ledger" on the tag surface
    // (Entity::bindValue): what a store keeps and a reflection starts from,
    // read off the surface and handed back to it -- no capture of the
    // type's own. One value, so the base and the lines arrive together.
    Ledger()
    {
        bindValue(ETCS::Buffer("ledger"), ETCS::Entity::ValueBinding{
            this,
            [](void* self, std::string& out) { static_cast<Ledger*>(self)->writeState(out); },
            [](void* self, const std::string& in) { return static_cast<Ledger*>(self)->readState(in); } });
    }
    ~Ledger() { close(); }

    // What a line appended here, not over a link, is authored as.
    void Author(const std::string& name) { author_ = name; }
    // This ledger is a door onto `rid`'s (0: its own again).
    bool Feed(ETCS::RID rid)
    {
        ETCS::Entity* e = rid ? ETCS::etcs_resolve_rid_anywhere(&ETCS::getLoader(), rid) : nullptr;
        if (rid && !(e && e->getInterfacePointer(ETCS::Buffer("Record"))))
        { ETCS_LOG("Ledger", "Feed: RID:" << rid << " is not a Record."); return false; }
        feeds_ = rid;
        return true;
    }

    uint64_t AppendConcrete(const std::string& author, const std::string& line) override
    {
        if (feeds_)
        {
            ETCS::Entity* e = ETCS::etcs_resolve_rid_anywhere(&ETCS::getLoader(), feeds_);
            void* rec = e ? e->getInterfacePointer(ETCS::Buffer("Record")) : nullptr;
            if (!rec) { ETCS_LOG("Ledger", "Feed: the record this feeds is gone."); return UINT64_MAX; }
            return static_cast<Record_*>(rec)->Append(author, line);   // the link's author, on this thread
        }
        const std::string& remote = etcs_link::caller().name;
        std::string who = !remote.empty() ? remote : !author.empty() ? author : !author_.empty() ? author_ : "local";
        if (who.size() > kAuthor) who.resize(kAuthor);
        for (char& c : who) if (c == ' ' || c == '\n') c = '_';
        if (line.empty() || line.size() > kLine || line.find('\n') != std::string::npos) return UINT64_MAX;
        std::lock_guard<std::mutex> lock(mu_);
        const uint64_t seq = next();
        push(std::to_string(seq) + " " + who + " " + line);
        return seq;
    }
    std::string HeadConcrete() const override
    {
        std::lock_guard<std::mutex> lock(mu_);
        return std::to_string(next()) + " " + hex(chain());
    }
    std::string SinceConcrete(uint64_t seq, size_t budget) const override
    {
        std::lock_guard<std::mutex> lock(mu_);
        std::string lead;
        if (seq < base_) { lead = restart(); seq = base_; }
        uint64_t i = seq > next() ? next() : seq;
        std::string body;
        while (i < next() && body.size() + at(i).size() + 1 <= budget) body += at(i++) + "\n";
        return lead + std::to_string(seq) + " " + std::to_string(i) + " " + hex(chainAt(i)) + "\n" + body;
    }
    // The ordering authority's alone: a line arriving over a link is a
    // guest's, and a guest does not shorten what everyone else reads.
    bool CheckpointConcrete(uint64_t seq) override
    {
        if (!etcs_link::caller().name.empty()) return false;
        std::lock_guard<std::mutex> lock(mu_);
        if (seq < base_ || seq > next()) return false;
        if (seq == base_) return true;
        const size_t drop = static_cast<size_t>(seq - base_);
        base_chain_ = chains_[drop - 1];
        lines_.erase(lines_.begin(), lines_.begin() + static_cast<std::ptrdiff_t>(drop));
        chains_.erase(chains_.begin(), chains_.begin() + static_cast<std::ptrdiff_t>(drop));
        base_ = seq;
        cv_.notify_all();
        return true;
    }

    /*
     * One message for Follow: blocks until line `seq` exists, the ledger
     * closes, or `ctx` is raised (false on the last two). A follower that
     * has fallen behind a checkpoint -- asked from before it, or passed by
     * one while it was sending -- gets the restart first and `seq` moves
     * to the checkpoint.
     */
    bool waitLine(uint64_t& seq, std::string& out, const ETCS::SignalContext& ctx,
                  const std::function<bool()>& gone = {})
    {
        std::unique_lock<std::mutex> lock(mu_);
        while (seq >= next())
        {
            if (closed_ || ctx.isInterrupted() || ctx.isTerminated() || (gone && gone())) return false;
            cv_.wait_for(lock, std::chrono::milliseconds(100));
        }
        if (seq < base_) { out = restart(); seq = base_; return true; }
        out = at(seq++);
        return true;
    }
    // Mirror's append: a line as its origin numbered it. Out of order or
    // malformed is refused -- a copy with a hole is not a copy. A restart
    // ("~ <base> <chain>") empties the copy and starts it there.
    bool appendExact(const std::string& full)
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (full.compare(0, 2, "~ ") == 0)
        {
            unsigned long long b = 0, c = 0;
            if (std::sscanf(full.c_str() + 2, "%llu %llx", &b, &c) != 2) return false;
            lines_.clear(); chains_.clear();
            base_ = b; base_chain_ = c;
            return true;
        }
        const size_t sp = full.find(' ');
        if (sp == std::string::npos || full.substr(0, sp) != std::to_string(next())) return false;
        push(full);
        return true;
    }

    bool DeleteConcrete() override
    {
        close();
        std::string key = getSourceModule().toString() + ":" + getSourceTag().toString();
        return ETCS::DestroyEvent{ key.c_str(), this }();
    }

    // ── Environmental ──────────────────────────────────────────────────────
    // The values are back on the surface already (the "ledger" binding read
    // them); nothing more to do here or as a reflection.
    bool RebuildLocalConcrete(const ETCS::EnvironmentState&) override  { return true; }
    bool ReflectRemoteConcrete(const ETCS::EnvironmentState&) override { return true; }

    static std::string hex(uint64_t v) { char b[17]; std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v)); return b; }

private:
    uint64_t next() const { return base_ + lines_.size(); }
    const std::string& at(uint64_t seq) const { return lines_[static_cast<size_t>(seq - base_)]; }
    uint64_t chainAt(uint64_t seq) const { return seq > base_ ? chains_[static_cast<size_t>(seq - base_ - 1)] : base_chain_; }
    uint64_t chain() const { return chainAt(next()); }
    std::string restart() const { return "~ " + std::to_string(base_) + " " + hex(base_chain_); }
    void push(std::string full)
    {
        chains_.push_back(XXH3_64bits_withSeed(full.data(), full.size(), chain()));
        lines_.push_back(std::move(full));
        cv_.notify_all();
    }
    void close() { std::lock_guard<std::mutex> lock(mu_); closed_ = true; cv_.notify_all(); }
    // The "ledger" value: the restart line when there is a base ("~ <base>
    // <chain>"), then every line, one per line. The same text a restart
    // reads, so one format serves both.
    void writeState(std::string& out) const
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (base_) out += restart() + "\n";
        for (auto& l : lines_) out += l + "\n";
    }
    // Lines as captured, re-chained here: the chain is derived, never taken.
    bool readState(const std::string& all)
    {
        std::lock_guard<std::mutex> lock(mu_);
        lines_.clear(); chains_.clear();
        base_ = 0; base_chain_ = 0;
        size_t from = 0;
        if (all.compare(0, 2, "~ ") == 0)
        {
            const size_t nl = all.find('\n');
            unsigned long long n = 0, c = 0;
            if (std::sscanf(all.c_str() + 2, "%llu %llx", &n, &c) == 2) { base_ = n; base_chain_ = c; }
            from = (nl == std::string::npos) ? all.size() : nl + 1;
        }
        while (from < all.size())
        {
            const size_t nl = all.find('\n', from);
            if (nl == std::string::npos) break;
            push(all.substr(from, nl - from));
            from = nl + 1;
        }
        return true;
    }

    mutable std::mutex       mu_;
    std::condition_variable  cv_;
    std::vector<std::string> lines_;    // lines_[i] is line base_ + i
    std::vector<uint64_t>    chains_;
    uint64_t                 base_       = 0;
    uint64_t                 base_chain_ = 0;
    ETCS::RID                feeds_      = 0;
    std::string              author_;
    bool                     closed_ = false;
};

#endif // NETWORKPROVIDER_LEDGER_H__
