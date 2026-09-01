#include "RenderProvider.h"

// Surface is HYBRID: its frame pump is a produce/consume pair, which is
// what lets a renderer run on its own thread while the window keeps the
// poll loop (scripts/render_frames.etcs, and RenderProvider.h's own comment
// on why the clock and the Vulkan work sit on the sides they do).
// Instance and ImageSurface stay BASIC -- neither has anything continuous
// to carry.
ETCS_MODULE_EXPORT_MAIN(RenderProvider, "Instance Surface ImageSurface PolygonDrawable2D CompositeDrawable2D Scene3D Camera3D TextLabel")

// The Vulkan instance/device/queue/command pool. Spawn one, Create it,
// hand its RID to every Surface.
ETCS_TAG_BLOCK_BASIC(Instance,
    Create, Delete)

// The window-bound presentable surface: spawn it as a Window's child.
// Clear/DrawRect/Blit accumulate in call order and are RETAINED until
// something composes a new frame; Present is the only call that touches the
// GPU queue, and ProduceFrames/ConsumeFrames is that same Present driven by
// a clock on another thread.
ETCS_TAG_BLOCK_HYBRID(Surface,
    (Create, Clear, DrawRect, Blit, Compose, Present, Delete, RunDemo),
    (ProduceFrames, ConsumeFrames))

// An offscreen CPU-backed surface -- a layer. Same drawing verbs, no
// Present (it has nowhere to present to, see ontology/Presentable.h), and
// its bytes are reachable as Pixels_ by anything holding its RID.
ETCS_TAG_BLOCK_BASIC(ImageSurface,
    Create, Clear, DrawRect, Blit, Delete)

// The scene-graph leaf: an arbitrary polygon, nested in another polygon's
// space. Owns no pixels -- it draws through whatever Surface it is realised
// onto, so the same tree lands on the window or on a CPU layer unchanged.
// Draw is the whole subtree in one call, which is what a scene living in the
// entity tree buys over one living in a script.
ETCS_TAG_BLOCK_BASIC(PolygonDrawable2D,
    Create, AddPoint, ClearPoints, SetFill, SetOrder, Draw, Clear, DrawRect, Blit, Delete)

// The merge point: a Drawable2D that owns pixels, so everything nested under
// it renders into its buffer and reaches the destination as one Blit. Drawing
// it when nothing beneath it changed costs that one blit and no subtree walk
// at all -- see CompositeDrawable2D.h on how Pixels_'s own dirty flag ends up
// serving both this and the device upload, in sequence.
ETCS_TAG_BLOCK_BASIC(CompositeDrawable2D,
    Create, SetPosition, SetOrder, SetBackground, Draw, Clear, DrawRect, Blit, Delete)

// The 3D scene node: a box, self-similar with its children, which projects its
// whole subtree into a camera against one depth buffer. HYBRID because of the
// input edge -- ConsumeInput is a stream consumer fed by a window's event
// producer, which is how w/a/s/d reaches the scene without this module knowing
// what a window is.
ETCS_TAG_BLOCK_HYBRID(Scene3D,
    (Create, SetPosition, Move, SetColor, SetOrder, SetSpeed, SetDamping, SetSensitivity, SetMouseDpi, SetScreenDpi, SetTurnsPerPass,
     Impulse, Halt, Order, SetEmissivity, SetVisible, Project, DepthAt, Delete),
    (ConsumeInput))

// The camera: a Drawable2D that owns pixels, filled by a scene rather than by
// its children. Everything downstream treats it as an ordinary 2D node, which
// is what lets a 3D view nest under a compositor, carry UI children, or blit
// into a window with no case anywhere for "this one is 3D".
ETCS_TAG_BLOCK_BASIC(Camera3D,
    Create, SetPosition, SetOrder, SetBackground, LookAt, SetLens, SetScene,
    Render, Draw, Clear, DrawRect, Blit, Delete)

// Text, as a Drawable2D that also claims Glyphs -- so a caption is a CHILD of
// whatever it labels and needs no drawing code at the call site. Its font is
// built in (TextLabel.h on why a font file would be four failure modes for a
// frame counter), and it draws through Surface_::DrawRect, so it lands on a
// compositor, a camera, an offscreen layer or the device surface identically.
ETCS_TAG_BLOCK_BASIC(TextLabel,
    Create, SetText, SetSize, SetPosition, SetOrder, SetColor, SetBackground,
    SetPadding, BindFps, Measure, Rasterize, Draw, Delete)
