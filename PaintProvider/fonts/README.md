# Fonts for text boxes

Loaded by `PaintFonts` (PaintProvider.h) from `boot_paint_panels.etcs`, and staged
into the browser like the scripts are (`assets` in `scripts/www/modules.json`).
They ship with the program, not borrowed from the machine, because a text box in a
shared session has to wrap at the same words on every page in the room, and that
only holds when every page measures with the same outlines.

| font | file | for |
|------|------|-----|
| 1 sans  | InstrumentSans-Regular.ttf    | plain captions |
| 2 serif | YoungSerif-Regular.ttf        | titles |
| 3 mono  | RedHatMono-Regular.ttf        | code, tables |
| 4 hand  | NothingYouCouldDo-Regular.ttf | handwriting |

Font 0 is the sheet's own 5x8 pixel font (RenderProvider::TextLabel), not a file.

Each is unmodified and licensed under the SIL Open Font License 1.1; the licence
and copyright for each is the `*-OFL.txt` beside it, and none reserves its name.

## Getting the files

The `.ttf` files are added to this folder by hand (they are binary, so they do
not travel in patches). The copies used were taken from the `canvas-fonts` folder
of https://github.com/anthropics/skills (`skills/canvas-design/canvas-fonts/`), and
they are the same fonts each project publishes under OFL. Check against:

    a22cb26e48fd79bcb01bf2fc92d36785474dce36d9c544ab0a8868c2657c4a87  InstrumentSans-Regular.ttf
    f8dc08f77abad753a00670af70756a8ace938e5c3f0b770f4f4c2071c4bd8fc6  YoungSerif-Regular.ttf
    452fe826871b37539f5212b20c87cf30f82f58dd2741f1c96edd1dcbdc0db6b4  RedHatMono-Regular.ttf
    d866f985896d3280f4fce72db7e17302c24a0c1fdb0699b6b5ed3af14f944d57  NothingYouCouldDo-Regular.ttf

(`sha256sum -c` reads those lines as they are.)

A missing file is not an error. Its number keeps its place, so font 3 is still
mono in a document saved elsewhere, and a box in that font draws in the pixel
font until the file is there: `PaintFonts::Load` answers `missing`, `Report` says
`(not loaded: pixel font)`, and the page's loader logs `skipped` and carries on.
Every page in a shared session has to have the same set, or they wrap the same
box at different words.
