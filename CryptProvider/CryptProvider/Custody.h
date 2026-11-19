#ifndef CRYPTPROVIDER_CUSTODY_H__
#define CRYPTPROVIDER_CUSTODY_H__

#include "Certificate.h"
#include "Shard.h"
#include <mutex>

/*
 * Custody -- what one custodian holds, as a Directory: the stand-in its
 * FileRelation (NetworkProvider) lands on. Namecheap's shape: the relation is
 * found in an Embassy by name (SetEmbassy, SetRelation), must be bound to
 * THIS stand-in, and every call is one Foreign_::Exchange -- a closed Embassy
 * stops it cold.
 *
 *   Advertise <name> shard <shard>   the share, as <name>.shard.private
 *   Advertise <name> cert <cert>     the certificate, hex, as <name>.cert
 *   Recall <name> <shard|cert>       the custodian's copy back into an empty one
 *   Withdraw <name>                  the custodian's copy dropped
 *   List [kind]                      "name kind relation", as the custodian holds it now
 *
 * A shard's share passes through here as {private:<rid>} and nothing else.
 */
class Custody : public DirectoryBase<Custody>, public DeletableBase<Custody>
{
public:
    WIRE_TYPE_IDENTITY(Custody);

    Custody() = default;

    // --- Directory_ ---------------------------------------------------------------
    bool AdvertiseConcrete(const std::string& name, const std::string& kind, const std::string& info) override
    {
        if (!plainName(name)) { ETCS_LOG("Custody", "Advertise: '" << name << "' is not a custody name."); return false; }
        ETCS::RID rid = 0;
        try { rid = static_cast<ETCS::RID>(std::stoull(info)); } catch (...) { rid = 0; }
        std::string answer;
        if (kind == "shard")
        {
            Shard* s = etcs_crypt::as<Shard>(rid);
            if (!s || !s->Holds()) { ETCS_LOG("Custody", "Advertise: RID " << info << " is not a Shard holding a share."); return false; }
            return exchange("PUT " + name + ".shard.private\n{private:" + std::to_string(rid) + "}", answer);
        }
        if (kind == "cert")
        {
            Certificate* c = etcs_crypt::as<Certificate>(rid);
            const std::string bytes = c ? c->Bytes() : std::string();
            if (bytes.empty()) { ETCS_LOG("Custody", "Advertise: RID " << info << " is not an issued Certificate."); return false; }
            return exchange("PUT " + name + ".cert\n" + etcs_crypt::hex(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()) + "\n", answer);
        }
        ETCS_LOG("Custody", "Advertise: kind '" << kind << "' (shard, cert).");
        return false;
    }

    bool WithdrawConcrete(const std::string& name) override
    {
        const std::string file = held(name);
        std::string answer;
        return !file.empty() && exchange("DROP " + file, answer);
    }

    std::string ListingConcrete(const std::string& kind) const override
    {
        std::string names, out, relation;
        { std::lock_guard<std::mutex> l(mu_); relation = relation_; }
        if (!exchange("LIST", names)) return {};
        size_t at = 0;
        while (at < names.size())
        {
            size_t end = names.find('\n', at);
            if (end == std::string::npos) end = names.size();
            const std::string f = names.substr(at, end - at);
            at = end + 1;
            std::string n, k;
            if (!split(f, n, k) || (!kind.empty() && k != kind)) continue;
            out += n + " " + k + " " + relation + "\n";
        }
        return out;
    }

    bool DeleteConcrete() override
    {
        std::string key = getSourceModule().toString() + ":" + getSourceTag().toString();
        return ETCS::DestroyEvent{key.c_str(), this}();
    }

    // --- verbs ---------------------------------------------------------------------
    bool Recall(const std::string& name, ETCS::RID rid)
    {
        if (!plainName(name)) return false;
        std::string answer;
        if (Shard* s = etcs_crypt::as<Shard>(rid))
        {
            if (s->Holds()) { ETCS_LOG("Custody", "Recall: the shard already holds -- custody never overwrites."); return false; }
            return exchange("GET " + name + ".shard.private\n{private:" + std::to_string(rid) + "}", answer);
        }
        if (Certificate* c = etcs_crypt::as<Certificate>(rid))
        {
            if (!exchange("GET " + name + ".cert", answer)) return false;
            while (!answer.empty() && (answer.back() == '\n' || answer.back() == '\r')) answer.pop_back();
            std::string bytes(answer.size() / 2, '\0');
            if (!etcs_crypt::unhex(answer, reinterpret_cast<uint8_t*>(&bytes[0]), bytes.size()) || !c->Load(bytes))
            { ETCS_LOG("Custody", "Recall: '" << name << ".cert' is not a certificate."); return false; }
            return true;
        }
        ETCS_LOG("Custody", "Recall: RID " << rid << " is neither a Shard nor a Certificate.");
        return false;
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
    static bool plainName(const std::string& n)
    {
        if (n.empty() || n.size() > 100 || n[0] == '.') return false;
        for (char c : n)
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) return false;
        return true;
    }
    // "<name>.shard.private" / "<name>.cert" -> name, kind.
    static bool split(const std::string& f, std::string& name, std::string& kind)
    {
        static const std::string shard = ".shard.private", cert = ".cert";
        auto ends = [&](const std::string& s) { return f.size() > s.size() && f.compare(f.size() - s.size(), s.size(), s) == 0; };
        if (ends(shard)) { name = f.substr(0, f.size() - shard.size()); kind = "shard"; return true; }
        if (ends(cert))  { name = f.substr(0, f.size() - cert.size());  kind = "cert";  return true; }
        return false;
    }
    std::string held(const std::string& name) const
    {
        std::string names;
        if (!plainName(name) || !exchange("LIST", names)) return {};
        for (const char* suffix : { ".shard.private", ".cert" })
            if (names.find(name + suffix + "\n") == 0 || names.find("\n" + name + suffix + "\n") != std::string::npos)
                return name + suffix;
        return {};
    }

    // The relation named in the Embassy -- bound to this stand-in -- then Exchange.
    bool exchange(const std::string& question, std::string& answer) const
    {
        answer.clear();
        ETCS::RID embassy = 0;
        std::string relation;
        ETCS::SignalContext ctx;
        { std::lock_guard<std::mutex> l(mu_); embassy = embassy_; relation = relation_; ctx = run_ctx_; }
        if (relation.empty()) { ETCS_LOG("Custody", "no relation named (SetRelation)."); return false; }
        ETCS::Entity* e = etcs_crypt::resolve(embassy);
        if (!e) { ETCS_LOG("Custody", "no embassy (RID:" << embassy << ")."); return false; }

        ETCS::Buffer action, data;
        action.write(e->getSourceTag().c_str());
        action.write(".List");
        data.write("file");
        try { e->call(action, data, ctx); }
        catch (const std::exception& ex) { ETCS_LOG("Custody", "embassy List threw: " << ex.what()); return false; }

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
        if (!f) { ETCS_LOG("Custody", "the embassy lists no live relation '" << relation << "' (closed?)."); return false; }
        if (f->StandIn() != getRID()) { ETCS_LOG("Custody", "relation '" << relation << "' is bound to another stand-in; refused."); return false; }
        return f->Exchange(question, answer, ctx);
    }

    mutable std::mutex  mu_;
    ETCS::RID           embassy_ = 0;
    std::string         relation_;
    ETCS::SignalContext run_ctx_;
};

#endif // CRYPTPROVIDER_CUSTODY_H__
