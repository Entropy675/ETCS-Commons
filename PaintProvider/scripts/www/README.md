# ETCS Paint in the browser — two canvases

    cd modules/PaintProvider/scripts && etcs serve_paint.etcs
    then open https://localhost:8443/

Or as part of the whole site, at `/paint/` rather than at the root:

    etcs scripts/run_website.etcs          # http://localhost:8080/paint/
    etcs scripts/run_tls_website.etcs      # https://localhost:8443/paint/

Those two mount this directory with `FileHtmlPage::MountTree`, which gives it a
url prefix of its own — necessary because this page brings its own `index.html`
and the site already has one. Everything the page fetches is relative, so it
does not learn it was mounted; its one absolute link (`href="/"`) leads back to
the site's landing page, which is where it reads like it leads. The two
isolation headers are the same pair `serve_paint.etcs` sets, hoisted to the
server because there is one server.

The page holds the runtime, two canvases and the terminal:

    #canvas    1024x768   the sheet -- strokes land here
    #toolbar    640x64    the palette strip, above the ETCS Shell block
    iframe      shell     ShellProvider's own page, embedded as the terminal

Deploy is build, and that is all of it — there is no copy step any more:

    ace wasm make module WindowProvider
    ace wasm make module RenderProvider
    ace wasm make module PaintProvider
    ace wasm make module ShellProvider
    ace wasm make loader etcs

`ace wasm make` writes every web artifact into `bin/wasm/` (`WASM_DIR` in ETCS's
Makefile, `ARTIFACT_DIR` in the generated `loaders/Makefile`), the serve scripts
mount that one directory at `/wasm/`, and this page fetches its modules and its
glue from there. Nothing is copied beside the page: a per-page copy is a chance
to be one epoch behind, and a stale one does not 404 — it answers 200 and the
loader refuses it on a manifest hash (the Makefile's `WASM_DIR` note has the
whole argument).

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
it is where the edge ruler's marks every 100 px go. Drawn along the inside of the
drawing area instead they are on the picture — over the paper near the edges, and
paintable, so a stroke near a corner goes through the scale it is being checked
against (boot_paint_panels.etcs, at `paper_pane`).

Nothing enforces that the band is unpaintable; two facts already in the tree do it:

* `input.BindCanvas(@paper_pane)` — only a pick that lands on the canvas node is
  the picture, so a press in the margin is scenery and no tool sees it. The router
  pane stays `sheet_root`, so the toolbar and the zoom steps are picked as before.
* the pane is its own raster — the document blit is clipped by that buffer's extent
  rather than by a check, so the picture cannot reach the band in the first place.

`canvas.BindRulerFrame(@ruler_pane)` is what tells the surface which raster to
draw on: a sibling of the pane at the sheet's origin and size, retained (the
surface is its one writer) and flagged `passthrough` (`SetPassthrough(1)`; the
pick walk asks the flag beside `Hidden`, or every press on the paper would land
on it). Its own raster and not the sheet's,
because the sheet is composed on the frame edge's thread while `Render` draws on
the input thread; when the band was written straight into the sheet, which of
the two got there last decided whether a frame showed the band or the toolbar
over it -- measured at a third of the frames during a drag. With the ruler as a
node, the sheet is rebuilt from its children in order every recompose, nothing
else writes it, and a moved popup or window needs no re-render to uncover what
was under it. With nothing bound the ruler falls back to the inside of the pane,
which is all a page with no margin can have. The band sizes itself per side from the room the page
left (the toolbar takes the bottom strip, so there is no bottom band) and measures
its own labels, so the numbers are not clipped by a provider with a wider advance.
The margin is CLEARED to a fixed width (64px) each frame even though the band
follows the labels: the band is 36px at 100% and 31px at 125%, and a clear that
shrank with it left the previous width's pixels standing -- the stray "1" in front
of the extent at some zooms was the corner label drawn twice, five pixels apart. A
tick label that would start under the extent's tail is skipped for the same reason.

## What an anchored preview costs, and what it is allowed to cost

A continuous tool (brush, smudge) stamps one dab per sample: every sample is
worth having, and dropping one loses the middle of a fast stroke and nothing
else. An anchored tool (line, rect, ellipse, ruler, glyph, select, shape) throws
the whole preview away and rebuilds it -- the view is re-composited from the
document, then the outline is drawn over it -- so samples are coalesced to
100ms, which is the interval that rebuild can actually keep.

