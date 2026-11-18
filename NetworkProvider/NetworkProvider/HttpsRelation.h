#ifndef NETWORKPROVIDER_HTTPSRELATION_H__
#define NETWORKPROVIDER_HTTPSRELATION_H__
#include "../../../ontology.h"
#include "HttpsWire.h"

#include <atomic>
#include <mutex>
#include <string>

/*
 * HttpsRelation -- a Foreign relation (ontology/Foreign.h) to one stranger,
 * spoken in HTTPS (HttpsWire).
 *
 * ONE INSTANCE PER RELATION. The type is generic; an instance is one stranger:
 * its origin (Origin https://host[:port]), its secrets, its trust anchors, the
 * surface its stand-in must carry (Target), and the stand-in itself. A
 * question names only a method and a path -- the host is the relation's, so
 * this relation cannot be pointed at anyone else.
 *
 * LOCKED ONCE ACTIVE. Binding the stand-in makes the relation active, and from
 * then on every configuration verb refuses: a relation's identity is fixed for
 * as long as it lives. To change one, delete it and make another. So the set of
 * live HttpsRelations is a set of known strangers, each exactly as configured
 * when it went live.
 *
 *   QUESTION  "<METHOD> /path[?query]" (a POST's body after the first newline)
 *   ANSWER    "<status> <body>"
 *
 * Apply: this relation is pulled, not pushed -- its stand-in asks through
 * Exchange -- so Apply refuses (false) and says so.
 */
class HttpsRelation : public ForeignBase<HttpsRelation>, public DeletableBase<HttpsRelation>
{
public:
    WIRE_TYPE_IDENTITY(HttpsRelation);

    HttpsRelation() = default;

    // --- Foreign_ / Relational_ -------------------------------------------------

    std::string SpeaksConcrete() const override { return "https"; }
    std::string TargetsConcrete() const override { std::lock_guard<std::mutex> l(mu_); return target_; }

    bool ExchangeConcrete(const std::string& question, std::string& answer, ETCS::SignalContext ctx) override
    {
        answer.clear();
        if (StandIn() == 0) { ETCS_LOG("HttpsRelation", "refused: not active (no stand-in bound)."); return false; }
        const size_t sp = question.find(' ');
        if (sp == std::string::npos || sp + 1 >= question.size() || question[sp + 1] != '/')
        { ETCS_LOG("HttpsRelation", "refused: '" << question << "' (expected '<METHOD> /path')."); return false; }
        std::string origin;
        { std::lock_guard<std::mutex> l(mu_); origin = origin_; }
        ETCS_LOG("HttpsRelation", question.substr(0, question.find('\n')) << "  -> " << origin);
        return wire_.Exchange(question.substr(0, sp + 1) + origin + question.substr(sp + 1), answer, ctx);
    }

    bool ApplyConcrete(ETCS::Entity& target, ETCS::SignalContext ctx) override
    {
        (void)target; (void)ctx;
        ETCS_LOG("HttpsRelation", "Apply: pulled, not pushed -- the stand-in asks through Exchange.");
        return false;
    }

    bool DeleteConcrete() override
    {
        std::string conjugate_key = getSourceModule().toString() + ":" + getSourceTag().toString();
        return ETCS::DestroyEvent{conjugate_key.c_str(), this}();
    }

    // --- configuration: refused once active --------------------------------------

    bool Origin(const std::string& origin)
    {
        std::string host; int port = 0;
        if (refuseActive("Origin") || !HttpsWire::parseOrigin(origin, host, port)) return false;
        std::lock_guard<std::mutex> l(mu_);
        origin_ = "https://" + host + (port == 443 ? std::string() : ":" + std::to_string(port));
        return true;
    }
    bool Target(const std::string& tags)
    {
        if (refuseActive("Target") || tags.empty()) return false;
        std::lock_guard<std::mutex> l(mu_);
        target_ = tags;
        return true;
    }
    bool SetCaFile(const std::string& path) { return !refuseActive("SetCaFile") && wire_.SetCaFile(path); }
    bool Secret(const std::string& name, const std::string& path)
    {
        if (refuseActive("Secret") || !wire_.Secret(name, path)) return false;
        ETCS_LOG("HttpsRelation", "secret '" << name << "' bound to a file.");
        return true;
    }

    // Goes live: the origin and target must be set, the stand-in must land.
    bool Activate(ETCS::RID stand_in)
    {
        if (refuseActive("Activate")) return false;
        { std::lock_guard<std::mutex> l(mu_); if (origin_.empty() || target_.empty()) { ETCS_LOG("HttpsRelation", "Activate: set Origin and Target first."); return false; } }
        if (!BindStandIn(stand_in)) { ETCS_LOG("HttpsRelation", "Activate: RID " << stand_in << " doesn't carry '" << target_ << "'."); return false; }
        ETCS_LOG("HttpsRelation", "active: " << origin_ << " for stand-in RID:" << stand_in << " (configuration locked).");
        return true;
    }

private:
    bool refuseActive(const char* what) const
    {
        if (StandIn() == 0) return false;
        ETCS_LOG("HttpsRelation", what << ": refused, the relation is active. Delete it and make another.");
        return true;
    }

    mutable std::mutex mu_;
    std::string        origin_;
    std::string        target_;
    HttpsWire          wire_;
};

#endif // NETWORKPROVIDER_HTTPSRELATION_H__
