#ifndef KARTPROVIDER_KARTARENA_H__
#define KARTPROVIDER_KARTARENA_H__

#include "../../../ontology.h"
#include "KartArms.h"

#include <cstdint>
#include <string>
#include <vector>

/*
 * ── KartArena: a map as data, and the ground's height ──────────────────────
 *
 * A MAP IS WHAT IS ON IT, not the nodes that draw it: blocks (solid, the
 * karts bump them and a shot stops on them), ramps and hills (the GROUND: not
 * solid, a height the battle keeps each kart at), spawns and power-up spots.
 * The battle draws the map in play with nodes from a pool -- the same pool
 * for every map, the hand-made ones and the one made from a seed -- so a
 * change of map is a line of the record, and a map nobody made by hand is
 * the same on every runtime because its seed is.
 *
 * THE GROUND is the highest of what stands at a point, 0 where nothing does:
 *
 *   ramp   along its direction (yaw, degrees, from +z toward +x): `up` units
 *          rising to `height`, `flat` units level, `down` units falling --
 *          `down` 0 is a jump, its lip a drop; `up` 0 a landing, its near
 *          edge a step; `width` across
 *   hill   a dome of `radius` and `height`: the top of an ellipsoid twice the
 *          radius across, sunk so it meets the ground at a slope of about
 *          2.2 x height / radius, never a wall
 *
 * Every number is the rows' own integers (Fixed), the generator's too, so
 * the ground under a kart is the same on every runtime.
 */
namespace KartArena
{
    struct Spot  { Fixed x, z; };
    struct Block { Fixed x, z, w, d, h; int yaw = 0; };
    struct Ramp
    {
        Fixed x, z; int yaw = 0;
        Fixed width, up, flat, down, height;
        Fixed sx, cz;                  // derived: the direction (sin yaw, cos yaw)
        Fixed length() const { return up + flat + down; }
    };
    struct Hill
    {
        Fixed x, z, radius, height;
        Fixed a, b, y0;                // derived: the ellipsoid's half extents and its centre's height
    };
    struct Map
    {
        std::string        name;
        bool               generated = false;   // made from a seed (Generate), not by hand
        std::vector<Block> blocks;
        std::vector<Ramp>  ramps;
        std::vector<Hill>  hills;
        std::vector<Spot>  spawns, pickups;
    };

    inline Fixed F(int64_t n, int64_t d = 1) { return Fixed::FromInt(n) / Fixed::FromInt(d); }
    inline void Turn(int yaw_deg, Fixed& s, Fixed& c) { (Fixed::TwoPi() * F(yaw_deg, 360)).SinCos(s, c); }

    // What the ground is made of follows from what was given: call once a
    // map's ramps and hills are in.
    inline void Prepare(Map& m)
    {
        for (Ramp& r : m.ramps) Turn(r.yaw, r.sx, r.cz);
        const Fixed half_root3 = F(3).Sqrt() / F(2);   // where an ellipsoid twice as wide meets the ground
        for (Hill& h : m.hills)
        {
            h.a  = h.radius * F(2);
            h.b  = h.height / (Fixed::One() - half_root3);
            h.y0 = -h.b * half_root3;
        }
    }

