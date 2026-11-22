#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Wait for a given commit's xnu build, fetch the kernel, check marks, pack images.

Usage: autopack_watch.py <sha-prefix> <kernel-dir> <tag> <payload-base> [extra-cmdline]
Multiple images can be packed by calling the pack() helper repeatedly; this
script packs exactly one image per invocation to keep it simple.
"""
import hashlib
import json
import os
import subprocess
import sys
import time
import urllib.request

ROOT = r'C:\Users\Administrator\Desktop\deepseek刷机'
BASE = os.path.join(ROOT, '红米Note10_MTK_iOS')
TOKEN = open(os.path.join(ROOT, '.gh_token')).read().strip()
REPO = 'hyperOS-li/ios-sweet-camellia-build'
OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))

SHA = sys.argv[1]
KDIR = sys.argv[2]
IMAGES = []          # (tag, payload base, extra)
for spec in sys.argv[3:]:
    tag, base, extra = spec.split(':', 2)
    IMAGES.append((tag, base, extra))


def api(path):
    req = urllib.request.Request(
        'https://api.github.com' + path,
        headers={'Authorization': 'Bearer ' + TOKEN, 'User-Agent': 'x',
                 'Accept': 'application/vnd.github+json'})
    with OPENER.open(req, timeout=90) as r:
        return json.load(r)


run = None
for _ in range(20):
    runs = api('/repos/%s/actions/runs?per_page=8' % REPO)['workflow_runs']
    for r in runs:
        if r['head_sha'].startswith(SHA):
            run = r
            break
    if run:
        break
    time.sleep(10)
print('run', run['id'] if run else None, flush=True)
if not run:
    raise SystemExit('run not found')

concl = None
for _ in range(150):
    s = api('/repos/%s/actions/runs/%d' % (REPO, run['id']))
    if s['status'] == 'completed':
        concl = s['conclusion']
        print('BUILD', concl, flush=True)
        break
    time.sleep(25)
if concl != 'success':
    raise SystemExit('build failed: %s' % concl)

outdir = os.path.join(BASE, 'payload', 'out', KDIR)
os.makedirs(outdir, exist_ok=True)
env = dict(os.environ)
env['GH_TOKEN'] = TOKEN
subprocess.run([sys.executable, '-X', 'utf8',
                os.path.join(BASE, 'tools', 'fetch_kernel.py'),
                str(run['id']), os.path.join('payload', 'out', KDIR), REPO],
               cwd=BASE, check=False, env=env)
kernel = os.path.join(outdir, 'kernel.development.camellia')
if not os.path.exists(kernel):
    raise SystemExit('NO KERNEL')
data = open(kernel, 'rb').read()
print('marks', {
    'v50': b'v50 compute copy' in data,
    'installer': b'SWEET INSTALL: probe' in data,
    'skipstatic': b'sweetfbskipstatic' in data,
}, flush=True)


def cmdline_from(payload_name):
    path = os.path.join(BASE, 'payload', 'out', 'n2', payload_name)
    raw = open(path, 'rb').read(0x48 + 608)
    return raw[0x48:0x48 + 608].split(b'\x00')[0].decode('ascii')


for tag, base, extra in IMAGES:
    cl = cmdline_from(base)
    if extra:
        cl = cl + ' ' + extra
    out = os.path.join(BASE, 'payload', 'out', 'n2', 'boot_iosroot%s.img' % tag)
    pl = os.path.join(BASE, 'payload', 'out', 'n2', 'payload-%s.bin' % tag)
    subprocess.run([sys.executable, '-X', 'utf8',
                    os.path.join(BASE, 'tools', 'mk_device_boot.py'),
                    '--root', 'ufs', '--kernel', kernel,
                    '--adt', os.path.join(BASE, 'payload', 'out', 'camellia-smp3.adt'),
                    '--allow-userdata-write', '--cmdline', cl,
                    '--payload-out', pl, '--out', out],
                   cwd=BASE, check=False,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    h = hashlib.sha256(open(out, 'rb').read()).hexdigest()[:16]
    print('IMAGE %s %s sha256=%s' % (tag, out, h), flush=True)
print('DONE', flush=True)
