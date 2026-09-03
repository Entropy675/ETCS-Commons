#ifndef RENDERPROVIDER_CONTRACT__

#if defined(__EMSCRIPTEN__)
    // No WASM backend yet -- WebGPU would be the natural target, and it is
    // a different enough surface that it wants its own concrete types
    // rather than a #define around these ones.

#elif defined(_WIN32) || defined(__linux__)
    #define RENDERPROVIDER_CONTRACT__
    #include "OS/VulkanInstance.h"
    #include "OS/VulkanSurface.h"
    typedef VulkanInstance Instance;
    typedef VulkanSurface  Surface;

#else
    #warning "RenderProvider_Contract: Platform not detected (expects win/linux). Check preprocessor definitions."
    #error "Unsupported platform"
#endif

// Not typedef'd and not under OS/: ImageSurface is CPU-only (its buffer and
// raster live in ontology/Pixels.h), so there is no per-platform concrete
// type to select between -- the class IS the tag type on every platform, the
// same way ChessProvider's OS-invariant types are.
#include "ImageSurface.h"
#include "PolygonDrawable2D.h"
#include "CompositeDrawable2D.h"

// The 3D pair, and CPU-only for the same reason ImageSurface is: the
// projection is arithmetic and a depth test, with no device object anywhere in
// it. Scene3D fills a camera's Pixels_ and Camera3D owns those pixels, so the
// result reaches the GPU by the route every other CPU-side surface already
// takes -- one Blit into a VulkanSurface. A device-side renderer would be a
// second concrete Drawable3D under OS/, selected here; it would not change a
// line of either header, which is the point of the seam being Project.
#include "Scene3D.h"
#include "Camera3D.h"

// The layout solver. Not a Drawable2D and never in the tree it arranges --
// it computes where the others go and writes the answers back through their
// own family verbs. The vendored Clay it wraps is reachable from nowhere
// else (clay/VENDORED.md).
#include "ClayLayout.h"

// The Glyphs leaf. AFTER the surfaces, because a label bound to a frame rate
// reads it off this platform's concrete Surface -- the rate is a property of
// the frame loop, not of the Surface family, so the type has to be complete
// here rather than reachable by family name.
#include "TextLabel.h"

// auto generated hashes of headers:
#include "../../ETCS.h"
#include "module_hashes.h"

#endif // RENDERPROVIDER_CONTRACT__
