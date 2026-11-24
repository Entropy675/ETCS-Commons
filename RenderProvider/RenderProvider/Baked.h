#ifndef RENDERPROVIDER_BAKED_H__
#define RENDERPROVIDER_BAKED_H__

#include "DeviceFrame.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

/*
 * A STILL CONTAINER'S MERGED SOLIDS (Scene3D, a still container's picture):
 * the opaque nodes under it as one mesh in its own frame, seven floats a
 * vertex, each node's colour as its vertices' tint (DeviceFrame.h). Not an
 * entity: a picture's cache, made and dropped by the container it belongs to,
 * and kept here by that container's RID, which is where the window surface
 * sends it from (HostSurface::sendMeshes) -- as it sends a Mesh, when the
 * device lacks it or the version moved. One registry a module: this header
 * is RenderProvider's own.
 */
struct BakedMesh
{
    uint64_t              version = 0;
    std::vector<float>    vertices;   // x y z nx ny nz tint
    std::vector<uint32_t> indices;
};

class BakedMeshes
{
public:
    static void Put(ETCS::RID rid, std::shared_ptr<const BakedMesh> mesh)
    {
        std::lock_guard<std::mutex> g(mu());
        map()[rid] = std::move(mesh);
    }
    static std::shared_ptr<const BakedMesh> Get(ETCS::RID rid)
    {
        std::lock_guard<std::mutex> g(mu());
        auto it = map().find(rid);
        return it == map().end() ? nullptr : it->second;
    }
    static void Drop(ETCS::RID rid)
    {
        std::lock_guard<std::mutex> g(mu());
        map().erase(rid);
    }
    // Versions from one counter, so a container baked, dropped and baked
    // again never repeats one a device still holds.
    static uint64_t NextVersion() { static std::atomic<uint64_t> v{ 0 }; return ++v; }

private:
    static std::mutex& mu() { static std::mutex m; return m; }
    static std::unordered_map<ETCS::RID, std::shared_ptr<const BakedMesh>>& map()
    {
        static std::unordered_map<ETCS::RID, std::shared_ptr<const BakedMesh>> m;
        return m;
    }
};

#endif // RENDERPROVIDER_BAKED_H__
