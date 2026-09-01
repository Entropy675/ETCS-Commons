#include "RenderProvider.h"

// Surface is HYBRID: its frame pump is a produce/consume pair, which is
// what lets a renderer run on its own thread while the window keeps the
// poll loop (scripts/render_frames.etcs, and RenderProvider.h's own comment
// on why the clock and the Vulkan work sit on the sides they do).
// Instance and ImageSurface stay BASIC -- neither has anything continuous
// to carry.
ETCS_MODULE_EXPORT_MAIN(RenderProvider, "Instance Surface ImageSurface PolygonDrawable2D")

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
    (Create, Clear, DrawRect, Blit, Present, Delete, RunDemo),
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
