#include "WindowProvider.h"



// Define the DLL and its tag types (space separated list)
ETCS_MODULE_EXPORT_MAIN(WindowProvider, "Window")

// Define Window tag's functions (name, (work funcs), (stream funcs))
ETCS_TAG_BLOCK_HYBRID(
    Window, 
    (Create, Delete, PollEvents, Close, Run, SetPosition, MoveBy, CenterOnMonitor,
     CaptureMouse), 
    (ProduceEvents, ConsumeEvents, ProducePointer, ConsumePointer)
)


