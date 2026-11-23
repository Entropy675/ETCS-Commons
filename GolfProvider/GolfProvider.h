#ifndef GOLFPROVIDER_H__
#define GOLFPROVIDER_H__

#define ETCS_DLL_EXPORTS
#include "../../core_defs.h"
#include "../../ontology.h"
#include "Contract_GolfProvider.h"

// ── GolfMatch ─────────────────────────────────────────────────────────────────
// Every RID handed over is a RenderProvider node (or a TextLabel for the card
// and the board): the world, the pools of balls and pieces the course is laid
// with, the eye on them. See GolfMatch.h for the trace and the game, and
// GolfCourse.h for what a course is.

DEFINE_WORK_FUNC(GolfMatch, Create)                                  { (void)ctx; (void)data; self.Create(); }
DEFINE_WORK_FUNC_TYPED(GolfMatch, BindWorld,  (ETCS::RID, rid))     { (void)ctx; self.BindWorld(rid); }
DEFINE_WORK_FUNC_TYPED(GolfMatch, BindCamera, (ETCS::RID, rid))     { (void)ctx; self.BindCamera(rid); }
DEFINE_WORK_FUNC_TYPED(GolfMatch, BindCard,   (ETCS::RID, rid))     { (void)ctx; self.BindCard(rid); }
DEFINE_WORK_FUNC_TYPED(GolfMatch, BindFps,    (ETCS::RID, rid))     { (void)ctx; self.BindFps(rid); }
// BindArrow <arrow> <shadow> -- what a pull draws, and its shadow on the ground.
DEFINE_WORK_FUNC_TYPED(GolfMatch, BindArrow, (ETCS::RID, arrow), (ETCS::RID, shadow)) { (void)ctx; self.BindArrow(arrow, shadow); }
// BindGhost <ring> <arrow> -- where a power will go, while the left holds it.
DEFINE_WORK_FUNC_TYPED(GolfMatch, BindGhost, (ETCS::RID, ring), (ETCS::RID, arrow)) { (void)ctx; self.BindGhost(ring, arrow); }
DEFINE_WORK_FUNC_TYPED(GolfMatch, AddBoardRow, (ETCS::RID, rid))    { (void)ctx; self.AddBoardRow(rid); }
// The longest pull (scene units) and the speed it gives (units per second).
DEFINE_WORK_FUNC_TYPED(GolfMatch, SetPower, (float, max_pull), (float, max_speed)) { (void)ctx; self.SetPower(max_pull, max_speed); }
// The camera's distance from the ball, its bearing and its pitch, in degrees.
DEFINE_WORK_FUNC_TYPED(GolfMatch, SetOrbit, (float, distance), (float, yaw), (float, pitch)) { (void)ctx; self.SetOrbit(distance, yaw, pitch); }

// The pools (golf_pool.etcs): a ball and its shadow; a slab; the cup; the
// tee's mark; a pad's plate and three arrows; a spring's plate and four; a
// power's spot; a puff the powers draw with.
DEFINE_WORK_FUNC_TYPED(GolfMatch, AddBall, (ETCS::RID, ball), (ETCS::RID, shadow)) { (void)ctx; self.AddBall(ball, shadow); }
DEFINE_WORK_FUNC_TYPED(GolfMatch, AddSlabNode, (ETCS::RID, rid))    { (void)ctx; self.AddSlabNode(rid); }
DEFINE_WORK_FUNC_TYPED(GolfMatch, AddCupNode,  (ETCS::RID, rid))    { (void)ctx; self.AddCupNode(rid); }
// BakeCup <rim> <rim mesh> <well mesh> -- the cup drawn round: the rim's node (coloured as the
// course's turf) and the two empty meshes the match bakes the rim and the well into.
DEFINE_WORK_FUNC_TYPED(GolfMatch, BakeCup, (ETCS::RID, rim), (ETCS::RID, rim_mesh), (ETCS::RID, well_mesh)) { (void)ctx; self.BakeCup(rim, rim_mesh, well_mesh); }
DEFINE_WORK_FUNC_TYPED(GolfMatch, AddTeeNode,  (ETCS::RID, rid))    { (void)ctx; self.AddTeeNode(rid); }
DEFINE_WORK_FUNC_TYPED(GolfMatch, AddSpotNode, (ETCS::RID, rid))    { (void)ctx; self.AddSpotNode(rid); }
DEFINE_WORK_FUNC_TYPED(GolfMatch, AddPuffNode, (ETCS::RID, rid))    { (void)ctx; self.AddPuffNode(rid); }
DEFINE_WORK_FUNC_TYPED(GolfMatch, AddPadNode, (ETCS::RID, plate), (ETCS::RID, a), (ETCS::RID, b), (ETCS::RID, c))
{ (void)ctx; self.AddPadNode(plate, a, b, c); }
DEFINE_WORK_FUNC_TYPED(GolfMatch, AddSpringNode, (ETCS::RID, plate), (ETCS::RID, a), (ETCS::RID, b), (ETCS::RID, c), (ETCS::RID, d))
{ (void)ctx; self.AddSpringNode(plate, a, b, c, d); }

