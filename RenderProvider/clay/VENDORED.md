# Clay

    https://github.com/nicbarker/clay          v0.14, MIT

A single-header immediate-mode layout solver. It is here because layout is a
solved problem that we were about to solve again badly: the 2D tree already
knows how to draw itself and where its children sit relative to it, and the one
thing it could not do was work out what those positions and sizes should BE
once the window stopped matching the numbers a script typed in.

Clay answers exactly that and nothing else. It computes boxes; it does not draw,
does not own a window, and does not care what a Drawable2D is. So it sits behind
`RenderProvider/ClayLayout.h`, which turns a set of ETCS entities into a Clay
element tree, runs a pass, and writes the resulting boxes back through the
family verbs `Drawable2D_::MoveTo` and `Resizable_::ResizeTo`. Nothing outside
that file includes clay.h, and clay.h names no ETCS type -- the two halves meet
only in ClayLayout.h.

## The one local change

`clay.h` line ~35: the language guard accepts `__cplusplus >= 201703L` instead
of `202002L`. Clay's own comment block there explains the reasoning in full.
Short version: only the `CLAY({...})` macro needs C++20 and we do not use it;
the implementation compiles under C++17.

Its own `-Wmissing-field-initializers` noise (several hundred lines, from C
compound literals that are fine as written) is silenced across the `#include`
in ClayLayout.h and nowhere else, so the module keeps building `-Wall -Wextra`
clean for the code we do write.

**On upgrade:** drop in the new `clay.h` and re-apply that one edit. Nothing
else here is modified.
