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
- **Drive over a gold box** for a weapon off the dice; **Space** fires it. The box is back eight
  seconds later. **You carry two**: the one in hand and a spare. A box with a weapon in hand
  goes to the spare, in place of what was there, and the spare comes to hand when the one in
  hand is spent. **The same weapon again is more of it** -- its ammo on top, up to three boxes'
  worth -- whichever of the two it is.
- A kart has **10 HP**, shown over it. Damage follows how hard a hit is to land:

  | weapon  | shots | damage |
  |---|---|---|
  | rocket  | 2  | flies until it has crossed the arena; a burst up to 4.0 at its centre |
  | mine    | 2  | dropped behind, armed in half a second: a burst up to 3.0; left alone it blinks and bursts by itself after twenty seconds |
  | shotgun | 2  | five pellets in a short fan, 1.0 each |
  | boost   | 1  | two seconds faster; ramming a kart at speed, 2.0 |
  | bomb    | 2  | tossed up and forward, bursts where it lands: up to 4.0 |
  | minigun | 30 | while Space is held, 0.4 each; a round flies until it has crossed the arena |
  | fuse    | 1  | lit on your kart, bursts round it after 2.5 s: up to 5.0, not to you |
  | sniper  | 1  | an instant line: 10.0 dead on, 5.0 at the edge of a kart |
  | homing  | 2  | a slower rocket that turns to the nearest kart ahead of it, trailing smoke: a burst up to 3.0 |
  | oil     | 1  | a slick dropped behind for twelve seconds: on it a kart has a sixth of its grip and a third of its pedal |
  | shockwave | 1 | a ring out from your kart to nine: 1.5 to every kart it passes, and thrown outward |

  Nothing hurts the kart that fired it. **A burst is a tree of fire**: branches grow out from
  it along lines, forking, and bend round what is dense -- a block, a wall, the ground, a
  deck -- rather than through it, losing strength with every unit they go. A kart is hurt by
  the strongest branch that reaches it, so in the open that is the old falloff, and **behind
  a block you are covered**. Every tab grows the same tree. The fire leaves **smoke**: dark
  puffs where its branches burned and big pale clouds in the gaps between them, which swell as
  they rise and thin away.
- **A kart at nothing is eliminated**: one to whoever did it, one against the kart, which is back
  three seconds later at the spawn furthest from everyone, whole and unarmed.
- **Between rounds is the lobby.** The karts wait parked at their spawns and the arena shows the
  lobby's menu: the next round's map and length, the drivers and the last round's scores. The host
  moves it with the arrows (up/down choose, left/right change) and starts the round with
  **Enter**, or uses the page's own controls; a guest sees every change and cannot make one.
