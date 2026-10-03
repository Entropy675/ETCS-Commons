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
 *             [Thread + Deletable + Lifecycle]. One class, Shell.h; the
 *             platform fork is the Terminal, not the Shell -- the contract
 *             name says the role, not the terminal API (Shell.h).
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

/*
 * ── The console, as four ordinary actions ───────────────────────────────────
 *
 * A terminal is something a Shell DOES, so it is dispatch and nothing else --
 * no export, no dlsym, no registry, no hook. The loader calls
 * `shell->call("Shell.ReadLine", data, sig)` exactly as a script calls any
 * other action, and the reply comes back in the same buffer the prompt went out
 * in, because WorkFunc has always taken `Buffer&`.
 *
 * RAW, not TYPED: DEFINE_WORK_FUNC_TYPED parses the buffer into arguments and
 * is the right macro for `Run <path>`. These need the buffer itself, in both
 * directions.
 *
 * THE REPLY PROTOCOL (documented at THE SHELL SEAM, core/CommandExecutor.h):
 *
 *     out   '+' <line>   a line was read; the line may legitimately be empty
 *           '-'          the source is finished
 *           <nothing>    this shell does not hold the console -- also finished
 *
 * The status byte is what separates "you pressed enter" from "there is nothing
 * more to read", which a bare buffer cannot say and which the navigator's loop
 * turns on: one continues, the other leaves.
 */
DEFINE_WORK_FUNC(Shell, OpenConsole)
{
    (void)ctx;
    data.clear();
    if (!lsh::claim_console(self.getRID()))
    {
        ETCS_LOG("Shell::OpenConsole", "RID:" << self.getRID()
                 << " asked for the console; another shell already holds it.");
        return;
    }
    lsh::load_history();
    data.writeString("+");
}

DEFINE_WORK_FUNC(Shell, CloseConsole)
{
    (void)ctx;
    data.clear();
    if (!lsh::owns_console(self.getRID())) return;
    lsh::save_history();
    lsh::release_console(self.getRID());
}

DEFINE_WORK_FUNC(Shell, ReadLine)
{
    const std::string prompt = data.toString();
    data.clear();

    // Not the console holder, or in teardown: nothing to read from, which is
    // the same answer as a closed stdin.
    if (!lsh::owns_console(self.getRID()) || self.Halted()) return;
    if (ctx.isInterrupted() || ctx.isTerminated()) { data.writeString("-"); return; }

    const std::string line = lsh::read_line(prompt, ctx);
    if (ctx.isInterrupted() || ctx.isTerminated()) { data.writeString("-"); return; }

    /*
     * REFUSED, NOT TRUNCATED. The reply rides a Buffer -- 256 bytes -- so a
     * line has 254 characters and a status byte's worth of room. A silently
     * shortened line is a command the operator did not type, executing anyway;
     * saying so and re-prompting costs them one retype and costs nothing else.
     * Same stance ThreadBase::Bind takes on an over-long closure value.
     */
    if (line.size() + 1 >= ETCS::Buffer::bufsize)
    {
        std::cout << "\n[Shell] line too long (" << line.size() << " chars; the limit is "
                  << (ETCS::Buffer::bufsize - 2) << ") -- not run.\n" << std::flush;
        data.writeString("+");          // empty line: the loop re-prompts
        return;
    }

    std::string reply(1, '+');
    reply += line;
    data.writeString(reply.c_str());
}

/*
 * attach <socket> -- drive another runtime's control socket from this console.
 *
 * Console-holder only, and that is not a policy choice: the relay borrows this
 * process's own line editor, so a shell that is not holding it has nothing to
 * lend. A socket session asking gets the same refusal, which is the correct
 * answer rather than a restriction.
 */
DEFINE_WORK_FUNC(Shell, Attach)
{
    const std::string path = data.toString();
    data.clear();
    if (!lsh::owns_console(self.getRID())) return;
    if (path.empty()) return;
    lsh::attach(path, ctx);
}

#endif // SHELLPROVIDER_H__
