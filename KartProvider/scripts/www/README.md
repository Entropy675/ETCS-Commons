# KartProvider in a browser -- a battle between runtimes

    cd ETCS && ./bin/etcs modules/KartProvider/scripts/serve_kart.etcs
    # then open https://localhost:8445/ in two tabs: host in one, join from the other

Build with `ace wasm make all` and `ace wasm make loader etcs`; the page resolves its modules
from `bin/wasm/`, mounted at `/wasm/`. On the site (`scripts/site_apps.etcs`) the page is `/kart/`.

## Playing

- **Host a battle** and it is listed in the hall; anyone who opens the page sees it and **joins**,
  taking the next free kart of eight. The link under the button joins it directly.
- **W** go, **S** brake and reverse, **A** / **D** turn the wheels -- the kart turns by its speed
  and the wheels' angle, so a kart standing still does not turn. Click the arena first so it has
  the keys.
- **Drive over a gold box** for a weapon off the dice, in place of one you have not used up;
  **Space** fires it. The box is back eight seconds later.
- A kart has **10 HP**, shown over it. Damage follows how hard a hit is to land:

  | weapon  | shots | damage |
  |---|---|---|
  | rocket  | 2  | a burst up to 4.0 at its centre, less further out |
  | mine    | 2  | dropped behind, armed in half a second: a burst up to 3.0 |
  | shotgun | 2  | five pellets in a short fan, 1.0 each |
  | boost   | 1  | two seconds faster; ramming a kart at speed, 2.0 |
  | bomb    | 2  | tossed up and forward, bursts where it lands: up to 4.0 |
  | minigun | 30 | while Space is held, 0.4 each |
  | fuse    | 1  | lit on your kart, bursts round it after 2.5 s: up to 5.0, not to you |
  | sniper  | 1  | an instant line: 10.0 dead on, 5.0 at the edge of a kart |

  Nothing hurts the kart that fired it.
- **A kart at nothing is eliminated**: one to whoever did it, one against the kart, which is back
  three seconds later at the spawn furthest from everyone, whole and unarmed.
- **Between rounds is the lobby.** The karts wait parked at their spawns and the arena shows the
  lobby's menu: the next round's map and length, the drivers and the last round's scores. The host
  moves it with the arrows (up/down choose, left/right change) and starts the round with
  **Enter**, or uses the page's own controls; a guest sees every change and cannot make one.
- **A round's settings are locked** until it is over: the clock runs out (the results stand for
  six seconds, then the lobby, which keeps them), or the host **abandons** it (**Esc**, or the
  page's button) -- back to the lobby, its scores discarded.
- **Hold Tab** in a round for the scores on the arena.
- **The maps:** bowl, pillars and crossroads (flat); **hills** (four rolling hills with a box on
  every top, two jumps); **skatepark** (two jumps down the middle, a raised deck on either side
  with a ramp at each end); and **random** -- a new layout of hills, ramps and blocks from a seed
  each time it is chosen (**new layout** on the page, or **Enter** on the map in the menu), the
  same on every tab.
- **The ground has slopes.** A kart slows going up and gains going down; off a jump's lip at speed
  it flies and lands beyond it. Ground too steep to climb -- a lip met from the drop side, the side
  of a deck -- is a wall.
- Out of a battle, the arena is yours alone: practice, with the lobby yours.

## How it stays one battle

Every tab steps its own copy of the world -- the same arena script built it -- from the host's
record (`KartBattle.h`): `join`, `drive <pedal> <wheel> <trigger>`, `tick <n>`, and the host's
`leave`, `config <map> <secs> [<layout seed>]`, `round <seed>`, `abandon` and `give` lines, in one
order. A guest's keys go to the host's proposals ledger and come back as lines of the record, a
round trip late; the host's own frame edge is the battle's clock. The world is marked driven
(`Scene3D.SetDriven`), so no picture steps it. The karts' rows are the fixed-point Causal rows, and
so is everything the battle adds to them -- the ground's height under each kart, the shots in
flight, the damage, the power-ups' dice (seeded by the `round` line), a random map's layout
(seeded by its `config` line) -- so two tabs fed the same lines are the same battle to the bit.
Each tab advertises a hash of the battle every five seconds with its presence; a guest whose
battle is not the host's at the same tick says so.

The karts are solid spheres to everything (`etcs_causal_constraints.md` §13): they bump each other,
the walls and the maps' blocks with the family's own contact, and a shot stops on a wall or a
block. The ground is not a solid: a map's ramps and hills are a height the battle keeps each kart
at (`KartArena.h`). A map is data; the battle draws the one in play with pools of blocks, slabs and
domes made at boot, so a change of map is one line of the record. What you see of a kart is a body
on four wheels, tilted to the ground, and an HP bar; the battle puts the wheels at the body's
corners after every step (a node's children do not turn with it) and turns each tab's bars to that
tab's own eye. The hall is the site's name server, the one chess lists its tables in
(`ChessProvider/scripts/chess_hall.etcs`).
