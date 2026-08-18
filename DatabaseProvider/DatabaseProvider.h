#ifndef DATABASEPROVIDER_H__
#define DATABASEPROVIDER_H__

#define ETCS_DLL_EXPORTS
#include "../../core_defs.h"
#include "../../ontology.h"
#include "Contract_DatabaseProvider.h"

// --- LocalDatabase Implementation ---

DEFINE_WORK_FUNC(LocalDatabase, Delete)
{
    (void)ctx; (void)data;
    self.DeleteConcrete();
}

DEFINE_WORK_FUNC_TYPED(LocalDatabase, Connect, (std::string, path))
{
    (void)ctx;
    ETCS_LOG("Connect", "Opening SQLite database at: " << path);
    ETCS::Buffer pathBuf(path);
    self.CreateConnection(pathBuf);
}

DEFINE_WORK_FUNC(LocalDatabase, InitializeSchema)
{
    (void)ctx;
    ETCS_LOG("InitializeSchema", "Applying structural schema to entity instance...");
    if (!self.InitializeSchema(data)) {
        ETCS_LOG("InitializeSchema", "Failed to initialize schema.");
    } else {
        ETCS_LOG("InitializeSchema", "Schema applied successfully.");
    }
}

DEFINE_WORK_FUNC(LocalDatabase, Disconnect)
{
    (void)data; (void)ctx;
    ETCS_LOG("Disconnect", "Closing database connection.");
    self.CloseConnection();
}

DEFINE_WORK_FUNC(LocalDatabase, ExecuteRaw)
{
    (void)ctx;
    ETCS_LOG("ExecuteRaw", "Executing SQL command...");
    if (!self.ExecuteRaw(data)) {
        ETCS_LOG("ExecuteRaw", "Execution failed.");
    }
}

DEFINE_WORK_FUNC(LocalDatabase, ExecuteTransaction)
{
    (void)ctx;
    ETCS_LOG("ExecuteTransaction", "Executing transactional SQL command...");
    auto guard = self.Transaction();
    if (!self.ExecuteRaw(data))
    {
        ETCS_LOG("ExecuteTransaction", "Execution failed, rolling back.");
        return;
    }
    guard.commit();
    ETCS_LOG("ExecuteTransaction", "Transaction committed.");
}

DEFINE_WORK_FUNC(LocalDatabase, BeginTransaction)
{
    (void)data; (void)ctx;
    ETCS_LOG("BeginTransaction", "Beginning transaction...");
    if (!self.BeginTransaction())
        ETCS_LOG("BeginTransaction", "Failed — already in transaction.");
}

DEFINE_WORK_FUNC(LocalDatabase, Commit)
{
    (void)data; (void)ctx;
    ETCS_LOG("Commit", "Committing transaction...");
    if (!self.Commit())
        ETCS_LOG("Commit", "Failed — no active transaction.");
}

DEFINE_WORK_FUNC(LocalDatabase, Rollback)
{
    (void)data; (void)ctx;
    ETCS_LOG("Rollback", "Rolling back transaction...");
    if (!self.Rollback())
        ETCS_LOG("Rollback", "Failed — no active transaction.");
}

// --- Streaming helpers ---
// Module-local, not exported — used only by QueryProduce below.

// Heuristic table-name extraction: finds "FROM <name>" in the query text.
// Not a real SQL parser — handles every simple single-table SELECT these
// demo scripts use (SELECT * FROM users, SELECT COUNT(*) FROM users,
// SELECT * FROM users WHERE ..., etc.) but wouldn't correctly resolve a
// JOIN or subquery. SQLite's own sqlite3_column_table_name() would be more
// robust here, but it depends on SQLITE_ENABLE_COLUMN_METADATA being set
// at SQLite's own build time, which this build's configuration isn't
// confirmed to have — this heuristic has no such dependency.
static std::string _extractTableNameFromQuery(const std::string& query)
{
    std::string upper = query;
    for (auto& c : upper) c = static_cast<char>(std::toupper((unsigned char)c));

    size_t fromPos = upper.find(" FROM ");
    if (fromPos == std::string::npos) return "query_result";

    size_t nameStart = fromPos + 6;
    while (nameStart < query.size() && std::isspace((unsigned char)query[nameStart])) nameStart++;

    size_t nameEnd = nameStart;
    while (nameEnd < query.size() &&
           (std::isalnum((unsigned char)query[nameEnd]) || query[nameEnd] == '_'))
        nameEnd++;

    if (nameEnd == nameStart) return "query_result";
    return query.substr(nameStart, nameEnd - nameStart);
}

