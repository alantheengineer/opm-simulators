#!/usr/bin/env bash
# Run clang-tidy on the sources changed since BASE and report warnings on the
# changed lines only, in sources and headers. Upstream code in a touched file
# is not held to these rules.
# Usage: check-tidy.sh BUILD_DIR BASE
set -euo pipefail

build_dir=$1
merge_base=$(git merge-base "$2" HEAD)

mapfile -t changed < <(git diff --name-only --diff-filter=AM "$merge_base" -- '*.cpp' '*.cc')

# Only sources in this build have compile flags to check with.
mapfile -t sources < <(python3 - "$build_dir/compile_commands.json" "${changed[@]}" <<'PY'
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
PY
)

if [ "${#sources[@]}" -eq 0 ]; then
    echo "No changed sources."
    exit 0
fi

# Changed line ranges of every changed file, as clang-tidy's -line-filter.
lines=$(git diff -U0 --diff-filter=AM "$merge_base" -- '*.cpp' '*.cc' '*.hpp' '*.hh' '*.h' | python3 -c '
import json
import re
import sys

ranges = {}
name = None
for line in sys.stdin:
    if line.startswith("+++ b/"):
        name = line[6:].rstrip("\n")
    elif line.startswith("@@") and name:
        start, _, count = re.match(r"@@ -\S+ \+(\d+)(,(\d+))? @@", line).groups()
        count = 1 if count is None else int(count)
        if count > 0:
            ranges.setdefault(name, []).append([int(start), int(start) + count - 1])
print(json.dumps([{"name": n, "lines": r} for n, r in ranges.items()]))
')

clang-tidy-18 -p "$build_dir" --quiet \
    --warnings-as-errors='*' \
    --header-filter='.*' \
    --line-filter="$lines" \
    --extra-arg=-Wno-unknown-warning-option \
    "${sources[@]}"
