#!/bin/bash
# Times a deck run normally and under step control, to measure what control
# costs. Each run is timed from launch to exit and the median of REPEATS runs
# is reported. Control is timed in two modes: step, one Step call per report
# step, and advance, one AdvanceTo call for the whole schedule.
#
# Usage: run-benchmark.sh FLOW CLIENT DECK OUTPUT_DIR [RANKS] [REPEATS] [FLOW_ARGS...]
#
# Prints one line per mode: deck, ranks, mode, the median and each run in
# seconds, and the overhead of the median against the normal run. Not run by
# ctest, as timings on shared machines are too noisy to assert on.

set -uo pipefail

flow=$1
client=$2
deck=$3
out=$4
ranks=${5:-1}
repeats=${6:-3}
shift $(($# < 6 ? $# : 6))
args=("$@")

launch=()
if [ "$ranks" -gt 1 ]; then
    launch=(mpirun -np "$ranks" --oversubscribe)
fi

now() {
    date +%s.%N
}

median() {
    sort -g | awk '{ v[NR] = $1 } END { print (NR % 2) ? v[(NR + 1) / 2] : (v[NR / 2] + v[NR / 2 + 1]) / 2 }'
}

normal() {
    "${launch[@]}" "$flow" "$deck" "${args[@]}" --output-dir="$1" > "$1.log" 2>&1
}

controlled() {
    local dir=$1 stop=$2 port pid
    port=$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
    "${launch[@]}" "$flow" "$deck" "${args[@]}" --output-dir="$dir" \
        --control-endpoint="127.0.0.1:$port" > "$dir.log" 2>&1 &
    pid=$!
    if ! "$client" "127.0.0.1:$port" --stop-at-step="$stop" > "$dir.client.log" 2>&1; then
        kill "$pid" 2> /dev/null
        wait "$pid"
        return 1
    fi
    wait "$pid"
}

# Runs one mode REPEATS times and prints each wall time.
time_mode() {
    local mode=$1 i start dir
    for ((i = 1; i <= repeats; i++)); do
        dir="$out/$mode-$i"
        rm -rf "$dir"
        mkdir -p "$dir"
        start=$(now)
        case "$mode" in
            normal) normal "$dir" ;;
            step) controlled "$dir" 1 ;;
            advance) controlled "$dir" end ;;
        esac || { echo "FAILED: $mode run $i, see $dir.log" >&2; exit 1; }
        echo "$(now) - $start" | bc
    done
}

# Prints the median and every run, for example "12.10 s (12.31 12.10 11.98)".
summary() {
    printf '%.2f s (%s)' "$(median <<< "$1")" "$(printf '%.2f\n' $1 | paste -sd ' ')"
}

mkdir -p "$out"
name=$(basename "$deck" .DATA)
times=$(time_mode normal) || exit 1
base=$(median <<< "$times")
echo "$name ranks=$ranks normal $(summary "$times")"
for mode in step advance; do
    times=$(time_mode "$mode") || exit 1
    t=$(median <<< "$times")
    printf '%s ranks=%s %s %s overhead %+.1f%%\n' "$name" "$ranks" "$mode" "$(summary "$times")" \
        "$(echo "100 * ($t - $base) / $base" | bc -l)"
done
