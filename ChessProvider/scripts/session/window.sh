#!/bin/bash
# THE SEAT IN A WINDOW, driven by verb: the OS root script
# (scripts/chess_table.etcs) under a virtual screen, its table clicked and
# typed at through the prompt the way the window's streams would, and the
# window photographed. Needs xvfb-run and ImageMagick's `import`; without a
# display of its own this is how the OS side is seen at all.
#
#     modules/ChessProvider/scripts/session/window.sh      (from ACE_ROOT)
#
# Checks that clicks move (e2e4 e7e5 through the board's own frame), that the
# bars take a line and a verb, and that a refused verb is said in the note.
# Leaves OUT/1.png (the seat), OUT/2.png (the bars in) to look at.
S=$(cd "$(dirname "$0")" && pwd); cd "$S/../../../.."
OUT=${OUT:-/tmp/etcs-chess-window}; rm -rf "$OUT"; mkdir -p "$OUT"
command -v xvfb-run >/dev/null || { echo "xvfb-run is needed"; exit 2; }
cat > "$OUT/probe.etcs" <<'P'
#!/usr/bin/env etcs
table.Click(420 604)
table.Click(420 428)
table.Click(420 164)
table.Click(420 340)
table.Toggle()
table.Type(hello from the window)
table.Submit()
table.Type(/nope)
table.Submit()
table.Type(a line still being typed)
table.Report()
game.Fen()
P
cat > "$OUT/drive.sh" <<D
#!/bin/bash
cd "$PWD"
mkfifo "$OUT/w.fifo"
(bin/etcs < "$OUT/w.fifo" > "$OUT/w.out" 2>&1 & echo \$! > "$OUT/w.pid")
exec 3>"$OUT/w.fifo"
echo "scripts/chess_table.etcs" >&3; sleep 8
import -window root "$OUT/1.png"
echo "$OUT/probe.etcs table=table game=game" >&3; sleep 4
import -window root "$OUT/2.png"
kill \$(cat "$OUT/w.pid")
D
chmod +x "$OUT/drive.sh"
xvfb-run -a -s "-screen 0 1280x900x24" "$OUT/drive.sh"
fail=0
check() { if [ "$1" = "$2" ]; then echo "  ok    $3"; else echo "  FAIL  $3 -- '$1'"; fail=1; fi; }
rep=$(grep -a "workFunc\]: seat" "$OUT/w.out" | tail -1 | sed 's/.*workFunc\]: //')
fen=$(grep -a -A1 "game.Fen()" "$OUT/w.out" | grep -a "workFunc" | tail -1 | sed 's/.*workFunc\]: //')
echo "  $rep"
check "$(echo "$rep" | grep -c "bars in, line 'a line still being typed', 30 chat row(s), note '/nope: not found'")" "1" "the bars are in, the line is typed, the refused verb is the note"
check "$fen" "rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 2" "two clicks a move: e2e4 e7e5 on the board"
[ $fail = 0 ] && echo PASSED || echo FAILED
exit $fail
