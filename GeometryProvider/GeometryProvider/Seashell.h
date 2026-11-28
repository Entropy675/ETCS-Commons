#ifndef GEOMETRYPROVIDER_SEASHELL_H__
#define GEOMETRYPROVIDER_SEASHELL_H__

#include "../../../core_defs.h"
#include "../../../ontology.h"
#include <cmath>
#include <sstream>
#include <string>

/*
 * Seashell -- three-parameter tubular shell (reference geometry).
 * Same posture as RenderProvider::Mesh: Deletable + WIRE_TYPE_IDENTITY only.
 * Sample / Mesh / SetPose are the surface other modules bind.
 */

class Seashell : public DeletableBase<Seashell>
{
public:
    WIRE_TYPE_IDENTITY(Seashell);

    Seashell() = default;

    bool DeleteConcrete() override { return true; }

    bool Create(double a, double b, double turns)
    {
        if (a <= 0.0 || b <= 0.0 || turns <= 0.0) return false;
        m_a = a; m_b = b; m_turns = turns;
        this->addTag("active");
        return true;
    }

    bool SetParams(double a, double b, double turns)
    {
        if (a <= 0.0 || b <= 0.0 || turns <= 0.0) return false;
        m_a = a; m_b = b; m_turns = turns;
        return true;
    }

    void SetPose(double x, double y, double z,
                 double qw, double qx, double qy, double qz)
    {
        m_x = x; m_y = y; m_z = z;
        m_qw = qw; m_qx = qx; m_qy = qy; m_qz = qz;
    }

    std::string Sample(int n) const
    {
        if (n < 4) n = 4;
        std::ostringstream o;
        o << n;
        const double pitch = m_b * 0.35;
        for (int i = 0; i < n; ++i)
        {
            const double t     = static_cast<double>(i) / static_cast<double>(n);
            const double theta = t * 2.0 * M_PI * m_turns;
            const double phi   = t * 2.0 * M_PI * 3.0;
            const double cx = m_x + (m_a + m_b * std::cos(phi)) * std::cos(theta);
            const double cy = m_y + (m_a + m_b * std::cos(phi)) * std::sin(theta);
            const double cz = m_z +  m_b * std::sin(phi) + pitch * theta;
            o << ' ' << cx << ' ' << cy << ' ' << cz;
        }
        return o.str();
    }

    std::string Mesh() const
    {
        std::ostringstream o;
        o << "seashell " << m_a << ' ' << m_b << ' ' << m_turns
          << ' ' << m_x << ' ' << m_y << ' ' << m_z;
        return o.str();
    }

private:
    double m_a = 1.0, m_b = 0.25, m_turns = 3.0;
    double m_x = 0, m_y = 0, m_z = 0;
    double m_qw = 1, m_qx = 0, m_qy = 0, m_qz = 0;
};

#endif // GEOMETRYPROVIDER_SEASHELL_H__
