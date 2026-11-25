# ETCS Web Shell host page

Serve this directory with the native ETCS HttpServer (COOP/COEP -- a `-pthread`
build has no threads without cross-origin isolation) and mount `bin/wasm/` at
`/wasm/`, which is where the page resolves `etcs.js`, `etcs.wasm` and
`ShellProvider.wasm` from (resolveWasmBase in `index.html`; `WASM_DIR` in the
ETCS Makefile on why the artifacts live there and not beside a page). Nothing is
copied into this directory.

Build with `ace wasm make module ShellProvider` (or `ace wasm make all`) and `ace wasm make loader etcs`.

Embedded as the terminal of the paint and window pages, it skips its own boot and
is a view only; served on its own it boots the runtime itself.

The page terminal calls `etcs_web_shell_push_line` so `lsh::read_line` unblocks.

## The prompt is a menu, and the menu is buttons by default

Every prompt the navigator puts up is a choice from a list it just printed --
modules, tags, instances, actions, and a few words like `back` and `spawn`. The
runtime publishes that list as data beside the text (`repl_menu_publish`,
core/CommandExecutor.h; `etcs_web_shell_menu`, loaders/etcs.cc): for each entry
the line it stands for, a label, its kind, and whether it wants words after it.
The page polls it on the drain's clock and draws it as buttons under the
transcript. A button sends its line down the same path a typed line takes, so
the navigator never knows which it got. An entry that wants words -- an
action's payload, `spawn`'s name, `cd`'s path -- opens the one input under the
buttons; `go` sends both, and empty words are allowed (most actions read none).

`type instead` switches to the command line, `buttons instead` switches back;
the choice is kept in the browser. Buttons are the default because on a phone
they are the only usable form and on a desktop they are the readable one.
Embedded in the paint page, the host relays the menu JSON to the frame with the
text (`{etcs:'menu'}`), so the framed terminal has the same two views. A host
whose pane is too short for the buttons frames it as `shell?view=line` (the
chess page does): that view is then the default, and a switch made in that
frame is kept for that frame's path only.

## Output is pulled in chunks, not pushed per line

`ETCS_LOG` writes to `std::cout` and flushes per line -- one line, one write, so
a transcript cannot interleave mid-line (core/Log.h). Under emscripten that
flush used to be a write to stdout, stdout is emscripten's `out()`, and `out()`
calls `Module.print`; from a pthread that whole path is PROXIED TO THE MAIN
THREAD and blocks the logging thread until the main thread takes a turn. The
terminal then built spans for the line, appended them to a live document and
read `scrollHeight` to follow the tail -- a forced synchronous layout, per line.

Measured on the paint page: the runtime finished building the world in 0.6s and
the canvas appeared 60s later, with output arriving at a flat ~54 lines a
second (about 18ms each -- one main-thread turn per line). The terminal was not
showing the boot, it was pacing it.

So the loader redirects `std::cout` into a mutex-guarded buffer
(`ShellOutBuf`, loaders/etcs.cc) and the page drains it on its own clock:

    setInterval(() => Module.ccall('etcs_web_shell_drain', 'string', [], []), 250)

A write costs a string append -- no FS, no proxy, no DOM -- and a tick hands the
terminal everything at once, which becomes one `postMessage` and one layout
however many lines arrived. A quarter second
rather than a frame: the tick costs the same whether it carries one line or
three hundred, so a longer window is strictly less work for the same text, and
nobody is timing a log line to the frame. Same boot: **60s
to 12.7s**. What is logged does not change, and the per-line flush stays exactly
as atomic as it was.

Nothing pushes per write, so the drain is the only way out and order holds. A
writer that finds no recent drain (a page that does not pull, a timer starved by
a busy main thread) queues one drain on the main thread and goes on -- it never
waits for that thread. A writer on the main thread queues nothing: `main()`
writes the whole module handshake there before it returns, and the page's
timer drains it as soon as it does. Pushing those lines one message each cost
the kart page 14s to its arena on a fast machine and 75s at a 4x CPU slowdown;
drained, about 4s and 7s. `stderr` is deliberately left on the direct path: it is
what an abort has time to say, and a buffered channel is the wrong place for
the last words of a dying process.

The transcript is blocks of up to 64 lines, each laid out on its own and
skipped while offscreen (`content-visibility`), and the tail is followed in the
next animation frame rather than by reading `scrollHeight` after each append.
One run of text for the whole transcript meant every append re-laid all of it,
on the spot: a quarter of the kart page's main thread while its arena was
being built, half of it at a 4x slowdown. The scrollback is capped at 12000
lines, trimmed to 8000.

Framed, this page posts `{ etcs: 'view-ready' }` when it loads and again
whenever its parent posts `{ etcs: 'hello' }`, so a parent whose script runs
after the frame has loaded still hears it.

`etcs -q` (a session page's `?quiet`) turns `ETCS_LOG` off in the loader and in
every module, which take the loader's setting as they load.
