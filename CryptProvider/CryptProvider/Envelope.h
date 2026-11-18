#ifndef CRYPTPROVIDER_ENVELOPE_H__
#define CRYPTPROVIDER_ENVELOPE_H__

#include "Keypair.h"
#include <mutex>

/*
 * Envelope -- a Wrapper that seals. What leaves is encrypted and authenticated
 * under a key only another holder of it can open, and a frame that does not
 * open is refused (ETCS::wire_refuse): Seal's place in a chain, with a lock.
 * A child of what it guards, like any stage.
 *
 * NO VERB GIVES IT ITS KEY. The key is this entity's Private child -- a
 * Keypair, or any Private medium that reveals one (DatabaseProvider's
 * Keystore) -- found the way a chain finds its stages. Finding it is the
 * handshake: once, at the first frame, and what it yields is held for the
 * session. An Envelope with no key refuses every frame; it never passes one
 * in the clear.
 *
 *   frame   nonce[24] | mac[16] | ciphertext        XChaCha20-Poly1305
 *   key     BLAKE2b-256("ETCSENV1" | the child's material)
 *
 *   Holds    has the handshake happened
 *   Forget   wipe the session key; the next frame shakes hands again
 */
class Envelope : public WrapperBase<Envelope>, public DeletableBase<Envelope>
{
public:
    WIRE_TYPE_IDENTITY(Envelope);

    Envelope() = default;
    ~Envelope() override { Forget(); }

    void WrapConcrete(ETCS::MBuffer& io, ETCS::SignalContext) override
    {
        uint8_t key[32], head[kHead];
        if (!session(key) || io.written + kHead > ETCS::MBuffer::bufsize
            || !etcs_crypt::random_bytes(head, 24))
        { etcs_crypt::wipe(key, sizeof key); ETCS::wire_refuse(io); return; }
        uint8_t* body = reinterpret_cast<uint8_t*>(io.buf);
        std::memmove(body + kHead, body, io.written);
        etcs_crypt::mc::crypto_aead_lock(body + kHead, head + 24, key, head, nullptr, 0, body + kHead, io.written);
        std::memcpy(body, head, kHead);
        io.written += kHead;
        etcs_crypt::wipe(key, sizeof key);
    }
    void UnwrapConcrete(ETCS::MBuffer& io, ETCS::SignalContext) override
    {
        uint8_t key[32];
        uint8_t* body = reinterpret_cast<uint8_t*>(io.buf);
        const size_t n = io.written >= kHead ? io.written - kHead : 0;
        const bool ok = session(key) && io.written >= kHead
            && etcs_crypt::mc::crypto_aead_unlock(body + kHead, body + 24, key, body, nullptr, 0, body + kHead, n) == 0;
        etcs_crypt::wipe(key, sizeof key);
        if (!ok) { ETCS::wire_refuse(io); return; }
        std::memmove(body, body + kHead, n);
        io.written = n;
        if (n < ETCS::MBuffer::bufsize) io.buf[n] = '\0';
    }
    void CloseConcrete(ETCS::MBuffer&, ETCS::SignalContext) override {}
    ETCS::WireScope ScopeConcrete() const override { return ETCS::WireScope::NetworkOnly; }

    bool DeleteConcrete() override
    {
        Forget();
        std::string key = getSourceModule().toString() + ":" + getSourceTag().toString();
        return ETCS::DestroyEvent{ key.c_str(), this }();
    }

    bool Holds() const { std::lock_guard<std::mutex> l(mu_); return held_; }
    void Forget()
    {
        std::lock_guard<std::mutex> l(mu_);
        etcs_crypt::wipe(key_, sizeof key_);
        held_ = false;
    }

private:
    static constexpr size_t kHead = 24 + 16;

    // The session key, shaking hands first if that has not happened.
    bool session(uint8_t out[32])
    {
        std::lock_guard<std::mutex> l(mu_);
        if (!held_ && !handshake()) return false;
        std::memcpy(out, key_, sizeof key_);
        return true;
    }
    bool handshake()
    {
        std::vector<std::pair<ETCS::Buffer, ETCS::RID>> children;
        getTypedChildren(children);
        for (auto& [tag, rid] : children)
        {
            ETCS::Entity* child = getTypedChild(tag, rid);
            if (!child || !child->hasTag(ETCS::Buffer("Private"))) continue;
            std::string material;
            uint8_t seed[32];
            if (Keypair* kp = dynamic_cast<Keypair*>(child))
            {
                if (kp->CopySeed(seed)) material.assign(reinterpret_cast<const char*>(seed), sizeof seed);
                etcs_crypt::wipe(seed, sizeof seed);
            }
            else if (void* raw = child->getInterfacePointer(ETCS::Buffer("Private")))
                static_cast<Private_*>(raw)->Reveal(material);
            if (material.empty()) continue;
            etcs_crypt::mc::crypto_blake2b_ctx ctx;
            etcs_crypt::mc::crypto_blake2b_init(&ctx, sizeof key_);
            etcs_crypt::mc::crypto_blake2b_update(&ctx, reinterpret_cast<const uint8_t*>("ETCSENV1"), 8);
            etcs_crypt::mc::crypto_blake2b_update(&ctx, reinterpret_cast<const uint8_t*>(material.data()), material.size());
            etcs_crypt::mc::crypto_blake2b_final(&ctx, key_);
            etcs_crypt::wipe(&material[0], material.size());
            held_ = true;
            return true;
        }
        ETCS_LOG("Envelope", "no key: a Private child holds it (a Keypair, a Keystore). Refusing the frame.");
        return false;
    }

    mutable std::mutex mu_;
    bool    held_ = false;
    uint8_t key_[32] = {};
};

#endif // CRYPTPROVIDER_ENVELOPE_H__
