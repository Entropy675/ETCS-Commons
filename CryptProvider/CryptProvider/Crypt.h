#ifndef CRYPTPROVIDER_CRYPT_H__
#define CRYPTPROVIDER_CRYPT_H__

#include "../../../core_defs.h"
#include "../../../ontology.h"
#include "../../../libs/monocypher.h"

#include <cstdint>
#include <cstring>
#include <string>

#if defined(__EMSCRIPTEN__)
#include <unistd.h>
#elif defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#elif defined(__linux__)
#include <sys/random.h>
#include <cerrno>
#endif

/*
 * What every CryptProvider type shares: the operating system's randomness,
 * RFC 8032 Ed25519 (crypto_ed25519_*, SHA-512 -- not crypto_eddsa_*, which is
 * Monocypher's BLAKE2b EdDSA and verifies against nothing else), hex, and
 * resolving a RID to this module's own type.
 */
namespace etcs_crypt
{
namespace mc = etcs_monocypher;

// Fills n bytes or says it could not; never a partial fill.
inline bool random_bytes(uint8_t* out, size_t n)
{
#if defined(__EMSCRIPTEN__)
    // getentropy is crypto.getRandomValues underneath; 256 bytes a call.
    for (size_t at = 0; at < n; at += 256)
        if (::getentropy(out + at, n - at < 256 ? n - at : 256) != 0) return false;
    return true;
#elif defined(__linux__)
    size_t at = 0;
    while (at < n)
    {
        const ssize_t r = ::getrandom(out + at, n - at, 0);
        if (r < 0) { if (errno == EINTR) continue; return false; }
        at += static_cast<size_t>(r);
    }
    return true;
#elif defined(_WIN32)
    return BCryptGenRandom(nullptr, out, static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    (void)out; (void)n;
    return false;
#endif
}

inline void wipe(void* p, size_t n) { mc::crypto_wipe(p, n); }

inline std::string hex(const uint8_t* p, size_t n)
{
    static const char* d = "0123456789abcdef";
    std::string out(n * 2, '0');
    for (size_t i = 0; i < n; ++i) { out[2 * i] = d[p[i] >> 4]; out[2 * i + 1] = d[p[i] & 15]; }
    return out;
}
inline bool unhex(const std::string& s, uint8_t* out, size_t n)
{
    if (s.size() != n * 2) return false;
    auto v = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < n; ++i)
    {
        const int hi = v(s[2 * i]), lo = v(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<uint8_t>(hi << 4 | lo);
    }
    return true;
}

inline void sha256(const void* p, size_t n, uint8_t out[32])
{
    picohash_ctx_t ctx;
    picohash_init_sha256(&ctx);
    picohash_update(&ctx, p, n);
    picohash_final(&ctx, out);
}

inline ETCS::Entity* resolve(ETCS::RID rid)
{
    return rid ? ETCS::etcs_resolve_rid_anywhere(&ETCS::getLoader(), rid) : nullptr;
}

// A RID that names one of this module's own types, as that type.
template <typename T>
inline T* as(ETCS::RID rid) { return dynamic_cast<T*>(resolve(rid)); }

} // namespace etcs_crypt

#endif // CRYPTPROVIDER_CRYPT_H__
