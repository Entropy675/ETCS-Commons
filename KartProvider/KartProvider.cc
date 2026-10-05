#include "KartProvider.h"

// One type: the race (KartRace.h). The karts and the track are RenderProvider's
// Scene3D nodes and the bumps are the Causal family's; what is a race here is
// the record every runtime steps its world from, and what a key means.
ETCS_MODULE_EXPORT_MAIN(KartProvider, "KartRace")

// HYBRID for the edges, as ChessGame's: a guest's proposals out (Emit), the
// record in (Absorb), the host's judging (Judge), the presence listing
// (Roster), and the window's keys (ConsumeKeys).
ETCS_TAG_BLOCK_HYBRID(KartRace,
    (Create, BindWorld, BindCamera, BindCard, AddKart, Practice, Host, Join, Leave, Tick,
     Key, Run, Status, Standings, Kart, Hash, Snapshot, Note, Delete),
    (Emit, Absorb, Judge, Roster, ConsumeKeys))
