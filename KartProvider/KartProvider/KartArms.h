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
 *             up to 40 at the centre of the burst, less further out; it
 *             flies until it has crossed the arena
 *   mine      2: dropped behind, armed in half a second, 30 to who drives on
 *             it; left alone it blinks and bursts by itself after twenty seconds
 *   shotgun   2: five pellets in a short fan, 10 each
 *   boost     1: two seconds of a faster kart; ramming at speed is 20
 *   bomb      2: tossed up and forward, bursts where it lands, up to 40
 *   minigun   30 rounds while Space is held, 4 each, each flying until it
 *             has crossed the arena
 *   fuse      1: lit on the kart, bursts round it after 2.5 s, up to 50 --
 *             not to the kart that carries it
 *   sniper    1: an instant line, 100 dead on, 50 at the edge of a kart
 *   homing    2: a slower rocket that turns toward the nearest kart ahead of
 *             it, trailing smoke; bursts on what it meets, up to 30
 *   oil       1: a slick dropped behind for twelve seconds: a kart on it has
 *             a sixth of its grip and a third of its pedal, and slides
 *   shockwave 1: a ring out from the kart to nine in a third of a second:
 *             15 to every kart it passes, and thrown outward at 18
 *   smoke     1: a cloud round the kart, thick for five seconds and
 *             dissipating over fifteen -- a homing rocket does not see a kart
 *             in it while it is thick enough -- and a DAGGER for eight: every
 *             fifth of a second, 8 to each kart the kart is up against
 *
 * TWO ARE CARRIED: the one in hand and a SPARE. A power-up goes in hand if
 * that is empty, else into the spare -- in place of what was there -- and the
 * spare comes to hand when the one in hand is spent. The same weapon again
 * is more of it instead: its ammo on top (at most three power-ups' worth).
 *
 * Nothing hurts the kart that fired it. A burst's damage is its tree of fire
 * (KartBattle, a burst): less further out along a branch, and none behind
 * what the fire cannot get round. Every number is the arena's, in the rows'
 * own integers (Fixed), so every runtime lands the same hits.
 */
namespace KartArms
{
    enum Weapon : uint8_t { None, Rocket, Mine, Shotgun, Boost, Bomb, Minigun, Fuse, Sniper, Homing, Oil, Shockwave, Smoke, Count };

    inline const char* Name(uint8_t w)
    {
        static const char* names[] = { "-", "rocket", "mine", "shotgun", "boost", "bomb", "minigun", "fuse", "sniper", "homing", "oil", "shockwave", "smoke" };
        return w < Count ? names[w] : "-";
    }
    inline uint8_t Ammo(uint8_t w)
    {
        static const uint8_t ammo[] = { 0, 2, 2, 2, 1, 2, 30, 1, 1, 2, 1, 1, 1 };
        return w < Count ? ammo[w] : 0;
    }
    inline uint8_t ByName(const std::string& n)
    {
        for (uint8_t w = 1; w < Count; ++w) if (n == Name(w)) return w;
        return None;
    }

    // What flies (or lies) about the arena: the shots in the air, a mine or a
    // slick on the ground, a lit fuse on a kart, a sniper's line for a moment,
    // a shockwave's ring going out, a cloud of smoke.
    enum Shot : uint8_t { RocketShot, Pellet, Bullet, BombShot, MineShot, FuseShot, Tracer, HomingShot, OilShot, WaveShot, SmokeShot };

    // The arena's dice: xorshift64*, its state a line of the record's to
    // seed, so a power-up rolls the same weapon on every runtime.
    inline uint64_t Next(uint64_t& s)
    {
        s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
        return s * 2685821657736338717ull;
    }
}

#endif // KARTPROVIDER_KARTARMS_H__
