#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""v48 = v47 with valid resources / thread-storage pointers in the compute job.

A compute job whose Shader Environment points `resources` at address 0 can fault
before the shader ever runs, so point it (and thread storage) at zero pages we
own.  Everything else is unchanged from v47.
"""
import io
import os

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(BASE, 'payload', 'out', 'n2', 'sweet_touch_mtk_v47.c')
DST = os.path.join(BASE, 'payload', 'out', 'n2', 'sweet_touch_mtk_v48.c')

T = '\t'
src = io.open(SRC, encoding='utf-8-sig').read()

alloc_old = (T + 'vm_offset_t srcpage = 0, dstpage = 0, isapage = 0, faupage = 0, '
             'descpage = 0, rsdpage = 0;')
alloc_new = (T + 'vm_offset_t srcpage = 0, dstpage = 0, isapage = 0, faupage = 0, '
             'descpage = 0, rsdpage = 0,\n' + T + '    rsrpage = 0, tlspage = 0;')
assert alloc_old in src
src = src.replace(alloc_old, alloc_new, 1)

kern_old = (T + '    kmem_alloc(kernel_map, &rsdpage, PAGE_SIZE,\n'
            + T + '    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {')
kern_new = (T + '    kmem_alloc(kernel_map, &rsdpage, PAGE_SIZE,\n'
            + T + '    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS ||\n'
            + T + '    kmem_alloc(kernel_map, &rsrpage, PAGE_SIZE,\n'
            + T + '    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS ||\n'
            + T + '    kmem_alloc(kernel_map, &tlspage, PAGE_SIZE,\n'
            + T + '    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {')
assert kern_old in src
src = src.replace(kern_old, kern_new, 1)

decl_old = T + 'uint64_t src_phys, dst_phys, shader_phys, job_phys, fau_phys, rsd_phys;'
decl_new = (T + 'uint64_t src_phys, dst_phys, shader_phys, job_phys, fau_phys, rsd_phys,\n'
            + T + '    rsr_phys, tls_phys;')
assert decl_old in src
src = src.replace(decl_old, decl_new, 1)

phys_old = T + 'rsd_phys = kvtophys(rsdpage);'
phys_new = (phys_old + '\n' + T + 'rsr_phys = kvtophys(rsrpage);\n'
            + T + 'tls_phys = kvtophys(tlspage);')
assert phys_old in src
src = src.replace(phys_old, phys_new, 1)

res_old = (T + 'j[24] = 0;\n' + T + 'j[25] = 0;                              /* resources */')
res_new = (T + 'j[24] = (uint32_t)rsr_phys;\n'
           + T + 'j[25] = (uint32_t)(rsr_phys >> 32);   /* resources table */')
assert res_old in src
src = src.replace(res_old, res_new, 1)

tls_old = (T + 'j[28] = 0;\n' + T + 'j[29] = 0;                              /* thread storage */')
tls_new = (T + 'j[28] = (uint32_t)tls_phys;\n'
           + T + 'j[29] = (uint32_t)(tls_phys >> 32);   /* thread storage */')
assert tls_old in src
src = src.replace(tls_old, tls_new, 1)

flush_old = T + 'FlushPoC_DcacheRegion(rsdpage, PAGE_SIZE);'
flush_new = (flush_old + '\n' + T + 'FlushPoC_DcacheRegion(rsrpage, PAGE_SIZE);\n'
             + T + 'FlushPoC_DcacheRegion(tlspage, PAGE_SIZE);')
assert flush_old in src
src = src.replace(flush_old, flush_new, 1)

free_old = T + 'kmem_free(kernel_map, rsdpage, PAGE_SIZE);'
free_new = (free_old + '\n' + T + 'kmem_free(kernel_map, rsrpage, PAGE_SIZE);\n'
            + T + 'kmem_free(kernel_map, tlspage, PAGE_SIZE);')
assert free_old in src
src = src.replace(free_old, free_new, 1)

src = src.replace('SWEET MALI: v47 power-on start', 'SWEET MALI: v48 power-on start')
src = src.replace('mali_bg3d_gate("v47")', 'mali_bg3d_gate("v48")')
src = src.replace('SWEET MALI: v47 first job state', 'SWEET MALI: v48 first job state')
src = src.replace('SWEET MALI: v47 compute copy', 'SWEET MALI: v48 compute copy')
src = src.replace('SWEET MALI: v47 no memory', 'SWEET MALI: v48 no memory')

io.open(DST, 'w', encoding='utf-8-sig', newline='').write(src)
print('wrote', DST, len(src))
