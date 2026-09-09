#ifndef SHELLPROVIDER_CONTRACT__
#define SHELLPROVIDER_CONTRACT__

// ShellProvider is where the platform-specific half of running ETCS scripts
// finally has somewhere to live.
//
// The split this module exists to make: COMMAND EXECUTION is core, because it
// defines what an ETCS line means and that is a property of the language.
// RUNNING commands -- a terminal, raw mode, a prompt, line editing -- is
// platform-specific, and until now it sat in ShellREPL.h beside core behind an
// ETCS_LOADER guard because there was no provider to own it. There is now.
//
// CommandExecutor does not move. Its script-running entry points are already
// visible to every module (core_defs.h -> ETCS_API.h -> CommandExecutor.h);
// only ShellREPL.h was ever gated. So the shell reaches execution directly and
// needs no hook.

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
