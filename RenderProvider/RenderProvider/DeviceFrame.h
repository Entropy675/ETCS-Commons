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
 * A SCENE IS A BLIT WHOSE SOURCE THE DEVICE DRAWS ITSELF. A camera projecting
 * on the device path does not rasterise: it hands over what it would have
 * drawn -- a view, a lens, and a mesh per node with that node's matrix,
 * derived from its OrderVector (ontology/OrderVector.h: the picture is a
 * projection of the rows) -- and the device draws that into a texture of its
 * own, colour and depth, keyed by the camera's RID. The Blit of the camera
 * then samples that texture exactly as it would a layer's bytes. So 3D is
 * not a second pass with its own composition: it is one more source, and the
 * list is still the whole composition. Geometry travels like pixels do: a
 * mesh is sent once, by RID, and re-sent only when it changed.
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

// One node of a scene: which mesh, where (column-major 4x4, the
// OrderVector's matrix times the node's extent), what colour -- and how much
// it GLOWS (Scene3D::SetGlow): 0 is a lit solid; above it the node gives
// light rather than taking it, ADDED to what is behind it and fading where
// its surface turns away from the eye, so its edges are soft -- and how
// much it SMOKES (Scene3D::SetSmoke): the same soft share, laid over what is
// behind it rather than added (a node with both is smoke). A scene's ops come
// solid first, then the soft ones farthest first (tested against the solids'
// depth, writing none of their own), so a backend switches pipeline as the
// soft ones change kind.
struct DeviceMeshOp
{
    ETCS::RID mesh = 0;
    float     model[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    float     color[4]  = { 1.0f, 1.0f, 1.0f, 1.0f };
    float     glow      = 0.0f;
    float     smoke     = 0.0f;
};

// The soft share's strength a backend sends (the model columns' w's carry
// forward times it), and the colour's alpha -- negated for smoke, which is
// how the shader tells the two apart (mesh.vert/mesh.frag, the WGSL pass).
inline float device_soft(const DeviceMeshOp& op) { return op.smoke > 0.0f ? op.smoke : op.glow; }
inline float device_alpha(const DeviceMeshOp& op) { return op.smoke > 0.0f ? -op.color[3] : op.color[3]; }

// The eye's forward in world space, from a scene's column-major view: its
// third row. What a glowing surface's edge is measured against.
inline void device_forward(const float view[16], float out[3])
{
    out[0] = view[2]; out[1] = view[6]; out[2] = view[10];
}

// A camera's projection, for the device to draw: into a texture of its own
// the size of the camera's plane, cleared to the camera's colour, with a
// depth buffer nobody else sees. Column-major view and projection; the
// projection maps depth to [0,1] and y up, and each backend applies its own
// clip convention on the way in.
struct DeviceScene
{
    ETCS::RID                target = 0;          // the camera: what a Blit of it samples
    uint32_t                 w = 0, h = 0;
    std::array<float, 4>     clear { 0.0f, 0.0f, 0.0f, 1.0f };
    float                    view[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    float                    proj[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    std::vector<DeviceMeshOp> meshes;
};

// A mesh's geometry, sent once: interleaved position, normal and tint, seven
// floats a vertex, and triangle indices. The TINT multiplies the node's
// colour: white for a Mesh (it has one colour, its node's), each source node's
// colour where many nodes were merged into one mesh (Scene3D, a still
// container's picture). It travels as one float holding r | g << 8 | b << 16,
// an integer below 2^24 and so exact in a float, on every device and through
// every copy (a packed bit pattern would be a NaN some copies rewrite).
constexpr int   kDeviceVertexFloats = 7;
constexpr float kDeviceTintWhite    = 16777215.0f;
inline float device_tint(float r, float g, float b)
{
    auto byte = [](float v) { return static_cast<uint32_t>(v <= 0.0f ? 0.0f : v >= 1.0f ? 255.0f : v * 255.0f + 0.5f); };
    return static_cast<float>(byte(r) | (byte(g) << 8) | (byte(b) << 16));
}
struct DeviceMeshUpload
{
    ETCS::RID             mesh = 0;
    std::vector<float>    vertices;   // x y z nx ny nz tint, per vertex
    std::vector<uint32_t> indices;    // three per triangle
};
// Six floats a vertex (a Mesh's, the unit box's) widened to the device's
// seven, white.
inline void device_widen(const std::vector<float>& six, std::vector<float>& out)
{
    out.clear();
    out.reserve(six.size() / 6 * kDeviceVertexFloats);
    for (size_t i = 0; i + 5 < six.size(); i += 6)
    {
        out.insert(out.end(), six.begin() + static_cast<long>(i), six.begin() + static_cast<long>(i + 6));
        out.push_back(kDeviceTintWhite);
    }
}

// The unit cube, centred on the origin, six faces each with its own normal:
// mesh 0, what a node with no mesh of its own is drawn as, scaled to its
// extent. Here, once, because three things need the same box -- the Mesh
// entity's Box(), and each backend's built-in copy that never travels.
inline void device_unit_box(std::vector<float>& vertices, std::vector<uint32_t>& indices)
{
    static const float n[6][3] = { {-1,0,0}, {1,0,0}, {0,-1,0}, {0,1,0}, {0,0,-1}, {0,0,1} };
    // Corner bits: 1 = +x, 2 = +y, 4 = +z; each face wound to face out.
    static const int corners[6][4] = { {0,4,6,2}, {1,3,7,5}, {0,1,5,4}, {2,6,7,3}, {0,2,3,1}, {4,5,7,6} };
    for (int f = 0; f < 6; ++f)
    {
        const uint32_t base = static_cast<uint32_t>(vertices.size() / 6);
        for (int k = 0; k < 4; ++k)
        {
            const int c = corners[f][k];
            vertices.insert(vertices.end(), { (c & 1) ? 0.5f : -0.5f, (c & 2) ? 0.5f : -0.5f, (c & 4) ? 0.5f : -0.5f,
                                              n[f][0], n[f][1], n[f][2] });
        }
        indices.insert(indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
    }
}

// Column-major a*b, for folding a scene's lens, view and a node's model into
// the one matrix a vertex needs -- done here, on the way to the device,
// rather than per vertex on it.
inline void device_mul4(const float a[16], const float b[16], float out[16])
{
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
        {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) s += a[k * 4 + r] * b[c * 4 + k];
            out[c * 4 + r] = s;
        }
}

struct DeviceFrame
{
    std::array<float, 4>          clear { 0.0f, 0.0f, 0.0f, 1.0f };
    std::vector<DeviceOp>         ops;
    std::vector<DeviceUpload>     uploads;
    std::vector<DeviceScene>      scenes;    // drawn before the ops, into their targets
    std::vector<DeviceMeshUpload> meshes;    // geometry the device lacks, sent beside the scenes
    uint32_t                      width = 0, height = 0;   // the surface's raster
};

/*
 * What a window surface needs from a device, and nothing more. Present is
 * the frame; Has says whether a source's texture is already there at a size
 * (so a surface that just switched to the device knows what it must send);
 * HasMesh the same for geometry; DeviceKey is the Renderable_/Device_ key --
 * equality only.
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
    virtual bool     HasMesh(ETCS::RID mesh) const = 0;
    virtual uint64_t DeviceKey() const = 0;
};

#endif // RENDERPROVIDER_DEVICEFRAME_H__
