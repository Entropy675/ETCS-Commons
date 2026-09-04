#ifndef LAYOUTPROVIDER_CONTRACT__
#define LAYOUTPROVIDER_CONTRACT__

// LayoutProvider is OS-invariant and RENDERER-invariant, which is a stronger
// claim and the reason this module exists apart from RenderProvider.
//
// A layout solver reaches its subjects by family name -- "Drawable2D",
// "Resizable" -- so it arranges anything that claims those families, from any
// provider, and links against none of them. There is no platform fork here
// because there is no platform: it is arithmetic over rectangles.
//
// WHAT VENDORING CLAY MEANS FOR THIS FILE'S NEIGHBOURS. Clay is MIT and lives
// whole under clay/ with its provenance in clay/VENDORED.md. Confining it to
// one module makes the licence boundary a directory boundary rather than a
// paragraph in a README, and makes the module handable to someone else intact
// -- which is easier to do to a provider than to a file wedged inside one.

#include "../../ETCS.h"
#include "module_hashes.h"

#include "Clayout.h"

/*
 * THE CONTRACT NAME IS `Layout`. The implementation is `Clayout`.
 *
 * NOT for the reason the aliases next door exist. MbedTLSContext -> TLSContext
 * and GLFWWindow -> Window are platform indirection: the concrete type is one
 * of several possible backends and the contract name is what survives the
 * fork. Clay needs none of that -- it is a header-only C library that compiles
 * anywhere this runtime does, and there is no second solver behind an #ifdef.
 *
 * The alias is here because THE ETCS SURFACE NAMES TYPES FOR THEIR CAUSAL
 * ROLE. What a script spawns and hands RIDs to is the thing that decides where
 * everything sits; that role is `Layout`, and it would still be `Layout` if
 * the arithmetic underneath were replaced tomorrow. `Clayout` names the
 * implementation -- accurately, and as a pun on the library it wraps -- which
 * makes it exactly the wrong thing to put in a trace. A script that reads
 *
 *     spawn LayoutProvider::Layout bar
 *
 * says what is happening. `LayoutProvider::Clayout` says what we happened to
 * build it out of, and reads as a typo to anyone who has not been told the
 * joke. The surface is a vocabulary of roles, not a bill of materials.
 *
 * WIRE_TYPE_IDENTITY splits the two deliberately (ETCS_API.h): TAG is
 * stringized from the concrete class and stays "Clayout", so a backtrace still
 * names what it is standing in; the tag block below names the contract and
 * overwrites CONTRACT_TAG with "Layout", which is what the ridMap, the module
 * catalog and every script key on.
 */
typedef Clayout Layout;

#endif // LAYOUTPROVIDER_CONTRACT__