// Double-quote a SQL identifier, escaping any embedded quotes. Needed
// because column names aren't guaranteed to be valid bare identifiers —
// "SELECT COUNT(*) FROM users" produces a column literally named
// "COUNT(*)", which isn't legal unquoted in a CREATE TABLE or INSERT
// column list.
static std::string _quoteIdentifier(const std::string& name)
{
    std::string out = "\"";
    for (char c : name) { if (c == '"') out += '"'; out += c; }
    out += "\"";
    return out;
}

// QueryProduce — produce/consume as a proper pair: the FIRST message this
// sends is a complete CREATE TABLE statement (derived from the query's own
// column names and, heuristically, its source table), and every message
// after that is a complete, self-contained INSERT statement — one per row.
// RowConsume's job is then just "execute whatever arrives, in order" (see
// below); no schema knowledge needs to exist on the consuming side at all.
//
// Row values are embedded via sqlite3_mprintf("%Q", ...) — SQLite's own
// safe-literal formatter — not hand-built string concatenation. Row
// content is arbitrary database data and could contain quotes; %Q handles
// escaping and NULL-vs-empty-string correctly, which a naive "'" + val +
// "'" concatenation would not.
// Query <sql>  ->  one row, tab-separated, into the caller's Buffer.
//
// The synchronous read this provider never had. QueryProduce is bulk and needs
// a paired consumer; an entity that wants ONE bounded answer mid-request had no
// way to ask, so callers either round-tripped through a stream or gave up --
// ForumNode refuses to evict a thread past its cap for exactly this reason,
// with the data sitting on disk and no way to read it back.
//
// Bounded by the argument channel on purpose. This is a work function, so the
// answer is arguments: one row, truncated with a flag rather than silently.
// Anything larger is bulk and belongs on QueryProduce/RowProduce.
//
// Result: "<col>\t<col>\t...\n" for the first row, or "" for no rows. A
// truncated row is prefixed "!" so a caller can tell a clipped answer from a
// short one -- silent truncation here would look exactly like a NULL column.
DEFINE_WORK_FUNC(LocalDatabase, Query)
{
    (void)ctx;
    const std::string sql = data.restAsString();
    data.reset();

    sqlite3* db_ptr = (sqlite3*)self.GetHandle();
    if (!db_ptr) { ETCS_LOG("Query", "no connection."); return; }

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_ptr, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
    {
        ETCS_LOG("Query", "prepare failed: " << sqlite3_errmsg(db_ptr));
        return;
    }

    std::string row;
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        const int cols = sqlite3_column_count(stmt);
        for (int i = 0; i < cols; ++i)
        {
            if (i > 0) row += '\t';
            const unsigned char* v = sqlite3_column_text(stmt, i);
            if (v) row += reinterpret_cast<const char*>(v);
        }
    }
    sqlite3_finalize(stmt);

    if (row.empty()) return;
    if (row.size() + 2 > ETCS::Buffer::bufsize)
    {
        row.resize(ETCS::Buffer::bufsize - 2);
        row = "!" + row;
        ETCS_LOG("Query", "row exceeds the " << ETCS::Buffer::bufsize
                 << "-byte argument channel -- truncated and flagged. Use "
                 << "QueryProduce for anything this size.");
    }
    row += '\n';
    data.writeString(row.c_str());
}

