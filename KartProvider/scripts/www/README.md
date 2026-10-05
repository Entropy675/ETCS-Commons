# KartProvider in a browser -- a race between runtimes

    cd ETCS && ./bin/etcs modules/KartProvider/scripts/serve_kart.etcs
    # then open https://localhost:8445/ in two tabs: host in one, join from the other

Build with `ace wasm make all` and `ace wasm make loader etcs`; the page resolves its modules
from `bin/wasm/`, mounted at `/wasm/`. On the site (`scripts/site_apps.etcs`) the page is `/kart/`.

## Racing

- **Host a race** and it is listed in the hall; anyone who opens the page sees it and **joins**,
  taking the next free kart of eight. The link under the button joins it directly.
- **W** go, **S** brake and reverse, **A** / **D** turn the wheels -- the kart turns by its speed
  and the wheels' angle, so a kart standing still does not turn. Click the track first so it has
  the keys.
- A lap is the line on the near straight after the yellow gate on the far one; the standings
  keep each driver's laps, last and best.
- Out of a race, the track is yours alone: practice.

## How it stays one race

Every tab steps its own copy of the world -- the same course script built it -- from the host's
record (`KartRace.h`): `join`, `drive <pedal> <wheel>`, `tick <n>` and `leave` lines, in one
order. A guest's keys go to the host's proposals ledger and come back as lines of the record, a
round trip late; the host's own frame edge is the race's clock. The world is marked driven
(`Scene3D.SetDriven`), so no picture steps it, and its rows are the fixed-point Causal rows, so two
tabs fed the same lines are the same race to the bit. Each tab advertises a hash of the race every
five seconds with its presence; a guest whose race is not the host's at the same tick says so.

The karts are solid spheres to everything (`etcs_causal_constraints.md` §13): they bump each other
and the walls with the family's own contact. What you see is a body on four wheels; the race puts
the wheels at the body's corners after every step and turns the front two with A and D (a node's
children do not turn with it, so the wheels are the race's to place). The hall is the site's name server, the one chess
lists its tables in (`ChessProvider/scripts/chess_hall.etcs`).
