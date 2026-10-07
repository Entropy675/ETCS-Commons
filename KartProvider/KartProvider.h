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
// handed over. See KartBattle.h for the record's lines and what a key does.

DEFINE_WORK_FUNC(KartBattle, Create)                                 { (void)ctx; (void)data; self.Create(); }
DEFINE_WORK_FUNC_TYPED(KartBattle, BindWorld,  (ETCS::RID, rid))    { (void)ctx; self.BindWorld(rid); }
DEFINE_WORK_FUNC_TYPED(KartBattle, BindCamera, (ETCS::RID, rid))    { (void)ctx; self.BindCamera(rid); }
DEFINE_WORK_FUNC_TYPED(KartBattle, BindCard,   (ETCS::RID, rid))    { (void)ctx; self.BindCard(rid); }
// AddKart <body> <fl> <fr> <rl> <rr> <bar back> <bar fill> -- a kart of the
// pool, its four wheels and its HP bar.
DEFINE_WORK_FUNC_TYPED(KartBattle, AddKart, (ETCS::RID, body), (ETCS::RID, fl), (ETCS::RID, fr), (ETCS::RID, rl),
                       (ETCS::RID, rr), (ETCS::RID, bar_back), (ETCS::RID, bar_fill))
{
    (void)ctx;
    self.AddKart(body, fl, fr, rl, rr, bar_back, bar_fill);
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
DEFINE_WORK_FUNC_TYPED(KartBattle, AddSpawn, (float, x), (float, z))      { (void)ctx; self.AddSpawn(x, z); }
DEFINE_WORK_FUNC_TYPED(KartBattle, AddPickup, (float, x), (float, z))     { (void)ctx; self.AddPickup(x, z); }
DEFINE_WORK_FUNC_TYPED(KartBattle, AddBlockNode, (ETCS::RID, node))      { (void)ctx; self.AddBlockNode(node); }
DEFINE_WORK_FUNC_TYPED(KartBattle, AddSlabNode, (ETCS::RID, node))       { (void)ctx; self.AddSlabNode(node); }
DEFINE_WORK_FUNC_TYPED(KartBattle, AddDomeNode, (ETCS::RID, node))       { (void)ctx; self.AddDomeNode(node); }
DEFINE_WORK_FUNC_TYPED(KartBattle, AddPickupNode, (ETCS::RID, node))     { (void)ctx; self.AddPickupNode(node); }
DEFINE_WORK_FUNC_TYPED(KartBattle, AddShotNode, (ETCS::RID, node))       { (void)ctx; self.AddShotNode(node); }
DEFINE_WORK_FUNC_TYPED(KartBattle, AddBlastNode, (ETCS::RID, node))      { (void)ctx; self.AddBlastNode(node); }
DEFINE_WORK_FUNC_TYPED(KartBattle, AddBoardRow, (ETCS::RID, label))      { (void)ctx; self.AddBoardRow(label); }
// The arena is made: the first map out, the lobby open.
DEFINE_WORK_FUNC(KartBattle, Ready) { (void)ctx; (void)data; self.Ready(); }
// Practice <name> -- alone, its own judge, nothing recorded.
DEFINE_WORK_FUNC_TYPED(KartBattle, Practice, (std::string, name))   { (void)ctx; self.Practice(name); }
// Host <record> <proposals> <presence> <name> <hall> <id>
DEFINE_WORK_FUNC_TYPED(KartBattle, Host, (ETCS::RID, record), (ETCS::RID, proposals), (ETCS::RID, presence),
                       (std::string, name), (ETCS::RID, hall), (std::string, id))
{
    (void)ctx;
    self.Host(record, proposals, presence, name, hall, id);
}
// Join <presence> <name> <owner>
DEFINE_WORK_FUNC_TYPED(KartBattle, Join, (ETCS::RID, presence), (std::string, name), (std::string, owner))
{
    (void)ctx;
    self.Join(presence, name, owner);
}
DEFINE_WORK_FUNC(KartBattle, Leave) { (void)ctx; (void)data; self.Leave(); }
DEFINE_WORK_FUNC(KartBattle, Tick)  { (void)ctx; (void)data; self.Tick(); }
// The host's lobby and round, each a line of the record (refused on a guest,
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
// Key <code> <down> -- a GLFW key, for a test or a page.
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
// "<name> <layout> <blocks> <ramps> <hills> <spawns> <pickups> <digest>": the map in play.
DEFINE_WORK_FUNC(KartBattle, Layout)    { (void)ctx; data.reset(); data.writeString(self.Layout().c_str()); }
// "<tick> <hash>": the battle now.
DEFINE_WORK_FUNC(KartBattle, Hash)      { (void)ctx; data.reset(); data.writeString(self.Hash().c_str()); }
// "<tick> <hash>": the last snapshot, what presence carries.
DEFINE_WORK_FUNC(KartBattle, Snapshot)  { (void)ctx; data.reset(); data.writeString(self.Snapshot().c_str()); }
// Kart <name> -- "<slot> <x> <z> <heading x> <heading z> <speed> <hp> <weapon> <ammo> <y> <air|ground>", or "-".
DEFINE_WORK_FUNC(KartBattle, Kart)      { (void)ctx; std::string who; data >> who; data.reset(); data.writeString(self.Kart(who).c_str()); }
// Run <ticks> -- the judge's clock by hand: stepped and recorded.
DEFINE_WORK_FUNC_TYPED(KartBattle, Run, (uint32_t, ticks)) { (void)ctx; self.Run(ticks); }
DEFINE_WORK_FUNC(KartBattle, Note)      { (void)ctx; data.reset(); data.writeString(self.Note().c_str()); }
DEFINE_WORK_FUNC(KartBattle, Delete)    { (void)ctx; (void)data; data.writeString(self.Delete() ? "deleted" : "FAILED"); }

// battle.Emit() -> proposals.Take() -- a guest's lines, for as long as the
// session lasts. Ends when the reader goes.
DEFINE_STREAM_FUNC_PRODUCE_STANDING(KartBattle, Emit)
{
    (void)data;
    std::string line;
    while (!ctx.isInterrupted() && !ctx.isTerminated() && !stream.readerGone())
    {
        if (!self.nextEmit(line)) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue; }
        if (!stream.writeMessage(line)) break;
    }
}
// record.Follow(0) -> battle.Absorb() -- the record, applied in its order.
DEFINE_STREAM_FUNC_CONSUME(KartBattle, Absorb)
{
    (void)data;
    std::string m;
    while (!ctx.isInterrupted() && stream.readMessage(m, 1u << 20)) self.Absorb(m);
    ETCS_LOG("KartBattle", "Absorb: the record ended.");
    self.RecordEnded();
}
// proposals.Follow(0) -> battle.Judge() -- the host judging every line proposed.
DEFINE_STREAM_FUNC_CONSUME(KartBattle, Judge)
{
    (void)data;
    std::string m;
    while (!ctx.isInterrupted() && stream.readMessage(m, 1u << 20)) self.JudgeLine(m);
}
// presence.Watch() -> battle.Roster() -- the listing, whole, on every change.
DEFINE_STREAM_FUNC_CONSUME(KartBattle, Roster)
{
    (void)data;
    std::string listing;
    while (!ctx.isInterrupted() && stream.readMessage(listing)) self.Roster(listing);
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
