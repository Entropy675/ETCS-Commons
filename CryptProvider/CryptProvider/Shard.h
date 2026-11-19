#ifndef CRYPTPROVIDER_SHARD_H__
#define CRYPTPROVIDER_SHARD_H__

#include "Crypt.h"
#include <mutex>
#include <cstdlib>

/*
 * Shard -- one Shamir share of a key's seed. Private, and the one Private
 * thing here that is MEANT to leave: Reveal is its custody form, text a
 * person can copy by hand, and Receive takes that form back.
 *
 *   etcs-shard 1
 *   set <public key, hex>       which key the shares rebuild
 *   threshold <k>
 *   index <x>                   1..255
 *   share <32 bytes, hex>
 *   check <first 8 bytes of SHA-256 over the lines above, hex>
 *
 * A shard that holds refuses Receive: custody never overwrites.
 */
class Shard : public PrivateBase<Shard>, public DeletableBase<Shard>
{
public:
    WIRE_TYPE_IDENTITY(Shard);

    Shard() = default;
    ~Shard() override { WipeConcrete(); }

    // --- Private_ ----------------------------------------------------------------
    bool HoldsConcrete() const override { std::lock_guard<std::mutex> l(mu_); return held_; }
    void WipeConcrete() override
    {
        std::lock_guard<std::mutex> l(mu_);
        etcs_crypt::wipe(share_, sizeof share_);
        std::memset(set_, 0, sizeof set_);
        threshold_ = index_ = 0;
        held_ = false;
    }
    bool RevealConcrete(std::string& out) const override
    {
        std::lock_guard<std::mutex> l(mu_);
        out.clear();
        if (!held_) return false;
        out = body(set_, threshold_, index_, share_);
        uint8_t d[32];
        etcs_crypt::sha256(out.data(), out.size(), d);
        out += "check " + etcs_crypt::hex(d, 8) + "\n";
        return true;
    }
    bool ReceiveConcrete(const std::string& in) override
    {
        // Parsed in place: the only copies of the share made here are the
        // fixed buffers below, each wiped before return.
        uint8_t set[32] = {}, share[32] = {};
        int k = 0, x = 0;
        bool magic = false, has_set = false, has_share = false;
        char check[17] = {};
        auto field = [&](const char* key, size_t at, size_t end, size_t& vat) {
            const size_t n = std::strlen(key);
            if (end - at <= n || in.compare(at, n, key) != 0 || in[at + n] != ' ') return false;
            vat = at + n + 1;
            return true;
        };
        size_t at = 0;
        while (at < in.size())
        {
            size_t next = in.find('\n', at);
            if (next == std::string::npos) next = in.size();
            size_t end = next;
            if (end > at && in[end - 1] == '\r') --end;   // copied through a CRLF editor
            size_t v = 0;
            if (field("etcs-shard", at, end, v))     magic = (end - v == 1 && in[v] == '1');
            else if (field("threshold", at, end, v)) k = std::atoi(in.substr(v, end - v).c_str());
            else if (field("index", at, end, v))     x = std::atoi(in.substr(v, end - v).c_str());
            else if (field("set", at, end, v))       has_set = end - v == 64 && unhexAt(in, v, set);
            else if (field("share", at, end, v))     has_share = end - v == 64 && unhexAt(in, v, share);
            else if (field("check", at, end, v) && end - v == 16) std::memcpy(check, in.data() + v, 16);
            at = next + 1;
        }
        if (!(magic && has_set && has_share && k >= 2 && k <= 255 && x >= 1 && x <= 255))
        {
            ETCS_LOG("Shard", "Receive: not a shard.");
            etcs_crypt::wipe(share, sizeof share);
            return false;
        }
        std::string b = body(set, k, x, share);
        uint8_t d[32];
        etcs_crypt::sha256(b.data(), b.size(), d);
        etcs_crypt::wipe(&b[0], b.size());
        if (etcs_crypt::hex(d, 8) != check)
        {
            ETCS_LOG("Shard", "Receive: check failed -- the share was copied wrong.");
            etcs_crypt::wipe(share, sizeof share);
            return false;
        }
        const bool ok = Fill(set, k, x, share);
        etcs_crypt::wipe(share, sizeof share);
        return ok;
    }

    bool DeleteConcrete() override
    {
        WipeConcrete();
        std::string key = getSourceModule().toString() + ":" + getSourceTag().toString();
        return ETCS::DestroyEvent{key.c_str(), this}();
    }

    // --- reads (none of them the share) ------------------------------------------
    int Index() const     { std::lock_guard<std::mutex> l(mu_); return held_ ? index_ : 0; }
    int Threshold() const { std::lock_guard<std::mutex> l(mu_); return held_ ? threshold_ : 0; }
    std::string Set() const
    {
        std::lock_guard<std::mutex> l(mu_);
        return held_ ? etcs_crypt::hex(set_, sizeof set_) : std::string();
    }

    // --- Shamir's reach ------------------------------------------------------------
    bool Fill(const uint8_t set[32], int k, int x, const uint8_t share[32])
    {
        std::lock_guard<std::mutex> l(mu_);
        if (held_) { ETCS_LOG("Shard", "refused: already holds a share -- custody never overwrites."); return false; }
        std::memcpy(set_, set, sizeof set_);
        std::memcpy(share_, share, sizeof share_);
        threshold_ = k; index_ = x; held_ = true;
        return true;
    }
    bool Copy(uint8_t set[32], int& k, int& x, uint8_t share[32]) const
    {
        std::lock_guard<std::mutex> l(mu_);
        if (!held_) return false;
        std::memcpy(set, set_, sizeof set_);
        std::memcpy(share, share_, sizeof share_);
        k = threshold_; x = index_;
        return true;
    }

private:
    static bool unhexAt(const std::string& s, size_t at, uint8_t out[32])
    {
        auto v = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        for (size_t i = 0; i < 32; ++i)
        {
            const int hi = v(s[at + 2 * i]), lo = v(s[at + 2 * i + 1]);
            if (hi < 0 || lo < 0) return false;
            out[i] = static_cast<uint8_t>(hi << 4 | lo);
        }
        return true;
    }
    static std::string body(const uint8_t set[32], int k, int x, const uint8_t share[32])
    {
        return "etcs-shard 1\nset " + etcs_crypt::hex(set, 32) + "\nthreshold " + std::to_string(k)
             + "\nindex " + std::to_string(x) + "\nshare " + etcs_crypt::hex(share, 32) + "\n";
    }

    mutable std::mutex mu_;
    bool    held_ = false;
    uint8_t set_[32]   = {};
    uint8_t share_[32] = {};
    int     threshold_ = 0, index_ = 0;
};

#endif // CRYPTPROVIDER_SHARD_H__
