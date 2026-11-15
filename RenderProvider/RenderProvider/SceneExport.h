#ifndef RENDERPROVIDER_SCENEEXPORT_H__
#define RENDERPROVIDER_SCENEEXPORT_H__

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

/*
 * A SCENE, WRITTEN OUT. What the device is sent to draw a scene -- a mesh, a
 * model matrix and a colour a node (DeviceFrame.h, DeviceMeshOp) -- is what a
 * file of one holds, so an export is that same list handed to a writer:
 *
 *   .obj   Wavefront, with a .mtl beside it: a colour is a material
 *   .stl   binary: triangles and nothing else, for a printer's slicer
 *   .glb   glTF 2.0 in one file: a node per part, a shape stored once however
 *          many parts are drawn with it
 *
 * HANDEDNESS. A scene is x right, y up, z AWAY from the eye (Scene3D's basis:
 * right = up x forward); all three formats are read z TOWARD it. So z is
 * negated on the way out and each triangle's winding swapped with it, which
 * keeps the picture as it was and the faces outward.
 *
 * Nothing of ETCS's is in this file: plain structs in, bytes out. Floats are
 * written as the host holds them, little-endian on every substrate this
 * builds for.
 */
struct SceneExportPart
{
    uint64_t geometry = 0;                        // parts with the same one share a shape
    float    model[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };   // column-major T*R*S
    float    color[4]  = { 1.0f, 1.0f, 1.0f, 1.0f };
    const std::vector<float>*    vertices = nullptr;   // x y z nx ny nz, in the shape's unit space
    const std::vector<uint32_t>* indices  = nullptr;   // three a triangle
};

namespace scene_export_detail
{
inline bool usable(const SceneExportPart& p)
{
    return p.vertices && p.indices && p.vertices->size() >= 18 && p.indices->size() >= 3;
}
inline void normalise(float n[3])
{
    const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (l <= 0.0f) { n[0] = 0.0f; n[1] = 1.0f; n[2] = 0.0f; return; }
    n[0] /= l; n[1] /= l; n[2] /= l;
}
// A part's vertices where the scene has them, mirrored for the file: positions
// by the model matrix, normals by its inverse transpose (a part stretched on
// one axis leans its normals the other way).
inline void placed(const SceneExportPart& p, std::vector<float>& pos, std::vector<float>& nrm)
{
    const float* m = p.model;
    const float a[3][3] = { { m[0], m[4], m[8] }, { m[1], m[5], m[9] }, { m[2], m[6], m[10] } };
    const float c[3][3] = {
        { a[1][1] * a[2][2] - a[1][2] * a[2][1], a[1][2] * a[2][0] - a[1][0] * a[2][2], a[1][0] * a[2][1] - a[1][1] * a[2][0] },
        { a[0][2] * a[2][1] - a[0][1] * a[2][2], a[0][0] * a[2][2] - a[0][2] * a[2][0], a[0][1] * a[2][0] - a[0][0] * a[2][1] },
        { a[0][1] * a[1][2] - a[0][2] * a[1][1], a[0][2] * a[1][0] - a[0][0] * a[1][2], a[0][0] * a[1][1] - a[0][1] * a[1][0] } };
    const size_t n = p.vertices->size() / 6;
    pos.resize(n * 3); nrm.resize(n * 3);
    for (size_t i = 0; i < n; ++i)
    {
        const float* v = &(*p.vertices)[i * 6];
        pos[i * 3 + 0] =   a[0][0] * v[0] + a[0][1] * v[1] + a[0][2] * v[2] + m[12];
        pos[i * 3 + 1] =   a[1][0] * v[0] + a[1][1] * v[1] + a[1][2] * v[2] + m[13];
        pos[i * 3 + 2] = -(a[2][0] * v[0] + a[2][1] * v[1] + a[2][2] * v[2] + m[14]);
        float q[3] = {   c[0][0] * v[3] + c[0][1] * v[4] + c[0][2] * v[5],
                         c[1][0] * v[3] + c[1][1] * v[4] + c[1][2] * v[5],
                       -(c[2][0] * v[3] + c[2][1] * v[4] + c[2][2] * v[5]) };
        normalise(q);
        nrm[i * 3 + 0] = q[0]; nrm[i * 3 + 1] = q[1]; nrm[i * 3 + 2] = q[2];
    }
}
inline uint32_t colourKey(const float c[4])
{
    uint32_t k = 0;
    for (int i = 0; i < 4; ++i)
    {
        const float f = c[i] < 0.0f ? 0.0f : (c[i] > 1.0f ? 1.0f : c[i]);
        k = (k << 8) | static_cast<uint32_t>(f * 255.0f + 0.5f);
    }
    return k;
}
// The triangles of a part whose indices name vertices it has.
inline size_t triangles(const SceneExportPart& p)
{
    const size_t n = p.vertices->size() / 6;
    size_t t = 0;
    for (size_t i = 0; i + 2 < p.indices->size(); i += 3)
        if ((*p.indices)[i] < n && (*p.indices)[i + 1] < n && (*p.indices)[i + 2] < n) ++t;
    return t;
}
template <typename T> inline void put(std::string& out, T v)
{
    char b[sizeof(T)];
    std::memcpy(b, &v, sizeof(T));
    out.append(b, sizeof(T));
}
inline std::string num(double v)
{
    char b[40];
    std::snprintf(b, sizeof b, "%.9g", v);
    return b;
}
}   // namespace scene_export_detail

