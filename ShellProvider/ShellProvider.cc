#include "ShellProvider.h"

// One tag. BASIC because a shell owns no continuous pump of its own: Run
// blocks for the length of a script and returns, and Detach hands the work to
// a child rather than starting a loop here.
ETCS_MODULE_EXPORT_MAIN(ShellProvider, "Shell")

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

ETCS_TAG_BLOCK_BASIC(Shell,
    Create, Run, Spawn, Detach, Bind, Halt, Report, Delete,
    ReadLine, Attach, OpenConsole, CloseConsole)

