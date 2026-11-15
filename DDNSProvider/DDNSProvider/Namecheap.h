#ifndef DDNSPROVIDER_NAMECHEAP_H__
#define DDNSPROVIDER_NAMECHEAP_H__

#include "../../../core_defs.h"
#include "../../../ontology.h"

#include <mutex>
#include <string>
#include <vector>

/*
 * Namecheap -- Namecheap's dynamic DNS as a Directory.
 *
 * A roll written in someone else's hand: a name is a held host ("@" for the
 * bare domain, "www", ...), its kind is A, its info is the address the world
 * resolves it to. It claims Directory and Deletable and nothing that pretends
 * the far side is ours -- not Environmental (nothing over there replays), not
 * Remote (it is not ETCS).
 *
 *   Advertise <host> A <ip>   GET /update?host=..&domain=..&password=..&ip=..
 *                             true only on 200 with <ErrCount>0</ErrCount>
 *   Withdraw                  false: DDNS cannot delete a record
 *   List [A]                  each held host as the resolver answers it now
 *                             ("-" when it has no record)
 *   Self                      GET /getip: our address as Namecheap sees it, a
 *                             witness for a Herald
 *
 * NO SOCKETS HERE. This Directory is the STAND-IN two Foreign relations land
 * on (one per stranger, ontology/Foreign.h): the REGISTRAR (Namecheap's API --
 * what we told the roll) and the RESOLVER (the public resolvers -- what the
 * world believes the roll says). Each is named in the Embassy (SetEmbassy,
 * SetRegistrar, SetResolver); every call finds its relation in the Embassy's
 * List by name, checks the relation is bound to THIS stand-in, and calls
 * Foreign_::Exchange -- so a closed Embassy stops this type cold, a relation
 * bound elsewhere is refused, and answers of any size never pass a Buffer.
 * The registrar's Self against the resolver's List is exactly the
 * disagreement a Herald watches.
 *
 * THE PASSWORD NEVER PASSES THROUGH HERE. The update names it as
 * {secret:<name>} (SetSecret, default "namecheap"); the https speaker fills
 * it in on the wire. Namecheap's host and domain must match the case shown
 * in the account.
 */
