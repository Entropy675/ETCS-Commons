#ifndef DATABASEPROVIDER_KEYSTOREDATABASE_H__
#define DATABASEPROVIDER_KEYSTOREDATABASE_H__

#include "../../../ontology.h"
#include "../../../core/Keystore.h"
#include "../../../core/Provenance.h"

#include <cstring>
#include <mutex>
#include <string>

/*
 * The OS keystore as a Database that claims Private (core/Keystore.h): the
 * medium a script names when it needs a secret kept, `requires keys
 * [Database, Private]`, and the one a LocalDatabase can never satisfy.
 *
 * One entity, one secret. Connect names it; the rest of Database is SQL and
 * a keystore holds no tables, so those refuse. The material moves only
 * between Private types (Receive in, Reveal out); a script line never
 * carries it.
 *
 * ITS WRAPPER CHILDREN GUARD IT, as they would a node on a link: the keystore
 * is a conversation that leaves the process, so what Receive hands it has
 * passed this entity's network-scope stages, and what Reveal brings back must
 * pass them in reverse or is refused. A secret kept under an Envelope is
 * reached only by an entity carrying the same one -- naming it is not
 * enough.
 *
 * Its names are the script's own: every item is under "script/", so no
 * script reaches a secret the loader keeps for itself (its signing key).
 * Scoped to this installation's store, like the loader's own.
 *
 * Delete forgets the entity, not the secret. Forget erases the secret.
 */
class KeystoreDatabase :
    public DatabaseBase<KeystoreDatabase>, public PrivateBase<KeystoreDatabase>,
    public DeletableBase<KeystoreDatabase>
{
    WIRE_TYPE_IDENTITY(KeystoreDatabase);

    KeystoreDatabase() = default;
    ~KeystoreDatabase() override = default;

    bool DeleteConcrete() override
    {
        std::string key = getSourceModule().toString() + ":" + getSourceTag().toString();
        return ETCS::DestroyEvent{key.c_str(), this}();
    }

    // --- Database_: Connect names the secret; SQL refuses ----------------------
    void CreateConnectionConcrete(const ETCS::Buffer& name) override
    {
        std::lock_guard<std::mutex> l(mu_);
        const std::string n = name.toString();
        if (connected) { ETCS_LOG("Keystore", "Connect: already names '" << item_ << "'."); return; }
        bool ok = !n.empty() && n.size() <= 64 && n[0] != '.';
        for (char c : n)
            ok = ok && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                        || c == '.' || c == '_' || c == '-');
        if (!ok) { ETCS_LOG("Keystore", "Connect: '" << n << "' is not a secret's name ([A-Za-z0-9._-], 64)."); return; }
        item_ = "script/" + n;
        connected = true;
    }
    void CloseConnectionConcrete() override
    {
        std::lock_guard<std::mutex> l(mu_);
        connected = false;
        item_.clear();
    }
    bool InitializeSchemaConcrete(const ETCS::Buffer&) override { return refuse("InitializeSchema"); }
    bool ExecuteRawConcrete(ETCS::Buffer&) override             { return refuse("ExecuteRaw"); }
    void* PrepareConcrete(const char*) override                 { refuse("Prepare"); return nullptr; }
    bool BindConcrete(void*, int, const DatabaseValue&) override { return refuse("Bind"); }
    int  StepConcrete(void*) override                           { refuse("Step"); return -1; }
    bool ColumnConcrete(void*, int, DatabaseValue&) override    { return refuse("Column"); }
    void FinalizeConcrete(void*) override                       {}

    // --- Private_ ------------------------------------------------------------------
    bool HoldsConcrete() const override
    {
        std::string s, why;
        const bool held = named() && ETCS::keystore::load(ETCS::etcs_store_dir(), item(), s, why)
                                     == ETCS::keystore::Status::Ok;
        if (!s.empty()) std::memset(&s[0], 0, s.size());
        return held;
    }
    void WipeConcrete() override {}                    // nothing is held in this process
    bool RevealConcrete(std::string& out) const override
    {
        out.clear();
        std::string why;
        if (!named()) { ETCS_LOG("Keystore", "Reveal: Connect names a secret first."); return false; }
        const auto st = ETCS::keystore::load(ETCS::etcs_store_dir(), item(), out, why);
        if (st == ETCS::keystore::Status::Absent) ETCS_LOG("Keystore", "Reveal: nothing kept as '" << item() << "'.");
        else if (st != ETCS::keystore::Status::Ok) ETCS_LOG("Keystore", "Reveal: " << why);
        if (st != ETCS::keystore::Status::Ok) return false;
        if (!unwrap(out))
        {
            if (!out.empty()) std::memset(&out[0], 0, out.size());
            out.clear();
            ETCS_LOG("Keystore", "Reveal: '" << item() << "' is not kept under this entity's stages -- refused.");
            return false;
        }
        return true;
    }
    bool ReceiveConcrete(const std::string& in) override
    {
        std::string why;
        if (!named()) { ETCS_LOG("Keystore", "Receive: Connect names a secret first."); return false; }
        std::string kept = in;
        if (!wrap(kept)) { ETCS_LOG("Keystore", "Receive: refused by this entity's stages."); return false; }
        const auto st = ETCS::keystore::store_new(ETCS::etcs_store_dir(), item(), kept, why);
        if (!kept.empty()) std::memset(&kept[0], 0, kept.size());
        if (st != ETCS::keystore::Status::Ok) ETCS_LOG("Keystore", "Receive: " << why);
        return st == ETCS::keystore::Status::Ok;
    }

    // --- verbs -------------------------------------------------------------------
    bool Forget()
    {
        std::string why;
        if (!named()) return false;
        const auto st = ETCS::keystore::erase(ETCS::etcs_store_dir(), item(), why);
        if (st == ETCS::keystore::Status::Unavailable) ETCS_LOG("Keystore", "Forget: " << why);
        return st == ETCS::keystore::Status::Ok;
    }

