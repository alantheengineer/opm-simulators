#!/bin/bash
# Runs a deck under step control over TLS with a token, using a certificate
# made for the test, and checks that the output matches a normal run.
#
# Usage: run-tls-test.sh FLOW CLIENT DECK OUTPUT_DIR

set -uo pipefail

flow=$1
client=$2
deck=$3
out=$4
here=$(dirname "$0")

fail() {
    echo "FAILED: $*"
    tail -n 40 "$out/control.log"
    exit 1
}

rm -rf "$out"
mkdir -p "$out/normal" "$out/control" "$out/tls"

openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj /CN=localhost \
    -addext "subjectAltName=IP:127.0.0.1,DNS:localhost" \
    -keyout "$out/tls/key.pem" -out "$out/tls/cert.pem" > /dev/null 2>&1 || fail "cannot make a certificate"
echo "tls-test-token" > "$out/tls/token"

"$flow" "$deck" --output-dir="$out/normal" > "$out/normal.log" 2>&1 || fail "normal run exited with $?"

port=$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
"$flow" "$deck" --output-dir="$out/control" --control-endpoint="127.0.0.1:$port" \
    --control-token-file="$out/tls/token" --control-tls-cert="$out/tls/cert.pem" \
    --control-tls-key="$out/tls/key.pem" > "$out/control.log" 2>&1 &
pid=$!

if ! "$client" "127.0.0.1:$port" --token-file="$out/tls/token" --tls-ca="$out/tls/cert.pem"; then
    kill "$pid" 2> /dev/null
    fail "client failed over TLS"
fi
wait "$pid" || fail "controlled run exited with $?"

name=$(basename "$deck" .DATA)
"$here/compare-smspec.py" "$out/normal/$name.SMSPEC" "$out/control/$name.SMSPEC" || fail "SMSPEC differs"
for file in "$out"/normal/"$name".[SX][0-9]*; do
    cmp "$file" "$out/control/$(basename "$file")" || fail "$(basename "$file") differs"
done
echo "run over TLS matches the normal run"
