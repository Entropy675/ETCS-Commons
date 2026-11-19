#ifndef CRYPTPROVIDER_UPSTREAM_H__
#define CRYPTPROVIDER_UPSTREAM_H__

#include "Crypt.h"

#include <mutex>
#include <sstream>

/*
 * Upstream -- the site, as this loader reads it: the stand-in an HttpsRelation
 * (NetworkProvider) to the site lands on, found in an Embassy by name as
 * Custody finds its own. Every question is one Foreign_::Exchange. What touches
 * this loader's key or store is the loader's, asked through TrustEvent on its
 * ordering thread (core/Trust.h, answer) -- nothing here holds either.
 *
 *   Register        ACE's half for this loader's key, from the site's registrar
 *   Check           the site's list against what is here; the report is List's
 *   Update          Check, then fetch what the site has moved past, and put it in place
 *   Paths <p> <d>   where the registrar and the binaries are; /pki and /dist unless said
 *
 * What Register and Update put in place is the next session's: this one keeps
 * what it mapped. Except a RE-SYNC (core/Trust.h): when the list is a sibling
 * marketplace's -- this loader's marketplace key retired -- Check, whenever it
 * sees one, fetches the successor, has the loader put it in place, and asks
 * the loader to restart on it where this session left off.
 */
class Upstream : public DirectoryBase<Upstream>, public DeletableBase<Upstream>
{
public:
    WIRE_TYPE_IDENTITY(Upstream);

    Upstream() = default;

    // --- Directory_: the site is read, not written ------------------------------------
    bool AdvertiseConcrete(const std::string&, const std::string&, const std::string&) override
    {
        ETCS_LOG("Upstream", "Advertise: the site is read, not written.");
        return false;
    }
    bool WithdrawConcrete(const std::string&) override { return false; }
    std::string ListingConcrete(const std::string&) const override { std::lock_guard<std::mutex> l(mu_); return report_; }

    bool DeleteConcrete() override
    {
        std::string key = getSourceModule().toString() + ":" + getSourceTag().toString();
        return ETCS::DestroyEvent{key.c_str(), this}();
    }

    // --- verbs ---------------------------------------------------------------------------
    bool Register()
    {
        std::string registrar, chain, pki;
        { std::lock_guard<std::mutex> l(mu_); pki = pki_; }
        if (!get(pki + "/registrar", registrar)) return false;
        trim(registrar);
        ETCS::TrustEvent ask(ETCS::TrustEvent::Op::Request);
        ask.a = registrar;
        if (!ask()) { ETCS_LOG("Upstream", "Register: " << ask.out); return false; }
        const size_t sp = ask.out.find(' ');
        if (!get(pki + "/register/" + ask.out.substr(0, sp) + "/" + ask.out.substr(sp + 1), chain)) return false;
        ETCS::TrustEvent install(ETCS::TrustEvent::Op::Install);
        install.a = chain;
        const bool ok = install();
        ETCS_LOG("Upstream", "Register: " << install.out);
        return ok;
    }

    bool Check()
    {
        std::string list, dist;
        { std::lock_guard<std::mutex> l(mu_); dist = dist_; }
        if (!get(dist + "/manifest", list)) return false;
        ETCS::TrustEvent survey(ETCS::TrustEvent::Op::Survey);
        survey.a = list;
        const bool ok = survey();
        std::istringstream lines(survey.out);
        for (std::string line; std::getline(lines, line); ) ETCS_LOG("Upstream", "Check: " << line);
        {
            std::lock_guard<std::mutex> l(mu_);
            list_   = ok ? list : std::string();
            report_ = survey.out;
        }
        if (ok) resync(list, survey.out, dist);
        return ok;
    }

    bool Update()
    {
        if (!Check()) return false;
        std::string list, report, dist;
        { std::lock_guard<std::mutex> l(mu_); list = list_; report = report_; dist = dist_; }
        int failed = 0, placed = 0;
        std::istringstream lines(report);
        for (std::string line; std::getline(lines, line); )
        {
            std::istringstream ls(line);
            std::string verb, name, file, bytes;
            if (!(ls >> verb >> name >> file) || verb != "fetch") continue;
            if (!get(dist + "/" + file, bytes)) { ++failed; continue; }
            ETCS::TrustEvent put(ETCS::TrustEvent::Op::Replace);
            put.a = list; put.b = std::move(bytes); put.name = name;
            if (put()) { ++placed; ETCS_LOG("Upstream", "Update: " << name << " -> " << put.out); }
            else       { ++failed; ETCS_LOG("Upstream", "Update: " << name << " refused: " << put.out); }
        }
        ETCS_LOG("Upstream", "Update: " << placed << " put in place, " << failed << " not"
                 << (placed ? "; the next session reads them" : ""));
        return failed == 0;
    }

