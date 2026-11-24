#!/bin/bash
# Bring upstream 3FS changes to the exported files into this repo (3FS is only read).
# usage: scripts/sync_upstream.sh <3FS root> [new commit, default HEAD]
set -e
SRC=${1:?usage: sync_upstream.sh <3FS root> [commit]}
NEW=$(git -C "$SRC" rev-parse "${2:-HEAD}")
cd "$(dirname "$0")/.."
OLD=$(cat UPSTREAM_COMMIT)
[ "$OLD" = "$NEW" ] && { echo "already at $NEW"; exit 0; }
PATHS=$(cat scripts/paths.txt; echo tests/common/net tests/common/serde tests/main tests/GtestHelpers.h patches/folly.patch)
# Files we changed on purpose (see README "Differences from 3FS") may conflict: resolve by hand, then rerun gen_paths.
git -C "$SRC" diff "$OLD" "$NEW" -- $PATHS | git apply -3 --index
echo "$NEW" > UPSTREAM_COMMIT
python3 scripts/gen_paths.py "$SRC" | diff - scripts/paths.txt || echo "NOTE: upstream include closure changed, review the diff above and update paths.txt"
python3 scripts/check_deps.py
