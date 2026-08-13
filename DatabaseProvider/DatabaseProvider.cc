#include "DatabaseProvider.h"


ETCS_MODULE_EXPORT_MAIN(DatabaseProvider, "LocalDatabase") // define your tags in this string separated list
// Make sure to create your tag types exported functions in the DatabaseProvider.h included above, with one of: 
//  - DEFINE_WORK_FUNC_TYPED(Type, FuncName, (Type, VarName),(Type, VarName), ...)
//  - DEFINE_WORK_FUNC(Type, FuncName)
//  - DEFINE_STREAM_FUNC_PRODUCE(Type, FuncName)
//  - DEFINE_STREAM_FUNC_CONSUME(Type, FuncName)

// Then declare your tag type here with an action map decleration, use either: 
//  - ETCS_TAG_BLOCK_HYBRID(tag_name, (WorkFunc1, WorkFunc2, ...), (StreamFunc1, StreamFunc2, ...))
//  - ETCS_TAG_BLOCK_BASIC(tag_name, WorkFunc1, WorkFunc2, ...)
// Depending on if you have stream functions on this type or not. Up to you to pick!
// Even wrongly mapped stream functions technically load, they just don't interface via default format
// Also remember, Tag must be a type! You can typedef your types in the Contract_DatabaseProvider.h header for cross platform use.
// (Which you probably want if your using this system!)


// Standard Local
ETCS_TAG_BLOCK_HYBRID(
    LocalDatabase, 
    (Delete, Connect, Disconnect, ExecuteRaw, InitializeSchema, BeginTransaction, ExecuteTransaction, Commit, Rollback), 
    (QueryProduce, RowConsume)
)

// Non-Standard Remote (e.g., with specific Connection Pooling logic)

/*
ETCS_TAG_BLOCK_HYBRID(
    RemoteDatabase, 
    (RemoteConnect, Disconnect, ExecuteRaw), 
    (QueryProduce, RowConsume) 
)
*/
