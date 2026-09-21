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

ONE TEXT PANE, WITH A FLOOR. Boot progress goes into that same terminal, through
the replay buffer that already existed for runtime output produced during iframe
load; the header keeps a one-LINE status (current step, red on failure) and
nothing else.

The terminal is also what the layout protects, because it is the thing you type
into. At 62rem and wider the page is two columns -- canvas left, terminal right,
both full height. Below that it stacks, and the terminal takes
`clamp(15rem, 40%, 28rem)` while the canvas takes what is left and SHRINKS to fit.
That order is deliberate: the canvas scales freely, since `Create()` sets its
framebuffer and CSS only decides how big it is drawn, whereas a three-line
terminal is useless -- and the first version let a 1024x768 canvas claim the
height and handed the terminal the remainder. Measured: 45 lines at 1440x900, 39
at 1100x800, 16 stacked at 900x800, 15 on a 420x740 phone, no page scrolling at
any of them.

The shell page detects being framed and skips its own boot entirely; served on
its own it is unchanged and still works standalone. There is no second copy of
the terminal: it is the same file, in both roles.

## The layout: one exposed directory, everything else mounted by name

Run the server FROM THE scripts DIRECTORY -- every path in `serve_web.etcs` is
relative to it:

    cd modules/WindowProvider/scripts && etcs serve_web.etcs

The served tree is this directory:

    index.html           this page      (a Directory node resolves "/" through it)
    modules.json         the include list: modules, scripts, boot, glue
    boot.etcs            handed to the runtime as argv[1]
    etcs.js  etcs.wasm   build output, copied in
    *.wasm               every provider named in modules.json

and mounted into that same tree FILE BY FILE by `../serve_web.etcs`, each at the
exact URL the page fetches it from, without exposing the directory it lives in:

    /window_events.etcs  <- ../window_events.etcs   (the OS-side pump, itself)
    /window_pointer.etcs <- ../window_pointer.etcs
    /shell               <- ../../../ShellProvider/scripts/www/index.html

(written from `serve_web.etcs`'s own directory those last three are
`./window_events.etcs`, `./window_pointer.etcs` and
`../../ShellProvider/scripts/www/index.html` -- one `..` fewer, because the script
sits one level above this file.)

WHY MOUNTS AND NOT COPIES. A page can only fetch what the server serves, and the
runtime can only open what the page staged, so `detach window_events.etcs` in the
browser needs that exact name to answer over HTTP. The reachable-by-URL set is
whatever the tree holds, and a file one level up is not in it. The first version
of this directory therefore held copies of all three -- which makes "the same pump
script as the OS side" a claim a diff has to keep true rather than a fact.
`FileHtmlPage.MountFile` takes one url path and one file, so one line per file
serves the ORIGINAL at the name the page wants, and there is exactly one copy of
each script in the repo.

MountFile, NOT MountExternal, and the difference decides what those paths can even
look like. MountExternal forwards to a `StaticHtmlPage`, whose `SetHtmlFromFile`
canonicalises the path against the CURRENT WORKING DIRECTORY and refuses anything
that leaves it -- "SECURITY VIOLATION: Path traversal blocked", then an empty page
that resolves as a miss and 404s with no other sign. So `../..` to ShellProvider's
page was unreachable from here by construction. It also caps at
`ETCS_NETWORK_MAX_HEADER_SIZE`, answers `/`, `/index.html`, `/style.css` and
`/app.js` rather than the one path asked for, and carries no extension to take a
MIME type from -- `/window_events.etcs` came back as `text/html`, `ListPaths`
advertised `/shell/app.js` and `/shell/style.css` that 404, and a real `.wasm`
could not be mounted at all. `MountFile` opens the path as given and builds the
same File-kind node `LoadFromDisk` builds: unbounded bytes, `Content-Type` from
the disk name, one url path. MountExternal remains the right call for a page some
other entity keeps rewriting -- it is the only mount kind that re-reads per
request.

