#ifndef DATABASEPROVIDER_PERSISTENCE_STORE_H__
#define DATABASEPROVIDER_PERSISTENCE_STORE_H__
#include "../../../ontology.h"
#include "../sqlite/sqlite3.h"

#include <cerrno>
#include <cstdio>
#include <ctime>
#include <fcntl.h>
#include <mutex>
#include <random>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#endif

/*
 * THE PERSISTENCE DATABASE -- the one store every Persistence child writes
 * to (Persistence.h), in the place this runtime keeps what outlives it
 * (ETCS::etcs_store_dir): `persistence.db` beside `loader.key`.
 *
 * NOT AN ENTITY. It is the local frame's own disk, not a node in the graph:
 * a Database in the scene would be part of what the scene hashes and
 * rebuilds, and the store cannot be something it restores.
 *
 * THE KEY IS THIS LOADER'S, and never leaves it: 32 random bytes made on
 * first use, readable only by its owner. Everything written here is MACed
 * with it (HMAC-SHA256), and a record whose MAC does not verify is not
 * read -- a scene is this loader's own, and a store carried to another
 * loader, or edited by hand, rebuilds nothing. `keyid` (the first 16 hex
 * of the key's SHA-256) is part of every key, so two loaders sharing a
 * directory keep separate scenes.
 *
 * TWO TABLES:
 *   records  (keyid, type, hash, ord) -> tags, script, kv, mac
 *            One per Environmental entity: its Module:Tag, the RID-free
 *            merkle hash of its surface (Entity::getHash), and among
 *            entities with both of those equal, the order they were made in
 *            -- emergent, never a stored RID. `kv` is what its script
 *            cannot say (ontology/Environmental.h).
 *   scenes   (keyid, name) -> at, script, roots, mac
 *            The whole: the script that rebuilds every persisted root, and
 *            each root's name and hash, which a resume checks itself against.
 *
 * In a browser the directory is IDBFS (/persist), which the page must mount
 * and pull before main() runs; after a write the page is asked to push it
 * back (window.etcsPersist), exactly as the sqlite leaf does.
 */
class PersistenceStore
{
public:
    struct Record
    {
        std::string type, hash;
        int         ord = 0;
        std::string tags, script, kv;
    };

    // Leaked on purpose: the watcher that writes here may still be running
    // while the process exits (Persistence.h).
    static PersistenceStore& get()
    {
        static PersistenceStore* s = new PersistenceStore();
        return *s;
    }

    bool ready()
    {
        std::lock_guard<std::mutex> lock(mu_);
        return openLocked();
    }
    std::string dir() const { return dir_; }
    std::string keyid()
    {
        std::lock_guard<std::mutex> lock(mu_);
        return openLocked() ? keyid_ : std::string();
    }

