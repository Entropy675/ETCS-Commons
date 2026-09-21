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
    ace wasm make loader etcs
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

## The drawing area is INSET, and the ruler lives in the margin

`paper_pane` is a 952x632 pane at (36, 36) inside `sheet_root`, and the band around
it is where the edge ruler's marks every 100 px go. They used to be drawn along the
inside of the drawing area, which put them on the picture — over the paper near the
edges, and paintable, so a stroke near a corner went through the scale it was being
checked against.

Nothing enforces that the band is unpaintable; two facts already in the tree do it:

* `input.BindCanvas(@paper_pane)` — only a pick that lands on the canvas node is
  the picture, so a press in the margin is scenery and no tool sees it. The router
  pane stays `sheet_root`, so the toolbar and the zoom steps are picked as before.
* the pane is its own raster — the document blit is clipped by that buffer's extent
  rather than by a check, so the picture cannot reach the band in the first place.

`canvas.BindRulerFrame(@sheet_root)` is what tells the surface which raster to mark;
with nothing bound the ruler falls back to the inside of the pane, which is all a
page with no margin can have. The band sizes itself per side from the room the page
left (the toolbar takes the bottom strip, so there is no bottom band) and measures
its own labels, so the numbers are not clipped by a provider with a wider advance.

## Selecting, and moving what is selected

The `select` slice sits between `brush` and `line` in the bar, which is now
sixteen 60px slices (960x64, 32px in from either edge of the 1024 sheet; rect and oval share one `shape` slice whose arrow steps rect, oval, triangle, diamond, star). One
tool, four ways of drawing the boundary — the word under `select` says which,
and the arrow at the top of the slice steps through them:

    rect     the two corners of the drag, as the rect tool reads them
    oval     inscribed in the same drag, as the ellipse tool is
    wand     the run of colour under the press, bounded like a fill and by the
             fill's own tolerance (PaintTool.SetTolerance)
    lasso    the path the pointer took, closed back to its start

The arrow is the swatch arrow's pattern — a control that names its slice at
layout time (`palette.AddModeArrow(@arrow, @tool_select)`) — doing the one thing
a four-way choice needs. A colour is continuous, so a swatch's arrow has to open
a picker; a mode is four words, and a popup for four words would be a wheel for
a switch. So it steps, and the readout (`palette.SetModeReadout`) says where it
stepped to. Pressing it also takes the select tool up, for the reason a wheel
pick leaves the picked colour in hand: stepping the mode of a tool you are not
holding is a control that visibly does nothing.

Whatever drew it, a selection is a MASK on the document (`PaintSelection`), so
inside/outside, the outline and the move are written once. The outline is
two-tone dashes drawn from the mask on every render — a preview, never a mark on
a layer — which is why it survives a pan, a zoom, and the wheel closing over it,
and why it is drawn in the render path rather than by the input machine, which
is not the only thing that re-renders. While you drag, what you see IS the
selection re-stated from the anchor and the tip each flush; nothing stands in
for it, so nothing can disagree with it.

Defining a region commits nothing. The commit is the CARRY: press inside the
region and drag. The selected pixels leave the active layer at the press
(`PaintDocument::LiftSelection` — the hole appears at once, not at the release),
follow the pointer from the selection's own buffer, and land where the button
comes up, blended source-over so the transparent part of a lifted region lands
as nothing. The outline goes with them, so the same region can be carried again.
It is the text box's press-inside-to-carry, for a region, and it is coalesced
where the text box's is not, because every sample re-composites the document and
resamples the lift over it.

A click with the select tool — a drag that went nowhere — clears the selection,
as it does for the text tool; so does Escape when no text box has the keyboard.
Either lands a lift still in the air rather than losing it.

All of it is reachable without a pointer, in document coordinates:

    doc.SelectRect(40, 40, 200, 120)        doc.SelectEllipse(40, 40, 200, 120)
    doc.SelectColor(50, 50, 24)              doc.SelectPath(10, 10, 90, 20, 60, 80)
    doc.MoveSelection(30, 0)                 doc.ClearSelection()
    brush.SetMode(lasso)                     doc.Report()   # extent and pixel count

`MoveSelection` is the carry in one call, for a script that knows the offset;
two `Report`s either side of it are the assertion that the ink moved.

## Raw images in and out

Three verbs on the document, each taking a path:

    doc.ImportImage(/path/to/photo.pam)    # a new layer, sized to the image, on top, active
    doc.ExportImage(/path/to/out.pam)      # the visible layers composited, alpha included
    doc.ExportLayer(/path/to/layer.pam)    # the active layer alone, with its alpha
    canvas.Render()                        # then show it, as after any scripted change

They work from the terminal on both substrates with no change of spelling. That is
the design: the browser hands the page a `File` and the desktop hands the shell a
path, and those are two ways of arriving at bytes at a name the process can open.
The page already stages every script it boots into the emscripten filesystem
(`preRun`: `FS.mkdirTree`, `FS.writeFile`), so an upload is one more file written
the same way and a download is one file read back. One import and one export, no
`#ifdef` in either, and the page holds no image code — it is a file proxy
(`PaintDocument::ImportImage`, and the PAM note above `PaintImage`).

