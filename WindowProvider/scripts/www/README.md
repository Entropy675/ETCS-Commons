# ETCS in the browser — window + canvas + shell

`index.html` is the WindowProvider page: it owns the wasm runtime, it owns the
`<canvas id="canvas">` that a `Window` draws into, and it embeds
`ShellProvider/scripts/www/index.html` as its terminal so the navigator is still
there to type at.

## Why the runtime lives HERE and the shell page is the sub-object

`GLFWWindow::CreateWindow` calls `emscripten_set_canvas_element_size("#canvas", …)`
(OS/GLFWWindow.h), and that selector resolves in the RUNTIME's own document. So
the canvas and the runtime cannot be in different frames — put the runtime in the
iframe and `#canvas` is a selector for an element the runtime's document does not
contain.

The terminal has no such constraint: it is text in, text out. So the split runs
the other way from what you might expect — this page holds the runtime and the
canvas, and the shell page is embedded purely as the terminal VIEW, bridged over
`postMessage`. One runtime, one set of modules, one ETCS.

The shell page detects being framed and skips its own boot entirely; served on
its own it is unchanged and still works standalone. There is no second copy of
the terminal: it is the same file, in both roles.

## The layout: one exposed directory, everything else mounted by name

    ../serve_web.etcs    the server. OUTSIDE www/, and it serves www/
    index.html           this page      (StaticHtmlPage, so "/" lands here)
    modules.json         the include list: modules, scripts, boot, glue
    boot.etcs            handed to the runtime as argv[1]
    etcs.js  etcs.wasm   build output, copied in
    *.wasm               every provider named in modules.json

and mounted FILE BY FILE by `../serve_web.etcs`, each at the exact URL the page
fetches it from, without exposing the directory it lives in:

    /window_events.etcs  <- ../window_events.etcs   (the OS-side pump, itself)
    /window_pointer.etcs <- ../window_pointer.etcs
    /shell               <- ../../../ShellProvider/scripts/www/index.html

WHY MOUNTS AND NOT COPIES. A page can only fetch what the server serves, and the
runtime can only open what the page staged, so `detach window_events.etcs` in the
browser needs that exact name to answer over HTTP. The reachable-by-URL set is
whatever `LoadFromDisk` mounted, and a file one level up is not in it. The first
version of this directory therefore held copies of all three -- which makes "the
same pump script as the OS side" a claim a diff has to keep true rather than a
fact. `FileHtmlPage.MountExternal` takes a page entity and a path segment, so one
spawn and one mount per file serves the ORIGINAL at the name the page wants, and
there is exactly one copy of each script in the repo.

It also means only `www/` is ever exposed as a directory. The mounts are explicit
and enumerable -- `tree.ListPaths()` at the end of the serve script prints the
whole served surface, which is the fastest way to spot a name the page asks for
and the server does not have.

The consequence to know: the iframe's `src` is `shell`, a mount point, so this
page expects to be served by `serve_web.etcs` rather than by any static file
server. That is the trade for not duplicating ShellProvider's page.

Build, then copy the outputs into `www/`:

    ace make loader etcs -DETCS_REPL_SHELL EMSCRIPTEN=1
    ace make module WindowProvider EMSCRIPTEN=1
    ace make module ShellProvider  EMSCRIPTEN=1

`ace` emits `etcs.js` + `etcs.wasm` on the web path now -- it used to emit an
extensionless `etcs`, which a server hands over as application/octet-stream and
the browser warns is not a valid JavaScript MIME type, and which forced every page
to probe two names and log a failed fetch on the way. `modules.json` names the
glue instead.

## Threads are fine here -- the constraint is WHEN, not whether

`Window.ProduceEvents` is the OS message pump as well as the event source: a
`while (IsActive())` loop around `PollEvents` (WindowProvider.h). So `boot.etcs`
detaches it, and `window_events.etcs` detaches the pointer channel again.

That works in the browser, and the reason it works is the ordering this boot path
already enforces. A Worker started while the main module is still inside
`loadDynamicLibrary` finds `wasmMemory` undefined -- which is what the deferred
static init (`ETCS_MODULE_STATIC_REACH_LOADER`) and `etcs_boot_runtime_threads`
exist for. `boot.etcs` is argv[1], so it runs after both: every module is loaded
and the pool is armed before its first line. Threads after load are ordinary.

Two things had to be true for the pump to actually pump, and both are in the
patch this page came with:

- `etcs_boot_runtime_threads` now ARMS the ThreadPool workers instead of leaving
  them deferred forever. A `->` edge enqueues its producer onto that pool, so with
  no workers the pump was accepted and dead -- stated, unrefused, and silent.
- The producers' waits are cooperative (`etcs_cooperative_pause_ms`). A
  `std::this_thread::yield()` spin waiting for `IsActive()` can never finish on the
  browser's main thread, because the callback that would set it is delivered by
  the event loop the spin refuses to return to.

Build (no special pool size needed):

    ace make loader etcs -DETCS_REPL_SHELL EMSCRIPTEN=1 -DPTHREAD_POOL_SIZE=0
    ace make module WindowProvider EMSCRIPTEN=1
    ace make module ShellProvider  EMSCRIPTEN=1

## What is not solved here

`glfwMakeContextCurrent` runs on the pump's worker for a context created on the
main thread by `glfw_web::do_create`. A WebGL context belongs to the thread that
created it, and that call returns `void`, so the failure is silent. The window
gets input and the canvas stays blank until either the context is created on the
thread that will use it, or drawing is marshalled the way the five GLFW entry
points in `glfw_web` already are. Nothing on this page can paint until then --
and there is no render backend for the browser yet regardless
(`Contract_RenderProvider.h`'s `__EMSCRIPTEN__` branch is empty).
