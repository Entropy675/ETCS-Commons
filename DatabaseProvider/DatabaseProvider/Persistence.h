#ifndef DATABASEPROVIDER_PERSISTENCE_H__
#define DATABASEPROVIDER_PERSISTENCE_H__
#include "../../../ontology.h"
#include "Store.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

/*
 * Persistence -- the child that makes its parent outlive the runtime.
 *
 *   spawn NetworkProvider::Ledger book
 *   book.spawn(DatabaseProvider::Persistence keep)
 *
 * From then on `book` -- and every Environmental entity under it -- is kept
 * in this loader's store (Store.h) as it changes, and the next start of the
 * loader can put it back: "Continue where you left off?" (loaders/etcs.cc).
 *
 * WHAT IS KEPT IS HOW, AND THE VALUES BEHIND IT. The parent claims
 * Environmental (ontology/Environmental.h), so the runtime has been
 * recording the actions that made its tags what they are (core/
 * Provenance.h); the scene is those actions, compacted, as the ETCS script
 * that makes it again (etcs_replay_capture), plus the VALUE SURFACE of every
 * entity under the root -- the values behind its tags (Entity::values_: a
 * body's rows behind "Causal", a ledger's lines behind "ledger", a flag's
 * value set by an action), read off the one surface by etcs_capture_values.
 * Not only the Environmental ones: a box under the root has rows, and they
 * come back with it. A resume runs the script, then Restore hands each
 * entity its values back (etcs_restore_values) and lets an Environmental
 * one finish (RebuildLocal) -- the local frame's half of the family; a
 * surface on the far side of a MirrorBuffer gets the other half
 * (ReflectRemote).
 *
 * LOOKED UP BY WHAT IT IS. Each entity's record is keyed by its Module:Tag
 * and its RID-free identity hash (Entity::identityHash: the merkle over the
 * tag surface, which the values are not part of -- a box that moved is the
 * same box) and, among entities equal in both, by the order they were made
 * in: emergent, the same in the replay as the first time, and never a
 * stored RID. So a replay that did not reproduce an entity finds no record
 * for it, and says so, rather than handing it someone else's state. Whether
 * it came back AS IT WAS is the second question, and the STATE hash answers
 * it (Entity::getHash: the identity half and every value under it, as one
 * number -- Finish compares each root's, and names the values that differ).
 *
 * ONE STATE PER READ. A scene is read while it moves, so each root is read
 * frozen (etcs_freeze, ontology/Environmental.h): its Causal trees held, the
 * funnel watched by the root's hash epoch, the state hash composed from the
 * very values copied and the replay's records gathered in the same window;
 * composing, packing and writing read the copy with nothing held.
 *
 * ONLY WHAT MOVED. Each step does the work of what changed and no more: the
 * read takes again only the nodes whose hash epoch or value digest moved
 * (the rest come from the last read); the replay is composed again only when
 * a root's epoch moved (an action is a funnel change); a record is rebuilt
 * only for a node that was read; the store writes only the records whose
 * content changed, drops the ones whose entity is gone, and commits them with
 * the scene in one transaction. Info reports how little the last save took.
 *
 * WHEN. As it goes: a watcher recaptures twice a second and writes only
 * when the scene changed; and once more as the loader closes (Closing, the
 * family's wire), after which nothing is written this run. A capture that
 * cannot name something (a change made outside any action, an action on
 * something the scene does not rebuild) keeps what it can and says what it
 * could not; the hash check at the end of a resume says whether it mattered.
 *
 * WHAT MAY HOLD ONE. A global-scope entity that claims Environmental. A
 * Persistence with no parent is the loader's handle for a resume
 * (Resume/Finish) and keeps nothing itself.
 *
 * LOCAL ONLY. Its verbs are this loader's store, so it carries the bare
 * `Local` tag and no MirrorBuffer will bridge it (MirrorBuffer::localFrame).
 */
class Persistence : public EnvironmentalBase<Persistence>, public DeletableBase<Persistence>
{
public:
    WIRE_TYPE_IDENTITY(Persistence);

    Persistence()
    {
        addTypeTag(ETCS::Buffer("Local"));
        Keeper::get().add(this);
    }
    ~Persistence() { Keeper::get().remove(this); }

