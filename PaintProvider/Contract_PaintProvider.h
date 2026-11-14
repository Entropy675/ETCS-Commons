#ifndef PAINTPROVIDER_CONTRACT__
#define PAINTPROVIDER_CONTRACT__

// PaintProvider is OS-invariant: document/layer/tool/surface/input types are
// pure CPU + ontology composition on top of RenderProvider::Surface. No
// per-platform concrete typedefs, so no platform directory: every type is in
// PaintProvider/, one header each, in the order they build on one another
// (each includes the one before it -- the order IS the dependency).
#include "PaintProvider/PaintColor.h"
#include "PaintProvider/PaintBrush.h"
#include "PaintProvider/PaintModifierKeys.h"
#include "PaintProvider/PaintTool.h"
#include "PaintProvider/PaintSelection.h"
#include "PaintProvider/PaintLayer.h"
#include "PaintProvider/PaintImage.h"
#include "PaintProvider/PaintFonts.h"
#include "PaintProvider/PaintTextBox.h"
#include "PaintProvider/PaintOp.h"
#include "PaintProvider/PaintNotebook.h"
#include "PaintProvider/PaintDocument.h"
#include "PaintProvider/PaintSurface.h"
#include "PaintProvider/PaintPages.h"
#include "PaintProvider/PaintPagePanel.h"
#include "PaintProvider/PaintAnimation.h"
#include "PaintProvider/PaintTextBar.h"
#include "PaintProvider/PaintPalette.h"
#include "PaintProvider/PaintLayerPanel.h"
#include "PaintProvider/PaintColorWheel.h"
#include "PaintProvider/PaintCanvasMenu.h"
#include "PaintProvider/PaintVisitors.h"
#include "PaintProvider/PaintInput.h"
#include "PaintProvider/PaintRouter.h"
#include "PaintProvider/PaintShare.h"

// auto generated hashes of headers:
#include "../../ETCS.h"
#include "module_hashes.h"

#endif // PAINTPROVIDER_CONTRACT__
