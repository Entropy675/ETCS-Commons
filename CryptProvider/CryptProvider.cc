// Monocypher's bodies, once in this binary (libs/monocypher.h).
#define ETCS_MONOCYPHER_IMPLEMENTATION
#include "CryptProvider.h"

// Keypair, Shard: Private. Shamir: splits a Keypair over its Shard children
// and rebuilds one. Certificate: a link of the chain (core/Signature.h).
// Custody: the Directory of what one custodian holds, the stand-in for that
// custodian's FileRelation (NetworkProvider). Envelope: a Wrapper stage that
// seals under the key its Private child holds. Registrar: the route that hands
// a loader ACE's half through its ACE identity, and keeps accounts. Upstream: the site, as a loader reads it.
ETCS_MODULE_EXPORT_MAIN(CryptProvider, "Keypair Shard Shamir Certificate Custody Envelope Registrar Upstream")

ETCS_TAG_BLOCK_BASIC(Keypair,
    Generate, PublicKey, Sign, StoreTo, LoadFrom, Holds, Wipe, Delete
)

ETCS_TAG_BLOCK_BASIC(Shard,
    Index, Threshold, Set, Holds, Wipe, Delete
)

ETCS_TAG_BLOCK_BASIC(Shamir,
    Threshold, Split, Combine, Delete
)

ETCS_TAG_BLOCK_BASIC(Certificate,
    AddEpoch, Issue, Verify, Role, Subject, Issuer, Digest, Delete
)

ETCS_TAG_BLOCK_BASIC(Custody,
    SetEmbassy, SetRelation, Advertise, Recall, Withdraw, List, Delete
)

ETCS_TAG_BLOCK_BASIC(Envelope,
    Holds, Forget, Delete
)

ETCS_TAG_BLOCK_BASIC(Registrar,
    Use, Above, Mount, Derive, Accounts, Head, Epoch, Window, Current, Key, Filter, Request, Delete
)

ETCS_TAG_BLOCK_BASIC(Upstream,
    SetEmbassy, SetRelation, Paths, Register, Join, Admit, Retire, Check, Update, List, Delete
)
