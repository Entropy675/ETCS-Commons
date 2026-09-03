#include "LayoutProvider.h"

// One tag. BASIC because a layout owns no continuous pump: it is driven by
// size changes, and a size change already arrives on somebody else's edge --
// FollowResize hangs the re-solve off that rather than inventing a clock.
ETCS_MODULE_EXPORT_MAIN(LayoutProvider, "Layout")

ETCS_TAG_BLOCK_BASIC(Layout,
    Create, AddBox, SetDirection, SetPadding, SetGap,
    Solve, FollowResize, Report, Delete)
