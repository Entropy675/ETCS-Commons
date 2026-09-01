#ifndef FORUMWEBSITEPROVIDER_CONTRACT__

// Like ChessProvider, this code is OS invariant, so there is no typedef
// shenanigans to do to reach a concrete type. Nothing here touches a socket, a
// file handle, or a platform API: the network edge belongs to NetworkProvider
// and the storage edge is a RID invoked by name, so both cross-platform
// questions are somebody else's.
//
// That is a property worth keeping rather than an accident. The moment this
// module needs a #if defined(__linux__) it has grown an edge it should have
// delegated.

#include "ForumWebsiteProvider/ForumThread.h"
#include "ForumWebsiteProvider/ForumSelf.h"
#include "ForumWebsiteProvider/ForumNode.h"

#define FORUMWEBSITEPROVIDER_CONTRACT__

// auto generated hashes of headers:
#include "../../ETCS.h"
#include "module_hashes.h"

#endif // FORUMWEBSITEPROVIDER_CONTRACT__
