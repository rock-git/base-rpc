#!/usr/bin/env python3
"""Fail if any #include "..." under src/ or tests/ points outside this repo (e.g. back into 3FS)."""
import os, re, sys

root = os.path.normpath(os.path.join(os.path.dirname(__file__), ".."))
INC = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)
tp = os.path.join(root, "third_party")  # quoted includes of third-party headers are fine
THIRD_PARTY = tuple(os.path.join(tp, d) for d in ("folly", "toml11", "scnlib/include", "fmt/include", "zstd/lib",
                                                  "googletest/googletest/include", "googletest/googlemock/include"))
bad = []
for top in ("src", "tests", "examples"):
    for r, _, fs in os.walk(os.path.join(root, top)):
        for n in fs:
            if not n.endswith((".h", ".hpp", ".cc")):
                continue
            f = os.path.join(r, n)
            for m in INC.findall(open(f, errors="ignore").read()):
                roots = (os.path.dirname(f), os.path.join(root, "src"), root, os.path.join(root, "compat")) + THIRD_PARTY
                if not any(os.path.isfile(os.path.join(b, m)) for b in roots):
                    bad.append(f"{os.path.relpath(f, root)}: \"{m}\"")
print("\n".join(bad) or "ok: all quoted includes resolve inside the repo")
sys.exit(1 if bad else 0)