// The courses (golf_course_*.etcs): a course by name, or one made from a seed;
// then its holes, each with what is on it.
DEFINE_WORK_FUNC_TYPED(GolfMatch, AddCourse, (std::string, name))       { (void)ctx; self.AddCourse(name); }
DEFINE_WORK_FUNC_TYPED(GolfMatch, AddRandomCourse, (std::string, name)) { (void)ctx; self.AddRandomCourse(name); }
// SetTheme <turf r g b> <wall r g b> <sand r g b> <sky r g b> -- the latest course's look
// (drawn only; a course without one has the first courses' green and blue).
DEFINE_WORK_FUNC_TYPED(GolfMatch, SetTheme, (float, tr), (float, tg), (float, tb), (float, wr), (float, wg), (float, wb),
                       (float, sr), (float, sg), (float, sb), (float, kr), (float, kg), (float, kb))
{ (void)ctx; const float c[12] = { tr, tg, tb, wr, wg, wb, sr, sg, sb, kr, kg, kb }; self.SetTheme(c); }
// AddHole <par> -- the next hole of the latest course.
DEFINE_WORK_FUNC_TYPED(GolfMatch, AddHole, (uint32_t, par))             { (void)ctx; self.AddHole(par); }
// AddGreen / AddWall / AddSand <x> <y> <z> <w> <h> <d> <tilt> <yaw> -- a slab: its
// middle and size, tilted about its own x and turned about +y (degrees).
DEFINE_WORK_FUNC_TYPED(GolfMatch, AddGreen, (float, x), (float, y), (float, z), (float, w), (float, h), (float, d), (float, tilt), (float, yaw))
{ (void)ctx; self.AddGreen(x, y, z, w, h, d, tilt, yaw); }
DEFINE_WORK_FUNC_TYPED(GolfMatch, AddWall, (float, x), (float, y), (float, z), (float, w), (float, h), (float, d), (float, tilt), (float, yaw))
{ (void)ctx; self.AddWall(x, y, z, w, h, d, tilt, yaw); }
DEFINE_WORK_FUNC_TYPED(GolfMatch, AddSand, (float, x), (float, y), (float, z), (float, w), (float, h), (float, d), (float, tilt), (float, yaw))
{ (void)ctx; self.AddSand(x, y, z, w, h, d, tilt, yaw); }
DEFINE_WORK_FUNC_TYPED(GolfMatch, AddCup, (float, x), (float, z))      { (void)ctx; self.AddCup(x, z); }
DEFINE_WORK_FUNC_TYPED(GolfMatch, AddTee, (float, x), (float, z))      { (void)ctx; self.AddTee(x, z); }
DEFINE_WORK_FUNC_TYPED(GolfMatch, AddPad, (float, x), (float, z), (float, yaw)) { (void)ctx; self.AddPad(x, z, yaw); }
DEFINE_WORK_FUNC_TYPED(GolfMatch, AddSpring, (float, x), (float, z))   { (void)ctx; self.AddSpring(x, z); }
DEFINE_WORK_FUNC_TYPED(GolfMatch, AddSpot, (float, x), (float, z))     { (void)ctx; self.AddSpot(x, z); }
// The courses are made: the first out, the lobby open.
DEFINE_WORK_FUNC(GolfMatch, Ready) { (void)ctx; (void)data; self.Ready(); }

