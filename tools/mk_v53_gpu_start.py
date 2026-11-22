#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""v53 = v52 + ask IOKit to publish the accelerator right after GPU power-on.

The 0176 block's SweetHID trigger never ran (the stub printed nothing on the
phone), so publication is driven from the driver that owns the power sequence
instead: once the GPU is out of reset, call sweet_gpu_start() and let IOKit
create and register the IOAcceleratorES service.
"""
import io
import os

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(BASE, 'payload', 'out', 'n2', 'sweet_touch_mtk_v52.c')
DST = os.path.join(BASE, 'payload', 'out', 'n2', 'sweet_touch_mtk_v53.c')

src = io.open(SRC, encoding='utf-8-sig').read()

old = '\tmali_powered = 1;\n\tprintf("SWEET MALI: power-on done\\n");'
if old not in src:
    raise SystemExit('power-on done anchor not found')
new = ('\tmali_powered = 1;\n'
       '\t{\n'
       '\t\t/* Let IOKit publish the Metal accelerator now that the registers read. */\n'
       '\t\textern void sweet_gpu_start(void);\n\n'
       '\t\tsweet_gpu_start();\n'
       '\t}\n'
       '\tprintf("SWEET MALI: power-on done\\n");')
src = src.replace(old, new, 1)

src = src.replace('SWEET MALI: v52 power-on start', 'SWEET MALI: v53 power-on start')
src = src.replace('mali_bg3d_gate("v52")', 'mali_bg3d_gate("v53")')
src = src.replace('SWEET MALI: v52 first job state', 'SWEET MALI: v53 first job state')
src = src.replace('SWEET MALI: v52 compute copy', 'SWEET MALI: v53 compute copy')
src = src.replace('SWEET MALI: v52 no memory', 'SWEET MALI: v53 no memory')

io.open(DST, 'w', encoding='utf-8-sig', newline='').write(src)
print('wrote', DST, len(src))
