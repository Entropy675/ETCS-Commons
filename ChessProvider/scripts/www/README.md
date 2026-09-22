# ETCS Chess in the browser -- one board, both sides, local

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

## The causal graph, as the page shows it

The page is laid out as the ontology is: a NODE (this page's own runtime, or
a remote one -- "Name servers, all the way down" below), SELVES that take
SEATS in a MATCH, and the match itself. Three types in `ChessProvider.h`,
three places on screen. Nothing in the C++ changed to get there; the
server-authoritative page already drove these same verbs, it just left the
seat step implicit.

**Seats are joined, then claimed.** Each colour's card starts as `open` with
one button, Join -- taking a seat, the same way the server page's home
screen had you join a match. Joining is this page declaring which self it
will move as: `white` and `black` are the two selves this page can be, the
same tokens the move verb has always been sent as. The causal claim itself
still lands exactly where `ChessGame` puts it, on that self's first legal
move (`applyMoveLocked`, "claim by moving") -- so the log still reads
`white sits as white` at the first move, not at the click. That is the
server page's own model with the one step it skipped made explicit: there
you were a viewer until your first move; here you are a viewer until you
say which seat you are taking, and then until your first move. A click on
a piece whose seat this page has not joined is refused with `join white to
move`, the way a spectator's click was refused there.

Once joined, the card shows what a seat can do -- Resign, Draw, leave --
which is what the server page showed once `/status` reported you seated.
`leave` is the verb that page sent on departure: it releases the seat in
the game if the first move had already claimed it (`leaveLocked`, logged as
`white left -- white seat is open`), and is a no-op if not. New game clears
the seats in the game (`resetLocked`), so the cards go back to Join --
colours are re-taken next round, as there.

Hotseat is joining both seats. Both cards say `you`; the page moves as
whichever colour the FEN says is to move (`sideToMoveOf`), and every seat
check after the first move (`seat == tok`) is satisfied because the two
seats hold the two different tokens `applyMoveLocked` insists on ("one
browser cannot quietly become both players" -- it cannot, but two selves
can). A seat that reads `taken` without this page having joined it is one
somebody ELSE holds; nothing reaches that state today, and it is exactly
what the other end of the peer link will look like when it does.

**Draw is one verb, labelled per seat.** `drawLocked` answers both
directions -- press one seat's Draw to offer, the other's to accept -- and a
spectator is only ever told an offer is `theirs`, not whose. So when the
status read as `local` says an offer stands, the page asks once more as
`white` to learn whose it is, and labels the cards from that: the offerer's
button reads `offered…` (disabled), the other seat's `Accept draw`, the
status line says `draw offered by white`. Moving is declining, as it always
was (`applyMoveLocked` clears the offer).

**No token, no cache, no poll.** The server page carried a client-chosen
token in a cookie (so "this record is mine" meant something across
requests) and a `localStorage` cache of positions (so a poll every second
or two did not re-fetch a game the browser already had). Neither describes
one runtime with one person at the keyboard: the selves are the two colours,
and a local `callResult()` round trip (a `ThreadPool` enqueue plus one
`EventStream` wait, not a network hop) is cheap enough that `refreshStatus`,
`refreshChat` and every review step simply ask again rather than caching
what the last answer said. The `HIST_BLOCK`/prefetch machinery the old page
needed to make paging cheap is not reproduced here, only the paging loop
itself (`ChessGame::kFrameBudget` still caps one reply, whatever asks). A
name for the local self -- what a name server's `players` list would show
you as -- belongs with the peer link, where there is a second identity to
be told apart from.

## Name servers, all the way down

"Play online" (header) opens the name-server menu: `rooms` and `players`,
read off a node and rendered the way the server page's own home screen
rendered them -- because they are the same two verbs, in the same line
format (`ChessNode::roomsLocked`, `ChessLobby::profileLocked`).

There is no name-server TYPE anywhere in this. `ChessNode.h`'s own words:
"a node hosting many lobbies is a server, and a node hosting exactly ONE
lobby is a peer. Nothing else about the arrangement changes between those
two cases -- same types, same edges, same routing." A name server is any
node asked its listing verbs. The assumption underneath, stated so nothing
drifts from it: **the exact same ETCS runtime sits on the other side of any
address, and whether it is in a page or in a server on Linux does not
matter.** Nothing in this page branches on which it is; the address is the
only thing that varies by where a node lives. The menu's field names which
one:

- `https://anticurrententropy.com/chess/lobby` -- the DEFAULT. The hosted
  node, one instance of the thing, reached by `fetch()`; the global
  namespace everyone's page opens on, so strangers can find each other
  without exchanging an address first. (The verbs are read as
  `<field>/rooms` and `<field>/players`, so the field is the node's mount
  as seen from outside -- whatever path the node is served at.)
- `local` (the "this node" button) -- this page's own runtime, asked the
  same two verbs through the same call bridge every move goes through
  (`game/rooms`, `game/players` -- the reserved selves `requestLocked`
  routes). It lists match `local` and selves `white`, `black`, `local`.
- anything else -- a friend's node, your own hosted one. Same rendering,
  same format, no code that knows the difference.

