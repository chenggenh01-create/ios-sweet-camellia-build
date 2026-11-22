#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Wait for the queued XNU builds, fetch each kernel, pack device images.

Unattended helper: run it and come back to a list of ready images + hashes.
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


def api(path):
    req = urllib.request.Request(
        'https://api.github.com' + path,
        headers={'Authorization': 'Bearer ' + TOKEN, 'User-Agent': 'x',
                 'Accept': 'application/vnd.github+json'})
    with OPENER.open(req, timeout=90) as r:
        return json.load(r)


def find_run(sha_prefix, workflow='xnu.yml', tries=20):
    for _ in range(tries):
        runs = api('/repos/%s/actions/runs?per_page=6&workflow=%s'
                   % (REPO, workflow))['workflow_runs']
        for r in runs:
            if r['head_sha'].startswith(sha_prefix):
                return r
        time.sleep(10)
    return None


def wait_run(run, limit=120):
    for _ in range(limit):
        s = api('/repos/%s/actions/runs/%d' % (REPO, run['id']))
        if s['status'] == 'completed':
            return s['conclusion']
        time.sleep(25)
    return 'timeout'


def cmdline_from(payload_name):
    path = os.path.join(BASE, 'payload', 'out', 'n2', payload_name)
    data = open(path, 'rb').read(0x48 + 608)
    return data[0x48:0x48 + 608].split(b'\x00')[0].decode('ascii')


def pack(tag, kernel, payload_base, extra):
    cl = cmdline_from(payload_base)
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
    print('IMAGE %-4s %s  sha256=%s' % (tag, out, h), flush=True)
    return out


KERNEL = 'kernel.development.camellia'

# (sha prefix, kernel dir, images: [(tag, payload base, extra cmdline)])
JOBS = [
    ('450884de', 'xnu281', [
        ('281', 'payload-250.bin', ''),
        ('282', 'payload-245.bin', 'sweetpresentwindow=2400 sweetpresentmap=1'),
    ]),
    ('83a3238c', 'xnu283', [
        ('283', 'payload-245.bin',
         'sweetpresentwindow=2400 sweetpresentmap=1 sweetfbskipstatic=1'),
        ('284', 'payload-250.bin', 'sweetfbskipstatic=1'),
    ]),
    ('23ea814a', 'xnu285', [
        ('285', 'payload-250.bin', ''),
        ('286', 'payload-245.bin', 'sweetpresentwindow=2400 sweetpresentmap=1'),
        ('287', 'payload-245.bin',
         'sweetpresentwindow=2400 sweetpresentmap=1 sweetfbskipstatic=1'),
    ]),
]

for sha, kdir, images in JOBS:
    run = find_run(sha)
    if run is None:
        print('RUN %s not found' % sha, flush=True)
        continue
    concl = wait_run(run)
    print('BUILD %s -> %s (run %d)' % (sha, concl, run['id']), flush=True)
    if concl != 'success':
        continue
    kdir_path = os.path.join(BASE, 'payload', 'out', kdir)
    subprocess.run([sys.executable, '-X', 'utf8',
                    os.path.join(BASE, 'tools', 'fetch_kernel.py'),
                    str(run['id']), os.path.join('payload', 'out', kdir), REPO],
                   cwd=BASE, check=False,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    kernel = os.path.join(kdir_path, KERNEL)
    if not os.path.exists(kernel):
        print('NO KERNEL for %s' % sha, flush=True)
        continue
    data = open(kernel, 'rb').read()
    marks = {
        'v46_compute': b'v46 compute copy' in data,
        'v47_compute': b'v47 compute copy' in data,
        'skipstatic': b'sweetfbskipstatic' in data,
    }
    print('KERNEL %s marks %s' % (kdir, marks), flush=True)
    for tag, base, extra in images:
        pack(tag, kernel, base, extra)

print('ALL DONE', flush=True)
