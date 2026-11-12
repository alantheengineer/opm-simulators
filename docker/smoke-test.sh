#!/bin/bash
# Runs SPE1 under control in the image, on one rank and on two, and checks
# that each run steps to the end and writes its output.
#
# Usage: docker/smoke-test.sh IMAGE

set -euo pipefail

image=$1
deck=python/test_data/SPE1CASE1a

run() {
    local ranks=$1
    local work
    work=$(mktemp -d)
    chmod 777 "$work"
    cp "$deck"/* "$work/"
    echo "smoke-test-token" > "$work/token"

    docker run -d --name opm-smoke -v "$work:/data" -e OPM_RANKS="$ranks" -e CONTROL_IDLE_TIMEOUT=60 \
        -e CONTROL_TOKEN_FILE=/data/token "$image" /data/SPE1CASE1.DATA --output-dir=/data/out > /dev/null

    local ok=0
    if ! docker exec opm-smoke control_step_client 127.0.0.1:50051 --token-file=/data/token; then
        ok=1
        docker kill opm-smoke > /dev/null
    fi
    local status
    status=$(docker wait opm-smoke)
    if [ "$ok" -ne 0 ] || [ "$status" -ne 0 ] || [ ! -f "$work/out/SPE1CASE1.SMSPEC" ]; then
        docker logs opm-smoke | tail -n 40
        docker rm opm-smoke > /dev/null
        echo "FAILED on $ranks rank(s)"
        exit 1
    fi
    docker rm opm-smoke > /dev/null
    echo "SPE1 under control on $ranks rank(s): passed"
}

run 1
run 2
