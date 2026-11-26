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
 *   Join <member>   this key's word that it joins <member>'s account: "<key> <signature>",
 *                   carried to that member by hand
 *   Admit <key> <signature>   as a member, admit the key whose Join that is
 *   Retire <key>    as a member, retire a key of this account
 *   Portal <code>   as a member, let in the account page showing <code>
 *   Check           the site's list against what is here; the report is List's
 *   Update          Check, then fetch what the site has moved past, stage it, and swap
 *   Paths <p> <d>   where the registrar and the binaries are; /pki and /dist unless said
 *
 * A HOT SWAP: Update stages what it fetches (core/Trust.h, staged_dir) and,
 * once the set is whole, asks the loader to restart on it where this session
 * left off -- the scene saved and replayed, the staged set in place before the
 * successor starts. A registration Register installs is the next session's.
 * When Update stages a successor loader, this loader registers its key for the
 * successor's epoch on its own word first, so the swap starts registered. A
 * RE-SYNC is the same restart when the list is a sibling marketplace's -- this
 * loader's marketplace key retired: Check, whenever it sees one, fetches the
 * successor and has the loader put it in place. A HEAL is the loader's own
 * (core/CommandExecutor.h, heal_at_quiet_point): it runs Update through this
 * type, against the remotes built into it.
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
    bool Register(bool next = false)
    {
        std::string registrar, chain, pki;
        if (!registrar_key(registrar, pki)) return false;
        ETCS::TrustEvent ask(ETCS::TrustEvent::Op::Request);
        ask.a = registrar;
        if (next) ask.b = "next";
        if (!ask()) { ETCS_LOG("Upstream", "Register: " << ask.out); return false; }
        if (!post(pki + "/register", ask.out, chain)) return false;
        ETCS::TrustEvent install(ETCS::TrustEvent::Op::Install);
        install.a = chain;
        const bool ok = install();
        ETCS_LOG("Upstream", "Register: " << install.out);
        return ok;
    }

    std::string Join(const std::string& member)
    {
        std::string registrar, pki;
        if (!registrar_key(registrar, pki)) return std::string();
        ETCS::TrustEvent w(ETCS::TrustEvent::Op::Join);
        w.a = registrar; w.b = member;
        if (!w()) { ETCS_LOG("Upstream", "Join: " << w.out); return std::string(); }
        return w.out;
    }

    bool Admit(const std::string& key, const std::string& signature)
    {
        std::string registrar, pki, said;
        if (!registrar_key(registrar, pki)) return false;
        ETCS::TrustEvent w(ETCS::TrustEvent::Op::Admit);
        w.a = registrar; w.b = key;
        if (!w()) { ETCS_LOG("Upstream", "Admit: " << w.out); return false; }
        if (!post(pki + "/admit", key + " " + signature + " " + w.out, said)) return false;
        ETCS_LOG("Upstream", "Admit: " << said);
        return said == "admitted\n";
    }

    bool Retire(const std::string& key)
    {
        std::string registrar, pki, said;
        if (!registrar_key(registrar, pki)) return false;
        ETCS::TrustEvent w(ETCS::TrustEvent::Op::Retire);
        w.a = registrar; w.b = key;
        if (!w()) { ETCS_LOG("Upstream", "Retire: " << w.out); return false; }
        if (!post(pki + "/retire", key + " " + w.out, said)) return false;
        ETCS_LOG("Upstream", "Retire: " << said);
        return said == "retired\n";
    }

    bool Portal(const std::string& code)
    {
        std::string registrar, pki, said;
        if (!registrar_key(registrar, pki)) return false;
        ETCS::TrustEvent w(ETCS::TrustEvent::Op::Portal);
        w.a = registrar; w.b = code;
        if (!w()) { ETCS_LOG("Upstream", "Portal: " << w.out); return false; }
        if (!post(pki + "/portal/signin", code + " " + w.out, said)) return false;
        ETCS_LOG("Upstream", "Portal: " << said);
        return said == "signed in\n";
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
        bool successor = false;
        std::istringstream lines(report);
        for (std::string line; std::getline(lines, line); )
        {
            std::istringstream ls(line);
            std::string verb, name, file, bytes;
            if (!(ls >> verb >> name >> file) || verb != "fetch") continue;
            if (!get(dist + "/" + file, bytes)) { ++failed; continue; }
            ETCS::TrustEvent put(ETCS::TrustEvent::Op::Replace);
            put.a = list; put.b = std::move(bytes); put.name = name;
            if (put()) { ++placed; successor = successor || name == "etcs"; ETCS_LOG("Upstream", "Update: " << name << " -> " << put.out); }
            else       { ++failed; ETCS_LOG("Upstream", "Update: " << name << " refused: " << put.out); }
        }
        if (successor && !Register(true)) ETCS_LOG("Upstream", "Update: the successor starts unregistered, and registers itself");
        const bool staged = placed > 0 || report.find(" staged\n") != std::string::npos;
        ETCS_LOG("Upstream", "Update: " << placed << " staged, " << failed << " not");
        if (failed || !staged) return failed == 0;
        ETCS::TrustEvent go(ETCS::TrustEvent::Op::Resync);
        const bool ok = go();
        ETCS_LOG("Upstream", "Update: " << go.out);
        return ok;
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
            if (!Register(true)) ETCS_LOG("Upstream", "re-sync: the successor starts unregistered, and registers itself");
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
    // The site registrar's key, hex, and where it is.
    bool registrar_key(std::string& key, std::string& pki) const
    {
        { std::lock_guard<std::mutex> l(mu_); pki = pki_; }
        if (!get(pki + "/registrar", key)) return false;
        trim(key);
        return key.size() == 64;
    }

    static void trim(std::string& s)
    {
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    }

    // GET <path>, or POST <path> with words: the body of a 200, or false.
    bool get(const std::string& path, std::string& body) const { return fetch("GET " + path, path, body); }
    bool post(const std::string& path, const std::string& words, std::string& body) const
    {
        return fetch("POST " + path + "\n" + words, path, body);
    }
    bool fetch(const std::string& question, const std::string& path, std::string& body) const
    {
        std::string answer;
        body.clear();
        if (!exchange(question, answer)) return false;
        const size_t sp = answer.find(' ');
        const std::string status = answer.substr(0, sp);
        if (status != "200" || sp == std::string::npos)
        { ETCS_LOG("Upstream", path << ": " << answer.substr(0, 80)); return false; }
        body = answer.substr(sp + 1);
        if (body.compare(0, 9, "refused: ") == 0 || body == "no such path\n"
            || body.compare(0, 26, "this registrar is not set ") == 0)
        { ETCS_LOG("Upstream", path << ": " << body.substr(0, 120)); return false; }
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
