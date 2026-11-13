#!/bin/bash
# Runs the schedule test on a brine or tracer variant of SPE1, changing what
# the injector injects at report step 5.
#
# Usage: run-composition-test.sh FLOW CLIENT SPE1_DECK VARIANT OUTPUT_DIR

set -uo pipefail

here=$(dirname "$0")
variant=$4
out=$5
name=$(basename "$3" .DATA)

mkdir -p "$out/base"
"$here/make-deck.py" "$variant" "$3" "$out/base/$name.DATA" || exit 1
exec "$here/run-schedule-test.sh" "$1" "$2" "$out/base/$name.DATA" "$out/run" 1 \
    "$here/$variant-change.txt" "$here/schedule-refused.txt"
