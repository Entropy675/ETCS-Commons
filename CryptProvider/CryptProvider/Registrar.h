#ifndef CRYPTPROVIDER_REGISTRAR_H__
#define CRYPTPROVIDER_REGISTRAR_H__

#include "Certificate.h"

#include <map>
#include <mutex>

/*
 * Registrar -- ACE's half of the chain, handed out on one path of a site: a
 * route (HttpServer.AddRoute, NetworkProvider) that certifies a loader's key
 * as Loader under this registrar's place in ACE's tree.
 *
 *   <mount>/registrar               this registrar's key, hex
 *   <mount>/chain                   its chain up to the root, hex
 *   <mount>/register/<key>/<sig>    a Loader certificate for <key> and the chain above it, hex --
 *                                   if <sig> is <key>'s over sig::registration_statement(<key>, this key)
 *
 *   Use <keypair>     the registrar's key: a Keypair, kept in the site's Keystore
 *   Above <cert>      the next certificate up, in order -- the registrar's own, the active key's, the root's
 *   Mount <segment>   the path's first segment; "pki" unless said
 *
 * A REQUEST IS A PATH, so it fits the route's channel, and its answer is a
 * function of it alone: the same key and signature get the same certificate.
 * Proving the key is the asker's is all that is asked -- a certificate for a
 * key one does not hold signs nothing for anyone.
 */
class Registrar : public DeletableBase<Registrar>
{
public:
    WIRE_TYPE_IDENTITY(Registrar);

    Registrar() = default;

    bool DeleteConcrete() override
    {
        std::string key = getSourceModule().toString() + ":" + getSourceTag().toString();
        return ETCS::DestroyEvent{key.c_str(), this}();
    }

    // --- configuration, before the route is added -----------------------------------
    bool Use(ETCS::RID keypair)
    {
        Keypair* k = etcs_crypt::as<Keypair>(keypair);
        std::lock_guard<std::mutex> l(mu_);
        if (!k || !k->Holds()) { ETCS_LOG("Registrar", "Use: RID " << keypair << " is not a Keypair holding a key."); return false; }
        if (!above_.empty()) { ETCS_LOG("Registrar", "Use: the chain above is already for another key."); return false; }
        keypair_ = keypair;
        k->PublicBytes(pub_.data());
        key_hex_ = etcs_crypt::hex(pub_.data(), pub_.size()) + "\n";
        return true;
    }

    bool Above(ETCS::RID cert)
    {
        Certificate* c = etcs_crypt::as<Certificate>(cert);
        const std::string bytes = c ? c->Bytes() : std::string();
        ETCS::sig::Certificate x;
        size_t used = 0;
        if (bytes.empty() || !ETCS::sig::decode(bytes.data(), bytes.size(), x, &used) || !c->Verify(0))
        { ETCS_LOG("Registrar", "Above: RID " << cert << " is not an issued certificate."); return false; }
        std::lock_guard<std::mutex> l(mu_);
        if (keypair_ == 0) { ETCS_LOG("Registrar", "Above: Use a key first."); return false; }
        const ETCS::sig::Key&  below      = above_.empty() ? pub_ : above_.back().issuer;
        const ETCS::sig::Role  below_role = above_.empty() ? ETCS::sig::Role::Loader : above_.back().role;
        if (x.subject != below || !ETCS::sig::may_issue(x.role, below_role))
        {
            ETCS_LOG("Registrar", "Above: a " << ETCS::sig::role_name(x.role) << " certificate for "
                     << etcs_crypt::hex(x.subject.data(), 32) << " does not certify what is below it.");
            return false;
        }
        above_.push_back(x);
        const std::string chain = ETCS::sig::encode_chain(above_);
        chain_hex_ = etcs_crypt::hex(reinterpret_cast<const uint8_t*>(chain.data()), chain.size()) + "\n";
        return true;
    }

    bool Mount(const std::string& segment)
    {
        if (segment.empty() || segment.find('/') != std::string::npos) return false;
        std::lock_guard<std::mutex> l(mu_);
        mount_ = segment;
        return true;
    }

    std::string Key() const { std::lock_guard<std::mutex> l(mu_); return keypair_ ? key_hex_.substr(0, 64) : std::string(); }

