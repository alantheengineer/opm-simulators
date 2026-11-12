#!/usr/bin/env bash
# Run clang-tidy on the sources changed since BASE. Warnings from changed
# files are errors; warnings from untouched upstream headers are ignored.
# Usage: check-tidy.sh BUILD_DIR BASE
set -euo pipefail

build_dir=$1
merge_base=$(git merge-base "$2" HEAD)

mapfile -t changed < <(git diff --name-only --diff-filter=AM "$merge_base" -- '*.cpp' '*.cc')

# Only sources in this build have compile flags to check with.
mapfile -t sources < <(python3 - "$build_dir/compile_commands.json" "${changed[@]}" <<'EOF'
import json
import os
import sys

entries = json.load(open(sys.argv[1]))
built = {os.path.realpath(os.path.join(e["directory"], e["file"])) for e in entries}
for path in sys.argv[2:]:
    if os.path.realpath(path) in built:
        print(path)
    else:
        print("Not in the build, not checked:", path, file=sys.stderr)
EOF
)
mapfile -t headers < <(git diff --name-only --diff-filter=AM "$merge_base" -- '*.hpp' '*.hh' '*.h')

if [ "${#sources[@]}" -eq 0 ]; then
    echo "No changed sources."
    exit 0
fi

filter='^$'
if [ "${#headers[@]}" -gt 0 ]; then
    filter="($(printf '%s|' "${headers[@]}" | sed 's/|$//; s/\./\\./g'))\$"
fi

clang-tidy-18 -p "$build_dir" --quiet \
    --warnings-as-errors='*' \
    --header-filter="$filter" \
    --extra-arg=-Wno-unknown-warning-option \
    "${sources[@]}"
