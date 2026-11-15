#include "DDNSProvider.h"

// Herald: re-advertises a far Directory's entries when witnesses agree the
// address moved (DDNSProvider/Herald.h). Namecheap: Namecheap's dynamic DNS as
// that Directory, spoken through an Embassy (DDNSProvider/Namecheap.h).
ETCS_MODULE_EXPORT_MAIN(DDNSProvider, "Herald Namecheap")

ETCS_TAG_BLOCK_BASIC(Herald,
    Track, Witness, SetQuorum, SetKind, SetInterval, SetHoldoff,
    Start, Stop, IsStarted, Sweep, Info, Delete
)

ETCS_TAG_BLOCK_BASIC(Namecheap,
    SetDomain, SetRegistrar, SetResolver, SetSecret, SetEmbassy, Hold,
    Advertise, Withdraw, List, Self, Delete
)
