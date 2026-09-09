#include "ShellProvider.h"

// One tag. BASIC because a shell owns no continuous pump of its own: Run
// blocks for the length of a script and returns, and Detach hands the work to
// a child rather than starting a loop here.
ETCS_MODULE_EXPORT_MAIN(ShellProvider, "Shell")

ETCS_TAG_BLOCK_BASIC(Shell,
    Create, Run, Spawn, Detach, Bind, Halt, Report, Delete)

