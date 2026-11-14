#!/bin/bash
# ETCS HOSTING ETCS: a shared canvas between three native runtimes, driven
# from here -- a host and two guests, each an `etcs` reading scripts off a
# fifo, exactly as a page's runtime would run them (www/index.html fills the
# same templates in). What it checks is the session's whole life:
#
#   1  a guest joins and follows the host's page (PictureHash agrees)
#   2  the host makes them a writer: the key goes by post (a mailbox on the
#      guest's own link), and the guest opens the way in
#   3  what the writer draws, the host has
#   4  the host restates the page; the record is checkpointed there, and a
#      late guest starts from the checkpoint and agrees
#   5  a text box the host holds is refused to the writer (a hold line)
#   6  the writer is demoted: the key turns, the next stroke is refused by
#      the seal and comes off the writer's own canvas
#   7  a guest leaves: withdrawn from presence, the rest go on
#   8  the host ends the session: every guest's record ends
#
#     modules/PaintProvider/scripts/session/run.sh      (from ACE_ROOT)
#
# Needs bin/etcs built with -DETCS_REPL_SHELL (the interactive loader) and
# port 8457 free. Prints the eight steps; exits 1 if any hash disagrees.
S=$(cd "$(dirname "$0")" && pwd); cd "$S/../../../.."; ROOT=$PWD
OUT=${OUT:-/tmp/etcs-paint-session}; mkdir -p "$OUT"
for n in h a b; do rm -f "$OUT/$n.fifo"; mkfifo "$OUT/$n.fifo"; done
(bin/etcs < "$OUT/h.fifo" > "$OUT/h.out" 2>&1 &); (bin/etcs < "$OUT/a.fifo" > "$OUT/a.out" 2>&1 &); (bin/etcs < "$OUT/b.fifo" > "$OUT/b.out" 2>&1 &)
exec 3>"$OUT/h.fifo" 4>"$OUT/a.fifo" 5>"$OUT/b.fifo"
sed 's/{{name}}/alice/g' "$S/guest.etcs" > "$OUT/alice.etcs"; sed 's/{{name}}/bob/g' "$S/guest.etcs" > "$OUT/bob.etcs"
H() { echo "$S/$1.etcs ${2:-share=share doc=doc}" >&3; }
A() { echo "$S/$1.etcs ${2:-share=share doc=doc}" >&4; }
B() { echo "$S/$1.etcs ${2:-share=share doc=doc}" >&5; }
hash() { grep -a "workFunc\]: [0-9a-f]\{16\} [0-9]* [01]" "$1" | tail -1 | sed 's/.*workFunc\]: //' | cut -d' ' -f1; }
fail=0
agree() { local first=$1; shift; for h in "$@"; do [ "$h" = "$first" ] || fail=1; done; }
say() { echo; echo "== $*"; }
echo "$S/host.etcs" >&3; sleep 6
say "1 alice joins"; echo "$OUT/alice.etcs" >&4; sleep 8
H tick; A tick; sleep 3; h=$(hash "$OUT/h.out"); a=$(hash "$OUT/a.out"); echo "H $h  A $a"; agree "$h" "$a"
say "2 alice is promoted: the key goes by post, alice opens the way in"
H promote; sleep 2
KEY=$(grep -a "share.Key()" -A1 "$OUT/h.out" | grep -a workFunc | tail -1 | sed 's/.*workFunc\]: //' | tr -d ' ')
printf 'spawn NetworkProvider::Ledger amail\namail.spawn(NetworkProvider::Remote ramail)\nramail.Bind(mail @room alice)\namail.Append(key %s)\n' "$KEY" > "$OUT/grant.etcs"
echo "$OUT/grant.etcs room=room" >&3; sleep 3
grep -a "given this page drawing" "$OUT/a.out" | tail -1 | sed 's/.*::PaintShare\] //'
A write "doc=doc intake=intake"; sleep 2
say "3 alice draws; the host has it"; A draw1; sleep 4; H tick; A tick; sleep 3; h=$(hash "$OUT/h.out"); a=$(hash "$OUT/a.out"); echo "H $h  A $a"; agree "$h" "$a"
say "4 the host draws, restates; bob joins late"; H draw2; sleep 2; H restate; H draw3; sleep 5
grep -a "checkpointed" "$OUT/h.out" | tail -1 | sed 's/.*PaintDocument\] //'
echo "$OUT/bob.etcs" >&5; sleep 8; grep -a "record starts at\|following the session" "$OUT/b.out" | sed 's/.*PaintDocument\] //'
H tick; A tick; B tick; sleep 3; h=$(hash "$OUT/h.out"); a=$(hash "$OUT/a.out"); b=$(hash "$OUT/b.out"); echo "H $h  A $a  B $b"; agree "$h" "$a" "$b"
say "5 a text box: the host holds it, alice is refused"; H textbox; sleep 4; A grab; sleep 2
grep -a "until they let go" "$OUT/a.out" | tail -1 | sed 's/.*PaintDocument\] //'
H untext; sleep 3
say "6 alice is demoted: the key turns; what she draws next comes off"; H demote; sleep 3
A draw4; sleep 8
grep -a "did not take\|comes back off" "$OUT/a.out" | tail -2 | sed 's/.*\] //'
H tick; A tick; B tick; sleep 3; h=$(hash "$OUT/h.out"); a=$(hash "$OUT/a.out"); b=$(hash "$OUT/b.out"); echo "H $h  A $a  B $b"; agree "$h" "$a" "$b"
say "7 alice leaves; the host draws on; bob has it"; A leave "peer=peer"; sleep 3; H draw5; sleep 4
grep -a "withdrawn" "$OUT/h.out" | tail -1 | sed 's/.*\] //'
H tick; B tick; sleep 3; h=$(hash "$OUT/h.out"); b=$(hash "$OUT/b.out"); echo "H $h  B $b"; agree "$h" "$b"
say "8 the host ends the session"; H close "share=share room=room record=record"; sleep 4
grep -a "session has ended" "$OUT/b.out" | tail -1 | sed 's/.*PaintShare\] //'
echo exit >&5; echo exit >&4; sleep 1; echo exit >&3; exec 3>&- 4>&- 5>&-; sleep 3
if grep -aq "preflight: refusing\|Segmentation\|Aborted" "$OUT"/*.out; then grep -a "preflight: refusing\|Segmentation\|Aborted" "$OUT"/*.out | head -3; fail=1; fi
echo; [ $fail = 0 ] && echo "session: all steps agree" || echo "session: FAILED"
exit $fail
