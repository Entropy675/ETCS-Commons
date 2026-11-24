#ifndef RENDERPROVIDER_MESH_H__
#define RENDERPROVIDER_MESH_H__

#include "../../../core_defs.h"
#include "../../../ontology.h"
#include "../../../ontology/Noise.h"
#include "DeviceFrame.h"

#include <algorithm>
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

    void Clear() { m_vertices.clear(); m_indices.clear(); m_part = 0; m_unit = { 1.0f, 1.0f, 1.0f }; ++m_version; }

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

    // A box added to what is here: centred at (cx, cy, cz), w x h x d, its
    // faces as the unit cube's -- so a shape is built of boxes, in the unit
    // box a node scales to its extent (a part may reach past it).
    void AddBox(float cx, float cy, float cz, float w, float h, float d)
    {
        if (w <= 0.0f || h <= 0.0f || d <= 0.0f) return;
        std::vector<float> v;
        std::vector<uint32_t> ix;
        device_unit_box(v, ix);
        const uint32_t base = VertexCount();
        for (size_t i = 0; i < v.size(); i += 6)
            m_vertices.insert(m_vertices.end(), { cx + v[i] * w, cy + v[i + 1] * h, cz + v[i + 2] * d, v[i + 3], v[i + 4], v[i + 5] });
        for (uint32_t k : ix) m_indices.push_back(base + k);
        ++m_version;
    }

    // The box the parts after it are said in: the extent a node will scale
    // this mesh by. As made, the unit box.
    void Unit(float w, float h, float d) { if (w > 0.0f && h > 0.0f && d > 0.0f) m_unit = { w, h, d }; }
    /*
     * A PART added to what is here: `src`'s triangles (the unit box when
     * null) sized to twice (hx, hy, hz), turned by the quaternion and moved
     * to (px, py, pz) -- a modelled part as it stands -- then said in the
     * Unit box. Its normals stay as the part faces: a node's extent turns no
     * normal (Scene3D::rasterMesh), so scaled back it faces as modelled.
     */
    void AddPart(const Mesh* src, float px, float py, float pz, float hx, float hy, float hz,
                 float qw, float qx, float qy, float qz)
    {
        if (hx <= 0.0f || hy <= 0.0f || hz <= 0.0f) return;
        const float ql = std::sqrt(qw * qw + qx * qx + qy * qy + qz * qz);
        if (ql <= 0.0f) return;
        qw /= ql; qx /= ql; qy /= ql; qz /= ql;
        auto turn = [&](float& x, float& y, float& z) {   // q v q*: v + 2w(q x v) + 2 q x (q x v)
            const float cx = qy * z - qz * y, cy = qz * x - qx * z, cz = qx * y - qy * x;
            const float dx = qy * cz - qz * cy, dy = qz * cx - qx * cz, dz = qx * cy - qy * cx;
            x += 2.0f * (qw * cx + dx); y += 2.0f * (qw * cy + dy); z += 2.0f * (qw * cz + dz);
        };
        std::vector<float> box;
        std::vector<uint32_t> boxIx;
        if (!src) device_unit_box(box, boxIx);
        const std::vector<float>&    v  = src ? src->m_vertices : box;
        const std::vector<uint32_t>& ix = src ? src->m_indices  : boxIx;
        const uint32_t base = VertexCount();
        for (size_t i = 0; i + 5 < v.size(); i += 6)
        {
            float x = v[i] * 2.0f * hx, y = v[i + 1] * 2.0f * hy, z = v[i + 2] * 2.0f * hz;
            float nx = v[i + 3], ny = v[i + 4], nz = v[i + 5];
            turn(x, y, z); turn(nx, ny, nz);
            m_vertices.insert(m_vertices.end(), { (px + x) / m_unit[0], (py + y) / m_unit[1], (pz + z) / m_unit[2], nx, ny, nz });
        }
        for (uint32_t k : ix) m_indices.push_back(base + k);
        ++m_version;
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

    /*
     * BENT COPIES. Two kinds of push, summed at each vertex, said before a
     * copy and spent by it:
     *
     *   a WARP   Perlin's noise in each axis (ontology/Noise.h: a Warp
     *            with Perlin in its slot, as the state's), the field
     *            the mesh's points sit in once turned by (ts, tc) about +y
     *            and scaled by `scale` -- its frequency `freq` there, its
     *            push `amp` times the point's distance from (0, 0, 0):
     *            nothing at the middle, most at the ends, so all of a
     *            shape curves rather than a part of it
     *   a BEND   a smooth push by (dx, dy, dz) at (cx, cy, cz), fading to
     *            nothing `r` away -- (1 - d^2/r^2)^2 of it at a distance d --
     *            and held off the middle: within `hold` of (0, 0, 0) it is
     *            scaled down to nothing there
     *
     * BY PARTS, when the source says it is made of them (Parts): runs of
     * `n` vertices, the first two of each its poles. A part is not pushed a
     * vertex at a time -- that would knead it -- but bent whole: its curve
     * is the quadratic through three points on its axis, its two ends (at
     * `ends` of the way from its middle to its poles) and its middle, each
     * put through the pushes; and each vertex goes where its place along the
     * axis falls on the curve, its offset from the axis turned as the curve
     * turns there. So a puff stays a puff and follows the warp smoothly, and
     * a game that cuts and hurts along the same curve through the same three
     * points (KartBattle's fire) draws what it decided.
     *
     * CopyBent makes this mesh a copy of another put through them. A shape made once and shared stays as made; each bent copy of it
     * is its own, so one shape bent to many places costs a copy each, not a
     * remake (KartBattle's fire: a tree baked once a round, bent to the
     * walls round every burst). The normals are the source's: a bend is
     * gentle, and a glow's soft edge does not need them exact.
     */
    static constexpr size_t kMaxBends = 16;
    void Bend(float cx, float cy, float cz, float dx, float dy, float dz, float r, float hold)
    {
        if (m_bends.size() < kMaxBends && r > 0.0f) m_bends.push_back({ { cx, cy, cz }, { dx, dy, dz }, r, hold });
    }
    void Parts(uint32_t n, float ends) { m_part = n >= 2 ? n : 0; m_ends = ends > 0.0f ? ends : 1.0f; }
    void Warp(uint32_t seed, float freq, float amp, float scale, float ts, float tc)
    {
        m_warp = { { seed, freq, amp }, scale > 0.0f ? scale : 1.0f, ts, tc, true };
    }
    bool CopyBent(const Mesh& src)
    {
        std::vector<float> v = src.m_vertices;
        // The pushes at a point: the warp, and every bend.
        auto pushAt = [&](const float* p, float push[3]) {
            push[0] = push[1] = push[2] = 0.0f;
            const float out = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
            if (m_warp.on)
            {
                // Into the field's frame: scaled, and turned +z onto (ts, tc).
                const WarpBy& w = m_warp;
                const float x = p[0] * w.scale, y = p[1] * w.scale, z = p[2] * w.scale;
                const float fx = x * w.tc + z * w.ts, fz = z * w.tc - x * w.ts;
                float nx = 0.0f, ny = 0.0f, nz = 0.0f;
                w.field.Push(fx, y, fz, nx, ny, nz);
                // ...and back.
                push[0] += (nx * w.tc - nz * w.ts) / w.scale; push[1] += ny / w.scale; push[2] += (nz * w.tc + nx * w.ts) / w.scale;
            }
            for (const BendAt& b : m_bends)
            {
                const float ex = p[0] - b.c[0], ey = p[1] - b.c[1], ez = p[2] - b.c[2];
                const float q = (ex * ex + ey * ey + ez * ez) / (b.r * b.r);
                if (q >= 1.0f) continue;
                const float w = (1.0f - q) * (1.0f - q) * (b.hold > 0.0f ? std::min(1.0f, out / b.hold) : 1.0f);
                for (int k = 0; k < 3; ++k) push[k] += b.d[k] * w;
            }
        };
        const size_t count = v.size() / 6, part = src.m_part;
        size_t first = 0;
        if (part >= 2)
            for (; first + part <= count; first += part)
            {
                // The part's axis, pole to pole, and the curve's three points on it.
                const float* A = &v[first * 6];
                const float* B = &v[(first + 1) * 6];
                float m[3], c0[3], c1[3], d[3];
                for (int k = 0; k < 3; ++k)
                {
                    m[k] = (A[k] + B[k]) * 0.5f;
                    c0[k] = m[k] - (B[k] - A[k]) * 0.5f * src.m_ends; c1[k] = m[k] + (B[k] - A[k]) * 0.5f * src.m_ends;
                    d[k] = c1[k] - c0[k];
                }
                const float ll = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
                if (!(ll > 1e-12f)) break;   // no axis: pushed a vertex at a time, below
                const float dl = std::sqrt(ll), dn[3] = { d[0] / dl, d[1] / dl, d[2] / dl };
                float p0[3], pm[3], p1[3], push[3];
                pushAt(c0, push); for (int k = 0; k < 3; ++k) p0[k] = c0[k] + push[k];
                pushAt(m, push);  for (int k = 0; k < 3; ++k) pm[k] = m[k] + push[k];
                pushAt(c1, push); for (int k = 0; k < 3; ++k) p1[k] = c1[k] + push[k];
                // The originals, read before any of the part is written.
                const std::vector<float> was(v.begin() + static_cast<long>(first * 6), v.begin() + static_cast<long>((first + part) * 6));
                for (size_t j = 0; j < part; ++j)
                {
                    const float* o = &was[j * 6];
                    float* out = &v[(first + j) * 6];
                    float t = ((o[0] - c0[0]) * d[0] + (o[1] - c0[1]) * d[1] + (o[2] - c0[2]) * d[2]) / ll;
                    t = std::max(-0.25f, std::min(1.25f, t));
                    const float a = 2.0f * t * t - 3.0f * t + 1.0f, mm = 4.0f * t - 4.0f * t * t, e = 2.0f * t * t - t;
                    const float da = 4.0f * t - 3.0f, dm = 4.0f - 8.0f * t, de = 4.0f * t - 1.0f;
                    float q[3], tg[3], r[3];
                    for (int k = 0; k < 3; ++k)
                    {
                        q[k] = p0[k] * a + pm[k] * mm + p1[k] * e;
                        tg[k] = p0[k] * da + pm[k] * dm + p1[k] * de;
                        r[k] = o[k] - (c0[k] + d[k] * t);
                    }
                    // The turn taking the axis onto the curve's way there (the least turn: about their cross).
                    const float tl = std::sqrt(tg[0] * tg[0] + tg[1] * tg[1] + tg[2] * tg[2]);
                    float tn[3] = { dn[0], dn[1], dn[2] };
                    if (tl > 1e-9f) for (int k = 0; k < 3; ++k) tn[k] = tg[k] / tl;
                    const float kx = dn[1] * tn[2] - dn[2] * tn[1], ky = dn[2] * tn[0] - dn[0] * tn[2], kz = dn[0] * tn[1] - dn[1] * tn[0];
                    const float c = dn[0] * tn[0] + dn[1] * tn[1] + dn[2] * tn[2];
                    auto turn = [&](const float* x, float* y) {
                        if (c <= -0.999f) { y[0] = -x[0]; y[1] = -x[1]; y[2] = -x[2]; return; }
                        const float kd = (kx * x[0] + ky * x[1] + kz * x[2]) / (1.0f + c);
                        y[0] = x[0] * c + (ky * x[2] - kz * x[1]) + kx * kd;
                        y[1] = x[1] * c + (kz * x[0] - kx * x[2]) + ky * kd;
                        y[2] = x[2] * c + (kx * x[1] - ky * x[0]) + kz * kd;
                    };
                    float rr[3], nn[3];
                    turn(r, rr);
                    turn(o + 3, nn);
                    for (int k = 0; k < 3; ++k) { out[k] = q[k] + rr[k]; out[3 + k] = nn[k]; }
                }
            }
        // What is not in a part: pushed a vertex at a time.
        for (size_t i = first * 6; i + 5 < v.size(); i += 6)
        {
            float push[3];
            pushAt(&v[i], push);
            for (int k = 0; k < 3; ++k) v[i + k] += push[k];
        }
        m_vertices.swap(v);
        m_indices = src.m_indices;
        m_part = src.m_part; m_ends = src.m_ends;
        m_bends.clear();
        m_warp.on = false;
        ++m_version;
        return true;
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

    struct BendAt { float c[3], d[3], r, hold; };
    // The noise in the warp's slot is Perlin's: the one a game's state was
    // warped with (KartBattle's fire), so the copy is what was decided.
    struct WarpBy { ETCS::Noise::Warp<ETCS::Noise::Perlin, float> field; float scale = 1.0f, ts = 0.0f, tc = 1.0f; bool on = false; };
    std::vector<float>    m_vertices;
    std::vector<uint32_t> m_indices;
    std::vector<BendAt>   m_bends;      // said, not yet spent by a copy
    WarpBy                m_warp;       // ...and the warp, likewise
    size_t                m_part = 0;   // vertices a part (Parts), 0: none
    float                 m_ends = 1.0f;
    std::array<float, 3>  m_unit = { 1.0f, 1.0f, 1.0f };   // Unit: the box AddPart says parts in
    uint64_t              m_version = 0;
};

#endif // RENDERPROVIDER_MESH_H__
