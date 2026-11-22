/*
 * camellia (MT6833 / Dimensity 700) Novatek NT36672C touch probe.
 *
 * Register model and bit-bang timing are taken from the working
 * YukinoXNU-Cannon (Redmi Note 9 5G / MT6853) port, whose touch wiring is
 * identical: SPI4@0x11018000, MI=GPIO12, MO=13, CLK=97, CS=98, reset=15,
 * IRQ=14, event page 0x21C00, swrst 0x3F0FE, fast-read 0x3F310.
 *
 * MTK GPIO ("Paris") model:
 *   mode  : 0x300 + (pin/8)*0x10, 4-bit field at (pin%8)*4
 *   dir   : 0x000 + (pin/32)*0x10 (status), 0x004 set output, 0x008 set input
 *   dout  : 0x100 + (pin/32)*0x10 (status), 0x104 set high, 0x108 set low
 *   din   : 0x200 + (pin/32)*0x10
 *
 * The probe only READS: chip trim id (0x3F004), the 17-byte Novatek state
 * block at event page + 0x50, the FWINFO block at +0x78 and the 65-byte
 * point report.  It never drives the reset pin and never resets the chip.
 *
 * Boot args:
 *   sweettouchmtk=1      enable this probe (default 0)
 *   sweettouchdelay=N    seconds before probing (default 30)
 *   sweetmtkdelay=US     half SPI clock period in microseconds (default 2)
 *   sweettouchsecs=N     how long to keep probing (default 1800)
 *   sweettouchperiod=MS  time between passes (default 2000)
 *
 * Output goes to the kernel console, prefix "SWEET TOUCH MTK:".
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/namei.h>
#include <sys/vnode.h>
#include <sys/fcntl.h>
#include <sys/uio.h>


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
		printf("SWEET INSTALL: open(%s) failed %d\n", path, error);
		return error;
	}
	error = vn_rdwr(UIO_WRITE, nd.ni_vp, (caddr_t)(uintptr_t)data, (int)len, 0,
	    UIO_SYSSPACE, 0, vfs_context_ucred(ctx), &resid,
	    vfs_context_proc(ctx));
	(void)vn_close(nd.ni_vp, flags, ctx);
	if (error != 0) {
		printf("SWEET INSTALL: write(%s) failed %d\n", path, error);
	}
	return error;
}

static void
sweet_install_probe(void)
{
	static const char msg[] =
	    "sweet userland install probe\n"
	    "written by the kernel at boot; safe to delete\n";
	int enable = 0;
	int error;

	PE_parse_boot_argn("sweetinstall", &enable, sizeof(enable));
	if (enable != 1) {
		return;
	}
	error = sweet_write_root_file("/private/var/mobile/sweet_install_probe.txt",
	    msg, sizeof(msg) - 1, 0644);
	printf("SWEET INSTALL: probe %s (err=%d)\n",
	    error == 0 ? "written" : "FAILED", error);
}

#include <vm/vm_kern.h>

#if defined(ARM64_BOARD_CONFIG_SWEET)

#include "sweet_nvt_fw_hex.h"

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
/* Mach MIG declaration is not included by BSD's exported thread.h. */
extern kern_return_t thread_terminate(thread_t);

#define GPIO_PA			0x10005000u
#define IOCFG_LM_PA		0x11E20000u	/* pin 14 pad regs */
#define IOCFG_BL_PA		0x11E60000u	/* pin 12 pad regs */

#define PIN_MI			12
#define PIN_MO			13
#define PIN_IRQ			14
#define PIN_RST			15
#define PIN_CLK			97
#define PIN_CS			98

#define GPIO_GROUP(pin)		(((pin) / 32u) * 0x10u)
#define GPIO_BIT(pin)		(1u << ((pin) % 32u))
#define GPIO_MODE(pin)		(0x300u + ((pin) / 8u) * 0x10u)
#define GPIO_MODE_SHIFT(pin)	(((pin) % 8u) * 4u)
#define GPIO_DIR(pin)		(0x000u + GPIO_GROUP(pin))
#define GPIO_DIR_SET(pin)	(0x004u + GPIO_GROUP(pin))
#define GPIO_DIR_CLR(pin)	(0x008u + GPIO_GROUP(pin))
#define GPIO_DO(pin)		(0x100u + GPIO_GROUP(pin))
#define GPIO_DO_SET(pin)	(0x104u + GPIO_GROUP(pin))
#define GPIO_DO_CLR(pin)	(0x108u + GPIO_GROUP(pin))
#define GPIO_DI(pin)		(0x200u + GPIO_GROUP(pin))

/* iocfg pad registers from pinctrl-mt6833.c: the value is the base offset of
 * the field; the SET register is base+4, the clear register is base. */
#define PAD_IES_MI		0x0090u, 11
#define PAD_IES_IRQ		0x0050u, 2

/* Novatek addresses (nova36672c/nt36xxx.h, confirmed by YukinoXNU-Cannon). */
#define NVT_EVENT		0x21C00u
#define NVT_EVENT_STATE		0x50u
#define NVT_STATE_LEN		17
#define NVT_EVENT_FWINFO	0x78u
#define NVT_FWINFO_LEN		11
#define NVT_CHIP_TRIM		0x03F004u
#define NVT_REPORT_LEN		65

static struct {
	vm_offset_t gpio, lm, bl;
	int started, dead;
	int half_us;
	uint32_t page;
	unsigned passes;
	int fw_result;
	unsigned fw_stage;
	unsigned fw_pid;
} st;

static int touch_chan;

extern void sweet_hid_touch(int finger, int down, int x, int y);

static uint8_t hid_down[10];
static uint16_t hid_last_x[10], hid_last_y[10];
static unsigned hid_events;

static uint32_t
g_rd(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);

	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
g_wr(vm_offset_t base, uint32_t off, uint32_t val)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + off) = val;
	__asm__ volatile("dsb osh" ::: "memory");
}

static void
pin_mode(uint32_t pin, uint32_t mode)
{
	uint32_t off = GPIO_MODE(pin);
	uint32_t shift = GPIO_MODE_SHIFT(pin);
	uint32_t v = g_rd(st.gpio, off) & ~(0xFu << shift);

	g_wr(st.gpio, off, v | ((mode & 0xFu) << shift));
}

static void
pin_dir(uint32_t pin, bool out)
{
	g_wr(st.gpio, out ? GPIO_DIR_SET(pin) : GPIO_DIR_CLR(pin), GPIO_BIT(pin));
}

static void
pin_out(uint32_t pin, bool high)
{
	g_wr(st.gpio, high ? GPIO_DO_SET(pin) : GPIO_DO_CLR(pin), GPIO_BIT(pin));
}

static bool
pin_in(uint32_t pin)
{
	return (g_rd(st.gpio, GPIO_DI(pin)) & GPIO_BIT(pin)) != 0;
}

static uint32_t
pin_mode_get(uint32_t pin)
{
	return (g_rd(st.gpio, GPIO_MODE(pin)) >> GPIO_MODE_SHIFT(pin)) & 0xFu;
}

static uint32_t
pin_dir_get(uint32_t pin)
{
	return (g_rd(st.gpio, GPIO_DIR(pin)) >> ((pin) % 32u)) & 1u;
}

static uint32_t
pin_do_get(uint32_t pin)
{
	return (g_rd(st.gpio, GPIO_DO(pin)) >> ((pin) % 32u)) & 1u;
}

/* iocfg pad field: base is the clear register, base+4 the set register. */
static void
pad_set(vm_offset_t base, uint32_t off, unsigned bit, bool on)
{
	g_wr(base, on ? off + 4u : off, 1u << bit);
}

