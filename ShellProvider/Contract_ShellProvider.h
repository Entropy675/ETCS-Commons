#ifndef SHELLPROVIDER_CONTRACT__
#define SHELLPROVIDER_CONTRACT__

// ShellProvider is where the platform-specific half of running ETCS scripts
// finally has somewhere to live.
//
// ROOT IS THE ENTRY POINT FOR THE ONTOLOGY; SHELL IS THE ENTRY POINT FOR
// LIFETIMES AND SIGNALS. A Root attaches a module and hands out typed entities
// -- it answers "what exists". A Shell is a control structure: it holds signal
// authority, carries a closure, and is the only kind of entity that can produce
// another control thread. So "what is running, and how do I stop it" is a
// question about a Shell, and running anything at all is therefore something a
// Shell does -- which is why the interactive loader loads this module before it
// shows a prompt (drive_main_loop_then_exit, core/CommandExecutor.h).
//
// The split this module exists to make: COMMAND EXECUTION is core, because it
// defines what an ETCS line means and that is a property of the language.
// RUNNING commands -- a terminal, raw mode, a prompt, line editing -- is
// platform-specific, and until now it sat in ShellREPL.h beside core behind an
// ETCS_LOADER guard because there was no provider to own it. There is now, and
// ShellREPL.h is gone: its navigator half went to core/CommandExecutor.h, whose
// client it always was, and its terminal half came here (Linux/LinuxTerminal.h).
//
// CommandExecutor does not move. Its script-running entry points are already
// visible to every module (core_defs.h -> ETCS_API.h -> CommandExecutor.h). So
// the shell reaches execution directly and needs no hook.
//
// THE ONE HOOK RUNS THE OTHER WAY. Core asks the terminal for a line, not the
// reverse -- ReplLineSource, which the navigator has taken since before a
// socket session existed. Linux/LinuxTerminal.h publishes that seam onto this
// module's own EventNode at static-init and the loader adopts it during
// registration (EventNode::ShellTerminal), so it costs no exported symbol and
// no exports.map entry. A hook pointing the other way would have made the
// terminal a caller of the executor, which is the arrangement this module
// exists to undo.

#include "../../ETCS.h"
#include "module_hashes.h"

#include "Linux/LinuxShell.h"

/*
 * THE CONTRACT NAME IS `Shell`. The implementation is `LinuxShell`.
 *
 * Platform indirection, the same as MbedTLSContext -> TLSContext and
 * GLFWWindow -> Window: the concrete type is one of several possible backends
 * and the contract name is what survives the fork. A WinShell would sit behind
 * the same name and every script that spawns `ShellProvider::Shell` keeps
 * working, because what a script names is the causal role -- "the thing that
 * runs my scripts" -- not which terminal API it happens to be built on.
 *
 * WIRE_TYPE_IDENTITY keeps the two apart (ETCS_API.h): TAG stays "LinuxShell"
 * so a backtrace still names what it is standing in, while CONTRACT_TAG becomes
 * "Shell", which is what the ridMap, the module catalog and every script key on.
 */
#if defined(_WIN32) || defined(WIN32)
    // No WinShell yet -- the fork is declared so adding one is a file, not a
    // redesign, and so this file states plainly what is missing.
    #error "ShellProvider has no Windows backend yet; see Contract_ShellProvider.h"
#else
    typedef LinuxShell Shell;
#endif

#endif // SHELLPROVIDER_CONTRACT__