// RowProduce <sql>  ->  a stream of RECORDS, not statements.
//
// QueryProduce emits SQL text because it was built for database-to-database
// mirroring, where the consumer is another sqlite handle that can just execute
// it. A consumer that is NOT a database has to parse SQL back into fields -- a
// round trip through a format neither side wanted, and the ugliest thing in
// ForumWebsiteProvider's own load path.
//
// This emits the same rows in a neutral form any consumer can read:
//
//   first frame:  "#" <col> \t <col> \t ...      column names, once
//   then:         <val> \t <val> \t ...          one frame per row
//
// Tab-separated because the values are already going through sqlite's own text
// conversion and tabs do not occur in the identifiers or the numeric forms;
// a value containing one is escaped as "\t" so a row can never gain a field.
// QueryProduce is untouched -- mirroring still wants statements.
DEFINE_STREAM_FUNC_PRODUCE(LocalDatabase, RowProduce)
{
    (void)data;
    const std::string sql = stream.getConfig().restAsString();

    sqlite3* db_ptr = (sqlite3*)self.GetHandle();
    if (!db_ptr) { ETCS_LOG("RowProduce", "no connection."); return; }

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_ptr, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
    {
        ETCS_LOG("RowProduce", "prepare failed: " << sqlite3_errmsg(db_ptr));
        return;
    }

    auto escape = [](const char* v) {
        std::string out;
        for (const char* p = v; p && *p; ++p)
        {
            if      (*p == '\t') out += "\\t";
            else if (*p == '\\') out += "\\\\";
            else if (*p == '\n') out += "\\n";
            else                 out += *p;
        }
        return out;
    };

    const int cols = sqlite3_column_count(stmt);

    std::string header = "#";
    for (int i = 0; i < cols; ++i)
    {
        if (i > 0) header += '\t';
        header += sqlite3_column_name(stmt, i);
    }
    ETCS::Buffer slot;
    slot.writeString(header.c_str());
    if (!stream.writeRaw(slot))
    {
        ETCS_LOG("RowProduce", "consumer gone before the header landed.");
        sqlite3_finalize(stmt);
        return;
    }

    long rows = 0, dropped = 0;
    while (!ctx.isInterrupted() && !ctx.isTerminated()
           && sqlite3_step(stmt) == SQLITE_ROW)
    {
        std::string line;
        for (int i = 0; i < cols; ++i)
        {
            if (i > 0) line += '\t';
            const unsigned char* v = sqlite3_column_text(stmt, i);
            if (v) line += escape(reinterpret_cast<const char*>(v));
        }

        // A row wider than one frame is DROPPED with a count, not clipped: a
        // truncated record read back as data is worse than a missing one,
        // because nothing downstream can tell it was truncated.
        if (line.size() + 1 > ETCS::Buffer::bufsize) { ++dropped; continue; }

        ETCS::Buffer row;
        row.writeString(line.c_str());
        if (!stream.writeRaw(row)) break;
        ++rows;
    }
    sqlite3_finalize(stmt);

    ETCS_LOG("RowProduce", rows << " row(s) sent"
             << (dropped ? (", " + std::to_string(dropped) + " DROPPED (wider than one frame)") : ""));
}

