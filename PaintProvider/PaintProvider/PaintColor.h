#ifndef PAINTPROVIDER_PAINTCOLOR_H__
#define PAINTPROVIDER_PAINTCOLOR_H__

#include "../../../core_defs.h"
#include "../../../ontology.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <mutex>
#include <random>
#include <thread>
#include <string>
#include <map>
#include <memory>
#include <cctype>
#include <unordered_map>
#include <vector>

struct PaintColor
{
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
};

enum class PaintBlendMode : uint8_t
{
    Normal = 0,
    Multiply = 1,
    Screen = 2,
    Erase = 3,
};

/*
 * ── the nib ──────────────────────────────────────────────────────────────
 *
 * WHAT IS LOADED, not what the gesture is. PaintToolKind is the shape of the
 * whole interaction and says so; this is the other half that comment names --
 * the thing a kind is holding. Every marking kind holds one, and they all
 * reduce to DrawBrush, so a rectangle outlined with the stylus comes out
 * calligraphic without the shape code hearing about it.
 *
 * Two independent facts on one stepped list, because that is the choice a hand
 * makes: Round and Stylus differ in the stamp's SHAPE, Erase in what the stamp
 * WRITES (blend, below). One arrow instead of two, at the cost of stepping past
 * a shape to reach the eraser.
 */
enum class PaintTipMode : uint8_t
{
    Round = 0,   // the disc paint_stamp_of has always made
    Stylus = 1,  // a chisel: a straight nib held at an angle
    Erase = 2,   // the disc again, taking ink off rather than laying it
};

inline const char* paint_tip_mode_name(PaintTipMode t)
{
    switch (t)
    {
        case PaintTipMode::Round:  return "round";
        case PaintTipMode::Stylus: return "stylus";
        case PaintTipMode::Erase:  return "erase";
    }
    return "round";
}

inline PaintTipMode paint_tip_mode_from(const std::string& s)
{
    if (s == "stylus") return PaintTipMode::Stylus;
    if (s == "erase" || s == "eraser") return PaintTipMode::Erase;
    return PaintTipMode::Round;
}

#endif // PAINTPROVIDER_PAINTCOLOR_H__
