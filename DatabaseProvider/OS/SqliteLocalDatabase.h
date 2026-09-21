#ifndef SQLITE_LOCALDATABASE_H__
#define SQLITE_LOCALDATABASE_H__

#include "../../../ontology.h"
#include "../sqlite/sqlite3.h"
#include <iostream>

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#include <emscripten/threading.h>
#endif

/*
 * The sqlite leaf. Claims `Database` -- the one family -- and nothing about
 * locality, because locality is not a constraint on what a database owes
 * (ontology/DatabaseBase.h). What makes this the LOCAL one is that it opens a
 * path on this machine's filesystem, which is a fact about this class, stated
 * by this class, and visible to a script as the tag it spawns.
 *
 * The name keeps `Sqlite` in it on purpose: this is the implementation, and
 * the implementation may name its ingredient. The contract name it reaches
 * ETCS under is `LocalDatabase`, typedef'd in Contract_DatabaseProvider.h --
 * the same split Clayout/Layout makes, for the same reason.
 *
 * THE SAME LEAF IN THE BROWSER. Under emscripten the path is a name in the
 * runtime's own filesystem, and the file under it is as local as one on a
 * disk: MEMFS answers open/read/write/fcntl the way a kernel does, and sqlite
 * neither knows nor cares what backs the descriptor. What the browser cannot
 * do is keep those bytes across a reload on its own -- MEMFS is memory -- so
 * the page mounts IDBFS on a directory (/persist) and the ONE thing this leaf
 * adds there is saying WHEN the file is settled (persist_if_settled). Not a
 * Web/ platform header, because nothing about the engine differs; a second
 * class would be this one with an #ifdef moved.
 */
