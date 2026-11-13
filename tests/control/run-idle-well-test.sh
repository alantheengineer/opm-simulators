#!/bin/bash
# Runs the control test on SPE1 with an extra well that never gets a control,
# so GetWellData has to report a well with no control mode.
#
# Usage: run-idle-well-test.sh FLOW CLIENT SPE1_DECK OUTPUT_DIR

here=$(dirname "$0")
mkdir -p "$4"
"$here/make-deck.py" idle "$3" "$4/IDLE.DATA" || exit 1
exec "$here/run-control-test.sh" "$1" "$2" "$4/IDLE.DATA" "$4/run"
