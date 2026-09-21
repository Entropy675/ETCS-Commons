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
The margin is CLEARED to a fixed width (64px) each frame even though the band
follows the labels: the band is 36px at 100% and 31px at 125%, and a clear that
shrank with it left the previous width's pixels standing -- the stray "1" in front
of the extent at some zooms was the corner label drawn twice, five pixels apart. A
tick label that would start under the extent's tail is skipped for the same reason.

## The brush's size is the width of the mark

`brush.SetRadius(n)` -- and the `size` stepper on the bar, which is the same
setting -- states how many document pixels wide the mark is. Size 1 draws one
pixel, size 6 draws six, and a rectangle stroked at size 1 lands exactly on the
box that was dragged.

It used to be a RADIUS, and the verb is still spelled that way because scripts
and the toolbar already say it; only the arithmetic changed. As a radius, size 1
put down every pixel inside `dx^2 + dy^2 <= 1` -- three across -- so the thinnest
available line was three pixels and every stroked outline stood a pixel outside
its own box on all four sides, while the preview drew `size` view-pixels wide and
so disagreed with what it was previewing. One footprint (`paint_stamp_of`, a
`PaintStamp`) now answers for the committed mark, the live dab on the view and
the smudge window, so the three cannot drift again. Even widths sit between
pixels so that a 2px mark is two pixels rather than three; stroking a path with a
wide brush still spills half the width either side of it, which is what stroking
means.

## A selection belongs to one layer

A lift is cut from the active layer and dropped onto the active layer, and the
whole of that contract is that the two are the same layer. Nothing enforced it:
the layer window is a different pane's input, so a press on a row never touched
the canvas's drag state, and a selection lifted from Ink and dropped after
clicking Paper wrote Ink's pixels into Paper. `PaintDocument::SetActiveLayer`
now lands any carry before the ground moves -- on the layer it came from, where
the user last put it -- which covers every caller at once: the panel, the
exported verb, a page load, an import.

## Selecting, and moving what is selected

The `select` slice sits between `brush` and `line` in the bar, which is now
sixteen 60px slices (960x64, 32px in from either edge of the 1024 sheet; rect and
oval share one `shape` slice whose arrow steps rect, oval, triangle, diamond, star,
with the current outline's name under the word in gold, as `select`'s mode is
under its word; the two steppers at the end carry `size` and `opacity` captions
the same way, always shown). One tool, four ways of drawing the boundary — the
word under `select` says which, and the arrow at the top of the slice steps
through them:

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

## Images in and out

Four verbs on the document, each taking a path:

    doc.ImportImage(/path/to/photo.png)    # a new layer, sized to the image, on top, active
    doc.ImportCanvas(/path/to/photo.png)   # a new page the image's size, with the image as a layer
    doc.ExportImage(/path/to/out.png)      # the visible layers composited, alpha included
    doc.ExportLayer(/path/to/layer.png)    # the active layer alone, with its alpha
    canvas.Render()                        # then show it, as after any scripted change

They work from the terminal on both substrates with no change of spelling. That is
the design: the browser hands the page a `File` and the desktop hands the shell a
path, and those are two ways of arriving at bytes at a name the process can open.
The page already stages every script it boots into the emscripten filesystem
(`preRun`: `FS.mkdirTree`, `FS.writeFile`), so an upload is one more file written
the same way and a download is one file read back. The page holds no image code —
it is a file proxy (`PaintDocument::ImportImage`, and the format note above
`PaintImage`).

**In**: PNG, JPEG, BMP, GIF (first frame), TGA — read by `stb_image`, vendored
into the module by its manifest (`manifests/PaintProvider.json`, fetched by `ace`
to `modules/PaintProvider/stb/`) — and PAM (`P7`, `RGB_ALPHA` or `RGB`) or PPM
(`P6`), read by the module's own parser. Everything lands as 8-bit RGBA
(`stbi_load_from_memory(..., 4)`), the `Pixels_` format. **Out**: PNG when the
path ends in `.png`, PAM otherwise. PAM stays because it is the page store's
blob format (the pages table keeps each layer as its raw bytes behind a text
header, no decode on load); PNG is for files a person opens elsewhere. Anything
else — 16-bit, a format stb does not know, a side over 16384 — is refused with a
line saying what was found and what is accepted, and so is a picture that would
not fit the browser's fixed heap (see the note under the gear).

