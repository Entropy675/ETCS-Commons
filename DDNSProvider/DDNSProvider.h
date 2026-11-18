#ifndef DDNSPROVIDER_H__
#define DDNSPROVIDER_H__

#define ETCS_DLL_EXPORTS
#include "../../core_defs.h"
#include "../../ontology.h"
#include "Contract_DDNSProvider.h"

// ── Herald ────────────────────────────────────────────────────────────────────
// Track <rid>: the Directory to keep true.
// Witness <rid> <Action>: one witness; one line per witness.
DEFINE_WORK_FUNC(Herald, Track)
{
    (void)ctx;
    ETCS::RID rid = 0;
    data >> rid;
    const bool ok = self.Track(rid);
    data.reset(); data << ok;
}

DEFINE_WORK_FUNC(Herald, Witness)
{
    (void)ctx;
    ETCS::RID   rid = 0;
    std::string action;
    data >> rid;
    data >> action;
    const bool ok = self.Witness(rid, action);
    data.reset(); data << ok;
}

// "1" (default: any dissent holds), "0.5", "2/3".
DEFINE_WORK_FUNC(Herald, SetQuorum)
{
    (void)ctx;
    std::string q;
    data >> q;
    const bool ok = self.SetQuorum(q);
    data.reset(); data << ok;
}

DEFINE_WORK_FUNC(Herald, SetKind)
{
    (void)ctx;
    std::string kind;
    data >> kind;
    const bool ok = self.SetKind(kind);
    data.reset(); data << ok;
}

DEFINE_WORK_FUNC(Herald, SetInterval)
{
    (void)ctx;
    int seconds = -1;
    data >> seconds;
    const bool ok = self.SetInterval(seconds);
    data.reset(); data << ok;
}

DEFINE_WORK_FUNC(Herald, SetHoldoff)
{
    (void)ctx;
    int seconds = -1;
    data >> seconds;
    const bool ok = self.SetHoldoff(seconds);
    data.reset(); data << ok;
}

// The context is captured at Start, as HttpServer's is: a keeper started from
// a detached script answers to that script's signal scope.
DEFINE_WORK_FUNC(Herald, Start)
{
    (void)data;
    self.SetRunContext(ctx);
    self.Start();
}

DEFINE_WORK_FUNC(Herald, Stop)
{
    (void)data; (void)ctx;
    self.Stop();
}

DEFINE_WORK_FUNC(Herald, IsStarted)
{
    (void)ctx;
    const bool started = self.IsStarted();
    data.reset(); data << started;
}

// One sweep now, on this call's thread (refused while the clock runs).
DEFINE_WORK_FUNC(Herald, Sweep)
{
    self.SetRunContext(ctx);
    const bool ok = self.SweepNow();
    data.reset(); data << ok;
}

DEFINE_WORK_FUNC(Herald, Info)
{
    (void)ctx;
    const std::string info = self.Info();
    data.writeString(info.c_str());
    ETCS_LOG("Herald", info);
}

DEFINE_WORK_FUNC(Herald, Delete)
{
    (void)data; (void)ctx;
    self.Delete();
}


// ── Namecheap ─────────────────────────────────────────────────────────────────
// Every verb captures its context: the Exchange it makes answers to the
// script that asked.
DEFINE_WORK_FUNC(Namecheap, SetDomain)
{
    self.SetRunContext(ctx);
    std::string v;
    data >> v;
    const bool ok = self.SetDomain(v);
    data.reset(); data << ok;
}
DEFINE_WORK_FUNC(Namecheap, SetRegistrar)
{
    self.SetRunContext(ctx);
    std::string v;
    data >> v;
    const bool ok = self.SetRegistrar(v);
    data.reset(); data << ok;
}
DEFINE_WORK_FUNC(Namecheap, SetResolver)
{
    self.SetRunContext(ctx);
    std::string v;
    data >> v;
    const bool ok = self.SetResolver(v);
    data.reset(); data << ok;
}
DEFINE_WORK_FUNC(Namecheap, SetSecret)
{
    self.SetRunContext(ctx);
    std::string v;
    data >> v;
    const bool ok = self.SetSecret(v);
    data.reset(); data << ok;
}
DEFINE_WORK_FUNC(Namecheap, Hold)
{
    self.SetRunContext(ctx);
    std::string v;
    data >> v;
    const bool ok = self.Hold(v);
    data.reset(); data << ok;
}

DEFINE_WORK_FUNC(Namecheap, SetEmbassy)
{
    self.SetRunContext(ctx);
    ETCS::RID rid = 0;
    data >> rid;
    const bool ok = self.SetEmbassy(rid);
    data.reset(); data << ok;
}

// Advertise <host> A <ip> -- Lobby's surface, so a Herald can track it.
DEFINE_WORK_FUNC(Namecheap, Advertise)
{
    self.SetRunContext(ctx);
    std::string name, kind, info;
    data >> name >> kind >> info;
    const bool ok = self.Advertise(name, kind, info);
    data.reset(); data << ok;
}

DEFINE_WORK_FUNC(Namecheap, Withdraw)
{
    self.SetRunContext(ctx);
    std::string name;
    data >> name;
    const bool ok = self.Withdraw(name);
    data.reset(); data << ok;
}

// List [A] -- "host A ip" per held host, as the resolver answers now.
DEFINE_WORK_FUNC(Namecheap, List)
{
    self.SetRunContext(ctx);
    std::string kind;
    data >> kind;
    std::string all = self.Listing(kind);
    if (all.size() >= ETCS::Buffer::bufsize) all.resize(ETCS::Buffer::bufsize - 4), all += "...";
    data.reset();
    data.write(all.c_str());
    ETCS_LOG("Namecheap", "List:\n" << all);
}

// Self -- our address as Namecheap sees it; empty when it can't say.
DEFINE_WORK_FUNC(Namecheap, Self)
{
    self.SetRunContext(ctx);
    const std::string ip = self.Self();
    data.reset();
    data.write(ip.c_str());
    ETCS_LOG("Namecheap", "Self: " << (ip.empty() ? std::string("(no answer)") : ip));
}

DEFINE_WORK_FUNC(Namecheap, Delete) { (void)ctx; (void)data; self.Delete(); }

#endif // DDNSPROVIDER_H__
