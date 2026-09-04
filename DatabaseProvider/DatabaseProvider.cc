#include "DatabaseProvider.h"


ETCS_MODULE_EXPORT_MAIN(DatabaseProvider, "LocalDatabase") // "LocalDatabase RemoteDatabase" once the postgres leaf lands
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


// The sqlite tag. HYBRID because the read surface is a stream: a result set
// is a sequence, and RowProduce/RowConsume is what lets a consumer take it a
// row at a time instead of materialising the whole thing first.
//
// Both tags a provider exports here claim the SAME family, `Database`
// (ontology/DatabaseBase.h). The tag name states the expected locale because
// that is what a script knows at spawn; the family says nothing about it,
// because locality is not part of what a database owes.
ETCS_TAG_BLOCK_HYBRID(
    LocalDatabase, 
    (Delete, Connect, Disconnect, ExecuteRaw, Query, InitializeSchema, BeginTransaction, ExecuteTransaction, Commit, Rollback), 
    (QueryProduce, RowProduce, RowConsume)
)

/*
 * The postgres tag, when its leaf exists (Contract_DatabaseProvider.h).
 *
 * The verb list is deliberately the SAME LIST. That is the whole test of
 * whether the collapse was right: if the remote one needed different verbs,
 * locality would be a constraint after all and would have earned its family
 * back. It does not -- connect, execute, query, transact are what a database
 * does, and a socket underneath changes none of them.
 *
ETCS_TAG_BLOCK_HYBRID(
    RemoteDatabase,
    (Delete, Connect, Disconnect, ExecuteRaw, Query, InitializeSchema, BeginTransaction, ExecuteTransaction, Commit, Rollback),
    (QueryProduce, RowProduce, RowConsume)
)
*/