DEFINE_STREAM_FUNC_PRODUCE(LocalDatabase, QueryProduce)
{
    (void)data;
    ETCS::Buffer configBuf = stream.getConfig();
    std::string sqlQuery = configBuf.restAsString();

    sqlite3_stmt* stmt = nullptr;
    sqlite3* db_ptr = (sqlite3*)self.GetHandle();

    ETCS_LOG("QueryProduce", "configBuf written=" << configBuf.written
         << " read_offset=" << configBuf.read_offset
         << " content='" << configBuf.c_str() << "'");

    ETCS_LOG("QueryProduce", "Initiating query on handle [" << db_ptr << "]: " << sqlQuery);

    if (!db_ptr) {
        ETCS_LOG("QueryProduce", "FATAL: Database handle is null. Connection may have failed.");
        stream.closeWrite();
        return;
    }

    if (sqlite3_prepare_v2(db_ptr, sqlQuery.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        ETCS_LOG("QueryProduce", "Prepare failed: " << sqlite3_errmsg(db_ptr));
        stream.closeWrite();
        return;
    }

    int colCount = sqlite3_column_count(stmt);

    // --- Schema message, sent once, before any row data ---
    // CREATE TABLE IF NOT EXISTS, not DROP+CREATE — if the consuming
    // connection already has a same-named table (as secondary does for
    // "users" in long_database_test.etcs, via its own InitializeSchema
    // call earlier in the script), the mirror lands directly in that
    // existing table rather than destroying and rebuilding it.
    std::string targetTable = _extractTableNameFromQuery(sqlQuery);
    std::string createStmt = "CREATE TABLE IF NOT EXISTS " + _quoteIdentifier(targetTable) + " (";
    for (int i = 0; i < colCount; i++) {
        if (i > 0) createStmt += ", ";
        createStmt += _quoteIdentifier(sqlite3_column_name(stmt, i));
    }
    createStmt += ")";

    ETCS::Buffer schemaSlot;
    schemaSlot.writeString(createStmt.c_str());
    if (!stream.writeRaw(schemaSlot)) {
        ETCS_LOG("QueryProduce", "Stream write failed sending schema; consumer likely disconnected.");
        sqlite3_finalize(stmt);
        stream.closeWrite();
        return;
    }
    ETCS_LOG("QueryProduce", "Schema sent: " << createStmt);

    int rowsFound = 0;

    while (true) {
        if (ctx.isInterrupted() || ctx.isTerminated()) { break; }

        int stepResult = sqlite3_step(stmt);

        if (stepResult == SQLITE_ROW) {
            std::string insertStmt = "INSERT OR REPLACE INTO " + _quoteIdentifier(targetTable) + " (";
            for (int i = 0; i < colCount; i++) {
                if (i > 0) insertStmt += ", ";
                insertStmt += _quoteIdentifier(sqlite3_column_name(stmt, i));
            }
            insertStmt += ") VALUES (";
            for (int i = 0; i < colCount; i++) {
                if (i > 0) insertStmt += ", ";
                if (sqlite3_column_type(stmt, i) == SQLITE_NULL) {
                    insertStmt += "NULL";
                } else {
                    const char* val = (const char*)sqlite3_column_text(stmt, i);
                    char* quoted = sqlite3_mprintf("%Q", val ? val : "");
                    insertStmt += quoted;
                    sqlite3_free(quoted);
                }
            }
            insertStmt += ")";

            ETCS::Buffer rowSlot;
            if (!rowSlot.writeString(insertStmt.c_str())) {
                // TBuffer's own writeString already logs a truncation
                // warning here (see TBuffer.h) — a fully-formed INSERT
                // statement is meaningfully larger than the old raw
                // tab-separated payload was, so a row with many columns
                // or long text values could exceed ETCS::Buffer's 256
                // bytes where the old format wouldn't have. Worth
                // widening this stream's payload type if that ever bites
                // on real data — flagging rather than silently hoping
                // demo-sized rows are representative.
                ETCS_LOG("QueryProduce", "INSERT statement truncated, skipping row " << rowsFound + 1);
                continue;
            }
            if (!stream.writeRaw(rowSlot)) {
                ETCS_LOG("QueryProduce", "Stream write failed; consumer likely disconnected.");
                break;
            }
            rowsFound++;
        }
        else if (stepResult == SQLITE_DONE) {
            break;
        }
        else {
            ETCS_LOG("QueryProduce", "Step encountered error [" << stepResult << "]: " << sqlite3_errmsg(db_ptr));
            break;
        }
    }

    sqlite3_finalize(stmt);
    stream.closeWrite();
    ETCS_LOG("QueryProduce", "Query stream completed. Total rows sent: " << rowsFound);
}

// RowConsume — the consumer half of the pair. No schema knowledge lives
// here at all: every message received is already a complete, executable
// SQL statement (the one schema message QueryProduce sends first, then one
// INSERT per row), so this just executes each one, in order, on ITS OWN
// connection (self) — not the producing connection. This is what makes
// long_database_test.etcs's "mirror primary into secondary" step actually
// land data in secondary's own tables now, rather than only logging what
// passed through.
//
// Batched in one transaction (self.transaction(), the same RAII guard
// ExecuteTransaction already uses) rather than one implicit autocommit per
// statement — committed unconditionally at the end, including on
// interrupt, since keeping whatever arrived before a Ctrl+C is more useful
// than losing the whole batch. A failed statement is logged and skipped,
// matching this file's existing convention elsewhere (ExecuteRaw,
// InitializeSchema) of logging failures rather than aborting the stream.
DEFINE_STREAM_FUNC_CONSUME(LocalDatabase, RowConsume)
{
    (void)data;
    sqlite3* db_ptr = (sqlite3*)self.GetHandle();

    ETCS_LOG("RowConsume", "Awaiting incoming database rows...");

    auto guard = self.Transaction();
    int executed = 0;

    while (true) {
        if (ctx.isInterrupted() || ctx.isTerminated()) { break; }

        ETCS::Buffer rowSlot;
        if (!stream.readRaw(rowSlot)) break;

        std::string statement = rowSlot.toString();
        ETCS_LOG("RowConsume", "EXECUTING: " << statement);

        if (db_ptr) {
            char* errMsg = nullptr;
            if (sqlite3_exec(db_ptr, statement.c_str(), nullptr, nullptr, &errMsg) != SQLITE_OK) {
                ETCS_LOG("RowConsume", "Statement failed: " << (errMsg ? errMsg : "unknown error"));
                sqlite3_free(errMsg);
            } else {
                executed++;
            }
        }
    }

    guard.commit();
    ETCS_LOG("RowConsume", "Consumption stream closed. Statements executed: " << executed);
}

#endif
