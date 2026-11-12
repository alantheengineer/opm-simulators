#!/usr/bin/env bash
# Fail if lines changed since BASE do not follow .clang-format.
# Usage: check-format.sh BASE
set -euo pipefail

base=$(git merge-base "$1" HEAD)
out=$(git clang-format-18 --binary clang-format-18 --diff "$base" -- '*.hpp' '*.cpp' '*.hh' '*.cc' || true)

if printf '%s\n' "$out" | grep -q '^diff --git'; then
    printf '%s\n' "$out"
    exit 1
fi
echo "Formatting is clean."
