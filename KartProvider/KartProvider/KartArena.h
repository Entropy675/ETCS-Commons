#ifndef KARTPROVIDER_KARTARENA_H__
#define KARTPROVIDER_KARTARENA_H__

#include "../../../ontology.h"
#include "KartArms.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
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
 *   arc    a ramp bent round a centre: `width` across a circle of `radius`,
 *          starting at bearing `from` (degrees, as a yaw) and running
 *          `up + flat + down` along that circle, clockwise seen from above
 *          for `turn` 1, the other way for -1. The profile is measured along
 *          the circle, so its edges are circles too and the turn has no
 *          joins; toward the centre the same rise is steeper (radius / r)
 *
 * A PAD lies on the ground -- level, or one slope: a bank's -- and pushes
 * what drives over it the way it points (yaw, as a ramp's): PadWidth() across,
 * PadLength() along. A SPRING is a round one, SpringRadius() about its middle,
 * on level ground or on a deck (`on`, that deck's top), that throws a kart up.
 *
 * THE GROUND HAS LEVELS, and they are meant big: half a map sunk, a storey
 * over a third of it. A PIT is a floor sunk below the arena's, its sides
 * walls (ground a kart cannot climb): what is in a pit stands on its floor --
 * a block, a hill, a ramp that rises from it; and a ramp as high as the pit
 * is deep, its lip at the pit's edge, is a way out. A pit of LAVA has no way
 * out and needs none: a kart that comes down on its floor is out. What stands
 * in it as high as it is deep -- a ramp all level top -- is an island, or a
 * causeway across. A DECK is a level plane in the air with nothing under it
 * but what is on the ground there: a kart at or above its top stands on it,
 * one below drives under it, and one thrown up from below goes through it
 * and comes down on top; a ramp as high as the deck, its top at the deck's
 * edge (a deck is laid in pieces round a ramp that comes up through it), is
 * a way up. So the ground under a kart depends on how high the kart is
 * (Height's `feet`); the SOLID ground -- the floor, the pits, the ramps and
 * hills -- does not (Solid).
 *
 * Every number is the rows' own integers (Fixed), the generator's too, so
 * the ground under a kart is the same on every runtime.
 */
namespace KartArena
{
    struct Spot  { Fixed x, z; bool up = false; };     // up: a box on the deck over the spot, not under it
    struct Block { Fixed x, z, w, d, h; int yaw = 0; Fixed base; };   // base derived, as a ramp's
    struct Ramp
    {
        Fixed x, z; int yaw = 0;
        Fixed width, up, flat, down, height;
        Fixed sx, cz;                  // derived: the direction (sin yaw, cos yaw)
        Fixed base;                    // derived: what it stands on (a pit's floor, or 0)
        bool  level = false;           // Generate's: a way between levels (a bank, an exit, a stairwell), stretched with them
        Fixed length() const { return up + flat + down; }
    };
    struct Hill
    {
        Fixed x, z, radius, height;
        Fixed a, b, y0;                // derived: the ellipsoid's half extents and its centre's height
        Fixed base;                    // derived: what it stands on, as a ramp's
    };
    struct Pad
    {
        Fixed x, z; int yaw = 0;
        Fixed sx, cz;                  // derived: the way it pushes (sin yaw, cos yaw)
        Fixed y, gx, gz;               // derived: the ground it lies on at its middle, and that ground's slope
    };
    struct Spring { Fixed x, z; Fixed y; Fixed on = Fixed::Zero(); };   // on: the deck top it lies on, 0 the ground; y derived
    struct Arc
    {
        Fixed x, z, radius; int from = 0, turn = 1;
        Fixed width, up, flat, down, height;
        Fixed a0;                      // derived: `from` in radians, [0, 2pi)
        Fixed base;                    // derived: what it stands on, as a ramp's
        Fixed length() const { return up + flat + down; }
    };
    // Its top at h, `w` across and `d` along its yaw (yaw 0: w along x, d along z).
    struct Deck   { Fixed x, z, w, d, h; int yaw = 0; Fixed sx = Fixed::Zero(), cz = Fixed::One(); };   // sx, cz derived
    struct Pit    { Fixed x, z, w, d, depth; bool lava = false; };   // its floor at -depth; lava: a kart on that floor is out
    struct Map
    {
        std::string        name;
        bool               generated = false;   // made from a seed (Generate), not by hand
        std::vector<Block> blocks;
        std::vector<Ramp>  ramps;
        std::vector<Hill>  hills;
        std::vector<Arc>   arcs;
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
    inline Fixed DeckThick() { return F(3, 4); }      // as drawn; nothing is under its top but air
    // THE ARENA: square, its walls' inside this far from the middle each way,
    // their top this high (kart_arena.etcs builds them to these).
    inline Fixed Half() { return F(96); }
    inline Fixed WallTop() { return F(4); }

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
    // atan for |t| <= 1 in the rows' own integers -- halved once to |u| <=
    // tan(pi/8), then the series -- so every runtime has the same raw angle.
    inline Fixed Atan1(Fixed t)
    {
        const Fixed u = t / (Fixed::One() + (Fixed::One() + t * t).Sqrt());
        const Fixed u2 = u * u;
        Fixed term = u, sum = u;
        for (int n = 1; n < 16; ++n) { term = -(term * u2); sum += term / F(2 * n + 1); }
        return sum * F(2);
    }
    // The bearing of (dx, dz) as a yaw is: from +z toward +x, in [0, 2pi).
    inline Fixed Bearing(Fixed dx, Fixed dz)
    {
        const Fixed ax = dx.Abs(), az = dz.Abs();
        if (ax.IsZero() && az.IsZero()) return Fixed::Zero();
        Fixed a = ax <= az ? Atan1(ax / az) : Fixed::Pi() * Fixed::Half() - Atan1(az / ax);
        if (dz.raw < 0) a = Fixed::Pi() - a;
        if (dx.raw < 0) a = Fixed::TwoPi() - a;
        return a;
    }
    // A profile's height `s` along it, and its rise there per unit along.
    inline Fixed Profile(Fixed s, Fixed up, Fixed flat, Fixed down, Fixed height, Fixed& rise)
    {
        rise = Fixed::Zero();
        if (s < up)               { rise = height / up; return height * s / up; }
        if (s <= up + flat)       return height;
        rise = -height / down;
        return height * (up + flat + down - s) / down;
    }
    // How far round an arc (x, z) is, along its middle; false off it.
    inline bool ArcAlong(const Arc& c, Fixed x, Fixed z, Fixed& s, Fixed& d)
    {
        const Fixed dx = x - c.x, dz = z - c.z;
        d = Fixed::Length(dx, Fixed::Zero(), dz);
        if (!d.IsPositive() || (d - c.radius).Abs() > c.width * Fixed::Half()) return false;
        Fixed round = (Bearing(dx, dz) - c.a0) * F(c.turn);
        if (round.raw < 0) round += Fixed::TwoPi();
        if (!(round < Fixed::TwoPi())) round -= Fixed::TwoPi();
        s = round * c.radius;
        return !(s > c.length());
    }
    inline bool ArcAt(const Arc& c, Fixed x, Fixed z, Fixed& h, Fixed& gx, Fixed& gz)
    {
        Fixed s, d, rise;
        if (!ArcAlong(c, x, z, s, d)) return false;
        h = Profile(s, c.up, c.flat, c.down, c.height, rise);
        // Along the way it runs at (x, z), steeper toward the centre.
        const Fixed slope = rise * c.radius / d;
        gx = slope * F(c.turn) * (z - c.z) / d;
        gz = -slope * F(c.turn) * (x - c.x) / d;
        return true;
    }
    // A point on an arc's middle `s` along it.
    inline void ArcPoint(const Arc& c, Fixed s, Fixed& x, Fixed& z)
    {
        Fixed sn, cs;
        (c.a0 + F(c.turn) * s / c.radius).SinCos(sn, cs);
        x = c.x + c.radius * sn; z = c.z + c.radius * cs;
    }
    // Is (x, z) under a deck, turned or not?
    inline bool DeckOver(const Deck& d, Fixed x, Fixed z)
    {
        const Fixed dx = x - d.x, dz = z - d.z;
        return (dx * d.cz - dz * d.sx).Abs() <= d.w * Fixed::Half() && (dx * d.sx + dz * d.cz).Abs() <= d.d * Fixed::Half();
    }
    // A point of a deck, `across` and `along` from its middle in its own frame.
    inline void DeckPoint(const Deck& d, Fixed across, Fixed along, Fixed& x, Fixed& z)
    {
        x = d.x + across * d.cz + along * d.sx; z = d.z - across * d.sx + along * d.cz;
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
    // The pad a point is over, or null (a kart is on it only on its ground, not a deck over it).
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
    // The spring a kart standing at `feet` over (x, z) is on, or null: one on a
    // deck is not under a kart on the ground there, nor the other way round.
    inline const Spring* SpringAt(const Map& m, Fixed x, Fixed z, Fixed feet)
    {
        for (const Spring& p : m.springs)
            if ((x - p.x) * (x - p.x) + (z - p.z) * (z - p.z) <= SpringRadius() * SpringRadius()
                && (feet - p.y).Abs() < F(5, 100)) return &p;
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
            if (HillAt(k, x, z, h, gx, gz) && (!stood || h + k.base > best)) { best = h + k.base; bgx = gx; bgz = gz; on = OnHill; stood = true; }
        for (const Arc& c : m.arcs)
            if (ArcAt(c, x, z, h, gx, gz) && (!stood || h + c.base > best)) { best = h + c.base; bgx = gx; bgz = gz; on = OnRamp; stood = true; }
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
            if (d.h > best && d.h <= feet + reach && DeckOver(d, x, z))
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
        for (Deck& d : m.decks) Turn(d.yaw, d.sx, d.cz);
        for (Arc& c : m.arcs)   c.a0 = Fixed::TwoPi() * F(((c.from % 360) + 360) % 360, 360);
        // What stands in a pit stands on its floor.
        auto floor = [&m](Fixed x, Fixed z) { const Pit* p = PitAt(m, x, z); return p ? -p->depth : Fixed::Zero(); };
        for (Ramp& r : m.ramps)   r.base = floor(r.x, r.z);
        for (Block& b : m.blocks) b.base = floor(b.x, b.z);
        for (Hill& h : m.hills)   h.base = floor(h.x, h.z);
        for (Arc& c : m.arcs)     { Fixed x, z; ArcPoint(c, c.length() * Fixed::Half(), x, z); c.base = floor(x, z); }
        const Fixed half_root3 = F(3).Sqrt() / F(2);   // where an ellipsoid twice as wide meets the ground
        for (Hill& h : m.hills)
        {
            h.a  = h.radius * F(2);
            h.b  = h.height / (Fixed::One() - half_root3);
            h.y0 = -h.b * half_root3;
        }
        for (Pad& p : m.pads)       p.y = Solid(m, p.x, p.z, &p.gx, &p.gz);
        for (Spring& p : m.springs) p.y = p.on.IsPositive() ? p.on : Solid(m, p.x, p.z);
    }

    /*
     * WHAT IS WRONG WITH A MAP, a fault a line, "" for a sound one (Prepare it
     * first). A map's maker -- a script's author, or Generate -- keeps these
     * so that what is on it can be reached and does what it shows:
     *
     *   pad      solid ground in one piece under the whole plate -- level, or
     *            one slope; its middle clear of blocks; no other pad over
     *            it, no spawn at it
     *   spring   the same on level ground, and no pad at it; one on a deck,
     *            on that deck all round, no ground standing up through it
     *   block    level solid ground under it: not on a ramp or a hill, and
     *            wholly in a pit or wholly out
     *   ramp     one that ends in a drop facing a wall runs on into the
     *            wall (its lip past the wall's inside), or stops well back
     *            from it: no trench under the lip to be stuck in -- not
     *            even the hairline a lip on the karts' bound leaves
     *   deck     room to drive under it, over the ground there (a block
     *            under one is a pillar)
     *   arc      less than a full circle round, narrower than its radius, and
     *            its inside edge no steeper than a kart can climb
     *   pit      a ramp, an arc or a hill wholly in it or wholly out, and a hill
     *            outside it well back (its dome is drawn on under the
     *            ground and would show in the pit); no spawn at its edge;
     *            a way out -- a spring in it, or a ramp in it as high as
     *            it is deep
     *   lava     a pit that needs no way out; no spawn, box, pad or spring
     *            on its floor
     *   bare     no stretch of ground with nothing on it: everywhere a kart
     *            can drive is within BareReach() of something to drive at,
     *            over, under or round -- a block, a ramp, a hill, a deck, a
     *            pit's edge, a pad, a spring, a box (Nearest)
     */
    inline Fixed BareReach() { return F(26); }
    // How far (x, z) is from the nearest thing on the map: a pad, a spring or a
    // box by its middle; a block, a ramp, a hill or a deck by its outline (a
    // turned one by the ring round it); a pit by its edge, from either side.
    inline Fixed Nearest(const Map& m, Fixed x, Fixed z)
    {
        Fixed best = F(100000);
        auto point = [&](Fixed px, Fixed pz) { best = Fixed::Min(best, Fixed::Length(x - px, Fixed::Zero(), z - pz)); };
        auto round = [&](Fixed px, Fixed pz, Fixed r) { best = Fixed::Min(best, Fixed::Max(Fixed::Zero(), Fixed::Length(x - px, Fixed::Zero(), z - pz) - r)); };
        auto rect = [&](Fixed px, Fixed pz, Fixed w, Fixed d) {
            const Fixed dx = (x - px).Abs() - w * Fixed::Half(), dz = (z - pz).Abs() - d * Fixed::Half();
            return std::pair<Fixed, Fixed>(dx, dz);
        };
        for (const Pad& p : m.pads)       point(p.x, p.z);
        for (const Spring& p : m.springs) point(p.x, p.z);
        for (const Spot& p : m.pickups)   point(p.x, p.z);
        for (const Hill& k : m.hills)     round(k.x, k.z, k.radius);
        // An outline square to the arena, or turned a quarter: by its edge.
        auto box = [&](Fixed px, Fixed pz, Fixed w, Fixed d, int yaw) {
            const int q = ((yaw % 180) + 180) % 180;
            if (q != 0 && q != 90) { round(px, pz, Fixed::Min(w, d) * Fixed::Half()); return; }
            const auto e = rect(px, pz, q ? d : w, q ? w : d);
            best = Fixed::Min(best, Fixed::Length(Fixed::Max(e.first, Fixed::Zero()), Fixed::Zero(), Fixed::Max(e.second, Fixed::Zero())));
        };
        for (const Block& b : m.blocks)   box(b.x, b.z, b.w, b.d, b.yaw);
        for (const Ramp& r : m.ramps)     box(r.x, r.z, r.width, r.length(), r.yaw);
        for (const Deck& d : m.decks)
        {
            if (d.yaw % 90 == 0) { box(d.x, d.z, d.w, d.d, d.yaw); continue; }
            // Turned: by discs along its length.
            const Fixed r = Fixed::Min(d.w, d.d) * Fixed::Half(), run = Fixed::Max(d.w, d.d) * Fixed::Half() - r;
            const bool along = d.d >= d.w;
            for (Fixed t = -run; !(t > run); t += F(2))
            {
                Fixed x, z;
                DeckPoint(d, along ? Fixed::Zero() : t, along ? t : Fixed::Zero(), x, z);
                round(x, z, r);
            }
        }
        // An arc by discs along its middle, two units apart.
        for (const Arc& c : m.arcs)
            for (Fixed t = Fixed::Zero(); !(t > c.length()); t += F(2))
            {
                Fixed x, z;
                ArcPoint(c, t, x, z);
                round(x, z, c.width * Fixed::Half());
            }
        for (const Pit& p : m.pits)
        {
            const auto e = rect(p.x, p.z, p.w, p.d);
            const bool in = e.first.raw < 0 && e.second.raw < 0;
            best = Fixed::Min(best, in ? Fixed::Min(-e.first, -e.second)
                                       : Fixed::Length(Fixed::Max(e.first, Fixed::Zero()), Fixed::Zero(), Fixed::Max(e.second, Fixed::Zero())));
        }
        return best;
    }
    // The ground a kart drives on, tried every eight units: is it bare there?
    // Not a lava pit's floor (nobody drives there), nor under the middle of a
    // long thing, which Nearest calls near.
    template <typename Each> inline void EachBare(const Map& m, Each each)
    {
        const int64_t edge = (Half() - F(8)).ToInt() / 8 * 8;
        for (int64_t i = -edge; i <= edge; i += 8)
            for (int64_t k = -edge; k <= edge; k += 8)
            {
                const Fixed x = F(i), z = F(k);
                bool molten = false;
                for (const Pit& p : m.pits) molten = molten || (p.lava && Over(x, z, p.x, p.z, p.w, p.d));
                if (!molten && Nearest(m, x, z) > BareReach()) each(x, z);
            }
    }
    inline Fixed Headroom() { return F(3, 2); }       // under a deck, over the highest ground below it
    inline Fixed HillSkirt() { return F(8, 5); }      // of a hill's radius: how far its dome runs on under the ground
    inline std::string Faults(const Map& m)
    {
        std::string out;
        auto say = [&out](const char* what, Fixed x, Fixed z, const char* fault) {
            char line[96];
            std::snprintf(line, sizeof line, "%s %g %g: %s\n", what, static_cast<double>(x.ToFloat()), static_cast<double>(z.ToFloat()), fault);
            out += line;
        };
        struct Turned { Fixed s, c; };
        std::vector<Turned> turn(m.blocks.size());
        for (size_t i = 0; i < m.blocks.size(); ++i) Turn(m.blocks[i].yaw, turn[i].s, turn[i].c);
        // Is a point in a block (or within `margin` of one)?
        auto blocked = [&](Fixed x, Fixed z, Fixed margin) {
            for (size_t i = 0; i < m.blocks.size(); ++i)
            {
                const Block& b = m.blocks[i];
                const Fixed dx = x - b.x, dz = z - b.z;
                if ((dx * turn[i].c - dz * turn[i].s).Abs() < b.w * Fixed::Half() + margin
                    && (dx * turn[i].s + dz * turn[i].c).Abs() < b.d * Fixed::Half() + margin) return true;
            }
            return false;
        };
        auto within = [](const Spot& a, Fixed x, Fixed z, Fixed d) { return (a.x - x) * (a.x - x) + (a.z - z) * (a.z - z) < d * d; };
        // A plate about (x, z), `along` and `across` its half extents the way
        // (sx, cz) points -- round, a disc of `along`: what it lies on, tried
        // at a grid of points over it.
        auto plate = [&](const char* what, Fixed x, Fixed z, Fixed sx, Fixed cz, Fixed along, Fixed across, bool round, const Pad* self) {
            bool level = true, alone = true;
            Fixed gx, gz;
            const Fixed y = Solid(m, x, z, &gx, &gz);
            if (!self && (!gx.IsZero() || !gz.IsZero())) level = false;   // a spring: level ground only
            for (int i = -2; i <= 2; ++i)
                for (int j = -2; j <= 2; ++j)
                {
                    if (round && i * i + j * j > 4) continue;
                    const Fixed u = along * F(i, 2), v = across * F(j, 2);
                    const Fixed px = x + sx * u + cz * v, pz = z + cz * u - sx * v;
                    // The one slope its middle is on, carried out to here.
                    level = level && (Solid(m, px, pz) - (y + gx * (px - x) + gz * (pz - z))).Abs() < F(1, 100);
                    const Pad* on = PadAt(m, px, pz);
                    alone = alone && (!on || on == self);
                }
            if (!level) say(what, x, z, self ? "not on one piece of ground" : "not on level ground");
            if (blocked(x, z, F(1, 5))) say(what, x, z, "in a block");
            if (!alone) say(what, x, z, self ? "over another pad" : "at a pad");
        };
        for (const Pad& p : m.pads)
        {
            plate("pad", p.x, p.z, p.sx, p.cz, PadLength() * Fixed::Half(), PadWidth() * Fixed::Half(), false, &p);
            for (const Spot& a : m.spawns) if (within(a, p.x, p.z, F(9, 2))) say("pad", p.x, p.z, "at a spawn");
        }
        for (const Spring& p : m.springs)
        {
            if (p.on.IsPositive())
            {
                bool held = true;
                for (int i = -2; i <= 2; ++i)
                    for (int j = -2; j <= 2; ++j)
                    {
                        if (i * i + j * j > 4) continue;
                        const Fixed px = p.x + SpringRadius() * F(i, 2), pz = p.z + SpringRadius() * F(j, 2);
                        bool deck = false;
                        for (const Deck& d : m.decks) deck = deck || (d.h == p.on && DeckOver(d, px, pz));
                        held = held && deck && Solid(m, px, pz) < p.on;
                    }
                if (!held) say("spring", p.x, p.z, "not on one deck at its height");
            }
            else plate("spring", p.x, p.z, Fixed::Zero(), Fixed::One(), SpringRadius(), SpringRadius(), true, nullptr);
            for (const Spot& a : m.spawns) if (within(a, p.x, p.z, F(7, 2))) say("spring", p.x, p.z, "at a spawn");
        }
        for (size_t i = 0; i < m.blocks.size(); ++i)
        {
            // Its middle and, a hair in from them, its corners.
            const Block& b = m.blocks[i];
            const Fixed hw = b.w * F(49, 100), hd = b.d * F(49, 100);
            bool level = Solid(m, b.x, b.z) == b.base;
            for (int u = -1; u <= 1; u += 2)
                for (int v = -1; v <= 1; v += 2)
                    level = level && Solid(m, b.x + turn[i].c * hw * F(u) + turn[i].s * hd * F(v),
                                              b.z - turn[i].s * hw * F(u) + turn[i].c * hd * F(v)) == b.base;
            if (!level) say("block", b.x, b.z, "not on level ground");
        }
        // A ramp that ends in a drop facing a wall, short of the wall by
        // less than a kart can turn in: the floor between its lip and the
        // wall is a trench to fall into, be stuck in, and not be seen. A lip
        // right on the karts' bound is one too: the ground's sums there are a
        // hair either side of it, and a slim ride finds the hair. Only ground
        // as high just past the lip (another ramp, a deck) fills it.
        for (const Ramp& r : m.ramps)
        {
            if (!r.down.IsZero()) continue;
            const bool along_x = r.sx.Abs() > F(99, 100), along_z = r.cz.Abs() > F(99, 100);
            if (!along_x && !along_z) continue;
            const Fixed hl = r.length() * Fixed::Half();
            const Fixed ex = r.x + r.sx * hl, ez = r.z + r.cz * hl;   // its lip's middle
            const Fixed out = along_x ? ex * r.sx : ez * r.cz;         // how far out, the way it goes
            if (!(out < Half()) || !(out > Half() - F(5))) continue;  // into the wall, or well back
            const Fixed past = F(1, 20);
            if (Solid(m, ex + r.sx * past, ez + r.cz * past) < r.base + r.height - F(35, 100))
                say("ramp", r.x, r.z, "a trench between its lip and the wall");
        }
        for (const Deck& d : m.decks)
        {
            Fixed top = F(-1000);
            for (int i = -10; i <= 10; ++i)
                for (int j = -10; j <= 10; ++j)   // a hair in: a ramp along its edge is beside it, not under
                {
                    Fixed x, z;
                    DeckPoint(d, d.w * F(i * 49, 1000), d.d * F(j * 49, 1000), x, z);
                    top = Fixed::Max(top, Solid(m, x, z));
                }
            if (top > d.h - DeckThick() - Headroom()) say("deck", d.x, d.z, "no room to drive under it");
        }
        for (const Arc& c : m.arcs)
        {
            if (!(c.length() < Fixed::TwoPi() * c.radius)) say("arc", c.x, c.z, "round onto itself");
            if (!(c.width < c.radius)) say("arc", c.x, c.z, "wider than its radius");
            const Fixed rise = Fixed::Max(c.up.IsPositive() ? c.height / c.up : Fixed::Zero(),
                                          c.down.IsPositive() ? c.height / c.down : Fixed::Zero());
            const Fixed inside = c.radius - c.width * Fixed::Half();
            if (inside.IsPositive() && rise * c.radius / inside > F(7, 10)) say("arc", c.x, c.z, "its inside edge too steep to climb");
        }
        for (const Pit& p : m.pits)
        {
            const Fixed hair = F(1, 50);   // what is laid to a pit's edge is in it
            for (const Spot& a : m.spawns)
                if (Over(a.x, a.z, p.x, p.z, p.w + F(4), p.d + F(4)) && !Over(a.x, a.z, p.x, p.z, p.w - F(4), p.d - F(4)))
                    say("pit", p.x, p.z, "a spawn at its edge");
            for (const Hill& k : m.hills)
            {
                if (Over(k.x, k.z, p.x, p.z, p.w, p.d))
                {
                    if (!Over(k.x, k.z, p.x, p.z, p.w - k.radius * F(2) + hair, p.d - k.radius * F(2) + hair)) say("pit", p.x, p.z, "a hill across its edge");
                    continue;
                }
                const Fixed dx = Fixed::Max((k.x - p.x).Abs() - p.w * Fixed::Half(), Fixed::Zero());
                const Fixed dz = Fixed::Max((k.z - p.z).Abs() - p.d * Fixed::Half(), Fixed::Zero());
                const Fixed back = k.radius * HillSkirt();
                if (dx * dx + dz * dz < back * back) say("pit", p.x, p.z, "a hill too near its edge");
            }
            bool out_by = p.lava;
            auto molten = [&](Fixed x, Fixed z) { return p.lava && Over(x, z, p.x, p.z, p.w, p.d) && Solid(m, x, z) == -p.depth; };
            for (const Spot& a : m.spawns)  if (molten(a.x, a.z)) say("lava", a.x, a.z, "a spawn in it");
            for (const Spot& a : m.pickups) if (!a.up && molten(a.x, a.z)) say("lava", a.x, a.z, "a box in it");
            for (const Pad& q : m.pads)     if (molten(q.x, q.z)) say("lava", q.x, q.z, "a pad in it");
            for (const Spring& q : m.springs) if (molten(q.x, q.z)) say("lava", q.x, q.z, "a spring in it");
            for (const Spring& q : m.springs) out_by = out_by || Over(q.x, q.z, p.x, p.z, p.w, p.d);
            for (const Ramp& r : m.ramps)
            {
                const Fixed hl = r.length() * Fixed::Half(), hw = r.width * Fixed::Half();
                // Its corners, what of it is in a wall cut off: a bank run on
                // into the wall a pit goes up to is in the pit.
                auto cut = [](Fixed v) { return Fixed::Max(-Half(), Fixed::Min(Half(), v)); };
                int in = 0;
                for (int i = -1; i <= 1; i += 2)
                    for (int j = -1; j <= 1; j += 2)
                        in += Over(cut(r.x + r.sx * hl * F(i) + r.cz * hw * F(j)), cut(r.z + r.cz * hl * F(i) - r.sx * hw * F(j)), p.x, p.z, p.w + hair, p.d + hair);
                const bool mine = Over(r.x, r.z, p.x, p.z, p.w, p.d);
                bool touched = in != 0;
                if (!mine && !touched)   // none of its corners in the pit: is any of the pit under it?
                {
                    Fixed h, gx, gz;
                    for (int i = -10; i <= 10 && !touched; ++i)
                        for (int j = -10; j <= 10 && !touched; ++j)
                            touched = RampAt(r, p.x + p.w * F(i, 20), p.z + p.d * F(j, 20), h, gx, gz);
                }
                if (mine ? in != 4 : touched) say("pit", p.x, p.z, "a ramp across its edge");
                out_by = out_by || (mine && in == 4 && !(r.height < p.depth));
            }
            for (const Arc& c : m.arcs)
            {
                // Its middle and both edges, every two units round: all in the pit, or none.
                int in = 0, all = 0;
                for (Fixed t = Fixed::Zero(); !(t > c.length()); t += F(2))
                    for (int e = -1; e <= 1; ++e)
                    {
                        Fixed sn, cs;
                        (c.a0 + F(c.turn) * t / c.radius).SinCos(sn, cs);
                        const Fixed r = c.radius + c.width * F(e, 2);
                        in += Over(c.x + r * sn, c.z + r * cs, p.x, p.z, p.w + hair, p.d + hair);
                        ++all;
                    }
                if (in != 0 && in != all) say("pit", p.x, p.z, "an arc across its edge");
                out_by = out_by || (in == all && !(c.height < p.depth));
            }
            if (!out_by) say("pit", p.x, p.z, "no way out");
        }
        int bare = 0;
        EachBare(m, [&](Fixed x, Fixed z) { if (bare++ < 8) say("bare", x, z, "a stretch of ground with nothing on it"); });
        if (!out.empty()) out.pop_back();
        return out;
    }

    /*
     * A MAP FROM A SEED. First what keeps the arena playable: eight spawns on
     * a ring and the middle's box. Then, from the dice:
     *
     *   the low    one map in two: the ground from one wall to a line short
     *   ground     of or past the middle, sunk -- a third to two thirds of
     *              the arena. Ramps along that line climb out of it, there
     *              are springs in it, and whatever lands in it stands on
     *              its floor
     *   banks      on up to two walls (a ramp up to the wall and on into
     *              it, ridden up and rolled back down from), nothing else
     *              within reach of one
     *   a storey   a deck over a seventh to a third of the arena, on the
     *              high ground, on pillars: a ramp up in each of up to two
     *              of its corners, from its edge in under it, the deck cut
     *              away over the ramp; springs under it and boxes on it
     *
     * and then, anywhere on either level and under the storey: hills; ramps
     * of three kinds -- jumps, decks (up, a level top, down) and small
     * kickers; pads, pointing any of eight ways; springs; blocks; and the
     * power-up boxes. Each is kept clear of everything placed before it and
     * off the low ground's edge, so a ramp never runs into a block and nobody
     * spawns in a wall: a map from any seed is sound (Faults), which
     * loaders/KartMapsTesterLoader holds it to over many seeds. The dice are
     * KartArms::Next, so the same seed is the same map anywhere.
     *
     * The numbers below are of a plan 64 across; the map is then stretched to
     * the arena (Stretch): every position 3x, the LEVELS -- storey, low
     * ground and the ramps between them -- 3x too, so they stay joined, and
     * everything that stands on the floor 2x, so the gaps between things are
     * wider than the plan had them. Heights are not stretched: they are the
     * map's as given, and nothing that may land under the storey is too high
     * to leave room there (Headroom).
     */
    inline Fixed StoreyHeight() { return F(5); }
    inline Fixed LowDepth()     { return F(5, 2); }
    // A map made further across: every position `at` times as far out, the
    // levels -- decks, pits and the ramps marked as between them -- `level`
    // times as big, and what stands on the floor -- blocks, hills, the other
    // ramps -- `thing` times. Not how high anything stands. A pad, a spring,
    // a box and a spawn stay the size they are, further apart.
    inline void Stretch(Map& m, int at, int level, int thing)
    {
        const Fixed f = F(at), lv = F(level), th = F(thing);
        for (Block& b : m.blocks)   { b.x = b.x * f; b.z = b.z * f; b.w = b.w * th; b.d = b.d * th; }
        for (Ramp& r : m.ramps)
        {
            const Fixed g = r.level ? lv : th;
            r.x = r.x * f; r.z = r.z * f; r.width = r.width * g; r.up = r.up * g; r.flat = r.flat * g; r.down = r.down * g;
        }
        for (Hill& h : m.hills)     { h.x = h.x * f; h.z = h.z * f; h.radius = h.radius * th; }
        for (Deck& d : m.decks)     { d.x = d.x * f; d.z = d.z * f; d.w = d.w * lv; d.d = d.d * lv; }
        for (Pit& p : m.pits)       { p.x = p.x * f; p.z = p.z * f; p.w = p.w * lv; p.d = p.d * lv; }
        for (Pad& p : m.pads)       { p.x = p.x * f; p.z = p.z * f; }
        for (Spring& p : m.springs) { p.x = p.x * f; p.z = p.z * f; }
        for (Spot& p : m.spawns)    { p.x = p.x * f; p.z = p.z * f; }
        for (Spot& p : m.pickups)   { p.x = p.x * f; p.z = p.z * f; }
    }
    inline void Generate(uint64_t seed, Map& m)
    {
        m.blocks.clear(); m.ramps.clear(); m.hills.clear(); m.pads.clear(); m.spawns.clear(); m.pickups.clear();
        m.springs.clear(); m.decks.clear(); m.pits.clear();
        m.generated = true;
        const int stretch = static_cast<int>((Half() / F(32)).ToInt());   // positions and levels; things 2x
        const Fixed lip = (Half() + F(1)) / F(stretch);   // the walls' outside, on the plan: a bank's lip is in the wall
        uint64_t r = seed | 1;
        auto pick = [&r](int lo, int hi) { return lo + static_cast<int>(KartArms::Next(r) % static_cast<uint64_t>(hi - lo + 1)); };
        struct Room { Fixed x, z, rad; };
        std::vector<Room> taken;
        // How far out a thing may reach on each side (+x, -x, +z, -z): to the
        // wall, or short of a bank on it.
        Fixed reach[4] = { F(30), F(30), F(30), F(30) };
        // The low ground, where there is any: the side it is on (as `reach`)
        // and the line it runs to, on that side's axis.
        int low = -1;
        Fixed edge;
        // How far a point is into the low ground (+) or short of it (-).
        auto into = [&low, &edge](Fixed x, Fixed z) {
            const Fixed u = low < 2 ? x : z;
            return (low & 1) ? edge - u : u - edge;
        };
        auto clear = [&](Fixed x, Fixed z, Fixed rad, Fixed hill = Fixed::Zero(), Fixed apart = F(3, 2)) {
            if (x + rad > reach[0] || rad - x > reach[1] || z + rad > reach[2] || rad - z > reach[3]) return false;
            if (low >= 0)
            {
                const Fixed in = into(x, z);
                if (in.Abs() < rad + F(4, 5)) return false;                          // across the edge
                if (in.raw < 0 && -in < hill * HillSkirt() + F(1, 2)) return false;   // a hill's dome showing in it
            }
            for (const Room& t : taken)
            {
                const Fixed gap = rad + t.rad + apart;
                if ((x - t.x) * (x - t.x) + (z - t.z) * (z - t.z) < gap * gap) return false;
            }
            return true;
        };
        // A long ramp, as the squares along it: `n` of `width`, about (x, z), the way (sx, cz) runs.
        auto run_clear = [&](Fixed x, Fixed z, Fixed sx, Fixed cz, Fixed width, int n, bool take) {
            const Fixed rad = width * F(71, 100);
            for (int i = 0; i < n; ++i)
            {
                const Fixed at = width * (F(i) - F(n - 1, 2));
                if (take) taken.push_back({ x + sx * at, z + cz * at, rad });
                else if (!clear(x + sx * at, z + cz * at, rad)) return false;
            }
            return true;
        };
        static const int ring[8][2] = { { 0, -20 }, { 0, 20 }, { -20, 0 }, { 20, 0 }, { 14, -14 }, { -14, 14 }, { 14, 14 }, { -14, -14 } };
        for (const auto& p : ring) { m.spawns.push_back({ F(p[0]), F(p[1]) }); taken.push_back({ F(p[0]), F(p[1]), F(2) }); }
        m.pickups.push_back({ Fixed::Zero(), Fixed::Zero() });
        taken.push_back({ Fixed::Zero(), Fixed::Zero(), F(2) });

        // (u, v) -> (x, z): u out toward `side`, v along its wall; and the
        // side a thing faces that runs out along u (0), back (1), or along v
        // either way (2, 3).
        auto place = [](int side, Fixed u, Fixed v, Fixed& x, Fixed& z) {
            const Fixed su = (side & 1) ? -u : u;
            if (side < 2) { x = su; z = v; } else { x = v; z = su; }
        };
        auto facing = [](int side, int way) { return way < 2 ? (side ^ way) : ((side < 2 ? 2 : 0) + (way - 2)); };
        static const int sides[4][2] = { { 90, 0 }, { 270, 1 }, { 0, 2 }, { 180, 3 } };   // a side's yaw: rising toward its wall

        // The low ground, one map in two: from a wall to a line 3 to 8 short
        // of the middle or past it, the whole arena long.
        const int toward = pick(0, 3);           // the side `u` runs out to, low ground or not
        const bool sunk = pick(0, 1) == 1;
        const Fixed line = F(pick(3, 8)) * (pick(0, 1) == 1 ? Fixed::One() : -Fixed::One());
        if (sunk)
        {
            Pit p; p.depth = LowDepth();
            const Fixed mid = (line + F(32)) * Fixed::Half(), wide = F(32) - line;
            place(toward, mid, Fixed::Zero(), p.x, p.z);
            p.w = toward < 2 ? wide : F(64); p.d = toward < 2 ? F(64) : wide;
            m.pits.push_back(p);
        }
        // Banks, on up to two walls -- not one the low ground's edge runs
        // into, which a bank would lie across.
        const int banks = pick(0, 2);
        int first = pick(0, 3);
        for (int b = 0; b < banks; ++b)
        {
            const int side = (first + b * pick(1, 3)) % 4;
            Ramp a; a.yaw = sides[side][0]; a.width = F(pick(20, 34)); a.up = F(pick(5, 7)); a.level = true;
            a.flat = a.down = Fixed::Zero(); a.height = F(pick(20, 30), 10);
            const Fixed at = lip - a.up * Fixed::Half();     // its lip in the wall
            const Fixed along = F(pick(-4, 4));
            if (reach[side] < F(30)) continue;               // the same side twice: one bank
            if (sunk && (side < 2) != (toward < 2)) continue;
            a.x = side == 0 ? at : side == 1 ? -at : along;
            a.z = side == 2 ? at : side == 3 ? -at : along;
            m.ramps.push_back(a);
            reach[side] = lip - a.up - F(1);
        }
        // Out of the low ground: up to four ramps over its line, as high as
        // it is deep; and two springs in it.
        if (sunk)
        {
            const size_t exits = m.ramps.size() + 4;
            for (int t = 0; t < 400 && m.ramps.size() < exits; ++t)
            {
                Ramp a; a.width = F(4); a.up = F(7); a.flat = a.down = Fixed::Zero(); a.height = LowDepth(); a.level = true;
                a.yaw = sides[toward ^ 1][0];    // rising back the other way, out of it
                place(toward, line + a.up * Fixed::Half(), F(pick(-26, 26)), a.x, a.z);
                const Fixed rad = Fixed::Length(a.up, Fixed::Zero(), a.width) * Fixed::Half();
                if (!clear(a.x, a.z, rad)) continue;   // (the edge is not set yet: this one lies up to it)
                m.ramps.push_back(a); taken.push_back({ a.x, a.z, rad });
            }
            low = toward;
            edge = (toward & 1) ? -line : line;
            for (int t = 0; t < 200 && m.springs.size() < 2; ++t)
            {
                Fixed x, z;
                place(toward, line + F(pick(4, 19)), F(pick(-26, 26)), x, z);
                if (!clear(x, z, SpringRadius())) continue;
                m.springs.push_back({ x, z, Fixed::Zero() }); taken.push_back({ x, z, SpringRadius() });
            }
        }
        // The storey, on the high ground: 20 to 30 by 30 to 46, and up to 40
        // wide where no ground is sunk. Up to it, a ramp in each of up to two
        // of its corners: twelve long, from the storey's edge in under it and
        // up to its height, the deck cut away over the ramp -- a stairwell --
        // so a kart drives up through the cut and straight on, onto the deck
        // beyond. The two are on opposite sides, each tried at either end,
        // and the storey is put where both can stand (after 150 tries, where
        // one can; a storey with none is reached by its springs). Its
        // pillars; springs under it; boxes on it.
        {
            const Fixed hi = sunk ? line - F(2) : F(30);   // as far out along u as it may lie
            const Fixed well = F(4), climb = F(12);
            Fixed du, dv, uc, vc;
            struct Well { Ramp ramp; Fixed u, v; };          // the ramp, and the cut's middle
            std::vector<Well> up;
            for (int t = 0; t < 200 && up.size() < (t < 150 ? 2u : 1u); ++t)
            {
                up.clear();
                du = Fixed::Min(F(pick(20, sunk ? 30 : 40)), hi + F(30)); dv = F(pick(30, 46));
                const int room = static_cast<int>(((hi + F(30) - du) * Fixed::Half()).ToInt());
                uc = (hi - F(30)) * Fixed::Half() + F(pick(-room, room));
                const int slack = 25 - static_cast<int>(dv.ToInt()) / 2;
                vc = F(pick(-slack, slack));
                for (int e = 0; e < 2; ++e)
                {
                    // Along the storey's side out along u (0) or back (1), from one end or the other.
                    const Fixed su = e == 0 ? Fixed::One() : -Fixed::One();
                    const int from = pick(0, 1);
                    for (int w = 0; w < 2; ++w)
                    {
                        const Fixed sv = ((from + w) & 1) ? -Fixed::One() : Fixed::One();
                        Well k; Ramp& a = k.ramp;
                        a.width = well; a.up = climb; a.flat = a.down = Fixed::Zero(); a.height = StoreyHeight(); a.level = true;
                        a.yaw = sides[facing(toward, sv.IsPositive() ? 3 : 2)][0];   // rising in from that end
                        k.u = uc + su * (du - well) * Fixed::Half();
                        k.v = vc + sv * (dv - climb) * Fixed::Half();
                        place(toward, k.u, k.v, a.x, a.z);
                        Turn(a.yaw, a.sx, a.cz);
                        // Room for it, and for a kart to come at its foot.
                        Fixed fx, fz;
                        place(toward, k.u, vc + sv * (dv * Fixed::Half() + F(3)), fx, fz);
                        if (!clear(fx, fz, F(2)) || !run_clear(a.x, a.z, a.sx, a.cz, a.width, 3, false)) continue;
                        up.push_back(k); run_clear(a.x, a.z, a.sx, a.cz, a.width, 3, true);
                        break;
                    }
                }
                // Not this place: what its ramps took is given back.
                if (up.size() < (t < 150 ? 2u : 1u) && t < 199) { taken.resize(taken.size() - up.size() * 3); up.clear(); }
            }
            // The deck: the whole of it, less each stairwell.
            struct Plate { Fixed u0, v0, u1, v1; };
            std::vector<Plate> plates{ { uc - du * Fixed::Half(), vc - dv * Fixed::Half(), uc + du * Fixed::Half(), vc + dv * Fixed::Half() } };
            for (const Well& k : up)
            {
                const Fixed u0 = k.u - well * Fixed::Half(), u1 = k.u + well * Fixed::Half();
                const Fixed v0 = k.v - climb * Fixed::Half(), v1 = k.v + climb * Fixed::Half();
                std::vector<Plate> next;
                for (const Plate& q : plates)
                {
                    if (q.u1 <= u0 || q.u0 >= u1 || q.v1 <= v0 || q.v0 >= v1) { next.push_back(q); continue; }
                    if (q.u0 < u0) next.push_back({ q.u0, q.v0, u0, q.v1 });
                    if (q.u1 > u1) next.push_back({ u1, q.v0, q.u1, q.v1 });
                    const Fixed a = Fixed::Max(q.u0, u0), b = Fixed::Min(q.u1, u1);
                    if (q.v0 < v0) next.push_back({ a, q.v0, b, v0 });
                    if (q.v1 > v1) next.push_back({ a, v1, b, q.v1 });
                }
                plates.swap(next);
                m.ramps.push_back(k.ramp);
            }
            for (const Plate& q : plates)
            {
                Deck d; d.h = StoreyHeight();
                place(toward, (q.u0 + q.u1) * Fixed::Half(), (q.v0 + q.v1) * Fixed::Half(), d.x, d.z);
                d.w = toward < 2 ? q.u1 - q.u0 : q.v1 - q.v0; d.d = toward < 2 ? q.v1 - q.v0 : q.u1 - q.u0;
                m.decks.push_back(d);
            }
            Fixed cx, cz;
            place(toward, uc, vc, cx, cz);
            const Fixed w = toward < 2 ? du : dv, d = toward < 2 ? dv : du;
            const Fixed post = F(6, 5);
            for (int i = -1; i <= 1; i += 2)
                for (int j = -1; j <= 1; j += 2)
                {
                    Block b; b.w = b.d = post; b.h = StoreyHeight() - DeckThick() * Fixed::Half(); b.yaw = 0;   // up into the deck
                    b.x = cx + (w * Fixed::Half() - F(3, 2)) * F(i); b.z = cz + (d * Fixed::Half() - F(3, 2)) * F(j);
                    if (!clear(b.x, b.z, post * F(71, 100), Fixed::Zero(), F(1, 4))) continue;
                    m.blocks.push_back(b); taken.push_back({ b.x, b.z, post });
                }
            const size_t springs = m.springs.size() + 2;
            for (int t = 0; t < 200 && m.springs.size() < springs; ++t)
            {
                const Fixed x = cx + w * F(pick(-40, 40), 100), z = cz + d * F(pick(-40, 40), 100);
                if (!clear(x, z, SpringRadius())) continue;
                m.springs.push_back({ x, z, Fixed::Zero() }); taken.push_back({ x, z, SpringRadius() });
            }
            static const int on[3][2] = { { 0, 0 }, { -25, 25 }, { 25, -25 } };
            for (const auto& o : on) m.pickups.push_back({ cx + w * F(o[0], 100), cz + d * F(o[1], 100), true });
        }
        // Then, on either level and under the storey, smallest first.
        const int pads = pick(4, 7);
        for (int t = 0; t < 200 && static_cast<int>(m.pads.size()) < pads; ++t)
        {
            Pad p; p.yaw = pick(0, 7) * 45;
            p.x = F(pick(-24, 24)); p.z = F(pick(-24, 24));
            const Fixed rad = F(5, 2);   // half its diagonal
            if (!clear(p.x, p.z, rad)) continue;
            m.pads.push_back(p); taken.push_back({ p.x, p.z, rad });
        }
        const int springs = static_cast<int>(m.springs.size()) + pick(2, 4);
        for (int t = 0; t < 200 && static_cast<int>(m.springs.size()) < springs; ++t)
        {
            const Fixed x = F(pick(-24, 24)), z = F(pick(-24, 24));
            if (!clear(x, z, SpringRadius())) continue;
            m.springs.push_back({ x, z, Fixed::Zero() }); taken.push_back({ x, z, SpringRadius() });
        }
        const int hills = pick(1, 3);
        for (int t = 0; t < 200 && static_cast<int>(m.hills.size()) < hills; ++t)
        {
            Hill k; k.radius = F(pick(5, 8)); k.height = F(pick(14, 24), 10);
            k.x = F(pick(-24, 24)); k.z = F(pick(-24, 24));
            // Its dome, drawn on under the ground twice its radius out, kept over the floor (Half + 4).
            const Fixed floor = ((Half() + F(4)) - k.radius * F(4)) / F(stretch);   // 2r at twice the size
            if (k.x.Abs() > floor || k.z.Abs() > floor) continue;
            if (!clear(k.x, k.z, k.radius, k.radius)) continue;
            m.hills.push_back(k); taken.push_back({ k.x, k.z, k.radius });
        }
        const int blocks = static_cast<int>(m.blocks.size()) + pick(3, 7);
        for (int t = 0; t < 200 && static_cast<int>(m.blocks.size()) < blocks; ++t)
        {
            Block b; b.w = F(pick(2, 7)); b.d = F(pick(2, 7)); b.h = F(pick(14, 40), 10);
            b.yaw = pick(0, 3) == 0 ? 45 : 0;
            b.x = F(pick(-24, 24)); b.z = F(pick(-24, 24));
            const Fixed rad = (b.w + b.d) * Fixed::Half();
            if (!clear(b.x, b.z, rad)) continue;
            m.blocks.push_back(b); taken.push_back({ b.x, b.z, rad });
        }
        const int ramps = static_cast<int>(m.ramps.size()) + pick(2, 4);
        for (int t = 0; t < 200 && static_cast<int>(m.ramps.size()) < ramps; ++t)
        {
            Ramp a; a.yaw = pick(0, 7) * 45;
            switch (pick(0, 2))
            {
                case 0:   // a jump
                    a.width = F(pick(3, 5)); a.up = F(pick(5, 7)); a.flat = a.down = Fixed::Zero();
                    a.height = F(pick(15, 27), 10); break;
                case 1:   // a deck
                    a.width = F(pick(4, 6)); a.up = F(pick(4, 6)); a.flat = F(pick(3, 6)); a.down = F(pick(4, 6));
                    a.height = F(pick(16, 26), 10); break;
                default:  // a kicker
                    a.width = F(3); a.up = F(pick(3, 4)); a.flat = a.down = Fixed::Zero();
                    a.height = F(pick(12, 18), 10); break;
            }
            a.x = F(pick(-22, 22)); a.z = F(pick(-22, 22));
            const Fixed rad = a.length() * Fixed::Half() + a.width * Fixed::Half();
            if (!clear(a.x, a.z, rad)) continue;
            m.ramps.push_back(a); taken.push_back({ a.x, a.z, rad });
        }
        for (int t = 0; t < 200 && m.pickups.size() < 11; ++t)
        {
            const Fixed x = F(pick(-24, 24)), z = F(pick(-24, 24));
            if (!clear(x, z, F(1))) continue;
            m.pickups.push_back({ x, z }); taken.push_back({ x, z, F(1) });
        }
        Stretch(m, stretch, stretch, 2);
        // No bare stretches: where the dice left one, something from them --
        // a bump to hop, a pad (any of the four ways), or a box. Each is far
        // from everything else there (that is what bare means), so it stands
        // on one level piece and crosses nothing.
        EachBare(m, [&](Fixed x, Fixed z) {
            if (Nearest(m, x, z) <= BareReach()) return;   // the last one laid covers this
            bool spawn = false;
            for (const Spot& a : m.spawns) spawn = spawn || ((a.x - x).Abs() < F(12) && (a.z - z).Abs() < F(12));
            // Within what the pools hold (kart_pool.etcs: ten domes, sixteen pads, twenty boxes).
            int kind = spawn ? 2 : pick(0, 2);
            if (kind == 0 && m.hills.size() >= 10) kind = 1;
            if (kind == 1 && m.pads.size() >= 16) kind = 2;
            if (kind == 2 && m.pickups.size() >= 20) kind = m.pads.size() < 16 && !spawn ? 1 : 0;
            switch (kind)
            {
                case 0: { Hill k; k.x = x; k.z = z; k.radius = F(pick(5, 7)); k.height = F(pick(8, 14), 10); m.hills.push_back(k); break; }
                case 1: { Pad p; p.x = x; p.z = z; p.yaw = pick(0, 3) * 90; m.pads.push_back(p); break; }
                default: m.pickups.push_back({ x, z }); break;
            }
        });
        Prepare(m);
    }
}

#endif // KARTPROVIDER_KARTARENA_H__