    // ── verbs ───────────────────────────────────────────────────────────────

    void Save()
    {
        if (!Keeper::get().save(true)) ETCS_LOG("Persistence", "Save: nothing written.");
    }

    // After a replay has run: the named values of every Environmental entity
    // from this child's parent down, children before parents, each found by
    // what the replay made it.
    void Restore()
    {
        ETCS::Entity* root = getParent();
        if (!root) { ETCS_LOG("Persistence", "Restore: no parent."); return; }
        std::vector<ETCS::Entity*> all;
        walk(root, all);
        std::map<std::string, int> seen;
        size_t put = 0, values = 0;
        for (ETCS::Entity* e : all)
        {
            const std::string type = typeOf(e), hash = hex64(e->identityHash());
            const int ord = seen[type + "#" + hash]++;
            if (!kept(e)) continue;
            PersistenceStore::Record rec;
            std::string why;
            if (!PersistenceStore::get().getRecord(type, hash, ord, rec, why))
            {
                ETCS_LOG("Persistence", "Restore: " << type << " (" << hash << "): " << why
                         << (why == "no record" ? " -- the replay did not make what was saved" : ""));
                continue;
            }
            ETCS::EnvironmentState st;
            if (!st.unpack(rec.kv)) { ETCS_LOG("Persistence", "Restore: " << type << ": state unreadable."); continue; }
            Environmental_* env = iface(e);
            if (env) st.migrate(env->MigrateTo());
            // The values back onto the surface, then what the type does with them.
            const size_t landed = etcs_restore_values(e, st);
            if (landed < st.kv.size())
                ETCS_LOG("Persistence", "Restore: " << type << ": " << (st.kv.size() - landed) << " of "
                         << st.kv.size() << " value(s) found no place on the surface.");
            values += landed;
            if (env && !st.kv.empty() && !env->RebuildLocal(st))
                ETCS_LOG("Persistence", "Restore: " << type << " refused its state.");
            ++put;
        }
        ETCS_LOG("Persistence", "restored " << put << " record(s), " << values << " value(s), under "
                 << typeOf(root) << " RID:" << root->getRID());
    }

    // On the loader's handle: the last scene, checked, as a script at
    // <store>/resume.etcs for the loader to run; saving held until Finish.
    bool Resume()
    {
        if (getParent()) { ETCS_LOG("Persistence", "Resume: this is a handle's verb -- spawn one at global scope."); return false; }
        auto& store = PersistenceStore::get();
        store.removeFile("resume.etcs");
        std::string script, roots, why;
        long long at = 0;
        if (!store.getScene("last", script, roots, at, why)) { ETCS_LOG("Persistence", "Resume: " << why); return false; }
        Keeper::get().expect(roots);
        std::string body = "# The scene this loader left (keyid " + store.keyid() + "), saved "
                         + std::to_string(at) + ". Written by Persistence.Resume.\n" + script;
        if (!store.writeFile("resume.etcs", body)) { Keeper::get().release(); ETCS_LOG("Persistence", "Resume: cannot write the script."); return false; }
        ETCS_LOG("Persistence", "resuming " << std::count(roots.begin(), roots.end(), '\n') << " root(s) from "
                 << store.dir() << "/resume.etcs");
        return true;
    }

    // On the handle, after the replay: did every root come back as it was?
    void Finish()
    {
        const auto result = Keeper::get().check();
        for (auto& line : result) ETCS_LOG("Persistence", line);
        PersistenceStore::get().removeFile("resume.etcs");
        Keeper::get().release();
        Keeper::get().save(true);
    }

    void Forget()
    {
        PersistenceStore::get().forgetScene("last");
        PersistenceStore::get().removeFile("scene");
        PersistenceStore::get().flush();
        ETCS_LOG("Persistence", "the last scene is forgotten.");
    }