It also means only this directory is ever exposed. The mounts are explicit and
enumerable -- `tree.ListPaths()` at the end of the serve script prints the whole
served surface, and a mount whose file could not be opened logs why and is ABSENT
from that list rather than listed and empty.

## One page entity, because "first match wins" is not what it sounds like

`serve_web.etcs` gives the server exactly ONE `HtmlPage` child. That is a
correctness requirement, not tidiness.

`HttpServer::ResolvePath` walks its page children and takes the first that
matches, and `run_website.etcs` says "whichever is attached first owns it". That
holds ACROSS tags -- `typed_child_order_` really is attach order. It does NOT hold
WITHIN one tag: that level is a `RIDList`, whose `entities` is an
`unordered_map`, so `invoke_collect_rids` hands the siblings back in hash order.

And every `StaticHtmlPage` answers `/` AND `/index.html` unconditionally, whatever
content it was given (`kIndexPath`/`kHtmlPath`). So two `StaticHtmlPage` children
of one server is a coin flip the hash re-tosses, and the symptom is the index page
serving some other page's bytes -- which is exactly what
`https://localhost:8443/index.html` returning `window_pointer.etcs` was.

There is no separate landing page here because there does not need to be: a
Directory node resolves `/` through its own `index.html` child, and this file is
already that child. One page entity, no ordering to depend on. The same hazard is
still latent in `run_website.etcs`, where `landing` and `chess_web.etcs`'s
`board_page` are both `StaticHtmlPage` children of the same server.

The consequence to know: the iframe's `src` is `shell`, a mount point, so this
page expects to be served by `serve_web.etcs` rather than by any static file
server. That is the trade for not duplicating ShellProvider's page.

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

Build, then copy the outputs into `www/`:

    ace make loader etcs EMSCRIPTEN=1
    ace make module WindowProvider EMSCRIPTEN=1
    ace make module ShellProvider  EMSCRIPTEN=1

No pool-size flag on that first line. `-sPTHREAD_POOL_SIZE=0` is already in the
generated loader Makefile, and `-DPTHREAD_POOL_SIZE=0` would not set it anyway:
`-D` is a PREPROCESSOR define and this is a linker `-s` option, so it defines a
macro nobody reads and leaves the real setting exactly as it already was.

THE WORKERS AND THE MEMORY FLAG. `growMemViews` is emitted only for
`ALLOW_MEMORY_GROWTH` together with threads, and it opens with
`wasmMemory.buffer` -- so in a pthread worker that has not yet been handed its
`wasmMemory`, that first line is `undefined.buffer`. That is the
`can't access property "buffer", wasmMemory is undefined` storm, one per worker.
The loader therefore links with a FIXED `-sINITIAL_MEMORY` by default: with growth
off the function is never generated and the line cannot throw. Both knobs are
Makefile variables, so comparing the two is one command and no regeneration --
`ETCS_WEB_MEMORY` and `ETCS_WEB_POOL`; the ACE patch spells out the trade. Fixed
means the heap is a budget: the default is 512 MB (it was 256, of which the
runtime's arenas took ~240 after a paint page booted, so a 1216x960 two-layer
canvas was the allocation that hit `Aborted(OOM)`), and anything that allocates by
the picture asks first rather than letting `malloc` abort the tab
(`paint_heap_can_take` in PaintProvider).

A PREWARMED POOL IS WORSE, which is why `ETCS_WEB_POOL` defaults to 0.
`-sPTHREAD_POOL_SIZE=4` creates its workers before the first side module opens, so
emscripten then has to replicate every `dlopen` into all of them through
`__emscripten_dlsync_threads` -- Asyncify, nesting inside a `dlopen` that is
itself unwinding. With an empty pool there is nothing to sync to while modules
load. It is the same rule the deferred arming already encodes, from the other end.

-sASYNCIFY HAS TO BE ON THE MODULES TOO, not only the loader. Asyncify can only
unwind and rewind through frames it INSTRUMENTED, and every cooperative pause in
ETCS is in a module -- `etcs_cooperative_pause_ms`'s call sites are all
WindowProvider, none are in the loader. With the flag on the loader alone, no
sleep in the program is instrumented end to end.