    bool putRecord(const Record& r)
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!openLocked()) return false;
        const std::string mac = macOf({ "R", keyid_, r.type, r.hash, std::to_string(r.ord), r.tags, r.script, r.kv });
        sqlite3_stmt* st = prepare("INSERT OR REPLACE INTO records(keyid,type,hash,ord,tags,script,kv,mac)"
                                   " VALUES(?,?,?,?,?,?,?,?)");
        if (!st) return false;
        bindText(st, 1, keyid_); bindText(st, 2, r.type); bindText(st, 3, r.hash);
        sqlite3_bind_int(st, 4, r.ord);
        bindText(st, 5, r.tags); bindText(st, 6, r.script);
        sqlite3_bind_blob(st, 7, r.kv.data(), static_cast<int>(r.kv.size()), SQLITE_TRANSIENT);
        bindText(st, 8, mac);
        const bool ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
        return ok;
    }

    // False with `why` when there is none, or it is not this loader's.
    bool getRecord(const std::string& type, const std::string& hash, int ord, Record& out, std::string& why)
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!openLocked()) { why = "no store"; return false; }
        sqlite3_stmt* st = prepare("SELECT tags,script,kv,mac FROM records WHERE keyid=? AND type=? AND hash=? AND ord=?");
        if (!st) { why = "store unreadable"; return false; }
        bindText(st, 1, keyid_); bindText(st, 2, type); bindText(st, 3, hash);
        sqlite3_bind_int(st, 4, ord);
        bool found = false;
        std::string mac;
        if (sqlite3_step(st) == SQLITE_ROW)
        {
            found      = true;
            out.type   = type; out.hash = hash; out.ord = ord;
            out.tags   = column(st, 0);
            out.script = column(st, 1);
            out.kv     = column(st, 2);
            mac        = column(st, 3);
        }
        sqlite3_finalize(st);
        if (!found) { why = "no record"; return false; }
        if (mac != macOf({ "R", keyid_, type, hash, std::to_string(ord), out.tags, out.script, out.kv }))
        { why = "the record's MAC does not verify -- not this loader's, or altered"; return false; }
        return true;
    }

    bool putScene(const std::string& name, const std::string& script, const std::string& roots)
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!openLocked()) return false;
        const std::string at  = std::to_string(static_cast<long long>(std::time(nullptr)));
        const std::string mac = macOf({ "S", keyid_, name, at, script, roots });
        sqlite3_stmt* st = prepare("INSERT OR REPLACE INTO scenes(keyid,name,at,script,roots,mac) VALUES(?,?,?,?,?,?)");
        if (!st) return false;
        bindText(st, 1, keyid_); bindText(st, 2, name); bindText(st, 3, at);
        bindText(st, 4, script); bindText(st, 5, roots); bindText(st, 6, mac);
        const bool ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
        return ok;
    }

    bool getScene(const std::string& name, std::string& script, std::string& roots, long long& at, std::string& why)
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!openLocked()) { why = "no store"; return false; }
        sqlite3_stmt* st = prepare("SELECT at,script,roots,mac FROM scenes WHERE keyid=? AND name=?");
        if (!st) { why = "store unreadable"; return false; }
        bindText(st, 1, keyid_); bindText(st, 2, name);
        bool found = false;
        std::string ats, mac;
        if (sqlite3_step(st) == SQLITE_ROW)
        {
            found  = true;
            ats    = column(st, 0);
            script = column(st, 1);
            roots  = column(st, 2);
            mac    = column(st, 3);
        }
        sqlite3_finalize(st);
        if (!found) { why = "no scene '" + name + "'"; return false; }
        if (mac != macOf({ "S", keyid_, name, ats, script, roots }))
        { why = "scene '" + name + "' does not verify -- not this loader's, or altered"; return false; }
        at = std::atoll(ats.c_str());
        return true;
    }

    void forgetScene(const std::string& name)
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!openLocked()) return;
        sqlite3_stmt* st = prepare("DELETE FROM scenes WHERE keyid=? AND name=?");
        if (!st) return;
        bindText(st, 1, keyid_); bindText(st, 2, name);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }

    // Everything above is autocommit; in a browser, push it to IndexedDB.
    void flush()
    {
#if defined(__EMSCRIPTEN__)
        MAIN_THREAD_EM_ASM({
            if (typeof window !== 'undefined' && typeof window.etcsPersist === 'function')
                window.etcsPersist('persistence');
        });
#endif
    }

    // Plain files beside the database: the loader reads these without sqlite.
    bool writeFile(const std::string& name, const std::string& body)
    {
        if (!ready()) return false;
        const std::string path = dir_ + "/" + name, tmp = path + ".tmp";
        FILE* f = std::fopen(tmp.c_str(), "wb");
        if (!f) return false;
        const bool ok = std::fwrite(body.data(), 1, body.size(), f) == body.size();
        std::fclose(f);
        return ok && std::rename(tmp.c_str(), path.c_str()) == 0;
    }
    void removeFile(const std::string& name) { if (ready()) std::remove((dir_ + "/" + name).c_str()); }