    // The report's "resync etcs <file>": fetch it, have the loader put it in
    // place (bridged), then restart on it. Nothing else of the list is taken.
    bool resync(const std::string& list, const std::string& report, const std::string& dist)
    {
        std::istringstream lines(report);
        for (std::string line; std::getline(lines, line); )
        {
            std::istringstream ls(line);
            std::string verb, name, file, bytes;
            if (!(ls >> verb >> name >> file) || verb != "resync") continue;
            ETCS_LOG("Upstream", "re-sync: this loader's identity moved; fetching its successor");
            if (!get(dist + "/" + file, bytes)) return false;
            ETCS::TrustEvent put(ETCS::TrustEvent::Op::Replace);
            put.a = list; put.b = std::move(bytes); put.name = name;
            if (!put()) { ETCS_LOG("Upstream", "re-sync: the successor was refused: " << put.out); return false; }
            ETCS::TrustEvent go(ETCS::TrustEvent::Op::Resync);
            const bool ok = go();
            ETCS_LOG("Upstream", "re-sync: " << go.out);
            return ok;
        }
        return true;
    }

    bool Paths(const std::string& pki, const std::string& dist)
    {
        if (pki.empty() || pki[0] != '/' || dist.empty() || dist[0] != '/') return false;
        std::lock_guard<std::mutex> l(mu_);
        pki_ = pki; dist_ = dist;
        return true;
    }

    bool SetEmbassy(ETCS::RID rid) { std::lock_guard<std::mutex> l(mu_); embassy_ = rid; return rid != 0; }
    bool SetRelation(const std::string& name)
    {
        if (name.empty() || name.find_first_of(" \n") != std::string::npos) return false;
        std::lock_guard<std::mutex> l(mu_);
        relation_ = name;
        return true;
    }
    void SetRunContext(const ETCS::SignalContext& ctx) { std::lock_guard<std::mutex> l(mu_); run_ctx_ = ctx; }

private:
    static void trim(std::string& s)
    {
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    }

    // GET <path>: the body of a 200, or false.
    bool get(const std::string& path, std::string& body) const
    {
        std::string answer;
        body.clear();
        if (!exchange("GET " + path, answer)) return false;
        const size_t sp = answer.find(' ');
        const std::string status = answer.substr(0, sp);
        if (status != "200" || sp == std::string::npos)
        { ETCS_LOG("Upstream", "GET " << path << ": " << answer.substr(0, 80)); return false; }
        body = answer.substr(sp + 1);
        if (body.compare(0, 9, "refused: ") == 0 || body == "no such path\n"
            || body.compare(0, 26, "this registrar is not set ") == 0)
        { ETCS_LOG("Upstream", "GET " << path << ": " << body.substr(0, 120)); return false; }
        return true;
    }

    // The relation named in the Embassy -- bound to this stand-in -- then Exchange.
    bool exchange(const std::string& question, std::string& answer) const
    {
        answer.clear();
        ETCS::RID embassy = 0;
        std::string relation;
        ETCS::SignalContext ctx;
        { std::lock_guard<std::mutex> l(mu_); embassy = embassy_; relation = relation_; ctx = run_ctx_; }
        if (relation.empty()) { ETCS_LOG("Upstream", "no relation named (SetRelation)."); return false; }
        ETCS::Entity* e = etcs_crypt::resolve(embassy);
        if (!e) { ETCS_LOG("Upstream", "no embassy (RID:" << embassy << ")."); return false; }

        ETCS::Buffer action, data;
        action.write(e->getSourceTag().c_str());
        action.write(".List");
        data.write("https");
        try { e->call(action, data, ctx); }
        catch (const std::exception& ex) { ETCS_LOG("Upstream", "embassy List threw: " << ex.what()); return false; }

        ETCS::RID rid = 0;
        const std::string listing = data.toString();
        size_t at = 0;
        while (at < listing.size() && rid == 0)
        {
            size_t end = listing.find('\n', at);
            if (end == std::string::npos) end = listing.size();
            const std::string line = listing.substr(at, end - at);
            at = end + 1;
            if (line.compare(0, relation.size() + 1, relation + " ") != 0) continue;
            try { rid = static_cast<ETCS::RID>(std::stoull(line.substr(line.rfind(' ') + 1))); } catch (...) { rid = 0; }
        }
        ETCS::Entity* s = etcs_crypt::resolve(rid);
        Foreign_* f = s ? static_cast<Foreign_*>(s->getInterfacePointer(ETCS::Buffer("Foreign"))) : nullptr;
        if (!f) { ETCS_LOG("Upstream", "the embassy lists no live relation '" << relation << "' (closed?)."); return false; }
        if (f->StandIn() != getRID()) { ETCS_LOG("Upstream", "relation '" << relation << "' is bound to another stand-in; refused."); return false; }
        return f->Exchange(question, answer, ctx);
    }

    mutable std::mutex  mu_;
    ETCS::RID           embassy_ = 0;
    std::string         relation_;
    ETCS::SignalContext run_ctx_;
    std::string         pki_ = "/pki", dist_ = "/dist";
    std::string         list_, report_;
};

#endif // CRYPTPROVIDER_UPSTREAM_H__