class Namecheap :
    public DirectoryBase<Namecheap>, public DeletableBase<Namecheap>
{
public:
    WIRE_TYPE_IDENTITY(Namecheap);

    Namecheap() = default;

    // --- Directory_ ------------------------------------------------------------

    bool AdvertiseConcrete(const std::string& name, const std::string& kind, const std::string& info) override
    {
        if (kind != "A") { ETCS_LOG("Namecheap", "Advertise: only A records ('" << kind << "')."); return false; }
        if (!holds(name)) { ETCS_LOG("Namecheap", "Advertise: '" << name << "' is not held."); return false; }
        if (!isIPv4(info)) { ETCS_LOG("Namecheap", "Advertise: '" << info << "' is not an IPv4 address."); return false; }
        std::string domain, secret, registrar;
        { std::lock_guard<std::mutex> lock(mu_); domain = domain_; secret = secret_; registrar = registrar_; }
        if (domain.empty()) { ETCS_LOG("Namecheap", "Advertise: no domain set."); return false; }

        std::string answer;
        const std::string q = "GET /update?host=" + name + "&domain=" + domain
                            + "&password={secret:" + secret + "}&ip=" + info;
        if (!exchange(registrar, q, answer)) return false;
        const bool ok = answer.rfind("200 ", 0) == 0 && answer.find("<ErrCount>0</ErrCount>") != std::string::npos;
        ETCS_LOG("Namecheap", "update '" << name << "." << domain << "' -> " << info << ": "
                 << (ok ? "accepted" : "refused (" + answer.substr(0, answer.find(' ')) + ")"));
        return ok;
    }

    bool WithdrawConcrete(const std::string& name) override
    {
        ETCS_LOG("Namecheap", "Withdraw '" << name << "': dynamic DNS cannot delete a record.");
        return false;
    }

    std::string ListingConcrete(const std::string& kind) const override
    {
        if (!kind.empty() && kind != "A") return {};
        std::vector<std::string> hosts;
        std::string domain, resolver;
        { std::lock_guard<std::mutex> lock(mu_); hosts = hosts_; domain = domain_; resolver = resolver_; }
        if (domain.empty()) return {};
        std::string out;
        for (const std::string& h : hosts)
        {
            std::string answer;
            const std::string fqdn = (h == "@") ? domain : h + "." + domain;
            const bool ok = exchange(resolver, fqdn, answer);
            const std::string first = answer.substr(0, answer.find(' '));
            out += h + " A " + (ok && !first.empty() ? first : std::string("-")) + "\n";
        }
        return out;
    }

    // --- Deletable_ ------------------------------------------------------------

    bool DeleteConcrete() override
    {
        std::string conjugate_key = getSourceModule().toString() + ":" + getSourceTag().toString();
        return ETCS::DestroyEvent{conjugate_key.c_str(), this}();
    }

    // --- the witness -----------------------------------------------------------

    std::string Self() const
    {
        std::string registrar;
        { std::lock_guard<std::mutex> lock(mu_); registrar = registrar_; }
        std::string answer;
        if (!exchange(registrar, "GET /getip", answer)) return {};
        if (answer.rfind("200 ", 0) != 0) return {};
        std::string ip = answer.substr(4);
        while (!ip.empty() && (ip.back() == '\n' || ip.back() == '\r' || ip.back() == ' ')) ip.pop_back();
        while (!ip.empty() && ip.front() == ' ') ip.erase(0, 1);
        return isIPv4(ip) ? ip : std::string();
    }

    // --- configuration ---------------------------------------------------------

    bool SetDomain(const std::string& d)   { return setField(domain_, d); }
    bool SetRegistrar(const std::string& r) { return setField(registrar_, r); }
    bool SetResolver(const std::string& r)  { return setField(resolver_, r); }
    bool SetSecret(const std::string& s)   { return setField(secret_, s); }
    bool SetEmbassy(ETCS::RID rid)         { std::lock_guard<std::mutex> lock(mu_); embassy_ = rid; return rid != 0; }

    bool Hold(const std::string& host)
    {
        if (host.empty() || host.find_first_of(" \n&=?/") != std::string::npos) return false;
        std::lock_guard<std::mutex> lock(mu_);
        for (const std::string& h : hosts_) if (h == host) return true;
        hosts_.push_back(host);
        return true;
    }

    void SetRunContext(const ETCS::SignalContext& ctx) { std::lock_guard<std::mutex> lock(mu_); run_ctx_ = ctx; }

private:
    bool setField(std::string& field, const std::string& v)
    {
        if (v.empty() || v.find_first_of(" \n&?#") != std::string::npos) return false;
        std::lock_guard<std::mutex> lock(mu_);
        field = v;
        return true;
    }

    bool holds(const std::string& host) const
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (const std::string& h : hosts_) if (h == host) return true;
        return false;
    }

    static bool isIPv4(const std::string& s)
    {
        int parts = 0, digits = 0, value = 0;
        for (char c : s)
        {
            if (c >= '0' && c <= '9') { value = value * 10 + (c - '0'); if (++digits > 3 || value > 255) return false; }
            else if (c == '.') { if (digits == 0) return false; ++parts; digits = 0; value = 0; }
            else return false;
        }
        return parts == 3 && digits > 0;
    }

    // The relation named `relation` from the Embassy -- bound to this stand-in
    // -- then Exchange on it.
    bool exchange(const std::string& relation, const std::string& question, std::string& answer) const
    {
        answer.clear();
        if (relation.empty()) { ETCS_LOG("Namecheap", "no relation named for this question."); return false; }
        ETCS::RID embassy = 0;
        ETCS::SignalContext ctx;
        { std::lock_guard<std::mutex> lock(mu_); embassy = embassy_; ctx = run_ctx_; }
        ETCS::Entity* e = resolve(embassy);
        if (!e) { ETCS_LOG("Namecheap", "no embassy (RID:" << embassy << ")."); return false; }

        const ETCS::Buffer tag = e->getSourceTag();
        ETCS::Buffer action;
        action.write(tag.c_str());
        action.write(".List");
        ETCS::Buffer data;
        try { e->call(action, data, ctx); }
        catch (const std::exception& ex) { ETCS_LOG("Namecheap", "embassy List threw: " << ex.what()); return false; }

        // "name scheme rid" per line: the line whose name is ours.
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
            const size_t b = line.rfind(' ');
            try { rid = static_cast<ETCS::RID>(std::stoull(line.substr(b + 1))); } catch (...) { rid = 0; }
        }
        ETCS::Entity* s = resolve(rid);
        Foreign_* f = s ? static_cast<Foreign_*>(s->getInterfacePointer(ETCS::Buffer("Foreign"))) : nullptr;
        if (!f) { ETCS_LOG("Namecheap", "the embassy lists no live relation '" << relation << "' (closed?)."); return false; }
        if (f->StandIn() != getRID()) { ETCS_LOG("Namecheap", "relation '" << relation << "' is bound to another stand-in; refused."); return false; }
        return f->Exchange(question, answer, ctx);
    }

    static ETCS::Entity* resolve(ETCS::RID rid)
    {
        return rid ? ETCS::etcs_resolve_rid_anywhere(&ETCS::getLoader(), rid) : nullptr;
    }

    mutable std::mutex       mu_;
    std::string              domain_;
    std::string              registrar_;
    std::string              resolver_;
    std::string              secret_ = "namecheap";
    std::vector<std::string> hosts_;
    ETCS::RID                embassy_ = 0;
    ETCS::SignalContext      run_ctx_;
};

#endif // DDNSPROVIDER_NAMECHEAP_H__
