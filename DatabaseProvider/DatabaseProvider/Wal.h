#ifndef DATABASEPROVIDER_WAL_H__
#define DATABASEPROVIDER_WAL_H__
#include "../../../core_defs.h"
#include "../../../ontology.h"
#include "../sqlite/sqlite3.h"

/*
 * EVERY DATABASE THIS PROVIDER OPENS IS IN WAL MODE. A commit appends to the
 * write-ahead log instead of rewriting pages under a rollback journal, and
 * with synchronous=NORMAL the log is synced at checkpoints rather than at
 * every commit: a crash of the process loses nothing, a power cut can lose
 * the last commits but never corrupts the file. Readers do not block the
 * writer -- a frequent writer (Persistence's watcher, twice a second) and a
 * reader (a resume, a query) no longer take turns.
 *
 * In a browser the database is on IDBFS in one process: EXCLUSIVE locking
 * keeps WAL's index in memory rather than in a -shm file mapped by several
 * processes, which that filesystem has no use for. Natively locking stays
 * normal: two loaders may share a store directory (Store.h).
 *
 * False when the database refused (an old file on a read-only medium, say);
 * it is still usable in whatever mode it was.
 */
inline bool etcs_sqlite_wal(sqlite3* db)
{
    if (!db) return false;
#if defined(__EMSCRIPTEN__)
    sqlite3_exec(db, "PRAGMA locking_mode=EXCLUSIVE;", nullptr, nullptr, nullptr);
#endif
    sqlite3_stmt* st = nullptr;
    bool wal = false;
    if (sqlite3_prepare_v2(db, "PRAGMA journal_mode=WAL;", -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
    {
        const unsigned char* mode = sqlite3_column_text(st, 0);
        wal = mode && (mode[0] == 'w' || mode[0] == 'W');
    }
    sqlite3_finalize(st);
    sqlite3_exec(db, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
    return wal;
}

#endif // DATABASEPROVIDER_WAL_H__
