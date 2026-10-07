#include "KartProvider.h"

// One type: the battle (KartBattle.h; its weapons in KartArms.h, its maps and
// their ground in KartArena.h). The karts and the arena are RenderProvider's
// Scene3D nodes and the bumps are the Causal family's; what is a battle here
// is the record every runtime steps its world from, and what a key means.
ETCS_MODULE_EXPORT_MAIN(KartProvider, "KartBattle")

// HYBRID for the edges, as ChessGame's: a guest's proposals out (Emit), the
// record in (Absorb), the host's judging (Judge), the presence listing
// (Roster), and the window's keys (ConsumeKeys).
ETCS_TAG_BLOCK_HYBRID(KartBattle,
    (Create, BindWorld, BindCamera, BindCard, AddKart, AddWall, AddMap, AddRandomMap, AddBlock, AddRamp, AddHill,
     AddSpawn, AddPickup, AddBlockNode, AddSlabNode, AddDomeNode, AddPickupNode, AddShotNode, AddBlastNode, AddBoardRow,
     Ready, Practice, Host, Join, Leave, Tick, Configure, Reroll, StartRound, Abandon, Give, Key, Run, Status, Scores,
     Maps, Layout, Kart, Hash, Snapshot, Note, Delete),
    (Emit, Absorb, Judge, Roster, ConsumeKeys))
