#ifndef GEOMETRYPROVIDER_GRAINSET_H__
#define GEOMETRYPROVIDER_GRAINSET_H__

#include "../../../core_defs.h"
#include "../../../ontology.h"
#include "Grain.h"
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

/*
 * GrainSet -- collection of Grains that participates in the Causal
 * observation edge.
 *
 * Performance model (matches existing ETCS hash discipline):
 *
 *   - Grains claim Causal. Their positions + material values contribute
 *     to the subtree identity / state hash (XXH3 under the type root).
 *   - Observe() is the explicit trigger on the observation edge. It
 *     lazy-resolves only the dirty region (hash mismatch or first touch).
 *   - A stable pile's resolve result is memoized under that hash; later
 *     ticks that see the same hash skip the cascade.
 *   - A perturbation (impulse, new grain, material change) dirties the
 *     path; the next Observe re-resolves only what changed.
 *
 * Angle of repose is read from each Grain (or the set default) during
 * resolve. No new ontology trait is required.
 */

class GrainSet : public CausalBase<GrainSet>, public DeletableBase<GrainSet>
{
public:
    ETCS_TYPE_IDENTITY(GrainSet);

    GrainSet() = default;
    ~GrainSet() = default;

    bool Create()
    {
        m_grains.clear();
        m_spawns.clear();
        m_memo_hash = 0;
        m_resolved  = false;
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

    // Records a spawn descriptor for Sample / visual use.
    // Full Causal child spawn is expected from the .etcs side.
    std::string SpawnGrain(double x, double y, double z, double r)
    {
        if (r <= 0.0) return "fail";
        SpawnRec rec{x, y, z, r, m_default_repose};
        m_spawns.push_back(rec);
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
        this->addTag("repose", deg);
        dirty();
    }

    // Observation-edge entry point. Lazy-resolves if dirty or never
    // resolved; otherwise returns the memoized status.
    // In a full implementation this walks grains, applies repose /
    // support tests, and writes stable positions back through the
    // value funnel so the subtree hash updates only on real change.
    std::string Observe()
    {
        if (m_resolved && !m_dirty)
        {
            std::ostringstream o;
            o << "memo " << m_memo_hash;
            return o.str();
        }

        // Placeholder resolve: mark clean and stamp a simple memo hash
        // from spawn count + default repose. Real code would hash the
        // Causal rows / grain state the same way Entity::getHash does.
        uint64_t h = 14695981039346656037ull; // FNV-ish seed
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

    void Delete()
    {
        m_grains.clear();
        m_spawns.clear();
        m_resolved = false;
        m_dirty    = true;
        this->removeTag("active");
    }

private:
    void dirty() { m_dirty = true; m_resolved = false; }

    struct SpawnRec { double x, y, z, r, repose; };
    std::vector<ETCS::RID> m_grains;
    std::vector<SpawnRec>  m_spawns;
    double   m_default_repose = 30.0;
    uint64_t m_memo_hash      = 0;
    bool     m_resolved       = false;
    bool     m_dirty          = true;
};

#endif // GEOMETRYPROVIDER_GRAINSET_H__