    void Info()
    {
        auto& store = PersistenceStore::get();
        ETCS_LOG("Persistence", "RID:" << getRID() << " store " << store.dir() << " keyid "
                 << store.keyid() << (getParent() ? " keeping " + typeOf(getParent()) : std::string(" (handle)")));
        const Keeper::Last l = Keeper::get().last();   // before the peek, which is a read too
        Scene sc;
        Keeper::get().peek(sc);
        ETCS_LOG("Persistence", "scene now (" << sc.nroots << " root(s), " << sc.recs.size() << " record(s)):\n" << sc.script);
        for (auto& w : sc.warnings) ETCS_LOG("Persistence", "  cannot rebuild: " << w);
        ETCS_LOG("Persistence", "last save: read " << l.read << " of " << l.nodes << " node(s) (the rest stood still), rebuilt "
                 << l.rebuilt << " record(s), wrote " << l.written << " of " << l.records << ", dropped " << l.dropped
                 << "; replay " << (l.replay_reused ? "reused" : "composed") << ", scene " << (l.scene_written ? "written" : "unchanged"));
    }

    bool DeleteConcrete() override
    {
        std::string key = getSourceModule().toString() + ":" + getSourceTag().toString();
        return ETCS::DestroyEvent{ key.c_str(), this }();
    }

    // ── Environmental: nothing off its surface ──────────────────────────────
    bool RebuildLocalConcrete(const ETCS::EnvironmentState&) override  { return true; }
    // The loader is leaving (IWireEnvironmental): the last save, then none.
    void Closing() override { Keeper::onClosing(); }
    bool ReflectRemoteConcrete(const ETCS::EnvironmentState&) override { return true; }

private:
    struct Scene
    {
        std::string                           script, roots;
        std::vector<PersistenceStore::Record> recs;
        std::vector<uint64_t>                 digests;   // each record's content (Keeper::digestOf), beside it
        std::vector<std::string>              warnings;
        size_t                                nroots = 0;
        uint64_t                              print  = 0;
    };

