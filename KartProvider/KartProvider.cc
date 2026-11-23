#include "KartProvider.h"

// One type: the battle (KartBattle.h; its weapons in KartArms.h, its maps and
// their ground in KartArena.h). The karts and the arena are RenderProvider's
// Scene3D nodes and the bumps are the Causal family's; what is a battle here
// is the trace every runtime steps its world from, and what a key means.
ETCS_MODULE_EXPORT_MAIN(KartProvider, "KartBattle")

// HYBRID for the edges, as ChessGame's: the battle's trace out (Trace) and a
// host's in (Follow), a seat out (Emit) and a guest's in (Take), and the
// window's keys and pointer (ConsumeKeys, ConsumePointer).
ETCS_TAG_BLOCK_HYBRID(KartBattle,
    (Create, BindWorld, BindCamera, BindCard, BindFps, AddKart, AddWall, AddMap, AddRandomMap, AddBlock, AddRamp, AddHill,
     AddPad, AddSpring, AddDeck, AddPit, AddLava, AddSpawn, AddPickup, AddPickupUp, AddBlockNode, AddSlabNode, AddWedgeNode,
     AddDomeNode, AddPickupNode, AddShotNode, AddBlastNode, AddSparkNode, AddSmokeNode, AddFxNode, AddRingNode, AddBoardRow, AddGroundNode, AddMarkNode, AddPadNode,
     AddSpringNode, Ready, Practice, Host, Join, Leave, Tick, Configure, Reroll, StartRound, Abandon, Give, Key,
     SetInvert, Look, Frame, Run, Status, Scores, Maps, Layout, Faults, Kart, Tyres, Eye, Hash, Snapshot, Note, Here, Delete),
    (Trace, Follow, Emit, Take, ConsumeKeys, ConsumePointer))
