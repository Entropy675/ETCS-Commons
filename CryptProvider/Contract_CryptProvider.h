#ifndef CRYPTPROVIDER_CONTRACT__

// OS invariant: one leaf per type for every platform. Randomness is the OS's
// (CryptProvider/Crypt.h). Nothing here touches a foreign medium: a
// custodian's directory is NetworkProvider's FileRelation, reached through
// an Embassy (CryptProvider/Custody.h), and the site is an HttpsRelation the
// same way (CryptProvider/Upstream.h).
#include "CryptProvider/Keypair.h"
#include "CryptProvider/Shard.h"
#include "CryptProvider/Shamir.h"
#include "CryptProvider/Certificate.h"
#include "CryptProvider/Custody.h"
#include "CryptProvider/Envelope.h"
#include "CryptProvider/Registrar.h"
#include "CryptProvider/Upstream.h"

#define CRYPTPROVIDER_CONTRACT__

// auto generated hashes of headers:
#include "../../ETCS.h"
#include "module_hashes.h"

#endif // CRYPTPROVIDER_CONTRACT__
