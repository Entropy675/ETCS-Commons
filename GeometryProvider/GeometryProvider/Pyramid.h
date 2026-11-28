#ifndef GEOMETRYPROVIDER_PYRAMID_H__
#define GEOMETRYPROVIDER_PYRAMID_H__

#include "../../../core_defs.h"
#include "../../../ontology.h"
#include <cmath>
#include <sstream>
#include <string>

/*
 * Pyramid -- regular n-sided pyramid with optional fractal depth.
 * PathOnFace / PathEdges expose parametric curves for other systems
 * (rails, ghost lines, paint strokes). Sample / Mesh are visual refs.
 */

class Pyramid : public Drawable3DBase<Pyramid>, public DeletableBase<Pyramid>
{
public:
    ETCS_TYPE_IDENTITY(Pyramid);

    Pyramid() = default;
    ~Pyramid() = default;

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
        this->addTag("fractal_depth", depth);
        this->addTag("fractal_scale", scale);
    }

    std::string PathOnFace(int face, double t) const
    {
        if (t < 0.0) t = 0.0;
        if (t > 1.0) t = 1.0;
        std::ostringstream o;
        if (face <= 0)
        {
            const double a0 = t * 2.0 * M_PI;
            o << (m_base * 0.5) * std::cos(a0) << " 0 "
              << (m_base * 0.5) * std::sin(a0);
        }
        else
        {
            const int i = (face - 1) % m_sides;
            const double a  = (2.0 * M_PI * i) / m_sides;
            const double bx = (m_base * 0.5) * std::cos(a);
            const double bz = (m_base * 0.5) * std::sin(a);
            o << bx * (1.0 - t) << ' ' << m_height * t << ' ' << bz * (1.0 - t);
        }
        return o.str();
    }

    std::string PathEdges() const
    {
        std::ostringstream o;
        for (int i = 0; i <= m_sides; ++i)
        {
            const double a = (2.0 * M_PI * (i % m_sides)) / m_sides;
            o << (m_base * 0.5) * std::cos(a) << " 0 "
              << (m_base * 0.5) * std::sin(a) << ' ';
        }
        for (int i = 0; i < m_sides; ++i)
        {
            const double a = (2.0 * M_PI * i) / m_sides;
            o << (m_base * 0.5) * std::cos(a) << " 0 "
              << (m_base * 0.5) * std::sin(a) << ' '
              << "0 " << m_height << " 0 ";
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
            o << ' ' << (m_base * 0.5) * std::cos(a)
              << " 0 " << (m_base * 0.5) * std::sin(a);
        }
        o << " 0 " << m_height << " 0";
        return o.str();
    }

    std::string Mesh() const
    {
        std::ostringstream o;
        o << "pyramid " << m_base << ' ' << m_height << ' ' << m_sides
          << ' ' << m_depth << ' ' << m_scale;
        return o.str();
    }

    void Delete() { this->removeTag("active"); }

    Box3D Bounds3D() const
    {
        const float h  = static_cast<float>(m_base * 0.5);
        const float ht = static_cast<float>(m_height);
        Box3D b;
        b.min = {-h, 0.f, -h};
        b.max = { h, ht,  h};
        return b;
    }

private:
    double m_base = 1.0, m_height = 1.0, m_scale = 0.5;
    int    m_sides = 4, m_depth = 0;
};

#endif // GEOMETRYPROVIDER_PYRAMID_H__
