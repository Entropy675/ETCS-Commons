# GolfProvider in a browser -- golf with anyone

    cd ETCS && ./bin/etcs modules/GolfProvider/scripts/serve_golf.etcs
    # then open https://localhost:8443/ in two tabs: host in one, join from the other

Build with `ace wasm make all` and `ace wasm make loader etcs`; the page resolves its modules
from `bin/wasm/`, mounted at `/wasm/`. A match of your own in a window: `./bin/etcs
modules/GolfProvider/scripts/golf.etcs`.

## Playing

- **Host a match** and it is listed in the hall; anyone who opens the page sees it and **joins**,
  taking the next free ball of eight. The link under the button joins it directly. Out of a
  match the course is yours alone: practice, with the lobby yours.
- **Everyone plays the hole at once**, every ball on the green together -- and solid to each
  other: knock a rival off its line.
- **Drag your ball back** to aim: the arrow growing out of it is the shot -- opposite the drag, as
  long as it (to a cap), green to red with power. Let go to shoot. **Drag anywhere else, or with
  the right button,** to walk the camera round your ball; the **wheel** brings it in and out.
  **A shot starts along the ground your ball is on**, wherever the camera is when you take hold
  of it -- on a slope, along the slope -- but not pinned to it: the drag's own rise off the
  ground comes through damped, none of it for a tap and half of it at a full pull, so a putt
  stays down and a long pull lifts. **More loft is what the camera does after**: hold the
  right button with the left and bring the camera down, and the shot tips up by as much more;
  bring it back up and it is back to the drag's own, and no lower.
- **Let go of the right button while the left holds** a shot or a power and it is put down: the
  left's release then does nothing.
- **Powers.** Roll through a gold box (they turn, as the kart's do) for one. **You carry two**:
  the one in hand and a spare -- a box with a power in hand goes to the spare, in place of what
  was there, and the spare comes to hand when you use the one in hand. **E** arms it and a
  **click** puts it down where you point, no further than thirty from your ball. **One that
  works for a while works for half a minute** (or until the hole is over): everyone plays at
  once, so it is there to be played round:
  - **pad** -- the kart's booster, pushing the way you look;
  - **bumper** -- a short wall across your view;
  - **spring** -- the kart's spring: throws a ball rolling onto it up;
  - **sand** -- a patch that drags hard;
  - **black hole** -- pulls every ball near it in;
  - **wind** -- blows every ball on the hole the way you look;
  - **teleport** -- your ball to where you point, ten at most;
  - **tornado** -- wanders, swirling balls near it, while lightning strikes round it: each
    strike a burst whose energy spreads as it goes, so a ball takes the share its size
    intercepts, a quarter of it through a wall;
  - **magnet** -- the cup pulls your own ball toward it from as far as ten off, slowly enough
    to drop in;
  - **cannon** -- your ball, at rest, fired in an arc to where you point (twenty at most), and
    down dead where it lands; no stroke;
  - **freeze** -- every other ball in the cold: it still plays, and its shots carry about a
    third as far.
  - **smoke** -- a ring of smoke round your ball, spreading out to six over five seconds and
    on to seven and a half as it thins away over fifteen: it hides the green from across it.
- **A round** is the course's holes in order, each with a time (the lobby's setting). A hole is
  over for a ball when it drops in, or when it comes to rest after eight strokes (scored nine);
  for everyone when every ball is in, or the time runs out (anyone still out scored nine). A ball
  that falls off goes back where it was struck, a stroke added. **Lowest total wins**; the little
  board, top right, keeps it.
- **The lobby's menu is drawn in the canvas**: the host's **course** and **time a hole**, **your
  ball's colour and your name** (yours, whoever you are), the first hole from above, and
  **start** (**Enter**). The arrows and the pointer both move it; **M** (or its last button)
  folds it away, and then anyone can knock a ball about on the first hole, uncounted. The page
  has the same controls in HTML, and neither is where the state is: each says a line of the
  trace (`config`, `look`, `round`), so a change made in one shows in the other, on every
  tab. In a round the host can **abandon** it (**Esc**).
- **Settings: O**, or the lobby's settings button, opens them over the course, in a round or
  not: how fast a drag turns the camera and how far the wheel brings it in. They are this
  tab's own, kept with your name, and none of them is the match's.
- **The cup is round**: what a ball drops through is still a square gap (the ground is boxes),
  under a rim that covers its corners and over a round well.

## The courses

- **classic** -- the flat one (a block round, a ridge over), the step down (off a terrace, down a
  slope), the climb (up to a terrace, past a sand trap).
- **boosters** -- the kart's pads and springs: a belt of pads between blocks; a wall right across
  with a spring to clear it; a gap in the green two down and eight on, a pad to fling you over.
- **terraces** -- down a staircase; up a climb whose slopes roll you back; a zigzag through
  part-walls with sand before the cup.
- **ridge** -- high ground at dusk (teal turf, slate walls, a violet sky): a narrow hump; an
  elbow, off the lane's side down a slope to a long shelf; a wide valley that narrows to a
  climb.
- **channels** -- a clay court in the evening (terracotta, teal walls, a warm sky), level all
  the way: a chicane; a cross of four arms with the cup round the corner; a funnel.
- **random** -- four holes from a seed: walled lanes, some with a terrace up or down a slope,
  blocks, sand, pads up the lane, a spring before a block, in one of four looks by its seed.
  **new course** rolls another.

A course has **its own colours** -- turf, walls, sand and the sky (`SetTheme`); one without
has the first courses' green and blue.

## How it stays one match

Every tab steps its own copy of the course -- the same scripts built it -- from the host's trace
(`GolfMatch.h`): `join`, `shot <x> <y> <z>`, `use <x> <y> <z> <yaw>`, `leave`, `tick <n>`, and the
host's `config`, `round`, `abandon` and `give` lines, in one order. A guest's shots and powers go
to the host's match on the guest's own seat (a stream into it) and come back as lines of the
trace, a round trip late; the host's own frame edge is the match's clock. The world is marked driven (`Scene3D.SetDriven`), so no
picture steps it. The balls are the Causal family's free solid spheres on anchored solid greens:
gravity is the world's space's, the bounce and the roll are the contact's, and the cup is a
container a ball that drops in fits -- fit taking it in is what holed means
(`ontology/etcs_causal_constraints.md` §11-§13). What the match adds -- pads, springs, sand, the
powers, the dice they are picked by -- is in the same fixed point, so two tabs fed the same lines
are the same match to the bit; the host keeps its hash as a line every five seconds, and a guest
that is not the host's there says so. The session itself is the ontology's (`Trace` ->
`Lockstep`, and `Present`) and NetworkProvider's (`session_*.etcs`, `session_page.js`): the kart's battle is the
same session.

A course is data (`golf_course_*.etcs`: `AddCourse`, then `AddHole` and each hole's slabs, cup,
tee, pads, springs and spots); the match lays the hole in play from pools made at boot
(`golf_pool.etcs`), so a new hole is a tick of the trace, not a script. A cup cuts the level
green under it round a square gap. `game.Faults()` says what is wrong with a course -- a cup not
in a level green, something standing on nothing, no ground all the way from a tee to its cup
on a hole with no pad or spring to throw a ball, more slabs than the pool -- or `none`.

From the terminal, while it runs: `game.Status()`, `game.Scores()`, `game.Ball()`,
`game.Layout()`, `game.Give(<you> tornado)` (alone, you are the host), `world.SetGravity(0, -1.6, 0)`
(the moon).
