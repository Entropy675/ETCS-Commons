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

/*
 * ONE Shell, AND THE FORK IS ONE LEVEL DOWN.
 *
 * There used to be a platform #if here choosing between WebShell.h and
 * LinuxShell.h, and then a typedef choosing between the two class names they
 * defined. Both files were the same file: identical apart from the class name,
 * the include guard, and which terminal they included. The typedef was the only
 * reason two names existed.
 *
 * The thing that actually has backends is the TERMINAL -- raw mode, line
 * editing, history, completion -- so the selection moved there (Shell.h) and
 * the contract name needs no indirection at all: `Shell` is the class. What a
 * script names is still the causal role, "the thing that runs my scripts", and
 * it is still the same name on every platform; there is simply no longer a
 * second spelling underneath it to keep in step.
 *
 * WIRE_TYPE_IDENTITY's TAG is therefore "Shell" too, where it used to be
 * "LinuxShell" so that a backtrace named which backend it was standing in.
 * That information did not go away -- it moved to where the backend is. A
 * frame in lsh::read_line names the terminal, which is the half that differs.
 */
#include "Shell.h"

#endif // SHELLPROVIDER_CONTRACT__
