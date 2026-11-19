#ifndef KARTPROVIDER_H__
#define KARTPROVIDER_H__

#define ETCS_DLL_EXPORTS
#include "../../core_defs.h"
#include "../../ontology.h"
#include "Contract_KartProvider.h"

#include <chrono>
#include <thread>

// ── KartBattle ───────────────────────────────────────────────────────────────
// The world, the camera on it and the card on the camera are RenderProvider
// nodes; the arena's walls, maps and pool are Scene3D nodes the course made and
// handed over. See KartBattle.h for the trace's lines and what a key does.

DEFINE_WORK_FUNC(KartBattle, Create)                                 { (void)ctx; (void)data; self.Create(); }
DEFINE_WORK_FUNC_TYPED(KartBattle, BindWorld,  (ETCS::RID, rid))    { (void)ctx; self.BindWorld(rid); }
DEFINE_WORK_FUNC_TYPED(KartBattle, BindCamera, (ETCS::RID, rid))    { (void)ctx; self.BindCamera(rid); }
DEFINE_WORK_FUNC_TYPED(KartBattle, BindCard,   (ETCS::RID, rid))    { (void)ctx; self.BindCard(rid); }
// BindFps <label> -- the frame counter: this runtime's pictures a second, said on the label.
DEFINE_WORK_FUNC_TYPED(KartBattle, BindFps,    (ETCS::RID, rid))    { (void)ctx; self.BindFps(rid); }
// AddKart <body> <fr> <fl> <rr> <rl> <bar back> <bar fill> -- a kart of the
// pool, its four wheels (the right ones first, as the picture shows them)
// and its HP bar.
DEFINE_WORK_FUNC_TYPED(KartBattle, AddKart, (ETCS::RID, body), (ETCS::RID, fr), (ETCS::RID, fl), (ETCS::RID, rr),
                       (ETCS::RID, rl), (ETCS::RID, bar_back), (ETCS::RID, bar_fill))
{
    (void)ctx;
    self.AddKart(body, fr, fl, rr, rl, bar_back, bar_fill);
}
// The arena: a wall of every map; a map by name (or one made from a seed each
// time it is chosen), then what is on it -- blocks, ramps, hills, spawns and
// power-up spots (KartArena.h); the pools the map in play is drawn with; the
// rows of the menu and the board.
DEFINE_WORK_FUNC_TYPED(KartBattle, AddWall, (ETCS::RID, part))           { (void)ctx; self.AddWall(part); }
DEFINE_WORK_FUNC_TYPED(KartBattle, AddMap, (std::string, name))          { (void)ctx; self.AddMap(name); }
DEFINE_WORK_FUNC_TYPED(KartBattle, AddRandomMap, (std::string, name))    { (void)ctx; self.AddRandomMap(name); }
// AddBlock <x> <z> <width> <depth> <height> <yaw degrees>
DEFINE_WORK_FUNC_TYPED(KartBattle, AddBlock, (float, x), (float, z), (float, w), (float, d), (float, h), (float, yaw))
{
    (void)ctx;
    self.AddBlock(x, z, w, d, h, yaw);
}
// AddRamp <x> <z> <yaw degrees> <width> <up> <flat> <down> <height>
DEFINE_WORK_FUNC_TYPED(KartBattle, AddRamp, (float, x), (float, z), (float, yaw), (float, width), (float, up),
                       (float, flat), (float, down), (float, height))
{
    (void)ctx;
    self.AddRamp(x, z, yaw, width, up, flat, down, height);
}
// AddHill <x> <z> <radius> <height>
DEFINE_WORK_FUNC_TYPED(KartBattle, AddHill, (float, x), (float, z), (float, radius), (float, height))
{
    (void)ctx;
    self.AddHill(x, z, radius, height);
}
// AddPad <x> <z> <yaw> -- a pad on the level ground, pushing karts over it
// the way it points (a ramp's yaw).
DEFINE_WORK_FUNC_TYPED(KartBattle, AddPad, (float, x), (float, z), (float, yaw)) { (void)ctx; self.AddPad(x, z, yaw); }
// AddSpring <x> <z> -- a round pad that throws a kart up.
DEFINE_WORK_FUNC_TYPED(KartBattle, AddSpring, (float, x), (float, z))     { (void)ctx; self.AddSpring(x, z); }
// AddDeck <x> <z> <width> <depth> <height> -- a level plane in the air: on
// it from above, under it from below, through it going up.
DEFINE_WORK_FUNC_TYPED(KartBattle, AddDeck, (float, x), (float, z), (float, width), (float, depth), (float, height))
{
    (void)ctx;
    self.AddDeck(x, z, width, depth, height);
}
// AddPit <x> <z> <width> <depth> <deep> -- a floor sunk `deep` below the
// arena's; a ramp put in it rises from that floor.
DEFINE_WORK_FUNC_TYPED(KartBattle, AddPit, (float, x), (float, z), (float, width), (float, depth), (float, deep))
{
    (void)ctx;
    self.AddPit(x, z, width, depth, deep);
}
// AddLava <x> <z> <width> <depth> <deep> -- a pit of lava: a kart that comes
// down on its floor is out. What stands in it as high as it is deep (a ramp
// all level top) is an island, or a causeway.
DEFINE_WORK_FUNC_TYPED(KartBattle, AddLava, (float, x), (float, z), (float, width), (float, depth), (float, deep))
{
    (void)ctx;
    self.AddLava(x, z, width, depth, deep);
}
DEFINE_WORK_FUNC_TYPED(KartBattle, AddSpawn, (float, x), (float, z))      { (void)ctx; self.AddSpawn(x, z); }
DEFINE_WORK_FUNC_TYPED(KartBattle, AddPickup, (float, x), (float, z))     { (void)ctx; self.AddPickup(x, z); }
// AddPickupUp <x> <z> -- a box on top of the deck over the spot (AddPickup's is on the ground under it).
DEFINE_WORK_FUNC_TYPED(KartBattle, AddPickupUp, (float, x), (float, z))   { (void)ctx; self.AddPickupUp(x, z); }
DEFINE_WORK_FUNC_TYPED(KartBattle, AddBlockNode, (ETCS::RID, node))      { (void)ctx; self.AddBlockNode(node); }
DEFINE_WORK_FUNC_TYPED(KartBattle, AddSlabNode, (ETCS::RID, node))       { (void)ctx; self.AddSlabNode(node); }
DEFINE_WORK_FUNC_TYPED(KartBattle, AddWedgeNode, (ETCS::RID, node))      { (void)ctx; self.AddWedgeNode(node); }
DEFINE_WORK_FUNC_TYPED(KartBattle, AddDomeNode, (ETCS::RID, node))       { (void)ctx; self.AddDomeNode(node); }
DEFINE_WORK_FUNC_TYPED(KartBattle, AddPickupNode, (ETCS::RID, node))     { (void)ctx; self.AddPickupNode(node); }
DEFINE_WORK_FUNC_TYPED(KartBattle, AddShotNode, (ETCS::RID, node))       { (void)ctx; self.AddShotNode(node); }
DEFINE_WORK_FUNC_TYPED(KartBattle, AddBlastNode, (ETCS::RID, node))      { (void)ctx; self.AddBlastNode(node); }
// AddSparkNode <node> -- a spark of the bursts' fire: a glowing streak the battle lays along a branch.
DEFINE_WORK_FUNC_TYPED(KartBattle, AddSparkNode, (ETCS::RID, node))      { (void)ctx; self.AddSparkNode(node); }
// AddSmokeNode <node> -- a puff of the bursts' smoke: see-through, swelled, raised and thinned by the battle.
DEFINE_WORK_FUNC_TYPED(KartBattle, AddSmokeNode, (ETCS::RID, node))      { (void)ctx; self.AddSmokeNode(node); }
DEFINE_WORK_FUNC_TYPED(KartBattle, AddBoardRow, (ETCS::RID, label))      { (void)ctx; self.AddBoardRow(label); }
// AddPadNode <plate> <arrow> <arrow> <arrow> -- a pad of the pool: its plate
// and the three arrows that run along it (kart_pad.etcs).
DEFINE_WORK_FUNC_TYPED(KartBattle, AddPadNode, (ETCS::RID, plate), (ETCS::RID, a), (ETCS::RID, b), (ETCS::RID, c))
{
    (void)ctx;
    self.AddPadNode(plate, a, b, c);
}
// AddSpringNode <plate> <arrow> <arrow> <arrow> <arrow> -- a spring of the
// pool: its round plate and the four arrows that close on its middle.
DEFINE_WORK_FUNC_TYPED(KartBattle, AddSpringNode, (ETCS::RID, plate), (ETCS::RID, a), (ETCS::RID, b), (ETCS::RID, c),
                       (ETCS::RID, d))
{
    (void)ctx;
    self.AddSpringNode(plate, a, b, c, d);
}
// AddGroundNode <floor> -- the arena's floor, coloured by the battle as its
// slabs and domes are: a tyre's mark fades into the ground it is on.
DEFINE_WORK_FUNC_TYPED(KartBattle, AddGroundNode, (ETCS::RID, node))     { (void)ctx; self.AddGroundNode(node); }
// AddMarkNode <node> -- a segment of a tyre's mark: twenty-four a tyre, ninety-six
// a kart, in the order the karts are made (kart_ropes.etcs).
DEFINE_WORK_FUNC_TYPED(KartBattle, AddMarkNode, (ETCS::RID, node))       { (void)ctx; self.AddMarkNode(node); }
// The arena is made: the first map out, the lobby open.
DEFINE_WORK_FUNC(KartBattle, Ready) { (void)ctx; (void)data; self.Ready(); }
// Practice <name> -- alone, the authority of a trace nobody follows.
DEFINE_WORK_FUNC_TYPED(KartBattle, Practice, (std::string, name))   { (void)ctx; self.Practice(name); }
// Host <name> <hall> <id> -- the authority, listed in the hall under `id`.
DEFINE_WORK_FUNC_TYPED(KartBattle, Host, (std::string, name), (ETCS::RID, hall), (std::string, id))
{
    (void)ctx;
    self.Host(name, hall, id);
}
// Join <name> <owner> -- a mirror of `owner`'s battle, seated as `name`.
DEFINE_WORK_FUNC_TYPED(KartBattle, Join, (std::string, name), (std::string, owner))
{
    (void)ctx;
    self.Join(name, owner);
}
DEFINE_WORK_FUNC(KartBattle, Leave) { (void)ctx; (void)data; self.Leave(); }
DEFINE_WORK_FUNC(KartBattle, Tick)  { (void)ctx; (void)data; self.Tick(); }
// The host's lobby and round, each a line of the trace (refused on a guest,
// and the settings refused outside the lobby): Configure <map> <seconds>,
// Reroll (a new seed for a map made from one), StartRound, Abandon,
// Give <driver> <weapon>.
DEFINE_WORK_FUNC(KartBattle, Configure)
{
    (void)ctx;
    std::string map; uint32_t secs = 0;
    data >> map >> secs;
    data.reset();
    data.writeString(self.Configure(map, secs) ? "ok" : "refused");
}
DEFINE_WORK_FUNC(KartBattle, Reroll)     { (void)ctx; data.reset(); data.writeString(self.Reroll() ? "ok" : "refused"); }
DEFINE_WORK_FUNC(KartBattle, StartRound) { (void)ctx; data.reset(); data.writeString(self.StartRound() ? "ok" : "refused"); }
DEFINE_WORK_FUNC(KartBattle, Abandon)    { (void)ctx; data.reset(); data.writeString(self.Abandon() ? "ok" : "refused"); }
DEFINE_WORK_FUNC(KartBattle, Give)
{
    (void)ctx;
    std::string who, weapon;
    data >> who >> weapon;
    data.reset();
    data.writeString(self.Give(who, weapon) ? "ok" : "refused");
}
// SetInvert <on> -- this runtime's wheel keys swapped (A right, D left): the
// page's own setting, no line of the trace.
DEFINE_WORK_FUNC_TYPED(KartBattle, SetInvert, (int32_t, on)) { (void)ctx; self.SetInvert(on != 0); }
// Key <code> <down> -- a GLFW key, for a test or a page.
// Look <yaw> <pitch> -- this runtime's eye swung this far round its kart, to
// the picture's right, and over it, in degrees: what a drag with the right
// button, or Q and E, does. Let alone it comes back behind the kart.
DEFINE_WORK_FUNC_TYPED(KartBattle, Look, (float, yaw), (float, pitch)) { (void)ctx; self.Look(yaw, pitch); }
// "<eye x> <y> <z> <at x> <y> <z> <look yaw> <pitch>": where the eye is headed.
DEFINE_WORK_FUNC(KartBattle, Eye)       { (void)ctx; data.reset(); data.writeString(self.Eye().c_str()); }
// Frame <ms> -- the frame edge by hand, for a script with no window.
DEFINE_WORK_FUNC_TYPED(KartBattle, Frame, (float, ms)) { (void)ctx; self.Frame(ms); }
DEFINE_WORK_FUNC_TYPED(KartBattle, Key, (uint32_t, code), (int32_t, down))
{
    (void)ctx;
    self.Key(static_cast<uint16_t>(code), down != 0);
}
// "<tick> <seq> <mode> <me> <slot> <phase> <secs left> <map> <round secs> <layout>"
DEFINE_WORK_FUNC(KartBattle, Status)    { (void)ctx; data.reset(); data.writeString(self.Status().c_str()); }
// "<slot> <driver> <elims> <deaths> <hp> <weapon> <ammo> <alive|out>" per driver, best first
// (in the lobby, the last round completed).
DEFINE_WORK_FUNC(KartBattle, Scores)    { (void)ctx; data.reset(); data.writeString(self.Scores().c_str()); }
// The maps' names, one a line.
DEFINE_WORK_FUNC(KartBattle, Maps)      { (void)ctx; data.reset(); data.writeString(self.Maps().c_str()); }
// "<name> <layout> <blocks> <ramps> <hills> <spawns> <pickups> <pads> <springs> <decks> <pits>
// <digest>": the map in play.
DEFINE_WORK_FUNC(KartBattle, Layout)    { (void)ctx; data.reset(); data.writeString(self.Layout().c_str()); }
// What is wrong with the maps (KartArena::Faults), "<map>: <fault>" a line, or "none".
DEFINE_WORK_FUNC(KartBattle, Faults)    { (void)ctx; data.reset(); data.writeString(self.Faults().c_str()); }
// "<tick> <hash>": the battle now.
DEFINE_WORK_FUNC(KartBattle, Hash)      { (void)ctx; data.reset(); data.writeString(self.Hash().c_str()); }
// "<tick> <hash>": the last snapshot, what the trace's `hash` lines carry.
DEFINE_WORK_FUNC(KartBattle, Snapshot)  { (void)ctx; data.reset(); data.writeString(self.Snapshot().c_str()); }
// Kart <name> -- "<slot> <x> <z> <heading x> <heading z> <speed> <hp> <weapon> <ammo> <y> <air|ground> <wheel>
// <slide>", or "-".
DEFINE_WORK_FUNC(KartBattle, Kart)      { (void)ctx; std::string who; data >> who; data.reset(); data.writeString(self.Kart(who).c_str()); }
// Tyres <name> -- per tyre (fr fl rr rl) "<heat> <rope> <shown> <darkest>": its heat, its mark's
// rope laid behind it, the segments drawn and the darkest (0..1); or "-".
DEFINE_WORK_FUNC(KartBattle, Tyres)     { (void)ctx; std::string who; data >> who; data.reset(); data.writeString(self.Tyres(who).c_str()); }
// Run <ticks> -- the authority's clock by hand: stepped and kept.
DEFINE_WORK_FUNC_TYPED(KartBattle, Run, (uint32_t, ticks)) { (void)ctx; self.Run(ticks); }
DEFINE_WORK_FUNC(KartBattle, Note)      { (void)ctx; data.reset(); data.writeString(self.Note().c_str()); }
// Who is here: whose stream into or out of this battle is open, one a line.
DEFINE_WORK_FUNC(KartBattle, Here)      { (void)ctx; data.reset(); data.writeString(self.Here().c_str()); }
DEFINE_WORK_FUNC(KartBattle, Delete)    { (void)ctx; (void)data; data.writeString(self.Delete() ? "deleted" : "FAILED"); }

