#ifndef GOLFPROVIDER_H__
#define GOLFPROVIDER_H__

#define ETCS_DLL_EXPORTS
#include "../../core_defs.h"
#include "../../ontology.h"
#include "Contract_GolfProvider.h"

// ── GolfGame ──────────────────────────────────────────────────────────────────
// What each RID names is a RenderProvider node (or a TextLabel for the card):
// the world the ball is in, the ball, the camera on it, the cup, the arrow
// drawn for a pull. See GolfGame.h for what a drag and a release mean.

DEFINE_WORK_FUNC(GolfGame, Create)                                  { (void)ctx; (void)data; self.Create(); }
DEFINE_WORK_FUNC_TYPED(GolfGame, BindWorld,  (ETCS::RID, rid))     { (void)ctx; self.BindWorld(rid); }
DEFINE_WORK_FUNC_TYPED(GolfGame, BindBall,   (ETCS::RID, rid))     { (void)ctx; self.BindBall(rid); }
DEFINE_WORK_FUNC_TYPED(GolfGame, BindCamera, (ETCS::RID, rid))     { (void)ctx; self.BindCamera(rid); }
DEFINE_WORK_FUNC_TYPED(GolfGame, BindCup,    (ETCS::RID, rid))     { (void)ctx; self.BindCup(rid); }
DEFINE_WORK_FUNC_TYPED(GolfGame, BindArrow,  (ETCS::RID, rid))     { (void)ctx; self.BindArrow(rid); }
DEFINE_WORK_FUNC_TYPED(GolfGame, BindHud,    (ETCS::RID, rid))     { (void)ctx; self.BindHud(rid); }
// Where the ball goes back to, in the world's frame.
DEFINE_WORK_FUNC_TYPED(GolfGame, SetTee, (float, x), (float, y), (float, z)) { (void)ctx; self.SetTee(x, y, z); }
// The longest pull (scene units) and the speed it gives (units per second).
DEFINE_WORK_FUNC_TYPED(GolfGame, SetPower, (float, max_pull), (float, max_speed)) { (void)ctx; self.SetPower(max_pull, max_speed); }
// The camera's distance from the ball, its bearing and its pitch, in degrees.
DEFINE_WORK_FUNC_TYPED(GolfGame, SetOrbit, (float, distance), (float, yaw), (float, pitch)) { (void)ctx; self.SetOrbit(distance, yaw, pitch); }
DEFINE_WORK_FUNC(GolfGame, Reset)   { (void)ctx; (void)data; self.Reset(); }
// The pointer's events as verbs (window pixels): a stroke played from a script.
DEFINE_WORK_FUNC_TYPED(GolfGame, Press,   (float, x), (float, y)) { (void)ctx; self.Press(x, y); }
DEFINE_WORK_FUNC_TYPED(GolfGame, Drag,    (float, x), (float, y)) { (void)ctx; self.Drag(x, y); }
DEFINE_WORK_FUNC_TYPED(GolfGame, Release, (float, x), (float, y)) { (void)ctx; self.Release(x, y); }
DEFINE_WORK_FUNC_TYPED(GolfGame, Wheel,   (float, notches))       { (void)ctx; self.Wheel(notches); }
// "x y z length mode": the pull as it stands.
DEFINE_WORK_FUNC(GolfGame, Pull) { (void)ctx; const std::string r = self.PullReport(); data.writeString(r.c_str()); ETCS_LOG("GolfGame", "pull: " << r); }
DEFINE_WORK_FUNC(GolfGame, Strokes) { (void)ctx; data.writeString(std::to_string(self.Strokes()).c_str()); ETCS_LOG("GolfGame", "strokes: " << self.Strokes()); }
DEFINE_WORK_FUNC(GolfGame, Delete)  { (void)ctx; (void)data; data.writeString(self.Delete() ? "deleted" : "FAILED"); }

// main.ProducePointer() -> game.ConsumePointer() -- presses, drags, releases
// and the wheel; what each means is GolfGame::Pointer's.
DEFINE_STREAM_FUNC_CONSUME(GolfGame, ConsumePointer)
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
DEFINE_STREAM_FUNC_CONSUME(GolfGame, ConsumeKeys)
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
