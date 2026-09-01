#ifndef RENDERPROVIDER_IMAGESURFACE_H__
#define RENDERPROVIDER_IMAGESURFACE_H__

#include "../../ontology.h"

// ImageSurface -- the offscreen, CPU-backed surface:
// [Surface + Pixels + Resizable + Deletable]. Not Presentable, because
// it has nowhere to present to; that is the whole point of the split
// (ontology/Presentable.h).
//
// This is the type a 2D editor's LAYER is. Its buffer, its dirty flag
// and its raster all live in Pixels_ (ontology/Pixels.h), so there is no
// Vulkan in this file at all and it sits at the module root rather than
// under OS/ -- nothing about it is platform-specific, the same way
// ChessProvider's own types are OS-invariant.
//
// The projection seam for PintaProvider is deliberately at TWO levels:
//
//   - C++: reach this entity's Pixels_ via
//     getInterfacePointer("Pixels"), then PixelData()/PixelStride() and
//     MarkDirty(). That is a raw ARGB-ish byte buffer of exactly the
//     shape Cairo's ImageSurface exposes (modulo the premultiplied-BGRA
//     conversion documented in Pixels.h), so a Pinta layer can be
//     projected onto one of these without copying through a work-func
//     boundary per stroke.
//
//   - Script/work-func: Clear/DrawRect/Blit below, which are enough to
//     compose and verify a layer stack without any C++ at all.
//
// Compositing a stack is Blit in layer order, either into another
// ImageSurface (CPU, this file) or straight onto the window's
// VulkanSurface (GPU upload, OS/VulkanSurface.h). Same call either way --
// which one you get depends only on which surface you call it on.
class ImageSurface : public SurfaceBase<ImageSurface>,
                      public PixelsBase<ImageSurface>,
                      public DeletableBase<ImageSurface>
{
public:
    // The ordering every Surface owes (Orderable, composed by SurfaceBase).
    // A layer stack is the motivating case: m_order is where a document says
    // which layer is above which, and the whole comparison set -- >, <=, >=,
    // ==, != -- is derived from this one operator by OrderableBase.
    //
    // Ties are equivalence, not identity: two layers at the same depth
    // compare equal here and are still two different entities. Identity is
    // the RID.
    int32_t m_order = 0;
    bool operator<(const ImageSurface& o) const { return m_order < o.m_order; }
    WIRE_TYPE_IDENTITY(ImageSurface);

    ImageSurface()  = default;
    ~ImageSurface() = default;

    // Allocate is idempotent for an unchanged size (Pixels_), so calling
    // this twice with the same dimensions does not throw away a layer's
    // contents.
    bool Create(uint32_t w, uint32_t h)
    {
        if (w == 0 || h == 0)
        {
            ETCS_LOG("ImageSurface", "Create called with a zero dimension (" << w << "x" << h << ").");
            return false;
        }
        Allocate(w, h);
        this->addTag("active");
        return true;
    }

    // --- Surface_ dispatch (SurfaceBase.h) ---

    void ClearConcrete(float r, float g, float b, float a) override
    {
        ClearTo(r, g, b, a);
    }

    void DrawRectConcrete(int32_t x, int32_t y, uint32_t w, uint32_t h,
                           float r, float g, float b, float a) override
    {
        FillRect(x, y, w, h, r, g, b, a);
    }

    // CPU composite of another Pixels_-bearing surface into this one --
    // the same call the window surface answers by uploading and drawing a
    // textured quad. w/h are accepted for signature parity with the family
    // and ignored: this path does not resample (see Pixels_::Composite for
    // why a layered editor composites 1:1 and lets the device scale).
    void BlitConcrete(Surface_* source, int32_t x, int32_t y,
                       uint32_t w, uint32_t h, float opacity) override
    {
        (void)w; (void)h;
        if (!source) { ETCS_LOG("ImageSurface", "Blit called with no source."); return; }
        Pixels_* px = static_cast<Pixels_*>(source->getInterfacePointer(ETCS::Buffer("Pixels")));
        if (!px)
        {
            ETCS_LOG("ImageSurface", "Blit source RID:" << source->getRID()
                     << " has no Pixels interface -- only a CPU-backed surface can be blitted from yet.");
            return;
        }
        if (px == static_cast<Pixels_*>(this))
        {
            ETCS_LOG("ImageSurface", "Blit source is this surface -- refusing to composite onto itself.");
            return;
        }
        Composite(*px, x, y, opacity);
    }

    // --- Resizable_ dispatch (ResizableBase, composed into SurfaceBase) ---

    WindowSize GetSizeConcrete() override
    {
        return { PixelWidth(), PixelHeight() };
    }

    // --- Deletable_ dispatch ---

    bool DeleteConcrete() override
    {
        std::string conjugate_key = getSourceModule().toString() + ":" + getSourceTag().toString();
        ETCS_LOG("ImageSurface", "firing self-DestroyEvent for RID:" << getRID());
        return ETCS::DestroyEvent{conjugate_key.c_str(), this}();
    }
};

#endif
