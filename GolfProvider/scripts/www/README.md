# GolfProvider in a browser -- one hole

    cd ETCS && ./bin/etcs modules/GolfProvider/scripts/serve_golf.etcs
    # then open https://localhost:8443/

Build with `ace wasm make all` and `ace wasm make loader etcs`; the page resolves its modules
from `bin/wasm/`, mounted at `/wasm/`. The same hole in a window: `./bin/etcs
modules/GolfProvider/scripts/golf.etcs`.

## Playing

- **Drag the ball back** to aim: the arrow growing out of the ball is the shot -- opposite the
  drag, as long as it (to a cap), green to red with power. Let go to shoot.
- **Drag anywhere else** to walk the camera round the ball; the **wheel** brings it in and out;
  **R** puts the ball back on the tee and clears the card.
- **The camera's pitch is the loft.** The pull is read on the plane through the ball facing the
  camera, so looking down at the ball is a putt and looking level is a lob.
- A ball that drops into the cup is counted and set back on the tee; one that leaves the
  course is set back with a stroke added.

From the terminal, while it runs: `game.Strokes()`, `game.Reset()`, `game.SetPower(3, 16)`,
`game.SetOrbit(10, 30, 50)`, `game.Pull()`, `world.SetGravity(0, -1.6, 0)` (the moon).

## What is where

The page is RenderProvider's 3D page, mounted (`golf_mounts.etcs`), not copied: it loads what
`modules.json` names and runs `boot_golf.etcs`. The course (`golf_course.etcs`, which hands its
ball, cup and arrow to `golf_play.etcs`) is the same script the window runs.

None of the physics is golf's. The world's space states a field (`SetGravity`); the green and
walls are anchored solids, the ball a free solid sphere (`SetSolid`, `SetShape`); the cup is a
container under a real gap in the green, with its own floor inside its own space -- a ball that
drops in fits that space, fit takes it in, and that move is what "holed" means
(`ontology/etcs_causal_constraints.md` §11-§13). `GolfGame` reads the rows and writes only
through verbs: the shot is the ball's own `Impulse`.
