# GolfProvider in a browser -- three holes

    cd ETCS && ./bin/etcs modules/GolfProvider/scripts/serve_golf.etcs
    # then open https://localhost:8443/

Build with `ace wasm make all` and `ace wasm make loader etcs`; the page resolves its modules
from `bin/wasm/`, mounted at `/wasm/`. The same course in a window: `./bin/etcs
modules/GolfProvider/scripts/golf.etcs`.

## Playing

- **Drag the ball back** to aim: the arrow growing out of the ball is the shot -- opposite the
  drag, as long as it (to a cap), green to red with power. Let go to shoot, or **press the
  right button** while holding it to put the pull down: no shot, no stroke.
- **Drag anywhere else** to walk the camera round the ball; the **wheel** brings it in and out;
  **R** starts the round again (the first tee, the card clear) and **N** goes on to the next
  hole.
- **The camera's pitch is the loft.** The pull is read on the plane through the ball facing the
  camera, so looking down at the ball is a putt and looking level is a lob.
- **The ball has a shadow** on whatever is straight under it: the gap between the two is how
  high the ball is.
- **Three holes, played in order**, side by side. A ball that drops into the cup is counted
  and set on the next hole's tee; after the third the round's strokes are shown and the first
  comes round again. One that leaves the course, or drops into another hole's cup, goes back
  to its tee with a stroke added. The card shows the hole, its strokes and the round's.
- **The holes have levels.** The first is flat: a block to play around, a ridge to play over.
  The second starts on a terrace a unit and a half up and falls down a slope to the cup's
  level: the ball gathers pace all the way, so the shot is how little to give it. The third
  climbs a slope to a terrace a unit up, where the cup is -- too soft and the ball rolls back
  down -- and a sand trap half a unit deep is cut into the green in the straight line.

From the terminal, while it runs: `game.Card()`, `game.PlayHole(2)`, `game.Strokes()`,
`game.Reset()`, `game.SetPower(3, 16)`, `game.SetOrbit(10, 30, 50)`, `game.Pull()`,
`world.SetGravity(0, -1.6, 0)` (the moon).

## What is where

The page is RenderProvider's 3D page, mounted (`golf_mounts.etcs`), not copied: it loads what
`modules.json` names and runs `boot_golf.etcs`. The course (`golf_course.etcs`) is the same
script the window runs: it runs a script a hole (`golf_hole_1.etcs` to `golf_hole_3.etcs`),
each of which builds its green and gives the game its cup and its tee (`AddHole`), and hands
the ball and the arrow to `golf_play.etcs`. A cup and its flag are `golf_cup.etcs`, a tee's
mark `golf_tee.etcs`. A fourth hole is a fourth script and a line in the course.

None of the physics is golf's. The world's space states a field (`SetGravity`) and a speed of
light (`SetLightSpeed`, 16 a second: a little over the hardest shot, and nothing on the course
moves faster); the green and walls are anchored solids, the ball a free solid sphere
(`SetSolid`, `SetShape`); the cup is a container under a real gap in the green, with its own
floor inside its own space -- a ball that drops in fits that space, fit takes it in, and that
move is what "holed" means (`ontology/etcs_causal_constraints.md` §11-§13). A hole's levels
are the same solids: a terrace is a thick slab, a slope a slab turned about x, a trap a floor
under a cut in the green. A ball meets a solid where its path first touches it, however far
one frame moves it (§13, the cut; §14, the smear): a lob on a slow frame comes down on the
green, not through it, and a drive is turned back by a bumper thinner than its step, from
where it met it and at the angle it met it. `GolfGame` reads the rows and writes only through
verbs: the shot is the ball's own `Impulse`.
