#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generate sweet_touch_mtk_v46.c: the v45 kernel plus the first compute job.

The machine code embedded here was produced by .github/workflows/panblob.yml
(Mesa 26.3.1: mesa_clc -> library SPIR-V -> panfrost_compile arch 9) for:

   KERNEL(64) panlib_copy(global const uint32_t *src,
                          global uint32_t *dst, uint32_t n)
   { uint32_t i = cl_global_id.x; if (i < n) dst[i] = src[i]; }

Argument block (see panlib_copy_v9.h): u64 src, u64 dst, u32 n = 20 bytes,
placed in the FAU put right after Mesa's sysvals struct (24 bytes), therefore
the first 11 FAU words are reserved (DIV_ROUND_UP(24 + 20, 4)).
"""
import io
import os
import re

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(BASE, 'payload', 'out', 'n2', 'sweet_touch_mtk_v45.c')
DST = os.path.join(BASE, 'payload', 'out', 'n2', 'sweet_touch_mtk_v47.c')
ISA_C = os.path.join(BASE, 'evidence', 'panblob_2026-11-21', 'panlib_copy_v9.c')

text = io.open(ISA_C, encoding='utf-8').read()
m = re.search(r'static const uint32_t panlib_copy_0_default\[(\d+)\] = \{(.*?)\};',
              text, re.S)
words = re.findall(r'0x[0-9a-fA-F]+', m.group(2))
assert len(words) == int(m.group(1)) == 340, len(words)

rows = []
for i in range(0, len(words), 8):
    rows.append('    ' + ', '.join(words[i:i + 8]) + ',')
isa_array = ('static const uint32_t mali_copy_isa[%d] = {\n%s\n};\n'
             % (len(words), '\n'.join(rows)))

FUNC = '''
/* ------------------------------------------------------------------ *
 * v46: the first compute job, i.e. the GPU doing real work with machine code
 * we generated ourselves (see .github/workflows/panblob.yml).  The job layout
 * follows Mesa's genxml/v9.xml "Compute Job": Header (words 0-7) then the
 * invocation (words 8-15) and the Shader Environment at byte 64.
 * ------------------------------------------------------------------ */
''' + isa_array + '''
#define MALI_FAU_WORDS       11     /* DIV_ROUND_UP(24 sysvals + 20 args, 4) */
#define MALI_FAU_OFFSET      24     /* BIFROST_PRECOMPILED_KERNEL_ARGS_OFFSET */
#define MALI_COPY_ELEMS      1024   /* one page of uint32 */
#define MALI_COPY_WG         64     /* KERNEL(64) in the shader source */

static void
mali_compute_copy(uint32_t shader_mask)
{
	vm_offset_t srcpage = 0, dstpage = 0, isapage = 0, faupage = 0, descpage = 0, rsdpage = 0;
	uint64_t src_phys, dst_phys, shader_phys, job_phys, fau_phys, rsd_phys;
	volatile uint32_t *j, *rsd;
	volatile uint64_t *fau;
	int i, ok = 0;

	if (kmem_alloc(kernel_map, &srcpage, PAGE_SIZE,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS ||
	    kmem_alloc(kernel_map, &dstpage, PAGE_SIZE,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS ||
	    kmem_alloc(kernel_map, &isapage, PAGE_SIZE,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS ||
	    kmem_alloc(kernel_map, &faupage, PAGE_SIZE,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS ||
	    kmem_alloc(kernel_map, &descpage, PAGE_SIZE,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS ||
	    kmem_alloc(kernel_map, &rsdpage, PAGE_SIZE,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("SWEET MALI: v47 no memory for the compute test\\n");
		return;
	}

	memcpy((void *)isapage, mali_copy_isa, sizeof(mali_copy_isa));
	for (i = 0; i < MALI_COPY_ELEMS; i++) {
		*(volatile uint32_t *)(srcpage + (uint64_t)i * 4) =
		    0x5A5A0000u + (uint32_t)i;
	}

	src_phys = kvtophys(srcpage);
	dst_phys = kvtophys(dstpage);
	shader_phys = kvtophys(isapage);
	rsd_phys = kvtophys(rsdpage);
	fau_phys = kvtophys(faupage);
	job_phys = kvtophys(descpage);          /* own page: 128-byte aligned */

	/*
	 * FAU / push uniforms: sysvals first (num_workgroups[3] + printf pointer
	 * = 24 bytes), then the kernel arguments at byte 24.
	 */
	fau = (volatile uint64_t *)faupage;
	fau[0] = (uint64_t)(MALI_COPY_ELEMS / MALI_COPY_WG) | ((uint64_t)1 << 32);
	fau[1] = 0;
	fau[2] = 0;
	*(volatile uint64_t *)(faupage + MALI_FAU_OFFSET + 0) = src_phys;
	*(volatile uint64_t *)(faupage + MALI_FAU_OFFSET + 8) = dst_phys;
	*(volatile uint32_t *)(faupage + MALI_FAU_OFFSET + 16) = MALI_COPY_ELEMS;

	/*
	 * Valhall's Shader Environment points at a Shader Program descriptor, not
	 * at the ISA itself (see pan_cmdstream.c:4612-4626 and genxml/v9.xml
	 * "Shader Program"): type Shader (8), stage Compute (1), 32 registers per
	 * thread (2) in word 0, then the ISA address in words 2/3.
	 */
	rsd = (volatile uint32_t *)rsdpage;
	rsd[0] = 8u | (1u << 4) | (2u << 30);
	rsd[1] = 0;                              /* preload r48..r63 */
	rsd[2] = (uint32_t)shader_phys;
	rsd[3] = (uint32_t)(shader_phys >> 32);

	j = (volatile uint32_t *)descpage;
	j[4] = (4u << 1) | (1u << 16);          /* Type 4 = COMPUTE, index 1 */
	j[5] = 0;
	j[6] = 0;
	j[7] = 0;                               /* next = 0: end of the chain */
	j[8] = (MALI_COPY_WG - 1);              /* workgroup size x/y/z, minus(1) */
	j[9] = 1u | (2u << 14);                 /* task increment 1, axis Z */
	j[10] = MALI_COPY_ELEMS / MALI_COPY_WG;
	j[11] = 1;
	j[12] = 1;
	j[13] = 0;
	j[14] = 0;
	j[15] = 0;
	j[16] = 0;                              /* Shader Environment: attr offset */
	j[17] = MALI_FAU_WORDS;                 /* FAU count */
	j[24] = 0;
	j[25] = 0;                              /* resources */
	j[26] = (uint32_t)rsd_phys;
	j[27] = (uint32_t)(rsd_phys >> 32);     /* Shader Program (RSD) address */
	j[28] = 0;
	j[29] = 0;                              /* thread storage */
	j[30] = (uint32_t)fau_phys;
	j[31] = (uint32_t)(fau_phys >> 32);     /* FAU address */

	FlushPoC_DcacheRegion(isapage, PAGE_SIZE);
	FlushPoC_DcacheRegion(faupage, PAGE_SIZE);
	FlushPoC_DcacheRegion(descpage, PAGE_SIZE);
	FlushPoC_DcacheRegion(rsdpage, PAGE_SIZE);

	mali_pwr(mali_p_gpu, MALI_JOB_INT_MASK, 0);
	mali_pwr(mali_p_gpu, MALI_JOB_INT_CLEAR, 0xffffffffu);
	mali_pwr(mali_p_gpu, MALI_JS1 + MALI_JS_HEAD_NEXT_LO, (uint32_t)job_phys);
	mali_pwr(mali_p_gpu, MALI_JS1 + MALI_JS_HEAD_NEXT_HI, (uint32_t)(job_phys >> 32));
	mali_pwr(mali_p_gpu, MALI_JS1 + MALI_JS_AFFINITY_LO, shader_mask);
	mali_pwr(mali_p_gpu, MALI_JS1 + MALI_JS_AFFINITY_HI, 0);
	mali_pwr(mali_p_gpu, MALI_JS1 + MALI_JS_CONFIG_NEXT, MALI_JS_CONFIG);
	mali_pwr(mali_p_gpu, MALI_JS1 + MALI_JS_COMMAND_NEXT, 1u);
	(void)mali_wait(mali_p_gpu, MALI_JOB_INT_RAWSTAT, (1u << 1), (1u << 1), 500000);
	mali_pwr(mali_p_gpu, MALI_JOB_INT_CLEAR, 0xffffffffu);

	FlushPoC_DcacheRegion(dstpage, PAGE_SIZE);
	for (i = 0; i < MALI_COPY_ELEMS; i++) {
		if (*(volatile uint32_t *)(dstpage + (uint64_t)i * 4) ==
		    0x5A5A0000u + (uint32_t)i) {
			ok++;
		}
	}
	printf("SWEET MALI: v47 compute copy %d/%d words ok (raw=%08x slot=%08x)\\n",
	    ok, (int)MALI_COPY_ELEMS,
	    mali_prd(mali_p_gpu, MALI_JOB_INT_RAWSTAT),
	    mali_prd(mali_p_gpu, MALI_JS1 + MALI_JS_STATUS));

	kmem_free(kernel_map, srcpage, PAGE_SIZE);
	kmem_free(kernel_map, dstpage, PAGE_SIZE);
	kmem_free(kernel_map, isapage, PAGE_SIZE);
	kmem_free(kernel_map, faupage, PAGE_SIZE);
	kmem_free(kernel_map, descpage, PAGE_SIZE);
	kmem_free(kernel_map, rsdpage, PAGE_SIZE);
}

static void
mali_first_job(uint32_t shader_mask)'''

src = io.open(SRC, encoding='utf-8-sig').read()
anchor = 'static void\nmali_first_job(uint32_t shader_mask)'
assert anchor in src
src = src.replace(anchor, FUNC, 1)
assert '\tmali_chain_bulk(shader_mask);' in src
src = src.replace('\tmali_chain_bulk(shader_mask);',
                  '\tmali_chain_bulk(shader_mask);\n\tmali_compute_copy(shader_mask);', 1)
src = src.replace('SWEET MALI: v45 power-on start', 'SWEET MALI: v47 power-on start')
src = src.replace('mali_bg3d_gate("v45")', 'mali_bg3d_gate("v47")')
src = src.replace('SWEET MALI: v45 first job state', 'SWEET MALI: v47 first job state')
io.open(DST, 'w', encoding='utf-8-sig', newline='').write(src)
print('wrote', DST, len(src), 'bytes')
