# Clay

    https://github.com/nicbarker/clay          v0.14, MIT
    Copyright (c) 2024 Nic Barker -- full text in clay.h's header

A single-header immediate-mode layout solver. It is here because layout is a
solved problem that we were about to solve again badly: the 2D tree already
knows how to draw itself and where its children sit relative to it, and the one
thing it could not do was work out what those positions and sizes should BE
once the window stopped matching the numbers a script typed in.

Clay answers exactly that and nothing else. It computes boxes; it does not draw,
does not own a window, and does not care what a Drawable2D is. So it sits behind
`../Clayout.h`, which turns a set of ETCS entities into a Clay element tree,
runs a pass, and writes the resulting boxes back through the family verbs
`Drawable2D_::MoveTo` and `Resizable_::ResizeTo`. Nothing outside that file
includes clay.h, and clay.h names no ETCS type -- the two halves meet in
`Clayout.h` and nowhere else.

## Why it has a provider to itself

`LayoutProvider` exists so that this tree has a boundary that a directory can
express. Three things fall out of that and none of them would if Clay lived
inside RenderProvider:

- **Licence.** Clay is MIT inside an LGPL module repo. Confined to one module,
  the boundary is a path rather than a paragraph in a README.
- **Ownership.** A provider can be handed to someone else intact -- upstream
  included, if Nic ever wants it. A file wedged into somebody's renderer cannot.
- **Honesty about the dependency.** `Clayout` reaches its subjects by family
  name ("Drawable2D", "Resizable") and links against no renderer at all. Being
  its own module is what makes that checkable rather than merely claimed: if it
  ever grew a RenderProvider include, the build would say so.

## The one local change

`clay.h` line ~35: the language guard accepts `__cplusplus >= 201703L` instead
of `202002L`. Clay's own comment block there explains the reasoning in full.
Short version: only the `CLAY({...})` macro needs C++20 and we do not use it;
the implementation compiles under C++17.

Nothing else in this directory is modified.

## Warnings

Clay is C, read by a C++ compiler under `-Wall -Wextra`, so it emits a couple
of dozen diagnostics that are not defects: unused debug locals, an unused
parameter on a uniform callback signature, int/uint comparisons in loop bounds,
compound literals leaving trailing members zeroed.

They are silenced by a `#pragma GCC diagnostic push/ignored/pop` wrapped tightly
around the `#include` in `../Clayout.h` -- named individually, restored
immediately, and nowhere else in the module. The standard the build holds is
zero warnings, and `-w` on the module would have bought that by giving up the
warnings on code we actually write.

**If an upgrade emits a new one**, add a line to that pragma block with a note
saying what it is. Do not widen the scope.

## Build integration

Declared in the ACE manifest (`manifests/LayoutProvider.json`) as:

    "source": { "type": "vendored", "path": "clay" }, "hash_scope": false

`vendored` rather than `git` because the tree is committed here rather than
fetched, so there is no ref to pin and no fetch rule is emitted. `hash_scope`
false keeps it out of `module_hashes.h` -- the ABI attestation should move when
our code moves, not when a third party's does.

It IS included in ace's build fingerprint (`ace_build.py` excludes trees with
their own `.git`, and this has none), which is the wanted behaviour: editing
`clay.h` here is a commit in our history and should force a rebuild.

## On upgrade

1. Drop in the new `clay.h`.
2. Re-apply the language-guard edit.
3. Build, and add a pragma line for any new warning.
4. Update the version at the top of this file.

Nothing else in the tree refers to Clay by name.
