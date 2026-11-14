#include "GolfProvider.h"

// One type: the hand on the club (GolfGame.h). The ball, the green and the cup
// are RenderProvider's Scene3D nodes and the physics is the Causal family's;
// what is golf here is what a drag means and what letting go does.
ETCS_MODULE_EXPORT_MAIN(GolfProvider, "GolfGame")

// HYBRID for the edges: main.ProducePointer() -> game.ConsumePointer(), and
// main.ProduceEvents() -> game.ConsumeKeys().
ETCS_TAG_BLOCK_HYBRID(GolfGame,
    (Create, BindWorld, BindBall, BindCamera, BindCup, BindArrow, BindHud,
     SetTee, SetPower, SetOrbit, Reset, Strokes, Press, Drag, Release, Wheel, Pull, Delete),
    (ConsumePointer, ConsumeKeys))
