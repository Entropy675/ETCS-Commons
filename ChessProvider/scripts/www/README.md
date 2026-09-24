# ETCS Chess in the browser -- your lobby, and a partner a name server finds

    etcs modules/ChessProvider/scripts/serve_chess.etcs
    then open https://localhost:8444/

Build only -- nothing is copied beside the page. `ace wasm make` writes one
copy of each artifact into `bin/wasm/`, `serve_chess.etcs` mounts that
directory at `/wasm/`, and the page resolves it there (`WASM_BASE_CANDIDATES`
/ `resolveWasmBase`, index.html: `/wasm/` first, then the page's own
directory, then the hosted site). Same for `/favicon/`, mounted from
`scripts/www/favicon`, which is where the marks in the page head come from.

    ace wasm make module ChessProvider
    ace wasm make module NetworkProvider
    ace wasm make module ShellProvider
    ace wasm make loader etcs

(`ace wasm make modules` builds only the modules whose own manifest declares
`"Web"`; ChessProvider now does. `ace wasm make loader etcs` always produces
the interactive build -- `-DETCS_REPL_SHELL` is on by default for every
loader build now, for the reason PaintProvider's own README gives: a wasm
loader that has returned from `main` is a page nothing can call into.)

## What changed to make this possible

Nothing in ChessGame or ChessLobby's own game logic did. The verb surface
that already exists -- `Request`, parsing `<mount>/<self>/<match>/<verb>[/<arg>]`
exactly as it does for the HTTP page -- is what this page calls too, just
locally instead of over a socket. `chess_basic_example.etcs` already proved
the shape this page relies on: two distinct selves, `white` and `black`,
claiming their seats by MOVING (`applyMoveLocked`), on the same board, with
no node-to-node networking anywhere in the loop. That script is a hotseat
game with a human reading the trace; this page is the same script, driven by
clicks instead of by hand.

`ChessNode` needed one addition (below), and three general-purpose things
changed too:

- **`manifests/ChessProvider.json`** now declares the `Web` platform and
  `em++` as its compiler, same shape as PaintProvider's manifest. CChess's
  own view classes (`NcView`, `WinView`, the ncurses/Windows split) were
  already excluded from the vendored source list -- ChessProvider only ever
  wraps `Board`/`Piece`/`MoveBehaviour`, which are "plain, dependency-free
  C++: no ETCS types, and no platform split" (ChessGame.h's own comment on
  why). Nothing there needed a guard for `__EMSCRIPTEN__`; it compiles
  unmodified.
- **`loaders/etcs.cc`** gained `etcs_web_call_async`, next to the existing
  `etcs_web_call`. That function's own comment explains why it exists: every
  page in this tree so far only ever fired verbs whose effect was visible
  some other way (a redraw, a file written), so nobody had needed the
  work function's actual ANSWER back in JS yet. Chess is built entirely out
  of verbs whose only output IS their answer -- a move's point is the FEN it
  produces -- so this page is what finally needed the missing half of that
  bridge. It is additive and generic: any future page whose verbs return
  values wants it too, not just this one.

  The first shape this took (`etcs_web_call_result`) got the answer back by
  blocking: a caller-owned output buffer the work function's response was
  copied into before the `ccall` returned. That is fine for a call made from
  a `ThreadPool` worker -- the native path every other blocking wait in this
  tree already uses -- and fatal for a call made from page JS, because
  `Module.ccall` runs the whole export inline on whichever thread calls it,
  which for a click handler is the browser's one real main thread. A chess
  move blocks on `ChessOpEvent` until its `EventStream`'s ordering thread
  answers, and blocking that thread froze the tab every time it was tried
  (not a theory -- reproduced and confirmed before this async version
  existed). `etcs_web_call_async` fixes this at the source: it only
  *enqueues*, onto a `ThreadPool` worker where blocking is exactly what the
  thread is for, and answers by calling back into the page
  (`window.__etcs_call_done`) once the worker is done -- the same way
  `etcs_web_shell_write` already reaches the page from off-main-thread
  output. The page's own `callResult()` wraps that callback in a Promise, so
  the rest of this file reads like the synchronous version it replaced.
- **`loaders/etcs.cc`** also gained the buffered-terminal-output rewrite
  (`shell_out_append`/`etcs_web_shell_drain`, chunking `std::cout` instead of
  proxying it to the main thread per line) -- not added for this page, but
  this page's own `drainShellOutput()`/`startShellDrain()` (below) is how it
  opts in to that rewrite rather than silently falling back to the old
  per-write push path.

