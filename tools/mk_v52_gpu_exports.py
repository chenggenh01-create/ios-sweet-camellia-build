#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""v52 = v50 + the two GPU register accessors the IOKit accelerator needs.

The Metal accelerator in SweetFramebuffer.cpp (patch 0176) must not touch
MFG/SPM/PMIC sequencing; it reads the GPU registers through these two tiny
exports from the driver that already powers the GPU.

Boot behaviour is unchanged.
"""
import io
import os

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(BASE, 'payload', 'out', 'n2', 'sweet_touch_mtk_v50.c')
DST = os.path.join(BASE, 'payload', 'out', 'n2', 'sweet_touch_mtk_v52.c')

src = io.open(SRC, encoding='utf-8-sig').read()

# 1) a powered flag next to the GPU register bases
anchor = 'static vm_offset_t mali_p_spm, mali_p_infra, mali_p_infra_pdn, mali_p_mfgcfg, mali_p_gpu;'
if anchor not in src:
    raise SystemExit('gpu base anchor not found')
src = src.replace(
    anchor,
    anchor + '\n\n/* Set once the GPU is out of reset, so the IOKit side knows when\n'
    ' * reading its registers is safe. */\nstatic int mali_powered;', 1)

# 2) the exports, right before the power function (anchor on the whole
#    "static void\nsweet_mali_power(void)" so the type stays attached to it)
power_fn = 'static void\nsweet_mali_power(void)'
if power_fn not in src:
    raise SystemExit('power function not found')
src = src.replace(
    power_fn,
    '/* Used by iokit/Kernel/arm/SweetFramebuffer.cpp (Metal accelerator); they\n'
    ' * keep the IOKit side free of MFG/SPM/PMIC sequencing. */\n'
    'int\n'
    'sweet_gpu_is_powered(void)\n'
    '{\n'
    '\treturn mali_powered;\n'
    '}\n\n'
    'uint32_t\n'
    'sweet_gpu_reg_read(uint32_t off)\n'
    '{\n'
    '\tif (!mali_powered || mali_p_gpu == 0) {\n'
    '\t\treturn 0;\n'
    '\t}\n'
    '\treturn mali_prd(mali_p_gpu, off);\n'
    '}\n\nstatic void\nsweet_mali_power(void)', 1)

# 3) mark powered where power-on succeeded
done = '\tprintf("SWEET MALI: power-on done\\n");'
if done not in src:
    raise SystemExit('power-on done print not found')
src = src.replace(done, '\tmali_powered = 1;\n' + done, 1)

src = src.replace('SWEET MALI: v50 power-on start', 'SWEET MALI: v52 power-on start')
src = src.replace('mali_bg3d_gate("v50")', 'mali_bg3d_gate("v52")')
src = src.replace('SWEET MALI: v50 first job state', 'SWEET MALI: v52 first job state')
src = src.replace('SWEET MALI: v50 compute copy', 'SWEET MALI: v52 compute copy')
src = src.replace('SWEET MALI: v50 no memory', 'SWEET MALI: v52 no memory')

io.open(DST, 'w', encoding='utf-8-sig', newline='').write(src)
print('wrote', DST, len(src))
