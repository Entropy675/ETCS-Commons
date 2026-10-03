#ifndef RENDERPROVIDER_MESH_H__
#define RENDERPROVIDER_MESH_H__

#include "../../../core_defs.h"
#include "../../../ontology.h"
#include "DeviceFrame.h"

#include <cstdint>
#include <vector>

// ---------------------------------------------------------------------------
// Mesh — geometry a scene node is drawn with on the device.
//
// NOT CAUSAL, NOT DRAWABLE. A mesh is the SHAPE a projection gives a node:
// the rows say where and which way (ontology/OrderVector.h); the extent says
// how big; the mesh says what it looks like. A node with no mesh is drawn as
// its box. Both paths draw the same triangles -- the device from the copy it
// was sent, the host rasteriser from these (Scene3D::rasterMesh) -- so a
// shape looks the same whichever path a camera is on.
//
// SENT ONCE, BY RID. The device keeps geometry the way it keeps a layer's
// pixels (DeviceFrame.h): keyed by this entity's RID, uploaded when it does
// not have it or the version moved. Every change here bumps the version,
// and a mesh nobody draws costs the device nothing.
//
// A vertex is a position and a normal, interleaved, and a triangle is three
// indices; flat shading comes from the normals a caller states. Box() is the
// unit cube with a normal per face, which is what a node without a mesh of
// its own scales to its extent.
// ---------------------------------------------------------------------------
class Mesh : public DeletableBase<Mesh>
{
public:
    WIRE_TYPE_IDENTITY(Mesh);

    Mesh() = default;
    bool DeleteConcrete() override { return true; }

    bool Create() { this->addTag("active"); return true; }

    void Clear() { m_vertices.clear(); m_indices.clear(); ++m_version; }

    // A vertex: where, and which way its face looks. Returns its index.
    uint32_t AddVertex(float x, float y, float z, float nx, float ny, float nz)
    {
        m_vertices.insert(m_vertices.end(), { x, y, z, nx, ny, nz });
        ++m_version;
        return static_cast<uint32_t>(m_vertices.size() / 6 - 1);
    }
    bool AddTriangle(uint32_t a, uint32_t b, uint32_t c)
    {
        const uint32_t n = VertexCount();
        if (a >= n || b >= n || c >= n) return false;
        m_indices.insert(m_indices.end(), { a, b, c });
        ++m_version;
        return true;
    }

    // The unit cube -- the device's own mesh 0 (DeviceFrame.h), as a starting
    // point a script can reshape, or a mesh a node names explicitly.
    void Box()
    {
        Clear();
        device_unit_box(m_vertices, m_indices);
        ++m_version;
    }

    // The geometry as it is kept, for a rasteriser on the host
    // (Scene3D::rasterMesh): the same interleaved floats Fill sends.
    const std::vector<float>&    Vertices() const { return m_vertices; }
    const std::vector<uint32_t>& Indices()  const { return m_indices; }

    uint32_t VertexCount()   const { return static_cast<uint32_t>(m_vertices.size() / 6); }
    uint32_t TriangleCount() const { return static_cast<uint32_t>(m_indices.size() / 3); }
    uint64_t Version()       const { return m_version; }

    // The geometry, for the device: what the surface sends when the device
    // says it lacks this RID (DeviceFrame.h).
    void Fill(DeviceMeshUpload& up) const
    {
        up.mesh     = getRID();
        up.vertices = m_vertices;
        up.indices  = m_indices;
    }

private:
    std::vector<float>    m_vertices;
    std::vector<uint32_t> m_indices;
    uint64_t              m_version = 0;
};

#endif // RENDERPROVIDER_MESH_H__
