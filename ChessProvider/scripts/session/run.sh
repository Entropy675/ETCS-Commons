#!/bin/bash
# ETCS HOSTING ETCS: chess between native runtimes, driven from here -- a
# site and two players, each an `etcs` reading scripts off a fifo, exactly as
# a page's runtime runs them (www/index.html fills the same templates in).
# The two models the chess page plays, back to back:
#
#   THE CASUAL GAME, peer to peer: alice hosts, her board judges; bob links to
#   her through the site's hub and follows her record. The site is the name
#   server only (the hall).
#     1  the hall lists the site's ranked tables; alice's table appears when
#        she opens it
#     2  bob joins; both boards agree
#     3  alice moves, bob moves: each move is one record line, both agree
#     4  bob proposes an illegal move: refused by the judge, NOT recorded --
#        the record's seq does not move and the boards agree
#     5  the hall shows the seats taken
#     6  bob resigns; both boards say so; bob leaves and his seat is released
#     7  alice closes her table: gone from the hall
#
#   THE RANKED GAME, server-mediated: the site hosts table1 and its board
#   judges, holding no seat -- one more observer in front of the record.
#     8  alice and bob join table1; the hall shows its seats taken
#     9  moves both ways; all three boards agree (the site's, through Who)
#    10  alice resigns: the site records the outcome
#    11  both leave: the table is reset and listed open again
#
#     modules/ChessProvider/scripts/session/run.sh      (from ACE_ROOT)
#
# Needs bin/etcs built with -DETCS_REPL_SHELL (the interactive loader) and
# port 8458 free -- a runtime left on it from an earlier run is the first
# thing to suspect when step 1 lists nothing. Exits 1 if any check fails.
S=$(cd "$(dirname "$0")" && pwd); cd "$S/../../../.."; ROOT=$PWD
T="$ROOT/modules/ChessProvider/scripts"
OUT=${OUT:-/tmp/etcs-chess-session}; rm -rf "$OUT"; mkdir -p "$OUT"
HUB=ws://127.0.0.1:8458/link
for n in s a b; do mkfifo "$OUT/$n.fifo"; done
(bin/etcs < "$OUT/s.fifo" > "$OUT/s.out" 2>&1 & echo $! > "$OUT/s.pid")
(bin/etcs < "$OUT/a.fifo" > "$OUT/a.out" 2>&1 & echo $! > "$OUT/a.pid")
(bin/etcs < "$OUT/b.fifo" > "$OUT/b.out" 2>&1 & echo $! > "$OUT/b.pid")
exec 3>"$OUT/s.fifo" 4>"$OUT/a.fifo" 5>"$OUT/b.fifo"
trap 'kill $(cat "$OUT"/*.pid) 2>/dev/null' EXIT

# The templates, filled as the page fills them.
fill() { sed -e "s/{{n}}/$2/g" -e "s/{{name}}/$3/g" -e "s/{{id}}/$4/g" -e "s/{{owner}}/$5/g" -e "s#{{hub}}#$HUB#g" "$T/$1.etcs" > "$OUT/$3_$1_$2.etcs"; echo "$OUT/$3_$1_$2.etcs"; }
A() { echo "$1 ${2-game=game share=share}" >&4; }
B() { echo "$1 ${2-game=game share=share}" >&5; }
act() { local f="$OUT/act_$RANDOM.etcs"; printf '#!/usr/bin/env etcs\n%s\n' "$2" > "$f"; echo "$f ${3-game=game share=share}" >&$1; }

fail=0
say() { echo; echo "== $*"; }
check() { if [ "$1" = "$2" ]; then echo "  ok    $3"; else echo "  FAIL  $3 -- '$1' vs '$2'"; fail=1; fi; }
# The last answer to a verb in an output, by the line the verb is on.
last() { grep -a -A"${3:-1}" "$2" "$1" | grep -a "workFunc\]:" | tail -1 | sed 's/.*workFunc\]: //'; }
hashof() { A "$OUT/alice_look.etcs"; B "$OUT/bob_look.etcs"; sleep 2; ah=$(last "$OUT/a.out" "game.Hash()"); bh=$(last "$OUT/b.out" "game.Hash()"); }
# The hall, as alice's mirror lists it -- only the LAST listing: awk keeps the
# block after the final "hall.List()" and stops at the next prompt, so a count
# never spans two listings.
hall()   { A "$OUT/list.etcs" "hall=hall1"; sleep 2; awk '
  /CommandExecutor\] hall.List\(\)/ { cur=""; blk=1; dat=0; next }
  blk && /workFunc\]:/ { l=$0; sub(/.*workFunc\]: /,"",l); if(l!="") cur=cur l "\n"; dat=1; next }
  blk && dat && /^[a-z]/ { cur=cur $0 "\n"; next }
  blk && /Root>|Navigator|ETCS_LOG/ { blk=0; last=cur }
  END { printf "%s", last }
' "$OUT/a.out"; }
# The hall, retried until a line matches (the mirror is eventually consistent,
# as the page's poll is): up to ~10s.
hall_row() { local want="$1" i; for i in $(seq 1 8); do r=$(hall | grep "^$2"); [ "$r" = "$want" ] && break; sleep 1; done; echo "$r"; }