An imported layer is PAGE-SIZED, with the image dropped into it at the origin.
It used to be created at the image's own extent, on the reasoning that a layer's
raster is its own and resampling on the way in would throw pixels away. What
that actually bought was a layer whose pixels were second class: every selection
operation works in document coordinates and lands through the layer's own
raster, so lifting the image and moving it wrote the pixels back outside those
bounds, where `DropPixels` clips -- and the picture vanished. Nothing is lost
that was ever going to be shown, since the composite clips to the page anyway,
and the path for "keep all of it" is the other answer to the prompt: **new
canvas** makes the page the image's size first.

An import arrives SELECTED, with the select tool already holding it. What
anyone does first with a picture they have just brought in is put it where they
want it, and that was three steps nobody was told about -- pick select, draw a
region round the image, then drag. The import knows the extent (the new layer's
raster IS the image), so it states it as the selection and switches the tool:
press inside and drag, and the first press lifts it.

The header's two controls sit beside the brand. **upload** writes the chosen file
to `/uploads/<name>` and calls `menu.OfferImport(<path>)`, which opens a prompt
over the paper asking what the picture is for: **new layer** (`ImportImage`),
**new canvas** (`ImportCanvas` — the page takes the image's size, every layer is
cleared, and the image goes on as a layer above the paper so its transparency is
kept and undoing it is still the row's delete), or **cancel**. The prompt is
`PaintProvider/scripts/paint_import.etcs`, a popup like the gear's menu: its
buttons are `palette.AddCall(@node, @menu, PaintCanvasMenu.ImportAsLayer)` and
so on, it is opened by `PaintPalette::OpenPopup` rather than by a press, and a
press anywhere else puts it away. With no prompt bound (a native session driven
from the terminal) `OfferImport` imports as a layer at once. **download** calls
`doc.ExportImage(/exports/paint.png)`, reads the file back and hands it to the
browser as `paint.png`. Every verb says in the terminal what it did — path,
size, layer — or why it did not.

An import is a layer spawned the way the boot script spawns one (`addTag<PaintLayer>`
is what `doc.spawn(PaintProvider::PaintLayer)` reduces to), so it is the document's
typed child like every other and shows in `doc.Report()`. It is not in the undo
history, which snapshots the active layer's bytes; undoing an import is
`doc.RemoveLayer`. The export is what `RenderToSurface` shows less the view: visible
layers at their opacity, a lift in flight at its depth, onto a transparent page — not
the hover dim, not the selection outline, and not the text boxes, which are strings
drawn through a `Glyphs` target by RID; the export log counts those so a file that
lost its captions says so.

## The layer window

Top-right of the paper: a title bar with a **+** on it and seven rows, built by
`PaintProvider/scripts/paint_layers.etcs` on the toolbar's bargain
(`PaintLayerPanel` maps nodes to layer actions and owns none of the drawing). A
row is `[eye][thumb][name .......][x]`, five nodes for five questions.

    +          a new, page-sized, transparent layer directly above the active
               one -- not on top of everything, because "add a layer" while
               working on layer 2 of 5 means one to draw on next to this
    eye        show or hide that layer; every layer has one, the base included,
               since hiding the paper to see through it is what it is for
    thumb      what is ON the layer, drawn from its own pixels over a checker.
               A name says which layer you MEANT; the picture says which one
               you are looking at, and with two imported images the names are
               all there otherwise is. Averaged, not sampled: a page is 1024
               wide and a thumb is 24, so one tap per pixel reads one source
               pixel in 1800 and a six-pixel stroke survives in none of them
    name       press to choose the layer, press again to rename it: the field
               opens on the row, takes every key until Enter (keep) or Escape
               (drop), and shows what is being typed with a caret. While it is
               open the keyboard is the panel's -- ctrl+z is a z
    x          remove the layer. The bottom row has none: the base is the
               page's ground and PaintDocument::RemoveLayer refuses it
               (ClearLayer empties it instead)

The window re-renders the canvas itself whenever it changes the PICTURE rather
than the list -- a restack, an eye, a delete, a press that lands a carry
(`PaintLayerPanel::BindSurface`). A press on a row returns from the input edge
before any tool runs, so nothing else was asking the surface to draw, and the
rows updated while the canvas kept showing the arrangement from before the
press.

Dragging a row onto another restacks; hovering one isolates its layer
(everything else dims to 0.25) so a layer can be found by looking. The title
bar is the handle -- press it and the window follows the pointer -- and there
is no close button, on purpose: the window is the only thing that says which
layer is active, and a window that can be dismissed will be.

**The rows say what the picture shows**, and that took three fixes. A layer's
own properties -- an eye, a rename, a restack, an opacity -- now touch the
document (`PaintLayer::touch_document`), so the window re-reads the stack after
a change made anywhere, not just after one made in the window; the comment that
promised this named a function that was never written. Hover isolation is
re-asserted whenever the rows are re-bound, so a row deleted or scrolled away
under the pointer cannot leave the picture faded with the eye column still
saying "visible". And a layer's opacity was applied twice on screen -- `BlitTo`
multiplies by it and `RenderToSurface` passed it in as well -- so a layer at 50%
showed at 25% while the export, which composites once, showed it at 50%.

By verb:

    layers.SelectRow(1)   layers.ToggleRow(0)   layers.RemoveRow(0)   layers.MoveRow(0, 1)
    doc.NewLayer()        layers.CommitRename(backdrop)               layers.Report()

## Pages: `new` keeps the one you were on

The gear menu's **new** makes a NEW PAGE at the size the two steppers show. It
used to call `PaintDocument::New`, which clears the layers where they stand: the
picture that was there was gone, the history gained nothing, and the page list
stayed empty however many times it was pressed. It goes through the store now
(`PaintPages::NewAt`) -- the present page is saved to its slot first, then a
fresh one opens -- which is what makes a second page exist to go back to.

The menu lists the last five, newest first, with the one on screen highlighted;
a press on a row loads that page. The store is the same sqlite database
`ctrl+PageUp` / `ctrl+PageDown` already stepped through, and it holds the
pixels: each layer goes in as its raw bytes behind a PAM header (about 6 MB for
a two-layer 1024x768 page), so a page comes back as the picture it was rather
than as its dimensions. Verified end to end in the browser: a mark on page 1,
`new`, a different mark on page 2, then the page-1 row -- and the first mark is
back and the second is gone.

## Sizes, and 1920x1080

The two steppers move in 64s, and 1080 is not a multiple of 64 -- so the extent
most people actually want could not be reached from this menu however long you
held the +. There is a row of presets now (`1920x1080`, `1280x720`,
`1024x768`), each one press, each setting both numbers at once
(`PaintCanvasMenu::SetExtent`); the steppers still do the fine work from
wherever a preset lands.

1920x1080 is 8.3 MB of raster per layer, and the guard that refuses a page too
big for the heap (`paint_heap_can_take`) passes it comfortably: the web loader
links with a 1 GB heap and the runtime's own arenas hold about 240 MB of it
after boot. Worth knowing at that size: a page saved to the store is its layers'
raw bytes, so a two-layer 1920x1080 page is about 16 MB per save.

## The gear: a new canvas, a resize, save and load

The octagon in the ruler's top-left corner, above the extent label, opens a
settings menu over the sheet. It holds a width and a height stepped by 64
(64..8192), a 3x3 grid of anchor cells, and four buttons:

    resize   the page re-stated around its pixels: the bright cell is the part
             of the page that stays put, every OTHER cell carries an arrow
             pointing away from it -- the direction the new room appears in, so
             the grid reads as a diagram of the resize rather than as nine
             buttons -- and new room is transparent, paper on the paper layer,
             which is the bottom layer when nothing shows through it
             (PaintDocument::Resize)
    new      a NEW PAGE at the size shown, the present one saved first
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
run (so `load` ends in the same layer-or-canvas prompt). `load` clicks the file
input from a call proxied off the router's Worker; a file dialog needs transient
user activation, and whether the canvas press that opened the menu still counts
by then is the browser's decision and is untested from this path. The header's
`upload` is the fallback that always works. On the desktop the two buttons log
the verb to type instead (`doc.ExportImage(<path>)` / `doc.ImportImage(<path>)`),
since no file dialog exists there yet.

**Memory.** The wasm heap is fixed at the size the loader was built with
(`-sINITIAL_MEMORY`, 512 MB by ACE's default; growth is off for the reason
`loaders/Makefile` gives) and the runtime's own arenas take ~240 MB of it after
boot, so a page is a budget. `resize`, `new` and every import ask first
(`paint_heap_can_take`: every layer at the new size, the mask, and the one old
layer `Rebase` holds while it copies) and refuse with the numbers -- `needs 832
MB and the page has 264 MB to spare` -- rather than letting `malloc` abort the
tab, which is what `unreachable executed` on a two-axis resize was.

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
