#ifndef RENDERPROVIDER_DEVICEFRAME_H__
#define RENDERPROVIDER_DEVICEFRAME_H__

#include "../../../ontology.h"

#include <array>
#include <cstdint>
#include <vector>

/*
 * ONE FRAME, AS A DEVICE DRAWS IT -- what a window surface hands the GPU
 * backend that presents for it (HostSurface.h).
 *
 * The window surface is a host raster first (there is always a CPU,
 * ontology/Device.h); with a Device child it stops rasterising and records
 * instead, and at Present the recording goes to one of these backends:
 * VulkanPresenter natively, WebGpuPresenter in a browser. The three verbs a
 * Surface_ has are the whole vocabulary -- Clear (the frame's colour), a
 * rect, and a blit of another surface's pixels -- so a frame is one clear
 * colour and one ordered list, in call order, because order IS the
 * composition (back to front).
 *
 * A BLIT NAMES ITS SOURCE BY RID, and the bytes travel beside the list only
 * when the device does not have them yet or they changed: a layer that did
 * not move since the last frame is a texture already on the device, and
 * re-sending it is the cost the observed bit exists to avoid.
 *
 * PLAIN OBJECTS, NOT ENTITIES. A backend is how one surface reaches one
 * device, owned by that surface and gone with it; nothing addresses it, so
 * it has no tag, no RID and no family -- the capability that IS addressable
 * is the Device child that caused it.
 */
struct DeviceOp
{
    enum class Kind : uint8_t { Rect, Blit };
    Kind      kind   = Kind::Rect;
    int32_t   x = 0, y = 0;
    uint32_t  w = 0, h = 0;          // the destination extent, source-sized when blitting 1:1
    float     c[4] = { 0.0f, 0.0f, 0.0f, 0.0f };   // Rect: colour. Blit: c[3] is the opacity.
    ETCS::RID source = 0;            // Blit only
};

// A source's pixels, copied when they changed -- at the Blit, so the frame
// shows what was blitted even if the source is painted again before Present.
struct DeviceUpload
{
    ETCS::RID            source = 0;
    uint32_t             w = 0, h = 0;
    std::vector<uint8_t> bytes;      // Pixels_ layout: RGBA8, not premultiplied, stride w*4
};

struct DeviceFrame
{
    std::array<float, 4>      clear { 0.0f, 0.0f, 0.0f, 1.0f };
    std::vector<DeviceOp>     ops;
    std::vector<DeviceUpload> uploads;
    uint32_t                  width = 0, height = 0;   // the surface's raster
};

/*
 * What a window surface needs from a device, and nothing more. Present is
 * the frame; Has says whether a source's texture is already there at a size
 * (so a surface that just switched to the device knows what it must send);
 * DeviceKey is the Renderable_/Device_ key -- equality only.
 *
 * Present answering false means THIS BACKEND IS FINISHED -- the device was
 * lost, the window's surface went away -- and the window surface drops it
 * and draws on the host again. A frame that merely could not be shown (a
 * minimised window, a resize in progress) answers true.
 */
class DevicePresenter
{
public:
    virtual ~DevicePresenter() = default;
    virtual bool     Present(DeviceFrame& frame) = 0;
    virtual bool     Has(ETCS::RID source, uint32_t w, uint32_t h) const = 0;
    virtual uint64_t DeviceKey() const = 0;
};

#endif // RENDERPROVIDER_DEVICEFRAME_H__
