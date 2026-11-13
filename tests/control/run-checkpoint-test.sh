#!/bin/bash
# Saves a checkpoint of SPE1 under step control at report step 5 and runs
# to the end, then starts a second run from the checkpoint with --load-file.
# Its restart and summary files from step 6 on must match a run started from
# a checkpoint that flow itself saved with --save-step. A resumed run differs
# from an uninterrupted one by round-off in near-zero saturations, so that
# is not the comparison. The controlled run itself must match the run that
# saved with --save-step, so saving does not change it. A run from the
# checkpoint under step control must report step 5 before it steps and
# match too.
#
# Usage: run-checkpoint-test.sh FLOW CLIENT DECK OUTPUT_DIR

set -uo pipefail

flow=$1
client=$2
deck=$3
out=$4
name=$(basename "$deck" .DATA)

fail() {
    echo "FAILED: $*"
    for run in control resume controlled-resume saved reference; do
        echo "--- $run.log, last 30 lines"
        tail -n 30 "$out/$run.log"
    done
    exit 1
}

rm -rf "$out"
mkdir -p "$out/control" "$out/resume" "$out/controlled-resume" "$out/saved" "$out/reference"

port=$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
"$flow" "$deck" --output-dir="$out/control" --control-endpoint="127.0.0.1:$port" \
    > "$out/control.log" 2>&1 &
pid=$!
if ! "$client" "127.0.0.1:$port" --stop-at-step=5 --checkpoint-label=half; then
    kill "$pid" 2> /dev/null
    fail "client failed"
fi
wait "$pid" || fail "controlled run exited with $?"

checkpoint="$out/control/$name-half.OPMRST"
[ -f "$checkpoint" ] || fail "no checkpoint file $checkpoint"

"$flow" "$deck" --output-dir="$out/resume" --load-file="$checkpoint" --load-step=5 \
    > "$out/resume.log" 2>&1 || fail "run from the checkpoint exited with $?"

port=$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
"$flow" "$deck" --output-dir="$out/controlled-resume" --load-file="$checkpoint" --load-step=5 \
    --control-endpoint="127.0.0.1:$port" > "$out/controlled-resume.log" 2>&1 &
pid=$!
if ! "$client" "127.0.0.1:$port" --stop-at-step=end > "$out/controlled-resume.client.log"; then
    kill "$pid" 2> /dev/null
    fail "client of the run from the checkpoint failed"
fi
wait "$pid" || fail "controlled run from the checkpoint exited with $?"
grep -q "^starting at report step 5$" "$out/controlled-resume.client.log" \
    || fail "controlled run from the checkpoint did not start at report step 5"

"$flow" "$deck" --output-dir="$out/saved" --save-step=5 --save-file="$out/saved/$name.OPMRST" \
    > "$out/saved.log" 2>&1 || fail "run saving with --save-step exited with $?"
"$flow" "$deck" --output-dir="$out/reference" --load-file="$out/saved/$name.OPMRST" --load-step=5 \
    > "$out/reference.log" 2>&1 || fail "run from the --save-step checkpoint exited with $?"

compared=0
for step in 0006 0007 0008 0009 0010; do
    for kind in X S; do
        file="$name.$kind$step"
        [ -f "$out/reference/$file" ] || continue
        for run in resume controlled-resume; do
            [ -f "$out/$run/$file" ] || fail "$run did not write $file"
            cmp "$out/reference/$file" "$out/$run/$file" || fail "$run: $file differs"
            compared=$((compared + 1))
        done
    done
done
[ "$compared" -gt 0 ] || fail "no files to compare"

# Saving must not change the run that saved.
for file in "$out"/saved/"$name".[SX][0-9]*; do
    cmp "$file" "$out/control/$(basename "$file")" || fail "saving changed the controlled run: $(basename "$file")"
done
echo "$compared files match a run from flow's own checkpoint, and saving left the run unchanged"
