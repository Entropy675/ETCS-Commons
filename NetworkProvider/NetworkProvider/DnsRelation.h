#ifndef NETWORKPROVIDER_DNSRELATION_H__
#define NETWORKPROVIDER_DNSRELATION_H__
#include "../../../ontology.h"

#include <algorithm>
#include <mutex>
#include <set>
#include <string>

#if !defined(__EMSCRIPTEN__)
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#endif

/*
 * DnsRelation -- a Foreign relation (ontology/Foreign.h) to the public
 * resolvers: what the world currently believes a name points at.
 *
 * QUESTION: "<name>". ANSWER: its IPv4 addresses, space-separated and sorted
 * ("" when it has none -- an answer, not a failure). A records only, which is
 * what a DDNS roll holds. The far reader is the system resolver, caches and
 * all; there are no wire bytes this runtime owns.
 *
 * One instance per relation, locked once active (HttpsRelation's rule):
 * Target and the stand-in fix its identity, Activate makes it live, and from
 * then on configuration refuses. Pulled, not pushed: Apply refuses.
 */
class DnsRelation : public ForeignBase<DnsRelation>, public DeletableBase<DnsRelation>
{
public:
    WIRE_TYPE_IDENTITY(DnsRelation);

    DnsRelation() = default;

    std::string SpeaksConcrete() const override { return "dns"; }

    bool ExchangeConcrete(const std::string& question, std::string& answer,
                          ETCS::SignalContext ctx) override
    {
        (void)ctx;
        answer.clear();
        if (StandIn() == 0) { ETCS_LOG("DnsRelation", "refused: not active (no stand-in bound)."); return false; }
        std::string name;
        if (!nameOf(question, name)) { ETCS_LOG("DnsRelation", "refused: '" << question << "'"); return false; }
#if defined(__EMSCRIPTEN__)
        ETCS_LOG("DnsRelation", "no resolver in a browser runtime.");
        return false;
#else
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        const int rc = ::getaddrinfo(name.c_str(), nullptr, &hints, &res);
        if (rc == EAI_NONAME || rc == EAI_NODATA) return true;   // an answer: no records
        if (rc != 0 || !res) { ETCS_LOG("DnsRelation", name << ": " << ::gai_strerror(rc)); return false; }
        std::set<std::string> seen;
        for (addrinfo* a = res; a; a = a->ai_next)
        {
            char ip[INET_ADDRSTRLEN] = {};
            const sockaddr_in* in = reinterpret_cast<const sockaddr_in*>(a->ai_addr);
            if (::inet_ntop(AF_INET, &in->sin_addr, ip, sizeof ip)) seen.insert(ip);
        }
        ::freeaddrinfo(res);
        for (const std::string& ip : seen) answer += (answer.empty() ? "" : " ") + ip;
        return true;
#endif
    }

    std::string TargetsConcrete() const override { std::lock_guard<std::mutex> l(mu_); return target_; }

    bool ApplyConcrete(ETCS::Entity& target, ETCS::SignalContext ctx) override
    {
        (void)target; (void)ctx;
        ETCS_LOG("DnsRelation", "Apply: pulled, not pushed -- the stand-in asks through Exchange.");
        return false;
    }

    bool Target(const std::string& tags)
    {
        if (refuseActive("Target") || tags.empty()) return false;
        std::lock_guard<std::mutex> l(mu_);
        target_ = tags;
        return true;
    }
    bool Activate(ETCS::RID stand_in)
    {
        if (refuseActive("Activate")) return false;
        { std::lock_guard<std::mutex> l(mu_); if (target_.empty()) { ETCS_LOG("DnsRelation", "Activate: set Target first."); return false; } }
        if (!BindStandIn(stand_in)) { ETCS_LOG("DnsRelation", "Activate: RID " << stand_in << " doesn't carry the target."); return false; }
        ETCS_LOG("DnsRelation", "active for stand-in RID:" << stand_in << " (configuration locked).");
        return true;
    }

    bool DeleteConcrete() override
    {
        std::string conjugate_key = getSourceModule().toString() + ":" + getSourceTag().toString();
        return ETCS::DestroyEvent{conjugate_key.c_str(), this}();
    }

private:
    bool refuseActive(const char* what) const
    {
        if (StandIn() == 0) return false;
        ETCS_LOG("DnsRelation", what << ": refused, the relation is active. Delete it and make another.");
        return true;
    }

    static bool nameOf(const std::string& q, std::string& name)
    {
        name = q.rfind("dns://", 0) == 0 ? q.substr(6) : q;
        while (!name.empty() && (name.back() == ' ' || name.back() == '/' || name.back() == '\n')) name.pop_back();
        if (name.empty() || name.size() > 253) return false;
        return std::all_of(name.begin(), name.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.';
        });
    }

    mutable std::mutex mu_;
    std::string        target_;
};

#endif // NETWORKPROVIDER_DNSRELATION_H__
