#include "RenderProvider.h"

// Every tag here is BASIC except Scene3D, whose input edge is a stream
// consumer (see its block). Surface included: its frame pump is a work function
// a script detaches, not a stream pair -- see the Surface block below.
ETCS_MODULE_EXPORT_MAIN(RenderProvider, "Instance Device Surface ImageSurface PolygonDrawable2D CompositeDrawable2D Scene3D Camera3D TextLabel Throbber")

// The GPU -- Vulkan natively, WebGPU in a browser. Spawn one, Create it, and
// name it from a Device under whatever should use it.
ETCS_TAG_BLOCK_BASIC(Instance,
    Create, Delete)

// The device capability attachment: spawn one under a Camera or a Surface and
// point it at an Instance, and that entity works through the GPU while the
// device is ready. Presence is the capability (ontology/Device.h) -- there is
// always a CPU, so only the addition needs saying.
ETCS_TAG_BLOCK_BASIC(Device,
    Create, Delete)

// The window surface: spawn it as a Window's child. A host raster, and the
// device's while a ready Device is under it (UseDevice says whether to use
// one). Clear/DrawRect/Blit accumulate in call order and are RETAINED until
// something composes a new frame; Present hands the frame to the window, and
// RunFrames is that same Present driven by a clock.
//
// BASIC: presenting is a step on the Presentable family
// (ontology/PresentableBase.h) and RunFrames is an ordinary work function a
// script detaches, so this tag has no stream, no ring and no pool occupant.
ETCS_TAG_BLOCK_BASIC(Surface,
    Create, SetTarget, ResizeTo, UseDevice, Clear, DrawRect, Blit, Compose, Present,
    Delete, RunDemo, RunFrames)

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
    Create, AddPoint, ClearPoints, SetOval, SetFill, SetOrder, SetHidden, Draw, Clear, DrawRect, Blit, Delete)

// The merge point: a Drawable2D that owns pixels, so everything nested under
// it renders into its buffer and reaches the destination as one Blit. Drawing
// it when nothing beneath it changed costs that one blit and no subtree walk
// at all -- see CompositeDrawable2D.h on how Pixels_'s own dirty flag ends up
// serving both this and the device upload, in sequence.
ETCS_TAG_BLOCK_BASIC(CompositeDrawable2D,
    Create, SetPosition, SetOrder, SetBackground, SetRetain, SetPassthrough, SetHidden, MoveTo,
    ResizeTo, FollowResize,
    Draw, Clear, DrawRect, Blit, Delete)

// The 3D scene node: a box, self-similar with its children, which projects its
// whole subtree into a camera against one depth buffer. HYBRID because of the
// input edge -- ConsumeInput is a stream consumer fed by a window's event
// producer, which is how w/a/s/d reaches the scene without this module knowing
// what a window is.
ETCS_TAG_BLOCK_HYBRID(Scene3D,
    (Create, SetPosition, Move, SetColor, SetOrder, SetSpeed, SetDamping, SetSensitivity,
     Impulse, Halt, Order, Look, SetEmissivity, SetVisible, Project, DepthAt, Delete),
    (ConsumeInput, ConsumeLook))

// The camera: a Drawable2D that owns pixels, filled by a scene rather than by
// its children. Everything downstream treats it as an ordinary 2D node, which
// is what lets a 3D view nest under a compositor, carry UI children, or blit
// into a window with no case anywhere for "this one is 3D".
ETCS_TAG_BLOCK_BASIC(Camera3D,
    Create, SetPosition, SetOrder, SetBackground, MoveTo, ResizeTo,
    LookAt, SetLens, SetScene, SetDeviceProjection,
    Render, Draw, Clear, DrawRect, Blit, Delete)

// Text, as a Drawable2D that also claims Glyphs -- so a caption is a CHILD of
// whatever it labels and needs no drawing code at the call site. Its font is
// built in (TextLabel.h on why a font file would be four failure modes for a
// frame counter), and it draws through Surface_::DrawRect, so it lands on a
// compositor, a camera, an offscreen layer or the device surface identically.
ETCS_TAG_BLOCK_BASIC(TextLabel,
    Create, SetText, SetSize, SetPosition, SetOrder, SetColor, SetBackground,
    SetPadding, SetHidden, BindFps, Measure, Rasterize, Draw, Delete)

// "ETCS" over a ring of cycling dots -- the wait indicator, as one entity at any
// size. A Drawable2D that owns pixels and claims Animated, so it rides the frame
// edge that is already running and SetHidden is both the switch and the whole of
// its state (Throbber.h on why there is no Start/Stop beside it).
//
// EVERY VERB HERE IS NAMED, which is not a formality: a DEFINE_WORK_FUNC missing
// from its tag block compiles, links, and fails silently at call time -- ten
// PaintProvider controls shipped dead exactly that way (PaintProvider.cc).
ETCS_TAG_BLOCK_BASIC(Throbber,
    Create, SetSize, SetStep, SetDots, SetColors, SetTextColor, SetText, SetPlate,
    SetPosition, SetOrder, SetHidden, Watch, CenterOn, Delete)
