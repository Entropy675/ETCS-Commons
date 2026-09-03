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

#endif // LAYOUTPROVIDER_CONTRACT__
