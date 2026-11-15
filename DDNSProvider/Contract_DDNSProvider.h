#ifndef DDNSPROVIDER_CONTRACT__

// OS invariant. Nothing here opens a socket: the far name server, the
// witnesses and the Foreign speakers are RIDs reached by name, and the network edge belongs to
// NetworkProvider. A #if defined(__linux__) here would mean an edge that
// should have been delegated.
#include "DDNSProvider/Herald.h"
#include "DDNSProvider/Namecheap.h"

#define DDNSPROVIDER_CONTRACT__

// auto generated hashes of headers:
#include "../../ETCS.h"
#include "module_hashes.h"

#endif // DDNSPROVIDER_CONTRACT__
