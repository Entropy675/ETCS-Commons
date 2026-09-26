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
 * WHAT IS KEPT IS HOW, NOT WHAT. The parent claims Environmental (ontology/
 * Environmental.h), so the runtime has been recording the actions that made
 * its tags what they are (core/Provenance.h); the scene is those actions,
 * compacted, as the ETCS script that makes it again (etcs_replay_capture),
 * plus what each Environmental entity says its script cannot (CaptureState).
 * A resume runs the script, then Restore puts each entity's named values
 * back (RebuildLocal) -- the local frame's half of the family; a surface on
 * the far side of a MirrorBuffer gets the other half (ReflectRemote).
 *
 * LOOKED UP BY WHAT IT IS. Each entity's record is keyed by its Module:Tag
 * and its RID-free merkle hash, and among entities equal in both, by the
 * order they were made in -- emergent, the same in the replay as the first
 * time, and never a stored RID. So a replay that did not reproduce an
 * entity finds no record for it, and says so, rather than handing it
 * someone else's state.
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
        std::vector<ETCS::Entity*> envs;
        walk(root, envs);
        std::map<std::string, int> seen;
        size_t put = 0;
        for (ETCS::Entity* e : envs)
        {
            const std::string type = typeOf(e), hash = hex64(e->getHash());
            const int ord = seen[type + "#" + hash]++;
            PersistenceStore::Record rec;
            std::string why;
            if (!PersistenceStore::get().getRecord(type, hash, ord, rec, why))
            {
                ETCS_LOG("Persistence", "Restore: " << type << " (" << hash << "): " << why
                         << (why == "no record" ? " -- the replay did not make what was saved" : ""));
                continue;
            }
            Environmental_* env = iface(e);
            if (!env) continue;
            ETCS::EnvironmentState st;
            if (!st.unpack(rec.kv)) { ETCS_LOG("Persistence", "Restore: " << type << ": state unreadable."); continue; }
            st.migrate(env->MigrateTo());
            if (!st.kv.empty() && !env->RebuildLocal(st))
                ETCS_LOG("Persistence", "Restore: " << type << " refused its state.");
            ++put;
        }
        ETCS_LOG("Persistence", "restored " << put << "/" << envs.size() << " under "
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
        Scene sc;
        Keeper::get().peek(sc);
        ETCS_LOG("Persistence", "scene now (" << sc.nroots << " root(s), " << sc.recs.size() << " record(s)):\n" << sc.script);
        for (auto& w : sc.warnings) ETCS_LOG("Persistence", "  cannot rebuild: " << w);
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
        std::vector<std::string>              warnings;
        size_t                                nroots = 0;
        uint64_t                              print  = 0;
    };

    static std::string typeOf(ETCS::Entity* e)
    { return e->getSourceModule().toString() + "::" + e->getSourceTag().toString(); }
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

    // Environmental entities under (and including) `e`, children first, in
    // the order the hash walks them -- the same order in any replay.
    static void walk(ETCS::Entity* e, std::vector<ETCS::Entity*>& out)
    {
        std::vector<std::pair<ETCS::Buffer, ETCS::RID>> kids;
        e->getTypedChildren(kids);
        ETCS::etcs_hash_detail::order_children(kids);
        for (auto& [tag, rid] : kids)
            if (ETCS::Entity* c = e->getTypedChild(tag, rid)) walk(c, out);
        if (e->isEnvironmental()) out.push_back(e);
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
                const std::string now = hex64(r->getHash());
                if (now == it->second.second) ++same;
                else out.push_back("'" + name + "' came back different (" + now + ", was "
                                   + it->second.second + ") -- see what Info says it cannot rebuild");
                want.erase(it);
            }
            for (auto& [name, th] : want) out.push_back("'" + name + "' (" + th.first + ") did not come back");
            out.insert(out.begin(), "resumed: " + std::to_string(same) + " root(s) as they were"
                       + (out.empty() ? "." : ", " + std::to_string(out.size()) + " not:"));
            return out;
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
            ETCS::ReplayCapture cap;
            // One names table for all of them: a line may name another root.
            for (auto& [r, hold] : held) ETCS::etcs_replay_capture(r, names[r->getRID()], names, cap);
            sc.script += cap.script;
            std::map<ETCS::Entity*, std::string> own;
            for (size_t i = 0; i < cap.environmental.size(); ++i)
                own[cap.environmental[i].second] =
                    cap.script.substr(cap.spans[i].first, cap.spans[i].second - cap.spans[i].first);

            for (auto& [r, hold] : held)
            {
                const ETCS::RID prid = rs.at(r->getRID());
                auto pn = names.find(prid);
                if (pn == names.end())
                { warn.push_back(typeOf(r) + "'s Persistence was not made by a script line"); continue; }
                sc.script += pn->second + ".Restore()\n";
                sc.roots  += names[r->getRID()] + " " + typeOf(r) + " " + hex64(r->getHash()) + "\n";
                ++sc.nroots;

                std::vector<ETCS::Entity*> envs;
                walk(r, envs);
                std::map<std::string, int> seen;
                for (ETCS::Entity* e : envs)
                {
                    PersistenceStore::Record rec;
                    rec.type = typeOf(e);
                    rec.hash = hex64(e->getHash());
                    rec.ord  = seen[rec.type + "#" + rec.hash]++;
                    std::vector<std::string> flags;
                    e->stateFlags(flags);
                    std::sort(flags.begin(), flags.end());
                    for (auto& f : flags) rec.tags += (rec.tags.empty() ? "" : " ") + f;
                    auto s = own.find(e);
                    if (s != own.end()) rec.script = s->second;
                    ETCS::EnvironmentState st;
                    if (Environmental_* env = iface(e)) env->CaptureState(st);
                    rec.kv = st.pack();
                    sc.recs.push_back(std::move(rec));
                }
            }
            for (auto& w : cap.warnings) warn.push_back(w);
            sc.warnings = std::move(warn);

            std::string all = sc.script + sc.roots;
            for (auto& rec : sc.recs) all += rec.type + rec.hash + std::to_string(rec.ord) + rec.kv;
            sc.print = XXH3_64bits(all.data(), all.size());
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
            bool ok = store.ready();
            for (auto& rec : sc.recs) ok = ok && store.putRecord(rec);
            ok = ok && store.putScene("last", sc.script, sc.roots);
            if (!ok) { ETCS_LOG("Persistence", "the store refused the scene."); return false; }
            std::string summary;
            std::istringstream in(sc.roots);
            std::string n, t, h;
            while (in >> n >> t >> h) summary += (summary.empty() ? "" : ", ") + n + " (" + t + ")";
            store.writeFile("scene", std::to_string(sc.nroots) + " root(s): " + summary + "\n");
            store.flush();
            if (sc.print != last_print_)
                ETCS_LOG("Persistence", "scene saved: " << summary << " -- " << sc.recs.size() << " record(s)");
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
        void watch()
        {
            while (!stop_.load())
            {
                for (int i = 0; i < 5 && !stop_.load(); ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (stop_.load()) continue;
                save(false);
            }
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
