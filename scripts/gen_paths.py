#!/usr/bin/env python3
"""Compute the file set to export from 3FS: include-closure of common/net + common/serde,
minus the parts replaced by shims or cut (monitor, memory, app, kv, fbs).
usage: gen_paths.py <3FS root>  > scripts/paths.txt   (paths relative to the 3FS root)"""
import os, re, sys

root = sys.argv[1]
src = os.path.join(root, "src")
EXCLUDE = ("common/monitor/", "memory/common/", "common/app/", "common/kv/", "fbs/")
INC = re.compile(r'#include [<"]([^">]+)[">]')
EXTRA = []

def excluded(p): return p.startswith(EXCLUDE)

seeds = [os.path.relpath(os.path.join(r, n), src)
         for d in ("common/net", "common/serde") for r, _, fs in os.walk(os.path.join(src, d)) for n in fs
         if n.endswith((".h", ".cc", ".hpp"))]
seen, todo = set(seeds), list(seeds)
while todo:
    p = todo.pop()
    cands = [p]
    if p.endswith((".h", ".hpp")):
        cands.append(p.rsplit(".", 1)[0] + ".cc")  # e.g. Toml.hpp -> Toml.cc
    for c in cands:
        if not os.path.isfile(os.path.join(src, c)) or excluded(c):
            continue
        if c not in seen:
            seen.add(c)
        else:
            if c != p:
                continue
        for m in INC.findall(open(os.path.join(src, c), errors="ignore").read()):
            # include paths are relative to src/ or to the including file
            for cand in (m, os.path.normpath(os.path.join(os.path.dirname(c), m))):
                if os.path.isfile(os.path.join(src, cand)) and not excluded(cand) and cand not in seen:
                    seen.add(cand); todo.append(cand)
seen.update(EXTRA)
for p in sorted(seen):
    print("src/" + p)
