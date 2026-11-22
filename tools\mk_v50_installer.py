#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""v50 = v49 + a kernel-side installer that proves we can write into the iOS root.

XNU does not export vn_open(); open1() uses vn_open_auth(), whose symbol is in
the kernel image.  vn_open_auth() runs namei() itself: for O_CREAT it switches
the op to CREATE and adds LOCKPARENT | LOCKLEAF | AUDITVNPATH1, so the call site
only needs the same NDINIT the syscalls use:

    NDINIT(&nd, LOOKUP, OP_OPEN, FOLLOW, UIO_SYSSPACE,
	    (user_addr_t)(uintptr_t)path, ctx);
    flags = FFLAGS(O_WRONLY | O_CREAT | O_TRUNC);
    vn_open_auth(&nd, &flags, &va, NULLVP);

Boot arg: sweetinstall=1
"""
import io
import os

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(BASE, 'payload', 'out', 'n2', 'sweet_touch_mtk_v49.c')
DST = os.path.join(BASE, 'payload', 'out', 'n2', 'sweet_touch_mtk_v50.c')
T = '\t'

src = io.open(SRC, encoding='utf-8-sig').read()

anchor = '#include <vm/vm_kern.h>'
if anchor not in src:
    raise SystemExit('vm_kern.h include not found')

extra_includes = '''#include <sys/namei.h>
#include <sys/vnode.h>
#include <sys/fcntl.h>
#include <sys/uio.h>
'''

installer = '''
/*
 * v50: write a file into the mounted iOS root from the kernel.
 *
 * The BSD layer does not export vn_open(); open1() uses vn_open_auth(), which is
 * linked into the kernel, so it is declared here with the same signature.
 * This is the prerequisite for installing our own userland (Vulkan driver and
 * Metal front-end) into the root later.
 */
extern int vn_open_auth(struct nameidata *ndp, int *fmodep,
    struct vnode_attr *vap, vnode_t authvp);
extern int vn_close(vnode_t vp, int flags, vfs_context_t ctx);

static int
sweet_write_root_file(const char *path, const void *data, size_t len, int mode)
{
	struct nameidata nd;
	struct vnode_attr va;
	vfs_context_t ctx = vfs_context_current();
	int uflags = O_WRONLY | O_CREAT | O_TRUNC;
	int flags = FFLAGS(uflags);
	int resid = 0, error;

	bzero(&nd, sizeof(nd));
	VATTR_INIT(&va);
	VATTR_SET(&va, va_mode, mode);

	NDINIT(&nd, LOOKUP, OP_OPEN, FOLLOW, UIO_SYSSPACE,
	    (user_addr_t)(uintptr_t)path, ctx);
	error = vn_open_auth(&nd, &flags, &va, NULLVP);
	if (error != 0) {
		printf("SWEET INSTALL: open(%s) failed %d\\n", path, error);
		return error;
	}
	error = vn_rdwr(UIO_WRITE, nd.ni_vp, (caddr_t)(uintptr_t)data, (int)len, 0,
	    UIO_SYSSPACE, 0, vfs_context_ucred(ctx), &resid,
	    vfs_context_proc(ctx));
	(void)vn_close(nd.ni_vp, flags, ctx);
	if (error != 0) {
		printf("SWEET INSTALL: write(%s) failed %d\\n", path, error);
	}
	return error;
}

static void
sweet_install_probe(void)
{
	static const char msg[] =
	    "sweet userland install probe\\n"
	    "written by the kernel at boot; safe to delete\\n";
	int enable = 0;
	int error;

	PE_parse_boot_argn("sweetinstall", &enable, sizeof(enable));
	if (enable != 1) {
		return;
	}
	error = sweet_write_root_file("/private/var/mobile/sweet_install_probe.txt",
	    msg, sizeof(msg) - 1, 0644);
	printf("SWEET INSTALL: probe %s (err=%d)\\n",
	    error == 0 ? "written" : "FAILED", error);
}
'''
src = src.replace(anchor, extra_includes + '\n' + installer + '\n' + anchor, 1)

done = T + 'printf("SWEET MALI: power-on done\\n");'
if done not in src:
    raise SystemExit('power-on done print not found')
src = src.replace(done, done + '\n' + T + 'sweet_install_probe();', 1)

# The compute test changes GPU state (a UI boot with it enabled did not reach
# the desktop), so it only runs when explicitly asked for.
call = T + 'mali_compute_copy(shader_mask);'
if call not in src:
    raise SystemExit('compute call not found')
gated = (T + '/* Opt-in: changes GPU state, so only run it when asked. */\n'
         + T + '{\n'
         + T + T + 'int compute_test = 0;\n\n'
         + T + T + 'PE_parse_boot_argn("sweetcomputetest", &compute_test,\n'
         + T + T + '    sizeof(compute_test));\n'
         + T + T + 'if (compute_test == 1) {\n'
         + T + T + T + 'mali_compute_copy(shader_mask);\n'
         + T + T + '}\n'
         + T + '}')
src = src.replace(call, gated, 1)

src = src.replace('SWEET MALI: v49 power-on start', 'SWEET MALI: v50 power-on start')
src = src.replace('mali_bg3d_gate("v49")', 'mali_bg3d_gate("v50")')
src = src.replace('SWEET MALI: v49 first job state', 'SWEET MALI: v50 first job state')
src = src.replace('SWEET MALI: v49 compute copy', 'SWEET MALI: v50 compute copy')
src = src.replace('SWEET MALI: v49 no memory', 'SWEET MALI: v50 no memory')

io.open(DST, 'w', encoding='utf-8-sig', newline='').write(src)
print('wrote', DST, len(src))