// Practice <name> -- alone, the authority of a trace nobody follows.
DEFINE_WORK_FUNC_TYPED(GolfMatch, Practice, (std::string, name))   { (void)ctx; self.Practice(name); }
// Host <name> <hall> <id> -- the authority, listed in the hall under `id`.
DEFINE_WORK_FUNC_TYPED(GolfMatch, Host, (std::string, name), (ETCS::RID, hall), (std::string, id))
{ (void)ctx; self.Host(name, hall, id); }
// Join <name> <owner> -- a mirror of `owner`'s match, seated as `name`.
DEFINE_WORK_FUNC_TYPED(GolfMatch, Join, (std::string, name), (std::string, owner))
{ (void)ctx; self.Join(name, owner); }
DEFINE_WORK_FUNC(GolfMatch, Leave) { (void)ctx; (void)data; self.Leave(); }
DEFINE_WORK_FUNC(GolfMatch, Tick)  { (void)ctx; (void)data; self.Tick(); }
// Configure <course> <secs a hole> -- the host's, in the lobby: "ok" or "refused".
DEFINE_WORK_FUNC(GolfMatch, Configure)
{
    (void)ctx;
    std::string course; uint32_t secs = 0;
    data >> course >> secs;
    data.reset();
    data.writeString(self.Configure(course, secs) ? "ok" : "refused");
}
DEFINE_WORK_FUNC(GolfMatch, Reroll)     { (void)ctx; const bool ok = self.Reroll(); data.reset(); data.writeString(ok ? "ok" : "refused"); }
DEFINE_WORK_FUNC(GolfMatch, StartRound) { (void)ctx; const bool ok = self.StartRound(); data.reset(); data.writeString(ok ? "ok" : "refused"); }
DEFINE_WORK_FUNC(GolfMatch, Abandon)    { (void)ctx; const bool ok = self.Abandon(); data.reset(); data.writeString(ok ? "ok" : "refused"); }
// Give <who> <power> [spare|pickup] -- the host's: a power in hand outright, as
// the spare, or as a spot hands one over (GolfPowers.h names).
DEFINE_WORK_FUNC(GolfMatch, Give)
{
    (void)ctx;
    std::string who, power, how;
    data >> who >> power >> how;
    data.reset();
    data.writeString(self.Give(who, power, how) ? "ok" : "refused");
}

// The hand as verbs (window pixels), a key, and a shot or a power stated outright.
DEFINE_WORK_FUNC_TYPED(GolfMatch, Press,   (float, x), (float, y)) { (void)ctx; self.Press(x, y); }
DEFINE_WORK_FUNC_TYPED(GolfMatch, Drag,    (float, x), (float, y)) { (void)ctx; self.Drag(x, y); }
DEFINE_WORK_FUNC_TYPED(GolfMatch, Release, (float, x), (float, y)) { (void)ctx; self.Release(x, y); }
DEFINE_WORK_FUNC_TYPED(GolfMatch, RightPress,   (float, x), (float, y)) { (void)ctx; self.RightPress(x, y); }
DEFINE_WORK_FUNC_TYPED(GolfMatch, RightRelease, (float, x), (float, y)) { (void)ctx; self.RightRelease(x, y); }
DEFINE_WORK_FUNC_TYPED(GolfMatch, Wheel,   (float, notches))       { (void)ctx; self.Wheel(notches); }
// Key <code> <down> -- E arms a power, Tab the board, the host's Enter and Esc.
DEFINE_WORK_FUNC_TYPED(GolfMatch, Key, (uint32_t, code), (int32_t, down)) { (void)ctx; self.Key(static_cast<uint16_t>(code), down != 0); }
// Shoot <x> <y> <z> -- this runtime's shot along a pull (scene units).
DEFINE_WORK_FUNC_TYPED(GolfMatch, Shoot, (float, x), (float, y), (float, z)) { (void)ctx; self.Shoot(x, y, z); }
// Use <x> <y> <z> <yaw> -- this runtime's power put down there, looking `yaw` degrees.
DEFINE_WORK_FUNC_TYPED(GolfMatch, Use, (float, x), (float, y), (float, z), (float, yaw)) { (void)ctx; self.Use(x, y, z, yaw); }
// Frame <ms> -- the frame edge by hand; Run <ticks> -- the authority's clock by hand.
DEFINE_WORK_FUNC_TYPED(GolfMatch, Frame, (float, ms))      { (void)ctx; self.Frame(ms); }
DEFINE_WORK_FUNC_TYPED(GolfMatch, Run, (uint32_t, ticks))  { (void)ctx; self.Run(ticks); }