inline bool scene_export_obj(const std::string& path, const std::vector<SceneExportPart>& parts, uint64_t& tris, std::string& why)
{
    using namespace scene_export_detail;
    const size_t slash = path.find_last_of("/\\");
    const size_t dot = path.find_last_of('.');
    const std::string mtl_path = path.substr(0, dot) + ".mtl";
    const std::string mtl_name = mtl_path.substr(slash == std::string::npos ? 0 : slash + 1);
    std::ofstream obj(path, std::ios::binary | std::ios::trunc), mtl(mtl_path, std::ios::binary | std::ios::trunc);
    if (!obj || !mtl) { why = "cannot open " + path + " (and its .mtl)"; return false; }

    std::map<uint32_t, size_t> materials;
    std::string body;
    size_t base = 1, part = 0;
    tris = 0;
    std::vector<float> pos, nrm;
    for (const SceneExportPart& p : parts)
    {
        if (!usable(p)) continue;
        const uint32_t key = colourKey(p.color);
        auto it = materials.find(key);
        if (it == materials.end())
        {
            it = materials.emplace(key, materials.size()).first;
            mtl << "newmtl m" << it->second << "\nKd " << num(p.color[0]) << ' ' << num(p.color[1]) << ' ' << num(p.color[2])
                << "\nd " << num(p.color[3]) << "\n\n";
        }
        placed(p, pos, nrm);
        const size_t n = pos.size() / 3;
        body += "o part" + std::to_string(part++) + "\n";
        for (size_t i = 0; i < n; ++i)
            body += "v " + num(pos[i * 3]) + ' ' + num(pos[i * 3 + 1]) + ' ' + num(pos[i * 3 + 2]) + "\n";
        for (size_t i = 0; i < n; ++i)
            body += "vn " + num(nrm[i * 3]) + ' ' + num(nrm[i * 3 + 1]) + ' ' + num(nrm[i * 3 + 2]) + "\n";
        body += "usemtl m" + std::to_string(it->second) + "\n";
        for (size_t i = 0; i + 2 < p.indices->size(); i += 3)
        {
            const size_t a = (*p.indices)[i], b = (*p.indices)[i + 1], c = (*p.indices)[i + 2];
            if (a >= n || b >= n || c >= n) continue;
            const std::string sa = std::to_string(base + a), sb = std::to_string(base + b), sc = std::to_string(base + c);
            body += "f " + sa + "//" + sa + ' ' + sc + "//" + sc + ' ' + sb + "//" + sb + "\n";   // a c b: the mirror's winding
            ++tris;
        }
        base += n;
    }
    if (tris == 0) { why = "nothing drawn under this node"; return false; }
    obj << "# ETCS Scene3D export: " << part << " part(s), " << tris << " triangle(s)\nmtllib " << mtl_name << "\n" << body;
    if (!obj || !mtl) { why = "write failed"; return false; }
    return true;
}

