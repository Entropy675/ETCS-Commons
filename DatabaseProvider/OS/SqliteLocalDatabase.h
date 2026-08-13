#ifndef SQLITE_LOCALDATABASE_H__
#define SQLITE_LOCALDATABASE_H__

#include "../../../ontology.h"
#include "../sqlite/sqlite3.h"
#include <iostream>

class SqliteLocalDatabase : 
    public LocalDatabaseBase<SqliteLocalDatabase>, public DeletableBase<SqliteLocalDatabase>
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

        return true;
    }

};

#endif
