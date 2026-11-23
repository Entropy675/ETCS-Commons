#ifndef GOLFPROVIDER_GOLFCOURSE_H__
#define GOLFPROVIDER_GOLFCOURSE_H__

#include "../../../ontology/Fixed.h"
#include "../../../ontology/OrderVector.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

/*
 * ── GolfCourse: a course as data ───────────────────────────────────────────
 *
 * A COURSE IS ITS HOLES, in order, and a hole is what a ball meets: SLABS
 * (boxes: a green, a terrace, a slope turned about its own x, a wall, a block,
 * a patch of sand), a CUP in a green, a TEE, and the things the kart's arena
 * has too -- PADS that push a ball the way they point, SPRINGS that throw it
 * up, and power-up SPOTS. All of it Fixed, so a hole laid from the same data
 * is the same hole on every runtime; a course made from a seed (Generate) is
 * the same data everywhere too, and only its seed is a line of the record.
 *
 * ONE HOLE IS LAID AT A TIME, about its own tee (the match places the pooled
 * nodes for the hole in play): every hole's coordinates are its own, the tee
 * near the origin, play running toward +z.
 *
 * THE CUP CUTS ITS GREEN. A course says where the cup is; the level green
 * under it is split round a square gap (CupGap) so the hole is a real hole --
 * a slow ball drops in, a fast one rides over the lip or hits the far side.
 * The cup itself is a container under the gap (golf_cup.etcs): a ball that
 * drops in fits its space, and fit taking it in is what "holed" means.
 *
 * Things stand ON a slab, found by casting down (TopAt): a tee, a pad, a
 * spring, a spot each sit on whatever is highest under them. A pad or a
 * spring acts only on a ball touching the ground at its own height.
 */
