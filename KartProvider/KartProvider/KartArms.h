#ifndef KARTPROVIDER_KARTARMS_H__
#define KARTPROVIDER_KARTARMS_H__

#include "../../../ontology.h"
#include <cstdint>

/*
 * ── KartArms: what a power-up hands you, and what it does ─────────────────
 *
 * DAMAGE FOLLOWS HOW HARD A HIT IS TO LAND. A kart has 100 points (10.0 on
 * the card); a weapon that sprays does little per hit, one that has to be
 * placed or aimed does more, and the one shot that must be dead on (the
 * sniper) takes a whole kart:
 *
 *   rocket    2 shots: a fast straight rocket that bursts on what it meets,
 *             up to 40 at the centre of the burst, less further out
 *   mine      2: dropped behind, armed in half a second, 30 to who drives on it
 *   shotgun   2: five pellets in a short fan, 10 each
 *   boost     1: two seconds of a faster kart; ramming at speed is 20
 *   bomb      2: tossed up and forward, bursts where it lands, up to 40
 *   minigun   30 rounds while Space is held, 4 each
 *   fuse      1: lit on the kart, bursts round it after 2.5 s, up to 50 --
 *             not to the kart that carries it
 *   sniper    1: an instant line, 100 dead on, 50 at the edge of a kart
 *
 * Nothing hurts the kart that fired it. Every number is the arena's, in the
 * rows' own integers (Fixed), so every runtime lands the same hits.
 */
namespace KartArms
{
    enum Weapon : uint8_t { None, Rocket, Mine, Shotgun, Boost, Bomb, Minigun, Fuse, Sniper, Count };

    inline const char* Name(uint8_t w)
    {
        static const char* names[] = { "-", "rocket", "mine", "shotgun", "boost", "bomb", "minigun", "fuse", "sniper" };
        return w < Count ? names[w] : "-";
    }
    inline uint8_t Ammo(uint8_t w)
    {
        static const uint8_t ammo[] = { 0, 2, 2, 2, 1, 2, 30, 1, 1 };
        return w < Count ? ammo[w] : 0;
    }
    inline uint8_t ByName(const std::string& n)
    {
        for (uint8_t w = 1; w < Count; ++w) if (n == Name(w)) return w;
        return None;
    }

    // What flies (or lies) about the arena: the shots in the air, a mine on
    // the ground, a lit fuse on a kart, a sniper's line for a moment.
    enum Shot : uint8_t { RocketShot, Pellet, Bullet, BombShot, MineShot, FuseShot, Tracer };

    // A burst: `max` at its centre, nothing at `radius`, straight between --
    // measured from the burst to the nearest of a kart (its centre less its
    // reach), so a direct hit is the full amount.
    inline int Falloff(Fixed d, Fixed radius, int max)
    {
        if (!(d < radius)) return 0;
        if (d.raw < 0) d = Fixed::Zero();
        const Fixed v = Fixed::FromInt(max) * (radius - d) / radius;
        return static_cast<int>(v.raw >> 32);
    }

    // The arena's dice: xorshift64*, its state a line of the record's to
    // seed, so a power-up rolls the same weapon on every runtime.
    inline uint64_t Next(uint64_t& s)
    {
        s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
        return s * 2685821657736338717ull;
    }
}

#endif // KARTPROVIDER_KARTARMS_H__