    // A ramp's height at a point, and its slope there (rise per unit, along x
    // and z); false off its footprint.
    inline bool RampAt(const Ramp& r, Fixed x, Fixed z, Fixed& h, Fixed& gx, Fixed& gz)
    {
        const Fixed dx = x - r.x, dz = z - r.z;
        const Fixed u = dx * r.sx + dz * r.cz;              // along
        const Fixed v = dx * r.cz - dz * r.sx;              // across
        const Fixed len = r.length();
        const Fixed s = u + len * Fixed::Half();
        if (v.Abs() > r.width * Fixed::Half() || s.raw < 0 || s > len) return false;
        Fixed rise = Fixed::Zero();
        if (s < r.up)                  { h = r.height * s / r.up; rise = r.height / r.up; }
        else if (s <= r.up + r.flat)   { h = r.height; }
        else                           { h = r.height * (len - s) / r.down; rise = -r.height / r.down; }
        gx = rise * r.sx; gz = rise * r.cz;
        return true;
    }
    inline bool HillAt(const Hill& k, Fixed x, Fixed z, Fixed& h, Fixed& gx, Fixed& gz)
    {
        const Fixed dx = x - k.x, dz = z - k.z;
        const Fixed d2 = dx * dx + dz * dz;
        if (!(d2 < k.radius * k.radius)) return false;
        const Fixed q = (Fixed::One() - d2 / (k.a * k.a)).Sqrt();
        h = k.y0 + k.b * q;
        if (h.raw <= 0) return false;
        const Fixed per = q.IsPositive() ? -k.b / (k.a * k.a * q) : Fixed::Zero();
        gx = per * dx; gz = per * dz;
        return true;
    }
    // The ground: the highest thing standing at (x, z), with its slope.
    inline Fixed Height(const Map& m, Fixed x, Fixed z, Fixed* gx_out = nullptr, Fixed* gz_out = nullptr)
    {
        Fixed best = Fixed::Zero(), bgx = Fixed::Zero(), bgz = Fixed::Zero();
        Fixed h, gx, gz;
        for (const Ramp& r : m.ramps) if (RampAt(r, x, z, h, gx, gz) && h > best) { best = h; bgx = gx; bgz = gz; }
        for (const Hill& k : m.hills) if (HillAt(k, x, z, h, gx, gz) && h > best) { best = h; bgx = gx; bgz = gz; }
        if (gx_out) *gx_out = bgx;
        if (gz_out) *gz_out = bgz;
        return best;
    }

