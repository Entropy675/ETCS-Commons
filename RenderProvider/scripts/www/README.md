# RenderProvider in a browser — the 3D page

A projected 3D scene, in the same runtime the paint page runs in, served the same
way:

    cd modules/RenderProvider/scripts && etcs serve_scene3d.etcs
    # then open https://localhost:8443/

Build with `ace wasm make module RenderProvider` (and WindowProvider, ShellProvider
and the `etcs` loader); the artifacts land in `bin/wasm/`, the serve script mounts
that directory at `/wasm/`, and the page resolves its modules from there --
nothing is copied beside the page (the ETCS Makefile's `WASM_DIR` note has the
argument).

Move the pointer over the canvas to look around. The terminal beside it is the
other half of the page: the frame edge re-walks the tree every tick, so anything
you type takes effect on the next frame.

    Root> RenderProvider                              # read the RIDs back
    Root> eye.LookAt(0.0, 12.0, -6.0, 0.0, 0.0, 6.0)  # move the eye
    Root> tower_far.SetColor(1.0, 0.2, 0.2, 1.0)      # repaint one box
    Root> block.SetVisible(0)                         # hide the occluder
    Root> world.Order()                               # energy, heat, ticks

## What it is for

Everything the browser runtime had been shown to do was flat. A paint document, a
toolbar, a colour wheel: all of it reaches the screen through `Surface_` and
`Pixels_`, which is exactly the half of RenderProvider a 2D page needs. The
projection, the depth buffer, the camera-as-a-2D-node arrangement and the scene's
own motion integration had never run here at all, so whether they worked was an
assumption. This page is what turns that into an answer.

The files are the same shape as `PaintProvider/scripts/www`: `modules.json` is the
only thing fetched blind and names the providers, the scripts to stage and which
one is `argv[1]`; `index.html` is the loader and the canvas; the terminal is
ShellProvider's own page, mounted rather than copied.

`scene3d.etcs` appears twice, at two paths, because they are two jobs. This
directory's is the SESSION — window, surface, compositor, camera, pumps. The one
at `RenderProvider/scripts/scene3d.etcs` is the GEOMETRY, stated entirely in its
anchor's space, and is the same file the OS-side script runs.

## What the answer turned out to be

**The projection works.** The scene draws correctly here: sampling the frame gives
the camera's own background as sky, the ground slab's shade, and three distinct
tower shades, with the near block occluding the middle tower — which is the depth
buffer doing its job across two subtrees that share no parent but the root.

**A projection on a detached thread corrupts the heap.** This is an open bug, not
a browser limitation, and it is why the pumps in `scene3d.etcs` are arranged the
way they are. Bisected in this page with everything else held constant:

| arrangement | result |
| --- | --- |
| 2D tree only, frame edge detached | stable — this is what the paint page does |
| camera + scene, frame edge on the script's thread | stable, and it renders |
| camera + scene, frame edge detached | faults in `free()`, inside `Scene3D::ProjectConcrete` or `CompositeDrawable2D::recompose` |
| the above plus a detached key edge | faults inside the loader, through `checkMailbox` — a proxied cross-thread call |

The fault is always in a *free*, and never twice in the same place, which is what
heap corruption looks like from the outside: the write that does the damage and
the call that trips over it are different calls. The camera's buffer size makes no
difference (1024x768 and 256x192 fault alike), and projecting once on the script's
own thread before detaching does not help — so it is not a first-touch or a
warm-up problem either.

**What that costs the page: W/A/S/D.** The key edge is also the window's event
pump, so it wants the script's thread too, and only one edge can block it. With
the frame edge holding that thread, the keys have nowhere to go. The line is left
in `scene3d.etcs`, commented, next to the note explaining it.

**Also open, and smaller:** `eye.LookAt` from the terminal did not change the
picture in a measured run. The call lands, so the likely cause is that moving the
eye does not mark the camera's own render gate — worth checking next to the above
rather than separately.
