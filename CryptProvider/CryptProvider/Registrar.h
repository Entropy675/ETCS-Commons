#ifndef CRYPTPROVIDER_REGISTRAR_H__
#define CRYPTPROVIDER_REGISTRAR_H__

#include "Certificate.h"
#include "../../NetworkProvider/NetworkProvider/RouteRequest.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

/*
 * Registrar -- ACE's half of the chain, handed out on one path of a site: a
 * request route (HttpServer.AddRequestRoute, NetworkProvider) that joins a
 * loader's key to ACE's tree through that key's ACE identity, and keeps the
 * accounts those identities are gathered in.
 *
 *   GET  <mount>/registrar     this registrar's key, hex
 *   GET  <mount>/chain         its chain up to the root, hex
 *   POST <mount>/register      "<key> <head> <epoch> <sig>": the key certified by its ACE identity,
 *                              that identity by this key for <epoch>, and the chain above, hex
 *   POST <mount>/admit         "<key> <sig> <member> <sig>": the key's Join word and a member's
 *                              Admit word; the key joins that member's account, at its next
 *                              registration if it holds one now
 *   POST <mount>/retire        "<key> <member> <sig>": a member's word retiring a key of its account
 *
 * THE PORTAL, for a browser, which holds no key: a member's loader lends it
 * the member's authority for a while (sig::portal_statement), and the page
 * holds a token no one else has seen.
 *
 *   POST <mount>/portal/open      "" -> "<code> <token>": the code is shown, the token kept
 *   POST <mount>/portal/signin    "<code> <member> <sig>": the member's portal word on that code
 *   POST <mount>/portal/state     "<token>" -> "pending", or "signed in <member>"
 *   POST <mount>/portal/members   "<token>" -> a line per key of the account:
 *                                 "<key> current|retired <epoch|-> [you] [moving]"
 *   POST <mount>/portal/admit     "<token> <key> <sig>": the key's Join word, naming the member;
 *                                 the session stands for the member's Admit word
 *   POST <mount>/portal/retire    "<token> <key>": the session stands for the member's word
 *   POST <mount>/portal/close     "<token>"
 *
 * A session ends when it closes, when its member is retired, or after
 * SessionLife seconds -- five minutes at most while nobody has signed in. A
 * code is good once. A full table drops its oldest, and a member's ninth
 * session its first.
 *
 * A word is the loader's answer to its TrustEvent (core/Trust.h), sent as it
 * came: two keys and two signatures are past what a path carries (256 bytes).
 *
 *   Use <keypair>       the registrar's key: a Keypair, kept in the site's Keystore
 *   Above <cert>        the next certificate up, in order -- the registrar's own, the active key's, the root's
 *   Mount <segment>     the path's first segment; "pki" unless said
 *   Derive <keypair>    the secret ACE identities and member rows are derived under: a Keypair, kept apart
 *   Accounts <db>       where accounts are kept (a Database), after Derive
 *   Head <hex>          an identity a registering loader may be built for
 *   Epoch <hex>         an epoch it may run in
 *   Current             this loader's own identity and epoch, as a Head and an Epoch
 *   Window <n>          how many epochs it serves, the newest it has been given; 2 unless said,
 *                       so the loaders of the last epoch keep registering while they heal
 *   SessionLife <s>     how long a portal session lasts; 900 unless said
 *
 * A registration is a function of its words alone while the key is a current
 * member.
 *
 * AN ACE IDENTITY IS DERIVED, NOT KEPT: K(L) is the key whose seed is the
 * secret's BLAKE2b of L, one for one, the same every time it is asked. What is
 * kept is a row per member, named by another BLAKE2b of L under the same
 * secret, so without the secret the rows say nothing of which public keys
 * share an account. Each row's key is kept sealed under the same secret,
 * for the portal to list. The epoch on K's certificate is the registration's
 * lifetime: a retired key's certificate holds to the end of its epoch, and the
 * registrar issues it no other.
 *
 * AN ACCOUNT HOLDS STILL FOR AN EPOCH. What a live certificate stands for
 * never changes under it: a key admitted to another account while it holds a
 * registration moves at its next registration in a later epoch -- which its
 * loader asks for itself, on its own word, as it heals -- and a key holding
 * none moves at once. A retirement is refused at the next registration.
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
        if (keypair == derive_) { ETCS_LOG("Registrar", "Use: that key is the secret identities are derived under."); return false; }
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
        const ETCS::sig::Role  below_role = above_.empty() ? ETCS::sig::Role::Identity : above_.back().role;
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

    bool Derive(ETCS::RID keypair)
    {
        Keypair* k = etcs_crypt::as<Keypair>(keypair);
        std::lock_guard<std::mutex> l(mu_);
        if (!k || !k->Holds()) { ETCS_LOG("Registrar", "Derive: RID " << keypair << " is not a Keypair holding a key."); return false; }
        if (keypair == keypair_) { ETCS_LOG("Registrar", "Derive: the registrar's own key signs; identities are derived under another."); return false; }
        if (db_) { ETCS_LOG("Registrar", "Derive: the accounts are already kept under a secret."); return false; }
        derive_ = keypair;
        return true;
    }

    // The accounts, read whole; every change after is written through. A check
    // of the secret is kept with them, so the rows are never read under another.
    bool Accounts(ETCS::RID database)
    {
        std::lock_guard<std::mutex> l(mu_);
        Keypair* k = etcs_crypt::as<Keypair>(derive_);
        if (!k || !k->Holds()) { ETCS_LOG("Registrar", "Accounts: Derive first."); return false; }
        if (db_) { ETCS_LOG("Registrar", "Accounts: already kept."); return false; }
        ETCS::Held<Database_> db = ETCS::resolve_held<Database_>("Database", database);
        if (!db) { ETCS_LOG("Registrar", "Accounts: no Database at RID:" << database << " -- Connect it first."); return false; }
        static const char* const schema[] = {
            "CREATE TABLE IF NOT EXISTS ace_secret(check_value BLOB NOT NULL)",
            "CREATE TABLE IF NOT EXISTS ace_members(member BLOB PRIMARY KEY, account BLOB NOT NULL,"
            " retired INTEGER NOT NULL DEFAULT 0, epoch BLOB, next BLOB)",
            "CREATE TABLE IF NOT EXISTS ace_epochs(epoch BLOB PRIMARY KEY, seen INTEGER NOT NULL)",
            "CREATE TABLE IF NOT EXISTS ace_sealed(member BLOB PRIMARY KEY, sealed BLOB NOT NULL)",
        };
        for (const char* sql : schema)
        {
            Stmt st(*db, sql);
            if (!st || st.step() < 0) { ETCS_LOG("Registrar", "Accounts: schema failed -- see the database's log."); return false; }
        }
        Bytes check{};
        static const uint8_t tag[8] = { 'E', 'T', 'C', 'S', 'C', 'H', 'K', '1' };
        k->Derive(tag, sizeof tag, check.data());
        {
            Stmt st(*db, "SELECT check_value FROM ace_secret");
            const int r = st ? st.step() : -1;
            if (r < 0) return false;
            if (r == 1)
            {
                const DatabaseValue v = st.col(0);
                if (v.kind != DatabaseValue::Blob || v.n != 32 || std::memcmp(v.p, check.data(), 32) != 0)
                { ETCS_LOG("Registrar", "Accounts: these were kept under another secret; refused."); return false; }
            }
            else
            {
                Stmt put(*db, "INSERT INTO ace_secret(check_value) VALUES (?1)");
                if (!put || !put.bind(1, DatabaseValue::blob(check.data(), 32)) || put.step() < 0) return false;
            }
        }
        std::map<Bytes, Row> rows;
        Stmt st(*db, "SELECT member, account, retired, epoch, next FROM ace_members");
        if (!st) return false;
        auto bytes = [](const DatabaseValue& v, Bytes& out) {
            if (v.kind != DatabaseValue::Blob || v.n != 32) return false;
            std::memcpy(out.data(), v.p, 32);
            return true;
        };
        for (int r; (r = st.step()) != 0; )
        {
            if (r < 0) return false;
            Bytes mb{};
            Row row;
            if (!bytes(st.col(0), mb) || !bytes(st.col(1), row.account)) continue;
            row.retired = st.col(2).i != 0;
            bytes(st.col(3), row.epoch);
            bytes(st.col(4), row.next);
            rows[mb] = row;
        }
        std::map<Bytes, Sealed> sealed;
        Stmt se(*db, "SELECT member, sealed FROM ace_sealed");
        for (int r; se && (r = se.step()) == 1; )
        {
            const DatabaseValue m = se.col(0), v = se.col(1);
            Bytes mb{};
            Sealed sv{};
            if (!bytes(m, mb) || v.kind != DatabaseValue::Blob || v.n != sv.size()) continue;
            std::memcpy(sv.data(), v.p, sv.size());
            sealed[mb] = sv;
        }
        sealed_ = std::move(sealed);
        std::vector<Bytes> given;
        Stmt ep(*db, "SELECT epoch FROM ace_epochs ORDER BY seen");
        for (int r; ep && (r = ep.step()) == 1; )
        {
            const DatabaseValue v = ep.col(0);
            if (v.kind != DatabaseValue::Blob || v.n != 32) continue;
            Bytes e{};
            std::memcpy(e.data(), v.p, 32);
            given.push_back(e);
        }
        for (const Bytes& e : given_) given.erase(std::remove(given.begin(), given.end(), e), given.end());
        given.insert(given.end(), given_.begin(), given_.end());
        given_ = std::move(given);
        members_ = std::move(rows);
        current_.clear();
        for (const auto& [m, r] : members_) if (!r.retired) ++current_[r.account];
        db_ = database;
        for (const Bytes& e : given_) keep_epoch(*db, e);
        serve();
        ETCS_LOG("Registrar", "accounts: " << members_.size() << " member row(s), " << current_.size() << " account(s)");
        return true;
    }

    bool Head(const std::string& hex)  { return accept(hex, heads_, "Head"); }
    bool Epoch(const std::string& hex)
    {
        Bytes e{};
        if (!etcs_crypt::unhex(hex, e.data(), 32)) { ETCS_LOG("Registrar", "Epoch: not 32 bytes of hex."); return false; }
        std::lock_guard<std::mutex> l(mu_);
        given_.erase(std::remove(given_.begin(), given_.end(), e), given_.end());
        given_.push_back(e);
        if (db_)
        {
            ETCS::Held<Database_> db = ETCS::resolve_held<Database_>("Database", db_);
            if (!db || !keep_epoch(*db, e)) return false;
        }
        serve();
        return true;
    }

    bool Window(int64_t n)
    {
        if (n < 1) return false;
        std::lock_guard<std::mutex> l(mu_);
        window_ = static_cast<size_t>(n);
        serve();
        return true;
    }

    bool SessionLife(int64_t seconds)
    {
        if (seconds < 1) return false;
        std::lock_guard<std::mutex> l(mu_);
        life_ = std::chrono::seconds(seconds);
        return true;
    }

    bool Current()
    {
        ETCS::TrustEvent who(ETCS::TrustEvent::Op::Identify);
        if (!who()) { ETCS_LOG("Registrar", "Current: " << who.out); return false; }
        const size_t sp = who.out.find(' ');
        if (sp == std::string::npos) return false;
        const std::string head = who.out.substr(0, sp), epoch = who.out.substr(sp + 1);
        if (head == std::string(64, '0')) { ETCS_LOG("Registrar", "Current: no identity is built into this loader."); return false; }
        return Head(head) && Epoch(epoch);
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

    // The request's address, as AddRequestRoute hands it; anything else is a
    // path-style registration, and is told so.
    void Request(ETCS::Buffer& io)
    {
        uint64_t p = 0;
        if (io.written != sizeof p) return inline_answer(io, "refused: a registrar is added with HttpServer.AddRequestRoute\n");
        std::memcpy(&p, io.buf, sizeof p);
        const RouteRequest& req = *reinterpret_cast<const RouteRequest*>(static_cast<uintptr_t>(p));
        const std::vector<std::string>& seg = req.seg;
        std::lock_guard<std::mutex> l(mu_);
        if (seg.empty() || seg[0] != mount_) return inline_answer(io, "no such path\n");
        if (keypair_ == 0 || above_.empty() || above_.back().role != ETCS::sig::Role::Root)
            return inline_answer(io, "this registrar is not set up: Use a key, then Above up to the root\n");
        if (seg.size() == 2 && seg[1] == "registrar") return inline_answer(io, key_hex_);
        if (seg.size() == 2 && seg[1] == "chain") return by_reference(io, chain_hex_);
        const bool portal = seg.size() == 3 && seg[1] == "portal";
        if (!portal && (seg.size() < 2 || (seg[1] != "register" && seg[1] != "admit" && seg[1] != "retire")))
            return inline_answer(io, "no such path\n");
        if (!req.posted() || (!portal && seg.size() != 2))
            return inline_answer(io, "refused: a word is POSTed to " + mount_ + "/" + seg[1] + ", as the loader said it\n");
        if (!db_ || heads_.empty() || epochs_.empty())
            return inline_answer(io, "this registrar is not set up for accounts: Derive, Accounts, then Current or Head and Epoch\n");
        std::istringstream body(req.bodyString());
        std::vector<std::string> w;
        for (std::string t; body >> t && w.size() < 5; ) w.push_back(t);
        if (portal)
        {
            req.reply = portal_answer(seg[2], w);
            return by_reference(io, req.reply);
        }
        if (seg[1] == "register" && w.size() == 4)
        {
            req.reply = registered(w[0], w[1], w[2], w[3]);
            return by_reference(io, req.reply);
        }
        if (seg[1] == "admit" && w.size() == 4) return inline_answer(io, admitted(w[2], w[0], w[3], w[1]));
        if (seg[1] == "retire" && w.size() == 3) return inline_answer(io, retired(w[1], w[0], w[2]));
        inline_answer(io, "refused: not the words " + seg[1] + " takes\n");
    }

private:
    using Bytes = std::array<uint8_t, 32>;
    // `epoch` is the last it was registered in; `next`, an account it moves to at its next.
    struct Row { Bytes account{}; bool retired = false; Bytes epoch{}; Bytes next{}; };
    using Sealed = std::array<uint8_t, 16 + 32>;           // a key under the secret's seal: mac, then the key
    using Clock = std::chrono::steady_clock;
    struct Session { Bytes code{}; ETCS::sig::Key member{}; bool signed_in = false; Clock::time_point until{}; };

    // The ACE identity's certificate for this epoch, its word on the loader's
    // key, and the chain above. A key no account holds begins one.
    const std::string& registered(const std::string& key_hex, const std::string& head_hex,
                                  const std::string& epoch_hex, const std::string& sig_hex)
    {
        static const std::string refused  = "refused: not a loader key's registration with this registrar\n";
        static const std::string outsider = "refused: a loader of an identity or epoch this registrar does not serve\n";
        static const std::string gone     = "refused: this key was retired from its account\n";
        static const std::string unkeyed  = "refused: the registrar's key or secret is gone\n";
        static const std::string unkept   = "refused: the account could not be kept\n";
        ETCS::sig::Key key{}, head{};
        ETCS::sig::Epoch epoch{};
        uint8_t s[ETCS::sig::kSigBytes];
        if (!etcs_crypt::unhex(key_hex, key.data(), 32) || !etcs_crypt::unhex(head_hex, head.data(), 32)
            || !etcs_crypt::unhex(epoch_hex, epoch.data(), 32) || !etcs_crypt::unhex(sig_hex, s, sizeof s)) return refused;
        if (!heads_.count(head) || !epochs_.count(epoch)) return outsider;
        Bytes idx{};
        if (!derive("ETCSMEM1", key, idx.data())) return unkeyed;
        // Only the key's own word is told it was retired: its standing is no one else's to ask.
        const auto row = members_.find(idx);
        const bool retired = row != members_.end() && row->second.retired;
        const std::string words = key_hex + head_hex + epoch_hex + sig_hex;
        const auto it = issued_.find(words);
        if (it != issued_.end() && !retired) return it->second;
        if (!verify(ETCS::sig::registration_statement(key, pub_, head, epoch), key, s)) return refused;
        if (retired) return gone;

        Keypair* k = etcs_crypt::as<Keypair>(keypair_);
        uint8_t seed[32], secret[64];
        ETCS::sig::Certificate ident, leaf;
        if (!k || !k->Holds() || !derive("ETCSACE1", key, seed)) return unkeyed;
        etcs_crypt::mc::crypto_ed25519_key_pair(secret, ident.subject.data(), seed);   // wipes seed
        ident.role = ETCS::sig::Role::Identity; ident.issuer = pub_; ident.epochs = { epoch };
        const std::string ipart = ETCS::sig::signed_part(ident);
        k->Sign(reinterpret_cast<const uint8_t*>(ipart.data()), ipart.size(), ident.signature);
        leaf.role = ETCS::sig::Role::Loader; leaf.subject = key; leaf.issuer = ident.subject;
        const std::string lpart = ETCS::sig::signed_part(leaf);
        etcs_crypt::mc::crypto_ed25519_sign(leaf.signature, secret, reinterpret_cast<const uint8_t*>(lpart.data()), lpart.size());
        etcs_crypt::wipe(secret, sizeof secret);

        Row r;
        const bool fresh = row == members_.end();
        if (fresh && !etcs_crypt::random_bytes(r.account.data(), 32)) return unkept;
        if (!fresh) r = row->second;
        // Only a registration in a later epoch than its last moves the row: one
        // asked again in an epoch still served -- by a loader kept back, or the
        // words said again -- changes nothing its newest certificate stands for.
        const bool later = place(epoch) > place(r.epoch);
        if (r.next != Bytes{} && later)
        {
            if (current_[r.account] > 1) ETCS_LOG("Registrar", "a move was dropped: the key came to share its account meanwhile");
            else r.account = r.next;
            r.next = Bytes{};
        }
        if (later) r.epoch = epoch;
        if ((fresh || later) && !keep(idx, r, key)) return unkept;
        std::vector<ETCS::sig::Certificate> chain{ leaf, ident };
        chain.insert(chain.end(), above_.begin(), above_.end());
        const std::string bytes = ETCS::sig::encode_chain(chain);
        ETCS_LOG("Registrar", "registered " << key_hex << " under ACE identity " << etcs_crypt::hex(ident.subject.data(), 32)
                 << (fresh ? ", a new account" : ""));
        if (issued_.size() >= kIssued) issued_.clear();
        return issued_[words] = etcs_crypt::hex(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()) + "\n";
    }

    // Where an epoch stands among those given: later is larger; 0 for none of them.
    size_t place(const Bytes& e) const
    {
        for (size_t i = given_.size(); i-- > 0; ) if (given_[i] == e) return i + 1;
        return 0;
    }

    // The member's word and the joining key's, each naming the other and one
    // epoch this registrar serves. A key sharing another account does not move.
    std::string admitted(const std::string& member_hex, const std::string& key_hex,
                         const std::string& msig_hex, const std::string& ksig_hex)
    {
        ETCS::sig::Key member{}, key{};
        uint8_t ms[ETCS::sig::kSigBytes], ks[ETCS::sig::kSigBytes];
        if (!etcs_crypt::unhex(member_hex, member.data(), 32) || !etcs_crypt::unhex(key_hex, key.data(), 32)
            || !etcs_crypt::unhex(msig_hex, ms, sizeof ms) || !etcs_crypt::unhex(ksig_hex, ks, sizeof ks) || member == key)
            return "refused: not an admission\n";
        bool said = false, agreed = false;
        for (const Bytes& e : epochs_)
            if (verify(ETCS::sig::admit_statement(member, key, pub_, e), member, ms))
            {
                said = true;
                agreed = verify(ETCS::sig::join_statement(key, member, pub_, e), key, ks);
                break;
            }
        if (!said) return "refused: the member's word does not verify, in an epoch this registrar serves\n";
        if (!agreed) return "refused: the joining key's word does not verify; it must agree to join, in the same epoch\n";
        return admit(member, key);
    }

    // `key` into `member`'s account: now if it holds no live registration,
    // else at its next one.
    std::string admit(const ETCS::sig::Key& member, const ETCS::sig::Key& key)
    {
        Bytes mi{}, ki{};
        if (!derive("ETCSMEM1", member, mi.data()) || !derive("ETCSMEM1", key, ki.data())) return "refused: the registrar's secret is gone\n";
        const auto m = members_.find(mi);
        if (m == members_.end() || m->second.retired) return "refused: the admitting key is not a current member\n";
        const Bytes account = m->second.account;
        const auto k = members_.find(ki);
        Row r;
        r.account = account;
        if (k != members_.end())
        {
            r = k->second;
            if (r.retired) return "refused: that key was retired, and comes back to no account\n";
            if (r.account == account) { r.next = Bytes{}; return keep(ki, r, key) ? "admitted\n" : "refused: the account could not be kept\n"; }
            if (current_[r.account] > 1) return "refused: that key shares another account; a member there retires it first\n";
            if (epochs_.count(r.epoch))
            {
                r.next = account;
                if (!keep(ki, r, key)) return "refused: the account could not be kept\n";
                ETCS_LOG("Registrar", "a key moves to an account at its next registration");
                return "admitted at its next registration\n";
            }
            r.account = account;
            r.next = Bytes{};
        }
        if (!keep(ki, r, key)) return "refused: the account could not be kept\n";
        ETCS_LOG("Registrar", "admitted a key to an account of " << current_[account]);
        return "admitted\n";
    }

    std::string retired(const std::string& member_hex, const std::string& key_hex, const std::string& sig_hex)
    {
        ETCS::sig::Key member{}, key{};
        uint8_t s[ETCS::sig::kSigBytes];
        if (!etcs_crypt::unhex(member_hex, member.data(), 32) || !etcs_crypt::unhex(key_hex, key.data(), 32)
            || !etcs_crypt::unhex(sig_hex, s, sizeof s))
            return "refused: not a retirement\n";
        if (!verify(ETCS::sig::retire_statement(member, key, pub_), member, s)) return "refused: the member's word does not verify\n";
        return retire(member, key);
    }

    std::string retire(const ETCS::sig::Key& member, const ETCS::sig::Key& key)
    {
        Bytes mi{}, ki{};
        if (!derive("ETCSMEM1", member, mi.data()) || !derive("ETCSMEM1", key, ki.data())) return "refused: the registrar's secret is gone\n";
        const auto m = members_.find(mi), k = members_.find(ki);
        if (m == members_.end() || k == members_.end() || m->second.account != k->second.account)
            return "refused: not keys of one account\n";
        if (k->second.retired) return "retired\n";
        if (m->second.retired) return "refused: the retiring key is not a current member\n";
        Row r = k->second;
        r.retired = true;
        r.next = Bytes{};
        if (!keep(ki, r, key)) return "refused: the account could not be kept\n";
        ETCS_LOG("Registrar", "retired a key; its account has " << current_[k->second.account] << " left");
        return "retired\n";
    }

    // The secret's BLAKE2b of a purpose and a key.
    bool derive(const char tag[9], const ETCS::sig::Key& key, uint8_t out[32]) const
    {
        Keypair* d = etcs_crypt::as<Keypair>(derive_);
        uint8_t msg[8 + 32];
        std::memcpy(msg, tag, 8);
        std::memcpy(msg + 8, key.data(), 32);
        return d && d->Derive(msg, sizeof msg, out);
    }

    static bool verify(const std::string& m, const ETCS::sig::Key& by, const uint8_t s[ETCS::sig::kSigBytes])
    {
        return etcs_crypt::mc::crypto_ed25519_check(s, by.data(), reinterpret_cast<const uint8_t*>(m.data()), m.size()) == 0;
    }

    // A member row, written through before it is held, and its key sealed
    // beside it the first time.
    bool keep(const Bytes& idx, const Row& row, const ETCS::sig::Key& key)
    {
        ETCS::Held<Database_> db = ETCS::resolve_held<Database_>("Database", db_);
        if (!db) { ETCS_LOG("Registrar", "the accounts' database is gone."); return false; }
        if (!sealed_.count(idx))
        {
            Sealed sv{};
            if (!seal(idx, key, sv)) return false;
            Stmt put(*db, "INSERT OR REPLACE INTO ace_sealed(member, sealed) VALUES (?1, ?2)");
            if (!put || !put.bind(1, DatabaseValue::blob(idx.data(), 32)) || !put.bind(2, DatabaseValue::blob(sv.data(), sv.size()))
                || put.step() < 0)
                return false;
            sealed_[idx] = sv;
        }
        Stmt st(*db, "INSERT OR REPLACE INTO ace_members(member, account, retired, epoch, next) VALUES (?1, ?2, ?3, ?4, ?5)");
        auto blob = [](const Bytes& b) { return b == Bytes{} ? DatabaseValue{} : DatabaseValue::blob(b.data(), 32); };
        if (!st || !st.bind(1, DatabaseValue::blob(idx.data(), 32)) || !st.bind(2, DatabaseValue::blob(row.account.data(), 32))
            || !st.bind(3, DatabaseValue::integer(row.retired ? 1 : 0)) || !st.bind(4, blob(row.epoch))
            || !st.bind(5, blob(row.next)) || st.step() < 0)
            return false;
        const auto was = members_.find(idx);
        if (was != members_.end() && !was->second.retired) --current_[was->second.account];
        if (!row.retired) ++current_[row.account];
        members_[idx] = row;
        return true;
    }

    // A row's key, sealed under the secret (XChaCha20-Poly1305), its nonce the
    // row's name: one key per name, so a nonce never seals two things.
    bool seal(const Bytes& idx, const ETCS::sig::Key& key, Sealed& out) const
    {
        uint8_t k[32];
        if (!sealing_key(k)) return false;
        etcs_crypt::mc::crypto_aead_lock(out.data() + 16, out.data(), k, idx.data(), nullptr, 0, key.data(), 32);
        etcs_crypt::wipe(k, sizeof k);
        return true;
    }
    bool unseal(const Bytes& idx, ETCS::sig::Key& key) const
    {
        const auto it = sealed_.find(idx);
        uint8_t k[32];
        if (it == sealed_.end() || !sealing_key(k)) return false;
        const bool ok = etcs_crypt::mc::crypto_aead_unlock(key.data(), it->second.data(), k, idx.data(), nullptr, 0,
                                                           it->second.data() + 16, 32) == 0;
        etcs_crypt::wipe(k, sizeof k);
        return ok;
    }
    bool sealing_key(uint8_t out[32]) const
    {
        Keypair* d = etcs_crypt::as<Keypair>(derive_);
        static const uint8_t tag[8] = { 'E', 'T', 'C', 'S', 'S', 'E', 'A', 'L' };
        return d && d->Derive(tag, sizeof tag, out);
    }

    // --- the portal ----------------------------------------------------------------
    std::string portal_answer(const std::string& verb, const std::vector<std::string>& w)
    {
        const Clock::time_point now = Clock::now();
        for (auto it = sessions_.begin(); it != sessions_.end(); )
            it = it->second.until < now ? sessions_.erase(it) : std::next(it);
        if (verb == "open" && w.empty())
        {
            // Opening costs nothing, so a full table gives way: the oldest one
            // still waiting, else the oldest. Nobody holds the portal shut.
            if (sessions_.size() >= kSessions) drop_oldest([](const Session&) { return true; });
            Bytes code{}, token{};
            if (!etcs_crypt::random_bytes(code.data(), 32) || !etcs_crypt::random_bytes(token.data(), 32))
                return "refused: no randomness\n";
            Session& x = sessions_[etcs_crypt::hex(token.data(), 32)];
            x.code = code;
            x.until = now + std::min(life_, kWaiting);
            return etcs_crypt::hex(code.data(), 32) + " " + etcs_crypt::hex(token.data(), 32) + "\n";
        }
        if (verb == "signin" && w.size() == 3)
        {
            ETCS::sig::Key code{}, member{};
            uint8_t s[ETCS::sig::kSigBytes];
            if (!etcs_crypt::unhex(w[0], code.data(), 32) || !etcs_crypt::unhex(w[1], member.data(), 32)
                || !etcs_crypt::unhex(w[2], s, sizeof s))
                return "refused: not a portal word\n";
            Session* x = nullptr;
            for (auto& [t, v] : sessions_) if (!v.signed_in && v.code == code) x = &v;
            if (!x) return "refused: no portal is waiting on that code\n";
            if (!verify(ETCS::sig::portal_statement(member, pub_, code), member, s)) return "refused: the word does not verify\n";
            if (!current_member(member)) return "refused: that key is not a current member of any account\n";
            size_t held = 0;
            for (const auto& [t, v] : sessions_) held += v.signed_in && v.member == member;
            if (held >= kPerMember) drop_oldest([&](const Session& v) { return v.signed_in && v.member == member; });
            x->member = member;
            x->signed_in = true;
            x->until = now + life_;
            ETCS_LOG("Registrar", "a portal session opened for a member");
            return "signed in\n";
        }
        if (w.empty()) return "refused: not the words portal/" + verb + " takes\n";
        const auto it = sessions_.find(w[0]);
        if (it == sessions_.end()) return "refused: no such session\n";
        Session& x = it->second;
        if (verb == "close" && w.size() == 1) { sessions_.erase(it); return "closed\n"; }
        if (verb == "state" && w.size() == 1)
            return x.signed_in ? "signed in " + etcs_crypt::hex(x.member.data(), 32) + "\n" : std::string("pending\n");
        if (!x.signed_in) return "refused: the session is not signed in\n";
        if (!current_member(x.member)) { sessions_.erase(it); return "refused: the session's member was retired\n"; }
        if (verb == "members" && w.size() == 1) return members_of(x.member);
        if (verb == "admit" && w.size() == 3)
        {
            ETCS::sig::Key key{};
            uint8_t s[ETCS::sig::kSigBytes];
            if (!etcs_crypt::unhex(w[1], key.data(), 32) || !etcs_crypt::unhex(w[2], s, sizeof s) || key == x.member)
                return "refused: not a key's word to join\n";
            bool agreed = false;
            for (const Bytes& e : epochs_) agreed = agreed || verify(ETCS::sig::join_statement(key, x.member, pub_, e), key, s);
            if (!agreed) return "refused: that key's word does not name this member, in an epoch this registrar serves\n";
            return admit(x.member, key);
        }
        if (verb == "retire" && w.size() == 2)
        {
            ETCS::sig::Key key{};
            if (!etcs_crypt::unhex(w[1], key.data(), 32)) return "refused: not a key\n";
            const ETCS::sig::Key member = x.member;
            const std::string said = retire(member, key);
            if (key == member && said == "retired\n") sessions_.erase(w[0]);
            return said;
        }
        return "refused: not the words portal/" + verb + " takes\n";
    }

    // The oldest session `of` picks goes: one not signed in before one that is.
    template <typename F> void drop_oldest(F of)
    {
        auto gone = sessions_.end();
        for (auto it = sessions_.begin(); it != sessions_.end(); ++it)
        {
            if (!of(it->second)) continue;
            if (gone == sessions_.end() || (gone->second.signed_in && !it->second.signed_in)
                || (gone->second.signed_in == it->second.signed_in && it->second.until < gone->second.until))
                gone = it;
        }
        if (gone != sessions_.end()) sessions_.erase(gone);
    }

    bool current_member(const ETCS::sig::Key& key) const
    {
        Bytes idx{};
        if (!derive("ETCSMEM1", key, idx.data())) return false;
        const auto it = members_.find(idx);
        return it != members_.end() && !it->second.retired;
    }

    // The account's keys, as only a member's session sees them.
    std::string members_of(const ETCS::sig::Key& member) const
    {
        Bytes mi{};
        if (!derive("ETCSMEM1", member, mi.data())) return "refused: the registrar's secret is gone\n";
        const Bytes account = members_.at(mi).account;
        std::string out;
        for (const auto& [idx, r] : members_)
        {
            if (r.account != account && r.next != account) continue;
            ETCS::sig::Key key{};
            out += (unseal(idx, key) ? etcs_crypt::hex(key.data(), 32) : std::string("-")) + (r.retired ? " retired " : " current ")
                 + (r.epoch == Bytes{} ? std::string("-") : etcs_crypt::hex(r.epoch.data(), 32))
                 + (idx == mi ? " you" : "") + (r.next == account ? " moving" : "") + "\n";
        }
        return out;
    }

    // The newest `window_` epochs given, kept with the accounts so a restart
    // on a new loader still serves the last one.
    void serve()
    {
        epochs_.clear();
        for (size_t i = given_.size() > window_ ? given_.size() - window_ : 0; i < given_.size(); ++i) epochs_.insert(given_[i]);
    }
    bool keep_epoch(Database_& db, const Bytes& e)
    {
        Stmt st(db, "INSERT OR REPLACE INTO ace_epochs(epoch, seen) VALUES (?1, (SELECT COALESCE(MAX(seen), 0) + 1 FROM ace_epochs))");
        return st && st.bind(1, DatabaseValue::blob(e.data(), 32)) && st.step() >= 0;
    }

    bool accept(const std::string& hex, std::set<Bytes>& into, const char* verb)
    {
        Bytes b{};
        if (!etcs_crypt::unhex(hex, b.data(), 32)) { ETCS_LOG("Registrar", verb << ": not 32 bytes of hex."); return false; }
        std::lock_guard<std::mutex> l(mu_);
        into.insert(b);
        return true;
    }

    static void inline_answer(ETCS::Buffer& io, const std::string& s) { io.reset(); io.writeString(s.c_str()); }

    // The bytes are this entity's and never change once answered, so they outlive the send.
    static void by_reference(ETCS::Buffer& io, const std::string& s) { RouteRef::Emit(io, s.data(), s.size()); }

    // One statement, finalized whatever the path out.
    struct Stmt
    {
        Database_& db;
        void*      s;
        Stmt(Database_& d, const char* sql) : db(d), s(d.Prepare(sql)) {}
        ~Stmt() { if (s) db.Finalize(s); }
        Stmt(const Stmt&) = delete;
        Stmt& operator=(const Stmt&) = delete;
        explicit operator bool() const { return s != nullptr; }
        bool bind(int i, const DatabaseValue& v) { return db.Bind(s, i, v); }
        int  step() { return db.Step(s); }
        DatabaseValue col(int c) { DatabaseValue v; db.Column(s, c, v); return v; }
    };

    mutable std::mutex                  mu_;
    ETCS::RID                           keypair_ = 0, derive_ = 0, db_ = 0;
    ETCS::sig::Key                      pub_{};
    std::vector<ETCS::sig::Certificate> above_;
    std::string                         mount_ = "pki";
    std::string                         key_hex_, chain_hex_;
    std::set<Bytes>                     heads_, epochs_;
    std::vector<Bytes>                  given_;        // epochs given, oldest first
    size_t                              window_ = 2;
    std::map<Bytes, Sealed>             sealed_;       // by row name
    std::map<std::string, Session>      sessions_;     // by token, hex
    std::chrono::seconds                life_{ 900 };
    static constexpr size_t             kSessions = 1024, kPerMember = 8, kIssued = 4096;
    static constexpr std::chrono::seconds kWaiting{ 300 };  // a session not yet signed in
    std::map<Bytes, Row>                members_;   // by the secret's BLAKE2b of the key
    std::map<Bytes, size_t>             current_;   // per account, its members not retired
    std::map<std::string, std::string>  issued_;    // answers already made, by their words; emptied when full
};

#endif // CRYPTPROVIDER_REGISTRAR_H__
