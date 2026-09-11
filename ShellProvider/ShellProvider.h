#ifndef SHELLPROVIDER_H__
#define SHELLPROVIDER_H__

#define ETCS_DLL_EXPORTS

/*
 * THIS MODULE HOSTS THE EXECUTOR.
 *
 * Declared before core_defs.h so CommandExecutor.h sees it: a Shell runs
 * scripts, so the engine has to be compiled where the Shell lives. See the
 * ETCS_EXECUTOR_HOST banner in core/CommandExecutor.h for why this is opt-in
 * per module rather than open to all of them.
 */
#define ETCS_SHELL_HOST 1

#include "../../core_defs.h"
#include "../../ontology.h"
#include "Contract_ShellProvider.h"

/*
 * ShellProvider -- one tag, and deliberately one.
 *
 *   Shell  -- an ETCS control thread you can hand scripts to
 *             [Thread + Deletable + Lifecycle]. Implemented by `LinuxShell`;
 *             the contract name says the role, not the terminal API
 *             (Contract_ShellProvider.h).
 *
 * The surface is small because a shell is not a program, it is an actor: give
 * it a script, or ask it to start one alongside. Everything expressive is in
 * WHAT you run, not in how many verbs the shell has.
 *
 * SHELL IS OPTIONAL, AND THAT FALLS OUT RATHER THAN BEING ENFORCED. A loader
 * with no modules boots and wires entities in C++ exactly as before; running an
 * .etcs script is what needs one of these. Nothing in core asks whether a Shell
 * exists, so its absence costs nothing.
 */

// ── Shell ────────────────────────────────────────────────────────────────

DEFINE_WORK_FUNC(Shell, Create)
{
    (void)ctx; (void)data;
    self.addTag(ETCS::Buffer("active"));
    ETCS_LOG("Shell::Create", "RID:" << self.getRID() << " ready.");
}

/*
 * Run <path> -- execute a script to completion, on this thread.
 *
 * Blocking on purpose. `run` in the language already means "launch and wait"
 * (Command.h's grammar note), and a shell asked to run a script is the same
 * statement one level up. The non-blocking form is Detach, below, which is a
 * different verb because it is a different causal claim.
 */
DEFINE_WORK_FUNC_TYPED(Shell, Run, (std::string, path))
{
    (void)ctx;
    if (!self.RunScript(path))
        ETCS_LOG("Shell::Run", "'" << path << "' did not complete.");
}

/*
 * Detach <path> -- start a script as a CHILD shell and keep going.
 *
 * Routed through the family's Detach rather than a private helper, so a script
 * and the runtime reach the same code: core's own detach path calls this
 * through IWireThread (ontology/Thread.h), because a detached script is a
 * Thread and core cannot allocate one.
 *
 * Logs the child's RID because that RID is the handle to it -- the DAG of
 * running jobs is the entity tree, so this is how you find it again.
 */
DEFINE_WORK_FUNC_TYPED(Shell, Detach, (std::string, path))
{
    (void)ctx;
    const uint64_t child = self.Detach(ETCS::Buffer(path.c_str()));
    if (!child) { ETCS_LOG("Shell::Detach", "refused '" << path << "'."); return; }
    ETCS_LOG("Shell::Detach", "'" << path << "' running as RID:" << child << ".");
}

/*
 * Bind <name> <data> -- put a value in this shell's closure.
 *
 * The closure travels to every child this shell detaches, by value, which is
 * what a detached job needs: it outlives the line that launched it, so a
 * reference into that line's scope is dead by the time it runs.
 *
 * Refuses rather than truncating when the value will not fit -- a silently
 * shortened value is a script that half-works with nothing to say why.
 */
DEFINE_WORK_FUNC_TYPED(Shell, Bind, (std::string, name), (std::string, value))
{
    (void)ctx;
    if (!self.Bind(ETCS::Buffer(name.c_str()), value.c_str(), value.size()))
        ETCS_LOG("Shell::Bind", "'" << name << "' rejected -- value does not fit "
                 "the standard buffer (" << ETCS::Buffer::bufsize << " bytes).");
}

// Stop accepting work. Cooperative (ontology/Threaded.h): the flag is the whole
// request, and a running body notices at its next opportunity.
DEFINE_WORK_FUNC(Shell, Halt)
{
    (void)ctx; (void)data;
    ETCS_LOG("Shell::Halt", "RID:" << self.getRID()
             << (self.Halt() ? " halting." : " was already halting."));
}

/*
 * Spawn <Provider::Tag> -- create a type from any module, through the loader.
 *
 * The gap this closes: a module could always create its OWN types (addTag<T>
 * compiles the type in) and the loader could create anyone's, but no module
 * could ask for another module's. Nothing was stopping it -- LoadEvent is
 * present in module builds and the ordering thread is the one that acts on it;
 * it simply had no caller.
 */
DEFINE_WORK_FUNC_TYPED(Shell, Spawn, (std::string, origin_tag))
{
    (void)ctx;
    const uint64_t rid = self.SpawnForeign(origin_tag);
    if (!rid) { ETCS_LOG("Shell::Spawn", "'" << origin_tag << "' produced nothing."); return; }
    ETCS_LOG("Shell::Spawn", "'" << origin_tag << "' -> RID:" << rid << ".");
}

DEFINE_WORK_FUNC(Shell, Report)
{
    (void)ctx; (void)data;
    self.Report();
}

DEFINE_WORK_FUNC(Shell, Delete)
{
    (void)ctx; (void)data;
    self.DeleteConcrete();
}

#endif // SHELLPROVIDER_H__
