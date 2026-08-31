#include "RenderProvider.h"

// Two tags, both flat-BASIC: no stream functions in V1. When a frame
// loop stops being driven by RunDemo and starts being a real produce/
// consume pair (a script producing draw commands, the target consuming
// them), Target becomes HYBRID -- that is also the point at which the
// pending-draw list stops living on the CPU side of one work func.
ETCS_MODULE_EXPORT_MAIN(RenderProvider, "Instance Target")

// The Vulkan instance/device/queue/command pool. Spawn one, Create it,
// hand its RID to every Target.
ETCS_TAG_BLOCK_BASIC(Instance,
    Create, Delete)

// A 2D render target bound to a Window (spawn it as that window's child).
// Clear/DrawRect accumulate CPU-side; Present is the only call that
// touches the GPU queue.
ETCS_TAG_BLOCK_BASIC(Target,
    Create, Clear, DrawRect, Present, Delete, RunDemo)