    // --- the route -------------------------------------------------------------------
    // Filter: claims the paths under the mount, as ForumNode does.
    void Accepts(ETCS::Buffer& io) const
    {
        const std::string path = io.restAsString();
        std::string mount;
        { std::lock_guard<std::mutex> l(mu_); mount = mount_; }
        size_t i = 0;
        while (i < path.size() && path[i] == '/') ++i;
        size_t j = path.find('/', i);
        if (j == std::string::npos) j = path.size();
        io.reset();
        if (path.compare(i, j - i, mount) == 0) io.writeString(mount.c_str());
    }

    void Request(ETCS::Buffer& io)
    {
        const std::string path = io.restAsString();
        std::vector<std::string> seg;
        for (size_t at = 0; at <= path.size(); )
        {
            size_t end = path.find('/', at);
            if (end == std::string::npos) end = path.size();
            if (end > at) seg.push_back(path.substr(at, end - at));
            at = end + 1;
        }
        std::lock_guard<std::mutex> l(mu_);
        if (seg.empty() || seg[0] != mount_) return inline_answer(io, "no such path\n");
        if (keypair_ == 0 || above_.empty() || above_.back().role != ETCS::sig::Role::Root)
            return inline_answer(io, "this registrar is not set up: Use a key, then Above up to the root\n");
        if (seg.size() == 2 && seg[1] == "registrar") return inline_answer(io, key_hex_);
        if (seg.size() == 2 && seg[1] == "chain") return by_reference(io, chain_hex_);
        if (seg.size() == 4 && seg[1] == "register") return by_reference(io, registered(seg[2], seg[3]));
        inline_answer(io, "no such path\n");
    }

private:
    // A Loader certificate for `key_hex` and the chain above, hex; or why not.
    const std::string& registered(const std::string& key_hex, const std::string& sig_hex)
    {
        auto it = issued_.find(key_hex + sig_hex);
        if (it != issued_.end()) return it->second;
        static const std::string refused = "refused: <key>/<signature> is not a key's request of this registrar\n";
        static const std::string unkeyed = "refused: the registrar's key is gone\n";
        ETCS::sig::Key key{};
        uint8_t s[ETCS::sig::kSigBytes];
        if (!etcs_crypt::unhex(key_hex, key.data(), key.size()) || !etcs_crypt::unhex(sig_hex, s, sizeof s)) return refused;
        const std::string m = ETCS::sig::registration_statement(key, pub_);
        if (etcs_crypt::mc::crypto_ed25519_check(s, key.data(), reinterpret_cast<const uint8_t*>(m.data()), m.size()) != 0)
            return refused;
        Keypair* k = etcs_crypt::as<Keypair>(keypair_);
        if (!k || !k->Holds()) return unkeyed;
        ETCS::sig::Certificate c;
        c.role = ETCS::sig::Role::Loader; c.subject = key; c.issuer = pub_;
        const std::string part = ETCS::sig::signed_part(c);
        k->Sign(reinterpret_cast<const uint8_t*>(part.data()), part.size(), c.signature);
        std::vector<ETCS::sig::Certificate> chain{ c };
        chain.insert(chain.end(), above_.begin(), above_.end());
        const std::string bytes = ETCS::sig::encode_chain(chain);
        ETCS_LOG("Registrar", "certified " << key_hex << " as loader");
        return issued_[key_hex + sig_hex] = etcs_crypt::hex(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()) + "\n";
    }

    static void inline_answer(ETCS::Buffer& io, const std::string& s) { io.reset(); io.writeString(s.c_str()); }

    // The frame NetworkProvider's RouteRef reads (RouteRequest.h): a magic word
    // with a NUL, the bytes' address and length, a mime type. The bytes are this
    // entity's and are never changed once answered, so they outlive the send.
    static void by_reference(ETCS::Buffer& io, const std::string& s)
    {
        io.reset();
        const uint64_t p = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(s.data()));
        const uint64_t n = s.size();
        char mime[40] = "text/plain";
        io.writeRaw("ETCSREF\0", 8);
        io.writeRaw(&p, sizeof p);
        io.writeRaw(&n, sizeof n);
        io.writeRaw(mime, sizeof mime);
    }

    mutable std::mutex                  mu_;
    ETCS::RID                           keypair_ = 0;
    ETCS::sig::Key                      pub_{};
    std::vector<ETCS::sig::Certificate> above_;
    std::string                         mount_ = "pki";
    std::string                         key_hex_, chain_hex_;
    std::map<std::string, std::string>  issued_;    // never erased: a reference answer points into it
};

#endif // CRYPTPROVIDER_REGISTRAR_H__
