#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Wait for the v48 XNU build (commit 65dc95e1), fetch the kernel, pack images."""
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
SHA = '65dc95e1'
OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))


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

for _ in range(150):
    s = api('/repos/%s/actions/runs/%d' % (REPO, run['id']))
    if s['status'] == 'completed':
        print('BUILD', s['conclusion'], flush=True)
        break
    time.sleep(25)
if s['conclusion'] != 'success':
    raise SystemExit('build failed')

outdir = os.path.join(BASE, 'payload', 'out', 'xnu288')
os.makedirs(outdir, exist_ok=True)
subprocess.run([sys.executable, '-X', 'utf8',
                os.path.join(BASE, 'tools', 'fetch_kernel.py'),
                str(run['id']), os.path.join('payload', 'out', 'xnu288'), REPO],
               cwd=BASE, check=False)
kernel = os.path.join(outdir, 'kernel.development.camellia')
if not os.path.exists(kernel):
    raise SystemExit('NO KERNEL')
data = open(kernel, 'rb').read()
print('marks', {
    'v48': b'v48 compute copy' in data,
    'skipstatic': b'sweetfbskipstatic' in data,
}, flush=True)


def cmdline_from(payload_name):
    path = os.path.join(BASE, 'payload', 'out', 'n2', payload_name)
    raw = open(path, 'rb').read(0x48 + 608)
    return raw[0x48:0x48 + 608].split(b'\x00')[0].decode('ascii')


ui = cmdline_from('payload-245.bin').replace('sweetpresentwindow=960',
                                             'sweetpresentwindow=2400')
ui = ui + ' sweetpresentmap=1'
console = cmdline_from('payload-250.bin')

for tag, cl in (('288', console), ('289', ui), ('290', ui + ' sweetfbskipstatic=1')):
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
