# ETCS Paint in the browser — two canvases

    cd modules/PaintProvider/scripts && etcs serve_paint.etcs
    then open https://localhost:8443/

The page holds the runtime, two canvases and the terminal:

    #canvas    1024x768   the sheet -- strokes land here
    #toolbar    640x64    the palette strip, above the ETCS Shell block
    iframe      shell     ShellProvider's own page, embedded as the terminal

Deploy is the same two steps every page in this tree uses — build, then copy the
outputs into `www/`:

    ace wasm make module WindowProvider
    ace wasm make module RenderProvider
    ace wasm make module PaintProvider
    ace wasm make module ShellProvider
    ace wasm make loader etcs -DETCS_REPL_SHELL
    cp bin/etcs.js bin/etcs.wasm bin/{Window,Render,Paint,Shell}Provider.wasm \
       modules/PaintProvider/scripts/www/

(`ace wasm make modules` builds only the modules whose own manifest declares
`"Web"`; PaintProvider and ShellProvider build from `default.json` and so are
listed as not-built rather than guessed at. The tool says so.)

## Why the second canvas is a second SURFACE, not a second window

A browser window cannot host two canvases, and the reason is not a gap in this
code. emscripten's GLFW owns exactly one canvas — `Browser.getCanvas()` — and:

- every one of its input handlers begins `if (event.target != Browser.getCanvas())
  return`, so events on any other element are dropped before GLFW sees them;
- `glfwCreateWindow` ends with `GLFW.active = win`, and every handler dispatches
  to `GLFW.active.<callback>`, so a second window silently takes the first one's
  input;
- and `GLFW.adjustCanvasDimensions()` resizes that one canvas to the new window's
  size.

So a second GLFW window would cost the first one its input and its dimensions and
still be pointed at the same element. A second SURFACE has none of those
properties: presenting is a `putImageData` and nothing about it is global. That is
why the target belongs to the surface (`RenderProvider/OS/CanvasSurface.h`), named
rather than numbered, with `"canvas"` as the default so a session that never
mentions one behaves exactly as it did before targets existed.

    main.spawn(RenderProvider::Surface bar_view)
    bar_view.Create(@gpu)
    bar_view.SetTarget(toolbar)
    bar_view.ResizeTo(640, 64)

`ResizeTo` drops the follow, because being told a size and following the window's
cannot both be live — `PollResize` would snap the strip back to 1024x768 at the
next frame boundary. Stating the precedence in one place beats a flag every reader
has to correlate.

On the OS side those same lines describe a REGION of the one window, which is what
a toolbar is there; `SetTarget` answers honestly from the Vulkan backend (a
swapchain has one target, and it says so) rather than storing a name nothing
reads. The script does not change — what the target names does.

## The strip's input comes from the page, through work functions

The sheet's pointer arrives on an edge, off the event loop:

    detach paint_pointer.etcs window=main input=input

The strip's cannot, because its events are never in that stream. So the page drives
it the way a script would:

    etcs_web_call("bar_input", "Pointer", "412, 22")   //  bar_input.Pointer(412, 22)
    etcs_web_call("bar_input", "Press",   "")          //  bar_input.Press()

`etcs_web_call` (loaders/etcs.cc) resolves a global name — a boot script runs as
root, so its names ARE the globals — and dispatches through the same path the
executor uses for an ordinary action. It knows nothing about palettes: every verb
a module exports is reachable the moment the module is loaded, with no per-verb
glue to write and nothing to keep in step.

BY NAME, NOT BY RID, and not only for readability: a RID is a full 64-bit hash and
a JS number carries 53 bits, so a RID crossing that boundary as a number would
arrive silently wrong for most values.

It is for DISCRETE events. The call is synchronous on the calling thread, so a call
per pointer *move* would put the page's event loop behind the module's stream for
every sample. The handler forwards moves only while the button is down, which is
what makes dragging across swatches work without paying for hover.

Two things in PaintProvider had to change for that to work, and both were the
script path diverging from the stream path rather than anything new:

- `ScriptPointer`/`ScriptPress`/`ScriptRelease` called `HandleEvent` directly, so a
  machine with a root bound routed everything arriving on an edge and nothing
  arriving from a script. Routing is a property of how the machine is CONFIGURED
  (`BindRoot`), not of which transport delivered the event, so they go through
  `RouteEvent` now — which falls through to `HandleEvent` when no root is bound, so
  an unrouted machine behaves exactly as it did.
- a press carries no position (`x`/`y` are meaningful for `INPUT_MOTION` only), so
  routing one on its own coordinates picks whatever sits at the origin — nothing.
  The routed key edge already fills it from the last position the pointer channel
  delivered; `ScriptPress`/`ScriptRelease` now do the same. Before that fix a
  swatch click was a pick at (0,0) every time, which is silence rather than an
  error.

`bar_input` is a second `PaintInput` with `BindRoot(@bar)` and
`BindPalette(@palette)` — that pair is what makes a press at a point mean "that
swatch" rather than "draw here" (`Drawable2D_::PickAt`). Two input machines because
there are two spaces, not because there are two devices.

## Colour in the terminal

The runtime emits the same ANSI SGR a terminal gets — `color_enabled()` is now true
under emscripten — and ShellProvider's page parses SGR into spans. The alternative
was a second colouring channel for the web and two tables that have to keep
agreeing about what `COLOR_LIB` means; this way there is one vocabulary and the
page is one more thing that reads it. The shades are the page's own (an xterm's
`#0dbc79` fights this background); the MAPPING is standard, which is the half that
has to match.

Only the status line strips escapes, because it is `textContent` on one element,
where an escape is bytes on screen rather than a colour.

## Deploying: `ace make loaders` is not the interactive loader

`ace make loader etcs -DETCS_REPL_SHELL` and `ace make loaders` produce different
binaries, and only the first one serves. Without the define
`drive_main_loop_then_exit` takes the drain path, so `serve_paint.etcs` runs every
line — the server starts, the mounts resolve, `ListPaths` prints — and then
`wait_for_environment_drain` reports "all detached executors finished" and the
process exits, because an `HttpServer` thread is not a detached executor. The
symptom is a serve script that looks like it worked and left nothing listening.

## What is not solved here

The strip renders and picks, but it does not follow the window: `ResizeTo` made it
a fixed 640x64, so a very narrow viewport scales it down rather than reflowing the
swatches. Reflow is a layout question for the 2D tree
(`paint_toolbar.etcs` states absolute positions in bar space), not for the surface.

3D does not draw on either canvas. `CanvasSurface` is `PixelsBase` — host bytes and
`putImageData` — while `Scene3D`/`Camera3D` draw through the device path
(`RenderableBase`), which in a browser means a WebGPU or WebGL context. A canvas has
exactly one context for its lifetime, so that is a different surface type on a
different canvas rather than an addition to this one.