class SqliteLocalDatabase : 
    public DatabaseBase<SqliteLocalDatabase>, public DeletableBase<SqliteLocalDatabase>
{
    WIRE_TYPE_IDENTITY(SqliteLocalDatabase); // does public: for you :) 
    
    SqliteLocalDatabase() = default;
    ~SqliteLocalDatabase() override { CloseConnectionConcrete(); }

    // --- CRTP Concrete Implementations ---
    bool DeleteConcrete() override 
    {
        std::string conjugate_key = getSourceModule().toString() + ":" + getSourceTag().toString();
        ETCS_LOG("close: firing self-DestroyEvent for RID:"
                 << getRID() << " (" << conjugate_key << ")");
        return ETCS::DestroyEvent{conjugate_key.c_str(), this}();
    }

    void CreateConnectionConcrete(const ETCS::Buffer& db_info) override 
    {
        if (connected) return;

        // Use the assignment operator you defined: Buffer& operator=(const Buffer& other)
        // This copies buf, written, and read_offset correctly.
        dbPath = db_info; 

        // Use the explicit operator const char*() or c_str()
        int rc = sqlite3_open(dbPath.c_str(), (sqlite3**)&db);

        if (rc == SQLITE_OK) 
        {
            connected = true;
            ETCS_LOG("Connection established to: " << dbPath.c_str());
        } 
        else 
        {
            ETCS_LOG("Failed to open DB: " << sqlite3_errmsg((sqlite3*)db));
            connected = false;
        }
    }

    void CloseConnectionConcrete() override
    {
        if (!connected || !db) return;

        sqlite3_close((sqlite3*)db);
        db = nullptr;
        connected = false;
        dbPath.clear(); // Ensure no leakage of config as per your comment
        persist();      // the last write may still be only in memory (see persist_if_settled)
    }

    bool InitializeSchemaConcrete(const ETCS::Buffer& schema) override
    {
        if (!connected || !db) return false;

        // 1. Store the schema in the protected base member
        this->schemaGenerator = schema;

        // 2. Actually apply it to the SQLite handle
        char* errMsg = nullptr;
        int rc = sqlite3_exec((sqlite3*)db, this->schemaGenerator.c_str(), nullptr, nullptr, &errMsg);

        if (rc != SQLITE_OK)
        {
            if (errMsg)
            {
                ETCS_LOG("Schema Init Error: " << errMsg);
                sqlite3_free(errMsg);
            }
            return false;
        }
        persist_if_settled();
        return true;
    }

    bool ExecuteRawConcrete(ETCS::Buffer& data) override
    {
        if (!connected || !db) return false;

        char* errMsg = nullptr;

        // Execute the raw buffer content using c_str()
        int rc = sqlite3_exec((sqlite3*)db, data.c_str(), nullptr, nullptr, &errMsg);

        if (rc != SQLITE_OK)
        {
            if (errMsg)
            {
                ETCS_LOG("SQL Error: " << errMsg);
                sqlite3_free(errMsg);
            }
            return false;
        }

        persist_if_settled();
        return true;
    }

    // ── the bound-statement surface (ontology/Database.h) ───────────────────
    //
    // Thin by design: each is one sqlite call plus the family's answer shape.
    // SQLITE_TRANSIENT on every bind, because the family says the caller's
    // bytes need only outlive the Bind -- a std::string built for the call is
    // the common case, and letting sqlite keep a pointer into it would make
    // that a use-after-free the moment the caller's frame moved on.
    void* PrepareConcrete(const char* sql) override
    {
        if (!connected || !db || !sql) return nullptr;
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2((sqlite3*)db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        {
            ETCS_LOG("Prepare: " << sqlite3_errmsg((sqlite3*)db) << " -- " << sql);
            return nullptr;
        }
        return stmt;
    }

    bool BindConcrete(void* stmt, int index, const DatabaseValue& v) override
    {
        if (!stmt) return false;
        auto* s = (sqlite3_stmt*)stmt;
        int rc = SQLITE_MISUSE;
        switch (v.kind)
        {
        case DatabaseValue::Null:    rc = sqlite3_bind_null(s, index); break;
        case DatabaseValue::Integer: rc = sqlite3_bind_int64(s, index, v.i); break;
        case DatabaseValue::Real:    rc = sqlite3_bind_double(s, index, v.d); break;
        case DatabaseValue::Text:    rc = sqlite3_bind_text(s, index, (const char*)v.p, (int)v.n, SQLITE_TRANSIENT); break;
        case DatabaseValue::Blob:    rc = sqlite3_bind_blob(s, index, v.p, (int)v.n, SQLITE_TRANSIENT); break;
        }
        if (rc != SQLITE_OK) ETCS_LOG("Bind " << index << ": " << sqlite3_errmsg((sqlite3*)db));
        return rc == SQLITE_OK;
    }

    int StepConcrete(void* stmt) override
    {
        if (!stmt) return -1;
        const int rc = sqlite3_step((sqlite3_stmt*)stmt);
        if (rc == SQLITE_ROW)  return 1;
        if (rc == SQLITE_DONE) return 0;
        ETCS_LOG("Step: " << sqlite3_errmsg((sqlite3*)db));
        return -1;
    }

    bool ColumnConcrete(void* stmt, int col, DatabaseValue& out) override
    {
        if (!stmt) return false;
        auto* s = (sqlite3_stmt*)stmt;
        if (col < 0 || col >= sqlite3_column_count(s)) return false;
        out = DatabaseValue();
        switch (sqlite3_column_type(s, col))
        {
        case SQLITE_INTEGER: out = DatabaseValue::integer(sqlite3_column_int64(s, col)); break;
        case SQLITE_FLOAT:   out = DatabaseValue::real(sqlite3_column_double(s, col)); break;
        case SQLITE_TEXT:    out = DatabaseValue::text((const char*)sqlite3_column_text(s, col),
                                                       (size_t)sqlite3_column_bytes(s, col)); break;
        case SQLITE_BLOB:    out = DatabaseValue::blob(sqlite3_column_blob(s, col),
                                                       (size_t)sqlite3_column_bytes(s, col)); break;
        default:             break;   // NULL
        }
        return true;
    }

    // Finalize is where a bound WRITE lands, so it is the persist point for
    // this surface -- the counterpart of ExecuteRaw's line above.
    void FinalizeConcrete(void* stmt) override
    {
        if (!stmt) return;
        sqlite3_finalize((sqlite3_stmt*)stmt);
        persist_if_settled();
    }

private:
    /*
     * WHEN THE FILE IS WORTH KEEPING, asked of sqlite rather than guessed.
     *
     * sqlite3_get_autocommit is true exactly when no transaction is open --
     * which is when the pages in the file are the committed state and not a
     * half-written one. Persisting inside a transaction would copy a file the
     * journal still has to roll back; persisting only at Commit would miss
     * every autocommit statement, which is most of what scripts run. Asking
     * the engine covers both and also a BEGIN..COMMIT that arrived in one
     * ExecuteRaw, which the base's in_transaction flag never sees.
     *
     * Native: nothing to do, the kernel owns the bytes. Browser: MEMFS owns
     * them and forgets on reload, so the page is told to copy the IDBFS mount
     * to IndexedDB (window.etcsPersist, scripts/www/index.html). Via
     * MAIN_THREAD_EM_ASM because the caller is whatever thread ran the verb
     * and FS.syncfs lives on the page's main thread; the page coalesces, so
     * a burst of statements is one IndexedDB transaction, not one each.
     */
    void persist_if_settled()
    {
        if (!connected || !db) return;
        if (sqlite3_get_autocommit((sqlite3*)db)) persist();
    }

    void persist()
    {
#if defined(__EMSCRIPTEN__)
        MAIN_THREAD_EM_ASM({
            if (typeof window !== 'undefined' && typeof window.etcsPersist === 'function')
                window.etcsPersist();
        });
#endif
    }

};

#endif
