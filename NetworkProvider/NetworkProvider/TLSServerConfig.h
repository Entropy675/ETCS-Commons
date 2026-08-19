#ifndef TLSSERVERCONFIG_H__
#define TLSSERVERCONFIG_H__
#include "../../../ontology.h"
#include <iostream>
#include <cstring>
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/pk.h"
#include "mbedtls/error.h"
#include "mbedtls/psa_util.h"

// TLSServerConfig — the ONE shared, read-only mbedtls_ssl_config a
// ConnectionManager builds ONCE, at the point TLS is enabled on it, and
// hands (by pointer) to every connection's own TLSServerContext
// (TLSServerContext.h). This is the standard mbedTLS pattern: a single
// mbedtls_ssl_config carries the server certificate, private key, and
// negotiation defaults; each in-flight connection gets its own
// mbedtls_ssl_context set up AGAINST that shared config via
// mbedtls_ssl_setup(&ssl_, shared_conf) -- cheap per-connection state,
// expensive per-config state built exactly once.
//
// NOT an Entity. This is plumbing owned directly by ConnectionManager,
// exactly the way ConnectionManager already owns its listen_fd_ and pool_
// as plain members -- there is no reason for a script to address "the TLS
// config" as an independently-listable, RID-addressable thing, and every
// existing precedent for a bare, non-ontology helper type in this module
// (MutableByteSpan in SocketConnectionState.h) already establishes that
// pattern.
//
// LIFETIME: reference-counted, and deliberately so. ConnectionManager
// holds a shared_ptr to the CURRENT config; every TLSServerContext holds
// its own shared_ptr to whichever config its session was set up against
// (TLSServerContext::Init). That is what makes ReloadCerts safe without
// dropping connections: installing a new config only changes what FUTURE
// connections get, while sessions already in flight keep the exact config
// their mbedtls_ssl_context points into until they close, at which point
// the superseded config is destroyed by the last release.
//
// This replaced an earlier raw-pointer arrangement that required "every
// referencing context must be torn down before this is freed" to hold as
// an ordering CONVENTION. Convention is the wrong mechanism for a
// use-after-free involving private key material: the refcount now enforces
// it, so no future call site can get the ordering wrong.
//
// ONE CONFIG, ONE LOAD. A config is built, loaded, and thereafter only
// read -- reloading means constructing a NEW one, never re-loading this
// one (see LoadCertAndKey's own guard for why re-loading in place is not
// merely discouraged but broken).
class TLSServerConfig
{
public:
    // No entropy_context / ctr_drbg_context members, and no
    // mbedtls_ssl_conf_rng call anywhere below. This tree is mbedTLS 4.x
    // (note the tf-psa-crypto include paths in the module Makefile), where
    // randomness is owned by PSA rather than configured per-SSL-config:
    // mbedtls_ssl_conf_rng no longer exists, and mbedtls_pk_parse_keyfile
    // lost its f_rng/p_rng parameters for the same reason. psa_crypto_init()
    // in LoadCertAndKey is what stands in for all of it -- exactly what the
    // client-side MbedTLSContext.h next door already does in its own
    // initContexts(), which is the version-correct precedent in this module.
    TLSServerConfig()
    {
        mbedtls_ssl_config_init(&conf_);
        mbedtls_x509_crt_init(&cert_);
        mbedtls_pk_init(&pkey_);
    }

    ~TLSServerConfig()
    {
        mbedtls_ssl_config_free(&conf_);
        mbedtls_x509_crt_free(&cert_);
        mbedtls_pk_free(&pkey_);
    }

    TLSServerConfig(const TLSServerConfig&)            = delete;
    TLSServerConfig& operator=(const TLSServerConfig&) = delete;

    // Loads the server certificate chain and private key, initialises PSA,
    // and finalizes conf_ as IS_SERVER. (No DRBG seeding -- that went away
    // with the 4.x move to PSA-owned randomness; see the ctor comment.)
    // CALL ONCE PER OBJECT. Refused on a second call, and that is a
    // correctness guard rather than tidiness: mbedtls_x509_crt_parse_file
    // APPENDS to the certificate chain rather than replacing it, so a
    // second load would leave conf_ presenting the old certificate and the
    // new one stapled together, and mbedtls_pk_parse_keyfile over an
    // already-populated context is its own problem. An earlier version of
    // this class described re-loading as "idempotent-ish", which was simply
    // wrong. Reload builds a fresh config instead -- see this class's own
    // LIFETIME note and ConnectionManager::ReloadCerts.
    bool LoadCertAndKey(const std::string& cert_path, const std::string& key_path)
    {
        if (ready_)
        {
            std::cerr << "[TLSServerConfig] LoadCertAndKey called twice on the same "
                         "config -- refusing. Build a new config instead.\n";
            return false;
        }

        // Before any crypto, and unconditionally: PSA is the RNG and key
        // backend in 4.x, so nothing below works without it. Idempotent --
        // a second call once initialised is a no-op success, which matters
        // because MbedTLSContext.h calls it too and either may run first.
        psa_status_t pstat = psa_crypto_init();
        if (pstat != PSA_SUCCESS)
        {
            std::cerr << "[TLSServerConfig] psa_crypto_init failed: " << pstat << "\n";
            return false;
        }

        int ret = mbedtls_x509_crt_parse_file(&cert_, cert_path.c_str());
        if (ret != 0) { logError("x509_crt_parse_file(cert)", ret); return false; }

        // Three arguments in 4.x: the f_rng/p_rng pair this used to take is
        // gone along with the rest of the caller-supplied RNG surface.
        ret = mbedtls_pk_parse_keyfile(&pkey_, key_path.c_str(), nullptr);
        if (ret != 0) { logError("pk_parse_keyfile", ret); return false; }

        // IS_SERVER, not IS_CLIENT -- the one config-level fact
        // MbedTLSContext.h (the client) got backwards for this use, per
        // the carry-forward doc. No VERIFY_REQUIRED / set_hostname here
        // either: those are client-side (verifying the PEER's cert
        // against an expected name); a server presents its own
        // certificate and, absent mutual-TLS, does not verify the client
        // at all -- MBEDTLS_SSL_VERIFY_NONE is the correct default here.
        ret = mbedtls_ssl_config_defaults(&conf_, MBEDTLS_SSL_IS_SERVER,
                                            MBEDTLS_SSL_TRANSPORT_STREAM,
                                            MBEDTLS_SSL_PRESET_DEFAULT);
        if (ret != 0) { logError("ssl_config_defaults", ret); return false; }

        // No mbedtls_ssl_conf_rng here -- see this class's own ctor comment.
        mbedtls_ssl_conf_authmode(&conf_, MBEDTLS_SSL_VERIFY_NONE);

        ret = mbedtls_ssl_conf_own_cert(&conf_, &cert_, &pkey_);
        if (ret != 0) { logError("ssl_conf_own_cert", ret); return false; }

        ready_ = true;
        return true;
    }

    bool IsReady() const { return ready_; }

    // Non-owning access for TLSServerContext::Init -- the config outlives
    // every connection referencing it (see this class's own LIFETIME note).
    mbedtls_ssl_config* Get() { return &conf_; }

private:
    void logError(const char* context, int ret)
    {
        char errbuf[256];
        mbedtls_strerror(ret, errbuf, sizeof(errbuf));
        std::cerr << "[TLSServerConfig] " << context << " failed: " << errbuf << "\n";
    }

    mbedtls_ssl_config       conf_;
    mbedtls_x509_crt         cert_;
    mbedtls_pk_context       pkey_;
    bool                     ready_ = false;
};

#endif // TLSSERVERCONFIG_H__
