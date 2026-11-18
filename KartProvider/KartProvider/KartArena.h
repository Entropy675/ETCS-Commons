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
 * A PAD lies on the level ground and pushes what drives over it the way it
 * points (yaw, as a ramp's): PadWidth() across, PadLength() along. A SPRING
 * is a round one, SpringRadius() about its middle, that throws a kart up.
 *
 * THE GROUND HAS LEVELS. A PIT is a floor sunk below the arena's, its sides
 * walls (ground a kart cannot climb): what is in a pit stands on its floor --
 * a ramp there rises from it, and one as high as the pit is deep is the way
 * out. A DECK is a level plane in the air with nothing under it: a kart at or
 * above its top stands on it, one below drives under it, and one thrown up
 * from below goes through it and comes down on top. So the ground under a
 * kart depends on how high the kart is (Height's `feet`); the SOLID ground --
 * the floor, the pits, the ramps and hills -- does not (Solid).
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
        Fixed base;                    // derived: what it stands on (a pit's floor, or 0)
        Fixed length() const { return up + flat + down; }
    };
    struct Hill
    {
        Fixed x, z, radius, height;
        Fixed a, b, y0;                // derived: the ellipsoid's half extents and its centre's height
    };
    struct Pad
    {
        Fixed x, z; int yaw = 0;
        Fixed sx, cz;                  // derived: the way it pushes (sin yaw, cos yaw)
        Fixed y;                       // derived: the ground it lies on
    };
    struct Spring { Fixed x, z; Fixed y; };          // y derived, as a pad's
    struct Deck   { Fixed x, z, w, d, h; };          // its top at h, w along x, d along z
    struct Pit    { Fixed x, z, w, d, depth; };      // its floor at -depth
    struct Map
    {
        std::string        name;
        bool               generated = false;   // made from a seed (Generate), not by hand
        std::vector<Block> blocks;
        std::vector<Ramp>  ramps;
        std::vector<Hill>  hills;
        std::vector<Pad>   pads;
        std::vector<Spring> springs;
        std::vector<Deck>  decks;
        std::vector<Pit>   pits;
        std::vector<Spot>  spawns, pickups;
    };

    inline Fixed F(int64_t n, int64_t d = 1) { return Fixed::FromInt(n) / Fixed::FromInt(d); }
    inline void Turn(int yaw_deg, Fixed& s, Fixed& c) { (Fixed::TwoPi() * F(yaw_deg, 360)).SinCos(s, c); }
    inline Fixed PadWidth()  { return F(3); }
    inline Fixed PadLength() { return F(4); }
    inline Fixed SpringRadius() { return F(3, 2); }
    inline Fixed DeckThick() { return F(1, 4); }      // as drawn; nothing is under its top but air

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
    // The pad a point is over, or null (a kart is on it only at its level: Pad::y).
    inline const Pad* PadAt(const Map& m, Fixed x, Fixed z)
    {
        for (const Pad& p : m.pads)
        {
            const Fixed dx = x - p.x, dz = z - p.z;
            const Fixed u = dx * p.sx + dz * p.cz, v = dx * p.cz - dz * p.sx;
            if (u.Abs() <= PadLength() * Fixed::Half() && v.Abs() <= PadWidth() * Fixed::Half()) return &p;
        }
        return nullptr;
    }
    inline const Spring* SpringAt(const Map& m, Fixed x, Fixed z)
    {
        for (const Spring& p : m.springs)
            if ((x - p.x) * (x - p.x) + (z - p.z) * (z - p.z) <= SpringRadius() * SpringRadius()) return &p;
        return nullptr;
    }
    inline bool Over(Fixed x, Fixed z, Fixed cx, Fixed cz, Fixed w, Fixed d)
    {
        return (x - cx).Abs() <= w * Fixed::Half() && (z - cz).Abs() <= d * Fixed::Half();
    }
    inline const Pit* PitAt(const Map& m, Fixed x, Fixed z)
    {
        for (const Pit& p : m.pits) if (Over(x, z, p.x, p.z, p.w, p.d)) return &p;
        return nullptr;
    }
    // What the ground is at a point: what a skid mark there is drawn over.
    enum Surface { Floor = 0, OnRamp = 1, OnHill = 2, OnPad = 3, OnDeck = 4, InPit = 5 };
    // THE SOLID GROUND at (x, z), with its slope and what it is: the highest
    // ramp or hill standing there, else a pit's floor, else the arena's.
    inline Fixed Solid(const Map& m, Fixed x, Fixed z, Fixed* gx_out = nullptr, Fixed* gz_out = nullptr,
                       Surface* what = nullptr)
    {
        Fixed best = Fixed::Zero(), bgx = Fixed::Zero(), bgz = Fixed::Zero();
        Surface on = Floor;
        bool stood = false;
        Fixed h, gx, gz;
        for (const Ramp& r : m.ramps)
            if (RampAt(r, x, z, h, gx, gz) && (!stood || h + r.base > best)) { best = h + r.base; bgx = gx; bgz = gz; on = OnRamp; stood = true; }
        for (const Hill& k : m.hills)
            if (HillAt(k, x, z, h, gx, gz) && (!stood || h > best)) { best = h; bgx = gx; bgz = gz; on = OnHill; stood = true; }
        if (!stood)
            if (const Pit* p = PitAt(m, x, z)) { best = -p->depth; on = InPit; }
        if (gx_out) *gx_out = bgx;
        if (gz_out) *gz_out = bgz;
        if (what) *what = on != OnRamp && on != OnHill && PadAt(m, x, z) ? OnPad : on;
        return best;
    }
    // THE GROUND UNDER SOMETHING whose feet are at `feet`: the solid ground,
    // or the highest deck over the point that it is on or above -- within
    // `reach` below a deck's top it is on it (a ramp's last step up; a fall's
    // last inch), and with no reach (rising) only above it.
    inline Fixed Height(const Map& m, Fixed x, Fixed z, Fixed feet, Fixed reach, Fixed* gx_out = nullptr,
                        Fixed* gz_out = nullptr, Surface* what = nullptr)
    {
        Fixed best = Solid(m, x, z, gx_out, gz_out, what);
        for (const Deck& d : m.decks)
            if (d.h > best && d.h <= feet + reach && Over(x, z, d.x, d.z, d.w, d.d))
            {
                best = d.h;
                if (gx_out) *gx_out = Fixed::Zero();
                if (gz_out) *gz_out = Fixed::Zero();
                if (what) *what = OnDeck;
            }
        return best;
    }
    // The top of everything at a point: where a box or a spawn is put.
    inline Fixed Top(const Map& m, Fixed x, Fixed z, Fixed* gx = nullptr, Fixed* gz = nullptr)
    {
        return Height(m, x, z, F(1000), Fixed::Zero(), gx, gz);
    }

    // What the ground is made of follows from what was given: call once a
    // map's ramps and hills are in.
    inline void Prepare(Map& m)
    {
        for (Ramp& r : m.ramps) Turn(r.yaw, r.sx, r.cz);
        for (Pad& p : m.pads)   Turn(p.yaw, p.sx, p.cz);
        // What stands in a pit stands on its floor.
        for (Ramp& r : m.ramps) { const Pit* p = PitAt(m, r.x, r.z); r.base = p ? -p->depth : Fixed::Zero(); }
        const Fixed half_root3 = F(3).Sqrt() / F(2);   // where an ellipsoid twice as wide meets the ground
        for (Hill& h : m.hills)
        {
            h.a  = h.radius * F(2);
            h.b  = h.height / (Fixed::One() - half_root3);
            h.y0 = -h.b * half_root3;
        }
        for (Pad& p : m.pads)       p.y = Solid(m, p.x, p.z);
        for (Spring& p : m.springs) p.y = Solid(m, p.x, p.z);
    }

    /*
     * A MAP FROM A SEED. First what keeps the arena playable: eight spawns on
     * a ring and the middle's box. Then, from the dice: banks on up to two
     * walls (a ramp up to the wall, ridden up and rolled back down from), and
     * nothing else within reach of a banked wall; a pit, one map in two, in
     * the middle, with a ramp out of it and a spring in it; a deck or two in
     * the air, each with a spring beside it and a box on top; hills; ramps of three kinds
     * -- jumps, decks (up, a level top, down) and small kickers; pads,
     * pointing any of eight ways; springs; blocks; and the power-up boxes.
     * Each is kept clear of everything placed before it, so a ramp never
     * runs into a block and nobody spawns in a wall. The dice are
     * KartArms::Next, so the same seed is the same map anywhere.
     */
    inline void Generate(uint64_t seed, Map& m)
    {
        m.blocks.clear(); m.ramps.clear(); m.hills.clear(); m.pads.clear(); m.spawns.clear(); m.pickups.clear();
        m.springs.clear(); m.decks.clear(); m.pits.clear();
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
        // A pit, one map in two, in the middle (the only room there is for
        // one), the middle's box at its bottom: its way out a ramp up its +z
        // wall, as high as it is deep, and a spring in the corner across.
        if (pick(0, 1) == 1)
        {
            Pit p; p.w = F(pick(9, 12)); p.d = F(pick(12, 15)); p.depth = F(5, 2);
            p.x = F(pick(-3, 3)); p.z = F(pick(-3, 3));
            m.pits.push_back(p);
            taken.push_back({ p.x, p.z, Fixed::Length(p.w, Fixed::Zero(), p.d) * Fixed::Half() });
            Ramp a; a.yaw = 0; a.width = F(4); a.up = F(7); a.flat = a.down = Fixed::Zero(); a.height = p.depth;
            a.x = p.x - p.w * Fixed::Half() + F(2); a.z = p.z + p.d * Fixed::Half() - a.up * Fixed::Half();
            m.ramps.push_back(a);
            m.springs.push_back({ p.x + p.w * Fixed::Half() - F(5, 2), p.z - p.d * Fixed::Half() + F(5, 2), Fixed::Zero() });
        }
        // Decks: a spring beside each, on whichever side has room, and a box
        // on top. Where 200 tries find room for none, a small one in a slot
        // against a wall: the spawns leave those free, and at most two walls
        // have banks.
        static const int slot[8][2] = { { 7, 25 }, { -7, -25 }, { 25, -7 }, { -25, 7 }, { -7, 25 }, { 7, -25 }, { 25, 7 }, { -25, -7 } };
        const int decks = pick(1, 2);
        for (int t = 0; t < 208 && static_cast<int>(m.decks.size()) < (t < 200 ? decks : 1); ++t)
        {
            Deck d; d.w = F(pick(6, 9)); d.d = F(pick(6, 9)); d.h = F(pick(6, 8), 2);
            d.x = F(pick(-24, 24)); d.z = F(pick(-24, 24));
            if (t >= 200) { d.w = d.d = F(6); d.x = F(slot[t - 200][0]); d.z = F(slot[t - 200][1]); }
            const Fixed rad = Fixed::Length(d.w, Fixed::Zero(), d.d) * Fixed::Half();
            if (!clear(d.x, d.z, rad)) continue;
            static const int side[4][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
            const int from = pick(0, 3);
            Fixed sx, sz;
            bool room = false;
            for (int k = 0; k < 4 && !room; ++k)
            {
                const int* at = side[(from + k) % 4];
                sx = d.x + F(at[0]) * (d.w * Fixed::Half() + F(2)); sz = d.z + F(at[1]) * (d.d * Fixed::Half() + F(2));
                room = clear(sx, sz, SpringRadius());
            }
            if (!room) continue;
            m.decks.push_back(d); taken.push_back({ d.x, d.z, rad });
            m.springs.push_back({ sx, sz, Fixed::Zero() }); taken.push_back({ sx, sz, SpringRadius() });
            m.pickups.push_back({ d.x, d.z });
        }
        const int hills = pick(1, 3);
        for (int t = 0; t < 200 && static_cast<int>(m.hills.size()) < hills; ++t)
        {
            Hill k; k.radius = F(pick(5, 8)); k.height = F(pick(10, 16), 10);
            k.x = F(pick(-24, 24)); k.z = F(pick(-24, 24));
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
        const int pads = pick(2, 4);
        for (int t = 0; t < 200 && static_cast<int>(m.pads.size()) < pads; ++t)
        {
            Pad p; p.yaw = pick(0, 7) * 45;
            p.x = F(pick(-22, 22)); p.z = F(pick(-22, 22));
            const Fixed rad = F(5, 2);   // half its diagonal
            if (!clear(p.x, p.z, rad)) continue;
            m.pads.push_back(p); taken.push_back({ p.x, p.z, rad });
        }
        const int springs = static_cast<int>(m.springs.size()) + pick(1, 2);
        for (int t = 0; t < 200 && static_cast<int>(m.springs.size()) < springs; ++t)
        {
            const Fixed x = F(pick(-22, 22)), z = F(pick(-22, 22));
            if (!clear(x, z, SpringRadius())) continue;
            m.springs.push_back({ x, z, Fixed::Zero() }); taken.push_back({ x, z, SpringRadius() });
        }
        const int blocks = pick(4, 8);
        for (int t = 0; t < 200 && static_cast<int>(m.blocks.size()) < blocks; ++t)
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
