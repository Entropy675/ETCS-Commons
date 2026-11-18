#ifndef CRYPTPROVIDER_H__
#define CRYPTPROVIDER_H__

#define ETCS_DLL_EXPORTS
#include "../../core_defs.h"
#include "../../ontology.h"
#include "Contract_CryptProvider.h"

// No verb here takes or answers secret material: a line names the entity,
// and the material moves only between Private types or through a Foreign
// relation's fill (ontology/Private.h).

// ── Keypair ───────────────────────────────────────────────────────────────────
DEFINE_WORK_FUNC(Keypair, Generate)
{
    (void)ctx;
    const bool ok = self.Generate();
    data.reset(); data << ok;
}
DEFINE_WORK_FUNC(Keypair, PublicKey)
{
    (void)ctx;
    const std::string pub = self.PublicKey();
    data.reset(); data.write(pub.c_str());
}
// Sign <hex> -- at most 127 bytes; the signature in hex.
DEFINE_WORK_FUNC(Keypair, Sign)
{
    (void)ctx;
    std::string hex;
    data >> hex;
    std::string msg(hex.size() / 2, '\0');
    uint8_t sig[64];
    const bool ok = hex.size() % 2 == 0
        && etcs_crypt::unhex(hex, reinterpret_cast<uint8_t*>(&msg[0]), msg.size())
        && self.Sign(reinterpret_cast<const uint8_t*>(msg.data()), msg.size(), sig);
    data.reset();
    if (ok) data.write(etcs_crypt::hex(sig, sizeof sig).c_str());
}
DEFINE_WORK_FUNC(Keypair, StoreTo)
{
    (void)ctx;
    ETCS::RID rid = 0;
    data >> rid;
    const bool ok = self.StoreTo(rid);
    data.reset(); data << ok;
}
DEFINE_WORK_FUNC(Keypair, LoadFrom)
{
    (void)ctx;
    ETCS::RID rid = 0;
    data >> rid;
    const bool ok = self.LoadFrom(rid);
    data.reset(); data << ok;
}
DEFINE_WORK_FUNC(Keypair, Holds)  { (void)ctx; const bool h = self.Holds(); data.reset(); data << h; }
DEFINE_WORK_FUNC(Keypair, Wipe)   { (void)ctx; (void)data; self.Wipe(); }
DEFINE_WORK_FUNC(Keypair, Delete) { (void)ctx; (void)data; self.Delete(); }

// ── Shard ─────────────────────────────────────────────────────────────────────
DEFINE_WORK_FUNC(Shard, Index)     { (void)ctx; const int x = self.Index(); data.reset(); data << x; }
DEFINE_WORK_FUNC(Shard, Threshold) { (void)ctx; const int k = self.Threshold(); data.reset(); data << k; }
DEFINE_WORK_FUNC(Shard, Set)       { (void)ctx; const std::string s = self.Set(); data.reset(); data.write(s.c_str()); }
DEFINE_WORK_FUNC(Shard, Holds)     { (void)ctx; const bool h = self.Holds(); data.reset(); data << h; }
DEFINE_WORK_FUNC(Shard, Wipe)      { (void)ctx; (void)data; self.Wipe(); }
DEFINE_WORK_FUNC(Shard, Delete)    { (void)ctx; (void)data; self.Delete(); }

// ── Shamir ────────────────────────────────────────────────────────────────────
DEFINE_WORK_FUNC(Shamir, Threshold)
{
    (void)ctx;
    int k = 0;
    data >> k;
    const bool ok = self.Threshold(k);
    data.reset(); data << ok;
}
DEFINE_WORK_FUNC(Shamir, Split)
{
    (void)ctx;
    ETCS::RID rid = 0;
    data >> rid;
    const bool ok = self.Split(rid);
    data.reset(); data << ok;
}
DEFINE_WORK_FUNC(Shamir, Combine)
{
    (void)ctx;
    ETCS::RID rid = 0;
    data >> rid;
    const bool ok = self.Combine(rid);
    data.reset(); data << ok;
}
DEFINE_WORK_FUNC(Shamir, Delete) { (void)ctx; (void)data; self.Delete(); }

// ── Certificate ───────────────────────────────────────────────────────────────
DEFINE_WORK_FUNC(Certificate, AddEpoch)
{
    (void)ctx;
    std::string hex;
    data >> hex;
    const bool ok = self.AddEpoch(hex);
    data.reset(); data << ok;
}
// Issue <issuer keypair> <role> <subject keypair | public key hex>
DEFINE_WORK_FUNC(Certificate, Issue)
{
    (void)ctx;
    ETCS::RID issuer = 0;
    std::string role, subject;
    data >> issuer >> role >> subject;
    const bool ok = self.Issue(issuer, role, subject);
    data.reset(); data << ok;
}
// Verify [<issuer certificate>]
DEFINE_WORK_FUNC(Certificate, Verify)
{
    (void)ctx;
    ETCS::RID up = 0;
    data >> up;
    const bool ok = self.Verify(up);
    data.reset(); data << ok;
}
DEFINE_WORK_FUNC(Certificate, Role)    { (void)ctx; const std::string s = self.Role();    data.reset(); data.write(s.c_str()); }
DEFINE_WORK_FUNC(Certificate, Subject) { (void)ctx; const std::string s = self.Subject(); data.reset(); data.write(s.c_str()); }
DEFINE_WORK_FUNC(Certificate, Issuer)  { (void)ctx; const std::string s = self.Issuer();  data.reset(); data.write(s.c_str()); }
DEFINE_WORK_FUNC(Certificate, Digest)  { (void)ctx; const std::string s = self.Digest();  data.reset(); data.write(s.c_str()); }
DEFINE_WORK_FUNC(Certificate, Delete)  { (void)ctx; (void)data; self.Delete(); }

