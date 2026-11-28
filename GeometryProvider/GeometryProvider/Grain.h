#ifndef GEOMETRYPROVIDER_GRAIN_H__
#define GEOMETRYPROVIDER_GRAIN_H__

#include "../../../core_defs.h"
#include "../../../ontology.h"

/*
 * Grain -- single sphere-like grain that claims Causal.
 *
 * Angle of repose and friction are material values on the tag surface.
 * The Causal trait surface (Interact, Absorb, SupportUnder, ...) is what
 * the constraints are implemented over; this type only supplies the
 * coefficients those constraints consult.
 *
 * Cascades are not driven from here. GrainSet (or any Causal parent)
 * triggers resolution on the observation edge; stable grains stay
 * memoized under the subtree hash ETCS already maintains.
 */

class Grain : public CausalBase<Grain>, public DeletableBase<Grain>
{
public:
    ETCS_TYPE_IDENTITY(Grain);

    Grain() = default;
    ~Grain() = default;

    bool Create(double radius)
    {
        if (radius <= 0.0) return false;
        m_radius = radius;
        this->addTag("active");
        this->addTag("radius", radius);
        return true;
    }

    bool SetMaterial(double repose_deg, double friction)
    {
        if (repose_deg < 0.0 || repose_deg > 90.0) return false;
        if (friction < 0.0) return false;
        m_repose_deg = repose_deg;
        m_friction   = friction;
        // Value surface so resume / replay / hash see the same numbers.
        this->addTag("repose",   repose_deg);
        this->addTag("friction", friction);
        return true;
    }

    double Radius()    const { return m_radius; }
    double ReposeDeg() const { return m_repose_deg; }
    double Friction()  const { return m_friction; }
    double AngleOfRepose() const { return m_repose_deg; }

    void Delete() { this->removeTag("active"); }

private:
    double m_radius     = 0.05;
    double m_repose_deg = 30.0;
    double m_friction   = 0.5;
};

#endif // GEOMETRYPROVIDER_GRAIN_H__