**`ChessNode.h`** needed one line: `stream_.arm_emscripten_ordering_thread()`,
called right after `stream_.start(...)` inside `ChessNode::stream()`'s
`call_once`. `EventStream::start()` cannot tell "mid-preload" from "long
after boot" from inside itself under emscripten's per-module dynamic
linking -- see that function's own comment in `core/EventStream.h` for why
two different attempts at teaching it to guess both failed -- so it always
starts in `sync_emscripten_` mode, correct only for a stream that exists
before `etcs_boot_runtime_threads()`'s one-time promotion pass runs.
`ChessNode`'s stream is created lazily, on first use, which for a script-
driven boot is always well after that pass -- so without this call it stays
in `sync_emscripten_` mode forever, and any blocking `ChessOpEvent` wait
against it (from `etcs_web_call_async`'s `ThreadPool` worker) spins with no
error and no timeout. This is the fix that makes moves actually resolve.

## Why no canvas

PaintProvider's page needs `WindowProvider.wasm` and `RenderProvider.wasm`
because it draws pixels: a `Surface` bound to a canvas via GLFW, redrawn
every frame. ChessGame has no `Drawable2D` base and no render path at all --
it has never drawn anything, on any substrate. The server-authoritative
`ChessProvider/www/chess.html` already proved the right shape for that: a
CSS grid of 64 `<div>`s, drawn from a FEN string in plain JS. That is what
this page still does; only where the FEN comes from changed, from a `fetch()`
poll to `callResult('game', 'Request', ...)`.

`modules.json` therefore lists no window and no renderer -- the same
minimal set ShellProvider's own standalone page uses
(`ShellProvider/scripts/www/README.md`: "and optionally
WindowProvider.wasm"). A page with no canvas has nothing for GLFW to own, so
there is nothing to leave out by omission and nothing to add back later
short of the board actually wanting to be drawn as pixels instead of DOM.
It lists `ChessProvider.wasm`, `NetworkProvider.wasm` and
`ShellProvider.wasm`; NetworkProvider is there for a reason that has
nothing to do with drawing -- "Name servers, all the way down", below.

## The call bridge: `etcs_web_call_async`

    let handle = 0;
    window.__etcs_call_done = (h, ok, text) => { if (h === handle) console.log(ok, text); };
    handle = 1;
    Module.ccall('etcs_web_call_async', null,
                 ['number','string','string','string'],
                 [handle, 'game', 'Request', 'game/white/local/move/e2e4']);

is the JS spelling of `game.Request("game/white/local/move/e2e4")`, answered
asynchronously instead of thrown away. `callResult()` in this page's script
wraps exactly that sequence in a `Promise`, keyed by an incrementing
`handle` so several calls can be in flight at once. The C++ side does the
actual blocking wait -- `ChessOpEvent::operator()`, the same wait a native
`HttpServer`'s `ThreadPool` worker already does for every HTTP request --
but it does it on a `ThreadPool` worker the enqueue lands on, never on the
thread `Module.ccall` was called from. That distinction is why this bridge
is async at all: proxying the call to the pthread hosting `main()` and
blocking IT was the first thing tried, and it is exactly the thread every
other pthread worker in this loader's `PROXY_TO_PTHREAD` build needs pumped
to make progress, so blocking it there is a deadlock, not a slow path (see
above).

## Your lobby, and how a seat is taken

Every person has a lobby: this page's own runtime, one board, two seats. You
have a name from the first visit -- made up, kept in the browser, shown in the
header and renamed by pressing it -- because it is what a name server lists
you as and what a link to your lobby opens.

**Taking a seat is making a move for a side.** A click as the side to move
sits you there if the seat is open -- claim by moving
(`ChessGame::applyMoveLocked`), the server page's own model -- so a board is
playable the moment it is up. The `Sit` button on each seat card is the same
seat taken without moving (`sit/<white|black>`, `ChessGame::sitLocked`, same
rule: a held seat is its holder's, and one self never holds both). A seat you
hold shows Resign, Draw (the label says whether it offers or accepts --
`drawLocked` is one verb both ways) and leave.

Alone, the two seats are the colours' own selves, `white` and `black`, so the
board is a hotseat: moving for either side sits you in it. The status line
names who holds each seat (its seventh and eighth fields), and the cards are
drawn from that and nothing else -- a seat is the board's fact, not the page's
bookkeeping. The board turns for black: the seat you hold, or with none, the
one your first move would take.

## The name server, and a partner

The panel beside the board is a NAME SERVER: any ChessNode, at the address in
its field -- by default whoever served this page, at `/game`
(`serve_chess.etcs` and `chess_web.etcs` both mount one). It holds no board.
It does three things, all verbs on the node:

    <self>/host/<token>[/<game>]    keep my lobby listed; answer my standing
    lobbies                         who is online: "owner partner|- open|waiting|playing game"
    <self>/pair/<token>[/<game>]    quick match: sit with whoever of my game is waiting, or wait
    <self>/visit/<token>/<owner>    sit at that lobby (a row, or a shared link)
    <self>/unpair/<token>           leave the pair
    <self>/push/<token>/<pair>/<verb>[/<arg>]   one line into the pair's record
    <self>/relay/<token>/<pair>/<since>         the record, paged like chat

**Any game's lobbies.** A page says which game it plays when it hosts or
asks for a match (this one says `chess`); the listing carries it, quick
match pairs like with like, and the relay passes another game's lines on
unread -- only a chess pair's lines are held to the chess verbs. So one name
server is a lobby list for every game whose pages use it.

**On the site.** `run_website.etcs` and `run_tls_website.etcs` serve this page
at `/chess/play/` (its files listed in `../chess_mounts.etcs`, mounted under
that prefix) beside the site's lobby list at `/chess/`
(`scripts/www/chess/index.html` in ETCS): the name server's list read with no
game attached, whose rows open this page with `?lobby=<owner>` and whose
Quick match opens it with `?quick=1`. The page links back to the list when it
is served under a `/play/` path.

**One page per name.** Every name-server verb carries the page's token --
one per tab, kept in `sessionStorage`, so a reload is still you and a second
tab is not. A name is the first token's while that page is online; any other
token is answered `NAME TAKEN` (checked before the self is touched, so the
refused calls cannot keep the holder's name alive), and once the holder has
been quiet for 30s the name is free again. A page told its name is taken goes
by the first free suffix (`name-2`) there, for that tab only; a rename onto a
held name is refused and the old name kept.

The page calls `host` every 1.5s; a lobby whose page stops calling for 30s
drops out of the listing and out of its pair (`ChessNode::endStaleLocked`).
`Share my lobby` copies `?lobby=<you>&ns=<node>` -- whoever opens it sits down
across from you. `this node` is the page's own runtime, which lists nobody but
you: a tab can ask any node and cannot be asked, so finding a partner takes a
node you both can reach.

**Paired, both runtimes replay one record.** The pair is a new board on each
runtime (its id, `<owner>-<n>`, is the match name there; your hotseat board is
kept). Every act at the table -- a move, a seat, a word, a resignation, a
draw, a new game -- is a line `<seq> <self> <verb>[ <arg>]` pushed to the
name server, which gives it the pair's order; each page pulls the record and
replays every line as that self's verb against its own board, the same
`game.Request` a click makes. Both boards take the same lines in the same
order, so they are one game.

**The two boards agree on each step.** A step is drawn on your board the
moment you make it (`<self>/propose/<pair>/<verb>[/<arg>]`,
`ChessNode::proposeLocked`), which answers as the verb would plus the state
hashes either side of it -- position, seats, the draw offer, the outcome
(`ChessGame::stateHashLocked`; not chat or presence, which each board
narrates at its own moments). The line carries them: `<arg>~<from>.<to>`.
Every board then takes a line only from `<from>` to `<to>`
(`ChessNode::replayLocked`):

- your own step, coming back in order, is confirmed -- already drawn;
- a line ordered ahead of it takes it back first, and it is judged in its
  turn like anyone's: made from a state that line replaced, it is void on
  both boards. Whoever reached the name server first has the seat, the move,
  the offer; the other page says its step was taken back;
- a step both boards start from but land differently on is a disagreement.
  The board that sees it pushes `void/<seq>`, and both take that one step
  back where it was taken;
- a `void` that is not about the line just before it is a disagreement past
  one step: there is nothing agreed to go back to, and the game is drawn,
  `desync`. A desynced board hashes as `desync` alone, so New game is a
  step both can take.

Talk (`say`) is not a step: it carries no hashes and lands in order. A step
that changes nothing, or is refused, is never sent. A pair's board reaps no
seats by its own clock -- two boards timing a seat out seconds apart would
disagree about it; a quiet partner ends the pair at the name server instead.

**The record checks itself, the way the share record does.** The name server
chains every line it stores -- XXH3 of the line seeded with the chain before
it, the paint session's own record chain -- and each page of the relay says
the chain through its last line (`<base> <next> <chain>`). The page replays
each line through its board's `replay` entry
(`<self>/replay/<pair>/<seq>/<verb>[/<arg>]`), which chains the same bytes
into the board, and reads the board's chain back (`chain`). Equal seqs with
different chains is a line one side missed -- transport, not disagreement --
and the board is rebuilt from the start of the record.

The relay is the proxy level a browser needs: a page can dial a node and
cannot be dialled. The lines are the verbs themselves, so a direct link --
a MirrorBuffer between two runtimes, or a WebRTC channel for two pages -- can
carry the same ones later without the replay changing.

## The address of a name server

An address is resolved before it is dialled (`nsResolve`): the scheme is
optional and https is assumed; the port is 443 for https unless the address
says `host:port`, and 80 for http likewise; trailing slashes are dropped. The
status line spells the resolved `host:port/path` out, so what was dialled is
never a guess. A node on another origin has to send
`Access-Control-Allow-Origin` (`chess_web.etcs` does; `HttpServer::AddHeader`
writes it into every response).

**NetworkProvider is in this page too -- built for the browser, loaded, and
booted the same way the hosted node boots it.** `boot_chess.etcs` runs the
same lines `chess_web.etcs` runs; `Start()` is refused by the browser at
`listen()` and the page goes on. This node is a name server in kind, lacking
only an address anyone else can dial.

## Waiting for the boot script, not just the runtime

`onRuntimeInitialized` only means the wasm instance itself is up -- it says
nothing about whether `boot_chess.etcs` has reached its own `spawn` yet, and
empirically it does not always land first: a click can arrive while `'game'`
is not yet a live global name. `etcs_web_call` and `etcs_web_call_async`
both treat that the same way a name that will never resolve is treated --
logged, not retried -- so without a guard here the click's own move was
silently dropped, and because `refreshStatus()` never overwrites a
`'bad'`-classed flash message, the resulting `runtime said: (empty)` error
stayed on screen forever, long after boot had actually finished and the
board was playable.

Two things fix this. `onRuntimeInitialized`'s first read of the board
(`callResult('game', 'Request', .../fen)`) retries briefly instead of
assuming the first attempt landed. And `boardReadyFlag`/`boardReady()` --
checked by every input path (`clickSquare`, `seated`, `doReset`, the chat
handler) -- does not flip true until that retry loop actually gets a real
position back, so a click landing in the same window a bare
`onRuntimeInitialized` check would have missed is rejected with a "still
starting up..." flash instead of silently lost. `moveInFlight` is a second,
narrower guard on top: it stops a second click from computing `sideToMoveOf`
against a stale `lastFen` while a prior move's own round trip is still
pending.

## Draining the terminal, not being pushed to by it

`etcs_web_shell_drain()` (`loaders/etcs.cc`) hands over whatever `std::cout`
output has piled up since it was last called; the write side only falls
back to pushing on its own if nothing has pulled recently (see that
function's own comment for why, and the measured cost of the old per-line
push -- ~54 lines/second, enough to turn a boot that finishes in under a
second into one that visibly takes closer to a minute). This page embeds
the shell as an iframe rather than owning `#term` directly, so its
`drainShellOutput()` calls `etcs_web_shell_drain` on a `SHELL_DRAIN_MS=60`
timer and forwards whatever comes back through the same local `termWrite()`
every other line already goes through (`postMessage` to the shell frame) --
the same pattern PaintProvider's page uses, for the same reason: both embed
rather than own the terminal DOM. `startShellDrain()` is called once,
from `onRuntimeInitialized`. Skipping it would not be a correctness bug --
the fallback push still fires -- it would just mean this page never sees
the speedup the rewrite exists to provide.

## What is not solved here

**The link is a relay.** Two pages talk through a name server, not to each
other. A direct link (WebRTC for two pages, a MirrorBuffer for two native
runtimes) would carry the same lines.

**A name is held, not proven.** A name server keeps a name to one page
while it is online, and a token is only as private as the page's own
traffic; a signed key is what would make a name belong to a person.

**Nothing persists.** A reload starts `boot_chess.etcs` fresh, a new board;
a pair survives it only if the page comes back within 30s (its `host` call
finds the pair and replays the record). PaintProvider mounts `/persist` on
IDBFS; this page could do the same.