`modules.json`'s `"glue"` key names the file the page loads, so whatever the web
path emits has to match it -- `etcs.js`, as written. An extensionless `etcs` is
served as `application/octet-stream`, which the browser warns is not a valid
JavaScript MIME type, and it lands on the same name as the NATIVE `etcs` binary in
`bin/` -- `copy_loaders` then moves the glue over it. Naming the web output
`etcs.js` is what keeps those two apart; see the ACE patch that goes with this.

## Why the navigator did not come up on this page: 30 missing GLFW symbols

The shell page worked and this one did not, and the difference was never the
canvas, the bridge, the load order, the thread pool or Asyncify. It was a link
error that does not fail at link time.

`WindowProvider.wasm` imports 30 `glfw*` functions. `etcs.wasm` exports exactly
one of them (`glfwGetProcAddress`) and `etcs.js` contains no GLFW at all. The
module is built with `-sUSE_GLFW=3`, which gives it the GLFW headers so it emits
those imports -- but `-sSIDE_MODULE` emits no JavaScript, so `library_glfw.js`
only arrives if the MAIN link asks for it, and it was not asking.

WHY THAT WAS SILENT. A `-sMAIN_MODULE` build needs
`-sERROR_ON_UNDEFINED_SYMBOLS=0`, because a side module legitimately imports what
it will find at runtime. So a symbol nobody defines is not rejected; emscripten
hands the module a lazy stub instead (`proxyHandler.get` in the glue):

    stubs[prop] = (...args) => { resolved ||= resolveSymbol(prop);
                                 return resolved(...args) }

`resolveSymbol` returns undefined and the program dies the first time that import
is CALLED -- as `TypeError: resolved is not a function`, under
`doRewind`/`handleSleep`, because the call happens inside `dlopen`'s asyncify
rewind. Nothing in that stack names the symbol, which is why it read like a
toolchain problem for several rounds.

And it explains the two pages exactly. A stub that is never called never throws.
The shell page never enters WindowProvider's code, so its 30 stubs sat unresolved
and harmless; this page's `boot.etcs` calls `Window.Create` -> `CreateWindow` ->
`glfwInit`, and dies there. `Window.Create` logs nothing on success and the throw
is an async rejection, so the last line was "Window.Create got size" with no
failure line after it. `run_script` never returned, `main` never fell through to
`drive_main_loop_then_exit`, and there was no prompt.

THE FIX IS IN ACE, not here: the loader's Web link now carries every JS-library
`-s` flag any module's Web profile declares (`ETCS_WEB_JSLIBS`, generated from the
module manifests -- `-sUSE_GLFW=3` from WindowProvider). Only the main module has
glue, so only the main link can carry them, and the loader cannot know which
modules will `dlopen` in, so it takes all of them.

CHECKING IT WITHOUT A BROWSER. `tools/wasm_link_check.py` does the set difference
the linker declined to do:

    wasm_link_check.py bin/etcs.wasm bin/WindowProvider.wasm bin/ShellProvider.wasm

It reads the wasm import and export sections directly (no emsdk, no wabt), folds
in symbols the sibling side modules export and names the glue mentions, and lists
what is left. On the binaries that produced this bug it prints the 30 `glfw*`
names and reports ShellProvider as clean. Exit status is 1 when anything is
missing, so it works as a post-build gate.

## ASYNCIFY is not available to this program, and the REPL lives on a Worker

Asyncify replaces a module's exports with JS closures, and dylink stores those as
the library's exports (`postInstantiation` does
`moduleExports = Asyncify.instrumentWasmExports(moduleExports)` before
`dso.exports = exports`). When a pool thread calls a module function through a
table slot the main thread created, emscripten's own catch-up runs

    addFunction(sym, sym.sig)

on a closure with no `.sig` that `setWasmTableEntry` refuses, falls through to
`convertJsFunctionToWasm(sym, undefined)` and dies on `sig.slice`. The thread's
table is left short, so the next indirect call reports
`table index is out of bounds`. Calling module functions from pool threads is what
`detach` and every `->` edge do, so this is the normal case. ETCS itself never
calls `dlsym` off the main thread -- this is emscripten's table catch-up, not ours.