    static std::string typeOf(ETCS::Entity* e)
    { return e->getSourceModule().toString() + "::" + e->getSourceTag().toString(); }
    static std::string typeOf(const FrozenNode& n) { return n.module + "::" + n.tag; }
    static std::string hex64(uint64_t v)
    { char b[17]; std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v)); return b; }
    // The name a script gave a root (IWireEnvironmental::NoteName).
    static std::string nameOf(ETCS::Entity* e)
    {
        ETCS::IWireEnvironmental* w = e ? e->environmentalWire() : nullptr;
        return w ? w->ScriptName() : std::string();
    }
    static Environmental_* iface(ETCS::Entity* e)
    { return static_cast<Environmental_*>(e->getInterfacePointer(ETCS::Buffer("Environmental"))); }

    // Every entity under (and including) `e`, children first, in the
    // canonical order (order_canonical) -- the same in any replay, whatever
    // RIDs it hands out: twins count in the order they were attached.
    static void walk(ETCS::Entity* e, std::vector<ETCS::Entity*>& out)
    {
        std::vector<std::pair<ETCS::Buffer, ETCS::RID>> kids;
        e->getTypedChildren(kids);
        ETCS::etcs_hash_detail::order_canonical(e, kids);
        for (auto& [tag, rid] : kids)
            if (ETCS::Entity* c = e->getTypedChild(tag, rid)) walk(c, out);
        out.push_back(e);
    }
    // Which of them get a record: one that claims Environmental, or one with
    // anything on its value surface. The rest the script makes whole.
    static bool kept(const ETCS::Entity* e)
    {
        if (e->isEnvironmental()) return true;
        std::vector<std::pair<std::string, std::string>> kv;
        e->values(kv);
        return !kv.empty();
    }
    static bool kept(const FrozenNode& n) { return n.environmental || !n.kv.empty(); }
    // A frozen tree (etcs_freeze, the hash's pre-order) in walk's order:
    // children first, which is the order a record's `ord` counts in.
    static void postOrder(const FrozenTree& t, size_t i, std::vector<size_t>& out)
    {
        for (size_t k : t.nodes[i].kids) postOrder(t, k, out);
        out.push_back(i);
    }
    static std::string packed(const FrozenNode& n)
    {
        ETCS::EnvironmentState st;
        for (auto& [k, v] : n.kv) st.set(k, v);
        return st.pack();
    }

    /*
     * THE KEEPER: every Persistence in the process, the watcher, and the
     * save. Leaked, like the store, because the watcher is a thread and may
     * outlive every entity; the module's static guard stops it first.
     */
    class Keeper
    {
    public:

        static Keeper& get() { static Keeper* k = new Keeper(); return *k; }

        void add(Persistence* p)
        {
            std::lock_guard<std::mutex> lock(mu_);
            live_.insert(p);
            if (!started_)
            {
                started_ = true;
                watcher_ = std::thread([this]() { watch(); });
            }
        }
        void remove(Persistence* p) { std::lock_guard<std::mutex> lock(mu_); live_.erase(p); }

        void expect(const std::string& roots)
        {
            std::lock_guard<std::mutex> lock(save_mu_);
            restoring_.store(true);
            expected_.clear();
            std::istringstream in(roots);
            std::string name, type, hash;
            while (in >> name >> type >> hash) expected_[name] = { type, hash };
        }
        void release() { restoring_.store(false); }

        std::vector<std::string> check()
        {
            std::map<std::string, std::pair<std::string, std::string>> want;
            { std::lock_guard<std::mutex> lock(save_mu_); want = expected_; }
            std::vector<std::string> out;
            size_t same = 0;
            for (auto& [rid, prid] : roots())
            {
                (void)prid;
                ETCS::Entity* r = ETCS::etcs_resolve_rid_anywhere(&ETCS::getLoader(), rid);
                if (!r) continue;
                const std::string name = nameOf(r);
                auto it = want.find(name);
                if (it == want.end()) continue;
                // The state hash: identity and every value under it. When it
                // differs, say which half -- the identity (the script did not
                // rebuild what was saved) or the values (named by count).
                FrozenTree t;
                etcs_freeze(r, t);
                const std::string now = hex64(t.state_hash);
                if (now == it->second.second) { ++same; want.erase(it); continue; }
                const size_t off = valuesOff(t);
                if (off) out.push_back("'" + name + "' came back different: " + std::to_string(off)
                                       + " value(s) under it differ from the record (state " + now + ", was "
                                       + it->second.second + ")");
                else     out.push_back("'" + name + "' came back different (" + now + ", was "
                                       + it->second.second + ") -- see what Info says it cannot rebuild");
                want.erase(it);
            }
            for (auto& [name, th] : want) out.push_back("'" + name + "' (" + th.first + ") did not come back");
            out.insert(out.begin(), "resumed: " + std::to_string(same) + " root(s) as they were"
                       + (out.empty() ? "." : ", " + std::to_string(out.size()) + " not:"));
            return out;
        }

        // The values under a root against their records, after a restore:
        // how many entities' surfaces differ from what was kept. The second
        // half of "as it was" -- the identity hash is the first.
        static size_t valuesOff(const FrozenTree& t)
        {
            std::vector<size_t> order;
            postOrder(t, 0, order);
            std::map<std::string, int> seen;
            size_t off = 0;
            for (size_t i : order)
            {
                const FrozenNode& n = t.nodes[i];
                const std::string type = typeOf(n), hash = hex64(n.identity);
                const int ord = seen[type + "#" + hash]++;
                if (!kept(n)) continue;
                PersistenceStore::Record rec;
                std::string why;
                if (!PersistenceStore::get().getRecord(type, hash, ord, rec, why)) { ++off; continue; }
                if (packed(n) != rec.kv) ++off;
            }
            return off;
        }

        // (root RID, its first Persistence's RID) for every global-scope
        // Environmental parent, in RID order.
        std::map<ETCS::RID, ETCS::RID> roots(std::vector<std::string>* warnings = nullptr)
        {
            std::vector<std::pair<ETCS::RID, ETCS::Entity*>> held;
            {
                std::lock_guard<std::mutex> lock(mu_);
                for (Persistence* p : live_) held.emplace_back(p->getRID(), p->getParent());
            }
            std::map<ETCS::RID, ETCS::RID> out;
            for (auto& [prid, parent] : held)
            {
                if (!parent) continue;
                ETCS::Entity* self = ETCS::etcs_resolve_rid_anywhere(&ETCS::getLoader(), prid);
                if (!self || self->getParent() != parent) continue;
                if (!parent->isGlobalScope())
                { if (warnings) warnings->push_back(typeOf(parent) + " is not at global scope; keep its root instead"); continue; }
                if (!parent->isEnvironmental())
                { if (warnings) warnings->push_back(typeOf(parent) + " does not claim Environmental"); continue; }
                auto it = out.find(parent->getRID());
                if (it == out.end() || prid < it->second) out[parent->getRID()] = prid;
            }
            return out;
        }

        void capture(Scene& sc)
        {
            std::vector<std::string> warn;
            const auto rs = roots(&warn);
            std::vector<std::pair<ETCS::Entity*, ETCS::LifetimeHold>> held;
            std::map<ETCS::RID, std::string> names;
            std::set<std::string> used;
            for (auto& [rid, prid] : rs)
            {
                (void)prid;
                ETCS::Entity* r = ETCS::etcs_resolve_rid_anywhere(&ETCS::getLoader(), rid);
                ETCS::LifetimeHold hold(r);
                if (!hold) continue;
                std::string name = nameOf(r);
                if (name.empty() || used.count(name)) name = "n" + std::to_string(held.size());
                used.insert(name);
                names[rid] = name;
                sc.script += "spawn " + typeOf(r) + " " + name + "\n";
                held.emplace_back(r, std::move(hold));
            }
            /*
             * ONE READ PER ROOT, FROZEN (etcs_freeze): its surface, its values
             * and the state hash composed from them, in one window -- and only
             * what moved since the last read is read again (prev_). The replay's
             * records are gathered in the same window, but only when the root's
             * hash epoch moved since they last were (every recorded action is a
             * funnel change, and moves it); the script is composed after, with
             * nothing held. All roots or none: they share one names table.
             */
            std::vector<FrozenTree> frozen(held.size()), base(held.size());   // base: what each read starts from
            for (size_t i = 0; i < held.size(); ++i)
            {
                auto pv = prev_.find(held[i].first->getRID());
                if (pv != prev_.end()) base[i] = std::move(pv->second);
            }
            prev_.clear();
            std::vector<ETCS::ReplayGather> gathered(held.size());
            std::vector<uint32_t> epochs(held.size(), 0);
            bool same_roots = held.size() == replay_epochs_.size();
            for (auto& [r, hold] : held) same_roots = same_roots && replay_epochs_.count(r->getRID());
            bool gather_all = !same_roots;
            for (int pass = 0; pass < 2; ++pass)
            {
                if (pass) { base = std::move(frozen); frozen = std::vector<FrozenTree>(held.size()); }
                bool gathered_any = false, reused_any = false;
                for (size_t i = 0; i < held.size(); ++i)
                {
                    ETCS::Entity* r = held[i].first;
                    bool took = false;
                    if (!etcs_freeze(r, frozen[i], [&]() {
                            epochs[i] = r->hashEpoch();
                            auto c = replay_epochs_.find(r->getRID());
                            took = gather_all || c == replay_epochs_.end() || c->second != epochs[i];
                            gathered[i] = ETCS::ReplayGather{};
                            if (took) ETCS::etcs_replay_gather(r, gathered[i]); }, 4, &base[i]))
                        warn.push_back("'" + names[r->getRID()] + "' kept changing through every read; this save may be torn");
                    (took ? gathered_any : reused_any) = true;
                }
                if (!(gathered_any && reused_any)) { gather_all = gathered_any; break; }
                gather_all = true;   // some moved and some did not: read them all once more, with their records
            }
            ETCS::ReplayCapture cap;   // one names table for all of them: a line may name another root
            if (gather_all)
            {
                for (size_t i = 0; i < held.size(); ++i)
                    ETCS::etcs_replay_compose(gathered[i], names[held[i].first->getRID()], names, cap);
                last_cap_ = cap; last_names_ = names;
                replay_epochs_.clear();
                for (size_t i = 0; i < held.size(); ++i) replay_epochs_[held[i].first->getRID()] = epochs[i];
            }
            else { cap = last_cap_; names = last_names_; }
            size_t copied = 0;
            last_.nodes = 0;
            for (auto& t : frozen) { copied += t.copied; last_.nodes += t.nodes.size(); }
            last_.read = copied; last_.rebuilt = 0; last_.replay_reused = !gather_all;
            // This read is what the next one starts from.
            auto keepReads = [&]() { for (size_t i = 0; i < held.size(); ++i) prev_[held[i].first->getRID()] = std::move(frozen[i]); };
            // Nothing moved anywhere: the scene is the last one, as it was built.
            if (!gather_all && copied == 0 && have_last_) { keepReads(); sc = last_scene_; return; }
            sc.script += cap.script;
            std::map<ETCS::Entity*, std::string> own;
            for (size_t i = 0; i < cap.environmental.size(); ++i)
                own[cap.environmental[i].second] =
                    cap.script.substr(cap.spans[i].first, cap.spans[i].second - cap.spans[i].first);

            // From here on the copy only: nothing is held, nothing live is read.
            std::unordered_map<ETCS::RID, CachedRecord> fresh_cache;
            size_t rebuilt = 0;
            for (size_t i = 0; i < held.size(); ++i)
            {
                ETCS::Entity* r = held[i].first;
                const FrozenTree& t = frozen[i];
                const ETCS::RID prid = rs.at(r->getRID());
                auto pn = names.find(prid);
                if (pn == names.end())
                { warn.push_back(typeOf(r) + "'s Persistence was not made by a script line"); continue; }
                sc.script += pn->second + ".Restore()\n";
                sc.roots  += names[r->getRID()] + " " + typeOf(r) + " " + hex64(t.state_hash) + "\n";   // the state hash: what Finish must see again
                ++sc.nroots;

                std::vector<size_t> order;
                postOrder(t, 0, order);
                std::map<std::string, int> seen;
                for (size_t k : order)
                {
                    const FrozenNode& n = t.nodes[k];
                    const std::string type = typeOf(n), hash = hex64(n.identity);   // the key: what it is, not where it stands
                    const int ord = seen[type + "#" + hash]++;   // counted for every entity: the order is the walk's
                    if (!kept(n)) continue;
                    auto s = own.find(n.e);
                    const std::string script = s != own.end() ? s->second : std::string();
                    // A node that stood still since its record was made (the
                    // stamp the freeze reuses it by) keeps that record.
                    auto c = rec_cache_.find(n.rid);
                    if (c != rec_cache_.end() && n.digestible && c->second.epoch == n.epoch && c->second.digest == n.digest
                        && c->second.rec.type == type && c->second.rec.hash == hash && c->second.rec.ord == ord
                        && c->second.rec.script == script)
                    {
                        sc.recs.push_back(c->second.rec);
                        sc.digests.push_back(c->second.content);
                        fresh_cache[n.rid] = std::move(c->second);
                        continue;
                    }
                    PersistenceStore::Record rec;
                    rec.type = type; rec.hash = hash; rec.ord = ord; rec.script = script;
                    for (auto& f : n.flags) rec.tags += (rec.tags.empty() ? "" : " ") + f;
                    rec.kv = packed(n);
                    const uint64_t content = digestOf({ rec.tags, rec.script, rec.kv });
                    sc.recs.push_back(rec);
                    sc.digests.push_back(content);
                    ++rebuilt;
                    fresh_cache[n.rid] = CachedRecord{ std::move(rec), content, n.epoch, n.digest };
                }
            }
            last_.rebuilt = rebuilt;
            rec_cache_ = std::move(fresh_cache);
            keepReads();
            for (auto& w : cap.warnings) warn.push_back(w);
            sc.warnings = std::move(warn);

            std::string all = sc.script + sc.roots;
            for (size_t i = 0; i < sc.recs.size(); ++i)
            {
                const auto& rec = sc.recs[i];
                all += rec.type + rec.hash + std::to_string(rec.ord);
                all.append(reinterpret_cast<const char*>(&sc.digests[i]), sizeof(uint64_t));
            }
            sc.print = XXH3_64bits(all.data(), all.size());
            last_scene_ = sc; have_last_ = true;
        }

        // A capture outside a save, for Info: one at a time with the saves.
        void peek(Scene& sc) { std::lock_guard<std::mutex> lock(save_mu_); capture(sc); }

        // Capture and, when it changed (or `force`), write. False when
        // nothing was written.
        bool save(bool force)
        {
            std::lock_guard<std::mutex> lock(save_mu_);
            if (restoring_.load() || (closed_ && !force)) return false;
            Scene sc;
            capture(sc);
            const std::string warned = join(sc.warnings);
            if (warned != last_warn_)
            {
                for (auto& w : sc.warnings) ETCS_LOG("Persistence", "cannot rebuild: " << w);
                last_warn_ = warned;
            }
            auto& store = PersistenceStore::get();
            if (sc.nroots == 0)
            {
                // Forget only what this run saved and has since let go of.
                if (!saved_any_) return false;
                store.forgetScene("last");
                store.removeFile("scene");
                store.flush();
                saved_any_ = false;
                last_print_ = 0;
                ETCS_LOG("Persistence", "nothing kept any more; the last scene is forgotten.");
                return true;
            }
            if (!force && sc.print == last_print_) return false;
            /*
             * ONLY WHAT CHANGED. Each record is remembered as it was last
             * written (written_, by content digest): a save writes the records
             * whose content moved, drops the ones whose entity is gone, and
             * rewrites the scene only when its script or roots did -- all in
             * one transaction (PersistenceStore::putSave). A run's first save
             * also drops what an earlier run left behind. A refused write
             * forgets everything, so the next save writes it all again.
             */
            if (!store.ready()) { ETCS_LOG("Persistence", "the store refused the scene."); return false; }
            std::map<std::string, std::pair<PersistenceStore::Key, uint64_t>> now;
            std::vector<const PersistenceStore::Record*> changed;
            for (size_t ri = 0; ri < sc.recs.size(); ++ri)
            {
                const auto& rec = sc.recs[ri];
                PersistenceStore::Key k{ rec.type, rec.hash, rec.ord };
                const std::string key = keyOf(k);
                const uint64_t d = sc.digests[ri];
                auto w = written_.find(key);
                if (w == written_.end() || w->second.second != d) changed.push_back(&rec);
                now[key] = { std::move(k), d };
            }
            std::vector<PersistenceStore::Key> gone;
            if (!swept_) { for (auto& k : store.recordKeys()) if (!now.count(keyOf(k))) gone.push_back(k); }
            else           for (auto& [key, kd] : written_) if (!now.count(key)) gone.push_back(kd.first);
            const uint64_t scene_d = digestOf({ sc.script, sc.roots });
            const bool scene_moved = scene_d != scene_written_;
            const PersistenceStore::SceneRow row{ "last", sc.script, sc.roots };
            if (!store.putSave(changed, gone, scene_moved ? &row : nullptr))
            {
                written_.clear(); swept_ = false; scene_written_ = 0;
                ETCS_LOG("Persistence", "the store refused the scene.");
                return false;
            }
            written_ = std::move(now); swept_ = true; scene_written_ = scene_d;
            std::string summary;
            std::istringstream in(sc.roots);
            std::string n, t, h;
            while (in >> n >> t >> h) summary += (summary.empty() ? "" : ", ") + n + " (" + t + ")";
            if (scene_moved) store.writeFile("scene", std::to_string(sc.nroots) + " root(s): " + summary + "\n");
            if (!changed.empty() || !gone.empty() || scene_moved) store.flush();
            if (sc.print != last_print_)
                ETCS_LOG("Persistence", "scene saved: " << summary << " -- " << sc.recs.size() << " record(s)");
            last_.records = sc.recs.size(); last_.written = changed.size(); last_.dropped = gone.size();
            last_.scene_written = scene_moved;
            last_print_ = sc.print;
            saved_any_  = true;
            return true;
        }

        // The module's static guard: stop before its code goes away.
        void stop()
        {
            stop_.store(true);
            if (watcher_.joinable()) watcher_.join();
        }

        static void onClosing()
        {
            Keeper& k = get();
            if (k.closing_.exchange(true)) return;   // said to every Persistence; once is the save
            k.save(false);
            std::lock_guard<std::mutex> lock(k.save_mu_);
            k.closed_ = true;
        }
    private:
        /*
         * WAITS FOR QUIET. A root whose hash epoch moved since the last wake is
         * being changed through the funnel right now -- a script building it,
         * say -- and a read in the middle of that is stale before it is
         * written and retries against every line. So the watcher reads once
         * the epochs have stood still for one wake, or after four wakes (two
         * seconds) whatever happens: a scene that never stops still gets kept.
         */
        void watch()
        {
            uint64_t last = 0;
            int deferred = 0;
            while (!stop_.load())
            {
                for (int i = 0; i < 5 && !stop_.load(); ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (stop_.load()) continue;
                const uint64_t now = epochSum();
                if (now != last && deferred < 4) { last = now; ++deferred; continue; }
                last = now; deferred = 0;
                save(false);
            }
        }
        // Every root's hash epoch, folded: moves when any flag or stored value
        // under any root does.
        uint64_t epochSum()
        {
            uint64_t h = 0;
            for (auto& [rid, prid] : roots())
            {
                (void)prid;
                ETCS::Entity* r = ETCS::etcs_resolve_rid_anywhere(&ETCS::getLoader(), rid);
                ETCS::LifetimeHold hold(r);
                if (hold) h = (h ^ (static_cast<uint64_t>(rid) + r->hashEpoch())) * 0x100000001b3ULL;
            }
            return h;
        }
        static std::string keyOf(const PersistenceStore::Key& k)
        { return k.type + "\n" + k.hash + "\n" + std::to_string(k.ord); }
        static uint64_t digestOf(std::initializer_list<std::string> fields)
        {
            std::string in;
            for (const std::string& f : fields) { const uint32_t n = static_cast<uint32_t>(f.size()); in.append(reinterpret_cast<const char*>(&n), 4); in += f; }
            return XXH3_64bits(in.data(), in.size());
        }
        static std::string join(const std::vector<std::string>& v)
        { std::string s; for (auto& x : v) s += x + "\n"; return s; }

        std::mutex               mu_;
        std::set<Persistence*>   live_;
        bool                     started_ = false;
        std::thread              watcher_;
        std::atomic<bool>        stop_{ false };

        std::mutex               save_mu_;           // one save at a time; guards below
        std::atomic<bool>        restoring_{ false };
        bool                     closed_    = false;   // the last save is written
        std::atomic<bool>        closing_{ false };
        bool                     saved_any_ = false;
        uint64_t                 last_print_ = 0;
        std::map<std::string, std::pair<PersistenceStore::Key, uint64_t>> written_;   // key -> (key, content digest) as last written
        bool                     swept_ = false;         // this run has dropped what earlier runs left
        // The last read, so the next reads only what moved (etcs_freeze prev),
        // and the replay as last composed, with the root epochs it saw.
        std::map<ETCS::RID, FrozenTree> prev_;
        // Each kept node's record as last built, with the stamp its node had
        // (hash epoch, value digest): unchanged, it is not built again.
        struct CachedRecord { PersistenceStore::Record rec; uint64_t content = 0; uint32_t epoch = 0; uint64_t digest = 0; };
        std::unordered_map<ETCS::RID, CachedRecord> rec_cache_;
        std::map<ETCS::RID, uint32_t>   replay_epochs_;
        ETCS::ReplayCapture             last_cap_;
        std::map<ETCS::RID, std::string> last_names_;
        Scene                           last_scene_;
        bool                            have_last_ = false;
    public:
        // What the last save did -- how little of the scene it touched.
        struct Last { size_t nodes = 0, read = 0, rebuilt = 0, records = 0, written = 0, dropped = 0;
                      bool replay_reused = false, scene_written = false; };
        Last last() { std::lock_guard<std::mutex> lock(save_mu_); return last_; }
    private:
        Last                            last_;
        uint64_t                 scene_written_ = 0;
        std::string              last_warn_;
        std::map<std::string, std::pair<std::string, std::string>> expected_;   // name -> (type, hash)
    };

    friend struct PersistenceModuleGuard;
    static void stopKeeper() { Keeper::get().stop(); }
};

// Joins the watcher before this module's code can be unmapped.
struct PersistenceModuleGuard { ~PersistenceModuleGuard() { Persistence::stopKeeper(); } };
static PersistenceModuleGuard persistence_module_guard;

#endif // DATABASEPROVIDER_PERSISTENCE_H__