private:
    bool refuse(const char* verb) const
    {
        ETCS_LOG("Keystore", verb << ": refused -- a keystore keeps named secrets, not tables.");
        return false;
    }
    bool named() const { std::lock_guard<std::mutex> l(mu_); return connected; }

    // What is kept under stages is "w1:" and their output as hex; bare is bare.
    size_t stages(ETCS::IWireWrapper** out) const
    {
        return ETCS::MirrorBuffer::chainOf(const_cast<KeystoreDatabase*>(this), ETCS::WireScope::Socket,
                                           out, ETCS::MAX_WRAP_STAGES);
    }
    bool wrap(std::string& s) const
    {
        ETCS::IWireWrapper* chain[ETCS::MAX_WRAP_STAGES] = {};
        const size_t n = stages(chain);
        if (n == 0) return true;
        ETCS::MBuffer io;
        if (s.size() > ETCS::MBuffer::bufsize / 2) return false;
        std::memcpy(io.buf, s.data(), s.size());
        io.written = s.size();
        for (size_t i = 0; i < n; ++i)
        {
            chain[i]->Wrap(io, ETCS::SignalContext{});
            if (ETCS::wire_refused(io)) return false;
        }
        static const char* d = "0123456789abcdef";
        std::memset(&s[0], 0, s.size());
        s = "w1:";
        for (size_t i = 0; i < io.written; ++i) { s += d[uint8_t(io.buf[i]) >> 4]; s += d[uint8_t(io.buf[i]) & 15]; }
        return true;
    }
    bool unwrap(std::string& s) const
    {
        ETCS::IWireWrapper* chain[ETCS::MAX_WRAP_STAGES] = {};
        const size_t n = stages(chain);
        const bool wrapped = s.compare(0, 3, "w1:") == 0;
        if (n == 0 || !wrapped) return n == 0 && !wrapped;
        ETCS::MBuffer io;
        if ((s.size() - 3) % 2 != 0 || (s.size() - 3) / 2 > ETCS::MBuffer::bufsize) return false;
        auto v = [](char c) -> int { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
        for (size_t i = 3; i < s.size(); i += 2)
        {
            const int hi = v(s[i]), lo = v(s[i + 1]);
            if (hi < 0 || lo < 0) return false;
            io.buf[io.written++] = char(hi << 4 | lo);
        }
        for (size_t i = n; i-- > 0; )
        {
            chain[i]->Unwrap(io, ETCS::SignalContext{});
            if (ETCS::wire_refused(io)) return false;
        }
        s.assign(io.buf, io.written);
        std::memset(io.buf, 0, io.written);
        return true;
    }
    std::string item() const { std::lock_guard<std::mutex> l(mu_); return item_; }

    mutable std::mutex mu_;
    std::string item_;
};

#endif // DATABASEPROVIDER_KEYSTOREDATABASE_H__