- **A round's settings are locked** until it is over: the clock runs out (the results stand for
  six seconds, then the lobby, which keeps them), or the host **abandons** it (**Esc**, or the
  page's button) -- back to the lobby, its scores discarded.
- **Hold Tab** in a round for the scores on the arena. The top right corner counts this tab's
  **frames a second**.
- **Look round the kart** with **Q** and **E**, or by dragging with the right button (up and
  down too): the eye swings round the kart and, let alone, drifts back behind it. It also keeps
  to the kart's side of the ground -- down in a pit with it, under a deck with it -- instead of
  looking at it through a wall.
- **The arena** is 192 units across inside its walls, which are four high: below their top they
  stop you; over it -- off a spring at speed, a jump, a storey's edge -- you have left the
  arena, and a kart that falls outside it is out, as one that comes down in lava is.
- **The maps:** **bowl** (every wall banked, the banks meeting in the corners with a pad on each
  bank either side of every corner pointing away along it -- drift up into a corner and you are
  sent round the bowl one way or the other by whichever you come down on -- a ring road, and
  the whole middle sunk: a mound, bumpers, a ramp up each side, a deck in the air over the
  mound); **pillars** (two storeys: a raised plaza and eight pillars on the ground, a deck over
  a third of the arena on the pillars, a long ramp up through it from each side); **crossroads**
  (a cross of roads and a ring road between buildings of different heights, and a roof over the
  whole north half, a ramp up to it from each end of the north ring road); **hills** (the south
  half sunk, hills and boulders on both levels, three ramps up, a ridge, two jumps, and a deck
  in the air over the north-east); **skatepark** (two jumps down the middle, a gap jump across
  it, a raised deck either side, the end walls banked, and a gallery in the air along each side
  wall with a long ramp up to each end); **lava** (a pool of it in the middle: an island with
  a causeway to each shore, a deck over the island with a scaffold over each causeway -- two
  ways across, to two heights -- and two ring roads in the air round it, the higher along the
  arena's wall); each with pads and springs; and **random** -- a new
  layout from a seed each time it is chosen (**new layout** on the page, or **Enter** on the
  map in the menu), the same on every tab: a storey on pillars with ramps up through it, one
  time in two a third to two thirds of the arena sunk with ramps out, banked walls, hills,
  jumps, raised decks and kickers, pads, springs and blocks.
- **The ground has slopes.** A kart slows going up and gains going down; off a jump's lip at speed
  it flies and lands beyond it. Ground too steep to climb -- a lip met from the drop side, the side
  of a raised deck -- is a wall. Ramps are solid to the ground: nothing is under one.
- **The ground has levels, and they are big**: a storey over a third of a map, half a map sunk.
  Decks are three quarters of a unit thick, and what holds one up runs into it.
  A **deck** is a level plate in the air with the floor still there under it: drive under it,
  land on it from above, go up through it from below off a spring, or drive up a ramp that
  comes up through a cut in it or meets its edge; any edge is a way down. **Low ground** is a
  floor sunk below the arena's, its sides walls, with whatever a floor has -- hills, blocks,
  pads, boxes, spawns: a ramp in it as high as it is deep, its top at the edge, is a way out
  (several, on every map that has any), and so is a spring.
- **Springs** -- round, their arrows closing on the middle -- throw a kart up: a hop standing,
  and half its speed more on top, so five and a half units (onto a storey) at 15 u/s and over
  the arena's wall at full speed; again when it comes down on one. The way onto a deck, out of
  a pit, or out of the arena.
- **Pads** push a kart that drives over them the way their arrows run -- a nudge standing, and
  harder the faster it is already going that way (four times its speed a second on top), up to
  40 u/s, the fastest anything goes. Along its heading that is speed, across it a slide to
  catch. A pad lies on level ground or on a slope -- a bank's. Every map has some, the random
  one too.
- Out of a battle, the arena is yours alone: practice, with the lobby yours. The lobby opens
  when the arena is made: until then the page offers no maps.

## How it stays one battle

Every tab steps its own copy of the world -- the same arena script built it -- from the host's
trace (`KartBattle.h`): `join`, `drive <pedal> <wheel> <trigger>`, `leave`, `tick <n>`, and the
host's `config <map> <secs> [<layout seed>]`, `round <seed>`, `abandon` and `give` lines, in one
order. A guest's keys go to the host's battle on the guest's own seat -- a stream into it, the
only way in the room publishes besides its trace -- and come back as lines of the trace, a round
trip late; the seat's stream ending is its `leave`. The host's own frame edge is the battle's
clock. The world is marked driven
(`Scene3D.SetDriven`), so no picture steps it. The karts' rows are the fixed-point Causal rows, and
so is everything the battle adds to them -- the ground's height under each kart, the shots in
flight, the damage, the power-ups' dice (seeded by the `round` line), a random map's layout
(seeded by its `config` line) -- so two tabs fed the same lines are the same battle to the bit.
Every five seconds the host keeps a hash of the battle as a line of its trace; a guest reaching
that line whose own battle is not the host's says so. None of that session is the kart's: it is the
ontology's (`Trace` -> `Lockstep`, and `Present`), the templates the page runs are
NetworkProvider's (`session_host.etcs`, `session_join.etcs` and their streams), and so is the page's
half (`session_page.js`) -- golf's match is the same session.

The karts are solid spheres to everything (`etcs_causal_constraints.md` §13): they bump each other,
the walls and the maps' blocks with the family's own contact, and a shot stops on a wall or a
block. The ground is not a solid: a map's ramps, hills, pits and decks are a height the battle
keeps each kart at, by how high the kart is (`KartArena.h`). A map is data; the battle draws the
one in play with pools of blocks, wedges, slabs, domes, pads and springs made at boot, so a change
of map is one line of the trace. `battle.Faults()` says what is wrong with a map -- a pad off
one piece of ground, a block across a low ground's edge, low ground with no way out, a spawn in
lava, more of a thing than its pool draws -- or `none`. What you see of a kart is a body on four tyres, tilted to the
ground, and an HP bar. The tyres are the body's members, anchored in its frame, so they go where it
goes; frames are translations, so the battle turns them with it after every step, and turns each
tab's bars to that tab's own eye. What the brake and a slide take from a kart's motion is heat in
the rows like any other loss: the battle hands each tyre its share, the tyre sheds it into the
body, and what it sheds per unit of road is the darkness of its mark. The hall is the site's name
server, the one chess lists its tables in (`ChessProvider/scripts/chess_hall.etcs`).