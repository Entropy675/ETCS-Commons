#ifndef RENDERPROVIDER_MESH_H__
#define RENDERPROVIDER_MESH_H__

#include "../../../core_defs.h"
#include "../../../ontology.h"
#include "DeviceFrame.h"

#include <array>
#include <cmath>
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

    /*
     * A SPHERE in the unit box (radius 0.5), `segments` bands from pole to
     * pole and twice that around, normals out from the centre so it shades
     * round. A node made Create(d, d, d) with this mesh and SetShape(sphere)
     * looks like the solid it is.
     */
    void Sphere(uint32_t segments)
    {
        Clear();
        const uint32_t bands = segments < 3 ? 3 : (segments > 64 ? 64 : segments);
        const uint32_t around = bands * 2;
        const float pi = 3.14159265358979f;
        for (uint32_t i = 0; i <= bands; ++i)
        {
            const float th = pi * static_cast<float>(i) / static_cast<float>(bands);
            for (uint32_t j = 0; j <= around; ++j)
            {
                const float ph = 2.0f * pi * static_cast<float>(j) / static_cast<float>(around);
                const float nx = std::sin(th) * std::cos(ph), ny = std::cos(th), nz = std::sin(th) * std::sin(ph);
                m_vertices.insert(m_vertices.end(), { 0.5f * nx, 0.5f * ny, 0.5f * nz, nx, ny, nz });
            }
        }
        const float c[3] = { 0.0f, 0.0f, 0.0f };
        for (uint32_t i = 0; i < bands; ++i)
            for (uint32_t j = 0; j < around; ++j)
            {
                const uint32_t a = i * (around + 1) + j, b = a + around + 1;
                if (i != 0)         outward(a, b, a + 1, c);
                if (i != bands - 1) outward(a + 1, b, b + 1, c);
            }
        ++m_version;
    }

    /*
     * A CYLINDER along +y in the unit box (radius 0.5, y from -0.5 to 0.5),
     * `segments` sides: the side shaded round (normals out from the axis),
     * the two caps flat. A wheel, laid on its side by the node's facing.
     */
    void Cylinder(uint32_t segments)
    {
        Clear();
        const uint32_t n = segments < 3 ? 3 : (segments > 128 ? 128 : segments);
        const float pi = 3.14159265358979f;
        const float c[3] = { 0.0f, 0.0f, 0.0f };
        // The side: a ring of vertices at each end, normals radial.
        for (uint32_t j = 0; j <= n; ++j)
        {
            const float a = 2.0f * pi * static_cast<float>(j) / static_cast<float>(n);
            const float nx = std::cos(a), nz = std::sin(a);
            m_vertices.insert(m_vertices.end(), { 0.5f * nx, -0.5f, 0.5f * nz, nx, 0.0f, nz });
            m_vertices.insert(m_vertices.end(), { 0.5f * nx,  0.5f, 0.5f * nz, nx, 0.0f, nz });
        }
        for (uint32_t j = 0; j < n; ++j)
        {
            const uint32_t a = 2 * j, b = a + 1, d = a + 2, e = a + 3;
            outward(a, d, b, c);
            outward(b, d, e, c);
        }
        // The caps: a fan about each end's centre, normals along the axis.
        for (int end = 0; end < 2; ++end)
        {
            const float y = end ? 0.5f : -0.5f, ny = end ? 1.0f : -1.0f;
            const uint32_t centre = VertexCount();
            m_vertices.insert(m_vertices.end(), { 0.0f, y, 0.0f, 0.0f, ny, 0.0f });
            for (uint32_t j = 0; j <= n; ++j)
            {
                const float a = 2.0f * pi * static_cast<float>(j) / static_cast<float>(n);
                m_vertices.insert(m_vertices.end(), { 0.5f * std::cos(a), y, 0.5f * std::sin(a), 0.0f, ny, 0.0f });
            }
            for (uint32_t j = 0; j < n; ++j) outward(centre, centre + 1 + j, centre + 2 + j, c);
        }
        ++m_version;
    }

    /*
     * AN ARROW along +y in the unit box: a square shaft from the bottom to
     * `head` (a fraction of the height from the top, the head's length), and a
     * four-sided head as wide as the box to the top. A node aimed along a
     * direction (Scene3D::Aim) with this mesh and its height set to a length
     * is that vector, drawn.
     */
    void Arrow(float shaft_width, float head)
    {
        Clear();
        const float w  = (shaft_width <= 0.0f || shaft_width >= 1.0f ? 0.25f : shaft_width) * 0.5f;
        const float hy = 0.5f - (head <= 0.0f || head >= 1.0f ? 0.35f : head);   // where the head starts
        // The shaft: a box from y = -0.5 to hy, each face its own normal.
        const float shaft_c[3] = { 0.0f, (hy - 0.5f) * 0.5f, 0.0f };
        const float xs[2] = { -w, w }, ys[2] = { -0.5f, hy }, zs[2] = { -w, w };
        auto corner = [&](int k) { return std::array<float, 3>{ xs[k & 1], ys[(k >> 1) & 1], zs[(k >> 2) & 1] }; };
        static const int faces[6][4] = { {0,4,6,2}, {1,3,7,5}, {0,1,5,4}, {2,6,7,3}, {0,2,3,1}, {4,5,7,6} };
        static const float fn[6][3] = { {-1,0,0}, {1,0,0}, {0,-1,0}, {0,1,0}, {0,0,-1}, {0,0,1} };
        for (int f = 0; f < 6; ++f)
        {
            uint32_t v[4];
            for (int k = 0; k < 4; ++k) { const auto p = corner(faces[f][k]); v[k] = AddVertex(p[0], p[1], p[2], fn[f][0], fn[f][1], fn[f][2]); }
            outward(v[0], v[1], v[2], shaft_c);
            outward(v[0], v[2], v[3], shaft_c);
        }
        // The head: a pyramid on a square base at hy, apex at the top.
        const float head_c[3] = { 0.0f, hy + (0.5f - hy) * 0.25f, 0.0f };
        const float b[4][2] = { { -0.5f, -0.5f }, { 0.5f, -0.5f }, { 0.5f, 0.5f }, { -0.5f, 0.5f } };
        for (int k = 0; k < 4; ++k)
        {
            const float* p = b[k]; const float* q = b[(k + 1) % 4];
            // The face's normal: out through the middle of its base edge, tipped up by the slope.
            float nx = (p[0] + q[0]) * 0.5f, nz = (p[1] + q[1]) * 0.5f, ny = 0.5f * 0.5f / (0.5f - hy);
            const float l = std::sqrt(nx * nx + ny * ny + nz * nz); nx /= l; ny /= l; nz /= l;
            const uint32_t a = AddVertex(p[0], hy, p[1], nx, ny, nz);
            const uint32_t c = AddVertex(q[0], hy, q[1], nx, ny, nz);
            const uint32_t t = AddVertex(0.0f, 0.5f, 0.0f, nx, ny, nz);
            outward(a, c, t, head_c);
        }
        const uint32_t base[4] = { AddVertex(-0.5f, hy, -0.5f, 0, -1, 0), AddVertex(0.5f, hy, -0.5f, 0, -1, 0),
                                   AddVertex(0.5f, hy, 0.5f, 0, -1, 0), AddVertex(-0.5f, hy, 0.5f, 0, -1, 0) };
        outward(base[0], base[1], base[2], head_c);
        outward(base[0], base[2], base[3], head_c);
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
    // A triangle wound to face away from `inside` (a point within the convex
    // part it bounds), as device_unit_box's faces are: (b - a) x (c - a) out.
    void outward(uint32_t a, uint32_t b, uint32_t c, const float inside[3])
    {
        const float* A = &m_vertices[a * 6]; const float* B = &m_vertices[b * 6]; const float* C = &m_vertices[c * 6];
        const float ux = B[0] - A[0], uy = B[1] - A[1], uz = B[2] - A[2];
        const float vx = C[0] - A[0], vy = C[1] - A[1], vz = C[2] - A[2];
        const float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
        const float ox = (A[0] + B[0] + C[0]) / 3.0f - inside[0], oy = (A[1] + B[1] + C[1]) / 3.0f - inside[1], oz = (A[2] + B[2] + C[2]) / 3.0f - inside[2];
        if (nx * ox + ny * oy + nz * oz >= 0.0f) m_indices.insert(m_indices.end(), { a, b, c });
        else                                     m_indices.insert(m_indices.end(), { a, c, b });
    }

    std::vector<float>    m_vertices;
    std::vector<uint32_t> m_indices;
    uint64_t              m_version = 0;
};

#endif // RENDERPROVIDER_MESH_H__
