#include "PaintProvider.h"

// Thin ontology layer on the existing Surface family. BASIC tags own no
// continuous pumps; PaintInput is HYBRID because ConsumeInput is the standing
// control-thread edge from Window::ProduceEvents (first detached script
// thread / OS event pump affinity).
ETCS_MODULE_EXPORT_MAIN(PaintProvider,
    "PaintDocument PaintLayer PaintTool PaintSurface PaintInput PaintPalette")

ETCS_TAG_BLOCK_BASIC(PaintTool,
    SetRadius, SetColor,
    BeginStroke, MoveStroke, EndStroke, CancelStroke, Delete)

ETCS_TAG_BLOCK_BASIC(PaintLayer,
    Create, Clear, DrawPixel, DrawLine, Delete)

ETCS_TAG_BLOCK_BASIC(PaintDocument,
    Create, AddLayer, SetActiveLayer, ClearLayer, RenderToSurface, Delete)

ETCS_TAG_BLOCK_BASIC(PaintSurface,
    Create, AttachDocument, SetTarget, Render, Delete)

ETCS_TAG_BLOCK_HYBRID(PaintInput,
    (Create, BindDocument, BindTool, BindSurface, SetBrush,
     BindRoot, BindCanvas, BindPalette,
     Pointer, Press, Release, Report, Delete),
    (ConsumeInput, ConsumePointer, ConsumeRouted, ConsumeRoutedPointer))

// A mapping from picked node to tool setting, and nothing else -- it owns no
// pixels, because the 2D tree it points into already does. See PaintPalette.
ETCS_TAG_BLOCK_BASIC(PaintPalette,
    BindTool, AddColor, AddSize, Report, Delete)
