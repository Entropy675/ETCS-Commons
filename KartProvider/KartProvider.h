#ifndef KARTPROVIDER_H__
#define KARTPROVIDER_H__

#define ETCS_DLL_EXPORTS
#include "../../core_defs.h"
#include "../../ontology.h"
#include "Contract_KartProvider.h"

#include <chrono>
#include <thread>

// ── KartRace ─────────────────────────────────────────────────────────────────
// The world, the camera on it and the card on the camera are RenderProvider
// nodes; each kart of the pool is a Scene3D the course made (AddKart). See
// KartRace.h for the record's lines and what a key does.

DEFINE_WORK_FUNC(KartRace, Create)                                 { (void)ctx; (void)data; self.Create(); }
DEFINE_WORK_FUNC_TYPED(KartRace, BindWorld,  (ETCS::RID, rid))    { (void)ctx; self.BindWorld(rid); }
DEFINE_WORK_FUNC_TYPED(KartRace, BindCamera, (ETCS::RID, rid))    { (void)ctx; self.BindCamera(rid); }
DEFINE_WORK_FUNC_TYPED(KartRace, BindCard,   (ETCS::RID, rid))    { (void)ctx; self.BindCard(rid); }
// AddKart <body> <fl> <fr> <rl> <rr> -- a kart of the pool and its four wheels.
DEFINE_WORK_FUNC_TYPED(KartRace, AddKart, (ETCS::RID, body), (ETCS::RID, fl), (ETCS::RID, fr), (ETCS::RID, rl), (ETCS::RID, rr))
{
    (void)ctx;
    self.AddKart(body, fl, fr, rl, rr);
}
// Practice <name> -- alone, its own judge, nothing recorded.
DEFINE_WORK_FUNC_TYPED(KartRace, Practice, (std::string, name))   { (void)ctx; self.Practice(name); }
// Host <record> <proposals> <presence> <name> <hall> <id>
DEFINE_WORK_FUNC_TYPED(KartRace, Host, (ETCS::RID, record), (ETCS::RID, proposals), (ETCS::RID, presence),
                       (std::string, name), (ETCS::RID, hall), (std::string, id))
{
    (void)ctx;
    self.Host(record, proposals, presence, name, hall, id);
}
// Join <presence> <name> <owner>
DEFINE_WORK_FUNC_TYPED(KartRace, Join, (ETCS::RID, presence), (std::string, name), (std::string, owner))
{
    (void)ctx;
    self.Join(presence, name, owner);
}
DEFINE_WORK_FUNC(KartRace, Leave) { (void)ctx; (void)data; self.Leave(); }
DEFINE_WORK_FUNC(KartRace, Tick)  { (void)ctx; (void)data; self.Tick(); }
// Key <code> <down> -- a GLFW key, for a test or a page.
DEFINE_WORK_FUNC_TYPED(KartRace, Key, (uint32_t, code), (int32_t, down))
{
    (void)ctx;
    self.Key(static_cast<uint16_t>(code), down != 0);
}
// "<tick> <seq> <mode> <me> <slot>"
DEFINE_WORK_FUNC(KartRace, Status)    { (void)ctx; data.reset(); data.writeString(self.Status().c_str()); }
// "<slot> <driver> <laps> <last s> <best s> <speed>" per kart with a driver.
DEFINE_WORK_FUNC(KartRace, Standings) { (void)ctx; data.reset(); data.writeString(self.Standings().c_str()); }
// "<tick> <hash>": the race now.
DEFINE_WORK_FUNC(KartRace, Hash)      { (void)ctx; data.reset(); data.writeString(self.Hash().c_str()); }
// "<tick> <hash>": the last snapshot, what presence carries.
DEFINE_WORK_FUNC(KartRace, Snapshot)  { (void)ctx; data.reset(); data.writeString(self.Snapshot().c_str()); }
// Kart <name> -- "<slot> <x> <z> <heading x> <heading z> <speed>", or "-".
DEFINE_WORK_FUNC(KartRace, Kart)      { (void)ctx; std::string who; data >> who; data.reset(); data.writeString(self.Kart(who).c_str()); }
// Run <ticks> -- the judge's clock by hand: stepped and recorded.
DEFINE_WORK_FUNC_TYPED(KartRace, Run, (uint32_t, ticks)) { (void)ctx; self.Run(ticks); }
DEFINE_WORK_FUNC(KartRace, Note)      { (void)ctx; data.reset(); data.writeString(self.Note().c_str()); }
DEFINE_WORK_FUNC(KartRace, Delete)    { (void)ctx; (void)data; data.writeString(self.Delete() ? "deleted" : "FAILED"); }

// race.Emit() -> proposals.Take() -- a guest's lines, for as long as the
// session lasts. Ends when the reader goes.
DEFINE_STREAM_FUNC_PRODUCE_STANDING(KartRace, Emit)
{
    (void)data;
    std::string line;
    while (!ctx.isInterrupted() && !ctx.isTerminated() && !stream.readerGone())
    {
        if (!self.nextEmit(line)) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue; }
        if (!stream.writeMessage(line)) break;
    }
}
// record.Follow(0) -> race.Absorb() -- the record, applied in its order.
DEFINE_STREAM_FUNC_CONSUME(KartRace, Absorb)
{
    (void)data;
    std::string m;
    while (!ctx.isInterrupted() && stream.readMessage(m, 1u << 20)) self.Absorb(m);
    ETCS_LOG("KartRace", "Absorb: the record ended.");
    self.RecordEnded();
}
// proposals.Follow(0) -> race.Judge() -- the host judging every line proposed.
DEFINE_STREAM_FUNC_CONSUME(KartRace, Judge)
{
    (void)data;
    std::string m;
    while (!ctx.isInterrupted() && stream.readMessage(m, 1u << 20)) self.JudgeLine(m);
}
// presence.Watch() -> race.Roster() -- the listing, whole, on every change.
DEFINE_STREAM_FUNC_CONSUME(KartRace, Roster)
{
    (void)data;
    std::string listing;
    while (!ctx.isInterrupted() && stream.readMessage(listing)) self.Roster(listing);
}
// main.ProduceEvents() -> race.ConsumeKeys() -- the keyboard, and the window's
// pump (golf_keys.etcs on why the key edge has to exist).
DEFINE_STREAM_FUNC_CONSUME(KartRace, ConsumeKeys)
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
