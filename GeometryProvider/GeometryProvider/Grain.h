#ifndef GEOMETRYPROVIDER_GRAIN_H__
#define GEOMETRYPROVIDER_GRAIN_H__

#include "../../../core_defs.h"
#include "../../../ontology.h"
#include <sstream>
#include <string>

/*
 * Grain -- sphere-like grain with material coefficients on the value surface.
 * Deletable only for now (Causal participation can be added later with a full
 * AdvanceConcrete / Interact surface). Angle of repose is a string-valued tag
 * so resume/hash see it.
 */

class Grain : public DeletableBase<Grain>
{
public:
    WIRE_TYPE_IDENTITY(Grain);

    Grain() = default;

    bool DeleteConcrete() override { return true; }

    bool Create(double radius)
    {
        if (radius <= 0.0) return false;
        m_radius = radius;
        this->addTag("active");
        this->addTag(ETCS::Buffer("radius"), std::to_string(radius));
        return true;
    }

    bool SetMaterial(double repose_deg, double friction)
    {
        if (repose_deg < 0.0 || repose_deg > 90.0) return false;
        if (friction < 0.0) return false;
        m_repose_deg = repose_deg;
        m_friction   = friction;
        this->addTag(ETCS::Buffer("repose"),   std::to_string(repose_deg));
        this->addTag(ETCS::Buffer("friction"), std::to_string(friction));
        return true;
    }

    double Radius()        const { return m_radius; }
    double ReposeDeg()     const { return m_repose_deg; }
    double Friction()      const { return m_friction; }
    double AngleOfRepose() const { return m_repose_deg; }

private:
    double m_radius     = 0.05;
    double m_repose_deg = 30.0;
    double m_friction   = 0.5;
};

#endif // GEOMETRYPROVIDER_GRAIN_H__
