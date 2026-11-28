#ifndef GEOMETRYPROVIDER_SEASHELL_H__
#define GEOMETRYPROVIDER_SEASHELL_H__

#include "../../../core_defs.h"
#include "../../../ontology.h"
#include <cmath>
#include <sstream>
#include <string>

/*
 * Seashell -- three-parameter tubular shell (Drawable3D reference geometry).
 *
 *   a      major (centerline) radius
 *   b      tube radius
 *   turns  revolutions
 *
 * Sample / Mesh produce point or descriptor data the rest of the runtime
 * can bind the way Kart binds burst / fire meshes.
 */

class Seashell : public Drawable3DBase<Seashell>, public DeletableBase<Seashell>
{
public:
    ETCS_TYPE_IDENTITY(Seashell);

    Seashell() = default;
    ~Seashell() = default;

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
        m_pose.on = true;
        m_pose.x  = static_cast<float>(x);
        m_pose.y  = static_cast<float>(y);
        m_pose.z  = static_cast<float>(z);
        m_pose.qw = static_cast<float>(qw);
        m_pose.qx = static_cast<float>(qx);
        m_pose.qy = static_cast<float>(qy);
        m_pose.qz = static_cast<float>(qz);
        etcs_mark_observed(this);
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
            const double cx = (m_a + m_b * std::cos(phi)) * std::cos(theta);
            const double cy = (m_a + m_b * std::cos(phi)) * std::sin(theta);
            const double cz =  m_b * std::sin(phi) + pitch * theta;
            o << ' ' << cx << ' ' << cy << ' ' << cz;
        }
        return o.str();
    }

    std::string Mesh() const
    {
        std::ostringstream o;
        o << "seashell " << m_a << ' ' << m_b << ' ' << m_turns;
        return o.str();
    }

    void Delete() { this->removeTag("active"); }

    Box3D Bounds3D() const
    {
        const float r = static_cast<float>(m_a + m_b);
        const float h = static_cast<float>(m_b * 2.0 + m_b * 0.35 * 2.0 * M_PI * m_turns);
        Box3D b;
        b.min = {-r, -r, 0.f};
        b.max = { r,  r, h};
        return b;
    }

private:
    double m_a = 1.0, m_b = 0.25, m_turns = 3.0;
    ShownPose m_pose;
};

#endif // GEOMETRYPROVIDER_SEASHELL_H__
