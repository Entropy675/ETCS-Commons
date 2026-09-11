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
/*
 * The one lookup that is a single tag rather than a family: Instance is flat by
 * design (no ontology supertype but Deletable), so its tag row is the only
 * place it appears.
 *
 * HERE RATHER THAN IN RenderProvider.h, where it used to sit beside its only
 * caller. RenderProvider/Device.h resolves an Instance the same way and is
 * included from THIS file, so a helper defined after these includes is one the
 * type headers cannot see. It is the same lookup for the same reason from both
 * sides; writing it twice is how the two stop agreeing.
 */
static inline ETCS::Entity* rp_resolve_tag(const char* tag, ETCS::RID rid)
{
    if (rid == 0) return nullptr;
    auto& ridMap = ETCS::EventNode::getInstance().ridMap;
    auto it = ridMap.find(ETCS::Buffer(tag));
    if (it == ridMap.end()) return nullptr;
    return it->second.invoke_get(rid);
}

// The device capability attachment. Beside these rather than under OS/ for a
// reason that looks like an exception and is not: what varies per platform is
// the Instance it names, which is already selected above -- this type only
// says "reachable from here", which is the same statement everywhere.
#include "RenderProvider/Device.h"

#include "RenderProvider/ImageSurface.h"
#include "RenderProvider/PolygonDrawable2D.h"
#include "RenderProvider/CompositeDrawable2D.h"

// The 3D pair, and CPU-only for the same reason ImageSurface is: the
// projection is arithmetic and a depth test, with no device object anywhere in
// it. Scene3D fills a camera's Pixels_ and Camera3D owns those pixels, so the
// result reaches the GPU by the route every other CPU-side surface already
// takes -- one Blit into a VulkanSurface. A device-side renderer would be a
// second concrete Drawable3D under OS/, selected here; it would not change a
// line of either header, which is the point of the seam being Project.
#include "RenderProvider/Scene3D.h"
#include "RenderProvider/Camera3D.h"

// The Glyphs leaf. AFTER the surfaces, because a label bound to a frame rate
// reads it off this platform's concrete Surface -- the rate is a property of
// the frame loop, not of the Surface family, so the type has to be complete
// here rather than reachable by family name.
#include "RenderProvider/TextLabel.h"

// auto generated hashes of headers:
#include "../../ETCS.h"
#include "module_hashes.h"

#endif // RENDERPROVIDER_CONTRACT__
