#!/usr/bin/env bash
# Recompile the sources changed since BASE with strict warnings.
# Usage: check-warnings.sh BUILD_DIR BASE
set -euo pipefail

build_dir=$1
base=$2

mapfile -t files < <(git diff --name-only --diff-filter=AM "$(git merge-base "$base" HEAD)" -- '*.cpp' '*.cc')
if [ "${#files[@]}" -eq 0 ]; then
    echo "No changed sources."
    exit 0
fi

python3 - "$build_dir/compile_commands.json" "${files[@]}" <<'EOF'
import json
import os
import shlex
import subprocess
import sys

entries = json.load(open(sys.argv[1]))
wanted = {os.path.realpath(f) for f in sys.argv[2:]}
failed = False

for entry in entries:
    path = os.path.realpath(os.path.join(entry["directory"], entry["file"]))
    if path not in wanted:
        continue
    wanted.discard(path)

    args = entry.get("arguments") or shlex.split(entry["command"])
    if os.path.basename(args[0]) == "ccache":
        args = args[1:]
    cmd = []
    skip_next = False
    for arg in args:
        if skip_next:
            skip_next = False
        elif arg == "-o":
            skip_next = True
        elif arg != "-c":
            cmd.append(arg)
    cmd += ["-fsyntax-only", "-Wall", "-Wextra", "-Wpedantic", "-Werror"]

    print("Checking", os.path.relpath(path), flush=True)
    if subprocess.run(cmd, cwd=entry["directory"]).returncode != 0:
        failed = True

for path in sorted(wanted):
    print("Not in the build, not checked:", os.path.relpath(path))

sys.exit(1 if failed else 0)
EOF
