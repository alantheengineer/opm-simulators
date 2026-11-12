#!/bin/bash
# Runs a deck normally and under step control, then checks that every output
# file apart from the logs is identical. SMSPEC is compared without the wall
# clock times it holds.
#
# Usage: run-control-test.sh FLOW CLIENT DECK OUTPUT_DIR

set -uo pipefail

flow=$1
client=$2
deck=$3
out=$4
here=$(dirname "$0")

fail() {
    echo "FAILED: $*"
    for run in normal control; do
        echo "--- $run.log, last 40 lines"
        tail -n 40 "$out/$run.log"
        echo "--- $run output files"
        ls -l "$out/$run"
    done
    exit 1
}

rm -rf "$out"
mkdir -p "$out/normal" "$out/control"

"$flow" "$deck" --output-dir="$out/normal" > "$out/normal.log" 2>&1 \
    || fail "normal run exited with $?"

port=$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
"$flow" "$deck" --output-dir="$out/control" --control-endpoint="127.0.0.1:$port" \
    > "$out/control.log" 2>&1 &
pid=$!

if ! "$client" "127.0.0.1:$port"; then
    kill "$pid" 2> /dev/null
    fail "client failed"
fi
wait "$pid" || fail "controlled run exited with $?"

compared=0
for file in "$out"/normal/*; do
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
[ "$compared" -gt 0 ] || fail "normal run wrote no output files"
echo "$compared output files identical"