So the loader and the modules both link without `-sASYNCIFY`, and the two things
that used it are arranged differently:

- `etcs_cooperative_pause_ms` returns false on the browser's main thread instead
  of unwinding, and callers return rather than spin. There is no way to wait
  there: `sleep_for` blocks the thread that delivers the event and `yield` never
  returns to the loop that would deliver it.
- the REPL's line wait runs on a Worker. `drive_main_loop_then_exit` builds the
  session -- Root, Shell, console and loop -- on a detached thread and returns, so
  the main thread stays on the event loop. The canvas needs that too: GLFW delivers
  through the main thread, and the page cannot paint while a REPL sits on it.

Two consequences to know. `ctx`/`root` are `static` in the emscripten build of
`main()` (`loaders/etcs.cc`), because main returns while the REPL thread is still
using them. And `etcs_web_shell_write` uses `MAIN_THREAD_EM_ASM`: a Worker's JS
scope has no `window`, so output would otherwise land in a Worker console.

The navigator also refuses a module it does not already have. The module set is
fixed at boot, so an unknown name cannot be found, and letting it reach
`ResolveEvent` meant `dlopen` failing inside the navigator plus a `RangeError` from
emscripten's failure path.

Verified in headless Chromium against a real build: cross-origin isolated, canvas
1024x768, window created, `key edge open` and `pointer edge open` both reached,
`Root>` live, a line typed in the embedded terminal round-tripping to the navigator
and back, and zero page errors.

## The canvas: what draws, and the five things that stopped it

`boot_paint.etcs` is the worked example -- PaintProvider drawing through
RenderProvider onto `<canvas id="canvas">`, with the pointer painting into it.
It is also byte-for-byte the shape `PaintProvider/scripts/paint_surface.etcs`
has on the desktop, because nothing in it is browser-specific: what differs is
which concrete `Surface` the contract selected
(`RenderProvider/Contract_RenderProvider.h`), and that is invisible from the
script.

Getting there took five fixes, and each one is a case of the same thing: a
single-threaded-or-single-image assumption that the desktop happens to satisfy
and the browser does not.

**1. GLFW's state lives in ONE thread's JS scope.** emscripten implements GLFW
in JavaScript, and its window list, hint table and callback table are properties
of a `GLFW` object in the calling thread's scope. A pthread worker gets its own
copy of the glue, so that object exists and is EMPTY. `glfwInit` on the main
thread does not initialise a worker's, and the worker's calls then read and write
state no canvas is behind:

    glfwWindowHint   -> TypeError: Cannot set properties of null (setting '139265')
    glfwSetKeyCallback, glfwGetFramebufferSize, ...  -> silently onto nothing

139265 is `GLFW_CLIENT_API`. So every GLFW entry point this module reaches is
routed to the main runtime thread through `ETCS_GLFW_MAIN` (`OS/GLFWWindow.h`),
which is a no-op wrapper on the desktop and a synchronous hop in the browser.
The window's whole callback registration is ONE hop rather than eleven.

**2. Two threads driving one ordering loop.** There is no ordering thread in the
browser (`EventStream::start`), so a waiter runs the loop inline -- and more than
one waiter is the normal case, because every `resolve_module`/`changeModule`/
`spawn_entity` waits. The REPL coming up while a boot script runs put two threads
in `emscripten_poll` within milliseconds. Two drivers is not a slow path, it is
corruption: both read the same `in_seq_`, both `markConsumed` the same ring slot,
both launch it -- which is how a sequence walks past the ring and lands as
`memory access out of bounds` inside `enqueue`, several frames from anything that
looks responsible. The quieter half is worse: one driver consumes the event the
other is waiting for the completion of. The loop is now CLAIMED, per thread and
counted: another thread stands down and yields, the owner may re-enter (work the
loop launches blocks on further events, and only the thread already inside can
serve those), and admission publishes `in_seq_` before running anything so a
nested pass and its caller cannot both advance over one event.

