# KartProvider in a browser -- a battle between runtimes

    cd ETCS && ./bin/etcs modules/KartProvider/scripts/serve_kart.etcs
    # then open https://localhost:8445/ in two tabs: host in one, join from the other

Build with `ace wasm make all` and `ace wasm make loader etcs`; the page resolves its modules
from `bin/wasm/`, mounted at `/wasm/`. On the site (`scripts/site_apps.etcs`) the page is `/kart/`.

## Playing

- **Host a battle** and it is listed in the hall; anyone who opens the page sees it and **joins**,
  taking the next free kart of eight. The link under the button joins it directly.
- **W** go, **S** brake and reverse, **A** / **D** turn the wheels left and right (**invert
  steering** on the page swaps the two, for this tab). A press turns them to their lock at
  once; held, they ease on to twice that over a second and a half, so a tap is a firm turn and
  a long hold a hard one; let go, they come back. The kart turns by its speed and the wheels'
  angle, so a kart standing still does not turn, and the faster it goes the less the wheels
  turn: at speed it carves an arc. Click the arena first so it has the keys.
- **Speed builds.** Quick off the line, then slower the faster you go: full speed takes longer
  than a run from one wall to the other, so speed kept is worth more than speed won back.
- **The tyres grip only so hard.** Turn too sharply for your speed (or brake in a turn) and the
  kart slides out of it: a drift, which keeps more of your speed through a corner than braking
  for it. Steer against the slide to catch it. Braking and sliding heat the tyres, and each leaves
  a **skid mark** where it ran, darker the harder it worked: forty units of trail that stay
  while you stand and are wiped from the far end as you drive on, either way.
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
- **The maps:** **bowl** (every wall banked, the banks meeting in the corners, a mound in the
  middle, a ring of bumpers);
  **pillars** (a raised plaza in the middle, ramps up from all four sides, pillars round it,
  kickers at the edge); **crossroads** (a cross of roads and a ring road between buildings of
  different heights, jumps round the outside); **hills** (six hills, a ridge to ride over, two
  jumps, boulders); **skatepark** (two jumps down the middle, a gap jump across it, a raised deck
  either side, the end walls banked, kickers in the corners); each with pads, springs and a deck
  or more in the air, crossroads and hills with a pit; and **random** -- a new layout from a seed
  each time it is chosen (**new layout** on the page, or **Enter** on the map in the menu), the
  same on every tab: banked walls, hills, jumps, raised decks and kickers, pads, springs, a deck
  or two in the air, sometimes a pit in the middle, and blocks.
- **The ground has slopes.** A kart slows going up and gains going down; off a jump's lip at speed
  it flies and lands beyond it. Ground too steep to climb -- a lip met from the drop side, the side
  of a raised deck -- is a wall. Ramps are solid to the ground: nothing is under one.
- **The ground has levels.** A **deck** is a level plate in the air: drive under it, land on it
  from above, and go up through it from below. A **pit** is a floor sunk below the arena's, its
  sides walls: the ramp in it (as high as the pit is deep) or a spring is the way back up.
- **Springs** -- round, their arrows closing on the middle -- throw a kart up five and a half
  units, as hard as a pad pushes one along, and again when it comes down on one. The way onto a
  deck, out of a pit, or just up.
- **Pads** push a kart that drives over them the way their arrows run -- hard, and harder the
  faster it is already going that way, up to 40 u/s, the fastest anything goes. Along its heading
  that is speed, across it a slide to catch. Every map has some, the random one too.
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
block. The ground is not a solid: a map's ramps, hills, pits and decks are a height the battle
keeps each kart at, by how high the kart is (`KartArena.h`). A map is data; the battle draws the
one in play with pools of blocks, wedges, slabs, domes, pads and springs made at boot, so a change
of map is one line of the record. What you see of a kart is a body on four tyres, tilted to the
ground, and an HP bar. The tyres are the body's members, anchored in its frame, so they go where it
goes; frames are translations, so the battle turns them with it after every step, and turns each
tab's bars to that tab's own eye. What the brake and a slide take from a kart's motion is heat in
the rows like any other loss: the battle hands each tyre its share, the tyre sheds it into the
body, and what it sheds per unit of road is the darkness of its mark. The hall is the site's name
server, the one chess lists its tables in (`ChessProvider/scripts/chess_hall.etcs`).