    /*
     * A MAP FROM A SEED. First what keeps the arena playable: eight spawns on
     * a ring and the middle's box. Then, from the dice: banks on up to two
     * walls (a ramp up to the wall, ridden up and rolled back down from), and
     * nothing else within reach of a banked wall; hills; ramps of three
     * kinds -- jumps, decks (up, a level top, down) and small kickers; blocks;
     * and the power-up boxes. Each is kept clear of everything placed before
     * it, so a ramp never runs into a block and nobody spawns in a wall. The
     * dice are KartArms::Next, so the same seed is the same map anywhere.
     */
    inline void Generate(uint64_t seed, Map& m)
    {
        m.blocks.clear(); m.ramps.clear(); m.hills.clear(); m.spawns.clear(); m.pickups.clear();
        m.generated = true;
        uint64_t r = seed | 1;
        auto pick = [&r](int lo, int hi) { return lo + static_cast<int>(KartArms::Next(r) % static_cast<uint64_t>(hi - lo + 1)); };
        struct Room { Fixed x, z, rad; };
        std::vector<Room> taken;
        // How far out a thing may reach on each side (+x, -x, +z, -z): to the
        // wall, or short of a bank on it.
        Fixed reach[4] = { F(30), F(30), F(30), F(30) };
        auto clear = [&taken, &reach](Fixed x, Fixed z, Fixed rad) {
            if (x + rad > reach[0] || rad - x > reach[1] || z + rad > reach[2] || rad - z > reach[3]) return false;
            for (const Room& t : taken)
            {
                const Fixed gap = rad + t.rad + F(3, 2);
                if ((x - t.x) * (x - t.x) + (z - t.z) * (z - t.z) < gap * gap) return false;
            }
            return true;
        };
        static const int ring[8][2] = { { 0, -20 }, { 0, 20 }, { -20, 0 }, { 20, 0 }, { 14, -14 }, { -14, 14 }, { 14, 14 }, { -14, -14 } };
        for (const auto& p : ring) { m.spawns.push_back({ F(p[0]), F(p[1]) }); taken.push_back({ F(p[0]), F(p[1]), F(2) }); }
        m.pickups.push_back({ Fixed::Zero(), Fixed::Zero() });
        taken.push_back({ Fixed::Zero(), Fixed::Zero(), F(2) });

        // Banks: the side (+x, -x, +z, -z) and its yaw, rising toward the wall.
        static const int sides[4][2] = { { 90, 0 }, { 270, 1 }, { 0, 2 }, { 180, 3 } };
        const int banks = pick(0, 2);
        int first = pick(0, 3);
        for (int b = 0; b < banks; ++b)
        {
            const int side = (first + b * pick(1, 3)) % 4;
            if (reach[side] < F(30)) continue;   // the same side twice: one bank
            Ramp a; a.yaw = sides[side][0]; a.width = F(pick(20, 34)); a.up = F(pick(5, 7));
            a.flat = a.down = Fixed::Zero(); a.height = F(pick(15, 22), 10);
            const Fixed at = F(31) - a.up * Fixed::Half();   // its lip on the karts' bound
            const Fixed along = F(pick(-4, 4));
            a.x = side == 0 ? at : side == 1 ? -at : along;
            a.z = side == 2 ? at : side == 3 ? -at : along;
            m.ramps.push_back(a);
            reach[side] = F(31) - a.up - F(1);
        }
        const int hills = pick(1, 3);
        for (int t = 0; t < 40 && static_cast<int>(m.hills.size()) < hills; ++t)
        {
            Hill k; k.radius = F(pick(5, 8)); k.height = F(pick(10, 16), 10);
            k.x = F(pick(-18, 18)); k.z = F(pick(-18, 18));
            if (!clear(k.x, k.z, k.radius)) continue;
            m.hills.push_back(k); taken.push_back({ k.x, k.z, k.radius });
        }
        const int ramps = static_cast<int>(m.ramps.size()) + pick(3, 6);
        for (int t = 0; t < 120 && static_cast<int>(m.ramps.size()) < ramps; ++t)
        {
            Ramp a; a.yaw = pick(0, 7) * 45;
            switch (pick(0, 2))
            {
                case 0:   // a jump
                    a.width = F(pick(3, 5)); a.up = F(pick(5, 7)); a.flat = a.down = Fixed::Zero();
                    a.height = F(pick(10, 18), 10); break;
                case 1:   // a deck
                    a.width = F(pick(4, 6)); a.up = F(pick(4, 6)); a.flat = F(pick(3, 6)); a.down = F(pick(4, 6));
                    a.height = F(pick(12, 20), 10); break;
                default:  // a kicker
                    a.width = F(3); a.up = F(pick(3, 4)); a.flat = a.down = Fixed::Zero();
                    a.height = F(pick(8, 12), 10); break;
            }
            a.x = F(pick(-20, 20)); a.z = F(pick(-20, 20));
            const Fixed rad = a.length() * Fixed::Half() + a.width * Fixed::Half();
            if (!clear(a.x, a.z, rad)) continue;
            m.ramps.push_back(a); taken.push_back({ a.x, a.z, rad });
        }
        const int blocks = pick(4, 8);
        for (int t = 0; t < 80 && static_cast<int>(m.blocks.size()) < blocks; ++t)
        {
            Block b; b.w = F(pick(2, 7)); b.d = F(pick(2, 7)); b.h = F(pick(14, 40), 10);
            b.yaw = pick(0, 3) == 0 ? 45 : 0;
            b.x = F(pick(-22, 22)); b.z = F(pick(-22, 22));
            const Fixed rad = (b.w + b.d) * Fixed::Half();
            if (!clear(b.x, b.z, rad)) continue;
            m.blocks.push_back(b); taken.push_back({ b.x, b.z, rad });
        }
        for (int t = 0; t < 60 && m.pickups.size() < 8; ++t)
        {
            const Fixed x = F(pick(-22, 22)), z = F(pick(-22, 22));
            if (!clear(x, z, F(1))) continue;
            m.pickups.push_back({ x, z }); taken.push_back({ x, z, F(1) });
        }
        Prepare(m);
    }
}

#endif // KARTPROVIDER_KARTARENA_H__
