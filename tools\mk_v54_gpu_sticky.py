#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""v54 = v53 + a sticky line about the Metal accelerator service.

Every boot line scrolls away in under a second, so a photo almost never catches
it.  Reprint the service state with the other 10-second status lines instead:
any photo taken while the phone runs shows whether the accelerator is up.
"""
import io
import os

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(BASE, 'payload', 'out', 'n2', 'sweet_touch_mtk_v53.c')
DST = os.path.join(BASE, 'payload', 'out', 'n2', 'sweet_touch_mtk_v54.c')

src = io.open(SRC, encoding='utf-8-sig').read()

anchor = '\tprintf("SWEET MALI: v53 compute copy %u/%u words ok\\n",'
if anchor not in src:
    raise SystemExit('sticky compute print not found')
# After the two-line printf that follows the anchor, add the GPU service line.
tail_marker = 'mali_compute_ok, mali_compute_words);'
idx = src.find(anchor)
end = src.find(tail_marker, idx)
if end < 0:
    raise SystemExit('sticky compute tail not found')
end += len(tail_marker)
insert = ('\n\t{\n'
          '\t\textern int sweet_gpu_service_state(void);\n\n'
          '\t\tprintf("SWEET GPU: service state=%d gpu_id=%08x\\n",\n'
          '\t\t    sweet_gpu_service_state(), sweet_gpu_reg_read(0));\n'
          '\t}')
src = src[:end] + insert + src[end:]

src = src.replace('SWEET MALI: v53 power-on start', 'SWEET MALI: v54 power-on start')
src = src.replace('mali_bg3d_gate("v53")', 'mali_bg3d_gate("v54")')
src = src.replace('SWEET MALI: v53 first job state', 'SWEET MALI: v54 first job state')
src = src.replace('SWEET MALI: v53 compute copy', 'SWEET MALI: v54 compute copy')
src = src.replace('SWEET MALI: v53 no memory', 'SWEET MALI: v54 no memory')

io.open(DST, 'w', encoding='utf-8-sig', newline='').write(src)
print('wrote', DST, len(src))
