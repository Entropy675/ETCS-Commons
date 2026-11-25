#ifndef CRYPTPROVIDER_KEYPAIR_H__
#define CRYPTPROVIDER_KEYPAIR_H__

#include "Crypt.h"
#include <mutex>

/*
 * Keypair -- one Ed25519 key, held. Private. Reveal and Receive refuse, so no
 * edge can write a Keypair out or read one in. A key leaves two ways only:
 * split, as Shamir's shards (back through Combine), or whole, into a Database
 * that claims Private (back through LoadFrom).
 *
 *   Generate        a fresh seed from the OS; refused while one is held
 *   PublicKey       hex, or "" when empty
 *   Sign <hex>      the signature of those bytes, hex
 *   StoreTo <db>    keep this key there; the db must claim Database and Private
 *   LoadFrom <db>   take the key kept there; refused while one is held
 */
class Keypair : public PrivateBase<Keypair>, public DeletableBase<Keypair>
{
public:
    WIRE_TYPE_IDENTITY(Keypair);

    Keypair() = default;
    ~Keypair() override { WipeConcrete(); }

    // --- Private_ ----------------------------------------------------------------
    bool HoldsConcrete() const override { std::lock_guard<std::mutex> l(mu_); return held_; }
    void WipeConcrete() override
    {
        std::lock_guard<std::mutex> l(mu_);
        etcs_crypt::wipe(seed_, sizeof seed_);
        etcs_crypt::wipe(secret_, sizeof secret_);
        std::memset(public_, 0, sizeof public_);
        held_ = false;
    }
    bool RevealConcrete(std::string& out) const override
    {
        out.clear();
        ETCS_LOG("Keypair", "Reveal: refused -- a keypair leaves only as shards.");
        return false;
    }
    bool ReceiveConcrete(const std::string& in) override
    {
        (void)in;
        ETCS_LOG("Keypair", "Receive: refused -- a keypair comes back only through Combine.");
        return false;
    }

    bool DeleteConcrete() override
    {
        WipeConcrete();
        std::string key = getSourceModule().toString() + ":" + getSourceTag().toString();
        return ETCS::DestroyEvent{key.c_str(), this}();
    }

    // --- verbs -------------------------------------------------------------------
    bool Generate()
    {
        uint8_t seed[32];
        if (!etcs_crypt::random_bytes(seed, sizeof seed))
        {
            ETCS_LOG("Keypair", "Generate: no randomness from the OS.");
            return false;
        }
        const bool ok = FromSeed(seed);
        etcs_crypt::wipe(seed, sizeof seed);
        return ok;
    }
    std::string PublicKey() const
    {
        std::lock_guard<std::mutex> l(mu_);
        return held_ ? etcs_crypt::hex(public_, sizeof public_) : std::string();
    }
    bool Sign(const uint8_t* msg, size_t n, uint8_t sig[64]) const
    {
        std::lock_guard<std::mutex> l(mu_);
        if (!held_) return false;
        etcs_crypt::mc::crypto_ed25519_sign(sig, secret_, msg, n);
        return true;
    }

    // Private to Private, in this process; no line or log carries the seed.
    bool StoreTo(ETCS::RID db) const
    {
        Private_* medium = private_database(db, "StoreTo");
        uint8_t seed[32];
        if (!medium || !CopySeed(seed)) return false;
        std::string text = etcs_crypt::hex(seed, sizeof seed);
        etcs_crypt::wipe(seed, sizeof seed);
        const bool ok = medium->Receive(text);
        etcs_crypt::wipe(&text[0], text.size());
        return ok;
    }
    bool LoadFrom(ETCS::RID db)
    {
        Private_* medium = private_database(db, "LoadFrom");
        std::string text;
        uint8_t seed[32];
        const bool ok = medium && medium->Reveal(text) && etcs_crypt::unhex(text, seed, sizeof seed) && FromSeed(seed);
        if (!text.empty()) etcs_crypt::wipe(&text[0], text.size());
        etcs_crypt::wipe(seed, sizeof seed);
        return ok;
    }

    // --- this module's own reach (Shamir, Certificate, Registrar) -------------
    // From a seed; refused while held. The caller still owns (and wipes) seed.
    bool FromSeed(const uint8_t seed[32])
    {
        std::lock_guard<std::mutex> l(mu_);
        if (held_) { ETCS_LOG("Keypair", "refused: a key is already held -- Wipe it first."); return false; }
        std::memcpy(seed_, seed, sizeof seed_);
        uint8_t scratch[32];
        std::memcpy(scratch, seed, sizeof scratch);   // key_pair wipes its seed argument
        etcs_crypt::mc::crypto_ed25519_key_pair(secret_, public_, scratch);
        held_ = true;
        return true;
    }
    bool CopySeed(uint8_t out[32]) const
    {
        std::lock_guard<std::mutex> l(mu_);
        if (!held_) return false;
        std::memcpy(out, seed_, sizeof seed_);
        return true;
    }
    // A secret of this key's for one purpose: BLAKE2b of `msg` keyed by the
    // seed. The seed never leaves; what is derived names its purpose in `msg`.
    bool Derive(const uint8_t* msg, size_t n, uint8_t out[32]) const
    {
        std::lock_guard<std::mutex> l(mu_);
        if (!held_) return false;
        etcs_crypt::mc::crypto_blake2b_keyed(out, 32, seed_, sizeof seed_, msg, n);
        return true;
    }
    bool PublicBytes(uint8_t out[32]) const
    {
        std::lock_guard<std::mutex> l(mu_);
        if (!held_) return false;
        std::memcpy(out, public_, sizeof public_);
        return true;
    }

private:
    // The medium, only if it is a Database that claims Private.
    static Private_* private_database(ETCS::RID db, const char* verb)
    {
        ETCS::Entity* e = etcs_crypt::resolve(db);
        Private_* p = e && e->getInterfacePointer(ETCS::Buffer("Database"))
                    ? static_cast<Private_*>(e->getInterfacePointer(ETCS::Buffer("Private"))) : nullptr;
        if (!p) ETCS_LOG("Keypair", verb << ": refused -- a key is kept only in a Database that claims Private.");
        return p;
    }

    mutable std::mutex mu_;
    bool    held_ = false;
    uint8_t seed_[32]   = {};
    uint8_t secret_[64] = {};
    uint8_t public_[32] = {};
};

#endif // CRYPTPROVIDER_KEYPAIR_H__