sed 's/{{name}}/alice/g' "$S/look.etcs" > "$OUT/alice_look.etcs"; sed 's/{{name}}/bob/g' "$S/look.etcs" > "$OUT/bob_look.etcs"
printf '#!/usr/bin/env etcs\nhall.List()\n' > "$OUT/list.etcs"

echo "$S/site.etcs" >&3; sleep 4
A "$S/seat.etcs" ""; B "$S/seat.etcs" ""; sleep 1
A "$(fill chess_hall 1 alice - -)" ""; B "$(fill chess_hall 1 bob - -)" ""; sleep 3

say "1 the hall: the site's tables, then alice's"
check "$(hall | grep -c ranked)" "2" "two ranked tables are listed"
A "$(fill chess_host 2 alice alice -)" "game=game share=share hall=halllist1"; sleep 3
check "$(hall | grep '^alice')" "alice chess alice open open" "alice's table is listed, both seats open"

say "2 bob joins alice"
B "$(fill chess_join 2 bob alice alice)"; sleep 4
hashof; echo "  A $ah  B $bh"; check "$ah" "$bh" "both boards agree"
check "$(last "$OUT/a.out" "share.Who()" 3 | wc -l)" "1" "alice sees bob (Who has two lines)"

say "3 moves both ways, one record line each"
act 4 "game.Act(alice move e2e4)"; sleep 2
act 5 "game.Act(bob move e7e5)"; sleep 3
hashof; echo "  A $ah  B $bh"; check "$ah" "$bh" "both boards agree after e2e4 e7e5"
check "${ah#* }" "2" "the record is at seq 2"

say "4 an illegal move from bob: refused by the judge, not recorded"
act 5 "game.Act(bob move e5e3)"; sleep 3
hashof; echo "  A $ah  B $bh"; check "$ah" "$bh" "both boards agree"
check "${ah#* }" "2" "the record did not move"
grep -a "refused\|ILLEGAL" "$OUT/a.out" | grep -a ChessGame | tail -1 | sed 's/.*ChessGame\] /  judge: /'

say "5 the hall shows the seats"
check "$(hall_row 'alice chess alice alice bob' alice)" "alice chess alice alice bob" "alice's table: alice white, bob black"

say "6 bob resigns and leaves; his seat is released"
act 5 "game.Act(bob resign)"; sleep 3
A "$OUT/alice_look.etcs"; sleep 2
check "$(last "$OUT/a.out" "game.Status(alice)" | cut -d' ' -f2)" "resign-black" "alice's board: black resigned"
B "$T/chess_leave.etcs" "share=share peer=peer2"; sleep 3
A "$OUT/alice_look.etcs"; sleep 2
check "$(last "$OUT/a.out" "game.Status(alice)" | cut -d' ' -f8)" "-" "black's seat is open again on alice's board"
grep -a "bob left" "$OUT/a.out" | tail -1 | sed 's/.*ChessGame\] /  /'

say "7 alice closes her table"
A "$T/chess_close.etcs" "share=share room=room2"; sleep 3
check "$(hall | grep -c '^alice')" "0" "alice's table is gone from the hall"
check "$(hall | grep -c ranked)" "2" "the ranked tables remain"

say "8 the ranked game: both join table1 and sit"
A "$(fill chess_join 3 alice table1 table1)"; B "$(fill chess_join 3 bob table1 table1)"; sleep 4
act 4 "game.Act(alice sit white)"; sleep 2; act 5 "game.Act(bob sit black)"; sleep 3
hashof   # a beat, so each guest's presence reaches the table
check "$(last "$OUT/a.out" "game.Status(alice)" | cut -d' ' -f4-5)" "taken taken" "both seats are taken on alice's board"

say "9 moves both ways; the site's board agrees; the hall shows the seats"
act 4 "game.Act(alice move d2d4)"; sleep 2; act 5 "game.Act(bob move d7d5)"; sleep 3
hashof; sh=$(last "$OUT/a.out" "share.Who()" 3 | grep '^table1' | cut -d' ' -f3-4); echo "  A $ah  B $bh  S $sh"
check "$ah" "$bh" "alice and bob agree"; check "$ah" "$sh" "and the site's board is the same"
# The seat holders, from the record every board replays -- the same two names
# the hall advertises (ChessShare::Tick reads them off this board), read here
# where they are authoritative. Not off a second hall link: a room refuses a
# name already connected, so a guest reopening the hall does so under a fresh
# name; the seats themselves are the record's. Status: "... whiteName blackName".
seats=$(last "$OUT/a.out" "game.Status(alice)" | awk '{print $(NF-1), $NF}')
check "$seats" "alice bob" "the seats are alice (white) and bob (black)"

say "10 alice resigns: the outcome is the site's"
act 4 "game.Act(alice resign)"; sleep 3
check "$(grep -a 'outcome: resign-white white=alice black=bob' "$OUT/s.out" | wc -l)" "1" "the site recorded resign-white, alice vs bob"

say "11 both leave: the table is reset and open again"
A "$T/chess_leave.etcs" "share=share peer=peer3"; B "$T/chess_leave.etcs" "share=share peer=peer3"; sleep 4
check "$(grep -ac 'table1: the table is reset' "$OUT/s.out")" "1" "the site reset table1"
check "$(hall_row 'table1 ranked table1 open open' table1)" "table1 ranked table1 open open" "table1 is listed open again"

echo; [ $fail = 0 ] && echo "PASSED" || echo "FAILED"
exit $fail