namespace GolfCourse
{
inline Fixed F(int64_t n, int64_t d = 1) { return Fixed::FromInt(n) / Fixed::FromInt(d); }

enum Kind : int { Green = 0, Wall = 1, Sand = 2 };

// A box: its middle, its full size, turned by yaw about +y and then tilted
// about its own x (each kept as a sine and a cosine), and what it is.
struct Slab
{
    Fixed x, y, z, w, h, d;
    Fixed ys = Fixed::Zero(), yc = Fixed::One();   // the yaw's
    Fixed ts = Fixed::Zero(), tc = Fixed::One();   // the tilt's
    int   kind = Green;
    bool  level() const { return ts.IsZero() && ys.IsZero(); }
};
struct Pad    { Fixed x, z, y; int yaw = 0; Fixed sx, cz; };   // (sx, cz) the way it pushes
struct Spot   { Fixed x, z, y; };
struct Hole
{
    std::vector<Slab> slabs;
    Fixed cup_x, cup_z, cup_y;   bool cup = false;   // cup_y: the green's top there
    Fixed tee_x, tee_z, tee_y;
    int   par = 3;
    std::vector<Pad>  pads;
    std::vector<Spot> springs, pickups;
    Fixed low;                                       // the lowest of it: under this, a ball is lost
    bool  settled = false;                           // Settle has run
};
// What a course looks like -- its turf, its walls, its sand and the sky over
// it (rgb each): drawn, never played, so floats and no part of a hash.
struct Theme
{
    float turf[3] = { 0.22f, 0.62f, 0.28f }, wall[3] = { 0.52f, 0.36f, 0.22f };
    float sand[3] = { 0.78f, 0.70f, 0.45f }, sky[3]  = { 0.45f, 0.62f, 0.85f };
};
struct Course { std::string name; bool generated = false; std::vector<Hole> holes; Theme theme; };

inline Fixed CupGap()       { return F(7, 10); }
inline Fixed CupUnder()     { return F(3, 4); }     // the cup's middle under the green's top
inline Fixed PadWidth()     { return F(2); }
inline Fixed PadLength()    { return F(3); }
inline Fixed SpringRadius() { return F(1); }
inline Fixed SpotReach()    { return F(9, 10); }
inline Fixed LostBelow()    { return F(8); }

// Degrees as a sine and a cosine, for the verbs that state them.
inline void Degrees(Fixed deg, Fixed& s, Fixed& c) { (deg * Fixed::Pi() / F(180)).SinCos(s, c); }

// The slab's turn as a quaternion on rows: yaw x tilt, each from half its angle.
inline void HalfOf(Fixed s, Fixed c, Fixed& hs, Fixed& hc)
{
    hc = ((Fixed::One() + c) * Fixed::Half()).Sqrt();
    hs = ((Fixed::One() - c) * Fixed::Half()).Sqrt();
    if (s.raw < 0) hs = -hs;
}
inline void Turn(const Slab& b, OrderVector& r)
{
    Fixed yh, yw, th, tw;
    HalfOf(b.ys, b.yc, yh, yw);
    HalfOf(b.ts, b.tc, th, tw);
    r.qw = yw * tw; r.qx = yw * th; r.qy = yh * tw; r.qz = -(yh * th);
}

// Where a line straight down from (x, from, z) first meets the slab: its y.
inline bool DownOnto(const Slab& b, Fixed x, Fixed z, Fixed from, Fixed& y)
{
    OrderVector r;
    Turn(b, r);
    Fixed px = x - b.x, py = from - b.y, pz = z - b.z, dx = Fixed::Zero(), dy = -Fixed::One(), dz = Fixed::Zero();
    r.UnrotateVector(px, py, pz);
    r.UnrotateVector(dx, dy, dz);
    const Fixed p[3] = { px, py, pz }, d[3] = { dx, dy, dz };
    const Fixed hl[3] = { b.w * Fixed::Half(), b.h * Fixed::Half(), b.d * Fixed::Half() };
    Fixed in = Fixed::Zero(), out = F(100000);
    for (int k = 0; k < 3; ++k)
    {
        if (d[k].Abs() < F(1, 100000)) { if (p[k].Abs() > hl[k]) return false; continue; }
        Fixed t0 = (-hl[k] - p[k]) / d[k], t1 = (hl[k] - p[k]) / d[k];
        if (t0 > t1) std::swap(t0, t1);
        in = Fixed::Max(in, t0); out = Fixed::Min(out, t1);
        if (in > out) return false;
    }
    y = from - in;
    return true;
}
// Is (x, y, z) inside the slab?
inline bool Inside(const Slab& b, Fixed x, Fixed y, Fixed z)
{
    OrderVector r;
    Turn(b, r);
    Fixed px = x - b.x, py = y - b.y, pz = z - b.z;
    r.UnrotateVector(px, py, pz);
    return px.Abs() <= b.w * Fixed::Half() && py.Abs() <= b.h * Fixed::Half() && pz.Abs() <= b.d * Fixed::Half();
}
// The highest surface under (x, z) at or below `from`; null when nothing is.
inline const Slab* TopAt(const Hole& h, Fixed x, Fixed z, Fixed& y, Fixed from = F(1000))
{
    const Slab* on = nullptr;
    for (const Slab& b : h.slabs)
    {
        Fixed at;
        if (DownOnto(b, x, z, from, at) && (!on || at > y)) { y = at; on = &b; }
    }
    return on;
}

// The level green under the cup, split round a CupGap square about it.
inline bool CutCup(Hole& h)
{
    if (!h.cup) return false;
    const Fixed g = CupGap() * Fixed::Half();
    for (size_t i = 0; i < h.slabs.size(); ++i)
    {
        const Slab b = h.slabs[i];
        if (b.kind != Green || !b.level()) continue;
        const Fixed x0 = b.x - b.w * Fixed::Half(), x1 = b.x + b.w * Fixed::Half();
        const Fixed z0 = b.z - b.d * Fixed::Half(), z1 = b.z + b.d * Fixed::Half();
        const Fixed top = b.y + b.h * Fixed::Half();
        if (h.cup_x - g <= x0 || h.cup_x + g >= x1 || h.cup_z - g <= z0 || h.cup_z + g >= z1) continue;
        if ((top - h.cup_y).Abs() > F(1, 100)) continue;
        auto piece = [&](Fixed ax, Fixed bx, Fixed az, Fixed bz) {
            Slab s = b;
            s.x = (ax + bx) * Fixed::Half(); s.w = bx - ax;
            s.z = (az + bz) * Fixed::Half(); s.d = bz - az;
            return s;
        };
        h.slabs.erase(h.slabs.begin() + static_cast<long>(i));
        h.slabs.push_back(piece(x0, x1, z0, h.cup_z - g));
        h.slabs.push_back(piece(x0, x1, h.cup_z + g, z1));
        h.slabs.push_back(piece(x0, h.cup_x - g, h.cup_z - g, h.cup_z + g));
        h.slabs.push_back(piece(h.cup_x + g, x1, h.cup_z - g, h.cup_z + g));
        return true;
    }
    return false;
}

// After a hole is stated: its cup cut, its things on their surfaces, its floor.
inline void Settle(Hole& h)
{
    if (h.cup) TopAt(h, h.cup_x, h.cup_z, h.cup_y);
    CutCup(h);
    TopAt(h, h.tee_x, h.tee_z, h.tee_y);
    for (Pad& p : h.pads)
    {
        TopAt(h, p.x, p.z, p.y);
        Fixed s, c; Degrees(F(p.yaw), s, c);
        p.sx = s; p.cz = c;   // yaw 0 pushes toward +z, 90 toward +x
    }
    for (Spot& p : h.springs) TopAt(h, p.x, p.z, p.y);
    for (Spot& p : h.pickups) TopAt(h, p.x, p.z, p.y);
    Fixed low = F(1000);
    for (const Slab& b : h.slabs) low = Fixed::Min(low, b.y - (b.h + b.d) * Fixed::Half());
    h.low = (h.slabs.empty() ? Fixed::Zero() : low) - LostBelow();
    h.settled = true;
}

// Is there ground all the way from the tee to the cup? In half-unit steps,
// each at most a quarter up or down (a slope of one in two): a slope that
// does not meet its level at either end, a wall right across, a gap -- a
// hole laid wrong -- has no such way. A spring, a drop or a lofted shot may
// find another -- and a hole with a pad or a spring on it may be made to
// need one (a wall to clear, a gap to be flung across), so only a hole with
// neither is held to this (Faults).
inline bool Walks(const Hole& h)
{
    Fixed x0 = F(100000), x1 = -x0, z0 = x0, z1 = -x0;
    for (const Slab& b : h.slabs)
    {
        const Fixed r = (b.w + b.d) * Fixed::Half();
        x0 = Fixed::Min(x0, b.x - r); x1 = Fixed::Max(x1, b.x + r);
        z0 = Fixed::Min(z0, b.z - r); z1 = Fixed::Max(z1, b.z + r);
    }
    const Fixed cell = Fixed::Half();
    const int nx = static_cast<int>(((x1 - x0) / cell).ToInt()) + 1, nz = static_cast<int>(((z1 - z0) / cell).ToInt()) + 1;
    if (h.slabs.empty() || nx <= 0 || nz <= 0 || nx > 600 || nz > 600) return false;
    auto at = [&](Fixed x, Fixed z) { return static_cast<int>(((z - z0) / cell).ToInt()) * nx + static_cast<int>(((x - x0) / cell).ToInt()); };
    const Fixed none = F(-100000);
    std::vector<Fixed> top(static_cast<size_t>(nx) * nz, none);
    std::vector<char> seen(top.size(), 0);
    for (int k = 0; k < nz; ++k)
        for (int i = 0; i < nx; ++i)
        {
            Fixed y;
            if (TopAt(h, x0 + cell * F(i) + cell * Fixed::Half(), z0 + cell * F(k) + cell * Fixed::Half(), y)) top[static_cast<size_t>(k) * nx + i] = y;
        }
    const int tee = at(h.tee_x, h.tee_z), cup = at(h.cup_x, h.cup_z);
    if (tee < 0 || tee >= static_cast<int>(top.size()) || cup < 0 || cup >= static_cast<int>(top.size())) return false;
    std::vector<int> open{ tee };
    seen[static_cast<size_t>(tee)] = 1;
    while (!open.empty())
    {
        const int c = open.back(); open.pop_back();
        // The cup's cell is the gap: next to it, at its green's height, is at it.
        if (c == cup) return true;
        const int ci = c % nx, ck = c / nx;
        static const int step[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
        for (const auto& s : step)
        {
            const int i = ci + s[0], k = ck + s[1];
            if (i < 0 || k < 0 || i >= nx || k >= nz) continue;
            const size_t n = static_cast<size_t>(k) * nx + i;
            if (seen[n]) continue;
            const bool hole = static_cast<int>(n) == cup && top[c] > h.cup_y - F(1, 4);
            if (!hole && (top[n] == none || (top[n] - top[c]).Abs() > F(1, 4))) continue;
            seen[n] = 1;
            open.push_back(static_cast<int>(n));
        }
    }
    return false;
}

// What is wrong with a course, "none" when nothing: what the match will not lay.
inline std::string Faults(const Course& c)
{
    std::string out;
    auto say = [&](size_t n, const std::string& what) { out += (out.empty() ? "" : "; ") + c.name + " hole " + std::to_string(n + 1) + ": " + what; };
    if (c.holes.empty()) out = c.name + ": no holes";
    for (size_t n = 0; n < c.holes.size(); ++n)
    {
        const Hole& h = c.holes[n];
        Fixed y;
        if (!h.cup) say(n, "no cup");
        // Settled, a cut hole has nothing at the cup's middle to fall on.
        else if (TopAt(h, h.cup_x, h.cup_z, y, h.cup_y + F(1, 10)) && y > h.cup_y - CupUnder()) say(n, "the cup is not cut into a level green");
        if (!TopAt(h, h.tee_x, h.tee_z, y)) say(n, "the tee stands on nothing");
        for (const Pad& p : h.pads)    if (!TopAt(h, p.x, p.z, y)) say(n, "a pad stands on nothing");
        for (const Spot& p : h.springs) if (!TopAt(h, p.x, p.z, y)) say(n, "a spring stands on nothing");
        for (const Spot& p : h.pickups) if (!TopAt(h, p.x, p.z, y)) say(n, "a spot stands on nothing");
        if (h.cup && h.pads.empty() && h.springs.empty() && !Walks(h)) say(n, "no ground all the way from the tee to the cup, and nothing to throw a ball");
    }
    return out.empty() ? "none" : out;
}

/*
 * A COURSE FROM A SEED: four holes, each a walled lane from a tee to a cup,
 * some with a terrace up or down a slope across the whole lane, blocks to
 * play round, sand, pads pointing up the lane, a spring before a block and
 * spots for power-ups. The same seed is the same course everywhere.
 */
struct Dice
{
    uint64_t s;
    explicit Dice(uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    uint64_t next() { s += 0x9E3779B97F4A7C15ull; uint64_t z = s; z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull; return z ^ (z >> 31); }
    int64_t in(int64_t lo, int64_t hi) { return lo + static_cast<int64_t>(next() % static_cast<uint64_t>(hi - lo + 1)); }   // lo..hi
    bool chance(int pct) { return static_cast<int>(next() % 100) < pct; }
};
inline Slab Box(Fixed x, Fixed y, Fixed z, Fixed w, Fixed h, Fixed d, int kind)
{
    Slab b; b.x = x; b.y = y; b.z = z; b.w = w; b.h = h; b.d = d; b.kind = kind;
    return b;
}
inline Hole GenerateHole(Dice& r)
{
    Hole h;
    const int64_t L = r.in(44, 72), W = 2 * r.in(7, 11);
    const Fixed half = F(W, 2), len = F(L);
    // The lane's levels: the tee's at 0, and maybe a terrace at -lift or +lift
    // from `at` on, down (or up) a slope `run` long across the whole lane.
    const bool terrace = r.chance(55), up = terrace && r.chance(40);
    const Fixed lift = terrace ? F(3 + r.in(0, 3), 2) : Fixed::Zero();     // 1.5 .. 3
    const Fixed far = up ? lift : -lift;                                   // the far level
    const Fixed run = up ? lift * F(5) : Fixed::Max(F(8), lift * F(4));
    const Fixed at = terrace ? len * F(r.in(30, 48), 100) : len;
    const Fixed lo = Fixed::Min(Fixed::Zero(), far), hi = Fixed::Max(Fixed::Zero(), far);
    // Greens a unit under their top at least, and down to the lowest level's.
    auto green = [&](Fixed z0, Fixed z1, Fixed top) {
        const Fixed bottom = lo - F(1);
        h.slabs.push_back(Box(Fixed::Zero(), (top + bottom) * Fixed::Half(), (z0 + z1) * Fixed::Half(), F(W), top - bottom, z1 - z0, Green));
    };
    green(F(-4), at, Fixed::Zero());
    if (terrace)
    {
        green(at + run, len, far);
        // The slope: its top edge on the near level, its bottom on the far.
        const Fixed rise = far, slen = Fixed::Length(run, rise, Fixed::Zero());
        Slab s = Box(Fixed::Zero(), Fixed::Zero(), Fixed::Zero(), F(W), F(1), slen, Green);
        s.ts = -rise / slen; s.tc = run / slen;            // + tilt runs down toward +z
        s.x = Fixed::Zero();
        s.y = rise * Fixed::Half() - Fixed::Half() * s.tc;
        s.z = at + run * Fixed::Half() - Fixed::Half() * s.ts;
        h.slabs.push_back(s);
    }
    // The walls: the lane's sides and ends, from under its lowest level to
    // over its highest.
    const Fixed wb = lo - F(1), wt = hi + F(3, 4), wh = wt - wb, wy = (wt + wb) * Fixed::Half();
    h.slabs.push_back(Box(-(half + F(3, 10)), wy, (len - F(4)) * Fixed::Half(), F(6, 10), wh, len + F(52, 10), Wall));
    h.slabs.push_back(Box(half + F(3, 10), wy, (len - F(4)) * Fixed::Half(), F(6, 10), wh, len + F(52, 10), Wall));
    h.slabs.push_back(Box(Fixed::Zero(), wy, F(-43, 10), F(W), wh, F(6, 10), Wall));
    h.slabs.push_back(Box(Fixed::Zero(), wy, len + F(3, 10), F(W), wh, F(6, 10), Wall));
    // Where things may go: a level part, inside the walls, clear of the
    // tee, the cup and each other.
    h.tee_x = Fixed::Zero(); h.tee_z = Fixed::Zero();
    h.cup = true;
    h.cup_x = F(r.in(-(W / 2 - 3), W / 2 - 3));
    h.cup_z = len - F(r.in(6, 10));
    struct Taken { Fixed x, z, r; };
    std::vector<Taken> taken = { { h.tee_x, h.tee_z, F(4) }, { h.cup_x, h.cup_z, F(4) } };
    auto levelAt = [&](Fixed z) { return !terrace || z < at - F(1) || z > at + run + F(1); };
    auto free = [&](Fixed x, Fixed z, Fixed rad) {
        if (x.Abs() + rad > half - F(1, 2) || z - rad < F(-35, 10) || z + rad > len - F(1, 2) || !levelAt(z - rad) || !levelAt(z + rad)) return false;
        for (const Taken& t : taken) if (Fixed::Length(x - t.x, Fixed::Zero(), z - t.z) < rad + t.r) return false;
        return true;
    };
    auto place = [&](Fixed rad, Fixed zlo, Fixed& x, Fixed& z) {
        for (int k = 0; k < 40; ++k)
        {
            x = F(r.in(-(W / 2 - 1) * 10, (W / 2 - 1) * 10), 10);
            z = F(r.in(zlo.ToInt() * 10, (L - 2) * 10), 10);
            if (free(x, z, rad)) { taken.push_back({ x, z, rad }); return true; }
        }
        return false;
    };
    auto topOf = [&](Fixed z) { return !terrace || z < at ? Fixed::Zero() : far; };
    // Blocks to play round.
    const int blocks = static_cast<int>(r.in(1, 2) + (L > 60 ? 1 : 0));
    std::vector<Taken> block_at;
    for (int k = 0; k < blocks; ++k)
    {
        Fixed x, z;
        const Fixed bw = F(r.in(24, 40), 10), bd = F(r.in(24, 40), 10), bh = F(r.in(5, 10), 10);
        if (!place(Fixed::Length(bw, Fixed::Zero(), bd) * Fixed::Half(), F(8), x, z)) continue;
        Slab b = Box(x, topOf(z) + bh * Fixed::Half(), z, bw, bh, bd, Wall);
        if (r.chance(35)) { Fixed s, c; Degrees(F(45), s, c); b.ys = s; b.yc = c; }
        h.slabs.push_back(b);
        block_at.push_back({ x, z, Fixed::Zero() });
    }
    // Sand: a thin patch over the green.
    if (r.chance(50))
    {
        Fixed x, z;
        const Fixed sw = F(r.in(4, 6)), sd = F(r.in(3, 5));
        if (place(Fixed::Length(sw, Fixed::Zero(), sd) * Fixed::Half(), F(6), x, z))
            h.slabs.push_back(Box(x, topOf(z) + F(3, 100), z, sw, F(6, 100), sd, Sand));
    }
    // A spring before the first block, in line from the tee.
    if (!block_at.empty() && r.chance(40))
    {
        const Taken& b = block_at.front();
        const Fixed x = b.x * F(6, 10), z = b.z - F(5);
        if (free(x, z, F(3, 2))) { taken.push_back({ x, z, F(3, 2) }); h.springs.push_back({ x, z, Fixed::Zero() }); }
    }
    // Pads up the lane, turned a little toward the cup.
    const int pads = static_cast<int>(r.in(0, 2));
    for (int k = 0; k < pads; ++k)
    {
        Fixed x, z;
        if (!place(F(2), F(5), x, z)) continue;
        Pad p; p.x = x; p.z = z; p.y = Fixed::Zero();
        p.yaw = static_cast<int>(r.in(-25, 25));
        h.pads.push_back(p);
    }
    // Spots for power-ups.
    const int spots = static_cast<int>(r.in(2, 3));
    for (int k = 0; k < spots; ++k)
    {
        Fixed x, z;
        if (place(F(3, 2), F(4), x, z)) h.pickups.push_back({ x, z, Fixed::Zero() });
    }
    h.par = L > 60 || terrace ? 4 : 3;
    Settle(h);
    return h;
}
inline Course Generate(uint64_t seed, const std::string& name)
{
    Course c; c.name = name; c.generated = true;
    // Its look by its seed, of a few: a course from a seed is told from the last.
    static const Theme looks[4] = {
        Theme{},
        Theme{ { 0.30f, 0.58f, 0.50f }, { 0.36f, 0.38f, 0.46f }, { 0.84f, 0.83f, 0.76f }, { 0.44f, 0.38f, 0.64f } },
        Theme{ { 0.74f, 0.47f, 0.31f }, { 0.17f, 0.36f, 0.40f }, { 0.93f, 0.86f, 0.66f }, { 0.93f, 0.76f, 0.54f } },
        Theme{ { 0.42f, 0.46f, 0.70f }, { 0.20f, 0.20f, 0.30f }, { 0.80f, 0.78f, 0.86f }, { 0.08f, 0.09f, 0.18f } } };
    c.theme = looks[seed % 4];
    Dice r(seed);
    for (int n = 0; n < 4; ++n) c.holes.push_back(GenerateHole(r));
    return c;
}
} // namespace GolfCourse

#endif // GOLFPROVIDER_GOLFCOURSE_H__
