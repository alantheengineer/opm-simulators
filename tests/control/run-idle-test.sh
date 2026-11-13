#!/bin/bash
# Starts a deck under step control with no client, so it reaches the idle
# timeout before any report step, and checks what flow does for the given
# --control-on-disconnect policy:
#   pause   exits cleanly after writing the grid and init files
#   finish  runs to the end of the schedule and exits cleanly
#   abort   exits with an error
#
# Usage: run-idle-test.sh FLOW DECK OUTPUT_DIR [POLICY]

set -uo pipefail

flow=$1
deck=$2
out=$3
policy=${4:-pause}
name=$(basename "$deck" .DATA)

rm -rf "$out"
mkdir -p "$out"

"$flow" "$deck" --output-dir="$out" --control-endpoint=127.0.0.1:0 --control-idle-timeout=2 \
    --control-on-disconnect="$policy" > "$out/flow.log" 2>&1
status=$?

fail() {
    tail -n 40 "$out/flow.log"
    ls -l "$out"
    echo "FAILED: $*"
    exit 1
}

case "$policy" in
    pause)
        [ "$status" -eq 0 ] || fail "flow did not exit cleanly after the idle timeout"
        if [ ! -f "$out/$name.EGRID" ] || [ ! -f "$out/$name.INIT" ]; then
            fail "the grid and init files are missing"
        fi
        ;;
    finish)
        [ "$status" -eq 0 ] || fail "flow did not exit cleanly after running to the end"
        if [ ! -f "$out/$name.X0010" ] && [ ! -f "$out/$name.UNRST" ]; then
            fail "no restart output for the last report step"
        fi
        ;;
    abort)
        [ "$status" -ne 0 ] || fail "flow exited cleanly but should have aborted"
        ;;
    *)
        fail "unknown policy $policy"
        ;;
esac
echo "flow handled the idle timeout with --control-on-disconnect=$policy"
