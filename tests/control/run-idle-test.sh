#!/bin/bash
# Starts a deck under step control with no client, so it reaches the idle
# timeout before any report step, and checks that flow finishes cleanly.
#
# Usage: run-idle-test.sh FLOW DECK OUTPUT_DIR

set -uo pipefail

flow=$1
deck=$2
out=$3
name=$(basename "$deck" .DATA)

rm -rf "$out"
mkdir -p "$out"

if ! "$flow" "$deck" --output-dir="$out" --control-endpoint=127.0.0.1:0 --control-idle-timeout=2 \
    > "$out/flow.log" 2>&1; then
    tail -n 40 "$out/flow.log"
    echo "FAILED: flow did not exit cleanly after the idle timeout"
    exit 1
fi
if [ ! -f "$out/$name.EGRID" ] || [ ! -f "$out/$name.INIT" ]; then
    ls -l "$out"
    echo "FAILED: the grid and init files are missing"
    exit 1
fi
echo "flow finished cleanly after the idle timeout"