**3. A module cannot drive the loader's loop through its own `stream`.** This is
the one that stopped `main.spawn(RenderProvider::Surface view)` dead. The member
offsets agree -- `LoaderStream` and `ModuleProxy` share the same `EventStream`
base -- but the DISPATCH does not: `launch_slot` calls
`static_cast<Derived*>(this)->on_event`, and `Derived` is whatever the CALLER
compiled. So a module driving the loader's events reaches
`ModuleProxy::on_event`, which forwards everything that is not a `TagModify`
straight back onto the same stream. The result is a loop consuming and
re-enqueueing at full speed with nothing completing: `in_seq_` climbing by
hundreds of thousands, every slot Empty, and the waiter blocked forever. The poll
now goes through `EventNode::drive_ordering`, a trampoline the OWNING image
installs -- the only image whose `Derived` is the real one. It sits above the
`#ifdef ETCS_LOADER` fork for the same layout reason `set_log_to_file` does.

The browser also had no stall diagnostics at all, which is why a blocked
admission read as "the script simply stopped". `EventStream::reportReorderState`
is now shared with the ordering loop's own reporting and a web waiter asks for it
after a second of waiting -- `pending`/`running`/`blocked` are three different
faults and printing them together is how you tell them apart.

**4. `dlsym` off the main thread blocks on every other thread.** A receiver-scoped
spawn was the program's only `dlsym` after load, and in the browser that is not a
lookup, it is a rendezvous: when the symbol is not yet in the calling thread's
table, emscripten's `__dlsym` calls `_emscripten_dlsync_threads()` and blocks
until every other live pthread has replayed the new entry. A thread replays it
only on the way out of a futex wait (`_emscripten_yield` ->
`_emscripten_process_dlopen_queue`), and `sched_yield` is a no-op in wasm -- so
one thread spinning on `yield()` anywhere in the process hangs the spawn forever.
Two changes: `<Tag>_MakeChild` is resolved with the rest of the catalog at load
time (`ModuleBundle::makeChildFunc`), so the spawn does no lookup at all; and
`etcs_emscripten_spin` stands down with a short SLEEP rather than a yield, which
is the whole difference between a thread that participates in the runtime's own
proxied work and one that merely burns.

**5. The pump's per-pass hop was the flake.** With all of the above fixed the
canvas painted, and then crashed on roughly one load in three -- always with
input, never idle -- inside emscripten's own proxying queue:

    RuntimeError: null function or function signature mismatch
        at call_with_ctx / em_task_queue_execute / receive_notification

`ShouldClose()` was asking the main thread `glfwWindowShouldClose` EVERY pass of
the pump, and the frame edge was asking `glfwGetFramebufferSize` every frame. In
the browser both are questions only this object knows the answer to: a canvas in
a page has no cross, so the only writer of `shouldClose` is
`CloseWindowConcrete`, and the only thing that can change a canvas's framebuffer
size is `framebuffer_size_callback`, which already records it in `m_size`. Both
now answer locally, and `glfwPollEvents` -- which is literally `() => 0` in
emscripten, because its backend runs callbacks from DOM listeners as the events
arrive and has no queue to drain -- is no longer proxied either. Answering the
two locally is both cheaper and MORE correct; the flake has not recurred.

The input rings needed one more thing. `InputSource`'s coalescing pair is
single-producer by construction -- "record from inside the callback, flush once
the queue is drained", both on the thread that pumps the OS queue. In the browser
those are two DIFFERENT threads, so the flush moved into `noteCursor`, on the
thread the callback arrived on. What that costs is the coalescing; what it buys
is not having two writers on a single-producer ring.

Verified in headless Chromium against a real build: cross-origin isolated, canvas
1024x768, all three edges open (`key edge open`, `pointer edge open`,
`Surface::RunFrames tick started at 16ms`), the paper layer composited to
white, and a scripted press-drag-release leaving 12000 pixels of brush colour
(`26,26,31` = the 0.10/0.10/0.12 the script asks for) in the same place on five
consecutive runs, with zero page errors.

