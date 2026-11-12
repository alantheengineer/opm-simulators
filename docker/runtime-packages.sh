#!/bin/bash
# Prints the Ubuntu packages that provide the shared libraries the given
# programs load.
#
# Usage: runtime-packages.sh PROGRAM...

set -euo pipefail

ldd "$@" | awk '$2 == "=>" && $3 ~ /^\// { print $3 }' | sort -u | while read -r lib; do
    # Ubuntu records some libraries under /usr/lib and others under /lib.
    dpkg -S "$lib" 2> /dev/null \
        || dpkg -S "$(realpath "$lib")" 2> /dev/null \
        || dpkg -S "${lib#/usr}" 2> /dev/null \
        || { echo "No package provides $lib" >&2; exit 1; }
done | cut -d: -f1 | sort -u
