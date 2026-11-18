#ifndef CRYPTPROVIDER_CERTIFICATE_H__
#define CRYPTPROVIDER_CERTIFICATE_H__

#include "Keypair.h"
#include "../../../core/Signature.h"
#include <mutex>

/*
 * Certificate -- one link of the chain, in core/Signature.h's layout. Public:
 * nothing here is a secret, and it leaves through Custody as hex.
 *
 *   AddEpoch <hex>                    an epoch it is valid for; before Issue
 *   Issue <keypair> <role> <subject>  signed by that keypair; the subject is a
 *                                     Keypair RID or a public key in hex. A root
 *                                     is its own subject; nothing else may be.
 *   Verify [<issuer cert>]            the signature under the issuer key it names;
 *                                     with the issuer's certificate, also that
 *                                     certificate's subject and a higher role
 */
class Certificate : public DeletableBase<Certificate>
{
public:
    WIRE_TYPE_IDENTITY(Certificate);

    Certificate() = default;

    bool DeleteConcrete() override
    {
        std::string key = getSourceModule().toString() + ":" + getSourceTag().toString();
        return ETCS::DestroyEvent{key.c_str(), this}();
    }

    bool AddEpoch(const std::string& hex)
    {
        ETCS::sig::Epoch e{};
        if (!etcs_crypt::unhex(hex, e.data(), e.size())) return false;
        std::lock_guard<std::mutex> l(mu_);
        if (issued_ || cert_.epochs.size() >= ETCS::sig::kMaxEpochs) return false;
        cert_.epochs.push_back(e);
        return true;
    }

    bool Issue(ETCS::RID issuer, const std::string& role, const std::string& subject)
    {
        ETCS::sig::Role r;
        if (!ETCS::sig::role_from_name(role, r)) { ETCS_LOG("Certificate", "Issue: no role '" << role << "'."); return false; }
        Keypair* by = etcs_crypt::as<Keypair>(issuer);
        if (!by || !by->Holds()) { ETCS_LOG("Certificate", "Issue: the issuer is not a Keypair holding a key."); return false; }
        ETCS::sig::Key sub{}, iss{};
        if (!subjectKey(subject, sub)) { ETCS_LOG("Certificate", "Issue: subject '" << subject << "' is neither a Keypair nor a key."); return false; }
        by->PublicBytes(iss.data());
        if ((r == ETCS::sig::Role::Root) != (sub == iss))
        {
            ETCS_LOG("Certificate", "Issue: a root is its own subject, and nothing else may be.");
            return false;
        }
        std::lock_guard<std::mutex> l(mu_);
        if (issued_) { ETCS_LOG("Certificate", "Issue: already issued -- make another."); return false; }
        cert_.role = r; cert_.subject = sub; cert_.issuer = iss;
        const std::string msg = ETCS::sig::signed_part(cert_);
        by->Sign(reinterpret_cast<const uint8_t*>(msg.data()), msg.size(), cert_.signature);
        issued_ = true;
        ETCS_LOG("Certificate", ETCS::sig::role_name(r) << " " << etcs_crypt::hex(sub.data(), 32)
                 << " issued by " << etcs_crypt::hex(iss.data(), 32));
        return true;
    }

    bool Verify(ETCS::RID issuer_cert) const
    {
        ETCS::sig::Certificate c;
        if (!copy(c)) return false;
        const std::string msg = ETCS::sig::signed_part(c);
        if (etcs_crypt::mc::crypto_ed25519_check(c.signature, c.issuer.data(),
                reinterpret_cast<const uint8_t*>(msg.data()), msg.size()) != 0)
            return false;
        if (c.role == ETCS::sig::Role::Root && c.subject != c.issuer) return false;
        if (!issuer_cert) return true;
        const Certificate* up = etcs_crypt::as<Certificate>(issuer_cert);
        ETCS::sig::Certificate u;
        if (!up || !up->copy(u)) return false;
        return u.subject == c.issuer && static_cast<uint8_t>(u.role) < static_cast<uint8_t>(c.role);
    }

    // --- reads ---------------------------------------------------------------------
    std::string Role() const    { ETCS::sig::Certificate c; return copy(c) ? ETCS::sig::role_name(c.role) : ""; }
    std::string Subject() const { ETCS::sig::Certificate c; return copy(c) ? etcs_crypt::hex(c.subject.data(), 32) : ""; }
    std::string Issuer() const  { ETCS::sig::Certificate c; return copy(c) ? etcs_crypt::hex(c.issuer.data(), 32) : ""; }
    std::string Digest() const
    {
        const std::string b = Bytes();
        if (b.empty()) return {};
        uint8_t d[32];
        etcs_crypt::sha256(b.data(), b.size(), d);
        return etcs_crypt::hex(d, 32);
    }

    // --- Custody's reach ------------------------------------------------------------
    std::string Bytes() const { ETCS::sig::Certificate c; return copy(c) ? ETCS::sig::encode(c) : std::string(); }
    bool Load(const std::string& bytes)
    {
        ETCS::sig::Certificate c;
        size_t used = 0;
        if (!ETCS::sig::decode(bytes.data(), bytes.size(), c, &used) || used != bytes.size()) return false;
        std::lock_guard<std::mutex> l(mu_);
        if (issued_) return false;
        cert_ = c;
        issued_ = true;
        return true;
    }

private:
    bool copy(ETCS::sig::Certificate& out) const
    {
        std::lock_guard<std::mutex> l(mu_);
        if (!issued_) return false;
        out = cert_;
        return true;
    }
    static bool subjectKey(const std::string& s, ETCS::sig::Key& out)
    {
        if (etcs_crypt::unhex(s, out.data(), out.size())) return true;
        ETCS::RID rid = 0;
        try { rid = static_cast<ETCS::RID>(std::stoull(s)); } catch (...) { return false; }
        Keypair* kp = etcs_crypt::as<Keypair>(rid);
        return kp && kp->PublicBytes(out.data());
    }

    mutable std::mutex     mu_;
    bool                   issued_ = false;
    ETCS::sig::Certificate cert_;
};

#endif // CRYPTPROVIDER_CERTIFICATE_H__