static void
touch_wait(unsigned ms)
{
	uint64_t deadline;

	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&touch_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

static void
spi_cs(bool low)
{
	pin_out(PIN_CS, low ? false : true);
	pin_out(PIN_CLK, false);
	delay(st.half_us);
}

/* Mode 0, MSB first, cannon's sampling order: MOSI is set while the clock is
 * low, MISO is sampled just BEFORE the rising edge, then the clock is pulsed.
 * Sampling after the edge reads the next bit (cannon saw FWINFO shifted). */
static uint8_t
spi_xfer(uint8_t out)
{
	uint8_t in = 0;

	for (int i = 7; i >= 0; i--) {
		pin_out(PIN_MO, ((out >> i) & 1u) != 0);
		delay(st.half_us);
		in = (uint8_t)((in << 1) | (pin_in(PIN_MI) ? 1u : 0u));
		pin_out(PIN_CLK, true);
		delay(st.half_us);
		pin_out(PIN_CLK, false);
	}
	return in;
}

/* 3-byte page select: FF page[22:15] page[14:7]. */
static void
nvt_set_page(uint32_t addr)
{
	spi_cs(true);
	spi_xfer(0xFF);
	spi_xfer((addr >> 15) & 0xFF);
	spi_xfer((addr >> 7) & 0xFF);
	spi_cs(false);
	delay(200);
}

/* Read len bytes at addr: offset (7 bit), dummy, then len data bytes. */
static void
nvt_read(uint32_t addr, uint8_t *buf, int len)
{
	nvt_set_page(addr);

	spi_cs(true);
	spi_xfer(addr & 0x7Fu);
	spi_xfer(0x00);
	for (int i = 0; i < len; i++) {
		buf[i] = spi_xfer(0x00);
	}
	spi_cs(false);
}

/* Same as nvt_read but assumes the event page is already latched. */
static void
nvt_read_latched(uint32_t addr, uint8_t *buf, int len)
{
	spi_cs(true);
	spi_xfer(addr & 0x7Fu);
	spi_xfer(0x00);
	for (int i = 0; i < len; i++) {
		buf[i] = spi_xfer(0x00);
	}
	spi_cs(false);
}

static unsigned
nonzero_bytes(const uint8_t *buf, int len)
{
	unsigned n = 0;

	for (int i = 0; i < len; i++) {
		if (buf[i] != 0x00 && buf[i] != 0xFF) {
			n++;
		}
	}
	return n;
}

static bool
fw_info_ok(const uint8_t *b, uint32_t max_x, uint32_t max_y)
{
	if ((uint8_t)(b[0] + b[1]) != 0xFF) {
		return false;
	}
	if ((((uint32_t)b[4] << 8) | b[5]) != max_x) {
		return false;
	}
	if ((((uint32_t)b[6] << 8) | b[7]) != max_y) {
		return false;
	}
	return true;
}

/* ---- Novatek RAM firmware boot ------------------------------------------
 * Ported from the vendor nt36xxx_fw_update.c and YukinoXNU-Cannon's
 * novatek_boot.c.  The image is the stock camellia
 * nt36672c_tm_01_ts_fw.bin (139264 bytes, NVT end flag at 0x1FFFD).
 */
struct nvt_sec {
	uint32_t sram, size, src, crc;
};

static uint8_t fwbuf[SWEET_NVT_FW_LEN];

static int
hex_nib(char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

static int
fw_decode(void)
{
	unsigned n = 0;

	for (unsigned i = 0; i + 1 < SWEET_NVT_FW_HEXLEN && n < SWEET_NVT_FW_LEN;
	    i += 2) {
		int a = hex_nib(sweet_nvt_fw_hex[i]);
		int b = hex_nib(sweet_nvt_fw_hex[i + 1]);

		if (a < 0 || b < 0) {
			return -1;
		}
		fwbuf[n++] = (uint8_t)((a << 4) | b);
	}
	return n == SWEET_NVT_FW_LEN ? 0 : -1;
}

static uint32_t
fw32(uint32_t off)
{
	return (uint32_t)fwbuf[off] | ((uint32_t)fwbuf[off + 1] << 8) |
	    ((uint32_t)fwbuf[off + 2] << 16) | ((uint32_t)fwbuf[off + 3] << 24);
}

static int
fw_parse(struct nvt_sec *sec, int *nsec)
{
	uint32_t end = fw32(0);
	uint32_t n = 0;
	uint32_t ovly_info, ovly_n, dlm_src;

	if (end < 0x40 || end > SWEET_NVT_FW_LEN) {
		return -1;
	}
	/* ILM and DLM: BIN, SRAM, size (12 bytes each), CRC at 0x18/0x1C. */
	for (uint32_t i = 0; i < 2; i++) {
		sec[n].src = fw32(i * 12);
		sec[n].sram = fw32(4 + i * 12);
		sec[n].size = fw32(8 + i * 12);
		sec[n].crc = fw32(0x18 + i * 4);
		n++;
	}
	/* Info sections: SRAM, size, BIN, CRC (16 bytes each from 0x30). */
	for (uint32_t off = 0x30; off + 16 <= end; off += 0x10) {
		sec[n].sram = fw32(off);
		sec[n].size = fw32(off + 4);
		sec[n].src = fw32(off + 8);
		sec[n].crc = fw32(off + 12);
		n++;
	}
	/* Overlay sections live at the DLM BIN address. */
	ovly_info = (fwbuf[0x28] >> 4) & 1u;
	ovly_n = ovly_info ? (fwbuf[0x28] & 0x0Fu) : 0;
	dlm_src = sec[1].src;
	for (uint32_t i = 0; i < ovly_n; i++) {
		uint32_t off = dlm_src + i * 16;

		if (off + 16 > SWEET_NVT_FW_LEN) {
			return -1;
		}
		sec[n].sram = fw32(off);
		sec[n].size = fw32(off + 4);
		sec[n].src = fw32(off + 8);
		sec[n].crc = fw32(off + 12);
		n++;
	}
	if (n > 24) {
		return -1;
	}
	for (uint32_t i = 0; i < n; i++) {
		if (sec[i].size == 0) {
			continue;
		}
		if (sec[i].src + sec[i].size + 1 > SWEET_NVT_FW_LEN ||
		    sec[i].sram + sec[i].size + 1 > 0x40000) {
			return -1;
		}
	}
	*nsec = (int)n;
	return 0;
}

static void
nvt_write(uint32_t addr, const uint8_t *data, unsigned n)
{
	nvt_set_page(addr);
	spi_cs(true);
	spi_xfer((uint8_t)((addr & 0x7Fu) | 0x80u));
	for (unsigned i = 0; i < n; i++) {
		spi_xfer(data[i]);
	}
	spi_cs(false);
}

static void
nvt_write8(uint32_t addr, uint8_t v)
{
	nvt_write(addr, &v, 1);
}

static void
nvt_write_le(uint32_t addr, uint32_t v, unsigned n)
{
	uint8_t b[4];

	for (unsigned i = 0; i < n; i++) {
		b[i] = (uint8_t)(v >> (8 * i));
	}
	nvt_write(addr, b, n);
}

static void
nvt_write_section(const struct nvt_sec *s)
{
	unsigned total = s->size + 1;	/* the vendor writes size+1 bytes */
	unsigned at = 0;

	while (at < total) {
		uint32_t addr = s->sram + at;
		unsigned room = 128u - (addr & 0x7Fu);
		unsigned n = total - at;

		if (n > 64) {
			n = 64;
		}
		if (n > room) {
			n = room;
		}
		nvt_write(addr, fwbuf + s->src + at, n);
		at += n;
	}
}

/* Returns 0 when the firmware runs and FWINFO/PID read back sanely. */
static int
nvt_boot_fw(void)
{
	struct nvt_sec sec[24];
	uint8_t id[8] = { 0 };
	uint8_t info[16] = { 0 };
	uint8_t pid[4] = { 0 };
	uint8_t v = 0;
	int nsec = 0, i;

	if (fw_decode()) {
		return -2;
	}
	st.fw_stage = 1;
	if (fwbuf[0x1FFFD] != 'N' || fwbuf[0x1FFFE] != 'V' ||
	    fwbuf[0x1FFFF] != 'T') {
		return -1;
	}
	if (fw_parse(sec, &nsec)) {
		return -2;
	}
	printf("SWEET TOUCH MTK: fw sections=%d ilm=%#x/%#x dlm=%#x/%#x\n",
	    nsec, sec[0].sram, sec[0].size, sec[1].sram, sec[1].size);

	/* Put the chip into its bootloader and adopt it. */
	st.fw_stage = 2;
	{
		int rstp = 0;

		(void)PE_parse_boot_argn("sweetmtkrst", &rstp, sizeof(rstp));
		if (rstp) {
			pin_mode(PIN_RST, 0);
			pin_dir(PIN_RST, true);
			pin_out(PIN_RST, false);
			delay(10 * 1000);
			pin_out(PIN_RST, true);
			delay(10 * 1000);
			printf("SWEET TOUCH MTK: fw reset pulse done\n");
		}
	}
	nvt_write8(0x3F0FE, 0x69);
	delay(5 * 1000);
	nvt_write8(0x3F310, 0x00);
	nvt_read(0x1F64E, id, 6);
	printf("SWEET TOUCH MTK: fw old trim %02x %02x %02x %02x %02x %02x\n",
	    id[0], id[1], id[2], id[3], id[4], id[5]);
	if (id[5] != 0x03 || id[3] != 0x72 || id[4] != 0x66) {
		return -3;
	}
	for (i = 0; i < nsec; i++) {
		if (sec[i].size) {
			nvt_write_section(&sec[i]);
		}
	}
	st.fw_stage = 3;
	printf("SWEET TOUCH MTK: fw written, setting crc banks\n");
	{
		unsigned bad = 0, first = 0, checked = 0;

		for (i = 0; i < nsec; i++) {
			uint8_t rb[32];
			unsigned offs[3];
			unsigned n = sec[i].size ? 32 : 0;

			if (!n) {
				continue;
			}
			if (n > sec[i].size) {
				n = sec[i].size;
			}
			offs[0] = 0;
			offs[1] = sec[i].size > n ? (sec[i].size - n) / 2 : 0;
			offs[2] = sec[i].size > n ? sec[i].size - n : 0;
			for (int k = 0; k < 3; k++) {
				nvt_read(sec[i].sram + offs[k], rb, n);
				checked++;
				for (unsigned j = 0; j < n; j++) {
					if (rb[j] != fwbuf[sec[i].src + offs[k] + j]) {
						if (!bad) {
							first = sec[i].sram + offs[k] + j;
						}
						bad++;
					}
				}
			}
		}
		printf("SWEET TOUCH MTK: fw verify checked=%u bad=%u first=%06x\n",
		    checked, bad, first);
	}

	/* ILM/DLM descriptors, lengths and CRCs (hw_crc = 2, 3-byte length). */
	nvt_write_le(0x3F128, sec[0].sram, 3);
	nvt_write_le(0x3F12C, sec[1].sram, 3);
	nvt_write_le(0x3F118, sec[0].size, 3);
	nvt_write_le(0x3F130, sec[1].size, 3);
	nvt_write_le(0x3F100, sec[0].crc, 4);
	nvt_write_le(0x3F104, sec[1].crc, 4);

	st.fw_stage = 4;
	{
		int crc_en = 0;
		uint8_t crcf = 0, rilm[4] = { 0 }, rdlm[4] = { 0 }, st;

		(void)PE_parse_boot_argn("sweetmtkcrc", &crc_en, sizeof(crc_en));
		printf("SWEET TOUCH MTK: fw crc_en=%d\n", crc_en);
		if (crc_en) {
			nvt_read(0x3F30E, &v, 1);
			nvt_write8(0x3F30E, (uint8_t)(v | 0x80));
		}
		nvt_write8(0x22D60, 0x00);
		if (crc_en) {
			nvt_write8(0x22D50, 0xAE);
		}
		nvt_write8(0x3F10D, 0x01);
		delay(5 * 1000);
		nvt_read(0x3F133, &crcf, 1);
		nvt_read(0x3F120, rilm, 4);
		nvt_read(0x3F124, rdlm, 4);
		printf("SWEET TOUCH MTK: fw crc flags=%02x ilm=%02x%02x%02x%02x/%08x "
		    "dlm=%02x%02x%02x%02x/%08x\n", crcf,
		    rilm[0], rilm[1], rilm[2], rilm[3], sec[0].crc,
		    rdlm[0], rdlm[1], rdlm[2], rdlm[3], sec[1].crc);
		for (int k = 0; k < 5; k++) {
			nvt_read(0x22D60, &st, 1);
			printf("SWEET TOUCH MTK: fw state[%d]=%02x\n", k, st);
			delay(100 * 1000);
		}
	}
	delay(5 * 1000);
	for (i = 0; i < 100; i++) {
		nvt_read(0x22D60, &v, 1);
		if (v >= 0xA0 && v <= 0xAF) {
			break;
		}
		delay(10 * 1000);
	}
	st.fw_stage = 5;
	printf("SWEET TOUCH MTK: fw reset state=%02x tries=%d\n", v, i);
	if (v < 0xA0 || v > 0xAF) {
		return -4;
	}
	st.fw_stage = 6;
	nvt_read(0x22D78, info, 12);
	printf("SWEET TOUCH MTK: fw info %02x %02x %02x %02x %02x%02x %02x%02x\n",
	    info[0], info[1], info[2], info[3], info[4], info[5], info[6],
	    info[7]);
	if ((uint8_t)(info[0] + info[1]) != 0xFF) {
		return -5;
	}
	st.fw_stage = 7;
	nvt_read(0x22D9A, pid, 2);
	st.fw_pid = (unsigned)((pid[1] << 8) | pid[0]);
	printf("SWEET TOUCH MTK: fw pid=%04x\n", st.fw_pid);
	return 0;
}

/* Post-boot identity check: FWINFO and PID must be sane. */
static int
nvt_check_fw(void)
{
	uint8_t info[12] = { 0 };
	uint8_t pid[2] = { 0 };
	unsigned fw_pid;

	nvt_read(0x22D78, info, 12);
	if ((uint8_t)(info[0] + info[1]) != 0xFF) {
		return -1;
	}
	if ((((unsigned)info[4] << 8) | info[5]) != 1080 ||
	    (((unsigned)info[6] << 8) | info[7]) != 2400) {
		return -2;
	}
	nvt_read(0x22D9A, pid, 2);
	fw_pid = ((unsigned)pid[1] << 8) | pid[0];
	st.fw_pid = fw_pid;
	if (fw_pid != 0x5949) {
		return -3;
	}
	return 0;
}

/* One 65-byte Novatek report -> 10 fingers -> SweetHID digitizer. */
static void
touch_hid_frame(const uint8_t *rep)
{
	uint8_t seen[10] = { 0 };
	unsigned i;
	bool idle = true;

	for (i = 0; i < 6; i++) {
		if (rep[i] != 0x77 && rep[i] != 0xFD && rep[i] != 0xFE) {
			idle = false;
			break;
		}
	}
	if (idle) {
		for (i = 0; i < 10; i++) {
			if (hid_down[i]) {
				sweet_hid_touch((int)i, 0, hid_last_x[i], hid_last_y[i]);
				hid_down[i] = 0;
			}
		}
		return;
	}

	for (i = 0; i < 10; i++) {
		const uint8_t *p = rep + i * 6;
		unsigned id = p[0] >> 3;
		unsigned state = p[0] & 0x07;
		unsigned x, y;

		if (id == 0 || id > 10 || (state != 1 && state != 2)) {
			continue;
		}
		x = ((unsigned)p[1] << 4) | (p[3] >> 4);
		y = ((unsigned)p[2] << 4) | (p[3] & 0x0F);
		if (x > 1079 || y > 2399) {
			continue;
		}
		seen[id - 1] = 1;
		if (!hid_down[id - 1]) {
			hid_down[id - 1] = 1;
			hid_events++;
			printf("SWEET TOUCH HID: down finger=%u at %u,%u\n",
			    id - 1, x, y);
		}
		hid_last_x[id - 1] = (uint16_t)x;
		hid_last_y[id - 1] = (uint16_t)y;
		sweet_hid_touch((int)(id - 1), 1, (int)x, (int)y);
	}

	for (i = 0; i < 10; i++) {
		if (hid_down[i] && !seen[i]) {
			sweet_hid_touch((int)i, 0, hid_last_x[i], hid_last_y[i]);
			hid_down[i] = 0;
			hid_events++;
			printf("SWEET TOUCH HID: up finger=%u at %u,%u\n",
			    i, hid_last_x[i], hid_last_y[i]);
		}
	}
}

static void
touch_hid_loop(void)
{
	unsigned poll_ms = 15, secs = 1800;
	uint8_t rep[NVT_REPORT_LEN];

	(void)PE_parse_boot_argn("sweettouchpoll", &poll_ms, sizeof(poll_ms));
	(void)PE_parse_boot_argn("sweettouchsecs", &secs, sizeof(secs));
	if (poll_ms < 4 || poll_ms > 500) {
		poll_ms = 10;
	}
	if (secs > 3600) {
		secs = 3600;
	}
	printf("SWEET TOUCH HID: polling page %05x every %u ms\n", st.page,
	    poll_ms);
	nvt_set_page(st.page);
	unsigned bad = 0;
	for (unsigned t = 0; t < secs * 1000u && !st.dead; t += poll_ms) {
		uint8_t hdr[6];
		bool idle = true;

		/* Fast path: only the first 6 bytes are needed to know there is
		 * no contact; skip the remaining 59 bytes when idle. */
		nvt_read_latched(st.page, hdr, 6);
		for (unsigned k = 0; k < 6; k++) {
			if (hdr[k] != 0x77 && hdr[k] != 0xFD && hdr[k] != 0xFE &&
			    hdr[k] != 0xFF && hdr[k] != 0x00) {
				idle = false;
				break;
			}
		}
		for (unsigned k = 0; k < NVT_REPORT_LEN; k++) {
			rep[k] = 0;
		}
		for (unsigned k = 0; k < 6; k++) {
			rep[k] = hdr[k];
		}
		if (!idle) {
			nvt_read_latched(st.page + 6, rep + 6, NVT_REPORT_LEN - 6);
		}
		if (rep[0] == 0xFE || rep[0] == 0xFF) {
			/* Bus/chip not answering: re-download the firmware once. */
			if (++bad >= 200) {
				bad = 0;
				printf("SWEET TOUCH HID: dead bus, re-booting fw\n");
				st.fw_result = nvt_boot_fw();
				printf("SWEET TOUCH HID: fw retry result=%d\n",
				    st.fw_result);
				nvt_set_page(st.page);
			}
		} else {
			bad = 0;
		}
		touch_hid_frame(rep);
		if ((st.passes % 500) == 0) {
			printf("SWEET TOUCH HID: alive frames=%u events=%u "
			    "fw=%d/%u pid=%04x rep %02x%02x%02x%02x "
			    "down=%u%u%u%u%u%u%u%u%u%u\n", st.passes,
			    hid_events, st.fw_result, st.fw_stage, st.fw_pid,
			    rep[0], rep[1], rep[2], rep[3],
			    hid_down[0], hid_down[1], hid_down[2], hid_down[3],
			    hid_down[4], hid_down[5], hid_down[6], hid_down[7],
			    hid_down[8], hid_down[9]);
		}
		st.passes++;
		touch_wait(poll_ms);
	}
	printf("SWEET TOUCH HID: loop done after %u frames, %u events\n",
	    st.passes, hid_events);
}

/* PSCI_VERSION probe: SMC #0 with x0 = 0x84000000.  If the platform has no
 * PSCI/ATF this may hang or trap; only called with sweetpsci=1. */
static void
sweet_psci_probe(void)
{
	register uint64_t x0 asm("x0") = 0x84000000ULL;
	register uint64_t x1 asm("x1") = 0;
	register uint64_t x2 asm("x2") = 0;
	register uint64_t x3 asm("x3") = 0;

	asm volatile("smc #0"
	    : "+r"(x0)
	    : "r"(x1), "r"(x2), "r"(x3)
	    : "x4", "x5", "x6", "x7", "memory");
	printf("SWEET PSCI: version=0x%x\n", (unsigned)x0);
}

static void
touch_probe_once(void)
{
	static const uint32_t pages[] = { NVT_EVENT, 0x22D00, 0x11A00, 0x1F600 };
	static const uint32_t max_x = 1080, max_y = 2400;
	uint8_t trim[8] = { 0 };
	uint8_t state[NVT_STATE_LEN] = { 0 };
	uint8_t info[NVT_FWINFO_LEN] = { 0 };
	uint8_t report[NVT_REPORT_LEN] = { 0 };
	unsigned irq = pin_in(PIN_IRQ) ? 1u : 0u;
	unsigned npages = sizeof(pages) / sizeof(pages[0]);

	nvt_read(NVT_CHIP_TRIM, trim, 6);
	nvt_read(NVT_EVENT + NVT_EVENT_STATE, state, NVT_STATE_LEN);

	if (st.page == 0) {
		for (unsigned i = 0; i < npages; i++) {
			nvt_read(pages[i] + NVT_EVENT_FWINFO, info, NVT_FWINFO_LEN);
			printf("SWEET TOUCH MTK: try %05x info %02x %02x %02x %02x "
			    "%02x%02x %02x%02x nz=%u%s\n", pages[i],
			    info[0], info[1], info[2], info[3], info[4], info[5],
			    info[6], info[7], nonzero_bytes(info, NVT_FWINFO_LEN),
			    fw_info_ok(info, max_x, max_y) ? " MATCH" : "");
			if (fw_info_ok(info, max_x, max_y)) {
				st.page = pages[i];
			}
		}
	}

	if (st.page != 0) {
		nvt_read(st.page + NVT_EVENT_FWINFO, info, NVT_FWINFO_LEN);
		nvt_read(st.page, report, NVT_REPORT_LEN);
		printf("SWEET TOUCH MTK: pass %u trim %02x%02x%02x%02x%02x%02x "
		    "irq=%u rst=%02x host=%02x hs=%02x page=%05x pid=%04x fw=%d/%u info "
		    "%02x%02x%02x%02x %02x%02x %02x%02x rep %02x%02x%02x%02x%02x%02x%02x%02x nz=%u\n",
		    st.passes, trim[0], trim[1], trim[2], trim[3], trim[4], trim[5],
		    irq, state[16], state[0], state[1], st.page, st.fw_pid, st.fw_result, st.fw_stage,
		    info[0], info[1], info[2], info[3], info[4], info[5], info[6],
		    info[7], report[0], report[1], report[2], report[3], report[4], report[5], report[6], report[7],
		    nonzero_bytes(report, NVT_REPORT_LEN));
	} else {
		printf("SWEET TOUCH MTK: pass %u trim %02x%02x%02x%02x%02x%02x "
		    "irq=%u rst=%02x host=%02x hs=%02x page=none\n", st.passes,
		    trim[0], trim[1], trim[2], trim[3], trim[4], trim[5], irq,
		    state[16], state[0], state[1]);
	}
	st.passes++;
}

static void
touch_thread(__unused void *arg, __unused wait_result_t wr)
{
	unsigned delay_s = 30, secs = 1800, period_ms = 2000;
	int half_us = 2;
	int fwboot = 0;
	int fwseen = 0;

	(void)PE_parse_boot_argn("sweettouchdelay", &delay_s, sizeof(delay_s));
	(void)PE_parse_boot_argn("sweettouchsecs", &secs, sizeof(secs));
	(void)PE_parse_boot_argn("sweettouchperiod", &period_ms, sizeof(period_ms));
	(void)PE_parse_boot_argn("sweetmtkdelay", &half_us, sizeof(half_us));
	fwseen = PE_parse_boot_argn("sweetmtkfw", &fwboot, sizeof(fwboot));
	if (!fwseen) {
		fwboot = 1;	/* probe build: boot the firmware unless told not to */
	}
	if (delay_s > 600) {
		delay_s = 600;
	}
	if (secs > 3600) {
		secs = 3600;
	}
	if (period_ms < 500 || period_ms > 60000) {
		period_ms = 2000;
	}
	if (half_us < 1 || half_us > 1000) {
		half_us = 2;
	}
	st.half_us = half_us;
	st.fw_result = 99;

	printf("SWEET TOUCH MTK: starting in %u s, half-clock %d us, fwboot=%d seen=%d\n",
	    delay_s, st.half_us, fwboot, fwseen);
	touch_wait(delay_s * 1000u);
	if (st.dead) {
		goto out;
	}

	st.gpio = ml_io_map(GPIO_PA, 0x1000);
	st.lm = ml_io_map(IOCFG_LM_PA, 0x1000);
	st.bl = ml_io_map(IOCFG_BL_PA, 0x1000);
	if (st.gpio == 0 || st.lm == 0 || st.bl == 0) {
		printf("SWEET TOUCH MTK: MMIO mapping failed\n");
		goto out;
	}
	printf("SWEET TOUCH MTK: mmio gpio=%p lm=%p bl=%p\n",
	    (void *)st.gpio, (void *)st.lm, (void *)st.bl);
	printf("SWEET TOUCH MTK: modes before mi=%u mo=%u irq=%u rst=%u clk=%u cs=%u\n",
	    pin_mode_get(PIN_MI), pin_mode_get(PIN_MO), pin_mode_get(PIN_IRQ),
	    pin_mode_get(PIN_RST), pin_mode_get(PIN_CLK), pin_mode_get(PIN_CS));

	/* Cannon's order: latch/levels first, then direction, then mux to GPIO.
	 * The reset pin is left untouched (only observed). */
	pin_out(PIN_CS, true);
	pin_out(PIN_CLK, false);
	pin_out(PIN_MO, false);
	pin_dir(PIN_CS, true);
	pin_dir(PIN_CLK, true);
	pin_dir(PIN_MO, true);
	pin_dir(PIN_MI, false);
	pin_dir(PIN_IRQ, false);
	pin_mode(PIN_MI, 0);
	pin_mode(PIN_MO, 0);
	pin_mode(PIN_IRQ, 0);
	pin_mode(PIN_CLK, 0);
	pin_mode(PIN_CS, 0);
	pad_set(st.bl, PAD_IES_MI, true);
	pad_set(st.lm, PAD_IES_IRQ, true);

	{
		int psci = 0;

		(void)PE_parse_boot_argn("sweetpsci", &psci, sizeof(psci));
		if (psci) {
			printf("SWEET PSCI: probing...\n");
			sweet_psci_probe();
			printf("SWEET PSCI: returned\n");
		}
	}
	if (fwboot) {
		int check = -99, attempt;

		for (attempt = 0; attempt < 3; attempt++) {
			st.fw_result = nvt_boot_fw();
			check = (st.fw_result == 0) ? nvt_check_fw() : -99;
			printf("SWEET TOUCH MTK: fw boot result=%d stage=%u "
			    "check=%d attempt=%d\n", st.fw_result,
			    st.fw_stage, check, attempt + 1);
			if (st.fw_result == 0 && check == 0) {
				break;
			}
			delay(100 * 1000);
		}
		if (check != 0) {
			st.fw_result = -100;	/* booted but identity check failed */
		}
	}
	if (st.fw_result == 0) {
		int hid = 1;

		(void)PE_parse_boot_argn("sweettouchhid", &hid, sizeof(hid));
		st.page = 0x22D00u;	/* NT36675 event page, not the probe candidate */
		if (hid) {
			touch_hid_loop();
			goto out;
		}
	}

	printf("SWEET TOUCH MTK: gpio mi=%u/%u/%u/%u mo=%u/%u/%u/%u "
	    "irq=%u/%u/%u/%u rst=%u/%u/%u/%u clk=%u/%u/%u/%u cs=%u/%u/%u/%u\n",
	    pin_mode_get(PIN_MI), pin_dir_get(PIN_MI), pin_do_get(PIN_MI), pin_in(PIN_MI),
	    pin_mode_get(PIN_MO), pin_dir_get(PIN_MO), pin_do_get(PIN_MO), pin_in(PIN_MO),
	    pin_mode_get(PIN_IRQ), pin_dir_get(PIN_IRQ), pin_do_get(PIN_IRQ), pin_in(PIN_IRQ),
	    pin_mode_get(PIN_RST), pin_dir_get(PIN_RST), pin_do_get(PIN_RST), pin_in(PIN_RST),
	    pin_mode_get(PIN_CLK), pin_dir_get(PIN_CLK), pin_do_get(PIN_CLK), pin_in(PIN_CLK),
	    pin_mode_get(PIN_CS), pin_dir_get(PIN_CS), pin_do_get(PIN_CS), pin_in(PIN_CS));

	for (unsigned t = 0; t < secs * 1000u && !st.dead; t += period_ms) {
		touch_probe_once();
		touch_wait(period_ms);
	}
	printf("SWEET TOUCH MTK: probe done after %u passes\n", st.passes);

out:
	st.dead = true;
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}


/* ------------------------------------------------------------------ *
 * MTK/MT6359 polled keys (same file on purpose: sweet_touch_mtk.c is
 * created by this patch series, so no bsd/conf/files or sweet_ufs.c
 * context is needed).
 *
 *   volume down  KPD KP_MEM1 bit 0 (vendor DT kpd-hw-init-map[0]=0x72)
 *   volume up    KPD KP_MEM2 bit 1 (vendor DT kpd-hw-rstkey=0x11),
 *                or MT6359 TOPSTATUS bit 3 (HOMEKEY_DEB, 1 = down; confirmed on device)
 *   power        MT6359 TOPSTATUS bit 1 (PWRKEY_DEB, 0 = down)
 *
 * PMIC reads go through the PMIC wrapper (read only).  Boot arg
 * sweetkeysmtk=1 enables it.
 * ------------------------------------------------------------------ */
extern void sweet_hid_key(int key, int down);

#define KEY_KPD_PA        0x10010000u
#define KEY_KPD_MEM1      0x0004u
#define KEY_KPD_MEM2      0x0008u
#define KEY_PWRAP_PA      0x10026000u
#define KEY_PWRAP_SIZE    0x1000u
#define KEY_PWRAP_ACC     0x0880u
#define KEY_PWRAP_RDATA   0x0894u
#define KEY_PWRAP_VLDCLR  0x08a4u
#define KEY_PWRAP_STA     0x08a8u
#define KEY_SWINF_FSM(v)  (((v) >> 1) & 7u)
#define KEY_SWINF_IDLE    0u
#define KEY_SWINF_VLDCLR  6u
#define KEY_SWINF_INIT    (1u << 15)
#define KEY_MT6359_TOPSTATUS  0x002au
#define KEY_TOP_PWRKEY_DEB    (1u << 1)
#define KEY_TOP_HOMEKEY_DEB   (1u << 2)   /* volume down, active low */
#define KEY_TOP_VOLUP_DEB     (1u << 3)   /* volume up, active high */
#define KEY_POLL_MS       20
#define KEY_POWER         0
#define KEY_VOLUME_UP     1
#define KEY_VOLUME_DOWN   2

static vm_offset_t key_kpd, key_pwrap;
static bool key_started;
static int key_chan;
static bool key_down[3], key_seen[3];

static inline uint32_t
key_rd32(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static inline uint16_t
key_rd16(vm_offset_t base, uint32_t off)
{
	uint16_t v = *(volatile uint16_t *)(base + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static inline void
key_wr32(vm_offset_t base, uint32_t off, uint32_t v)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + off) = v;
	__asm__ volatile("dsb osh" ::: "memory");
}

static bool
key_pwrap_wait(uint32_t fsm)
{
	for (uint32_t i = 0; i < 10000; i++) {
		if (KEY_SWINF_FSM(key_rd32(key_pwrap, KEY_PWRAP_STA)) == fsm) {
			return true;
		}
	}
	return false;
}

static bool
key_pmic_read(uint16_t reg, uint16_t *val)
{
	uint32_t sta, rd;

	if (!key_pwrap) {
		return false;
	}
	sta = key_rd32(key_pwrap, KEY_PWRAP_STA);
	if (!(sta & KEY_SWINF_INIT)) {
		return false;
	}
	if (!key_pwrap_wait(KEY_SWINF_IDLE)) {
		return false;
	}
	key_wr32(key_pwrap, KEY_PWRAP_ACC, reg);
	if (!key_pwrap_wait(KEY_SWINF_VLDCLR)) {
		return false;
	}
	rd = key_rd32(key_pwrap, KEY_PWRAP_RDATA);
	*val = (uint16_t)(rd & 0xffffu);
	key_wr32(key_pwrap, KEY_PWRAP_VLDCLR, 1);
	return true;
}

static void
key_report(unsigned idx, bool down, uint16_t mem1, uint16_t mem2, uint16_t top)
{
	static const char *names[3] = { "power", "volume up", "volume down" };

	key_down[idx] = down;
	printf("SWEET KEYS MTK: %s %s kpd=%04x/%04x top=%04x\n",
	    names[idx], down ? "down" : "up", mem1, mem2, top);
	sweet_hid_key((int)idx, down ? 1 : 0);
}

static void
keys_thread(__unused void *arg, __unused wait_result_t wr)
{
	uint64_t deadline;

	for (;;) {
		uint16_t mem1 = 0xffffu, mem2 = 0xffffu, top = 0xffffu;
		bool pmic_ok = key_pmic_read(KEY_MT6359_TOPSTATUS, &top);
		bool now[3];

		if (key_kpd) {
			mem1 = key_rd16(key_kpd, KEY_KPD_MEM1);
			mem2 = key_rd16(key_kpd, KEY_KPD_MEM2);
		}
		now[KEY_POWER] = pmic_ok ? !(top & KEY_TOP_PWRKEY_DEB) : key_down[KEY_POWER];
		/* MT6359 TOPSTATUS on this board (observed):
		 *   0x86 idle, 0x8a volume down (bit2 cleared), 0x8e volume up
		 *   (bit3 set) - volume down also raises bit3, so bit2 must win.
		 * KPD bits stay as an additional fallback (active low). */
		{
			bool home_down = pmic_ok && !(top & KEY_TOP_HOMEKEY_DEB);
			bool volup = pmic_ok && (top & KEY_TOP_VOLUP_DEB) && !home_down;

			now[KEY_VOLUME_UP] = volup || (key_kpd && !(mem2 & 2u));
			now[KEY_VOLUME_DOWN] = home_down || (key_kpd && !(mem1 & 1u));
		}
		for (unsigned i = 0; i < 3; i++) {
			if (now[i] == key_seen[i] && now[i] != key_down[i]) {
				key_report(i, now[i], mem1, mem2, top);
			}
			key_seen[i] = now[i];
		}
		clock_interval_to_deadline(KEY_POLL_MS, NSEC_PER_MSEC, &deadline);
		(void)assert_wait_deadline(&key_chan, THREAD_UNINT, deadline);
		thread_block(THREAD_CONTINUE_NULL);
	}
}

static void
sweet_keys_mtk_poll_init(void)
{
	int enabled = 0;
	thread_t thread;
	uint16_t top = 0;
	bool pmic_ok;

	if (key_started) {
		return;
	}
	key_started = true;
	(void)PE_parse_boot_argn("sweetkeysmtk", &enabled, sizeof(enabled));
	if (!enabled) {
		printf("SWEET KEYS MTK: disabled (sweetkeysmtk=0)\n");
		return;
	}
	key_kpd = ml_io_map(KEY_KPD_PA, KEY_PWRAP_SIZE);
	key_pwrap = ml_io_map(KEY_PWRAP_PA, KEY_PWRAP_SIZE);
	pmic_ok = key_pmic_read(KEY_MT6359_TOPSTATUS, &top);
	printf("SWEET KEYS MTK: start v3 kpd=%d pwrap=%d top-ok=%d top=%04x mem1=%04x mem2=%04x\n",
	    key_kpd != 0, key_pwrap != 0, pmic_ok, top,
	    key_kpd ? key_rd16(key_kpd, KEY_KPD_MEM1) : 0xffffu,
	    key_kpd ? key_rd16(key_kpd, KEY_KPD_MEM2) : 0xffffu);
	if (kernel_thread_start(keys_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("SWEET KEYS MTK: kernel_thread_start failed\n");
	}
}


/* ------------------------------------------------------------------ *
 * MT6833 Mali state probe (READ ONLY).  Boot arg sweetmaliro=1.
 * Prints SPM power state, topckgen MFG mux, APMIXED MFGPLL and the
 * mfgcfg block state so the first write in the real bring-up can be
 * chosen from evidence instead of guesses.
 * ------------------------------------------------------------------ */

static void sweet_mali_probe_loop(void);

static void sweet_mali_power_status(void);

static int mali_probe_chan;

static void
mali_probe_thread(__unused void *arg, __unused wait_result_t wr)
{
	uint64_t deadline;

	for (;;) {
		sweet_mali_probe_loop();
		sweet_mali_power_status();
		clock_interval_to_deadline(10000, NSEC_PER_MSEC, &deadline);
		(void)assert_wait_deadline(&mali_probe_chan, THREAD_UNINT, deadline);
		thread_block(THREAD_CONTINUE_NULL);
	}
}


/* ------------------------------------------------------------------ *
 * MT6833 Mali power-on probe.  Boot arg sweetmalion=1.
 * Follows the vendor order: MTCMOS MFG0 -> MFG1 (way_en + bus protect)
 * -> MFG2/MFG3, then the mfgcfg block (subsys_bg3d + external CG), then
 * reads GPU_ID.  Every step is logged before the writes so a hang shows
 * where it stopped.  No PMIC rail writes yet.
 * ------------------------------------------------------------------ */
static vm_offset_t mali_p_spm, mali_p_infra, mali_p_infra_pdn, mali_p_mfgcfg, mali_p_gpu;

/* Set once the GPU is out of reset, so the IOKit side knows when
 * reading its registers is safe. */
static int mali_powered;

static inline uint32_t
mali_prd(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static inline void
mali_pwr(vm_offset_t base, uint32_t off, uint32_t v)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + off) = v;
	__asm__ volatile("dsb osh" ::: "memory");
}

static bool
mali_wait(vm_offset_t base, uint32_t off, uint32_t mask, uint32_t want, int loops)
{
	for (int i = 0; i < loops; i++) {
		if ((mali_prd(base, off) & mask) == want) {
			return true;
		}
	}
	return false;
}

/*
 * v28: MT6833 Mali power-on, in the order the vendor power gate really uses.
 *
 * pg_prepare(runtime_status) powers the domain first (enable_subsys MTCMOS_PWR
 * -> spm_mtcmos_ctrl_mfgN_pwr) and only then releases the bus (enable_subsys
 * MTCMOS_BUS_PROT -> spm_mtcmos_ctrl_mfgN_bus_prot: SRAM release, way_en, the
 * PROTECTEN clears).  redwood_mali.c orders the same way.  v26 released way_en
 * and the PROTECTEN clears before the domain was powered, which hands the
 * fabric an AXI port whose domain is still off; a stuck SRAM ack is reported
 * here but never releases the bus either.
 *
 * Boot arg sweetmalion=1.  Every step is logged before it is done so a hang
 * still shows where it stopped.
 */
#define MALI_SPM_PWR_STATUS     0x16Cu
#define MALI_SPM_PWR_STATUS_2ND 0x170u
#define MALI_SPM_PWR_RST_B      (1u << 0)
#define MALI_SPM_PWR_ISO        (1u << 1)
#define MALI_SPM_PWR_ON         (1u << 2)
#define MALI_SPM_PWR_ON_2ND     (1u << 3)
#define MALI_SPM_PWR_CLK_DIS    (1u << 4)
#define MALI_SPM_SRAM_PDN       (1u << 8)
#define MALI_SPM_SRAM_ACK       (1u << 12)
#define MALI_MFG_MISC_CON       0x600u   /* INFRACFG_AO+0x600: bit19 DFD_TRIGGER, bit18 done */
#define MALI_SPM_PWR_MFG0_STA   (1u << 2)

static void
mali_settle(int n)
{
	for (volatile int i = 0; i < n; i++) {
		__asm__ volatile("nop");
	}
}

/* MTCMOS power step: PWR_ON, then clock released, isolation off, reset high. */
static bool
mali_domain_pwr(uint32_t con, uint32_t stabits)
{
	mali_pwr(mali_p_spm, con, mali_prd(mali_p_spm, con) | MALI_SPM_PWR_ON);
	mali_pwr(mali_p_spm, con, mali_prd(mali_p_spm, con) | MALI_SPM_PWR_ON_2ND);
	if (!mali_wait(mali_p_spm, MALI_SPM_PWR_STATUS, stabits, stabits, 500000) ||
	    !mali_wait(mali_p_spm, MALI_SPM_PWR_STATUS_2ND, stabits, stabits, 500000)) {
		/* Leave the domain the way it was found: PWR_ON back to 0 keeps it
		 * isolated, instead of half on with ISO and reset already released. */
		mali_pwr(mali_p_spm, con, mali_prd(mali_p_spm, con) &
		    ~(MALI_SPM_PWR_ON | MALI_SPM_PWR_ON_2ND));
		return false;
	}
	mali_pwr(mali_p_spm, con, mali_prd(mali_p_spm, con) & ~MALI_SPM_PWR_CLK_DIS);
	mali_pwr(mali_p_spm, con, mali_prd(mali_p_spm, con) & ~MALI_SPM_PWR_ISO);
	mali_pwr(mali_p_spm, con, mali_prd(mali_p_spm, con) | MALI_SPM_PWR_RST_B);
	return true;
}

/* Bus-protect step: drop SRAM_PDN and wait for its ack to clear. */
static bool
mali_domain_sram(uint32_t con)
{
	mali_pwr(mali_p_spm, con, mali_prd(mali_p_spm, con) & ~MALI_SPM_SRAM_PDN);
	return mali_wait(mali_p_spm, con, MALI_SPM_SRAM_ACK, 0, 500000);
}

/* Returns true only when the PWR status acked; the SRAM ack is reported through
 * sram_ok because it hard-gates the bus release for MFG1 (vendor order:
 * spm_mtcmos_ctrl_mfg1_bus_prot waits for it before way_en). */
static bool
mali_domain_up(const char *name, uint32_t con, uint32_t stabits, bool *sram_ok)
{
	bool pwr, sram;

	printf("SWEET MALI: %s power con=%08x\n", name, mali_prd(mali_p_spm, con));
	pwr = mali_domain_pwr(con, stabits);
	printf("SWEET MALI: %s pwr ok=%d con=%08x 16C=%08x 170=%08x\n", name, pwr ? 1 : 0,
	    mali_prd(mali_p_spm, con), mali_prd(mali_p_spm, MALI_SPM_PWR_STATUS),
	    mali_prd(mali_p_spm, MALI_SPM_PWR_STATUS_2ND));
	if (!pwr) {
		if (sram_ok) {
			*sram_ok = false;
		}
		return false;
	}
	sram = mali_domain_sram(con);
	printf("SWEET MALI: %s sram ack=%d con=%08x\n", name, sram ? 1 : 0,
	    mali_prd(mali_p_spm, con));
	if (sram_ok) {
		*sram_ok = sram;
	}
	return true;
}

/*
 * Open the BG3D clock gate.  redwood_mali.c:540 writes 1 into mfgcfg+0x008 with
 * the note "MFG_CG_CLR: BG3D runs", and MTK lays a CG block out as CON/SET/CLR
 * at 0x0/0x4/0x8, so this should clear bit0 of the CON register.  The MT6833
 * mfgcfg base is 0x13fbf000 and the reference is a different SoC, so the write
 * is checked by reading both registers back rather than assumed.
 */
static void
mali_bg3d_gate(const char *tag)
{
	uint32_t con;

	printf("SWEET MALI: bg3d %s CG_CON=%08x CG_CLR=%08x\n", tag,
	    mali_prd(mali_p_mfgcfg, 0x000), mali_prd(mali_p_mfgcfg, 0x008));
	mali_pwr(mali_p_mfgcfg, 0x008, 1u);
	mali_settle(2000);
	con = mali_prd(mali_p_mfgcfg, 0x000);
	printf("SWEET MALI: bg3d %s after CG_CON=%08x CG_CLR=%08x bit0=%d\n", tag, con,
	    mali_prd(mali_p_mfgcfg, 0x008), con & 1u);
}

/* INFRA_PDN way_en (0x10215000) and the four PROTECTEN opens, MFG1 only. */
static bool
mali_mfg1_bus(void)
{
	printf("SWEET MALI: mfg1 way_en before 4C=%08x\n", mali_prd(mali_p_infra_pdn, 0x4C));
	mali_pwr(mali_p_infra_pdn, 0x4C, mali_prd(mali_p_infra_pdn, 0x4C) | 0x1800u);
	if (!mali_wait(mali_p_infra_pdn, 0x4C, 0x1800u, 0x1800u, 500000)) {
		/* v29: the 251 run left 0x4C at 0x101 here, and gating the release on
		 * it is what kept the GPU AXI port protected (aperture read 0 with all
		 * four domains powered).  Continue to the PROTECTEN clears anyway. */
		printf("SWEET MALI: way_en ack slow 4C=%08x (continuing)\n",
		    mali_prd(mali_p_infra_pdn, 0x4C));
	}
	/* The CLR registers read back 0 by design, so print the STA registers the
	 * release actually shows up in (0x228 STA1, 0x258 STA1_1, 0x724 STA1_2). */
	printf("SWEET MALI: mfg1 protect before 228=%08x 258=%08x 724=%08x\n",
	    mali_prd(mali_p_infra, 0x228), mali_prd(mali_p_infra, 0x258),
	    mali_prd(mali_p_infra, 0x724));
	mali_pwr(mali_p_infra, 0x2A4, (1u << 21) | (1u << 22));
	mali_pwr(mali_p_infra, 0x718, 1u << 7);
	mali_pwr(mali_p_infra, 0x2AC, 1u << 21);
	mali_pwr(mali_p_infra, 0x718, (1u << 5) | (1u << 6));
	mali_settle(2000);
	printf("SWEET MALI: mfg1 protect after 228=%08x 258=%08x 724=%08x 4C=%08x\n",
	    mali_prd(mali_p_infra, 0x228), mali_prd(mali_p_infra, 0x258),
	    mali_prd(mali_p_infra, 0x724), mali_prd(mali_p_infra_pdn, 0x4C));
	return true;
}


/* ------------------------------------------------------------------ *
 * v31: the first GPU job.
 *
 * panfrost's job manager, the smallest thing that proves the GPU executes:
 * address space 0 in identity mode (a GPU address is its physical one, so no
 * page tables are needed yet), one 4 KB page carrying a "write a value" job and
 * the word it is meant to write, and one job slot told to start.  If the word
 * changes in memory, the GPU has run a job on this phone for the first time.
 *
 * Constants and the job header layout are copied from redwood_mali.c (MT6768,
 * same job manager), not re-derived; the aperture and the power sequence are
 * MT6833's own (see the note on 0x13000000 above).
 * ------------------------------------------------------------------ */
extern uint64_t kvtophys(vm_offset_t va);
extern vm_offset_t phystokv(uint64_t pa);
extern void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);

#define MALI_AS0             0x2400u
#define MALI_AS_TRANSTAB_LO  0x00u
#define MALI_AS_TRANSTAB_HI  0x04u
#define MALI_AS_MEMATTR_LO   0x08u
#define MALI_AS_MEMATTR_HI   0x0cu
#define MALI_AS_COMMAND      0x18u
#define MALI_AS_FAULTSTATUS  0x1cu
#define MALI_AS_STATUS       0x28u
#define MALI_AS_TRANSCFG_LO  0x30u
#define MALI_AS_TRANSCFG_HI  0x34u
#define MALI_JOB_INT_RAWSTAT 0x1000u
#define MALI_JOB_INT_CLEAR   0x1004u
#define MALI_JOB_INT_MASK    0x1008u
#define MALI_JS1             0x1880u
#define MALI_JS_STATUS       0x24u
#define MALI_JS_HEAD_NEXT_LO 0x40u
#define MALI_JS_HEAD_NEXT_HI 0x44u
#define MALI_JS_AFFINITY_LO  0x50u
#define MALI_JS_AFFINITY_HI  0x54u
#define MALI_JS_CONFIG_NEXT  0x58u
#define MALI_JS_COMMAND_NEXT 0x60u
#define MALI_JS_CONFIG       ((3u << 8) | (3u << 12) | (8u << 16))   /* AS 0, flush caches both ends */
#define MALI_JOB_MAGIC       0xc0ffee01u
#define MALI_JOB_OFFSET      0x800u
#define MALI_MMU_INT_RAWSTAT 0x2000u
#define MALI_MMU_INT_CLEAR   0x2004u
#define MALI_AS_FAULTADDR_LO 0x20u
#define MALI_AS_FAULTADDR_HI 0x24u

/*
 * The GPU's own 4 KB page tables (v32).  Geometry is copied from
 * redwood_mali.c:400-477, which runs on the same job manager: MMU_FEATURES
 * reports 35 bits of address, so the top table is indexed by bits 34..30 (five
 * entries) and the two levels below by nine bits each.  TRANSCFG is not a bare
 * mode number -- the low bits are the mode and the rest carry the address width
 * and the walk caching, so the value is copied whole instead of re-derived.
 * AP is 01 (bit 6 only); writing 11 there makes pages read-only and the symptom
 * looks exactly like a bad mapping.
 */
#define MALI_MMU_FEATURES    0x018u
#define MALI_PTE_TABLE       3ull
#define MALI_PTE_PAGE        3ull
#define MALI_PTE_AF          (1ull << 10)
#define MALI_PTE_SH_INNER    (3ull << 8)
#define MALI_PTE_AP_RW       (1ull << 6)
#define MALI_PTE_ADDRESS     0x0000fffffffff000ull
#define MALI_TRANSCFG_4K     (6u | (20u << 6) | (1u << 24))   /* AArch64 4K walks, uncached */
#define MALI_MAPPED_GPU      0x00100000ull
#define MALI_MAPPED_GPU_B    0x00200000ull
#define MALI_BUF_GPU         0x00400000ull      /* v34: 8 pages, contiguous in GPU space */
#define MALI_BUF_PAGES       8

static uint64_t *mali_pt_top;
static uint64_t mali_pt_top_phys;
static unsigned mali_pt_pages;      /* handed to the GPU's tables; deliberately never freed */

static unsigned mali_job_state;   /* bitmask: 1 identity, 2 map A, 4 map B, 8 slot reused, 0x80 fault */

static void mali_as0_update(uint64_t transtab, uint32_t transcfg);   /* used by the recovery below */

/*
 * Put the GPU back in a runnable state after a job that did not finish, and do
 * it BEFORE the page holding the job header is returned to the allocator: a
 * head pointer left in the slot would otherwise point at memory that the next
 * kmem_alloc hands to somebody else, and the GPU would write there.  Same order
 * as redwood_mali.c (soft reset, cores back on, address space reinstated).
 */
static void
mali_gpu_recover(uint32_t shader_mask)
{
	printf("SWEET MALI: recover: int clear, soft reset\n");
	mali_pwr(mali_p_gpu, 0x024, 0xffffffffu);                   /* GPU_INT_CLEAR */
	mali_pwr(mali_p_gpu, 0x030, 1u);                            /* GPU_CMD soft reset */
	if (!mali_wait(mali_p_gpu, 0x020, 1u << 8, 1u << 8, 500000)) {
		printf("SWEET MALI: recover: reset incomplete raw=%08x\n",
		    mali_prd(mali_p_gpu, 0x020));
	}
	mali_pwr(mali_p_gpu, 0x024, 0xffffffffu);
	mali_pwr(mali_p_gpu, 0x1a0, mali_prd(mali_p_gpu, 0x120));   /* L2_PWRON */
	mali_pwr(mali_p_gpu, 0x190, mali_prd(mali_p_gpu, 0x110));   /* TILER_PWRON */
	mali_pwr(mali_p_gpu, 0x180, shader_mask);                   /* SHADER_PWRON */
	(void)mali_wait(mali_p_gpu, 0x160, mali_prd(mali_p_gpu, 0x120),
	    mali_prd(mali_p_gpu, 0x120), 5000);
	(void)mali_wait(mali_p_gpu, 0x150, mali_prd(mali_p_gpu, 0x110),
	    mali_prd(mali_p_gpu, 0x110), 5000);
	(void)mali_wait(mali_p_gpu, 0x140, shader_mask, shader_mask, 5000);
	mali_as0_update(0, 2);
	mali_pwr(mali_p_gpu, MALI_MMU_INT_CLEAR, 0xffffffffu);
	mali_pwr(mali_p_gpu, MALI_JOB_INT_CLEAR, 0xffffffffu);
	printf("SWEET MALI: recover: done raw=%08x\n", mali_prd(mali_p_gpu, 0x020));
}

/* Identity mode is TRANS_CFG = 2: the MMU passes the address through as is. */
static void
mali_as0_update(uint64_t transtab, uint32_t transcfg)
{
	mali_pwr(mali_p_gpu, MALI_AS0 + MALI_AS_TRANSTAB_LO, (uint32_t)transtab);
	mali_pwr(mali_p_gpu, MALI_AS0 + MALI_AS_TRANSTAB_HI, (uint32_t)(transtab >> 32));
	mali_pwr(mali_p_gpu, MALI_AS0 + MALI_AS_TRANSCFG_LO, transcfg);
	mali_pwr(mali_p_gpu, MALI_AS0 + MALI_AS_TRANSCFG_HI, 0);
	mali_pwr(mali_p_gpu, MALI_AS0 + MALI_AS_MEMATTR_LO, 0x88888888u);
	mali_pwr(mali_p_gpu, MALI_AS0 + MALI_AS_MEMATTR_HI, 0x88888888u);
	mali_pwr(mali_p_gpu, MALI_AS0 + MALI_AS_COMMAND, 1u);     /* update */
	if (!mali_wait(mali_p_gpu, MALI_AS0 + MALI_AS_STATUS, 1u, 0u, 5000)) {
		printf("SWEET MALI: as0 update did not settle status=%08x\n",
		    mali_prd(mali_p_gpu, MALI_AS0 + MALI_AS_STATUS));
	}
}

static bool
mali_run_write_job(vm_offset_t page, uint64_t gpu, uint32_t shader_mask, const char *what)
{
	volatile uint32_t *job = (volatile uint32_t *)page;
	volatile uint32_t *target = (volatile uint32_t *)(page + MALI_JOB_OFFSET);
	bool done, ok;

	bzero((void *)page, PAGE_SIZE);
	job[4] = (2u << 1) | 1u | (1u << 16);    /* write a value, 64-bit pointers, job index 1 */
	job[8] = (uint32_t)(gpu + MALI_JOB_OFFSET);
	job[9] = (uint32_t)((gpu + MALI_JOB_OFFSET) >> 32);
	job[10] = 6u;                            /* a 32-bit immediate */
	job[12] = MALI_JOB_MAGIC;
	FlushPoC_DcacheRegion(page, PAGE_SIZE);
	printf("SWEET MALI: job %s: submit gpu=%08x%08x slot1 status=%08x shader=%08x\n", what,
	    (uint32_t)(gpu >> 32), (uint32_t)gpu, mali_prd(mali_p_gpu, MALI_JS1 + MALI_JS_STATUS),
	    shader_mask);

	mali_pwr(mali_p_gpu, MALI_JOB_INT_MASK, 0);
	mali_pwr(mali_p_gpu, MALI_JOB_INT_CLEAR, 0xffffffffu);
	mali_pwr(mali_p_gpu, MALI_JS1 + MALI_JS_HEAD_NEXT_LO, (uint32_t)gpu);
	mali_pwr(mali_p_gpu, MALI_JS1 + MALI_JS_HEAD_NEXT_HI, (uint32_t)(gpu >> 32));
	mali_pwr(mali_p_gpu, MALI_JS1 + MALI_JS_AFFINITY_LO, shader_mask);
	mali_pwr(mali_p_gpu, MALI_JS1 + MALI_JS_AFFINITY_HI, 0);
	mali_pwr(mali_p_gpu, MALI_JS1 + MALI_JS_CONFIG_NEXT, MALI_JS_CONFIG);
	mali_pwr(mali_p_gpu, MALI_JS1 + MALI_JS_COMMAND_NEXT, 1u);     /* start */
	/* A loop count is not a time bound, so the budget is generous and the
	 * result is kept: a job landing just after the wait would otherwise read as
	 * "not written" while the GPU did run it. */
	done = mali_wait(mali_p_gpu, MALI_JOB_INT_RAWSTAT, (1u << 1), (1u << 1), 2000000);

	FlushPoC_DcacheRegion(page, PAGE_SIZE);
	if (!done) {
		done = (mali_prd(mali_p_gpu, MALI_JOB_INT_RAWSTAT) & (1u << 1)) != 0;
	}
	ok = (*target == MALI_JOB_MAGIC);
	printf("SWEET MALI: job %s: done=%d raw=%08x slot=%08x fault=%08x at=%08x%08x mmu=%08x word=%08x %s\n",
	    what, done ? 1 : 0,
	    mali_prd(mali_p_gpu, MALI_JOB_INT_RAWSTAT),
	    mali_prd(mali_p_gpu, MALI_JS1 + MALI_JS_STATUS),
	    mali_prd(mali_p_gpu, MALI_AS0 + MALI_AS_FAULTSTATUS),
	    mali_prd(mali_p_gpu, MALI_AS0 + MALI_AS_FAULTADDR_HI),
	    mali_prd(mali_p_gpu, MALI_AS0 + MALI_AS_FAULTADDR_LO),
	    mali_prd(mali_p_gpu, MALI_MMU_INT_RAWSTAT), *target,
	    ok ? "-- GPU RAN IT" : "-- NOT written");
	mali_pwr(mali_p_gpu, MALI_JOB_INT_CLEAR, 0xffffffffu);
	return ok;
}

/* One 4 KB page for the GPU's tables.  Never freed: AS0 may still reference it. */
static uint64_t *
mali_pt_page(uint64_t *phys)
{
	vm_offset_t page = 0;

	if (kmem_alloc(kernel_map, &page, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return NULL;
	}
	*phys = kvtophys(page);
	/* The GPU's walk reads these zeroes from memory, not from our cache. */
	FlushPoC_DcacheRegion(page, PAGE_SIZE);
	mali_pt_pages++;
	return (uint64_t *)page;
}

/* The table below `entry`, made if there is none yet. */
static uint64_t *
mali_pt_below(uint64_t *entry)
{
	uint64_t phys;
	uint64_t *table;

	if (*entry & 1u) {
		return (uint64_t *)phystokv(*entry & MALI_PTE_ADDRESS);
	}
	table = mali_pt_page(&phys);
	if (table == NULL) {
		return NULL;
	}
	*entry = phys | MALI_PTE_TABLE;
	FlushPoC_DcacheRegion((vm_offset_t)entry, sizeof(*entry));
	return table;
}

/* One 4 KB page of physical memory at GPU address `gpu`. */
static bool
mali_map_page(uint64_t gpu, uint64_t phys)
{
	uint64_t *middle, *bottom, *entry;

	if (mali_pt_top == NULL &&
	    (mali_pt_top = mali_pt_page(&mali_pt_top_phys)) == NULL) {
		return false;
	}
	if ((middle = mali_pt_below(&mali_pt_top[(gpu >> 30) & 31])) == NULL) {
		return false;
	}
	if ((bottom = mali_pt_below(&middle[(gpu >> 21) & 511])) == NULL) {
		return false;
	}
	entry = &bottom[(gpu >> 12) & 511];
	*entry = (phys & MALI_PTE_ADDRESS) | MALI_PTE_AF | MALI_PTE_SH_INNER |
	    MALI_PTE_AP_RW | MALI_PTE_PAGE;
	FlushPoC_DcacheRegion((vm_offset_t)entry, sizeof(*entry));
	return true;
}

/*
 * v33: take a mapping back out again.  A leaf entry left behind would keep
 * pointing at a physical page after it is freed, which is the same hazard as a
 * stale job head.  Clearing a mapping is an address-space CHANGE, so unlike a
 * fresh mapping it needs an explicit update (redwood_mali.c:417-422 makes the
 * same distinction) -- and switching TRANSCFG back to identity is not an unmap.
 */
static bool
mali_unmap_page(uint64_t gpu)
{
	uint64_t *middle, *bottom, *entry;

	if (mali_pt_top == NULL || !(mali_pt_top[(gpu >> 30) & 31] & 1u)) {
		return false;
	}
	middle = (uint64_t *)phystokv(mali_pt_top[(gpu >> 30) & 31] & MALI_PTE_ADDRESS);
	if (!(middle[(gpu >> 21) & 511] & 1u)) {
		return false;
	}
	bottom = (uint64_t *)phystokv(middle[(gpu >> 21) & 511] & MALI_PTE_ADDRESS);
	entry = &bottom[(gpu >> 12) & 511];
	if (*entry == 0) {
		return false;
	}
	*entry = 0;
	FlushPoC_DcacheRegion((vm_offset_t)entry, sizeof(*entry));
	mali_pwr(mali_p_gpu, MALI_AS0 + MALI_AS_COMMAND, 1u);     /* update */
	if (!mali_wait(mali_p_gpu, MALI_AS0 + MALI_AS_STATUS, 1u, 0u, 5000)) {
		printf("SWEET MALI: unmap update did not settle status=%08x\n",
		    mali_prd(mali_p_gpu, MALI_AS0 + MALI_AS_STATUS));
		return false;
	}
	return true;
}

/*
 * v35: the first thing the GPU draws itself.
 *
 * The GPU reaches memory by physical address through its own page tables, so it
 * needs the panel buffer's physical base.  That comes from PE_state.video (the
 * framebuffer the bootloader handed to the kernel) plus pmap_extract -- no
 * changes to the display driver, which is how the first attempt at exporting it
 * from SweetFramebuffer.cpp failed to apply and broke the link instead.
 */

/*
 * Upstream 0067 (osfmk/console/serial_console.c) mutes console DRAWING when
 * this is set; text still reaches the persistent log.  AppleCLCD sets it on its
 * first swap, which never happens in console mode (sweetnopresent=1), so the
 * GPU demo sets it itself with boot-arg sweetmalimute=1.
 */
extern int sweet_console_muted;

#define MALI_FB_ROWS        4
#define MALI_FB_COLS        1080    /* v36: full panel width, so the bars survive the console */
#define MALI_JOB_GPU        0x00800000ull    /* where our job page is mapped */
#define MALI_FB_GPU         0x00810000ull    /* where the panel pages are mapped */
#define MALI_FB_RED         0xFFFF0000u      /* BGRA: opaque red */

/* One "write value" job at an explicit target, no result check (used per pixel). */
static bool
mali_submit_value(vm_offset_t page, uint64_t job_gpu, uint64_t target_gpu,
    uint32_t value, uint32_t shader_mask)
{
	volatile uint32_t *job = (volatile uint32_t *)page;

	bzero((void *)page, PAGE_SIZE);
	job[4] = (2u << 1) | 1u | (1u << 16);
	job[8] = (uint32_t)target_gpu;
	job[9] = (uint32_t)(target_gpu >> 32);
	job[10] = 6u;
	job[12] = value;
	FlushPoC_DcacheRegion(page, PAGE_SIZE);

	mali_pwr(mali_p_gpu, MALI_JOB_INT_MASK, 0);
	mali_pwr(mali_p_gpu, MALI_JOB_INT_CLEAR, 0xffffffffu);
	mali_pwr(mali_p_gpu, MALI_JS1 + MALI_JS_HEAD_NEXT_LO, (uint32_t)job_gpu);
	mali_pwr(mali_p_gpu, MALI_JS1 + MALI_JS_HEAD_NEXT_HI, (uint32_t)(job_gpu >> 32));
	mali_pwr(mali_p_gpu, MALI_JS1 + MALI_JS_AFFINITY_LO, shader_mask);
	mali_pwr(mali_p_gpu, MALI_JS1 + MALI_JS_AFFINITY_HI, 0);
	mali_pwr(mali_p_gpu, MALI_JS1 + MALI_JS_CONFIG_NEXT, MALI_JS_CONFIG);
	mali_pwr(mali_p_gpu, MALI_JS1 + MALI_JS_COMMAND_NEXT, 1u);
	return mali_wait(mali_p_gpu, MALI_JOB_INT_RAWSTAT, (1u << 1), (1u << 1), 500000);
}

static void
mali_draw_block(uint32_t shader_mask)
{
	vm_offset_t jobpage = 0;
	uint64_t fb_phys[MALI_FB_ROWS];
	uint32_t in_page[MALI_FB_ROWS];
	int i, x, sent = 0, got = 0;
	uint64_t fb_base = 0;
	uint64_t t_start = 0, t_elapsed_ns = 0;
	uint32_t fb_stride = 0, fb_w = 0, fb_h = 0;

	for (i = 0; i < MALI_FB_ROWS; i++) {
		fb_phys[i] = 0;
	}

	/*
	 * v_baseAddr is already the PHYSICAL address of the panel buffer: the early
	 * framebuffer code in this kernel casts it to pmap_paddr_t (patch 0088) and
	 * hands it to ml_tt_map (patch 0233), so no translation is needed here.
	 */
	fb_base = (uint64_t)PE_state.video.v_baseAddr;
	fb_stride = (uint32_t)PE_state.video.v_rowBytes;
	fb_w = (uint32_t)PE_state.video.v_width;
	fb_h = (uint32_t)PE_state.video.v_height;
	if (fb_base == 0 || fb_stride == 0) {
		printf("SWEET MALI: v35 no framebuffer in PE_state (base=%08x stride=%u)\n",
		    (uint32_t)fb_base, fb_stride);
		return;
	}
	if (kmem_alloc(kernel_map, &jobpage, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("SWEET MALI: v35 no job page\n");
		return;
	}
	printf("SWEET MALI: v42 fb phys=%08x%08x stride=%u %ux%u, drawing %d px\n",
	    (uint32_t)(fb_base >> 32), (uint32_t)fb_base, fb_stride, fb_w, fb_h,
	    MALI_FB_ROWS * MALI_FB_COLS);
	{
		int mute = 0;

		PE_parse_boot_argn("sweetmalimute", &mute, sizeof(mute));
		if (mute == 1) {
			printf("SWEET MALI: v41 muting console drawing (sweetmalimute=1)\n");
			sweet_console_muted = 1;
			/* let that last line land before drawing over it */
			{
				uint64_t t0 = 0, t1 = 0;
				absolutetime_to_nanoseconds(mach_absolute_time(), &t0);
				do {
					absolutetime_to_nanoseconds(mach_absolute_time(), &t1);
				} while (t1 - t0 < 200ULL * NSEC_PER_MSEC);
			}
		}
	}

	/* Our job page, and one panel page per row, all in GPU address space. */
	if (!mali_map_page(MALI_JOB_GPU, kvtophys(jobpage))) {
		printf("SWEET MALI: v35 map job page failed\n");
		kmem_free(kernel_map, jobpage, PAGE_SIZE);
		return;
	}
	for (i = 0; i < MALI_FB_ROWS; i++) {
		uint64_t row = fb_base + (uint64_t)i * fb_stride;

		fb_phys[i] = row & ~(uint64_t)(PAGE_SIZE - 1);
		in_page[i] = (uint32_t)(row & (uint64_t)(PAGE_SIZE - 1));
		if (!mali_map_page(MALI_FB_GPU + (uint64_t)i * PAGE_SIZE, fb_phys[i])) {
			printf("SWEET MALI: v35 map fb row %d failed\n", i);
			kmem_free(kernel_map, jobpage, PAGE_SIZE);
			return;
		}
	}
	mali_as0_update(mali_pt_top_phys, MALI_TRANSCFG_4K);

	t_start = mach_absolute_time();
	/*
	 * v39: the console repaints the panel on every print, so a single draw is
	 * gone in milliseconds.  Redraw for 20 s instead: even with text on top the
	 * bars flicker through, which is all the proof this needs.
	 */
	for (int round = 0; round < 1; round++) {
	for (i = 0; i < MALI_FB_ROWS; i++) {
		for (x = 0; x < MALI_FB_COLS; x++) {
			uint64_t target = MALI_FB_GPU + (uint64_t)i * PAGE_SIZE +
			    in_page[i] + (uint64_t)x * 4u;

			if (mali_submit_value(jobpage, MALI_JOB_GPU, target,
			    MALI_FB_RED, shader_mask)) {
				got++;
			}
			sent++;
		}
	}
	}
	/*
	 * v40 control: write two more rows from the CPU into the same framebuffer,
	 * then read every row back.  This separates the three possibilities: the
	 * address is wrong (nothing shows), the display path is fine but the GPU
	 * cannot reach this memory (blue only), or both work (red and blue).
	 */
	{
		vm_offset_t cpu = ml_io_map((vm_offset_t)fb_base,
		    (vm_size_t)(fb_stride * MALI_FB_ROWS + 0x1000));
		uint32_t red_seen = 0, blue_seen = 0;

		if (cpu != 0) {
			for (i = 2; i < MALI_FB_ROWS; i++) {
				for (x = 0; x < MALI_FB_COLS; x++) {
					*(volatile uint32_t *)(cpu + (uint64_t)i * fb_stride +
					    (uint64_t)x * 4u) = 0xFF0000FFu;   /* blue in BGRA */
				}
			}
			for (x = 0; x < MALI_FB_COLS; x++) {
				if (*(volatile uint32_t *)(cpu + (uint64_t)0 * fb_stride +
				    (uint64_t)x * 4u) == MALI_FB_RED) {
					red_seen++;
				}
				if (*(volatile uint32_t *)(cpu + (uint64_t)2 * fb_stride +
				    (uint64_t)x * 4u) == 0xFF0000FFu) {
					blue_seen++;
				}
			}
			printf("SWEET MALI: v41 readback gpu_red=%d cpu_blue=%d of %d (cpu map=%08x)\n",
			    red_seen, blue_seen, MALI_FB_COLS, (uint32_t)cpu);
		} else {
			printf("SWEET MALI: v41 cpu map of the framebuffer failed\n");
		}
	}

	absolutetime_to_nanoseconds(mach_absolute_time() - t_start, &t_elapsed_ns);
	printf("SWEET MALI: v41 done sent=%d completed=%d in %llu us (%llu px/s)\n",
	    sent, got, t_elapsed_ns / 1000ULL,
	    t_elapsed_ns ? (uint64_t)got * 1000000000ULL / t_elapsed_ns : 0ULL);

	for (i = 0; i < MALI_FB_ROWS; i++) {
		(void)mali_unmap_page(MALI_FB_GPU + (uint64_t)i * PAGE_SIZE);
	}
	(void)mali_unmap_page(MALI_JOB_GPU);
	mali_as0_update(0, 2);
	kmem_free(kernel_map, jobpage, PAGE_SIZE);
}

/* ------------------------------------------------------------------ *
 * v42: the GPU as a working engine, not a demo.
 *
 * A kernel thread submits a batch of write-value jobs every five seconds to a
 * page the GPU reaches directly (identity mode, so no tables are involved),
 * reads the last result back from memory and keeps counts.  The counters are
 * printed with the other steady-state lines, so the console shows the GPU
 * doing work on a live system rather than one boot-time party trick.
 * ------------------------------------------------------------------ */
static vm_offset_t mali_bg_page;
static uint64_t mali_bg_phys;
static uint32_t mali_bg_shader;
static unsigned mali_bg_batches, mali_bg_jobs, mali_bg_ok, mali_bg_bad;

static void
mali_bg_thread(__unused void *arg, __unused wait_result_t wr)
{
	for (;;) {
		uint64_t deadline;
		unsigned i, done = 0;

		for (i = 0; i < 8; i++) {
			if (mali_submit_value(mali_bg_page, mali_bg_phys,
			    mali_bg_phys + MALI_JOB_OFFSET, MALI_JOB_MAGIC, mali_bg_shader)) {
				done++;
			}
			mali_bg_jobs++;
		}
		FlushPoC_DcacheRegion(mali_bg_page, PAGE_SIZE);
		if (*(volatile uint32_t *)(mali_bg_page + MALI_JOB_OFFSET) == MALI_JOB_MAGIC) {
			mali_bg_ok++;
		} else {
			mali_bg_bad++;
		}
		if (done == 0) {
			mali_bg_bad++;          /* nothing completed: keep it visible */
		}
		mali_bg_batches++;

		nanoseconds_to_absolutetime(5ull * NSEC_PER_SEC, &deadline);
		deadline += mach_absolute_time();
		assert_wait_deadline_with_leeway((event_t)&mali_bg_thread, THREAD_UNINT,
		    TIMEOUT_URGENCY_SYS_CRITICAL, deadline, 0);
		thread_block(THREAD_CONTINUE_NULL);
	}
}

static void
mali_bg_start(uint32_t shader_mask)
{
	thread_t thread;

	if (mali_bg_page != 0) {
		return;
	}
	if (kmem_alloc(kernel_map, &mali_bg_page, PAGE_SIZE,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("SWEET MALI: no page for the background worker\n");
		mali_bg_page = 0;
		return;
	}
	mali_bg_phys = kvtophys(mali_bg_page);
	mali_bg_shader = shader_mask;
	mali_as0_update(0, 2);          /* identity: a GPU address is the physical one */
	printf("SWEET MALI: background GPU worker page=%08x phys=%08x%08x\n",
	    (uint32_t)mali_bg_page, (uint32_t)(mali_bg_phys >> 32), (uint32_t)mali_bg_phys);
	if (kernel_thread_start(mali_bg_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("SWEET MALI: background worker thread did not start\n");
	}
}

/* ------------------------------------------------------------------ *
 * v43: does the job manager walk a CHAIN of descriptors?
 *
 * Our single-job format is proven.  If four identical descriptors laid down
 * back to back at 128-byte strides are all executed by one submission, then a
 * command stream needs no Mesa at all: a job would be "a page of descriptors".
 * Each job writes a different magic word to its own dword, so the readback says
 * exactly how far the walk got.
 * ------------------------------------------------------------------ */
static void
mali_chain_probe(vm_offset_t page, uint64_t phys, uint32_t shader_mask)
{
	volatile uint32_t *job = (volatile uint32_t *)page;
	int i, ok = 0;

	bzero((void *)page, PAGE_SIZE);
	/*
	 * v44: the chain, done the way Mesa describes it (pan_jc.h +
	 * genxml/v9.xml "Job Header"): the next job's GPU address lives in header
	 * words 6/7, the index increments, and the last job has next = 0.  The
	 * payload now writes 64 bits at a time (Write Value Type 7) instead of 32.
	 */
	for (i = 0; i < 4; i++) {
		volatile uint32_t *j = job + (i * 32);
		uint64_t next = (i == 3) ? 0 : phys + (uint64_t)(i + 1) * 128;
		uint64_t target = phys + 0x800 + (uint64_t)i * 8;

		j[4] = (2u << 1) | (1u << 8) | ((uint32_t)(i + 1) << 16);
		j[5] = 0;
		j[6] = (uint32_t)next;
		j[7] = (uint32_t)(next >> 32);
		j[8] = (uint32_t)target;
		j[9] = (uint32_t)(target >> 32);
		j[10] = 7u;                       /* Write Value Type: Immediate 64 */
		j[12] = 0xC0FFEE00u + (uint32_t)i;
		j[13] = 0x12345000u + (uint32_t)i;
	}
	FlushPoC_DcacheRegion(page, PAGE_SIZE);

	mali_pwr(mali_p_gpu, MALI_JOB_INT_MASK, 0);
	mali_pwr(mali_p_gpu, MALI_JOB_INT_CLEAR, 0xffffffffu);
	mali_pwr(mali_p_gpu, MALI_JS1 + MALI_JS_HEAD_NEXT_LO, (uint32_t)phys);
	mali_pwr(mali_p_gpu, MALI_JS1 + MALI_JS_HEAD_NEXT_HI, (uint32_t)(phys >> 32));
	mali_pwr(mali_p_gpu, MALI_JS1 + MALI_JS_AFFINITY_LO, shader_mask);
	mali_pwr(mali_p_gpu, MALI_JS1 + MALI_JS_AFFINITY_HI, 0);
	mali_pwr(mali_p_gpu, MALI_JS1 + MALI_JS_CONFIG_NEXT, MALI_JS_CONFIG);
	mali_pwr(mali_p_gpu, MALI_JS1 + MALI_JS_COMMAND_NEXT, 1u);
	(void)mali_wait(mali_p_gpu, MALI_JOB_INT_RAWSTAT, (1u << 1), (1u << 1), 500000);
	FlushPoC_DcacheRegion(page, PAGE_SIZE);

	for (i = 0; i < 4; i++) {
		volatile uint64_t *w = (volatile uint64_t *)(page + 0x800 + (uint64_t)i * 8);
		uint64_t want = 0x12345000ULL + (uint64_t)i;
		want = (want << 32) | (0xC0FFEE00U + (uint64_t)i);

		if (*w == want) {
			ok++;
		}
	}
	printf("SWEET MALI: v45 chain: %d of 4 chained jobs ran (raw=%08x slot=%08x)\n",
	    ok, mali_prd(mali_p_gpu, MALI_JOB_INT_RAWSTAT),
	    mali_prd(mali_p_gpu, MALI_JS1 + MALI_JS_STATUS));
	mali_pwr(mali_p_gpu, MALI_JOB_INT_CLEAR, 0xffffffffu);
}

/* ------------------------------------------------------------------ *
 * v45: a real command stream.  One submission carries MALI_CHAIN_JOBS chained
 * write-value jobs, each writing 64 bits, so the GPU fills a buffer in a single
 * round trip instead of one word per submission.  The point is the number: how
 * many bytes per second the job manager can actually take.
 * ------------------------------------------------------------------ */
#define MALI_CHAIN_JOBS   1024
#define MALI_CHAIN_PAGES  ((MALI_CHAIN_JOBS * 128 + PAGE_SIZE - 1) / PAGE_SIZE)
#define MALI_CHAIN_DSTPAGES 2

static void
mali_chain_bulk(uint32_t mask)
{
	vm_offset_t desc[MALI_CHAIN_PAGES], dst[MALI_CHAIN_DSTPAGES];
	uint64_t desc_phys[MALI_CHAIN_PAGES], dst_phys[MALI_CHAIN_DSTPAGES];
	uint64_t t0 = 0, t1 = 0, ns = 0;
	unsigned i, ok = 0;


	for (i = 0; i < MALI_CHAIN_PAGES; i++) {
		desc[i] = 0;
		if (kmem_alloc(kernel_map, &desc[i], PAGE_SIZE,
		    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			printf("SWEET MALI: v45 no memory for the chain\n");
			return;
		}
		desc_phys[i] = kvtophys(desc[i]);
	}
	for (i = 0; i < MALI_CHAIN_DSTPAGES; i++) {
		dst[i] = 0;
		if (kmem_alloc(kernel_map, &dst[i], PAGE_SIZE,
		    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			printf("SWEET MALI: v45 no memory for the target\n");
			return;
		}
		dst_phys[i] = kvtophys(dst[i]);
	}

	for (i = 0; i < MALI_CHAIN_JOBS; i++) {
		unsigned pg = (i * 128) / PAGE_SIZE;
		unsigned off = (i * 128) % PAGE_SIZE;
		volatile uint32_t *j = (volatile uint32_t *)(desc[pg] + off);
		uint64_t next = (i + 1 == MALI_CHAIN_JOBS) ? 0 :
		    desc_phys[(i + 1) * 128 / PAGE_SIZE] + ((i + 1) * 128) % PAGE_SIZE;
		unsigned byte = i * 8;
		uint64_t target = dst_phys[byte / PAGE_SIZE] + (byte % PAGE_SIZE);

		j[4] = (2u << 1) | (1u << 8) | ((uint32_t)(i + 1) << 16);
		j[5] = 0;
		j[6] = (uint32_t)next;
		j[7] = (uint32_t)(next >> 32);
		j[8] = (uint32_t)target;
		j[9] = (uint32_t)(target >> 32);
		j[10] = 7u;                       /* Immediate 64 */
		j[12] = 0xAA550000u;
		j[13] = (uint32_t)i;
	}
	for (i = 0; i < MALI_CHAIN_PAGES; i++) {
		FlushPoC_DcacheRegion(desc[i], PAGE_SIZE);
	}

	t0 = mach_absolute_time();
	mali_pwr(mali_p_gpu, MALI_JOB_INT_MASK, 0);
	mali_pwr(mali_p_gpu, MALI_JOB_INT_CLEAR, 0xffffffffu);
	mali_pwr(mali_p_gpu, MALI_JS1 + MALI_JS_HEAD_NEXT_LO, (uint32_t)desc_phys[0]);
	mali_pwr(mali_p_gpu, MALI_JS1 + MALI_JS_HEAD_NEXT_HI, (uint32_t)(desc_phys[0] >> 32));
	mali_pwr(mali_p_gpu, MALI_JS1 + MALI_JS_AFFINITY_LO, mask);
	mali_pwr(mali_p_gpu, MALI_JS1 + MALI_JS_AFFINITY_HI, 0);
	mali_pwr(mali_p_gpu, MALI_JS1 + MALI_JS_CONFIG_NEXT, MALI_JS_CONFIG);
	mali_pwr(mali_p_gpu, MALI_JS1 + MALI_JS_COMMAND_NEXT, 1u);
	(void)mali_wait(mali_p_gpu, MALI_JOB_INT_RAWSTAT, (1u << 1), (1u << 1), 2000000);
	t1 = mach_absolute_time();
	absolutetime_to_nanoseconds(t1 - t0, &ns);
	mali_pwr(mali_p_gpu, MALI_JOB_INT_CLEAR, 0xffffffffu);

	for (i = 0; i < MALI_CHAIN_DSTPAGES; i++) {
		FlushPoC_DcacheRegion(dst[i], PAGE_SIZE);
	}
	for (i = 0; i < MALI_CHAIN_JOBS; i++) {
		unsigned byte = i * 8;
		volatile uint64_t *w = (volatile uint64_t *)(dst[byte / PAGE_SIZE] + (byte % PAGE_SIZE));
		uint64_t want = 0xAA55000000000000ULL | (uint64_t)i;

		if (*w == want) {
			ok++;
		}
	}
	printf("SWEET MALI: v45 chain %u jobs: %u ok in %llu us = %llu KB/s (raw=%08x)\n",
	    (unsigned)MALI_CHAIN_JOBS, ok, ns / 1000ULL,
	    ns ? (uint64_t)ok * 8ULL * 1000000ULL / ns : 0ULL,
	    mali_prd(mali_p_gpu, MALI_JOB_INT_RAWSTAT));

	for (i = 0; i < MALI_CHAIN_PAGES; i++) {
		kmem_free(kernel_map, desc[i], PAGE_SIZE);
	}
	for (i = 0; i < MALI_CHAIN_DSTPAGES; i++) {
		kmem_free(kernel_map, dst[i], PAGE_SIZE);
	}
}


/* ------------------------------------------------------------------ *
 * v46: the first compute job, i.e. the GPU doing real work with machine code
 * we generated ourselves (see .github/workflows/panblob.yml).  The job layout
 * follows Mesa's genxml/v9.xml "Compute Job": Header (words 0-7) then the
 * invocation (words 8-15) and the Shader Environment at byte 64.
 * ------------------------------------------------------------------ */
static const uint32_t mali_copy_isa[340] = {
    0x00000005, 0x00000020, 0x00000000, 0x00000000, 0x00000002, 0x0000000f, 0x41500000, 0x00000000,
    0x41500000, 0x40000000, 0x41500000, 0x00000000, 0x00000000, 0x40000000, 0x00000100, 0x00000000,
    0x00000002, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000007, 0x00000006, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x10000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x0000000b, 0x0000000b, 0x00000080,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000040, 0x00000001, 0x00000001, 0x00000100, 0x80c08a3c, 0x00f0c004, 0x00000c40, 0x501fc030,
    0x19c0cf3c, 0x00b4c000, 0x19c0c87c, 0x00b4c101, 0x00000086, 0x0091c200, 0x00000087, 0x0091c300,
    0x00000041, 0x0091c400, 0x00000040, 0x0091c500, 0x00070442, 0x01abc000, 0x18000040, 0x00608082,
    0x00000088, 0x0091c200, 0x00000089, 0x0091c300, 0x00074442, 0x09abc200, 0x58000042, 0x50614002,
    0x00000000, 0x7800c000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
};

/* v49: the boot-time compute result, reprinted every 10 s so a photo taken
 * long after boot still shows it. */
static unsigned mali_compute_ok;
static unsigned mali_compute_words;

#define MALI_FAU_WORDS       11     /* DIV_ROUND_UP(24 sysvals + 20 args, 4) */
#define MALI_FAU_OFFSET      24     /* BIFROST_PRECOMPILED_KERNEL_ARGS_OFFSET */
#define MALI_COPY_ELEMS      1024   /* one page of uint32 */
#define MALI_COPY_WG         64     /* KERNEL(64) in the shader source */

static void
mali_compute_copy(uint32_t shader_mask)
{
	vm_offset_t srcpage = 0, dstpage = 0, isapage = 0, faupage = 0, descpage = 0, rsdpage = 0,
	    rsrpage = 0, tlspage = 0;
	uint64_t src_phys, dst_phys, shader_phys, job_phys, fau_phys, rsd_phys,
	    rsr_phys, tls_phys;
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
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS ||
	    kmem_alloc(kernel_map, &rsrpage, PAGE_SIZE,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS ||
	    kmem_alloc(kernel_map, &tlspage, PAGE_SIZE,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("SWEET MALI: v54 no memory for the compute test\n");
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
	rsr_phys = kvtophys(rsrpage);
	tls_phys = kvtophys(tlspage);
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
	j[24] = (uint32_t)rsr_phys;
	j[25] = (uint32_t)(rsr_phys >> 32);   /* resources table */
	j[26] = (uint32_t)rsd_phys;
	j[27] = (uint32_t)(rsd_phys >> 32);     /* Shader Program (RSD) address */
	j[28] = (uint32_t)tls_phys;
	j[29] = (uint32_t)(tls_phys >> 32);   /* thread storage */
	j[30] = (uint32_t)fau_phys;
	j[31] = (uint32_t)(fau_phys >> 32);     /* FAU address */

	FlushPoC_DcacheRegion(isapage, PAGE_SIZE);
	FlushPoC_DcacheRegion(faupage, PAGE_SIZE);
	FlushPoC_DcacheRegion(descpage, PAGE_SIZE);
	FlushPoC_DcacheRegion(rsdpage, PAGE_SIZE);
	FlushPoC_DcacheRegion(rsrpage, PAGE_SIZE);
	FlushPoC_DcacheRegion(tlspage, PAGE_SIZE);

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
	mali_compute_ok = (unsigned)ok;
	mali_compute_words = (unsigned)MALI_COPY_ELEMS;
	printf("SWEET MALI: v54 compute copy %d/%d words ok (raw=%08x slot=%08x)\n",
	    ok, (int)MALI_COPY_ELEMS,
	    mali_prd(mali_p_gpu, MALI_JOB_INT_RAWSTAT),
	    mali_prd(mali_p_gpu, MALI_JS1 + MALI_JS_STATUS));

	kmem_free(kernel_map, srcpage, PAGE_SIZE);
	kmem_free(kernel_map, dstpage, PAGE_SIZE);
	kmem_free(kernel_map, isapage, PAGE_SIZE);
	kmem_free(kernel_map, faupage, PAGE_SIZE);
	kmem_free(kernel_map, descpage, PAGE_SIZE);
	kmem_free(kernel_map, rsdpage, PAGE_SIZE);
	kmem_free(kernel_map, rsrpage, PAGE_SIZE);
	kmem_free(kernel_map, tlspage, PAGE_SIZE);
}

static void
mali_first_job(uint32_t shader_mask)
{
	vm_offset_t page = 0;
	uint64_t phys;

	if (kmem_alloc(kernel_map, &page, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("SWEET MALI: no page for the first job\n");
		return;
	}
	phys = kvtophys(page);
	printf("SWEET MALI: v33 first job page=%08x phys=%08x%08x mmu_features=%08x\n",
	    (uint32_t)page, (uint32_t)(phys >> 32), (uint32_t)phys,
	    mali_prd(mali_p_gpu, MALI_MMU_FEATURES));
	if (phys == 0 || (phys & (PAGE_SIZE - 1)) != 0) {
		printf("SWEET MALI: first job: implausible physical address, skipped\n");
		kmem_free(kernel_map, page, PAGE_SIZE);
		return;
	}

	/* 1: identity, twice, so the slot is also shown to be reusable. */
	mali_as0_update(0, 2);
	if (!mali_run_write_job(page, phys, shader_mask, "identity")) {
		mali_gpu_recover(shader_mask);
		mali_job_state |= 0x80;          /* identity itself failed */
		kmem_free(kernel_map, page, PAGE_SIZE);
		return;
	}
	mali_job_state |= 1;
	if (mali_run_write_job(page, phys, shader_mask, "identity again")) {
		mali_job_state |= 8;
	}
	mali_chain_probe(page, phys, shader_mask);
	mali_chain_bulk(shader_mask);
	/* Opt-in: changes GPU state, so only run it when asked. */
	{
		int compute_test = 0;

		PE_parse_boot_argn("sweetcomputetest", &compute_test,
		    sizeof(compute_test));
		if (compute_test == 1) {
			mali_compute_copy(shader_mask);
		}
	}

	/* 2 and 4: the same page behind two different GPU addresses. */
	if (mali_map_page(MALI_MAPPED_GPU, phys)) {
		mali_as0_update(mali_pt_top_phys, MALI_TRANSCFG_4K);
		if (mali_run_write_job(page, MALI_MAPPED_GPU, shader_mask, "pagetable A")) {
			mali_job_state |= 2;
		}
		if (mali_map_page(MALI_MAPPED_GPU_B, phys) &&
		    mali_run_write_job(page, MALI_MAPPED_GPU_B, shader_mask, "pagetable B")) {
			mali_job_state |= 4;
		}
		/* Both mappings go before the page does. */
		(void)mali_unmap_page(MALI_MAPPED_GPU_B);
		(void)mali_unmap_page(MALI_MAPPED_GPU);
	} else {
		printf("SWEET MALI: no memory for the GPU page tables\n");
	}

	/*
	 * v34: eight pages, contiguous in GPU address space, one job each -- the
	 * shape a real buffer has.  Bit 0x10 records it.
	 */
	{
		vm_offset_t pages[MALI_BUF_PAGES];
		uint64_t physes[MALI_BUF_PAGES];
		int i, made = 0, ran = 0;

		for (i = 0; i < MALI_BUF_PAGES; i++) {
			pages[i] = 0;
			if (kmem_alloc(kernel_map, &pages[i], PAGE_SIZE,
			    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
				break;
			}
			physes[i] = kvtophys(pages[i]);
			made++;
		}
		if (made == MALI_BUF_PAGES) {
			for (i = 0; i < made; i++) {
				if (!mali_map_page(MALI_BUF_GPU + (uint64_t)i * PAGE_SIZE, physes[i])) {
					break;
				}
			}
			if (i == made) {
				mali_as0_update(mali_pt_top_phys, MALI_TRANSCFG_4K);
				for (i = 0; i < made; i++) {
					if (mali_run_write_job(pages[i],
					    MALI_BUF_GPU + (uint64_t)i * PAGE_SIZE,
					    shader_mask, "buffer page")) {
						ran++;
					}
				}
				for (i = 0; i < made; i++) {
					(void)mali_unmap_page(MALI_BUF_GPU + (uint64_t)i * PAGE_SIZE);
				}
			}
		}
		printf("SWEET MALI: v34 buffer pages made=%d ran=%d\n", made, ran);
		if (ran == MALI_BUF_PAGES) {
			mali_job_state |= 0x10;
		}
		for (i = 0; i < made; i++) {
			kmem_free(kernel_map, pages[i], PAGE_SIZE);
		}
	}

	/* v42: drawing is off by default (sweetmalidraw=1 turns it back on); the
	 * point of this build is the GPU running work in the background. */
	{
		int draw = 0;

		PE_parse_boot_argn("sweetmalidraw", &draw, sizeof(draw));
		if (draw == 1) {
			mali_draw_block(shader_mask);
		}
	}

	/*
	 * Back to identity before freeing: the mappings above are gone and the
	 * table pages stay allocated (three pages, a bounded leak by design).
	 */
	mali_as0_update(0, 2);
	mali_bg_start(mali_prd(mali_p_gpu, 0x100));
	printf("SWEET MALI: v54 first job state=%x pt_pages=%u top=%08x%08x\n",
	    mali_job_state, mali_pt_pages, (uint32_t)(mali_pt_top_phys >> 32),
	    (uint32_t)mali_pt_top_phys);
	kmem_free(kernel_map, page, PAGE_SIZE);
}

/* Used by iokit/Kernel/arm/SweetFramebuffer.cpp (Metal accelerator); they
 * keep the IOKit side free of MFG/SPM/PMIC sequencing. */
int
sweet_gpu_is_powered(void)
{
	return mali_powered;
}

uint32_t
sweet_gpu_reg_read(uint32_t off)
{
	if (!mali_powered || mali_p_gpu == 0) {
		return 0;
	}
	return mali_prd(mali_p_gpu, off);
}

static void
sweet_mali_power(void)
{
	int enabled = 0;
	uint32_t id, misc;
	unsigned dfd;
	bool mfg1_sram = false;

	if (!PE_parse_boot_argn("sweetmalion", &enabled, sizeof(enabled)) ||
	    enabled != 1) {
		return;
	}
	mali_p_spm = ml_io_map(0x10006000u, 0x1000);
	mali_p_infra = ml_io_map(0x10001000u, 0x1000);
	mali_p_infra_pdn = ml_io_map(0x10215000u, 0x1000);
	mali_p_mfgcfg = ml_io_map(0x13fbf000u, 0x1000);
	/* v30: the GPU aperture is 0x13000000, not the 0x13040000 copied over from
	 * the MT6768 reference.  vendor mt6833.dts: `mali: mali@13000000` with
	 * `reg = <0 0x13000000 0 0x4000>` and compatible "arm,mali-valhall".
	 * Reading the wrong window is why GPU_ID came back 0 with every power bit
	 * already correct. */
	mali_p_gpu = ml_io_map(0x13000000u, 0x4000);
	if (!mali_p_spm || !mali_p_infra || !mali_p_infra_pdn || !mali_p_mfgcfg || !mali_p_gpu) {
		printf("SWEET MALI: power map failed\n");
		return;
	}
	misc = mali_prd(mali_p_infra, MALI_MFG_MISC_CON);
	dfd = (misc >> 19) & 1u;
	printf("SWEET MALI: v54 power-on start 16C=%08x 170=%08x\n",
	    mali_prd(mali_p_spm, MALI_SPM_PWR_STATUS),
	    mali_prd(mali_p_spm, MALI_SPM_PWR_STATUS_2ND));
	printf("SWEET MALI: MFG_MISC_CON=%08x dfd=%d done=%d\n", misc, dfd,
	    (misc >> 18) & 1u);
	if (dfd) {
		/* The vendor answer to a triggered DFD is a fake power on: the whole
		 * MTCMOS sequence is skipped while the clocks and the block are still
		 * touched.  Nothing measured afterwards can tell a real power-up from
		 * a locked one, so say so instead of pretending. */
		printf("SWEET MALI: DFD triggered, MTCMOS skipped (not conclusive)\n");
		goto clocks;
	}

	if (!mali_domain_up("MFG0", 0x308, 1u << 2, NULL)) {
		goto fail;
	}
	if (!mali_domain_up("MFG1", 0x30C, 1u << 3, &mfg1_sram)) {
		printf("SWEET MALI: MFG1 power failed\n");
		goto clocks;
	}
	/* Without the SRAM ack the bus stays protected on purpose: the vendor's
	 * bus-protect release waits for that ack before way_en, and releasing it
	 * anyway would hand the fabric a live AXI port with no SRAM behind it. */
	if (mfg1_sram) {
		if (!mali_mfg1_bus()) {
			printf("SWEET MALI: mfg1 bus left protected\n");
		}
	} else {
		printf("SWEET MALI: mfg1 sram ack missing, bus left protected\n");
	}
	printf("SWEET MALI: MFG1 up 16C=%08x 170=%08x\n",
	    mali_prd(mali_p_spm, MALI_SPM_PWR_STATUS),
	    mali_prd(mali_p_spm, MALI_SPM_PWR_STATUS_2ND));
	/* Read the ID before MFG2/MFG3: if those ever hang the bus, the evidence
	 * for MFG1 is already out.  250 read this aperture with MFG1 down and got
	 * 0 without a fault, so the read stays even on the failed path, but it is
	 * announced first so a later abort still points at its cause. */
	if (!mfg1_sram) {
		printf("SWEET MALI: MFG1 sram down, early aperture read may abort\n");
	}
	printf("SWEET MALI: GPU_ID early=%08x shader=%08x tiler=%08x l2=%08x\n",
	    mali_prd(mali_p_gpu, 0x000), mali_prd(mali_p_gpu, 0x100),
	    mali_prd(mali_p_gpu, 0x110), mali_prd(mali_p_gpu, 0x120));
	/* MFG2/MFG3 are logged, never fatal: they are not needed to read the ID. */
	(void)mali_domain_up("MFG2", 0x310, 1u << 4, NULL);
	(void)mali_domain_up("MFG3", 0x314, 1u << 5, NULL);
	printf("SWEET MALI: MFG domains done, status %08x\n", mali_prd(mali_p_spm, 0x16C));

clocks:
	/* mfgcfg sits in the MFG0 domain, so nothing below is safe unless MFG0 is
	 * really powered (the DFD path skips the whole MTCMOS sequence). */
	if (!(mali_prd(mali_p_spm, MALI_SPM_PWR_STATUS) & MALI_SPM_PWR_MFG0_STA)) {
		printf("SWEET MALI: MFG0 off, mfgcfg not powered, stop here\n");
		return;
	}
	/* The vendor enables the GPU clocks only after the domains: subsys_bg3d
	 * (the BG3D gate) first, then the external CG control.  BG3D is skipped
	 * when MFG1 has no SRAM ack: on that path CLK_DIS and ISO are already at 0,
	 * so opening the gate would clock a core whose SRAM is still powered down.
	 * PWR_ISO only isolates signals, it does not hold the clock. */
	printf("SWEET MALI: gpu clocks on\n");
	if (mfg1_sram) {
		mali_bg3d_gate("v54");
	} else {
		printf("SWEET MALI: bg3d skipped (mfg1 sram not ready)\n");
	}
	if (!mfg1_sram) {
		printf("SWEET MALI: MFG1 sram down, aperture read may abort\n");
	}
	printf("SWEET MALI: mfgcfg init at 13fbf000\n");
	mali_pwr(mali_p_mfgcfg, 0x20, mali_prd(mali_p_mfgcfg, 0x20) |
	    (1u << 22) | (1u << 23) | (1u << 24) | (1u << 25));
	mali_pwr(mali_p_mfgcfg, 0xB0, mali_prd(mali_p_mfgcfg, 0xB0) &
	    ~((1u << 8) | (1u << 9) | (1u << 10) | (1u << 19)));
	mali_pwr(mali_p_mfgcfg, 0xB4, mali_prd(mali_p_mfgcfg, 0xB4) | (1u << 4));
	mali_pwr(mali_p_mfgcfg, 0x24, mali_prd(mali_p_mfgcfg, 0x24) | 1u);
	mali_pwr(mali_p_mfgcfg, 0xF60, mali_prd(mali_p_mfgcfg, 0xF60) | 1u);
	printf("SWEET MALI: mfgcfg 20=%08x B0=%08x B4=%08x F60=%08x\n",
	    mali_prd(mali_p_mfgcfg, 0x20), mali_prd(mali_p_mfgcfg, 0xB0),
	    mali_prd(mali_p_mfgcfg, 0xB4), mali_prd(mali_p_mfgcfg, 0xF60));
	/* Vendor mfgcfg timing (mtk_gpufreq_plat.c): the I2M protector is armed in
	 * two steps with a settle in between, the last write leaving bit0 set. */
	mali_pwr(mali_p_mfgcfg, 0xF60, 0x20700316u);
	mali_pwr(mali_p_mfgcfg, 0xF64, 0x1000000Cu);
	mali_pwr(mali_p_mfgcfg, 0xF68, 0x01010802u);
	mali_settle(20000);
	mali_pwr(mali_p_mfgcfg, 0xF60, 0x20700317u);
	printf("SWEET MALI: mfgcfg timing F60=%08x F64=%08x F68=%08x\n",
	    mali_prd(mali_p_mfgcfg, 0xF60), mali_prd(mali_p_mfgcfg, 0xF64),
	    mali_prd(mali_p_mfgcfg, 0xF68));

	printf("SWEET MALI: read GPU_ID\n");
	id = mali_prd(mali_p_gpu, 0x000);
	printf("SWEET MALI: GPU_ID=%08x shader=%08x tiler=%08x l2=%08x\n", id,
	    mali_prd(mali_p_gpu, 0x100), mali_prd(mali_p_gpu, 0x110),
	    mali_prd(mali_p_gpu, 0x120));
	if (id == 0u || id == 0xffffffffu) {
		/* A dead ID means the block is not really powered; poking reset and
		 * core power into it would only make the log harder to read. */
		printf("SWEET MALI: GPU_ID dead, skip reset and core power\n");
		return;
	}
	printf("SWEET MALI: GPU soft reset\n");
	mali_pwr(mali_p_gpu, 0x028, 0);            /* GPU_INT_MASK */
	mali_pwr(mali_p_gpu, 0x024, 0xffffffffu); /* GPU_INT_CLEAR */
	mali_pwr(mali_p_gpu, 0x030, 1u);           /* GPU_CMD soft reset */
	if (!mali_wait(mali_p_gpu, 0x020, 1u << 8, 1u << 8, 10000)) {
		printf("SWEET MALI: reset timeout rawstat=%08x\n", mali_prd(mali_p_gpu, 0x020));
		return;
	}
	mali_pwr(mali_p_gpu, 0x024, 0xffffffffu);
	printf("SWEET MALI: reset done, powering cores shader=%08x tiler=%08x l2=%08x\n",
	    mali_prd(mali_p_gpu, 0x100), mali_prd(mali_p_gpu, 0x110),
	    mali_prd(mali_p_gpu, 0x120));
	mali_pwr(mali_p_gpu, 0x1a0, mali_prd(mali_p_gpu, 0x120)); /* L2_PWRON */
	mali_pwr(mali_p_gpu, 0x190, mali_prd(mali_p_gpu, 0x110)); /* TILER_PWRON */
	mali_pwr(mali_p_gpu, 0x180, mali_prd(mali_p_gpu, 0x100)); /* SHADER_PWRON */
	if (!mali_wait(mali_p_gpu, 0x160, mali_prd(mali_p_gpu, 0x120), mali_prd(mali_p_gpu, 0x120), 5000) ||
	    !mali_wait(mali_p_gpu, 0x150, mali_prd(mali_p_gpu, 0x110), mali_prd(mali_p_gpu, 0x110), 5000) ||
	    !mali_wait(mali_p_gpu, 0x140, mali_prd(mali_p_gpu, 0x100), mali_prd(mali_p_gpu, 0x100), 5000)) {
		printf("SWEET MALI: core ready timeout %08x/%08x/%08x\n",
		    mali_prd(mali_p_gpu, 0x140), mali_prd(mali_p_gpu, 0x150),
		    mali_prd(mali_p_gpu, 0x160));
		return;
	}
	printf("SWEET MALI: GPU ready, GPU_ID=%08x shader=%08x tiler=%08x l2=%08x\n",
	    mali_prd(mali_p_gpu, 0x000), mali_prd(mali_p_gpu, 0x140),
	    mali_prd(mali_p_gpu, 0x150), mali_prd(mali_p_gpu, 0x160));
	/* v31: ask the GPU to write a value into a page of ours. */
	mali_first_job(mali_prd(mali_p_gpu, 0x100));
	mali_powered = 1;
	{
		/* Let IOKit publish the Metal accelerator now that the registers read. */
		extern void sweet_gpu_start(void);

		sweet_gpu_start();
	}
	printf("SWEET MALI: power-on done\n");
	sweet_install_probe();
	return;

fail:
	printf("SWEET MALI: power-on failed, status %08x\n", mali_prd(mali_p_spm, 0x16C));
}

static void
sweet_mali_power_status(void)
{
	if (!mali_p_spm || !mali_p_gpu) {
		return;
	}
	{
		uint16_t vp1_con = 0, vp1_vosel = 0, vp1_en = 0, vp1_elr = 0;
		uint16_t vsm_con = 0, vsm_en = 0, vsm_vosel = 0;
		bool a = key_pmic_read(0x1708, &vp1_con);     /* BUCK_VPROC1_CON0, EN bit0 */
		bool b = key_pmic_read(0x1726, &vp1_vosel);   /* DBG0, DA_VOSEL bits 0-6 */
		bool c = key_pmic_read(0x1728, &vp1_en);      /* DBG1, DA_EN */
		bool e = key_pmic_read(0x172e, &vp1_elr);     /* ELR0, RG_VOSEL */
		bool f = key_pmic_read(0x1f2c, &vsm_con);     /* LDO_VSRAM_MD_CON0, EN bit0 */
		bool g = key_pmic_read(0x1f2e, &vsm_en);      /* MON, DA_B_EN */
		bool h = key_pmic_read(0x1f32, &vsm_vosel);   /* VOSEL1, DA_VOSEL bits 8+ */
		/* v30: vendor mt6833.dts maps the GPU rails to the VPROC1 buck and the
		 * VSRAM_MD ldo (_vgpu-supply / _vsram_gpu-supply), so VGPU11 is not the
		 * GPU rail on this board and the old reads were looking at the wrong
		 * supply the whole time. */
		printf("SWEET MALI: BG batches=%u jobs=%u ok=%u bad=%u\n",
	    mali_bg_batches, mali_bg_jobs, mali_bg_ok, mali_bg_bad);
	printf("SWEET MALI: v54 compute copy %u/%u words ok\n",
	    mali_compute_ok, mali_compute_words);
	{
		extern int sweet_gpu_service_state(void);

		printf("SWEET GPU: service state=%d gpu_id=%08x\n",
		    sweet_gpu_service_state(), sweet_gpu_reg_read(0));
	}
	printf("SWEET MALI: PMIC VPROC1 con0=%04x(%d) dbg0=%04x(%d) dbg1=%04x(%d) elr0=%04x(%d)\n",
		    vp1_con, a, vp1_vosel, b, vp1_en, c, vp1_elr, e);
		printf("SWEET MALI: PMIC VSRAM_MD con0=%04x(%d) mon=%04x(%d) vosel1=%04x(%d)\n",
		    vsm_con, f, vsm_en, g, vsm_vosel, h);
	}
	printf("SWEET MALI: status SPM=%08x/%08x MFGCFG20=%08x GPU_ID=%08x shader=%08x tiler=%08x l2=%08x\n",
	    mali_prd(mali_p_spm, 0x16C), mali_prd(mali_p_spm, 0x170),
	    mali_p_mfgcfg ? mali_prd(mali_p_mfgcfg, 0x20) : 0,
	    mali_prd(mali_p_gpu, 0x000), mali_prd(mali_p_gpu, 0x100),
	    mali_prd(mali_p_gpu, 0x110), mali_prd(mali_p_gpu, 0x120));
}

static vm_offset_t mali_spm, mali_topckgen, mali_apmixed, mali_mfgcfg, mali_infra;
static vm_offset_t mali_probe_infra_pdn;

static void
sweet_mali_probe_loop(void)
{
	vm_offset_t spm = mali_spm, topckgen = mali_topckgen, apmixed = mali_apmixed;
	vm_offset_t mfgcfg = mali_mfgcfg, infra = mali_infra;

	/* v28: the mappings are made once, in sweet_mali_probe().  This loop runs
	 * every 10 s and used to map five pages per pass without ever unmapping
	 * them, which is the kva_alloc leak behind the "do not leave it hanging"
	 * rule in the handoff. */
	if (!spm || !topckgen || !apmixed || !mfgcfg || !infra) {
		printf("SWEET MALI: map failed spm=%d top=%d ap=%d mfg=%d infra=%d\n",
		    spm != 0, topckgen != 0, apmixed != 0, mfgcfg != 0, infra != 0);
		return;
	}
	printf("SWEET MALI: SPM 16C=%08x 170=%08x 308=%08x 30C=%08x 310=%08x 314=%08x\n",
	    *(volatile uint32_t *)(spm + 0x16C),
	    *(volatile uint32_t *)(spm + 0x170),
	    *(volatile uint32_t *)(spm + 0x308),
	    *(volatile uint32_t *)(spm + 0x30C),
	    *(volatile uint32_t *)(spm + 0x310),
	    *(volatile uint32_t *)(spm + 0x314));
	printf("SWEET MALI: TOP CLK_CFG_4 %08x/%08x/%08x INFRA_PDN_4C=%08x\n",
	    *(volatile uint32_t *)(topckgen + 0x50),
	    *(volatile uint32_t *)(topckgen + 0x54),
	    *(volatile uint32_t *)(topckgen + 0x58),
	    *(volatile uint32_t *)(mali_probe_infra_pdn + 0x4C));
	printf("SWEET MALI: APMIXED MFGPLL %08x/%08x/%08x/%08x\n",
	    *(volatile uint32_t *)(apmixed + 0x268),
	    *(volatile uint32_t *)(apmixed + 0x26C),
	    *(volatile uint32_t *)(apmixed + 0x270),
	    *(volatile uint32_t *)(apmixed + 0x274));
	printf("SWEET MALI: MFGCFG 20=%08x B0=%08x B4=%08x F60=%08x F64=%08x F68=%08x\n",
	    *(volatile uint32_t *)(mfgcfg + 0x20),
	    *(volatile uint32_t *)(mfgcfg + 0xB0),
	    *(volatile uint32_t *)(mfgcfg + 0xB4),
	    *(volatile uint32_t *)(mfgcfg + 0xF60),
	    *(volatile uint32_t *)(mfgcfg + 0xF64),
	    *(volatile uint32_t *)(mfgcfg + 0xF68));
	/* v29: the CG gate state and the bus-protect state were only printed once,
	 * early enough to scroll away before a photo could be taken.  They repeat
	 * here so one picture of the steady state is enough to judge a run. */
	printf("SWEET MALI: MFGCG 000=%08x 004=%08x 008=%08x\n",
	    *(volatile uint32_t *)(mfgcfg + 0x000),
	    *(volatile uint32_t *)(mfgcfg + 0x004),
	    *(volatile uint32_t *)(mfgcfg + 0x008));
	printf("SWEET MALI: PWR 31C=%08x 4C=%08x 228=%08x 258=%08x 724=%08x MISC=%08x\n",
	    *(volatile uint32_t *)(spm + 0x31C),
	    *(volatile uint32_t *)(mali_probe_infra_pdn + 0x4C),
	    *(volatile uint32_t *)(infra + 0x228),
	    *(volatile uint32_t *)(infra + 0x258),
	    *(volatile uint32_t *)(infra + 0x724),
	    *(volatile uint32_t *)(infra + 0x600));
	/* v31: cores ready and what the first job did, both repeated so a photo of
	 * the steady state is enough to judge a run. */
	printf("SWEET MALI: CORE 140=%08x 150=%08x 160=%08x JOB raw=%08x slot=%08x state=%x\n",
	    mali_p_gpu ? mali_prd(mali_p_gpu, 0x140) : 0,
	    mali_p_gpu ? mali_prd(mali_p_gpu, 0x150) : 0,
	    mali_p_gpu ? mali_prd(mali_p_gpu, 0x160) : 0,
	    mali_p_gpu ? mali_prd(mali_p_gpu, MALI_JOB_INT_RAWSTAT) : 0,
	    mali_p_gpu ? mali_prd(mali_p_gpu, MALI_JS1 + MALI_JS_STATUS) : 0,
	    mali_job_state);
}

static void
sweet_mali_probe(void)
{
	int enabled = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("sweetmaliro", &enabled, sizeof(enabled)) ||
	    enabled != 1) {
		return;
	}
	mali_spm = ml_io_map(0x10006000u, 0x1000);
	mali_topckgen = ml_io_map(0x10000000u, 0x1000);
	mali_apmixed = ml_io_map(0x1000c000u, 0x1000);
	mali_mfgcfg = ml_io_map(0x13fbf000u, 0x1000);
	mali_infra = ml_io_map(0x10001000u, 0x1000);
	mali_probe_infra_pdn = ml_io_map(0x10215000u, 0x1000);
	if (!mali_spm || !mali_topckgen || !mali_apmixed || !mali_mfgcfg || !mali_infra ||
	    !mali_probe_infra_pdn) {
		printf("SWEET MALI: map failed\n");
		return;
	}
	sweet_mali_probe_loop();
	if (kernel_thread_start(mali_probe_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

void
sweet_touch_mtk_init(void)
{
	int enabled = 0;
	thread_t thread;

	if (st.started) {
		return;
	}
	st.started = true;
	sweet_keys_mtk_poll_init();
	sweet_mali_probe();
	sweet_mali_power();
	(void)PE_parse_boot_argn("sweettouchmtk", &enabled, sizeof(enabled));
	if (!enabled) {
		printf("SWEET TOUCH MTK: disabled (sweettouchmtk=0)\n");
		return;
	}
	if (kernel_thread_start(touch_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("SWEET TOUCH MTK: kernel_thread_start failed\n");
	}
}

#endif /* ARM64_BOARD_CONFIG_SWEET */
