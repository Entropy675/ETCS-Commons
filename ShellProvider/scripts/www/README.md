# ETCS Web Shell host page

Serve this directory with the native ETCS HttpServer (COOP/COEP) together with:

- `etcs.js` (or extensionless `etcs` glue) + `etcs.wasm`
- `ShellProvider.wasm` (and optionally `WindowProvider.wasm`)

Rebuild ShellProvider / etcs with `EMSCRIPTEN=1` after the WebTerminal bridge
and ETCS `DL_EXTENSION=.wasm` patches.

The page terminal calls `etcs_web_shell_push_line` so `lsh::read_line` unblocks.

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

    setInterval(() => Module.ccall('etcs_web_shell_drain', 'string', [], []), 60)

A write costs a string append -- no FS, no proxy, no DOM -- and a tick hands the
terminal everything at once, which becomes one `postMessage`, one
`DocumentFragment` and one reflow however many lines arrived. Same boot: **60s
to 12.7s**. What is logged does not change, and the per-line flush stays exactly
as atomic as it was.

A write that finds no recent drain falls back to the old proxied push, so a page
that does not pull -- an older copy of this one, or one whose timer has died --
still shows output rather than silently buffering it. `stderr` is deliberately
left on the direct path: it is what an abort has time to say, and a buffered
channel is the wrong place for the last words of a dying process.

The scrollback is capped (12000 spans, trimmed to 8000). A transcript nobody
will scroll back to is still laid out on every reflow.