## Specialising an empty canvas from the navigator

`boot_canvas.etcs` is the other boot: device, window, surface, a 2D anchor and a
frame pump, and then it stops. Everything after that is a script run from the
prompt in the terminal -- and the navigator needs no verb for it, because a
target ending in `.etcs` IS the run (see the script-execution branch of
`repl_shell_loop_with`):

    RenderProvider/scripts/scene_bars.etcs   anchor=scene
    RenderProvider/scripts/polygon_draw.etcs scene=scene view=view

Those are RenderProvider's own scene scripts, mounted from where RenderProvider
keeps them (`../serve_web.etcs`) and staged into the runtime's filesystem under
the same paths (`modules.json`), which is also what makes the navigator list them
next to the modules. One file per scene, read by both substrates.

A NAME IN `modules.json` MAY BE A PATH. The page creates the directories before
writing (`FS.mkdirTree`), so a script can be staged at the same relative path the
OS side reads it from -- which is why `boot_paint.etcs`'s
`detach RenderProvider/scripts/render_frames.etcs view=view` is spelled once for
both and not twice.

Preflight works here too, and is the fastest way to find out what a scene needs:

    RenderProvider/scripts/scene3d.etcs anchor=scene
    -> will not run -- 1 unmet requirement(s):
       'anchor' (line 25) does not carry [Drawable3D] -- RID:... carries
       [CompositeDrawable2D, Clippable, Drawable2D, Surface, Drawable, ...]

## What is not solved here

**3D does not draw on this surface, and it is not meant to yet.** `CanvasSurface`
is `PixelsBase`: it owns host bytes and presents them with `putImageData`.
`Scene3D`/`Camera3D` draw through the device path (`RenderableBase`), which on
the desktop is Vulkan and in the browser would be a WebGPU or WebGL context --
and a canvas has exactly ONE context for its lifetime, so that is a different
surface type on a different canvas rather than an addition to this one. The 2D
half is what is finished; `PixelsBase` and `RenderableBase` are mutually
exclusive under `Raster_` for exactly this reason.

**A navigator script that SPAWNS can trap while the frame pump is live.** A
spawn-free script (`polygon_draw.etcs`) runs repeatedly and cleanly against a
presenting canvas. One that spawns (`scene_bars.etcs`) builds all its entities,
logs every work func, and then traps as `table index is out of bounds` inside
emscripten's nested queue execution -- `em_task_queue_execute` ->
`call_with_ctx` -> `receive_notification` -> `em_task_queue_execute` ->
`call_with_ctx`, with a task struct being read as an `em_proxying_ctx`. With the
frame pump off there is no trap and the second command silently never starts, so
there are two symptoms of one cause. The shape is the same as fix 3 above: a
side-module function pointer (`addTagTrampoline<T>`, carried on the AddTag event)
invoked by whichever thread happens to hold the poll claim, rather than by the
thread whose table is known to have it. The next step is to make the driver
identity part of the event rather than a race -- either the enqueuer serves its
own AddTag events, or the trampoline moves into the loader the way
`drive_ordering` did.

**The remaining synchronous main-thread hops are per-frame, not per-pass.**
`PresentConcrete` still blocks its thread on a `MAIN_THREAD_EM_ASM`, holding the
raster mutex across the hop. `MAIN_THREAD_ASYNC_EM_ASM` would decouple the frame
rate from main-thread latency at the cost of a possible tear, which wants a
second raster to be correct rather than merely fast.

**The build cannot do both platforms at once.** `.ace_obj/`, vendored `build/`
directories, `$(DEPFILE)` and `module_hashes.h` are not platform-tagged, so a
native and a web build of the same tree overwrite each other's intermediates. One
consequence is already fixed: a NATIVE `etcs` left in `loaders/` made make
consider the web target up to date (the recipe writes `$@$(WEB_SUFFIX)`, so the
target name and the file differ), and the web link was silently skipped while the
page kept loading the previous `etcs.wasm`. The web path now declares those
targets `.PHONY`.
