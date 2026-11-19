#ifndef KARTPROVIDER_KARTARENA_H__
#define KARTPROVIDER_KARTARENA_H__

#include "../../../ontology.h"
#include "KartArms.h"

#include <cstdint>
#include <cstdio>
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
 * A PAD lies on the ground -- level, or one slope: a bank's -- and pushes
 * what drives over it the way it points (yaw, as a ramp's): PadWidth() across,
 * PadLength() along. A SPRING is a round one, SpringRadius() about its middle,
 * on level ground, that throws a kart up.
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
    struct Spring { Fixed x, z; Fixed y; };          // y derived, as a pad's
    struct Deck   { Fixed x, z, w, d, h; };          // its top at h, w along x, d along z
    struct Pit    { Fixed x, z, w, d, depth; bool lava = false; };   // its floor at -depth; lava: a kart on that floor is out
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
            if (HillAt(k, x, z, h, gx, gz) && (!stood || h + k.base > best)) { best = h + k.base; bgx = gx; bgz = gz; on = OnHill; stood = true; }
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
        auto floor = [&m](Fixed x, Fixed z) { const Pit* p = PitAt(m, x, z); return p ? -p->depth : Fixed::Zero(); };
        for (Ramp& r : m.ramps)   r.base = floor(r.x, r.z);
        for (Block& b : m.blocks) b.base = floor(b.x, b.z);
        for (Hill& h : m.hills)   h.base = floor(h.x, h.z);
        const Fixed half_root3 = F(3).Sqrt() / F(2);   // where an ellipsoid twice as wide meets the ground
        for (Hill& h : m.hills)
        {
            h.a  = h.radius * F(2);
            h.b  = h.height / (Fixed::One() - half_root3);
            h.y0 = -h.b * half_root3;
        }
        for (Pad& p : m.pads)       p.y = Solid(m, p.x, p.z, &p.gx, &p.gz);
        for (Spring& p : m.springs) p.y = Solid(m, p.x, p.z);
    }

    /*
     * WHAT IS WRONG WITH A MAP, a fault a line, "" for a sound one (Prepare it
     * first). A map's maker -- a script's author, or Generate -- keeps these
     * so that what is on it can be reached and does what it shows:
     *
     *   pad      solid ground in one piece under the whole plate -- level, or
     *            one slope; its middle clear of blocks; no other pad over
     *            it, no spawn at it
     *   spring   the same on level ground, and no pad at it
     *   block    level solid ground under it: not on a ramp or a hill, and
     *            wholly in a pit or wholly out
     *   deck     room to drive under it, over the ground there (a block
     *            under one is a pillar)
     *   pit      a ramp or a hill wholly in it or wholly out, and a hill
     *            outside it well back (its dome is drawn on under the
     *            ground and would show in the pit); no spawn at its edge;
     *            a way out -- a spring in it, or a ramp in it as high as
     *            it is deep
     *   lava     a pit that needs no way out; no spawn, box, pad or spring
     *            on its floor
     */
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
            plate("spring", p.x, p.z, Fixed::Zero(), Fixed::One(), SpringRadius(), SpringRadius(), true, nullptr);
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
        for (const Deck& d : m.decks)
        {
            Fixed top = F(-1000);
            for (int i = -10; i <= 10; ++i)
                for (int j = -10; j <= 10; ++j)   // a hair in: a ramp along its edge is beside it, not under
                    top = Fixed::Max(top, Solid(m, d.x + d.w * F(i * 49, 1000), d.z + d.d * F(j * 49, 1000)));
            if (top > d.h - DeckThick() - Headroom()) say("deck", d.x, d.z, "no room to drive under it");
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
                int in = 0;
                for (int i = -1; i <= 1; i += 2)
                    for (int j = -1; j <= 1; j += 2)
                        in += Over(r.x + r.sx * hl * F(i) + r.cz * hw * F(j), r.z + r.cz * hl * F(i) - r.sx * hw * F(j), p.x, p.z, p.w + hair, p.d + hair);
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
            if (!out_by) say("pit", p.x, p.z, "no way out");
        }
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
     *   banks      on up to two walls (a ramp up to the wall, ridden up and
     *              rolled back down from), nothing else within reach of one
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
        const Fixed lip = (Half() - F(1)) / F(stretch);   // the karts' bound, on the plan
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
            const Fixed at = lip - a.up * Fixed::Half();     // its lip on the karts' bound
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
        Prepare(m);
    }
}

#endif // KARTPROVIDER_KARTARENA_H__
