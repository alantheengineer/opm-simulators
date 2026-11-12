#!/bin/bash
# Shallow checkout of a branch, tag or commit.
#
# Usage: fetch REPO REF DIR

set -euo pipefail

git init -q "$3"
git -C "$3" fetch -q --depth 1 "$1" "$2"
git -C "$3" checkout -q FETCH_HEAD
