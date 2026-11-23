#ifndef GOLFPROVIDER_GOLFPOWERS_H__
#define GOLFPROVIDER_GOLFPOWERS_H__

#include <cstdint>
#include <cstring>
#include <string>

/*
 * ── GolfPowers: what a ball picks up, and what using it puts down ─────────
 *
 * A ball that rolls through a spot (GolfCourse::Spot) takes a power: one of
 * these, from the record's dice. TWO ARE CARRIED, the one in hand and a
 * SPARE: a power goes in hand if that is empty, else into the spare -- in
 * place of what was there -- and the spare comes to hand when the one in hand
 * is used (each is used once; there is no more of one to have). USING one is
 * a line -- `use <x> <y> <z> <yaw>`, the point it is put down at and the way
 * the user was looking -- so it lands at the tick the judge took it, the same
 * everywhere.
 *
 *   pad        the kart's booster, on the ground where put, pushing the way
 *              the user looked; until the hole is over
 *   bumper     a short wall across the user's view; until the hole is over
 *   spring     the kart's spring: throws a ball rolling onto it up
 *   sand       a patch that drags hard at a ball rolling on it
 *   blackhole  pulls every ball near it in toward it, for half a minute
 *   wind       pushes every ball on the hole the way the user looked, half a minute
 *   teleport   the user's own ball to the point, if within TeleportReach of it
 *   tornado    wanders where it was put for half a minute, swirling balls near
 *              it, and lightning strikes round it at random: each strike a
 *              burst whose energy spreads as it goes out, so a ball takes the
 *              share of it its size intercepts (GolfMatch::strikeLocked)
 *   magnet     the cup pulls the user's own ball toward it, from as far as
 *              ten off, for half a minute (wherever it is put down), slowly
 *              enough to drop in
 *   cannon     the user's own ball, at rest, fired in an arc to the point --
 *              up to twenty off -- and down dead where it lands; no stroke
 *   smoke      a cloud where it is put down: thick for five seconds, then
 *              dissipating over fifteen -- what is in it, a cup or a ball, is
 *              not seen
 *   freeze     every other ball is in the cold for half a minute: it rolls
 *              as through treacle, a shot carrying about a third as far --
 *              slowed, not stopped: everyone plays at once, and still does
 */
namespace GolfPowers
{
enum Kind : uint8_t { None = 0, Pad, Bumper, Spring, Sand, BlackHole, Wind, Teleport, Tornado, Magnet, Cannon, Freeze, Smoke, Count };

inline const char* Name(uint8_t k)
{
    static const char* names[Count] = { "-", "pad", "bumper", "spring", "sand", "blackhole", "wind", "teleport", "tornado", "magnet", "cannon", "freeze", "smoke" };
    return k < Count ? names[k] : "-";
}
inline uint8_t ByName(const std::string& s)
{
    for (uint8_t k = 1; k < Count; ++k) if (s == Name(k)) return k;
    return None;
}
// Does the way the user looked matter to it?
inline bool Aimed(uint8_t k) { return k == Pad || k == Bumper || k == Wind; }

// In ticks (GolfMatch's kHz is 50). A power that works for a while works for
// half a minute: long enough to play a hole round it, not a moment to wait
// out (and a hole's end ends it).
constexpr int kAtWorkTicks = 30 * 50;
constexpr int kStrikeEvery = 35;
constexpr int kCannonTicks = 200;   // at most: until its ball comes down
constexpr int kSmokeThick  = 5 * 50, kSmokeTicks = 20 * 50, kSmokeSteps = 10;   // thick, then dissipating fifteen seconds
// How long a power is at work, of those that are for a while.
constexpr int Ticks(uint8_t k)
{
    return k == Cannon ? kCannonTicks : k == Smoke ? kSmokeTicks : (k == BlackHole || k == Wind || k == Tornado || k == Magnet || k == Freeze) ? kAtWorkTicks : 0;
}
} // namespace GolfPowers

#endif // GOLFPROVIDER_GOLFPOWERS_H__
