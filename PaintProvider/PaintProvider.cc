// The image codecs' one compiled copy -- see the include note in the header.
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_TRUETYPE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
#include "PaintProvider.h"

// Thin ontology layer on the existing Surface family. BASIC tags own no
// continuous pumps; PaintInput is HYBRID because ConsumeInput is the standing
// control-thread edge from Window::ProduceEvents (first detached script
// thread / OS event pump affinity).
ETCS_MODULE_EXPORT_MAIN(PaintProvider,
    "PaintDocument PaintLayer PaintTool PaintSurface PaintInput PaintPalette "
    "PaintRouter PaintLayerPanel PaintColorWheel PaintCanvasMenu PaintPages PaintPagePanel PaintAnimation PaintNode PaintVisitors "
    "PaintFonts PaintTextBar")

// SetKind is what makes one tool eight: radius/colour/hardness vary
// independently of it, and the kind is the shape of the whole gesture rather
// than which nib is loaded. SetTip is the OTHER half of that sentence, and it
// was unreachable along with SetBlendMode and SetHardness until now -- see the
// note on this file's PaintPalette block for what an unlisted verb costs.
ETCS_TAG_BLOCK_BASIC(PaintTool,
    SetRadius, SetColor, SetKind, SetMode, SetShape, SetTip, SetBlendMode, SetHardness,
    SetText, SetTextSize, SetTolerance,
    SetMotionCoalesceMs, SetAlphaPercent,
    BeginStroke, MoveStroke, EndStroke, CancelStroke, Delete)

// SetOrder is the layer's whole contribution to stacking -- the order is a
// relation on the leaf, not a container operation (see PaintLayer).
ETCS_TAG_BLOCK_BASIC(PaintLayer,
    Create, Clear, DrawPixel, DrawLine,
    SetOrder, SetName, SetVisible, ToggleVisible, SetOpacity, Report, Delete)

// The selection verbs are the document's because the selection is: one per
// document, shown by every surface onto it (PaintDocument's selection note).
// So are the file verbs: an import is a new layer of THIS stack and an export
// is its composite, and both take a path (PaintDocument::ImportImage). And the
// extent verbs: Resize re-states the page around every layer at once, and New
// is that plus a clear (PaintDocument::Resize). ImportCanvas is New sized to a
// file plus ImportImage (PaintDocument::ImportCanvas).
ETCS_TAG_BLOCK_BASIC(PaintDocument,
    Create, SetActiveLayer, ClearLayer, RenderToSurface, NewLayer,
    MoveLayerTo, RenameLayer, RemoveLayer, IsolateLayer, ClearIsolate,
    BindGlyphs, AddTextBox, SetTextBoxText, RemoveTextBox,
    ShowTextBoxes, SelectTextBox,
    SelectRect, SelectEllipse, SelectColor, SelectPath, ClearSelection, MoveSelection,
    CopySelection, CutSelection, PasteSelection, DeleteSelection, Undo, Redo,
    ImportImage, ImportCanvas, ExportImage, ExportLayer, Resize, New,
    ExportOps, ExportBaseline, ImportOps, NotebookHead, PictureHash, PictureReport, SetAuthor, SetReadOnly,
    TextDenied, SetTextFont, SetTextSize, SetTextColor,
    MergeDown, MergeUp,
    Report, Delete)

// The projection verbs sit beside Render because every one of them ends in a
// re-composite: pan and zoom are what Render draws WITH.
ETCS_TAG_BLOCK_BASIC(PaintSurface,
    Create, AttachDocument, SetTarget, Render,
    SetPan, PanBy, SetZoom, ZoomAt, ZoomBy, SetBackground,
    ClearPeers, SetPeer, ViewRect,
    ZoomPercent, BindZoomLabel, BindGlyphs, ShowEdgeRuler, BindRulerFrame, Delete)

