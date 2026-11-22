#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Turn the 0176 GPU stub into the full Metal-capable accelerator.

The stub's code sits in SweetFramebuffer.cpp; its own patch hunk carries the
exact context that the tree has at that point of the series, so replacing the
added lines (and only those) keeps the patch applying while the block grows to
the real service: Metal properties, a user client with GET_VERSION/GET_PARAM,
and register reads forwarded to bsd/dev/sweet_touch_mtk.c.

Usage: mk_0176_gpu_service.py
"""
import io
import os
import re

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PATCH = os.path.join(BASE, 'xnu', 'patches', '0176-camellia-gpu-stub-v2.patch')
BLOCK = os.path.join(BASE, 'tools', 'drafts', 'gpu_stub_block.txt')
BACKUP = os.path.join(BASE, 'evidence', 'research', 'viva_ref', '0176-original.patch')

src = io.open(PATCH, encoding='utf-8', errors='replace').read()
if not os.path.exists(BACKUP):
    io.open(BACKUP, 'w', encoding='utf-8').write(src)
    print('backup written:', BACKUP)

block = io.open(BLOCK, encoding='utf-8', errors='replace').read()
block_lines = block.split('\n')
while block_lines and block_lines[-1] == '':
    block_lines.pop()

lines = src.split('\n')
out = []
i = 0
replaced = False
while i < len(lines):
    line = lines[i]
    m = re.match(r'^@@ -(\d+),(\d+) \+(\d+),(\d+) @@(.*)$', line)
    if not m or replaced:
        out.append(line)
        i += 1
        continue
    # This hunk is the SweetFramebuffer.cpp block: collect old/new counts by walking it.
    start = i
    i += 1
    ctx = []
    added = []
    while i < len(lines) and not lines[i].startswith(('@@', 'diff --git')):
        l = lines[i]
        if l.startswith('+'):
            added.append(l[1:])
        elif l.startswith(' '):
            ctx.append(l)
        elif l.startswith('-'):
            ctx.append(l)          # deletions do not occur in this hunk
        else:
            break
        i += 1
    if 'IOAcceleratorES' not in '\n'.join(added):
        out.extend(lines[start:i])
        continue
    old_count = len(ctx)
    new_count = len(ctx) + len(block_lines)
    header = '@@ -%s,%d +%s,%d @@' % (m.group(1), old_count, m.group(3), new_count)
    out.append(header)
    out.extend(ctx)
    out.extend('+' + bl for bl in block_lines)
    replaced = True

if not replaced:
    raise SystemExit('GPU hunk not found in 0176')

io.open(PATCH, 'w', encoding='utf-8', newline='\n').write('\n'.join(out))
print('rewrote', PATCH, 'bytes', os.path.getsize(PATCH))
