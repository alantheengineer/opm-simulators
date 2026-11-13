#!/bin/bash
# Brings opm-common and opm-simulators up to date with OPM.
#
# For each fork it moves master to OPM's master, then opens a pull request
# from a sync/<date> branch that merges it into deploy. The opm-simulators
# pull request also moves the opm-grid pin in .github/upstream-pins.env.
# OPM's three masters are read at the same moment, so changes OPM made across
# repositories arrive together. Nothing is merged into deploy here.
#
# If the merge conflicts, the pull request is opened from OPM's commit alone
# and lists the files; resolve them on its branch.
#
# Environment:
#   GH_TOKEN  token with contents, pull request and workflow write access to
#             both forks
#   OWNER     the account that owns the forks
#
# Usage: upstream-sync.sh

set -euo pipefail

owner=${OWNER:?}
: "${GH_TOKEN:?}"
today=$(date -u +%Y-%m-%d)
branch="sync/$today"
work=$(mktemp -d)
summary=${GITHUB_STEP_SUMMARY:-/dev/stdout}
trap 'rm -rf "$work"' EXIT

declare -A upstream
for repo in opm-common opm-simulators opm-grid; do
    upstream[$repo]=$(git ls-remote "https://github.com/OPM/$repo.git" refs/heads/master | cut -f1)
    test -n "${upstream[$repo]}"
done
echo "OPM master: opm-common ${upstream[opm-common]}, opm-simulators" \
    "${upstream[opm-simulators]}, opm-grid ${upstream[opm-grid]}" >> "$summary"

# Merged upstream pull requests in a range, one line each, without links so
# OPM's pull requests are not notified.
upstream_prs() {
    git -C "$1" log --first-parent --format='%s' "$2" \
        | sed -n 's/^Merge pull request #\([0-9]*\) from \(.*\)$/- `\1` \2/p'
}

sync_repo() {
    local repo=$1 sha=${upstream[$1]} dir="$work/$1"
    if gh pr list -R "$owner/$repo" --base deploy --state open --json headRefName \
            --jq '.[].headRefName' | grep -q '^sync/'; then
        echo "$repo: a sync pull request is already open, skipped" >> "$summary"
        return
    fi

    git clone -q --filter=blob:none --no-checkout \
        "https://x-access-token:$GH_TOKEN@github.com/$owner/$repo.git" "$dir"
    git -C "$dir" config user.name "Dr Alan Tominey"
    git -C "$dir" config user.email "132053438+alantheengineer@users.noreply.github.com"
    git -C "$dir" fetch -q origin deploy
    git -C "$dir" fetch -q "https://github.com/OPM/$repo.git" "$sha"

    local base pins=""
    base=$(git -C "$dir" merge-base origin/deploy "$sha")
    if [ "$base" = "$sha" ]; then
        echo "$repo: deploy already has OPM master" >> "$summary"
        return
    fi
    # master only ever mirrors OPM, so this is a fast-forward. It fails if
    # anything was committed to master.
    git -C "$dir" push -q origin "$sha:refs/heads/master"

    git -C "$dir" checkout -q -B "$branch" origin/deploy
    local conflicts=""
    if git -C "$dir" merge -q --no-ff -m "Merge OPM/$repo master into deploy" "$sha"; then
        if [ "$repo" = opm-simulators ]; then
            sed -i -e "s|^# OPM/opm-grid master on .*|# OPM/opm-grid master on $today, matching the $today sync.|" \
                -e "s|^OPM_GRID_REF=.*|OPM_GRID_REF=${upstream[opm-grid]}|" \
                "$dir/.github/upstream-pins.env"
            if ! git -C "$dir" diff --quiet; then
                git -C "$dir" commit -q -am "Move the opm-grid pin to OPM master of $today"
                pins="The opm-grid pin moves to \`${upstream[opm-grid]}\`, OPM's master at the same moment."
            fi
        fi
        git -C "$dir" push -q -f origin "$branch"
    else
        conflicts=$(git -C "$dir" diff --name-only --diff-filter=U | sed 's/^/- /')
        git -C "$dir" merge --abort
        git -C "$dir" push -q -f origin "$sha:refs/heads/$branch"
    fi

    local count body
    count=$(git -C "$dir" rev-list --count "$base..$sha")
    body="Merges OPM/$repo master at \`$sha\` into deploy: $count commits.

OPM pull requests:
$(upstream_prs "$dir" "$base..$sha")
"
    if [ -n "$pins" ]; then
        body+="
$pins
"
    fi
    if [ -n "$conflicts" ]; then
        body+="
**These files conflict with deploy.** This branch holds OPM's commit alone. Merge deploy into it, resolve them, then move the opm-grid pin if this is opm-simulators.
$conflicts
"
    fi
    body+="
Merge with **Create a merge commit**, not squash or rebase, so the next sync starts from here. Merge the opm-common pull request first."

    local url
    url=$(gh pr create -R "$owner/$repo" --base deploy --head "$branch" \
        --title "Sync with OPM master, $today" --body "$body")
    echo "$repo: $count commits, $url${conflicts:+ (conflicts)}" >> "$summary"
}

sync_repo opm-common
sync_repo opm-simulators