// Reads: GolfMatch.h says each one's form.
DEFINE_WORK_FUNC(GolfMatch, Status)   { (void)ctx; data.reset(); data.writeString(self.Status().c_str()); }
DEFINE_WORK_FUNC(GolfMatch, Scores)   { (void)ctx; data.reset(); data.writeString(self.Scores().c_str()); }
DEFINE_WORK_FUNC(GolfMatch, Maps)     { (void)ctx; data.reset(); data.writeString(self.Maps().c_str()); }
DEFINE_WORK_FUNC(GolfMatch, Layout)   { (void)ctx; data.reset(); data.writeString(self.Layout().c_str()); }
DEFINE_WORK_FUNC(GolfMatch, Faults)   { (void)ctx; const std::string f = self.Faults(); data.reset(); data.writeString(f.c_str()); ETCS_LOG("GolfMatch", "faults: " << f); }
DEFINE_WORK_FUNC(GolfMatch, Hash)     { (void)ctx; data.reset(); data.writeString(self.Hash().c_str()); }
DEFINE_WORK_FUNC(GolfMatch, Snapshot) { (void)ctx; data.reset(); data.writeString(self.Snapshot().c_str()); }
DEFINE_WORK_FUNC(GolfMatch, Note)     { (void)ctx; data.reset(); data.writeString(self.Note().c_str()); }
DEFINE_WORK_FUNC(GolfMatch, Here)     { (void)ctx; data.reset(); data.writeString(self.Here().c_str()); }
// Ball [<name>] -- "<slot> <x> <y> <z> <speed> <strokes> <total> <state> <power> <resting|moving> <spare>".
DEFINE_WORK_FUNC(GolfMatch, Ball)     { (void)ctx; std::string who; data >> who; data.reset(); data.writeString(self.Ball(who).c_str()); }
DEFINE_WORK_FUNC(GolfMatch, Pull)     { (void)ctx; data.reset(); data.writeString(self.Pull().c_str()); }
DEFINE_WORK_FUNC(GolfMatch, Delete)   { (void)ctx; (void)data; data.writeString(self.Delete() ? "deleted" : "FAILED"); }

// The session's streams: each ends with its session (ontology/Trace.h, "a
// trace is a session's"); the bodies are the family's.
//
// game.Trace(<from>) -> mirror.Follow() -- this match's lines, from `from` on.
DEFINE_STREAM_FUNC_PRODUCE_STANDING(GolfMatch, Trace)
{
    (void)data;
    uint64_t from = 0;
    { ETCS::Buffer cfg = stream.getConfig(); cfg >> from; }
    etcs_trace_produce(self, stream, ctx, from);
}
// host.Trace(0) -> game.Follow() -- a mirror of the host's match.
DEFINE_STREAM_FUNC_CONSUME(GolfMatch, Follow) { (void)data; etcs_trace_follow(self, stream, ctx); }
// game.Emit() -> host.Take() -- this runtime's seat in the host's match.
DEFINE_STREAM_FUNC_PRODUCE_STANDING(GolfMatch, Emit) { (void)data; etcs_trace_emit(self, stream, ctx); }
// ... -> game.Take() -- a guest's seat, its lines taken in the order they come.
DEFINE_STREAM_FUNC_CONSUME(GolfMatch, Take) { (void)data; etcs_trace_take(self, stream, ctx); }

// main.ProducePointer() -> game.ConsumePointer() -- presses, drags, releases
// and the wheel; what each means is GolfMatch::Pointer's.
DEFINE_STREAM_FUNC_CONSUME(GolfMatch, ConsumePointer)
{
    (void)data;
    while (stream.isOpen())
    {
        if (ctx.isInterrupted() || ctx.isTerminated()) break;
        ETCS::Buffer slot;
        if (!stream.readRaw(slot)) break;
        InputEvent ev{};
        slot.readRaw(&ev, sizeof(InputEvent));
        self.Pointer(ev);
    }
}

// main.ProduceEvents() -> game.ConsumeKeys() -- the keyboard, and the edge
// that pumps the window: without it no pointer event is polled either.
DEFINE_STREAM_FUNC_CONSUME(GolfMatch, ConsumeKeys)
{
    (void)data;
    while (stream.isOpen())
    {
        if (ctx.isInterrupted() || ctx.isTerminated()) break;
        ETCS::Buffer slot;
        if (!stream.readRaw(slot)) break;
        InputEvent ev{};
        slot.readRaw(&ev, sizeof(InputEvent));
        if (ev.action == INPUT_DOWN)    self.Key(ev.key, true);
        else if (ev.action == INPUT_UP) self.Key(ev.key, false);
    }
}

#endif // GOLFPROVIDER_H__
