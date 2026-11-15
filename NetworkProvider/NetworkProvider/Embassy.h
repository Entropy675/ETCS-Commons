#ifndef NETWORKPROVIDER_EMBASSY_H__
#define NETWORKPROVIDER_EMBASSY_H__
#include "../../../ontology.h"

#include <atomic>
#include <map>
#include <mutex>
#include <string>

/*
 * Embassy -- a Directory of live Foreign relations, behind a Gate.
 *
 * Every outward conversation goes through one place, whoever it is with. An
 * entry is one RELATION (ontology/Foreign.h: one per stranger), by name and
 * RID, under the protocol it speaks: Advertise <name> <scheme> <rid>. Only an
 * ACTIVE relation is accepted (its stand-in bound, its configuration locked),
 * so the listing is the set of strangers this runtime is talking to, each as
 * configured when it went live. The Embassy knows no instance: it answers
 * "which relation is called that?" and routes.
 *
 * TWO WAYS THROUGH. A caller in another module asks List, finds its relation
 * by name, resolves the RID, and calls Foreign_::Exchange itself: answers of
 * any size, no link to this module. A script uses Request <name> <question>,
 * answered inline, cut to one Buffer.
 *
 * THE GATE: closed by default. A closed embassy lists nobody and refuses every
 * Request, so nothing new leaves. Open() is the one line that lets traffic out;
 * Close() is the one line that stops it.
 *
 * Directory's liveness rule holds by resolution: an entry whose RID no longer
 * resolves is never listed or routed to.
 */
class Embassy :
    public DirectoryBase<Embassy>, public GateBase<Embassy>, public DeletableBase<Embassy>
{
public:
    WIRE_TYPE_IDENTITY(Embassy);

    Embassy() = default;

    // --- Directory_ ------------------------------------------------------------

    // info: the relation's RID. It must claim Foreign, speak `kind`, and be
    // active (a bound stand-in).
    bool AdvertiseConcrete(const std::string& name, const std::string& kind, const std::string& info) override
    {
        if (name.empty() || name.find_first_of(" \n") != std::string::npos) return false;
        ETCS::RID rid = 0;
        try { rid = static_cast<ETCS::RID>(std::stoull(info)); } catch (...) { rid = 0; }
        Foreign_* f = speakerAt(rid);
        if (!f) { ETCS_LOG("Embassy", "Advertise '" << name << "': RID " << info << " is not a Foreign relation."); return false; }
        if (f->StandIn() == 0) { ETCS_LOG("Embassy", "Advertise '" << name << "': the relation isn't active (no stand-in)."); return false; }
        const std::string speaks = f->Speaks();
        if (speaks != kind)
        {
            ETCS_LOG("Embassy", "Advertise '" << name << "': it speaks '" << speaks << "', not '" << kind << "'.");
            return false;
        }
        std::lock_guard<std::mutex> lock(mu_);
        entries_[name] = Entry{ kind, rid };
        ETCS_LOG("Embassy", "'" << name << "' speaks " << kind << " (RID:" << rid << ").");
        return true;
    }

    bool WithdrawConcrete(const std::string& name) override
    {
        std::lock_guard<std::mutex> lock(mu_);
        return entries_.erase(name) != 0;
    }

    // "name scheme rid" per line; an empty kind lists every scheme. Nothing
    // while closed.
    std::string ListingConcrete(const std::string& kind) const override
    {
        if (!open_) return {};
        std::string out;
        std::lock_guard<std::mutex> lock(mu_);
        for (const auto& [name, e] : entries_)
            if ((kind.empty() || e.scheme == kind) && speakerAt(e.rid))
                out += name + " " + e.scheme + " " + std::to_string(e.rid) + "\n";
        return out;
    }

    // --- Gate_ -----------------------------------------------------------------

    bool OpenConcrete(const ETCS::Buffer& config) override
    {
        (void)config;
        open_ = true;
        ETCS_LOG("Embassy", "open (RID:" << getRID() << ").");
        return true;
    }
    bool CloseConcrete() override
    {
        open_ = false;
        ETCS_LOG("Embassy", "closed (RID:" << getRID() << ").");
        return true;
    }
    bool IsOpenConcrete() const override { return open_; }

    // --- Deletable_ ------------------------------------------------------------

    bool DeleteConcrete() override
    {
        open_ = false;
        std::string conjugate_key = getSourceModule().toString() + ":" + getSourceTag().toString();
        return ETCS::DestroyEvent{conjugate_key.c_str(), this}();
    }

    // --- routing ---------------------------------------------------------------

    // To the relation called `name`. False when closed, unknown, or when the
    // relation gives no answer.
    bool Request(const std::string& name, const std::string& question, std::string& answer, ETCS::SignalContext ctx)
    {
        answer.clear();
        if (!open_) { ETCS_LOG("Embassy", "Request refused: closed."); return false; }
        ETCS::RID rid = 0;
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = entries_.find(name);
            if (it != entries_.end()) rid = it->second.rid;
        }
        Foreign_* f = speakerAt(rid);
        if (!f) { ETCS_LOG("Embassy", "Request: no live relation called '" << name << "'."); return false; }
        return f->Exchange(question, answer, ctx);
    }

private:
    struct Entry { std::string scheme; ETCS::RID rid = 0; };

    static Foreign_* speakerAt(ETCS::RID rid)
    {
        if (rid == 0) return nullptr;
        ETCS::Entity* e = ETCS::etcs_resolve_rid_anywhere(&ETCS::getLoader(), rid);
        return e ? static_cast<Foreign_*>(e->getInterfacePointer(ETCS::Buffer("Foreign"))) : nullptr;
    }

    mutable std::mutex           mu_;
    std::map<std::string, Entry> entries_;
    std::atomic<bool>            open_{false};
};

#endif // NETWORKPROVIDER_EMBASSY_H__
