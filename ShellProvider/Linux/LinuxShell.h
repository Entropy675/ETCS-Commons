#ifndef LINUXSHELL_H__
#define LINUXSHELL_H__

#include "../../../core_defs.h"
#include "../../../ontology.h"

#include <string>

#include "LinuxTerminal.h"

// ---------------------------------------------------------------------------
// LinuxShell — an ETCS control thread you can hand scripts to.
//
// [Thread + Deletable + Lifecycle]. Thread is the whole point: a shell IS an
// actor, not a thing that owns one. It holds signal authority, carries a
// closure, and is the only kind of entity that can produce another control
// thread -- which is why Detach lives on IWireThread and why a shell is the one
// non-detached Thread in a running system (ontology/Thread.h).
//
// WHY THIS IS A PROVIDER TYPE AND NOT CORE. CommandExecutor stays exactly where
// it is: it defines how an ETCS command executes, which is a property of the
// language and belongs to the runtime. What is platform-specific is the SHELL
// -- raw mode, prompts, line editing, which terminal you are on -- and that had
// no home before this module, so it lived in ShellREPL.h wedged beside core
// behind an ETCS_LOADER guard. Here it is an ordinary provider type with an OS
// fork behind a contract name, exactly like GLFWWindow -> Window.
//
// The terminal itself is LinuxTerminal.h beside this file, and is deliberately
// NOT a member of this type. A Shell's actions are the run layer -- give it a
// script, ask it to detach one -- while reading a keystroke is not a causal
// claim about anything and has no receiver worth naming. So the terminal is
// four function pointers this module publishes at registration, and this type
// is what a script names.
//
// So the split is: the command execution layer is core, the command RUN layer
// is a type. `Shell` is the contract name (Contract_ShellProvider.h) and every
// OS implementation unifies under it, which is what lets a script say
// `spawn ShellProvider::Shell sh` and mean the same thing everywhere.
//
// A ROOT SPAWNS A SHELL, and that is the only way a non-detached Thread comes
// into existence. Everything the shell runs, and every child it detaches, hangs
// off it -- so the script history of a session is a SUBTREE rather than a list,
// which is what makes it reachable by a merkle walk later.
// ---------------------------------------------------------------------------
class LinuxShell : public ThreadBase<LinuxShell>,
                   public DeletableBase<LinuxShell>,
                   public LifecycleBase<LinuxShell>
{
public:
    WIRE_TYPE_IDENTITY(LinuxShell);

    LinuxShell()  = default;
    ~LinuxShell() = default;

    // ── Thread_ dispatch ──────────────────────────────────────────────────

    ETCS::Buffer ScriptConcrete() { return m_script; }

    /*
     * A detached script is a child SHELL, because a detached script is a
     * control thread and a control thread is what this type is. There is no
     * separate "detached executor" kind -- that distinction was an artifact of
     * the executor keeping its own registry, and the registry was the entity
     * graph written out by hand.
     *
     * The child is a child ENTITY, so "which jobs did this shell start" is the
     * typed-child list and needs nothing beside it. It inherits a COPY of this
     * shell's closure (ThreadBase::Bind), which is what lets it carry its
     * inputs into a lifetime that outlives the line that launched it.
     *
     * The Halted guard is ThreadBase's, not repeated here.
     */
    uint64_t DetachConcrete(const ETCS::Buffer& script)
    {
        LinuxShell* child = this->addTag<LinuxShell>();
        if (!child) return 0;
        child->m_script = script;
        this->InheritClosureTo(*child);
        return child->getRID();
    }

    // ── The run layer ─────────────────────────────────────────────────────

    /*
     * Run a script to completion on THIS thread.
     *
     * Reaches straight into core (ETCS::run_root_script) with no hook:
     * CommandExecutor.h arrives transitively through core_defs.h, so the entry
     * point is callable from here.
     *
     * INCOMPLETE FROM A MODULE, AND THIS IS THE KNOWN GAP. execute_command's
     * spawn, run and detach arms are all inside #ifdef ETCS_LOADER, which a
     * module build does not define -- so a script executed here parses and
     * walks correctly and then creates nothing. Measured, not assumed: the
     * preprocessed module TU contains no `spawn_entity`, no "detach: launching"
     * and no "run: binding resolution failed".
     *
     * The fix is not to un-gate them. The loader is the only thing that may
     * change memory topology, which is what that boundary is FOR, and a module
     * already negotiates across it by raising an event -- addTag<T> does
     * exactly this through AddTagEvent, and LoadEvent/EntityUnloadEvent are
     * both present in a module build. So the spawn a script needs is a request
     * to the one loader every module is handed at RegisterDynamicLoader, not a
     * second copy of the machinery.
     *
     * Until that lands, this runs a script for its structure and its own
     * bookkeeping, and CmdDetach is exercised loader-side (see
     * ShellTesterLoader).
     *
     * The ExecutionContext is anchored on THIS ENTITY rather than on a bare
     * Root, which is the point of the type existing: everything the script
     * spawns is owned by the shell that ran it, so a session's history is a
     * subtree.
     */
    bool RunScript(const std::string& path)
    {
        if (this->Halted()) return false;
        m_script = ETCS::Buffer(path.c_str());

        ETCS::SignalContext sig = this->Signals();
        ETCS::ExecutionContext ctx(this, &sig);
        ctx.is_root = true;

        ETCS::ExecuteStatus status = ETCS::ExecuteStatus::Ok;
        const bool ok = ETCS::run_root_script(path, ctx, &status);
        m_last_status = static_cast<int>(status);
        if (!ok)
            ETCS_LOG("Shell", "script '" << path << "' stopped: "
                     << ETCS::execute_status_name(status));

        /*
 * THE SHELL DROPS THE CLOSURE. IT DOES NOT EXIT.
 *
 * A Thread's context is a closure boundary (SignalContext::closure_root), so a
 * raise from inside a script this shell is running -- a window closing, a
 * `signal` at the prompt -- lands on THESE flags rather than on g_sig_int. The
 * boundary is what makes that raise mean "end this script", and this is what
 * ending it does: delete what it created, then stand the shell back up.
 *
 * MY OWN FLAGS, NOT THE WALK. isInterrupted() crosses the boundary upward, so
 * it also answers true for a genuine process SIGTERM -- and clearing on that
 * would have the shell swallow a real shutdown. raised() on the local pointers
 * asks the narrower question: was the raise addressed to THIS closure.
 *
 * Cleared afterwards, which is the whole difference between a boundary and a
 * kill switch. The script is over; the shell is not, and the next Run must not
 * inherit a standing interrupt from the last one.
 */
        const bool mine = ETCS::SignalContext::raised(sig.interrupt)
                       || ETCS::SignalContext::raised(sig.terminate);
        if (mine)
        {
            const ETCS::ExecSource src{path.c_str(), 0};
            const size_t gone = ETCS::dissolve_closure(ctx, src);
            ETCS_LOG("Shell", "closure of '" << path << "' ended -- " << gone
                     << " entit" << (gone == 1 ? "y" : "ies")
                     << " deleted, shell still up.");
            if (sig.interrupt) sig.interrupt->store(0, std::memory_order_release);
            if (sig.terminate) sig.terminate->store(0, std::memory_order_release);
        }
        return ok;
    }

    int LastStatus() const { return m_last_status; }

    /*
     * Ask the loader to create a type from ANY module, by its origin-affixed
     * name -- "RenderProvider::ImageSurface".
     *
     * Creation is the loader's alone (it is the only thing that may change
     * memory topology), but REQUESTING it is an event, and events cross the
     * boundary from module scope by design -- addTag<T> already does exactly
     * this through AddTagEvent. LoadEvent is present in a module build, so the
     * pathway is there; what was missing was anyone using it.
     */
    uint64_t SpawnForeign(const std::string& origin_tag)
    {
        const size_t sep = origin_tag.find("::");
        if (sep == std::string::npos)
        {
            ETCS_LOG("Shell", "spawn needs Provider::Tag, got '" << origin_tag << "'.");
            return 0;
        }
        const std::string key = origin_tag.substr(0, sep) + ":" + origin_tag.substr(sep + 2);
        try
        {
            ETCS::LoadEvent evt{key.c_str()};
            evt.root = this;
            ETCS::Entity* e = evt();
            if (!e) { ETCS_LOG("Shell", "loader refused '" << origin_tag << "'."); return 0; }
            // Observable to anything holding this entity: the spawn is a fact
            // about the shell's own history, and a tag is the record of one.
            this->addTag(ETCS::Buffer("spawned"));
            m_last_spawn = e->getRID();
            return m_last_spawn;
        }
        catch (const std::exception& ex)
        {
            ETCS_LOG("Shell", "spawn '" << origin_tag << "' threw: " << ex.what());
            return 0;
        }
    }

    void Report()
    {
        std::vector<std::pair<ETCS::Buffer, ETCS::RID>> kids;
        this->getTypedChildren(kids);
        ETCS_LOG("Shell", "RID:" << this->getRID()
                 << " script:'" << m_script.c_str()
                 << "' children:" << kids.size()
                 << " closure:" << this->ClosureSize()
                 << (this->Halted() ? " [halted]" : ""));
    }

    // ── Lifecycle_ / Deletable_ ───────────────────────────────────────────

    /*
     * Stop before anything is taken away. A shell owns a control thread, so the
     * arena's retire path asks it to halt (etcs_retire_entity, core/Entity.h)
     * and this is where it comes to rest: no more detaching, no more running.
     */
    void ReleaseConcrete()
    {
        this->Halt();
    }

    bool DeleteConcrete() override
    {
        this->Halt();
        this->removeTag(ETCS::Buffer("active"));
        return true;
    }

private:
    ETCS::Buffer m_script{""};
    int          m_last_status = 0;
    uint64_t     m_last_spawn  = 0;
};

#endif // LINUXSHELL_H__
