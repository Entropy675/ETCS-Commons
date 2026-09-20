#include "PaintProvider.h"

// Thin ontology layer on the existing Surface family. BASIC tags own no
// continuous pumps; PaintInput is HYBRID because ConsumeInput is the standing
// control-thread edge from Window::ProduceEvents (first detached script
// thread / OS event pump affinity).
ETCS_MODULE_EXPORT_MAIN(PaintProvider,
    "PaintDocument PaintLayer PaintTool PaintSurface PaintInput PaintPalette "
    "PaintRouter PaintLayerPanel PaintColorWheel")

// SetKind is what makes one tool eight: radius/colour/hardness vary
// independently of it, and the kind is the shape of the whole gesture rather
// than which nib is loaded. See PaintToolKind.
ETCS_TAG_BLOCK_BASIC(PaintTool,
    SetRadius, SetColor, SetKind, SetText, SetTextSize, SetTolerance,
    SetMotionCoalesceMs, SetAlphaPercent,
    BeginStroke, MoveStroke, EndStroke, CancelStroke, Delete)

// SetOrder is the layer's whole contribution to stacking -- the order is a
// relation on the leaf, not a container operation (see PaintLayer).
ETCS_TAG_BLOCK_BASIC(PaintLayer,
    Create, Clear, DrawPixel, DrawLine,
    SetOrder, SetName, SetVisible, ToggleVisible, SetOpacity, Report, Delete)

ETCS_TAG_BLOCK_BASIC(PaintDocument,
    Create, SetActiveLayer, ClearLayer, RenderToSurface,
    MoveLayerTo, RenameLayer, RemoveLayer, IsolateLayer, ClearIsolate,
    Report, Delete)

// The projection verbs sit beside Render because every one of them ends in a
// re-composite: pan and zoom are what Render draws WITH.
ETCS_TAG_BLOCK_BASIC(PaintSurface,
    Create, AttachDocument, SetTarget, Render,
    SetPan, PanBy, SetZoom, ZoomAt, ZoomBy, SetBackground,
    ZoomPercent, BindZoomLabel, Delete)

ETCS_TAG_BLOCK_HYBRID(PaintInput,
    (Create, BindDocument, BindTool, BindSurface, SetBrush,
     BindRoot, BindCanvas, BindPalette, BindPanel, BindGlyphs,
     BindWheel, SetWheelPane,
     Pointer, Press, Release, Report, Delete),
    (ConsumeInput, ConsumePointer, ConsumeRouted, ConsumeRoutedPointer))

// A mapping from picked node to tool setting, and nothing else -- it owns no
// pixels, because the 2D tree it points into already does. See PaintPalette.
// EVERY Add*/Set* DEFINED ABOVE IS LISTED HERE, and the list is the whole
// contract: a DEFINE_WORK_FUNC that the block does not name still compiles, is
// still linked into the module, and is simply not reachable -- the executor
// answers "does not provide requested action" and carries on. A script calling
// it therefore FAILS SILENTLY, which is how AddRadiusDelta / AddCoalesceDelta /
// SetRadiusReadout / SetCoalesceReadout shipped: paint_toolbar.etcs drew the
// +/- nibs and the numbers beside them, ten calls were refused at boot, and the
// controls were dead while every other control on the same bar worked.
ETCS_TAG_BLOCK_BASIC(PaintPalette,
    BindTool, BindSurface, AddColor, AddSize, AddTool, AddZoom,
    AddRadiusDelta, AddAlphaDelta, SetRadiusReadout, SetAlphaReadout,
    SetColorOf, Report, Delete)

// A mapping from picked node to layer action, and nothing else -- it owns no
// pixels for the same reason PaintPalette owns none. See PaintLayerPanel.
ETCS_TAG_BLOCK_BASIC(PaintLayerPanel,
    Create, BindDocument, AddRow, SetHoverDim, SetEyeColors, SetRowColors,
    Scroll, Refresh, CommitRename,
    SelectRow, ToggleRow, RemoveRow, MoveRow, ArmRename, HoverRow,
    Report, Delete)

// A region where POSITION means colour, rather than a node meaning one -- which
// is why it is not a PaintPalette entry. See PaintColorWheel.
ETCS_TAG_BLOCK_BASIC(PaintColorWheel,
    Create, BindTool, BindPalette, BindRouter, BindPane, SetValue,
    Open, Close, Pick, Report, Delete)

// HYBRID for the same reason PaintInput is: its consume edges are the standing
// control-thread ends of the window's producers, not one-shot calls.
ETCS_TAG_BLOCK_HYBRID(PaintRouter,
    (Create, AddPane, RemovePane, SetPassBudget,
     Pointer, Press, Release, PressButton, ReleaseButton, Report, Delete),
    (ConsumePointer, ConsumeInput))
