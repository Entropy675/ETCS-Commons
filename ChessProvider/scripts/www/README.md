# ETCS Chess in the browser -- a board in your runtime, a table on somebody's

This page is an ETCS runtime with a chess board in it. The board is drawn by
that runtime (a `ChessBoard` in a window whose surface is the canvas), clicked
through that runtime (the window's pointer stream, `ChessTable`), and judged
by whichever runtime hosts the table -- yours, another player's, or the site's.
What is HTML here is what a window has no DOM for: the hall, the seats, the
conversation and the line you type. On the OS those are the bars
`scripts/chess_table.etcs` slides in; here they are the column beside the
board. The same boot script sets up both (`boot_chess.etcs`).

Serve it with `serve_chess.etcs` (this module) or as part of the site
(`scripts/run_website.etcs`, under `/chess/play/`). ETCS serves ETCS: the page,
the runtime and the link hub come from one `HttpServer`.

## The two models, one shape

A game is a **record** (a `Ledger` on the host's runtime) and **one board that
judges** it. Every other board **follows** the record: what it proposes goes to
the host's `proposals` ledger, authored by the link it came over; the judge
applies each line as its author and appends to the record only what the board
took; every board applies the record in its order. Two boards fed the same
lines by the same engine are the same board, so there is nothing to agree
(`ChessGame.h`).

Who the judge is decides the arrangement and nothing else:

- **Casual, peer to peer.** A player opens a table (`chess_host.etcs`): their
  board judges, their runtime hosts the room through the site's hub. Others
  join it (`chess_join.etcs`). The site is the name server only.
- **Ranked, server-mediated.** The site's runtime hosts a table
  (`chess_ranked_table.etcs`, via `chess_service.etcs`): its board judges,
  holding no seat -- the ACE server as one more observer, in front of the
  record. Players join it with the same `chess_join.etcs`, the table's name as
  the owner. The outcome is the judge's (`ChessGame::outcomeLocked`), and the
  table is reset for the next two once they have gone (`ChessShare::roster`).

Same type, same streams, same scripts. `modules/ChessProvider/scripts/session/run.sh`
runs both, back to back, between three native runtimes -- ETCS hosting ETCS,
no browser in the loop.

## The hall

The site publishes a `Lobby` in a `Room` on its hub (`/link/hall`): the name
server. Everyone links to it and lists it (`chess_hall.etcs`, mirrored into a
local Lobby the page polls). A person hosting advertises there --
`<name> chess <id> <white> <black>` -- and the entry goes when their link does;
a ranked table is advertised by the site as `<name> ranked <name> ...`. A row
with a seat open is a join; a full one can be watched. The hall is a service of
its own: a site that runs only the hall still finds casual games their
partners, and judges nothing.

## What the page does, and what it does not

The page fills templates and runs them (`runTemplate`): `chess_hall.etcs` once
the runtime is up, `chess_host.etcs` / `chess_join.etcs` for a table, with the
boot's globals injected (`game=game share=share hall=halllist0`) since a later
root script cannot name them. It beats `share.Tick` once a second while at a
table (presence, and the hall entry for a host). It polls `game.Status`,
`game.Tail`, `table.Report` and the hall mirror's `List` to draw the column.
The line you type is `ChessTable.Do`: a leading slash is a verb (`/sit black`,
`/resign`, `/draw`, `/flip`, `/fen ...`), the rest is said. The seat is the
table's, so the page needs to know neither your name nor whether it is a verb.

The page never touches the board. It does not know where a piece is, which
square was clicked, or whose move it is except as text to show. GLFW owns the
canvas; a press reaches `ChessTable::Click` through the window's pointer
stream, and the board redraws itself when the game changed
(`ChessBoard::AnimatingConcrete`).

`?s=<id>&owner=<name>` joins that table (the link a host copies);
`?ranked=1` sits at the first ranked table with a seat; `?quick=1` joins the
first table with a seat, or opens one; `?host=1` opens one.

## The call bridge

`etcs_web_call(name, verb, args)` runs a verb inline on the calling thread and
answers whether the name was live. `etcs_web_call_async` runs it on a pool
worker and answers through `window.__etcs_call_done` -- `callResult` in the
page. Anything that waits on the link goes through the async form, because the
page's event loop is the thread GLFW delivers every press on.

## Files

- `index.html` -- the page. Its bootstrap (manifest, wasm base, staging, glue)
  is PaintProvider's page's, the same contract.
- `modules.json` -- what is loaded and staged; the boot script; the glue.
- `boot_chess.etcs` -- the seat: window, surface, board, game, share, table,
  the two pumps. The same file the OS runs (`scripts/chess_table.etcs`).
- `../chess_mounts.etcs` -- the served surface, one list for every server.
- `../chess_service.etcs`, `../chess_ranked_table.etcs` -- the site's side.
- `../session/run.sh` -- both models, natively, end to end.
