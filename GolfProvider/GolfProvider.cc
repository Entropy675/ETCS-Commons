#include "GolfProvider.h"

// One type: the match (GolfMatch.h; its courses in GolfCourse.h, its powers in
// GolfPowers.h). The balls and the greens are RenderProvider's Scene3D nodes
// and the physics is the Causal family's; what is golf here is the trace
// every runtime steps its course from, and what a drag means.
ETCS_MODULE_EXPORT_MAIN(GolfProvider, "GolfMatch")

// HYBRID for the edges, as KartBattle's: the match's trace out (Trace) and a
// host's in (Follow), a seat out (Emit) and a guest's in (Take), and the
// window's pointer and keys.
ETCS_TAG_BLOCK_HYBRID(GolfMatch,
    (Create, BindWorld, BindCamera, BindCard, BindFps, BindArrow, BindGhost, AddBoardRow, SetPower, SetOrbit,
     AddBall, AddSlabNode, AddCupNode, AddTeeNode, AddSpotNode, AddPuffNode, AddPadNode, AddSpringNode,
     AddCourse, AddRandomCourse, AddHole, AddGreen, AddWall, AddSand, AddCup, AddTee, AddPad, AddSpring, AddSpot, Ready,
     Practice, Host, Join, Leave, Tick, Configure, Reroll, StartRound, Abandon, Give,
     Press, Drag, Release, RightPress, RightRelease, Wheel, Key, Shoot, Use, Frame, Run,
     Status, Scores, Maps, Layout, Faults, Hash, Snapshot, Note, Here, Ball, Pull, Delete,
     BakeCup, SetTheme),
    (Trace, Follow, Emit, Take, ConsumePointer, ConsumeKeys))
