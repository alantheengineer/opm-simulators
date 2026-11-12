#!/bin/bash
# Starts flow under step control. Arguments go to flow, for example the deck
# and --output-dir.
#
# OPM_RANKS             MPI ranks, default 1
# OPM_THREADS           threads per rank, default 1
# CONTROL_ENDPOINT      address to listen on, default 0.0.0.0:50051. Set it
#                       empty to run the deck to the end without control.
# CONTROL_TOKEN_FILE    file holding the bearer token. Required unless the
#                       endpoint is a loopback address.
# CONTROL_IDLE_TIMEOUT  seconds without a command before the run finishes,
#                       default 900

set -euo pipefail

ranks=${OPM_RANKS:-1}
endpoint=${CONTROL_ENDPOINT-0.0.0.0:50051}

args=(--threads-per-process="${OPM_THREADS:-1}")
if [ -n "$endpoint" ]; then
    args+=(--control-endpoint="$endpoint" --control-idle-timeout="${CONTROL_IDLE_TIMEOUT:-900}")
    if [ -n "${CONTROL_TOKEN_FILE:-}" ]; then
        args+=(--control-token-file="$CONTROL_TOKEN_FILE")
    fi
fi

if [ "$ranks" -gt 1 ]; then
    exec mpirun -np "$ranks" --bind-to none flow "${args[@]}" "$@"
fi
exec flow "${args[@]}" "$@"
