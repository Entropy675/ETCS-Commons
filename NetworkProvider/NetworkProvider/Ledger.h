#ifndef NETWORKPROVIDER_LEDGER_H__
#define NETWORKPROVIDER_LEDGER_H__
#include "../../../ontology.h"
#include "Edge.h"

#include <condition_variable>
#include <cstdio>
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
 * A line is one frame: at most kLine bytes of text, no newline.
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

    static constexpr size_t kLine   = 200;
    static constexpr size_t kAuthor = 32;

    Ledger()  = default;
    ~Ledger() { close(); }

    uint64_t AppendConcrete(const std::string& author, const std::string& line) override
    {
        const std::string& remote = etcs_link::caller().name;
        std::string who = !remote.empty() ? remote : (author.empty() ? "local" : author);
        if (who.size() > kAuthor) who.resize(kAuthor);
        for (char& c : who) if (c == ' ' || c == '\n') c = '_';
        if (line.empty() || line.size() > kLine || line.find('\n') != std::string::npos) return UINT64_MAX;
        std::lock_guard<std::mutex> lock(mu_);
        const uint64_t seq = lines_.size();
        push(std::to_string(seq) + " " + who + " " + line);
        return seq;
    }
    std::string HeadConcrete() const override
    {
        std::lock_guard<std::mutex> lock(mu_);
        return std::to_string(lines_.size()) + " " + hex(chain());
    }
    std::string SinceConcrete(uint64_t seq, size_t budget) const override
    {
        std::lock_guard<std::mutex> lock(mu_);
        uint64_t i = seq > lines_.size() ? lines_.size() : seq;
        std::string body;
        while (i < lines_.size() && body.size() + lines_[i].size() + 1 <= budget) body += lines_[i++] + "\n";
        return std::to_string(seq) + " " + std::to_string(i) + " " + hex(i ? chains_[i - 1] : 0) + "\n" + body;
    }

    // One line for Follow: blocks until line `seq` exists, the ledger
    // closes, or `ctx` is raised. False on the last two.
    bool waitLine(uint64_t seq, std::string& out, const ETCS::SignalContext& ctx)
    {
        std::unique_lock<std::mutex> lock(mu_);
        while (seq >= lines_.size())
        {
            if (closed_ || ctx.isInterrupted() || ctx.isTerminated()) return false;
            cv_.wait_for(lock, std::chrono::milliseconds(100));
        }
        out = lines_[seq];
        return true;
    }
    // Mirror's append: a line as its origin numbered it. Out of order or
    // malformed is refused -- a copy with a hole is not a copy.
    bool appendExact(const std::string& full)
    {
        std::lock_guard<std::mutex> lock(mu_);
        const size_t sp = full.find(' ');
        if (sp == std::string::npos || full.substr(0, sp) != std::to_string(lines_.size())) return false;
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
    void CaptureStateConcrete(ETCS::EnvironmentState& out) const
    {
        std::lock_guard<std::mutex> lock(mu_);
        std::string all;
        for (auto& l : lines_) all += l + "\n";
        out.set("lines", std::move(all));
    }
    bool RebuildLocalConcrete(const ETCS::EnvironmentState& st) override { return adopt(st); }
    bool ReflectRemoteConcrete(const ETCS::EnvironmentState& st) override { return adopt(st); }

    static std::string hex(uint64_t v) { char b[17]; std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v)); return b; }

private:
    uint64_t chain() const { return chains_.empty() ? 0 : chains_.back(); }
    void push(std::string full)
    {
        chains_.push_back(XXH3_64bits_withSeed(full.data(), full.size(), chain()));
        lines_.push_back(std::move(full));
        cv_.notify_all();
    }
    void close() { std::lock_guard<std::mutex> lock(mu_); closed_ = true; cv_.notify_all(); }
    // Lines as captured, re-chained here: the chain is derived, never taken.
    bool adopt(const ETCS::EnvironmentState& st)
    {
        const std::string* all = st.get("lines");
        if (!all) return true;
        std::lock_guard<std::mutex> lock(mu_);
        lines_.clear(); chains_.clear();
        size_t at = 0;
        while (at < all->size())
        {
            const size_t nl = all->find('\n', at);
            if (nl == std::string::npos) break;
            push(all->substr(at, nl - at));
            at = nl + 1;
        }
        return true;
    }

    mutable std::mutex       mu_;
    std::condition_variable  cv_;
    std::vector<std::string> lines_;
    std::vector<uint64_t>    chains_;
    bool                     closed_ = false;
};

#endif // NETWORKPROVIDER_LEDGER_H__
