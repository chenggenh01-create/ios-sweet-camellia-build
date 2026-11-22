#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Reconstruct the files a unified diff adds, so a reference implementation can be
read as plain source instead of as patch context.

Usage: extract_new_files.py <patch> <out-dir>
"""
import os
import re
import sys

patch_path, outdir = sys.argv[1], sys.argv[2]
os.makedirs(outdir, exist_ok=True)

current = None
buf = []
files = []

with open(patch_path, encoding='utf-8', errors='replace') as fh:
    for line in fh:
        line = line.rstrip('\n')
        m = re.match(r'^\+\+\+ b/(.+)$', line)
        if m:
            if current:
                files.append((current, buf))
            current, buf = m.group(1), []
            continue
        if current is None:
            continue
        if line.startswith('@@'):
            continue
        if line.startswith('+') and not line.startswith('+++'):
            buf.append(line[1:])
        elif line.startswith(' '):
            buf.append(line[1:])
        elif line.startswith('diff ') or line.startswith('--- '):
            continue
if current:
    files.append((current, buf))

for path, lines in files:
    dst = os.path.join(outdir, path.replace('/', '__'))
    with open(dst, 'w', encoding='utf-8') as fh:
        fh.write('\n'.join(lines) + '\n')
    print('%-52s %6d lines' % (path, len(lines)))
