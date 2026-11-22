#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Audit every file in the repository.

Downloads the repository tarball once, then checks the tree locally:
  1. empty / suspiciously small files
  2. git blob SHA of every file vs the tree entry (detects truncated entries)
  3. UTF-8 decode + mojibake markers in text files
  4. relative links in every markdown file point at existing paths
  5. python files compile; patch files pass hunk arithmetic
  6. files that exist locally (README, docs, tools, patches) match byte for byte

Usage: audit_repo.py [--work DIR]
Exit code: 0 if no blocking issue, 1 otherwise.
"""
import argparse
import hashlib
import io
import json
import os
import re
import subprocess
import sys
import tarfile
import urllib.request

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.dirname(BASE)
REPO = 'hyperOS-li/ios-sweet-camellia-build'
API = 'https://api.github.com'

TEXT_EXT = {'.md', '.py', '.ps1', '.sh', '.patch', '.yml', '.yaml', '.c', '.h',
            '.txt', '.json', '.toml', '.plist', '.entitlements'}
MOJIBAKE = ['锘', 'Ã', 'â€', 'ï¿½', '\ufffd']

# local file -> repo path, for byte-for-byte comparison
LOCAL_MAP = {
    'README.md': 'evidence/research/repo_README_new.md',
    'docs/40_GPU方案_参考项目对照与移植计划.md': '40_GPU方案_参考项目对照与移植计划.md',
    'docs/41_交接_2026-11-22_GPU假服务打通与下一步.md': '41_交接_2026-11-22_GPU假服务打通与下一步.md',
    'docs/USERDATA_方案.md': 'docs_USERDATA_方案.md',
    'docs/source/sweet_touch_mtk_v54.c': 'payload/out/n2/sweet_touch_mtk_v54.c',
    'tools/check_patch.py': 'tools/check_patch.py',
    'tools/autopack_watch.py': 'tools/autopack_watch.py',
    'tools/vidscan.py': 'tools/vidscan.py',
    'tools/replay_patches.py': 'tools/replay_patches.py',
    'tools/extract_new_files.py': 'tools/extract_new_files.py',
    'tools/mk_v50_installer.py': 'tools/mk_v50_installer.py',
    'tools/mk_v52_gpu_exports.py': 'tools/mk_v52_gpu_exports.py',
    'tools/mk_v53_gpu_start.py': 'tools/mk_v53_gpu_start.py',
    'tools/mk_v54_gpu_sticky.py': 'tools/mk_v54_gpu_sticky.py',
    'tools/mk_0176_gpu_service.py': 'tools/mk_0176_gpu_service.py',
    'tools/drafts/gpu_stub_block.txt': 'tools/drafts/gpu_stub_block.txt',
    'xnu/patches/0176-camellia-gpu-stub-v2.patch': 'xnu/patches/0176-camellia-gpu-stub-v2.patch',
    'xnu/patches/0268-camellia-gpu-exports.patch': 'xnu/patches/0268-camellia-gpu-exports.patch',
    'xnu/patches/0269-camellia-gpu-start.patch': 'xnu/patches/0269-camellia-gpu-start.patch',
    'xnu/patches/0270-camellia-gpu-sticky.patch': 'xnu/patches/0270-camellia-gpu-sticky.patch',
}


def token():
    for p in (os.path.join(ROOT, '.gh_token'), os.path.join(BASE, '.gh_token')):
        if os.path.exists(p):
            return open(p).read().strip()
    return ''


def api(url):
    req = urllib.request.Request(url, headers={
        'Authorization': 'Bearer ' + token(), 'User-Agent': 'x',
        'Accept': 'application/vnd.github+json'})
    with urllib.request.urlopen(req, timeout=120) as r:
        return json.load(r)


def git_blob_sha(data):
    h = hashlib.sha1()
    h.update(b'blob %d\0' % len(data))
    h.update(data)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--work', default=os.path.join(BASE, 'payload', 'out', 'repo_audit'))
    a = ap.parse_args()

    tree = api('%s/repos/%s/git/trees/main?recursive=1' % (API, REPO))
    entries = {t['path']: t for t in tree['tree'] if t['type'] == 'blob'}
    print('files in tree:', len(entries), '| truncated:', tree.get('truncated'))

    # tarball (one request) instead of one API call per file
    os.makedirs(a.work, exist_ok=True)
    tgz = os.path.join(a.work, 'repo.tgz')
    if not os.path.exists(tgz) or os.path.getsize(tgz) < 10000:
        req = urllib.request.Request('%s/repos/%s/tarball/main' % (API, REPO),
                                     headers={'Authorization': 'Bearer ' + token(),
                                              'User-Agent': 'x'})
        with urllib.request.urlopen(req, timeout=600) as r:
            data = r.read()
        open(tgz, 'wb').write(data)
        print('tarball bytes:', len(data))
    td = os.path.join(a.work, 'tree')
    if not os.path.isdir(td):
        with tarfile.open(tgz) as tf:
            tf.extractall(a.work)
        top = [n for n in os.listdir(a.work) if os.path.isdir(os.path.join(a.work, n)) and n != 'tree']
        os.rename(os.path.join(a.work, top[0]), td)
        print('extracted to', td)

    issues = []
    warned = []

    # 1/2. sizes and hashes
    for path, ent in sorted(entries.items()):
        full = os.path.join(td, path)
        if not os.path.isfile(full):
            issues.append('MISSING in tarball: %s' % path)
            continue
        data = open(full, 'rb').read()
        if len(data) == 0:
            issues.append('EMPTY file: %s' % path)
        elif len(data) < 32 and not path.endswith('.txt'):
            warned.append('tiny file (%d B): %s' % (len(data), path))
        if git_blob_sha(data) != ent['sha']:
            issues.append('HASH MISMATCH (truncated/corrupt): %s' % path)
        if len(data) != ent.get('size', -1):
            warned.append('size differs from tree entry: %s (%d vs %d)'
                          % (path, len(data), ent.get('size', -1)))

    # 3/4. text checks
    for path in sorted(entries):
        ext = os.path.splitext(path)[1].lower()
        if ext not in TEXT_EXT:
            continue
        full = os.path.join(td, path)
        if not os.path.isfile(full):
            continue
        raw = open(full, 'rb').read()
        try:
            text = raw.decode('utf-8')
        except UnicodeDecodeError as e:
            issues.append('not UTF-8: %s (%s)' % (path, e))
            continue
        for m in MOJIBAKE:
            if m in text:
                warned.append('possible mojibake %r in %s' % (m, path))
                break
        if ext == '.md':
            # relative links
            for link in re.findall(r'\[[^\]]*\]\(([^)]+)\)', text):
                target = link.split('#')[0].strip()
                if not target or target.startswith(('http://', 'https://', 'mailto:')):
                    continue
                resolved = os.path.normpath(os.path.join(os.path.dirname(path), target))
                if not os.path.exists(os.path.join(td, resolved)):
                    issues.append('broken link in %s -> %s' % (path, target))
            if not text.startswith('#') and not text.startswith('<!--'):
                warned.append('markdown without H1: %s' % path)

    # 5. python + patches
    sys.path.insert(0, os.path.join(BASE, 'tools'))
    try:
        import check_patch
    except Exception:
        check_patch = None
    for path in sorted(entries):
        full = os.path.join(td, path)
        if not os.path.isfile(full):
            continue
        if path.endswith('.py'):
            try:
                compile(open(full, encoding='utf-8', errors='replace').read(), path, 'exec')
            except SyntaxError as e:
                issues.append('python syntax error: %s: %s' % (path, e))
        if path.endswith('.patch'):
            if check_patch is None:
                continue
            for p in check_patch.check(full):
                if p.startswith(('hunk counts', 'BOM/encoding')) or 'unbalanced: braces' in p:
                    issues.append('patch structure: %s: %s' % (path, p))

    # 6. local vs repo bytes
    for repo_path, local_path in LOCAL_MAP.items():
        full = os.path.join(td, repo_path)
        local = os.path.join(BASE, local_path)
        if not os.path.isfile(full):
            issues.append('LOCAL_MAP target missing in repo: %s' % repo_path)
            continue
        if not os.path.isfile(local):
            warned.append('local source missing: %s' % local_path)
            continue
        if open(full, 'rb').read() != io.open(local, 'rb').read():
            issues.append('repo differs from local: %s' % repo_path)

    # 7. workflow sanity
    for wf in os.listdir(os.path.join(td, '.github', 'workflows')):
        p = os.path.join(td, '.github', 'workflows', wf)
        txt = open(p, encoding='utf-8', errors='replace').read()
        if '\t' in txt:
            warned.append('tab character in workflow: %s' % wf)
        if not re.search(r'^on:', txt, re.M):
            warned.append('workflow without on: %s' % wf)

    print()
    print('=' * 60)
    print('ISSUES: %d' % len(issues))
    for i in issues:
        print('  ✗', i)
    print('WARNINGS: %d' % len(warned))
    for w in warned[:40]:
        print('  ·', w)
    if len(warned) > 40:
        print('  ... %d more' % (len(warned) - 40))
    return 1 if issues else 0


if __name__ == '__main__':
    sys.exit(main())
