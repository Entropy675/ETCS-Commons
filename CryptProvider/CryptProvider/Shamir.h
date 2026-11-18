#ifndef CRYPTPROVIDER_SHAMIR_H__
#define CRYPTPROVIDER_SHAMIR_H__

#include "Keypair.h"
#include "Shard.h"
#include <mutex>
#include <vector>

/*
 * Shamir -- k-of-n over its own Shard children, one polynomial per byte of a
 * key's 32-byte seed, in GF(2^8) (the AES field, 0x11b). n is how many Shard
 * children it has; share x is the polynomial at x = 1..n, in child order.
 * It holds nothing between calls: the coefficients are drawn and wiped
 * inside Split.
 *
 *   Threshold <k>   2..n, set before Split
 *   Split <keypair> every child filled; refused if any child already holds
 *   Combine <kp>    the first k children that hold one set rebuild an empty
 *                   keypair; refused unless the rebuilt public key IS the set
 */
class Shamir : public DeletableBase<Shamir>
{
public:
    WIRE_TYPE_IDENTITY(Shamir);

    Shamir() = default;

    bool DeleteConcrete() override
    {
        std::string key = getSourceModule().toString() + ":" + getSourceTag().toString();
        return ETCS::DestroyEvent{key.c_str(), this}();
    }

    bool Threshold(int k)
    {
        if (k < 2 || k > 255) return false;
        std::lock_guard<std::mutex> l(mu_);
        k_ = k;
        return true;
    }

    bool Split(ETCS::RID keypair)
    {
        Keypair* kp = etcs_crypt::as<Keypair>(keypair);
        if (!kp) { ETCS_LOG("Shamir", "Split: RID " << keypair << " is not a CryptProvider Keypair."); return false; }
        const std::vector<Shard*> shards = children();
        int k = 0;
        { std::lock_guard<std::mutex> l(mu_); k = k_; }
        const int n = static_cast<int>(shards.size());
        if (k < 2 || n < k || n > 255)
        {
            ETCS_LOG("Shamir", "Split: need 2 <= threshold (" << k << ") <= shards (" << n << ") <= 255.");
            return false;
        }
        for (Shard* s : shards)
            if (s->Holds()) { ETCS_LOG("Shamir", "Split: a shard already holds -- refused."); return false; }

        uint8_t seed[32], pub[32];
        if (!kp->CopySeed(seed) || !kp->PublicBytes(pub)) { ETCS_LOG("Shamir", "Split: the keypair holds no key."); return false; }
        std::vector<uint8_t> coef(static_cast<size_t>(k - 1) * 32);
        bool ok = etcs_crypt::random_bytes(coef.data(), coef.size());
        uint8_t share[32];
        for (int i = 0; ok && i < n; ++i)
        {
            const uint8_t x = static_cast<uint8_t>(i + 1);
            for (int b = 0; b < 32; ++b)
            {
                // Horner, highest coefficient first; the constant term is the seed byte.
                uint8_t y = 0;
                for (int c = k - 2; c >= 0; --c) y = add(mul(y, x), coef[static_cast<size_t>(c) * 32 + b]);
                share[b] = add(mul(y, x), seed[b]);
            }
            ok = shards[static_cast<size_t>(i)]->Fill(pub, k, x, share);
        }
        etcs_crypt::wipe(share, sizeof share);
        etcs_crypt::wipe(coef.data(), coef.size());
        etcs_crypt::wipe(seed, sizeof seed);
        if (!ok)
        {
            for (Shard* s : shards) s->Wipe();
            ETCS_LOG("Shamir", "Split: failed part way -- every shard wiped.");
            return false;
        }
        ETCS_LOG("Shamir", "Split: " << k << " of " << n << " for " << etcs_crypt::hex(pub, 32));
        return true;
    }

