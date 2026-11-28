#ifndef GEOMETRYPROVIDER_PYRAMID_H__
#define GEOMETRYPROVIDER_PYRAMID_H__

#include "../../../core_defs.h"
#include "../../../ontology.h"
#include <cmath>
#include <sstream>
#include <string>

/*
 * Pyramid -- regular n-sided pyramid with optional fractal depth.
 * Deletable + WIRE_TYPE_IDENTITY. SetPose for placement; Sample/Mesh/Path*
 * for consumers.
 */

class Pyramid : public DeletableBase<Pyramid>
{
public:
    WIRE_TYPE_IDENTITY(Pyramid);

    Pyramid() = default;

    bool DeleteConcrete() override { return true; }

    bool Create(double base, double height, int sides)
    {
        if (base <= 0.0 || height <= 0.0 || sides < 3) return false;
        m_base = base; m_height = height; m_sides = sides;
        m_depth = 0; m_scale = 0.5;
        this->addTag("active");
        return true;
    }

    void SetFractal(int depth, double scale)
    {
        if (depth < 0) depth = 0;
        if (scale <= 0.0 || scale >= 1.0) scale = 0.5;
        m_depth = depth;
        m_scale = scale;
        this->addTag(ETCS::Buffer("fractal_depth"), std::to_string(depth));
        this->addTag(ETCS::Buffer("fractal_scale"), std::to_string(scale));
    }

    void SetPose(double x, double y, double z,
                 double qw, double qx, double qy, double qz)
    {
        m_x = x; m_y = y; m_z = z;
        m_qw = qw; m_qx = qx; m_qy = qy; m_qz = qz;
    }

    std::string PathOnFace(int face, double t) const
    {
        if (t < 0.0) t = 0.0;
        if (t > 1.0) t = 1.0;
        std::ostringstream o;
        if (face <= 0)
        {
            const double a0 = t * 2.0 * M_PI;
            o << m_x + (m_base * 0.5) * std::cos(a0) << ' ' << m_y << ' '
              << m_z + (m_base * 0.5) * std::sin(a0);
        }
        else
        {
            const int i = (face - 1) % m_sides;
            const double a  = (2.0 * M_PI * i) / m_sides;
            const double bx = (m_base * 0.5) * std::cos(a);
            const double bz = (m_base * 0.5) * std::sin(a);
            o << m_x + bx * (1.0 - t) << ' '
              << m_y + m_height * t << ' '
              << m_z + bz * (1.0 - t);
        }
        return o.str();
    }

    std::string PathEdges() const
    {
        std::ostringstream o;
        for (int i = 0; i <= m_sides; ++i)
        {
            const double a = (2.0 * M_PI * (i % m_sides)) / m_sides;
            o << m_x + (m_base * 0.5) * std::cos(a) << ' ' << m_y << ' '
              << m_z + (m_base * 0.5) * std::sin(a) << ' ';
        }
        for (int i = 0; i < m_sides; ++i)
        {
            const double a = (2.0 * M_PI * i) / m_sides;
            o << m_x + (m_base * 0.5) * std::cos(a) << ' ' << m_y << ' '
              << m_z + (m_base * 0.5) * std::sin(a) << ' '
              << m_x << ' ' << (m_y + m_height) << ' ' << m_z << ' ';
        }
        return o.str();
    }

    std::string Sample(int n) const
    {
        if (n < m_sides) n = m_sides;
        std::ostringstream o;
        o << (n + 1);
        for (int i = 0; i < n; ++i)
        {
            const double a = (2.0 * M_PI * i) / n;
            o << ' ' << m_x + (m_base * 0.5) * std::cos(a)
              << ' ' << m_y
              << ' ' << m_z + (m_base * 0.5) * std::sin(a);
        }
        o << ' ' << m_x << ' ' << (m_y + m_height) << ' ' << m_z;
        return o.str();
    }

    std::string Mesh() const
    {
        std::ostringstream o;
        o << "pyramid " << m_base << ' ' << m_height << ' ' << m_sides
          << ' ' << m_depth << ' ' << m_scale
          << ' ' << m_x << ' ' << m_y << ' ' << m_z;
        return o.str();
    }

private:
    double m_base = 1.0, m_height = 1.0, m_scale = 0.5;
    int    m_sides = 4, m_depth = 0;
    double m_x = 0, m_y = 0, m_z = 0;
    double m_qw = 1, m_qx = 0, m_qy = 0, m_qz = 0;
};

#endif // GEOMETRYPROVIDER_PYRAMID_H__
