#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Replay the patch series onto a copy of the local XNU worktree.

The CI applies every patch in xnu/patches in order onto the darwin-xnu-build
tree.  Doing the same locally gives us the exact file contents the next patch
must be written against, and proves the new patch applies before pushing.

Usage: replay_patches.py <worktree-src> <target-dir> [--check-only]
"""
import os
import re
import shutil
import subprocess
import sys

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = sys.argv[1]
DST = sys.argv[2]
check_only = '--check-only' in sys.argv
PATCHES = os.path.join(BASE, 'xnu', 'patches')


def run(args, cwd):
    return subprocess.run(args, cwd=cwd, capture_output=True, text=True)


def patch_key(name):
    m = re.match(r'^(\d{4})', name)
    return (0, int(m.group(1)), name) if m else (1, 0, name)


if not check_only and not os.path.exists(DST):
    print('copying worktree ->', DST, flush=True)
    shutil.copytree(SRC, DST)
    print('copied', flush=True)

names = sorted((n for n in os.listdir(PATCHES) if n.endswith('.patch')), key=patch_key)
applied, skipped, failed = 0, 0, []
for name in names:
    path = os.path.join(PATCHES, name)
    cmd = ['git', 'apply', '--check', '-p1', path]
    r = run(cmd, DST)
    if r.returncode == 0:
        if not check_only:
            a = run(['git', 'apply', '-p1', path], DST)
            if a.returncode != 0:
                failed.append((name, 'apply: ' + a.stderr.strip()[:160]))
                continue
        applied += 1
        print('applied  %s' % name, flush=True)
        continue
    # maybe already applied
    r2 = run(['git', 'apply', '--reverse', '--check', '-p1', path], DST)
    if r2.returncode == 0:
        skipped += 1
        print('present  %s' % name, flush=True)
        continue
    failed.append((name, r.stderr.strip().splitlines()[0][:160] if r.stderr.strip() else '?'))
    print('FAILED   %s' % name, flush=True)

print()
print('applied=%d present=%d failed=%d' % (applied, skipped, len(failed)))
for name, why in failed:
    print('  %-52s %s' % (name, why))