    bool Combine(ETCS::RID keypair)
    {
        Keypair* kp = etcs_crypt::as<Keypair>(keypair);
        if (!kp) { ETCS_LOG("Shamir", "Combine: RID " << keypair << " is not a CryptProvider Keypair."); return false; }
        if (kp->Holds()) { ETCS_LOG("Shamir", "Combine: the keypair already holds a key."); return false; }

        struct Point { uint8_t x; uint8_t y[32]; };
        std::vector<Point> pts;
        pts.reserve(255);   // never reallocated: every copy of a share is wiped below
        uint8_t set[32] = {};
        int k = 0;
        for (Shard* s : children())
        {
            Point p{};
            uint8_t sset[32];
            int sk = 0, sx = 0;
            if (!s->Copy(sset, sk, sx, p.y)) continue;
            if (pts.empty()) { std::memcpy(set, sset, 32); k = sk; }
            bool dup = false;
            for (const Point& q : pts) dup = dup || q.x == sx;
            if (std::memcmp(set, sset, 32) != 0 || sk != k || dup) { etcs_crypt::wipe(p.y, 32); continue; }
            p.x = static_cast<uint8_t>(sx);
            pts.push_back(p);
            etcs_crypt::wipe(p.y, 32);
            if (static_cast<int>(pts.size()) == k) break;
        }
        if (k < 2 || static_cast<int>(pts.size()) < k)
        {
            for (Point& p : pts) etcs_crypt::wipe(p.y, 32);
            ETCS_LOG("Shamir", "Combine: " << pts.size() << " share(s) of one set, " << k << " needed.");
            return false;
        }

        // Lagrange at 0: seed = sum_i y_i * prod_{j!=i} x_j / (x_j ^ x_i).
        uint8_t seed[32] = {};
        for (size_t i = 0; i < pts.size(); ++i)
        {
            uint8_t l = 1;
            for (size_t j = 0; j < pts.size(); ++j)
                if (i != j) l = mul(l, mul(pts[j].x, inv(add(pts[j].x, pts[i].x))));
            for (int b = 0; b < 32; ++b) seed[b] = add(seed[b], mul(l, pts[i].y[b]));
        }
        for (Point& p : pts) etcs_crypt::wipe(p.y, 32);

        bool ok = kp->FromSeed(seed);
        etcs_crypt::wipe(seed, sizeof seed);
        uint8_t pub[32];
        if (ok && (!kp->PublicBytes(pub) || std::memcmp(pub, set, 32) != 0))
        {
            kp->Wipe();
            ETCS_LOG("Shamir", "Combine: the shares rebuild a different key -- a share is wrong. Refused.");
            ok = false;
        }
        if (ok) ETCS_LOG("Shamir", "Combine: rebuilt " << etcs_crypt::hex(set, 32) << " from " << k << " shares.");
        return ok;
    }

private:
    std::vector<Shard*> children() const
    {
        std::vector<std::pair<ETCS::Buffer, ETCS::RID>> kids;
        getTypedChildren(kids);
        std::vector<Shard*> out;
        for (auto& [tag, rid] : kids)
            if (Shard* s = dynamic_cast<Shard*>(getTypedChild(tag, rid))) out.push_back(s);
        return out;
    }

    // GF(2^8), branch-free on the operands.
    static uint8_t add(uint8_t a, uint8_t b) { return a ^ b; }
    static uint8_t mul(uint8_t a, uint8_t b)
    {
        uint8_t p = 0;
        for (int i = 0; i < 8; ++i)
        {
            p ^= static_cast<uint8_t>(-(b & 1) & a);
            const uint8_t hi = static_cast<uint8_t>(-(a >> 7));
            a = static_cast<uint8_t>((a << 1) ^ (0x1b & hi));
            b >>= 1;
        }
        return p;
    }
    static uint8_t inv(uint8_t a)   // a^254
    {
        uint8_t r = 1;
        for (int i = 0; i < 7; ++i) { a = mul(a, a); r = mul(r, a); }
        return r;
    }

    mutable std::mutex mu_;
    int k_ = 0;
};

#endif // CRYPTPROVIDER_SHAMIR_H__
