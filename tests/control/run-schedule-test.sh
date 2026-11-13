#!/bin/bash
# Changes an SPE1 deck under step control at report step 5 and checks that
# the output matches a deck with the same keywords written at that date.
# Before the change the client sends a request that must be refused, so a
# refused request that changed the run would show as a difference.
#
# Usage: run-schedule-test.sh FLOW CLIENT DECK OUTPUT_DIR [RANKS [CHANGE [REFUSED]]]
#
# CHANGE and REFUSED default to schedule-change.txt and schedule-refused.txt.

set -uo pipefail

flow=$1
client=$2
deck=$3
out=$4
ranks=${5:-1}
here=$(dirname "$0")
change=${6:-$here/schedule-change.txt}
refused=${7:-$here/schedule-refused.txt}
name=$(basename "$deck" .DATA)

launch=()
if [ "$ranks" -gt 1 ]; then
    launch=(mpirun -np "$ranks" --oversubscribe)
fi

fail() {
    echo "FAILED: $*"
    for run in reference control; do
        echo "--- $run.log, last 40 lines"
        tail -n 40 "$out/$run.log"
    done
    exit 1
}

rm -rf "$out"
mkdir -p "$out/reference" "$out/control" "$out/deck"

# SPE1 runs ten report steps from one TSTEP record. The reference deck splits
# it after five and puts the keywords in between.
python3 - "$deck" "$change" "$out/deck/$name.DATA" <<'PY' || fail "cannot write the reference deck"
import sys
deck, change, target = sys.argv[1:4]
text = open(deck).read()
if text.count("\n10*1 /") != 1:
    sys.exit("expected one TSTEP record of 10*1")
split = "\n5*1 /\n\n" + open(change).read() + "\nTSTEP\n5*1 /"
open(target, "w").write(text.replace("\n10*1 /", split))
PY

"${launch[@]}" "$flow" "$out/deck/$name.DATA" --output-dir="$out/reference" > "$out/reference.log" 2>&1 \
    || fail "reference run exited with $?"

port=$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
"${launch[@]}" "$flow" "$deck" --output-dir="$out/control" --control-endpoint="127.0.0.1:$port" \
    > "$out/control.log" 2>&1 &
pid=$!

if ! "$client" "127.0.0.1:$port" --stop-at-step=5 --schedule-file="$change" \
    --refused-schedule-file="$refused"; then
    kill "$pid" 2> /dev/null
    fail "client failed"
fi
wait "$pid" || fail "controlled run exited with $?"

compared=0
for file in "$out"/reference/*; do
    base=$(basename "$file")
    case "$base" in
        *.PRT | *.DBG) continue ;;
    esac
    [ -f "$out/control/$base" ] || fail "controlled run did not write $base"
    if [[ "$base" == *.SMSPEC ]]; then
        "$here/compare-smspec.py" "$file" "$out/control/$base" || fail "$base differs"
    else
        cmp "$file" "$out/control/$base" || fail "$base differs"
    fi
    compared=$((compared + 1))
done
[ "$compared" -gt 0 ] || fail "reference run wrote no output files"
echo "$compared output files match the reference deck"
