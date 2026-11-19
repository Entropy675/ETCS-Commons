#ifndef GOLFPROVIDER_GOLFPOWERS_H__
#define GOLFPROVIDER_GOLFPOWERS_H__

#include <cstdint>
#include <cstring>
#include <string>

/*
 * ── GolfPowers: what a ball picks up, and what using it puts down ─────────
 *
 * A ball that rolls through a spot (GolfCourse::Spot) takes a power if it has
 * none: one of these, from the record's dice. USING one is a line -- `use <x>
 * <y> <z> <yaw>`, the point it is put down at and the way the user was
 * looking -- so it lands at the tick the judge took it, the same everywhere.
 *
 *   pad        the kart's booster, on the ground where put, pushing the way
 *              the user looked; until the hole is over
 *   bumper     a short wall across the user's view; until the hole is over
 *   spring     the kart's spring: throws a ball rolling onto it up
 *   sand       a patch that drags hard at a ball rolling on it
 *   blackhole  pulls every ball near it in toward it, for a little over a second
 *   wind       pushes every ball on the hole the way the user looked, a few seconds
 *   teleport   the user's own ball to the point, if within TeleportReach of it
 *   tornado    wanders where it was put for a few seconds, swirling balls near
 *              it, and lightning strikes round it at random: each strike a
 *              burst whose energy spreads as it goes out, so a ball takes the
 *              share of it its size intercepts (GolfMatch::strikeLocked)
 */
namespace GolfPowers
{
enum Kind : uint8_t { None = 0, Pad, Bumper, Spring, Sand, BlackHole, Wind, Teleport, Tornado, Count };

inline const char* Name(uint8_t k)
{
    static const char* names[Count] = { "-", "pad", "bumper", "spring", "sand", "blackhole", "wind", "teleport", "tornado" };
    return k < Count ? names[k] : "-";
}
inline uint8_t ByName(const std::string& s)
{
    for (uint8_t k = 1; k < Count; ++k) if (s == Name(k)) return k;
    return None;
}
// Does the way the user looked matter to it?
inline bool Aimed(uint8_t k) { return k == Pad || k == Bumper || k == Wind; }

// In ticks (GolfMatch's kHz is 50).
constexpr int kBlackHoleTicks = 60;
constexpr int kWindTicks      = 150;
constexpr int kTornadoTicks   = 360;
constexpr int kStrikeEvery    = 35;
} // namespace GolfPowers

#endif // GOLFPROVIDER_GOLFPOWERS_H__