**The ruler and the shape get 400ms**, because their previews are the two whose
cost grows with the drag. The rebuild underneath is the same for every anchored
kind; what is drawn over it is not. A line is a line however long it is, while
the ruler lays ticks and numbers along its whole extent and a shape walks every
edge of a star or a diamond stamping a nib-sized rect per step. Drag either far,
or with a wide brush, and the per-sample cost climbs until 100ms stops being an
interval the rebuild can keep and becomes a queue. Two and a half previews a
second is still enough to aim a shape whose corners you can already see, and the
committed shape is exact regardless, because the release flushes.

This is a CPU-raster number. Once a device backend draws the preview the rebuild
stops scaling with the object, and this ladder should come back down to one
interval for every anchored kind.

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

The `move` slice is the first tool, between the last swatch and `brush` -- the
one tool that does not mark comes before the ones that do. The `select` slice
sits between `brush` and `anim` in the bar, which is now
seventeen slices (960x64, 32px in from either edge of the 1024 sheet; rect and
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

## Text boxes

The `text` tool drags a box; the box is a COLUMN. What is typed into it is set
in the box's font at the box's size and wraps where the box's width runs out --
between words, or inside one wider than the whole box -- and Enter starts a new
line. Lines that would pass the bottom of the box are not drawn; the open box
has a handle on its bottom-right corner that resizes it, and the text reflows as
it goes (`PaintDocument::wrap_text`). A press inside a box opens it; a drag from
inside moves it. Shift works: capitals and the shifted symbols of a US layout.

**The bar.** While a box is open a bar sits over it (`PaintTextBar`,
`paint_textbar.etcs`): the five fonts, the size (a ladder from 8 to 400, the
height of a line in the page's pixels), eight colours, `x` to remove the box and
`ok` to let it go. The fonts are the sheet's own pixel face and four TrueType
files that ship with the program (`PaintProvider/fonts`, each with its OFL
licence), drawn antialiased by `PaintFonts` through stb_truetype -- files rather
than the machine's fonts because a box has to wrap at the same words on every
page in a shared session. A font whose file is missing keeps its number and
draws in the pixel font (`PaintProvider/fonts/README.md` says where the files
come from). A new box starts in the last font and size the bar set.

**Undo and redo have buttons** under the picture, in the row above the zoom
steps (`boot_paint_panels.etcs`), for a hand on a touch screen: the same step
ctrl+z and ctrl+y take, through the pane's input so the view repaints with it
(`PaintInput::Undo`).

**And `redo alt`, when there are two ways forward.** Undo, then draw, and the
history forks: the stroke you undid and the one you drew are both children of
where you stood. Redo takes the newer; the older used to be a picture no key
could reach. `redo alt` (or ctrl+shift+y) takes it (`PaintDocument::RedoAlt`),
and the button is there only while the place you stand has a second branch.
The document publishes that on every move of its cursor (`PaintDocument::
forked`, an atomic), and the control follows it on the frame edge
(`PaintInput`'s `Animated` step) rather than where the history changed:
flipped from an input's thread it raced the compose walk and appeared only when
some later input drove another frame -- and a key reaches the input of the
pane it lands on, which is not the one holding the button. Not in a shared
session, whose record does not fork (an undo there is a line).

**Undo.** An edit is a step: from opening a box to letting it go -- the typing,
the font, the size, the colour, a move, a resize -- is recorded when it ends as
one `text` entry on the notebook (`PaintOpKind::Text`), carrying the whole box.
Undo and redo rebuild the boxes from those entries along the path, the same
walk that restores the pixels, so ctrl+z after typing a caption takes the
caption away and ctrl+y brings it back. Removing a box is a step; a box placed
and let go empty is not recorded at all. Ctrl+z with a box open ends the edit
first, then undoes it.

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

Seven rows is the window, not a limit: the **wheel over the window** scrolls
the stack a row per notch, up toward the top, clamped at both ends
(`PaintInput::RouteEvent` hands the notch to `PaintLayerPanel::Scroll` instead
of the zoom when the pointer is over the window). The rows are re-bound to
different layers rather than moved.

**The eye on the title bar folds the window to its bar**, and opens it again.
Folded, the window IS its bar: the pane shrinks to it (`paint_window_fold`),
since a pane is its whole rectangle to a pick and to the router -- a window
that only hid its rows kept the rectangle they left, and a stroke drawn toward
it stopped at an edge nobody could see. The sharing window has the same eye,
left of `end` (`PaintVisitors::PressView`).

The window re-renders the canvas itself whenever it changes the PICTURE rather
than the list -- a restack, an eye, a delete, a press that lands a carry
(`PaintLayerPanel::BindSurface`). A press on a row returns from the input edge
before any tool runs, so nothing else was asking the surface to draw, and the
rows updated while the canvas kept showing the arrangement from before the
press.

**Dragging a row by its grip** (the `::` strip, dots included) restacks. It
becomes a drag once the pointer has moved four pixels; before that a press and
release on the grip only chooses the row. While it is live the row turns amber,
a ghost of it -- thumb and name -- follows the pointer, and a bar sits in the
gap the layer will land in. Where it lands is read from the pointer's HEIGHT in
the window, clamped to the rows that hold layers, so a release in the gap
between two rows, on a row's dots, over the title or below the last layer still
lands; a release outside the window cancels (`PaintLayerPanel::DragRow`/`Drop`).

**Hovering an EYE** isolates that layer --
everything else fades to 0.25 -- so a layer can be found by looking. The eye and
not the row: isolating on the row meant the picture faded whenever the pointer
crossed the window on its way to anything, so the answer to "which layer is
this" arrived constantly and uninvited, and the thing being looked at was the
thing being hidden. The eye is the control that is ABOUT visibility, so hovering
it is the one moment where "show me only this layer" is what the hand is already
asking. A SHUT eye answers too: the hidden layer fades in to 0.75 as the rest
fades out (`PaintLayer::SetPeek`), so a layer you switched off can be looked at
without switching it back on. The peek is the screen's only -- an export, a
thumbnail and the eyedropper still see the layer as hidden.

It FADES rather than snaps, over about 150ms each way. A hover has a
duration -- the pointer rests on the eye for as long as the question is being
asked -- and that is exactly what a snap throws away. The step rides the same
clock the toolbar's click-and-hold repeat does: both types claim `Animated`
(`ontology/Animated.h`) and are advanced by whatever drives that family, and
both stop being advanced the moment they answer that they have nowhere left to
go. Stated in milliseconds rather than in frames, so the fade reads the same on
a 30Hz display and a 144Hz one. The title
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

## Animation: frames cut from the page and put back

The `anim` tool (beside `select`) drags a region on the page. That region is
the frame: the animation window comes up under the layer window, the region is
outlined on the sheet in the page's highlight, and from then on the page is the
drawing board and the window is the reel (`PaintAnimation`,
`PaintProvider/scripts/paint_anim.etcs`, rows from `paint_anim_row.etcs`).

    snap    a frame from what is visible in the region now, after the current
            one -- draw, snap, draw, snap builds the reel in order
    put     the current frame back onto the active layer, in the region, as one
            undo step (PaintDocument::PastePixels)
    < >     step the current frame; a press on a row does the same
    play    run the reel at the rate; the same button pauses. The window claims
            Animated (ontology/Animated.h) and honours dt, unlike the throbber,
            because "12 a second" is a duration and must mean the same on every
            display
    - +     the rate, 1..60 frames a second
    x       remove that frame
    gif     the reel as a GIF, to the page's download (`anim.gif`)

The window onto the reel is four rows; the wheel over it scrolls, and playing
keeps the current frame in view. Resizing the region drops the frames (a frame
is the region's size by definition); moving it keeps them. The outline is four
bars on the sheet placed through the surface's projection, re-placed as the
view pans or zooms -- on the sheet and not in the view pane, because the view
pane is the surface's raster and a child of a pane somebody else clears is
drawn only when the tree changes.

**GIF both ways.** An uploaded GIF with more than one frame goes straight to the
reel rather than to the layer-or-canvas question (`PaintCanvasMenu::OfferImport`
decides, since "a file came in" is the same event on every substrate): the
region takes the file's size at its own corner, the frames replace the reel,
and the file's delay sets the rate. A still GIF is a picture and takes the
question. Out is the encoder in `PaintProvider.h` (`paint_gif`), since stb has
none: one global 256-colour table by median cut over every frame, plain LZW,
every frame whole, looping. Not small, and every viewer plays it -- verified by
exporting a reel and reading it back through the same upload.

## Pages: `new` keeps the one you were on

The gear menu's **new** makes a NEW PAGE at the size the two steppers show. It
used to call `PaintDocument::New`, which clears the layers where they stand: the
picture that was there was gone, the history gained nothing, and the page list
stayed empty however many times it was pressed. It goes through the store now
(`PaintPages::NewAt`) -- the present page is saved to its slot first, then a
fresh one opens -- which is what makes a second page exist to go back to.

**The list under the buttons is the store**, the layer window's bargain again
(`PaintPagePanel`, rows from `PaintProvider/scripts/paint_page_row.etcs`): a row
is `[thumb][name WxH][x]`, newest first, the page on screen highlighted, and
five rows is the window -- the wheel over the list scrolls it a row per notch.
The thumbnail is the picture the store took when the page was saved
(`page_thumbs`, 32x24, every visible layer averaged and composited), kept beside
the page so the list never decodes one to draw a row.

    thumb / name   load that page (the present is saved to its slot first)
    name, again    on the page already on screen: opens it for renaming, with
                   its name in the field -- Enter keeps, Escape drops
    x              delete that page from the store; the page on screen stays
                   on screen and gets a new slot at its next save

The store is the same sqlite database
`ctrl+PageUp` / `ctrl+PageDown` already stepped through, and it holds the
pixels: each layer goes in as its raw bytes behind a PAM header (about 6 MB for
a two-layer 1024x768 page), so a page comes back as the picture it was rather
than as its dimensions. Verified end to end in the browser: a mark on page 1,
`new`, a different mark on page 2, then the page-1 row -- and the first mark is
back and the second is gone.

Saving, loading and deleting those bytes takes a moment -- a second or so in
the browser -- on the thread the press arrived on, so the pointer is not
answered until it is done. The store raises a `busy` state tag on itself for
exactly that interval (`PaintPages::Waiting`), and the session's throbber follows the
flag (`main_throbber.Watch(@pages, busy)` in `boot_paint_panels.etcs`,
`Throbber::Watch`): the frame edge is another thread and reads the flag once a
frame, so the ring turns while the store works and goes when the page is up.
Neither side knows the other exists -- anything else that raises `busy` gets
the same indicator. It sits over the middle of the canvas whatever size the
window made it: `main_throbber.CenterOn(@paper_pane)` has it read the pane's box
each frame it shows.

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
    save     the present page into its slot in the store (`PaintPages::Save`),
             the same store the page list below the buttons reads; the header's
             download is where a PNG leaves the page
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

`load` cannot finish in the runtime -- a path becomes a file the user can see
only through the page -- so the verb raises a DOM event (`etcs-menu`) and
`index.html` answers with the same upload the header button runs (so `load`
ends in the same layer-or-canvas prompt); `save` raises the same event only on
a runtime with no store bound, where the download is the one place a picture
can go. `load` clicks the file
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

## Sharing a canvas

`share` in the header opens a session on the node that served the page (`/art`,
`PaintNode`, started by `paint_lobby.etcs`) and puts the link in the header.
Whoever opens the link joins. Everything travels as notebook lines through that
node: each page pushes what it made and reads what everyone else made, on a
timer (`index.html`, `pushMine` / `readTheirs`).

**Joining takes the host's page, and keeps yours.** The session opens with a
BASELINE, not with history: a `page` line (the page's size and its layer stack)
and a keyframe of every layer (`PaintDocument::ExportBaseline`). A page loaded
from the store or opened from a file is pixels no entry describes, so pushing
the history from zero sent the strokes without the picture under them. A
joiner first puts their own canvas away (`PaintPages::Stash`: saved to the page
list if it changed, and then in no slot), and the `page` line then makes their
document the host's -- same size, same layers -- before the keyframes fill it
(`PaintDocument::AcceptOp`). A change to the layer stack made later travels the
same way and changes every stack in the room.

**What you push is what you have not sent.** Every entry carries a mark saying
it is in the record (`PaintOp::sent`): set when it arrives from the session,
when it goes out, and on a keyframe when it is taken. A push is the entries on
your path without it, so nothing about it depends on how your notebook happens
to be numbered. It used to be "everything after my entry N", and a `page` line
from the room empties the notebook under N; that read as your own history
having been wound back, and you re-sent your whole page -- which emptied
everyone else's notebook, and they answered the same way, for as long as the
room lasted. Nobody could draw. A page-level change you make (New, a resize,
another page from the list) you now SAY you made (`m_page_changed`), and that
one push is your whole page.

**What you push is what you made.** Lines that arrived from the session sit in
your notebook too, so undo and keyframes see the whole picture, but they are
the room's already, and your keyframes are your own cache of the picture as
YOU derived it -- on another member one overwrote whatever they had drawn on
that layer since. Each member takes its own keyframes of arriving strokes
(`AcceptOp`), so the record past the baseline is changes and nothing else.

**Every change is a line.** A stroke is its path; everything else that
changes the picture is recorded as the change it is and pushed like one: a
layer's eye, opacity, name or place in the stack (`PaintLayer`'s setters, each
a pair of `layers` entries -- the stack before, the stack after), a new layer,
a merge, an import (the `layers` line then carries the layer's pixels), a
carried selection landing, a paste, a cut or a delete (`patch`: the rectangle
as it now is), a whole layer cleared (`clear`), a smear (`smudge`, its path).
Each used to be a whole-layer keyframe here and nothing at all to the room, so
a hidden layer was hidden on one canvas, and the picture check read that as a
divergence for as long as it stayed hidden. A mark made under a selection
carries the selection (`PaintOp::Clip`) and lands under it everywhere,
replay here included. Names travel as typed: `layer 3` used to arrive as
`layer_3`.

**A line is a change's exact input, and the change has one implementation.**
Every entry names its layer by the layer's key (`PaintLayer::key`: the same
number on every member, kept when an undo brings the layer back), never by a
RID, which is one runtime's own. Numbers travel with nine significant digits
(`paint_float_text`), which brings every float back to the same bits -- six
decimal places brought a brush size or an opacity back as a neighbouring value,
and a replay of the same input drew a different picture. And a change made here
is BUILT as its entry and handed to the document, which lands it through the
same code a replay runs: a shape, a fill or a cleared layer through `Perform`,
a stroke point by point through `StrokeTo` (the step `ApplyOp` takes for each of
its points), every change to the stack -- a new layer, a removal, a merge, an
import, a restack, a layer's eye, opacity or name -- through `PerformStack`,
which is reconcile and then the raster the entry carries, exactly what a
member reading it does. The replay itself is a call: `ImportOps` hands each
entry to the `Accept` verb by reference (`PaintOpRef`, the shape `RouteRef`
gives a request), so a replayed change passes the same dispatch a made one
does. `doc.Perform(<a record line without its sequence>)` replays a line from a
script.

**A reader's eye comes back off with every other edit**, since a layer's
visibility is part of the picture now; the hover peek still shows a hidden
layer to you alone.

**You are who the node says you are.** A page asks to join under the name it
keeps in the browser, and the node grants a free one -- suffixed when somebody
in the room has it -- which is the name the page then goes by, as the author of
what it pushes and the author it passes over on the way back in. The token is
the identity and is kept per tab, so a reload comes back as the same member and
a second tab is a new one. Three tabs of one browser used to be one member to
the node, and each reader dropped every one of the host's lines as its own.

**An undo is a line in the record.** In a session ctrl+z does not wind your
notebook back; it appends an `undo` entry naming YOUR newest stroke that still
stands -- by your name and its ordinal among your strokes, never by a sequence
number, since the node renumbers everything -- and every member, you included,
applies it the same way: the path is re-derived without that entry
(`PaintNotebook::EffectivePath`), from the last keyframe before it. Redo appends
the reverse. Only your own strokes are yours to take back. Winding a tree back
was what re-baselined the room with one page's whole picture on every undo,
and wiped the strokes the others had not sent yet.

**The record checks itself, two ways.** The node chains every stored line
(`PaintNode::Session::chain`, XXH3 seeded with the chain before it) and answers
the chain with the head; the runtime chains every line it takes in, own lines
included (`PaintDocument::ImportOps`), and the page compares the two after each
read. Equal heads with different chains is a line this page never took in --
which nothing else can tell from silence -- and the page reads the record again
from zero, whose `page` line replaces the document. Separately each member
sends, with its presence, the hash of what it MADE of the record
(`PaintDocument::PictureHash`: extent, each layer's place and pixels, the boxes)
and the record position it is the picture of, only once nothing of its own is
still to be pushed. A member at the owner's position whose picture differs for
three presence ticks running has diverged, whatever it received, and reads the
record again. The owner never resyncs to anyone.

**Out of step, the canvas waits, and the owner states the page if it has
to.** A member that finds itself out of step -- either check -- raises
`syncing` on the document (`PaintDocument::Syncing`) while it reads the record
again from zero: the throbber shows (it watches that flag as well as the page
store's `busy`), and the canvas starts no stroke until the read lands, or for
fifteen seconds at most. What it had pending survives the read. The red line
that said so goes once the picture is the owner's again. A second time without
having got back into step in between -- or a line the runtime cannot read,
which no re-read changes -- means the record will not rebuild this picture, so
the member asks for the page whole (the last field of its presence); the owner
states it with its next push (`PaintDocument::Restate`), at most every twenty
seconds, and every member follows it as a new page. The room's history starts
again there -- an undo counts its author's strokes from the page, and a count
that went on from before it would name a stroke other members no longer have
-- so the owner's does too. A page this runtime states is passed over when it
comes back (`write_baseline` remembers its Page line), with its own lines
still in flight ahead of it, which are in it already.

**Draw now; the room decides.** A change you make lands on your canvas at
once and is held as PENDING (your own entry, not yet read back from the node:
`PaintOp::confirmed`). The node is the source of truth, so nothing is refused
up front -- a reader draws like anyone -- and what the room refuses comes back
off. Everyone's lines, the host's included, arrive through the same read. When
another member's line arrives while you hold pending entries, the document
REWINDS: it sets the pending ones aside, appends what arrived after the
confirmed ones, and when the read is done puts yours back on top and replays
the page once (`PaintDocument::ImportOps`), so every member ends with the
node's order -- a stroke drawn over yours while yours was in flight used to
land under it on your canvas and over it on everyone else's. Your own line
coming back confirms the entry it was pushed as (matched by its text, in
order); one pushed earlier that never came back was not taken, and goes. A
push the node refuses -- you are a reader, or were just made one -- or that
fails takes every pending entry off (`doc.RevertPending`, and the page says
once that the room has you as a reader), so no mark of yours stands on your
canvas that is not on the host's. An open stroke or an open text box survives
the rewind and is put back where it was. Promotion takes effect within a few
seconds (the page asks the node for its role on a timer). A document change
takes the document's lock (`PaintDocument::m_doc_mu`), since the read runs on
its own thread and your pen on another.

**Everyone has a sharing window** (`PaintVisitors`, drawn by
`paint_visitors.etcs`), opened as the host's or a guest's (`OpenAs`). Its top
line is you: the name you go by in the room -- two words and a number made up
the first time, kept in the browser -- pressed to rename it (the field takes
every key until Enter or Escape; the node refuses a name somebody there
already has), and the colour your frame is drawn in on everyone else's canvas,
with eight swatches to change it. Under that, who is here, each with their
colour and role, your own row lit. The host's window adds `copy link` and, on
every row but their own, `draw` (make a writer), `view` (back to reader) and
`out`; its `end` ends the session. A guest's has `leave` instead. The title bar
moves the window: the router holds the pointer on it for the length of the drag
(`PaintRouter::Route`, the capture), because it is its own pane and a fast
flick would otherwise leave it behind.

**Text boxes travel too, one hand at a time.** Selecting a box claims it: the
page asks the node, which gives each box to the first person who asks and to
nobody else until they let go (`claim/<key>`; a box is named in the room by who
made it and their number for it, `PaintTextBox::key`). Someone else pressing a
held box is told who has it, and anything they typed into it is put back
(`PaintDocument::TextDenied`). The text bar being up IS the claim. Letting go
-- Escape, `ok`, a press elsewhere, or twenty seconds without a key -- ends the
edit, which is recorded as one `text` entry and pushed like any stroke, and
only then released, so it is in the room before anyone else can take it. The
node refuses a box entry from anyone but the holder, and a claim nobody has
touched for twenty seconds lapses. What travels is each finished edit, not the
keystrokes.

**Pushes of any size.** A request to the node is bounded (64 KB with its
headers, `ETCS_NETWORK_MAX_HEADER_SIZE`) and a keyframe is a layer's PNG, so a
push bigger than one request goes as numbered parts the node joins back
together before reading a line (`part/<i>/<n>`). The server hands a request on
only once its whole body has arrived (`PicoHTTPParser::FeedRaw` reads to the
`Content-Length`); it used to hand it on at the end of the headers, and a
browser that sent the body as a second segment pushed an empty part -- the
node then joined a keyframe from its second half, and every member logged it
as an unreadable `snap` line. The node's answer lives with the request
(`RouteRequest::reply`) rather than on the node, which is what two members
polling at once used to overwrite in each other's replies.

## On a phone

**Bigger under a finger.** On a touch screen the page tells the window a
framebuffer smaller than its box and lets the canvas stretch back over it
(`UI_SCALE`, `stageResize` in `index.html`), so every pane, row and button the
boot script lays out in its own pixels is that much bigger on the glass, with no
script knowing; GLFW maps a touch through the same ratio, and the picture keeps
its own pixels through the zoom. The scale is the width's to give -- one at
512 CSS pixels and under, two at 1024 and over -- because the toolbar is laid
out 640 wide and scaled down to fit anything narrower: below that width it is
already as small as the width makes it, and halving the framebuffer would only
halve the picture. `?ui=<n>` overrides. A phone held upright therefore gets no
bigger toolbar; that wants the reflow noted under "What is not solved".

**The keyboard comes up when something takes keys.** A phone raises its
keyboard for a focused field, and the canvas has none. So the page keeps one,
invisible (`#keys`), and while the runtime says a box or a name field is taking
keys (`PaintRouter::Editing`, polled, and asked right after every tap) that
field is focused and the keyboard is up; when the edit ends the field lets go.
What the keyboard delivers is characters, not keys -- its key events carry no
code -- so the page hands them to the router as presses (`PaintRouter::Type`,
through the same US-layout table the key path reads). Enter and backspace do
arrive as keys, and GLFW's window listener takes those as it always did. A
focus made from a timer with no tap behind it is refused on iOS, so a keyboard
button sits in the corner while something is taking keys, for when the tap that
opened the box was not enough. Desktop pages leave all of this off (`?kbd=1`
turns it on for a look).

**A short screen is the canvas's.** Under 30rem of height -- a phone on its
side -- the terminal keeps a 4rem strip and the stage takes the rest, where the
terminal's 15rem floor used to leave the picture a sliver.

## Deploying

`ace make loader etcs` and `ace make loaders` both produce the INTERACTIVE
loader: `-DETCS_REPL_SHELL` is on by default for every loader build, because a wasm
loader that has returned from `main` is a page nothing can call into.

Passing `-UETCS_REPL_SHELL` gets the draining loader, and its failure is quiet:
`drive_main_loop_then_exit` takes the drain path, so `serve_paint.etcs` runs every
line — the server starts, the mounts resolve, `ListPaths` prints — and then
`wait_for_environment_drain` reports "all detached executors finished" and the
process exits, because an `HttpServer` thread is not a detached executor. A serve
script that looks like it worked and leaves nothing listening.

## What is not solved here

A retracted structural entry (a layer added, removed or merged) is taken off the
path like a stroke, so the stack goes back to the previous `layers` entry -- but
a merge's carried bytes go with it, and a keyframe of the surviving layer taken
before the merge is what the layer falls back to. Nobody has undone a merge in a
room yet; when somebody does, that is where to look.

The strip renders and picks, but it does not follow the window: `ResizeTo` made it
a fixed 640x64, so a very narrow viewport scales it down rather than reflowing the
swatches. Reflow is a layout question for the 2D tree
(`paint_toolbar.etcs` states absolute positions in bar space), not for the surface.

3D does not draw on either canvas. `CanvasSurface` is `PixelsBase` — host bytes and
`putImageData` — while `Scene3D`/`Camera3D` draw through the device path
(`RenderableBase`), which in a browser means a WebGPU or WebGL context. A canvas has
exactly one context for its lifetime, so that is a different surface type on a
different canvas rather than an addition to this one.