private:
    PersistenceStore() : dir_(ETCS::etcs_store_dir()) {}

    bool openLocked()
    {
        if (db_) return true;
        if (failed_) return false;
        failed_ = true;                                   // until proven otherwise
        if (!makeDirs(dir_)) { ETCS_LOG("Persistence", "store: cannot create " << dir_); return false; }
        if (!loadKey())      { ETCS_LOG("Persistence", "store: no loader key in " << dir_); return false; }
        const std::string path = dir_ + "/persistence.db";
        if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK)
        {
            ETCS_LOG("Persistence", "store: cannot open " << path);
            sqlite3_close(db_); db_ = nullptr;
            return false;
        }
        const char* schema =
            "CREATE TABLE IF NOT EXISTS records(keyid TEXT, type TEXT, hash TEXT, ord INTEGER,"
            " tags TEXT, script TEXT, kv BLOB, mac TEXT, PRIMARY KEY(keyid,type,hash,ord));"
            "CREATE TABLE IF NOT EXISTS scenes(keyid TEXT, name TEXT, at TEXT, script TEXT,"
            " roots TEXT, mac TEXT, PRIMARY KEY(keyid,name));";
        if (sqlite3_exec(db_, schema, nullptr, nullptr, nullptr) != SQLITE_OK)
        {
            ETCS_LOG("Persistence", "store: schema: " << sqlite3_errmsg(db_));
            sqlite3_close(db_); db_ = nullptr;
            return false;
        }
        failed_ = false;
        return true;
    }

    static bool makeDirs(const std::string& d)
    {
        for (size_t at = 1; at <= d.size(); ++at)
        {
            if (at != d.size() && d[at] != '/') continue;
            const std::string part = d.substr(0, at);
            if (::mkdir(part.c_str(), 0700) != 0 && errno != EEXIST) return false;
        }
        return true;
    }

    // Made once, 0600, and read on every start after.
    bool loadKey()
    {
        const std::string path = dir_ + "/loader.key";
        if (FILE* f = std::fopen(path.c_str(), "rb"))
        {
            char b[64];
            const size_t n = std::fread(b, 1, sizeof b, f);
            std::fclose(f);
            if (n < 32) return false;
            key_.assign(b, n);
        }
        else
        {
            std::random_device rd;
            std::string k;
            while (k.size() < 32) { const uint32_t v = rd(); k.append(reinterpret_cast<const char*>(&v), 4); }
            const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
            if (fd < 0) return false;
            const bool ok = ::write(fd, k.data(), k.size()) == static_cast<ssize_t>(k.size());
            ::close(fd);
            if (!ok) return false;
            key_ = k;
        }
        unsigned char d[32];
        picohash_ctx_t ctx;
        picohash_init_sha256(&ctx);
        picohash_update(&ctx, key_.data(), key_.size());
        picohash_final(&ctx, d);
        keyid_ = hex(d, 8);
        return true;
    }

    // Fields length-prefixed, so no two different tuples MAC the same bytes.
    std::string macOf(std::initializer_list<std::string> fields) const
    {
        picohash_ctx_t ctx;
        picohash_init_hmac(&ctx, picohash_init_sha256, key_.data(), key_.size());
        for (const std::string& f : fields)
        {
            const uint32_t n = static_cast<uint32_t>(f.size());
            picohash_update(&ctx, &n, 4);
            picohash_update(&ctx, f.data(), f.size());
        }
        unsigned char d[32];
        picohash_final(&ctx, d);
        return hex(d, 32);
    }

    static std::string hex(const unsigned char* d, size_t n)
    {
        static const char* x = "0123456789abcdef";
        std::string s;
        for (size_t i = 0; i < n; ++i) { s += x[d[i] >> 4]; s += x[d[i] & 15]; }
        return s;
    }
    sqlite3_stmt* prepare(const char* sql)
    {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK)
        {
            ETCS_LOG("Persistence", "store: " << sqlite3_errmsg(db_));
            return nullptr;
        }
        return st;
    }
    static void bindText(sqlite3_stmt* st, int i, const std::string& v)
    { sqlite3_bind_text(st, i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT); }
    static std::string column(sqlite3_stmt* st, int i)
    {
        const void* p = sqlite3_column_blob(st, i);
        const int   n = sqlite3_column_bytes(st, i);
        return p && n > 0 ? std::string(static_cast<const char*>(p), static_cast<size_t>(n)) : std::string();
    }

    std::mutex  mu_;
    std::string dir_, key_, keyid_;
    sqlite3*    db_     = nullptr;
    bool        failed_ = false;
};

#endif // DATABASEPROVIDER_PERSISTENCE_STORE_H__
