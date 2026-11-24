#!/bin/bash
# Copy the file set in scripts/paths.txt (+ tests, cmake, patches) from a 3FS checkout. Read-only on 3FS.
# usage: scripts/export.sh <3FS root>
set -e
SRC=${1:?usage: export.sh <3FS root>}
cd "$(dirname "$0")/.."
while read -r p; do mkdir -p "$(dirname "$p")"; cp -p "$SRC/$p" "$p"; done < scripts/paths.txt
for d in tests/common/net tests/common/serde tests/main; do mkdir -p "$d"; cp -rp "$SRC/$d/." "$d/"; done
cp -p "$SRC/tests/GtestHelpers.h" tests/
mkdir -p cmake patches
cp -p "$SRC"/cmake/{CompileFlags,Sanitizers}.cmake cmake/
cp -p "$SRC"/patches/folly.patch patches/
cp -p "$SRC/LICENSE" LICENSE
git -C "$SRC" rev-parse HEAD > UPSTREAM_COMMIT
