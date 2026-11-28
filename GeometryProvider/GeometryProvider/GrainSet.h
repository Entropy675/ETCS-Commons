#ifndef GEOMETRYPROVIDER_GRAINSET_H__
#define GEOMETRYPROVIDER_GRAINSET_H__

#include "../../../core_defs.h"
#include "../../../ontology.h"
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

/*
 * GrainSet -- collection of grain spawn descriptors + lazy Observe memo.
 * Deletable only. Observe stamps a simple memo hash; a later full Causal
 * wiring can replace the placeholder resolve body without changing the
 * work-function surface.
 */

class GrainSet : public DeletableBase<GrainSet>
{
public:
    WIRE_TYPE_IDENTITY(GrainSet);

    GrainSet() = default;

    bool DeleteConcrete() override
    {
        m_grains.clear();
        m_spawns.clear();
        m_resolved = false;
        m_dirty = true;
        return true;
    }

    bool Create()
    {
        m_grains.clear();
        m_spawns.clear();
        m_memo_hash = 0;
        m_resolved = false;
        m_dirty = true;
        this->addTag("active");
        return true;
    }

    bool AddGrain(ETCS::RID g)
    {
        if (!g) return false;
        m_grains.push_back(g);
        dirty();
        return true;
    }

    std::string SpawnGrain(double x, double y, double z, double r)
    {
        if (r <= 0.0) return "fail";
        m_spawns.push_back(SpawnRec{x, y, z, r, m_default_repose});
        dirty();
        std::ostringstream o;
        o << "spawned " << m_spawns.size();
        return o.str();
    }

    void SetRepose(double deg)
    {
        if (deg < 0.0)  deg = 0.0;
        if (deg > 90.0) deg = 90.0;
        m_default_repose = deg;
        this->addTag(ETCS::Buffer("repose"), std::to_string(deg));
        dirty();
    }

    std::string Observe()
    {
        if (m_resolved && !m_dirty)
        {
            std::ostringstream o;
            o << "memo " << m_memo_hash;
            return o.str();
        }
        uint64_t h = 14695981039346656037ull;
        h ^= static_cast<uint64_t>(m_spawns.size());
        h *= 1099511628211ull;
        h ^= static_cast<uint64_t>(m_default_repose * 1000.0);
        h *= 1099511628211ull;
        for (const auto& s : m_spawns)
        {
            h ^= static_cast<uint64_t>(s.x * 1000) * 0x9e3779b97f4a7c15ull;
            h ^= static_cast<uint64_t>(s.y * 1000) * 0x9e3779b97f4a7c15ull;
            h ^= static_cast<uint64_t>(s.z * 1000) * 0x9e3779b97f4a7c15ull;
        }
        m_memo_hash = h;
        m_resolved  = true;
        m_dirty     = false;
        std::ostringstream o;
        o << "resolved " << m_spawns.size() << " hash " << m_memo_hash;
        return o.str();
    }

    std::string Sample() const
    {
        std::ostringstream o;
        o << m_spawns.size();
        for (const auto& s : m_spawns)
            o << ' ' << s.x << ' ' << s.y << ' ' << s.z << ' ' << s.r;
        return o.str();
    }

private:
    void dirty() { m_dirty = true; m_resolved = false; }

    struct SpawnRec { double x, y, z, r, repose; };
    std::vector<ETCS::RID> m_grains;
    std::vector<SpawnRec>  m_spawns;
    double   m_default_repose = 30.0;
    uint64_t m_memo_hash = 0;
    bool     m_resolved = false;
    bool     m_dirty = true;
};

#endif // GEOMETRYPROVIDER_GRAINSET_H__