ETCS_TAG_BLOCK_HYBRID(PaintInput,
    (Create, BindDocument, BindTool, BindSurface, SetBrush,
     BindRoot, BindCanvas, BindPalette, BindPanel, BindPagePanel, BindVisitors, BindTextBar, Undo, Redo, BindAnimation, BindGlyphs, SetHoldCapacity, BindPages,
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
    BindWheel, AddWheelArrow, AddModeArrow, AddArrow, SetModeReadout, SetShapeReadout, SetTipReadout,
    SetHoldCapacity, AddHoverLabel,
    AddCall, AddPopup, OpenPopup, ClosePopup, BindRouter,
    SetColorOf, Report, Delete)

// The pages of this session, kept in a database -- see PaintPages.
ETCS_TAG_BLOCK_BASIC(PaintPages,
    Create, BindSurface, Save, Stash, Load, New, Next, Prev, List, Rename, Delete, Destroy)

// The text boxes' faces: the sheet's pixel font as font 0, TrueType files after
// it, all behind the one Glyphs family the document draws with. See PaintFonts.
ETCS_TAG_BLOCK_BASIC(PaintFonts,
    Create, BindPixel, Load, Report, Delete)

// The bar over an open text box: font, size, colour, ok and remove, as palette
// calls; it shows exactly while a box is open. See PaintTextBar.
ETCS_TAG_BLOCK_BASIC(PaintTextBar,
    Create, BindSurface, BindWindow, BindFont, BindSizeLabel, BindColor, SetFontTints,
    PickFont, StepSize, PickColor, Done, Remove, Delete)

// Frames cut from a region of the page and put back; a reel that plays in its
// window and travels as a GIF. See PaintAnimation.
ETCS_TAG_BLOCK_BASIC(PaintAnimation,
    Create, BindSurface, BindWindow, BindPreview, BindReadout, BindOutline, SetRowColors,
    BeginRow, RowNode, SetRegion, Snap, Put, Remove, Select, Next, Prev,
    SetFps, StepFps, Play, Pause, Toggle, ImportGif, ExportGif, Download, Scroll, Refresh, Report, Delete)

// The store as a list: node to page action, nothing drawn. See PaintPagePanel.
ETCS_TAG_BLOCK_BASIC(PaintPagePanel,
    Create, BindPages, BindMenu, BindWindow, SetRowColors, BeginRow, RowNode,
    Scroll, Refresh, Report, Delete)

// A mapping from picked node to layer action, and nothing else -- it owns no
// pixels for the same reason PaintPalette owns none. See PaintLayerPanel.
ETCS_TAG_BLOCK_BASIC(PaintLayerPanel,
    Create, BindDocument, BeginRow, RowNode, BeginTitle, BindView, BindBody, BindAdd, BindTitle, BindWindow, BindSurface,
    SetHoverDim, SetEyeColors, SetIrisColors, SetRowColors, SetDragColor,
    BindGhost, GhostNode, BindDropMark,
    Scroll, Refresh, CommitRename,
    SelectRow, ToggleRow, RemoveRow, MoveRow, ArmRename, HoverRow,
    Report, Delete)

// A region where POSITION means colour, rather than a node meaning one -- which
// is why it is not a PaintPalette entry. See PaintColorWheel.
ETCS_TAG_BLOCK_BASIC(PaintColorWheel,
    Create, BindTool, BindPalette, BindRouter, BindPane, BindSurface, SetValue,
    Open, OpenAt, SetTargetSlot, Close, Pick, Report, Delete)

// The settings menu's PENDING extent and anchor, and the two verbs that hand
// them to the document; and the import prompt, which is the same shape -- a
// pending path and the verbs that answer for it. Its controls are script
// nodes bound through PaintPalette::AddCall, which is why nothing here is a
// pick. See PaintCanvasMenu.
ETCS_TAG_BLOCK_BASIC(PaintCanvasMenu,
    Create, BindSurface, StepWidth, StepHeight, SetAnchor,
    BindWidthReadout, BindHeightReadout, BindAnchorCell, BindAnchorArrow,
    ApplyResize, ApplyNew, Save, Load, SetExtent,
    BindTool, BindPages, BindPagePanel, BindAnimation, PageChanged,
    BindImportPrompt, OfferImport, ImportAsLayer, ImportAsCanvas, ImportCancel,
    Report, Delete)

// HYBRID for the same reason PaintInput is: its consume edges are the standing
// control-thread ends of the window's producers, not one-shot calls.
ETCS_TAG_BLOCK_HYBRID(PaintRouter,
    (Create, AddPane, RemovePane, SetPassBudget,
     Pointer, Press, Release, PressButton, ReleaseButton, Key, Type, Editing, Report, Delete),
    (ConsumePointer, ConsumeInput))

// PaintNode -- a shared session's roster and its entries, and nothing else. It
// holds no document and decodes no entry: see its own header note for why a
// relay that understood its payload would have to be rebuilt every time the
// payload grew.
//
// Request is registered with HttpServer.AddRequestRoute, NOT AddRoute -- it
// reads the method and the body, and a path string carries neither.
ETCS_TAG_BLOCK_BASIC(PaintNode,
    Mount, Request, Filter, Delete)

// PaintVisitors -- who is in the session, as a pane on the sheet, for the host
// and for everyone who joined. Rows are declared by the script and filled here;
// the buttons are ordinary palette calls naming the node pressed. The title
// moves it (PaintInput, PressTitle). It never reaches the node: a press raises
// an event and the PAGE performs the verb, because a fetch is the page's.
ETCS_TAG_BLOCK_BASIC(PaintVisitors,
    Create, BeginRow, RowNode, BindWindow, BindTitle, BindHostOnly, BindGuestOnly,
    BindMe, BindSwatch, SetRowColors, SetInk,
    SetRoster, SetMe, Open, OpenAs, Close, Hide, CopyLink, PickColor, EditName,
    Promote, Demote, Remove, Delete)