The header has the two controls: **upload** writes the chosen file to
`/uploads/<name>` and calls `doc.ImportImage` then `canvas.Render`; **download**
calls `doc.ExportImage(/exports/paint.pam)`, reads the file back and hands it to
the browser as `paint.pam`. Every verb says in the terminal what it did — path,
size, layer — or why it did not.

The format is PAM (`P7`, `TUPLTYPE RGB_ALPHA`, `MAXVAL 255`) out, and PAM
(`RGB_ALPHA` or `RGB`) or PPM (`P6`) in, 8 bits per channel. That is the pixel
buffer with a text header — literally the raw pixels `Pixels_` stores — and GIMP,
ImageMagick and netpbm open and write it:

    convert photo.png -depth 8 photo.pam     # -depth 8: ImageMagick writes 16-bit PAM otherwise
    convert paint.pam paint.png

There is no image codec in `libs/`, and vendoring one is a decision about the tree
rather than about this feature. PNG is one header (stb_image / lodepng) away and
drops in at `paint_pam_read`: a second reader filling the same `PaintImage` is all a
second format costs. Anything else — 16-bit, grayscale, a PNG handed to the upload
button — is refused with a line saying what was found and what is accepted.

An import is a layer spawned the way the boot script spawns one (`addTag<PaintLayer>`
is what `doc.spawn(PaintProvider::PaintLayer)` reduces to), so it is the document's
typed child like every other and shows in `doc.Report()`. It is not in the undo
history, which snapshots the active layer's bytes; undoing an import is
`doc.RemoveLayer`. The export is what `RenderToSurface` shows less the view: visible
layers at their opacity, a lift in flight at its depth, onto a transparent page — not
the hover dim, not the selection outline, and not the text boxes, which are strings
drawn through a `Glyphs` target by RID; the export log counts those so a file that
lost its captions says so.

## The gear: a new canvas, a resize, save and load

The octagon in the ruler's top-left corner, above the extent label, opens a
settings menu over the sheet. It holds a width and a height stepped by 64
(64..8192), a 3x3 grid of anchor cells, and four buttons:

    resize   the page re-stated around its pixels: the bright cell is the part
             of the page that stays put, and new room is transparent -- paper
             on the paper layer, which is the bottom layer when nothing shows
             through it (PaintDocument::Resize)
    new      the same extent with every layer cleared, paper to white
    save     the header's download, from inside the sheet
    load     the header's upload -- see below

Neither `resize` nor `new` is an undo step: the history is three whole-layer
snapshots restored only into a buffer of the same size, so it is dropped rather
than left to refuse one press at a time. Both are verbs too:

    doc.Resize(1600, 1200, 4)     # w h anchor (0 top-left .. 4 centre .. 8 bottom-right)
    doc.New(1024, 768)
    canvas.Render()

The menu is the toolbar's bargain again: `PaintCanvasMenu` holds the pending
numbers and pushes them onto the readouts, the look is
`PaintProvider/scripts/paint_menu.etcs`, and every control in it is a rectangle
bound with `palette.AddCall(@node, @menu, PaintCanvasMenu.StepWidth, 64)` -- the
general entry, a node that calls a verb. The gear itself is
`palette.AddPopup(@gear, @menu_pane, @menu_input)`: pressing it opens the pane
the way the colour wheel opens (into the router and drawn, one fact), and a
press anywhere outside the pane closes it and is swallowed, so putting the menu
away never leaves a dab.

`save` and `load` cannot finish in the runtime -- a path becomes a file the user
can see only through the page -- so the verbs raise a DOM event (`etcs-menu`)
and `index.html` answers with the same download and upload the header buttons
run. `load` clicks the file input from a call proxied off the router's Worker;
a file dialog needs transient user activation, and whether the canvas press
that opened the menu still counts by then is the browser's decision and is
untested from this path. The header's `upload` is the fallback that always
works. On the desktop the two buttons log the verb to type instead
(`doc.ExportImage(<path>)` / `doc.ImportImage(<path>)`), since no file dialog
exists there yet.

## Deploying

`ace make loader etcs` and `ace make loaders` both now produce the INTERACTIVE
loader: `-DETCS_REPL_SHELL` is on by default for every loader build, because a wasm
loader that has returned from `main` is a page nothing can call into.

It used to be per-spelling, and the failure that made was quiet: without the define
`drive_main_loop_then_exit` takes the drain path, so `serve_paint.etcs` ran every
line — the server started, the mounts resolved, `ListPaths` printed — and then
`wait_for_environment_drain` reported "all detached executors finished" and the
process exited, because an `HttpServer` thread is not a detached executor. A serve
script that looked like it worked and left nothing listening.

Pass `-UETCS_REPL_SHELL` for the draining loader, which is the only way to ask for
it now.

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