inline bool scene_export_stl(const std::string& path, const std::vector<SceneExportPart>& parts, uint64_t& tris, std::string& why)
{
    using namespace scene_export_detail;
    std::string body;
    tris = 0;
    std::vector<float> pos, nrm;
    for (const SceneExportPart& p : parts)
    {
        if (!usable(p)) continue;
        placed(p, pos, nrm);
        const size_t n = pos.size() / 3;
        for (size_t i = 0; i + 2 < p.indices->size(); i += 3)
        {
            const size_t ia = (*p.indices)[i], ic = (*p.indices)[i + 1], ib = (*p.indices)[i + 2];   // swapped: the mirror's winding
            if (ia >= n || ib >= n || ic >= n) continue;
            const float* a = &pos[ia * 3]; const float* b = &pos[ib * 3]; const float* c = &pos[ic * 3];
            const float u[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, v[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
            float f[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
            normalise(f);
            for (float x : f) put(body, x);
            for (int k = 0; k < 3; ++k) put(body, a[k]);
            for (int k = 0; k < 3; ++k) put(body, b[k]);
            for (int k = 0; k < 3; ++k) put(body, c[k]);
            put(body, static_cast<uint16_t>(0));
            ++tris;
        }
    }
    if (tris == 0) { why = "nothing drawn under this node"; return false; }
    if (tris > 0xFFFFFFFFull) { why = "too many triangles for an STL"; return false; }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) { why = "cannot open " + path; return false; }
    char header[80] = {};
    std::snprintf(header, sizeof header, "ETCS Scene3D export");
    out.write(header, sizeof header);
    std::string count;
    put(count, static_cast<uint32_t>(tris));
    out << count << body;
    if (!out) { why = "write failed"; return false; }
    return true;
}

inline bool scene_export_glb(const std::string& path, const std::vector<SceneExportPart>& parts, uint64_t& tris, std::string& why)
{
    using namespace scene_export_detail;
    struct Shape { size_t accessor = 0; };
    std::map<uint64_t, Shape> shapes;
    std::map<uint32_t, size_t> materials;
    std::map<std::pair<uint64_t, uint32_t>, size_t> meshes;
    std::string bin, views, accessors, mats, mesh_json, nodes, roots;
    size_t n_views = 0, n_accessors = 0, n_nodes = 0;
    tris = 0;

    auto view = [&](size_t offset, size_t length, int target) {
        if (n_views++) views += ',';
        views += "{\"buffer\":0,\"byteOffset\":" + std::to_string(offset) + ",\"byteLength\":" + std::to_string(length)
               + ",\"target\":" + std::to_string(target) + "}";
        return n_views - 1;
    };
    for (const SceneExportPart& p : parts)
    {
        if (!usable(p)) continue;
        const size_t t = triangles(p);
        if (t == 0) continue;

        // The shape, once: positions, normals and indices in its unit space.
        auto sh = shapes.find(p.geometry);
        if (sh == shapes.end())
        {
            const size_t n = p.vertices->size() / 6;
            float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
            const size_t at_pos = bin.size();
            for (size_t i = 0; i < n; ++i)
            {
                const float* v = &(*p.vertices)[i * 6];
                const float q[3] = { v[0], v[1], -v[2] };
                for (int k = 0; k < 3; ++k) { put(bin, q[k]); lo[k] = std::min(lo[k], q[k]); hi[k] = std::max(hi[k], q[k]); }
            }
            const size_t at_nrm = bin.size();
            for (size_t i = 0; i < n; ++i)
            {
                const float* v = &(*p.vertices)[i * 6];
                float q[3] = { v[3], v[4], -v[5] };
                normalise(q);
                for (float x : q) put(bin, x);
            }
            const size_t at_idx = bin.size();
            for (size_t i = 0; i + 2 < p.indices->size(); i += 3)
            {
                const uint32_t a = (*p.indices)[i], b = (*p.indices)[i + 1], c = (*p.indices)[i + 2];
                if (a >= n || b >= n || c >= n) continue;
                put(bin, a); put(bin, c); put(bin, b);   // a c b: the mirror's winding
            }
            const size_t v_pos = view(at_pos, at_nrm - at_pos, 34962);
            const size_t v_nrm = view(at_nrm, at_idx - at_nrm, 34962);
            const size_t v_idx = view(at_idx, bin.size() - at_idx, 34963);
            if (n_accessors) accessors += ',';
            accessors += "{\"bufferView\":" + std::to_string(v_pos) + ",\"componentType\":5126,\"count\":" + std::to_string(n)
                       + ",\"type\":\"VEC3\",\"min\":[" + num(lo[0]) + ',' + num(lo[1]) + ',' + num(lo[2])
                       + "],\"max\":[" + num(hi[0]) + ',' + num(hi[1]) + ',' + num(hi[2]) + "]},"
                       + "{\"bufferView\":" + std::to_string(v_nrm) + ",\"componentType\":5126,\"count\":" + std::to_string(n) + ",\"type\":\"VEC3\"},"
                       + "{\"bufferView\":" + std::to_string(v_idx) + ",\"componentType\":5125,\"count\":" + std::to_string(t * 3) + ",\"type\":\"SCALAR\"}";
            sh = shapes.emplace(p.geometry, Shape{ n_accessors }).first;
            n_accessors += 3;
        }

        const uint32_t key = colourKey(p.color);
        auto mt = materials.find(key);
        if (mt == materials.end())
        {
            mt = materials.emplace(key, materials.size()).first;
            if (mt->second) mats += ',';
            mats += "{\"pbrMetallicRoughness\":{\"baseColorFactor\":[" + num(p.color[0]) + ',' + num(p.color[1]) + ',' + num(p.color[2]) + ',' + num(p.color[3])
                  + "],\"metallicFactor\":0,\"roughnessFactor\":0.9}" + (p.color[3] < 1.0f ? ",\"alphaMode\":\"BLEND\"" : "") + "}";
        }
        auto ms = meshes.find({ p.geometry, key });
        if (ms == meshes.end())
        {
            ms = meshes.emplace(std::make_pair(p.geometry, key), meshes.size()).first;
            if (ms->second) mesh_json += ',';
            mesh_json += "{\"primitives\":[{\"attributes\":{\"POSITION\":" + std::to_string(sh->second.accessor)
                       + ",\"NORMAL\":" + std::to_string(sh->second.accessor + 1) + "},\"indices\":" + std::to_string(sh->second.accessor + 2)
                       + ",\"material\":" + std::to_string(mt->second) + "}]}";
        }

        // The node: the model matrix, mirrored on both sides (M A M, M = diag(1, 1, -1)).
        if (n_nodes) { nodes += ','; roots += ','; }
        nodes += "{\"name\":\"part" + std::to_string(n_nodes) + "\",\"mesh\":" + std::to_string(ms->second) + ",\"matrix\":[";
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
            {
                const float v = p.model[c * 4 + r] * (((r == 2) != (c == 2)) ? -1.0f : 1.0f);
                if (c || r) nodes += ',';
                nodes += num(v == 0.0f ? 0.0f : v);
            }
        nodes += "]}";
        roots += std::to_string(n_nodes++);
        tris += t;
    }
    if (tris == 0) { why = "nothing drawn under this node"; return false; }

    while (bin.size() % 4) bin.push_back('\0');
    std::string json = "{\"asset\":{\"version\":\"2.0\",\"generator\":\"ETCS RenderProvider Scene3D\"},\"scene\":0,\"scenes\":[{\"nodes\":[" + roots + "]}],"
                       "\"nodes\":[" + nodes + "],\"meshes\":[" + mesh_json + "],\"materials\":[" + mats + "],"
                       "\"accessors\":[" + accessors + "],\"bufferViews\":[" + views + "],\"buffers\":[{\"byteLength\":" + std::to_string(bin.size()) + "}]}";
    while (json.size() % 4) json.push_back(' ');
    const uint64_t total = 12 + 8 + json.size() + 8 + bin.size();
    if (total > 0xFFFFFFFFull) { why = "too large for a GLB"; return false; }

    std::string head;
    put(head, static_cast<uint32_t>(0x46546C67)); put(head, static_cast<uint32_t>(2)); put(head, static_cast<uint32_t>(total));
    put(head, static_cast<uint32_t>(json.size())); put(head, static_cast<uint32_t>(0x4E4F534A));
    std::string mid;
    put(mid, static_cast<uint32_t>(bin.size())); put(mid, static_cast<uint32_t>(0x004E4942));
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) { why = "cannot open " + path; return false; }
    out << head << json << mid << bin;
    if (!out) { why = "write failed"; return false; }
    return true;
}

// By the path's ending. False, and why, for an ending that names no format
// here, a file that cannot be written, or a scene with nothing to write.
inline bool scene_export(const std::string& path, const std::vector<SceneExportPart>& parts, uint64_t& tris, std::string& why)
{
    const size_t dot = path.find_last_of('.');
    std::string ext = dot == std::string::npos ? std::string() : path.substr(dot + 1);
    for (char& c : ext) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (ext == "obj") return scene_export_obj(path, parts, tris, why);
    if (ext == "stl") return scene_export_stl(path, parts, tris, why);
    if (ext == "glb") return scene_export_glb(path, parts, tris, why);
    why = "'" + ext + "' is not a format this writes (obj, stl, glb)";
    return false;
}

#endif // RENDERPROVIDER_SCENEEXPORT_H__