// The session's streams: each ends with its session (ontology/Trace.h, "a
// trace is a session's"), as well as when its other end goes. The bodies are
// the family's (ontology/TraceBase.h); the verbs are this type's.
//
// battle.Trace(<from>) -> mirror.Follow() -- this battle's lines, from `from` on.
DEFINE_STREAM_FUNC_PRODUCE_STANDING(KartBattle, Trace)
{
    (void)data;
    uint64_t from = 0;
    { ETCS::Buffer cfg = stream.getConfig(); cfg >> from; }
    etcs_trace_produce(self, stream, ctx, from);
}
// host.Trace(0) -> battle.Follow() -- a mirror of the host's battle.
DEFINE_STREAM_FUNC_CONSUME(KartBattle, Follow) { (void)data; etcs_trace_follow(self, stream, ctx); }
// battle.Emit() -> host.Take() -- this runtime's seat in the host's battle.
DEFINE_STREAM_FUNC_PRODUCE_STANDING(KartBattle, Emit) { (void)data; etcs_trace_emit(self, stream, ctx); }
// ... -> battle.Take() -- a guest's seat, its lines taken in the order they come.
DEFINE_STREAM_FUNC_CONSUME(KartBattle, Take) { (void)data; etcs_trace_take(self, stream, ctx); }
// main.ProducePointer() -> battle.ConsumePointer() -- the pointer: a drag with
// the right button looks round the kart (KartBattle::Pointer).
DEFINE_STREAM_FUNC_CONSUME(KartBattle, ConsumePointer)
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

// main.ProduceEvents() -> battle.ConsumeKeys() -- the keyboard, and the window's
// pump (golf_keys.etcs on why the key edge has to exist).
DEFINE_STREAM_FUNC_CONSUME(KartBattle, ConsumeKeys)
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

#endif // KARTPROVIDER_H__