An address is resolved before it is dialled (`nsResolve`): the scheme is
optional and https is assumed (a bare `host/path` is how one gets typed);
the port is 443 for https unless the address says `host:port`, in which
case that port is used, and 80 for http likewise; trailing slashes are
dropped. The status line spells the resolved `host:port/path` out even
when the port is the default, so what was dialled is never a guess --
`anticurrententropy.com/chess/lobby` reads as
`anticurrententropy.com:443/chess/lobby`, `myhost:8444/chess/lobby` keeps
its 8444.

That is what "every lobby is recursively a name server" cashes out to at
the level of this page. One asymmetry is the browser's, not the design's:
a tab can ASK any node but cannot BE asked -- there is no listening socket
in a browser -- so this page's own node is reachable as a name server only
from itself, until the peer link gives it an address (WebRTC is the one
transport a browser can be reached on, which is another reason the peer
link is where the name goes).

**This build reads a node; it does not connect to one.** Join / watch on a
room row and Create on the name input both stop at a status line saying so.
Connecting -- the two tabs finding each other through a node (the match's
own chat verb as the mailbox for the WebRTC handshake, so the hosted node
needs nothing new), then the game itself over the data channel with each
side running its own board and relaying its own moves -- is the next patch.

**CORS is one line in the node's own script, and it is in this patch.** A
browser only lets a page read a cross-origin answer that says so, and the
node as hosted today says nothing -- checked: its `rooms` answers 200,
`text/plain`, no `Access-Control-Allow-Origin`, so from any origin but its
own the default target reads as `unreachable from this origin`.
`HttpServer::AddHeader` is the fix, and it already works: `HttpServer::Serve`
(`NetworkProvider.h`) writes every header `AddHeader` stored into every
response it sends. So `NetworkProvider/scripts/chess_web.etcs` -- the script
the hosted node runs -- gains `web.AddHeader(Access-Control-Allow-Origin *)`
before `web.Start()`, wide open on purpose: rooms and players are the public
listing, every verb behind them is keyed by self, nothing is credentialed.
Verified on the wire against a native node running that script, and then by
this page reading that node's rooms and players through the menu.

**NetworkProvider is in this page too -- built for the browser, loaded, and
booted the same way the hosted node boots it.** It is a formatting provider:
HTTP parsing, pages, routes, the connection lifecycle as entities. The
transport under it is core's (ThreadPool's IO, io_uring on Linux and
compiled out in a browser), and its own socket calls resolve to emscripten's
POSIX layer -- so it compiles for Web unmodified, mbedtls and picohttpparser
included (see `ace-build-tools.diff` for the two build-tool changes that
needed: `emcmake` for a cmake dependency under Web, and a platform tag on
the dependency's build marker so a native build after a Web build does not
find wasm objects in `libmbedtls.a`). `boot_chess.etcs` then runs the same
lines `chess_web.etcs` runs: `ensure NetworkProvider::HttpServer web`,
`SetPort`, the `/game/...` route onto this node's `Request`, the CORS
header, `Start`. Every one of them takes; `Start()` is refused by the
browser at `listen()` -- `ConnectionManager` logs `listen() failed`,
`HttpServer` logs `not started`, the page goes on -- which is exactly right:
this node is a name server in kind, with routes and a header and a verb
surface identical to the hosted one, lacking only an address anyone else
can dial. Giving it one is the peer link's job. The point of carrying the
provider is the point of the whole design: the runtime on this side of an
address is the runtime on the other side, and nothing in this page may
depend on which it is talking to.

## Resign and draw, one set of controls per seat

The old page had one Resign button and one Offer-draw button because the
server told each browser which seat it held (`role` in `/status`). Here a
seat's controls appear on that seat's card once it is joined, calling
`resign` / `draw` as `white` or `black` explicitly -- the existing two-token
seat model with nothing new in the C++: `resignLocked`/`drawLocked` already
key off `roleOfLocked(tok)`, and `white`/`black` are the same tokens `Move`
already claims:

    White's card  -> resign as 'white'  / draw as 'white'  / leave as 'white'
    Black's card  -> resign as 'black'  / draw as 'black'  / leave as 'black'

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

**The peer link.** The name-server menu reads nodes; it does not connect to
them. The agreed shape, for the next patch: the two tabs find each other
through a node's listing, exchange the WebRTC offer/answer/candidates
through that match's own `say`/`chat` verbs (a mailbox the hosted node
already has, tagged so the page filters it out of the visible chat), and
then play over the data channel with each side running its own
`ChessNode`+board and relaying its own legal moves -- the receiving side
applies them as that self's moves, so its own `applyMoveLocked` validates
them like any other. That is deliberately the pragmatic relay, not the
event-replay-over-`MirrorBuffer` sync `ChessNode.h` describes as the
target; the target is substrate work, and this is a page.

**Chat has one voice, and the selves have no name.** Every chat line is
authored `local`, and the seats are the colour names. Both are the same
gap: there is one keyboard, so nothing needs telling apart yet. A name --
what a node's `players` list shows you as, what the other tab's board
would claim your seat with -- goes in with the peer link.

**Nothing persists.** PaintProvider mounts `/persist` on IDBFS so a page
survives a reload; this page does not; a reload starts `boot_chess.etcs`
fresh, a new board. The current game (and its history and chat, which
`ChessGame` already keeps in memory regardless of substrate) is gone the
moment the tab is. Worth doing the same way Paint's page does it, later.

**Board orientation is fixed.** The server-authoritative page flipped for a
`black` role; this page could now flip by which seat is joined (one seat
joined is a role), and does not yet. Stays at White's view throughout.