// ── Custody ───────────────────────────────────────────────────────────────────
// Every verb captures its context: the Exchange it makes answers to the
// script that asked.
DEFINE_WORK_FUNC(Custody, SetEmbassy)
{
    self.SetRunContext(ctx);
    ETCS::RID rid = 0;
    data >> rid;
    const bool ok = self.SetEmbassy(rid);
    data.reset(); data << ok;
}
DEFINE_WORK_FUNC(Custody, SetRelation)
{
    self.SetRunContext(ctx);
    std::string name;
    data >> name;
    const bool ok = self.SetRelation(name);
    data.reset(); data << ok;
}
// Advertise <name> <shard|cert> <rid>
DEFINE_WORK_FUNC(Custody, Advertise)
{
    self.SetRunContext(ctx);
    std::string name, kind, info;
    data >> name >> kind >> info;
    const bool ok = self.Advertise(name, kind, info);
    data.reset(); data << ok;
}
// Recall <name> <empty shard | empty certificate>
DEFINE_WORK_FUNC(Custody, Recall)
{
    self.SetRunContext(ctx);
    std::string name;
    ETCS::RID rid = 0;
    data >> name >> rid;
    const bool ok = self.Recall(name, rid);
    data.reset(); data << ok;
}
DEFINE_WORK_FUNC(Custody, Withdraw)
{
    self.SetRunContext(ctx);
    std::string name;
    data >> name;
    const bool ok = self.Withdraw(name);
    data.reset(); data << ok;
}
// List [shard|cert] -- "name kind relation" per line, as the custodian holds it now.
DEFINE_WORK_FUNC(Custody, List)
{
    self.SetRunContext(ctx);
    std::string kind;
    data >> kind;
    std::string all = self.Listing(kind);
    if (all.size() >= ETCS::Buffer::bufsize) all.resize(ETCS::Buffer::bufsize - 4), all += "...";
    data.reset();
    data.write(all.c_str());
}
DEFINE_WORK_FUNC(Custody, Delete) { (void)ctx; (void)data; self.Delete(); }

// ── Envelope ──────────────────────────────────────────────────────────────────
DEFINE_WORK_FUNC(Envelope, Holds)  { (void)ctx; const bool h = self.Holds(); data.reset(); data << h; }
DEFINE_WORK_FUNC(Envelope, Forget) { (void)ctx; (void)data; self.Forget(); }
DEFINE_WORK_FUNC(Envelope, Delete) { (void)ctx; (void)data; self.Delete(); }

// ── Registrar ─────────────────────────────────────────────────────────────────
DEFINE_WORK_FUNC(Registrar, Use)
{
    (void)ctx;
    ETCS::RID rid = 0;
    data >> rid;
    const bool ok = self.Use(rid);
    data.reset(); data << ok;
}
DEFINE_WORK_FUNC(Registrar, Above)
{
    (void)ctx;
    ETCS::RID rid = 0;
    data >> rid;
    const bool ok = self.Above(rid);
    data.reset(); data << ok;
}
DEFINE_WORK_FUNC(Registrar, Mount)
{
    (void)ctx;
    std::string segment;
    data >> segment;
    const bool ok = self.Mount(segment);
    data.reset(); data << ok;
}
DEFINE_WORK_FUNC(Registrar, Key)     { (void)ctx; const std::string k = self.Key(); data.reset(); data.write(k.c_str()); }
// The route's two halves (HttpServer.AddRoute <rid> Request <rid> Filter): a path in, an answer out.
DEFINE_WORK_FUNC(Registrar, Filter)  { (void)ctx; self.Accepts(data); }
DEFINE_WORK_FUNC(Registrar, Request) { (void)ctx; self.Request(data); }
DEFINE_WORK_FUNC(Registrar, Delete)  { (void)ctx; (void)data; self.Delete(); }

// ── Upstream ──────────────────────────────────────────────────────────────────
DEFINE_WORK_FUNC(Upstream, SetEmbassy)
{
    self.SetRunContext(ctx);
    ETCS::RID rid = 0;
    data >> rid;
    const bool ok = self.SetEmbassy(rid);
    data.reset(); data << ok;
}
DEFINE_WORK_FUNC(Upstream, SetRelation)
{
    self.SetRunContext(ctx);
    std::string name;
    data >> name;
    const bool ok = self.SetRelation(name);
    data.reset(); data << ok;
}
// Paths <pki> <dist>
DEFINE_WORK_FUNC(Upstream, Paths)
{
    (void)ctx;
    std::string pki, dist;
    data >> pki >> dist;
    const bool ok = self.Paths(pki, dist);
    data.reset(); data << ok;
}
DEFINE_WORK_FUNC(Upstream, Register) { self.SetRunContext(ctx); const bool ok = self.Register(); data.reset(); data << ok; }
DEFINE_WORK_FUNC(Upstream, Check)    { self.SetRunContext(ctx); const bool ok = self.Check();    data.reset(); data << ok; }
DEFINE_WORK_FUNC(Upstream, Update)   { self.SetRunContext(ctx); const bool ok = self.Update();   data.reset(); data << ok; }
DEFINE_WORK_FUNC(Upstream, List)     { (void)ctx; const std::string r = self.Listing(""); data.reset(); data.write(r.substr(0, 250).c_str()); }
DEFINE_WORK_FUNC(Upstream, Delete)   { (void)ctx; (void)data; self.Delete(); }

#endif // CRYPTPROVIDER_H__
