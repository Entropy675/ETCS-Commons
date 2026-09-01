#include "RenderProvider.h"

// Three tags, all flat-BASIC: no stream functions yet. When a frame loop
// stops being driven by RunDemo and becomes a real produce/consume pair
// (a script producing draw commands, the surface consuming them), Surface
// becomes HYBRID -- that is also the point at which the pending-draw list
// stops living on the CPU side of one work func, and the point at which a
// renderer can be handed off to its own thread while the window keeps the
// poll loop (see scripts/render_frames.etcs).
ETCS_MODULE_EXPORT_MAIN(RenderProvider, "Instance Surface ImageSurface")

// The Vulkan instance/device/queue/command pool. Spawn one, Create it,
// hand its RID to every Surface.
ETCS_TAG_BLOCK_BASIC(Instance,
    Create, Delete)

// The window-bound presentable surface: spawn it as a Window's child.
// Clear/DrawRect/Blit accumulate in call order; Present is the only call
// that touches the GPU queue.
ETCS_TAG_BLOCK_BASIC(Surface,
    Create, Clear, DrawRect, Blit, Present, Delete, RunDemo)

// An offscreen CPU-backed surface -- a layer. Same drawing verbs, no
// Present (it has nowhere to present to, see ontology/Presentable.h), and
// its bytes are reachable as Pixels_ by anything holding its RID.
ETCS_TAG_BLOCK_BASIC(ImageSurface,
    Create, Clear, DrawRect, Blit, Delete)
