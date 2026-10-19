/*
 * Linux userspace front end for the hardware library, for milestone 2:
 * drive the real card through sysfs with no kernel driver bound.
 *
 *   rdn_tool [options] status      report POST state (read-only)
 *   rdn_tool [options] post        bring the card up (ASIC_Init if needed)
 *   rdn_tool [options] vramtest    write/read patterns through the aperture
 *   rdn_tool [options] modeset     read the EDID from the DVI connector, set
 *                                  its preferred mode and show a test pattern
 *   rdn_tool [options] accel       start the 3D engine and the command
 *                                  processor (after post and modeset), run
 *                                  the ring test and one fence; -f names the
 *                                  directory with the microcode; -R uses
 *                                  the command byte order that is not the
 *                                  host's
 *   rdn_tool [options] grab [file]  save the scanout surface as a PPM image
 *   rdn_tool [options] peek REG...  read registers (hex offsets)
 *   rdn_tool [options] edid [file] probe every DDC line for an EDID; save
 *                                  the first one found to file
 *
 * Options:
 *   -s <addr>    PCI address (default 0000:10:00.0)
 *   -b <file>    take the VBIOS from a file instead of the expansion ROM
 *   -t <file>    log every register access, in the format of
 *                scripts/trace-split.py, for comparison with a reference
 *   -d           DVI signalling even if the display's EDID says HDMI
 *   -n           no I/O BAR: route AtomBIOS indirect I/O through MMIO
 *
 * Must run as root. Refuses to run while a kernel driver owns the card
 * (use scripts/card-bind.sh none).
 *
 * Copyright (c) 2026 kouta-kun and Claude
 * SPDX-License-Identifier: MIT
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../hw/rdn_accel.h"
#include "../hw/rdn_card.h"
#include "../hw/rdn_i2c.h"
#include "../hw/rdn_mode.h"
#include "../hw/rdn_pattern.h"
#include "../hw/rdn_reg.h"
#include "../hw/rdn_uvd.h"
#include "../hw/rdn_mc.h"
#include "../hw/rdn_pm.h"
#include "../hw/rdn_gart.h"
#include "../hw/rdn_uvd_reg.h"

#define MMIO_SIZE	0x20000
#define ROM_MAX		0x20000

#define PCI_COMMAND		0x04
#define PCI_COMMAND_IO		0x1
#define PCI_COMMAND_MEMORY	0x2

struct linux_card {
	char sysfs[128];
	int cfg_fd;
	int io_fd;
	volatile uint8_t *mmio;
	FILE *trace;
	uint16_t saved_command;
};

/* The host may be either endianness; the card is always little-endian. */
static uint32_t le32_load(const volatile uint8_t *p)
{
	uint32_t v;

	memcpy(&v, (const void *)p, 4);
	return rdn_swap_le32(v);
}

static void le32_store(volatile uint8_t *p, uint32_t v)
{
	*(volatile uint32_t *)p = rdn_swap_le32(v);
}

static uint32_t os_mmio_read32(void *c, uint32_t off)
{
	struct linux_card *lc = c;
	uint32_t v = rdn_swap_le32(*(volatile uint32_t *)(lc->mmio + off));

	if (lc->trace)
		fprintf(lc->trace, "R mmio %05x %08x 4\n", off, v);
	return v;
}

static void os_mmio_write32(void *c, uint32_t off, uint32_t v)
{
	struct linux_card *lc = c;

	if (lc->trace)
		fprintf(lc->trace, "W mmio %05x %08x 4\n", off, v);
	le32_store(lc->mmio + off, v);
}

static uint32_t os_io_read32(void *c, uint32_t off)
{
	struct linux_card *lc = c;
	uint8_t b[4];
	uint32_t v;

	if (pread(lc->io_fd, b, 4, off) != 4) {
		perror("I/O BAR read");
		exit(1);
	}
	v = le32_load(b);
	if (lc->trace)
		fprintf(lc->trace, "R io %05x %08x 4\n", off, v);
	return v;
}

static void os_io_write32(void *c, uint32_t off, uint32_t v)
{
	struct linux_card *lc = c;
	uint32_t le = rdn_swap_le32(v);

	if (lc->trace)
		fprintf(lc->trace, "W io %05x %08x 4\n", off, v);
	if (pwrite(lc->io_fd, &le, 4, off) != 4) {
		perror("I/O BAR write");
		exit(1);
	}
}

static uint32_t os_cfg_read32(void *c, uint32_t off)
{
	struct linux_card *lc = c;
	uint8_t b[4];

	if (pread(lc->cfg_fd, b, 4, off) != 4)
		return 0xffffffff;
	return le32_load(b);
}

static void os_cfg_write32(void *c, uint32_t off, uint32_t v)
{
	struct linux_card *lc = c;
	uint32_t le = rdn_swap_le32(v);

	if (pwrite(lc->cfg_fd, &le, 4, off) != 4)
		perror("config write");
}

static void os_delay_us(void *c, uint32_t usec)
{
	struct timespec ts;

	(void)c;
	ts.tv_sec = usec / 1000000;
	ts.tv_nsec = (long)(usec % 1000000) * 1000;
	nanosleep(&ts, NULL);
}

static uint64_t os_time_ms(void *c)
{
	struct timespec ts;

	(void)c;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void *os_alloc(void *c, size_t size)
{
	(void)c;
	return calloc(1, size);
}

static void os_free(void *c, void *ptr)
{
	(void)c;
	free(ptr);
}

static void os_log(void *c, enum rdn_log_level level, const char *fmt,
		   va_list ap)
{
	static const char *const tag[] = { "error", "info", "debug" };

	(void)c;
	if (level == RDN_LOG_DEBUG)
		return;
	fprintf(stderr, "rdn %s: ", tag[level]);
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
}

static int sysfs_open(struct linux_card *lc, const char *name, int flags)
{
	char path[192];
	int fd;

	snprintf(path, sizeof(path), "%s/%s", lc->sysfs, name);
	fd = open(path, flags);
	if (fd < 0)
		fprintf(stderr, "%s: %s\n", path, strerror(errno));
	return fd;
}

static int check_identity(struct linux_card *lc)
{
	char path[192], buf[64];
	uint32_t id;
	ssize_t n;

	snprintf(path, sizeof(path), "%s/driver", lc->sysfs);
	n = readlink(path, buf, sizeof(buf) - 1);
	if (n > 0 && getenv("RDN_ALLOW_DRIVER")) {
		fprintf(stderr, "warning: a kernel driver owns the card; going on (RDN_ALLOW_DRIVER)\n");
		n = 0;
	}
	if (n > 0) {
		buf[n] = 0;
		fprintf(stderr, "a kernel driver owns the card (%s); "
			"run scripts/card-bind.sh none first\n",
			strrchr(buf, '/') ? strrchr(buf, '/') + 1 : buf);
		return -1;
	}
	id = os_cfg_read32(lc, 0);
	if (id != 0x675d1002) {
		fprintf(stderr, "unexpected device %04x:%04x, refusing\n",
			id & 0xffff, id >> 16);
		return -1;
	}
	return 0;
}

static void *read_rom(struct linux_card *lc, const char *file)
{
	uint8_t *buf = calloc(1, ROM_MAX);
	ssize_t n;
	int fd;

	if (file) {
		fd = open(file, O_RDONLY);
		if (fd < 0) {
			perror(file);
			return NULL;
		}
	} else {
		/* Writing 1 only turns on ROM address decoding; nothing is flashed. */
		fd = sysfs_open(lc, "rom", O_RDWR);
		if (fd < 0)
			return NULL;
		if (pwrite(fd, "1", 1, 0) != 1) {
			perror("enable ROM decoding");
			return NULL;
		}
	}
	/* sysfs hands out at most a page per read. */
	n = 0;
	while (n < ROM_MAX) {
		ssize_t r = pread(fd, buf + n, ROM_MAX - n, n);

		if (r <= 0)
			break;
		n += r;
	}
	if (!file && pwrite(fd, "0", 1, 0) != 1)
		perror("disable ROM decoding");
	close(fd);
	if (n < 0x200) {
		fprintf(stderr, "VBIOS read returned %zd bytes\n", n);
		return NULL;
	}
	/* The image states its own length; a short read must not pass. */
	if (buf[0] != 0x55 || buf[1] != 0xaa || n < (ssize_t)buf[2] * 512) {
		fprintf(stderr, "VBIOS image is invalid or truncated (%zd bytes)\n", n);
		return NULL;
	}
	fprintf(stderr, "VBIOS: %zd bytes from %s\n", n,
		file ? file : "the PCI expansion ROM");
	return buf;
}

static void print_status(struct rdn_card *card)
{
	static const uint32_t crtc[6] = {
		0x6e70, 0x7a70, 0x10670, 0x11270, 0x11e70, 0x12a70
	};
	int i;

	printf("CRTC_CONTROL   ");
	for (i = 0; i < 6; i++)
		printf(" %08x", rdn_rreg(card, crtc[i]));
	/* On these chips the register holds megabytes. */
	printf("\nCONFIG_MEMSIZE  %08x (%u MB)\n", rdn_rreg(card, CONFIG_MEMSIZE),
	       rdn_rreg(card, CONFIG_MEMSIZE));
	printf("MC_SEQ_SUP_CNTL %08x\n", rdn_rreg(card, MC_SEQ_SUP_CNTL));
	printf("MC_SEQ_MISC0    %08x\n", rdn_rreg(card, MC_SEQ_MISC0));
	printf("state: %s\n", rdn_card_posted(card) ? "posted" : "NOT posted");
}

/*
 * Write a distinct word at many places across the aperture, then read them
 * all back. Memory that is not there, or not trained, fails this.
 */
static int vram_test(struct linux_card *lc)
{
	struct stat st;
	volatile uint32_t *fb;
	size_t size, words, step, i, bad = 0, tested = 0;
	int fd = sysfs_open(lc, "resource0_wc", O_RDWR);

	if (fd < 0)
		fd = sysfs_open(lc, "resource0", O_RDWR);
	if (fd < 0 || fstat(fd, &st))
		return -1;
	size = st.st_size;
	fb = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (fb == MAP_FAILED) {
		perror("mmap aperture");
		return -1;
	}
	words = size / 4;
	step = 4099;	/* prime, so every address bit takes part */

	for (i = 0; i < words; i += step)
		fb[i] = rdn_swap_le32((uint32_t)i * 2654435761u ^ 0xa5a5a5a5);
	for (i = 0; i < words; i += step) {
		uint32_t want = (uint32_t)i * 2654435761u ^ 0xa5a5a5a5;
		uint32_t got = rdn_swap_le32(fb[i]);

		tested++;
		if (got != want && bad++ < 8)
			printf("  offset %08zx: wrote %08x read %08x\n",
			       i * 4, want, got);
	}
	/* A second pass with the complement catches stuck bits. */
	for (i = 0; i < words; i += step)
		fb[i] = rdn_swap_le32(~((uint32_t)i * 2654435761u ^ 0xa5a5a5a5));
	for (i = 0; i < words; i += step) {
		uint32_t want = ~((uint32_t)i * 2654435761u ^ 0xa5a5a5a5);

		if (rdn_swap_le32(fb[i]) != want)
			bad++;
	}
	printf("aperture %zu MB, %zu words tested twice, %zu mismatches\n",
	       size >> 20, tested, bad);
	munmap((void *)fb, size);
	close(fd);
	return bad ? 1 : 0;
}

static int force_dvi;

static int do_modeset(struct linux_card *lc, struct rdn_card *card)
{
	uint8_t edid[RDN_EDID_MAX_SIZE];
	struct rdn_mode mode;
	struct rdn_fb fb;
	volatile uint32_t *pix;
	struct stat st;
	bool hdmi;
	int len, fd, r;

	if (!rdn_card_posted(card)) {
		fprintf(stderr, "card is not posted; run 'post' first\n");
		return -1;
	}

	/* The first connector with a display: DVI-I, then DisplayPort. */
	len = rdn_output_detect(card, edid);
	if (len < 0 || !rdn_edid_preferred_mode(edid, &mode)) {
		fprintf(stderr, "no EDID on any connector (%d)\n", len);
		return -1;
	}
	hdmi = rdn_edid_is_hdmi(edid, len) && !force_dvi &&
		!card->output->displayport;
	printf("%s, EDID: %d bytes, preferred %ux%u at %u kHz, %s\n",
	       card->output->name, len,
	       mode.hdisplay, mode.vdisplay, (unsigned)mode.clock,
	       card->output->displayport ? "DisplayPort" : hdmi ? "HDMI" : "DVI");

	memset(&fb, 0, sizeof(fb));
	fb.width = mode.hdisplay;
	fb.height = mode.vdisplay;
	fb.pitch_pixels = (mode.hdisplay + 63u) & ~63u;
	/* The pattern is written as native words. */
	fb.big_endian_pixels = RDN_BIG_ENDIAN;

	fd = sysfs_open(lc, "resource0_wc", O_RDWR);
	if (fd < 0)
		fd = sysfs_open(lc, "resource0", O_RDWR);
	if (fd < 0 || fstat(fd, &st))
		return -1;
	pix = mmap(NULL, st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (pix == MAP_FAILED) {
		perror("mmap aperture");
		return -1;
	}
	rdn_pattern_draw(pix, fb.width, fb.height, fb.pitch_pixels);

	r = rdn_display_init(card);
	if (r) {
		fprintf(stderr, "display init failed (%d)\n", r);
		return r;
	}
	r = rdn_modeset(card, &mode, &fb, hdmi);
	printf("modeset returned %d\n", r);
	munmap((void *)pix, st.st_size);
	close(fd);
	return r;
}

/*
 * Acceleration. Layout of the aperture while the tool runs: the scanout
 * surface at 0 (as modeset leaves it), the ring at 32 MB, scratch space for
 * indirect buffers, shaders and vertices at 34 MB.
 */
#define ACCEL_RING_OFFSET	(32u << 20)
#define ACCEL_RING_BYTES	(1u << 20)
#define ACCEL_WORK_OFFSET	(34u << 20)

static const char *fw_dir = "firmware";
/* -S: the self-test submits its commands big-endian with the swap flag. */
/* -R: command words in the byte order that is not the host's. */
static int other_order;

static uint8_t *read_fw(const char *name, size_t *size)
{
	char path[512];
	uint8_t *buf = malloc(1 << 16);
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", fw_dir, name);
	f = fopen(path, "rb");
	if (!f || !buf) {
		perror(path);
		return NULL;
	}
	*size = fread(buf, 1, 1 << 16, f);
	fclose(f);
	return buf;
}

static int do_accel(struct linux_card *lc, struct rdn_card *card)
{
	struct rdn_accel_fw fw;
	struct rdn_accel accel;
	volatile void *aperture;
	struct stat st;
	int fd, r;

	if (!rdn_card_posted(card)) {
		fprintf(stderr, "card is not posted; run 'post' and 'modeset' first\n");
		return -1;
	}
	fw.pfp = read_fw("TURKS_pfp.bin", &fw.pfp_size);
	fw.me = read_fw("TURKS_me.bin", &fw.me_size);
	if (!fw.pfp || !fw.me)
		return -1;

	/* Not the write-combining mapping: the ring is read back. */
	fd = sysfs_open(lc, "resource0", O_RDWR);
	if (fd < 0 || fstat(fd, &st))
		return -1;
	aperture = mmap(NULL, st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (aperture == MAP_FAILED) {
		perror("mmap aperture");
		return -1;
	}

	r = rdn_accel_init(&accel, card, aperture, (uint32_t)st.st_size, &fw,
			   ACCEL_RING_OFFSET, ACCEL_RING_BYTES,
			   RDN_BIG_ENDIAN ? !other_order : other_order);
	printf("accel init returned %d; tile_config 0x%x, backend_map 0x%x, "
	       "GRBM_STATUS %08x, CP_STAT %08x\n", r,
	       (unsigned)accel.cfg.tile_config, (unsigned)accel.cfg.backend_map,
	       (unsigned)rdn_rreg(card, 0x8010), (unsigned)rdn_rreg(card, 0x8680));
	if (!r) {
		uint32_t seq;

		r = rdn_fence_emit(&accel, &seq);
		if (!r)
			r = rdn_fence_wait(&accel, seq, 1000);
		printf("fence %u: %d\n", (unsigned)seq, r);
	}
	if (!r) {
		/* Draw on whatever CRTC 0 is scanning out, at aperture offset 0. */
		static const struct { uint32_t x, y; const char *what; } probe[] = {
			{ 128, 128, "square, red" }, { 256, 128, "square, green" },
			{ 128, 256, "square, blue" }, { 256, 256, "square, white" },
			{ 760, 128, "triangle, red" }, { 776, 128, "triangle, green" },
			{ 700, 300, "triangle, blue" }, { 840, 300, "triangle, white" },
			{ 660, 100, "outside the triangle" },
		};
		struct rdn_selftest_target t;
		volatile uint32_t *pix = aperture;
		unsigned i;

		memset(&t, 0, sizeof(t));
		t.gpu_addr = rdn_vram_addr(&accel, 0);
		t.width = rdn_rreg(card, EVERGREEN_GRPH_X_END);
		t.height = rdn_rreg(card, EVERGREEN_GRPH_Y_END);
		t.pitch_pixels = rdn_rreg(card, EVERGREEN_GRPH_PITCH);
		t.big_endian_pixels = RDN_BIG_ENDIAN;
		printf("drawing on %ux%u, pitch %u\n", (unsigned)t.width,
		       (unsigned)t.height, (unsigned)t.pitch_pixels);
		r = rdn_accel_selftest(&accel, &t, ACCEL_WORK_OFFSET);
		printf("selftest returned %d; GRBM_STATUS %08x\n", r,
		       (unsigned)rdn_rreg(card, 0x8010));
		for (i = 0; i < sizeof(probe) / sizeof(probe[0]); i++)
			printf("  pixel (%u,%u) = %08x  (%s)\n", (unsigned)probe[i].x,
			       (unsigned)probe[i].y,
			       (unsigned)pix[probe[i].y * t.pitch_pixels + probe[i].x],
			       probe[i].what);
	}

	munmap((void *)aperture, st.st_size);
	close(fd);
	return r;
}

/*
 * UVD: the engine as the kext starts it (3D engine first), then rdn_uvd_init() with the firmware
 * (SUMO_uvd.bin in the firmware directory), a ring test, and one indirect buffer that writes
 * UVD_CONTEXT_ID. `flags` are RDN_UVD_F_*. Needs a posted card.
 */
static int replay_fw(struct linux_card *lc)
{
	/* the firmware where radeon has it (VRAM offset RDN_FW_OFF), the rest of its window zero */
	static uint8_t uvd_fw[300000];
	struct stat st;
	char path[512];
	uint32_t fsz, bytes, i;
	unsigned long foff = strtoul(getenv("RDN_FW_OFF") ? getenv("RDN_FW_OFF") : "0x40000", NULL, 0);
	volatile uint8_t *ap;
	int fd = sysfs_open(lc, "resource0", O_RDWR);
	FILE *ff;

	snprintf(path, sizeof(path), "%s/SUMO_uvd.bin", fw_dir);
	ff = fopen(path, "rb");
	if (fd < 0 || fstat(fd, &st) || !ff)
		return -1;
	fsz = (uint32_t)fread(uvd_fw, 1, sizeof(uvd_fw), ff);
	fclose(ff);
	ap = mmap(NULL, st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (ap == MAP_FAILED)
		return -1;
	bytes = rdn_uvd_vcpu_bytes(fsz, RDN_UVD_DEFAULT_HANDLES);
	for (i = 0; i < bytes; i++)
		ap[foff + i] = i < fsz ? uvd_fw[i] : 0;
	munmap((void *)ap, st.st_size);
	close(fd);
	printf("firmware (%u bytes, window %u) written at VRAM %lx\n", fsz, bytes, foff);
	return 0;
}

/*
 * Experiment: replay a register trace ("W 0xOFFSET 0xVALUE" / "R 0xOFFSET 0xVALUE" lines, the
 * format of build/uvd-ref/host-all-regs.txt) from line FROM to line TO onto the card, in order, with a
 * short pause after every access and a longer one after clock and reset registers. Reads are
 * done too (they flush posted writes). Journal 2026-10-18.
 */
static int do_replay(struct linux_card *lc, struct rdn_card *card, const char *file, unsigned from,
		     unsigned to)
{
	unsigned fw_line = getenv("RDN_FW_LINE") ? (unsigned)strtoul(getenv("RDN_FW_LINE"), NULL, 0) : 0;
	FILE *f = fopen(file, "r");
	char line[128];
	unsigned n = 0, w = 0;

	if (!f) {
		perror(file);
		return -1;
	}
	while (fgets(line, sizeof(line), f)) {
		char kind;
		unsigned long off, val;

		if (n++ < from)
			continue;
		if (n > to)
			break;
		unsigned long dt = 0;

		kind = line[0];
		if ((fw_line && n - 1 == fw_line) || kind == 'F') {
			if (replay_fw(lc))
				return -1;
			if (kind == 'F')
				continue;
		}
		if (sscanf(line, "%c %lx %lx %lu", &kind, &off, &val, &dt) < 3)
			continue;
		if (dt && getenv("RDN_REPLAY_TIMED"))
			os_delay_us(NULL, (uint32_t)dt);
		if (kind == 'T') {
			/* a 64-bit write into the GART table in VRAM (RDN_GART_AT, default 0x162000) */
			static volatile uint8_t *tap;

			if (!tap) {
				struct stat tst;
				int tfd = sysfs_open(lc, "resource0", O_RDWR);

				if (tfd < 0 || fstat(tfd, &tst))
					return -1;
				tap = mmap(NULL, tst.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, tfd, 0);
				if (tap == MAP_FAILED)
					return -1;
			}
			*(volatile uint64_t *)(tap + 0x162000 + off) = val;
			continue;
		}
		if (kind == 'W') {
			rdn_wreg(card, (uint32_t)off, (uint32_t)val);
			w++;
		} else if (off == 0x718 || off == 0x71c || off == 0x79c || off == 0xf6bc) {
			/* a status the driver waited for: wait for the bits it saw (up to 200 ms) */
			unsigned t;

			for (t = 0; t < 2000 && (rdn_rreg(card, (uint32_t)off) & val) != val; t++)
				os_delay_us(NULL, 100);
		} else {
			uint32_t got = rdn_rreg(card, (uint32_t)off);

			if (getenv("RDN_REPLAY_CHECK") && got != (uint32_t)val)
				printf("MISMATCH line %u reg 0x%05lx expected 0x%08lx got 0x%08x\n", n - 1, off,
				       val & 0xffffffffu, (unsigned)got);
		}
		if (!getenv("RDN_REPLAY_TIMED"))
		os_delay_us(NULL, off == 0xf680 || off == 0xe60 || off == 0xf4f4 || off == 0xf480 || off == 0xf660 ?
			    10000 : (off >= 0x600 && off < 0x900) || off == 0x5480 ? 2000 : 20);
	}
	fclose(f);
	printf("replayed %u writes of lines %u-%u; UVD_STATUS %08x\n", w, from, n,
	       (unsigned)rdn_rreg(card, UVD_STATUS));
	return 0;
}

/* radeon's evergreen_rlc_resume(): UVD only boots with the RLC microcode loaded and running (journal 2026-10-18) */
static void load_rlc(struct rdn_card *card)
{
	size_t rsz;
	uint8_t *rlc = read_fw("BTC_rlc.bin", &rsz);
	static const uint32_t zero_regs[] = { 0x3f0c, 0x3f10, 0x3f20, 0x3f1c, 0x3f14, 0x3f18, 0x3f44, 0x3f48 };
	unsigned k;

	if (rlc && rsz == 768 * 4) {
		rdn_wreg(card, 0x3f00, 0);
		for (k = 0; k < sizeof(zero_regs) / sizeof(zero_regs[0]); k++)
			rdn_wreg(card, zero_regs[k], 0);
		for (k = 0; k < 768; k++) {
			rdn_wreg(card, 0x3f2c, k);
			rdn_wreg(card, 0x3f30, (uint32_t)rlc[k * 4] << 24 | rlc[k * 4 + 1] << 16 |
				 rlc[k * 4 + 2] << 8 | rlc[k * 4 + 3]);
		}
		rdn_wreg(card, 0x3f2c, 0);
		rdn_wreg(card, 0x3f00, 1);
		printf("RLC microcode loaded and started\n");
	}
}

static int do_uvd(struct linux_card *lc, struct rdn_card *card, uint32_t flags)
{
	static uint8_t uvd_fw[300000];
	struct rdn_accel_fw fw;
	struct rdn_accel accel;
	struct rdn_uvd uvd;
	volatile void *aperture;
	struct stat st;
	char path[512];
	uint32_t ref = 0, seq = 0, fw_size;
	FILE *f;
	int fd, r;

	if (!rdn_card_posted(card)) {
		fprintf(stderr, "card is not posted; run 'post' first\n");
		return -1;
	}
	if (getenv("RDN_RLC_FIRST"))
		load_rlc(card);
	/* radeon's order (ni_mc_load_microcode before the rest): the GDDR5 sequencer's microcode first */
	if (!getenv("RDN_NO_MC")) {
		size_t msz;
		uint8_t *mc = read_fw("TURKS_mc.bin", &msz);

		if (mc)
			printf("MC microcode: %d (0 loaded and trained, 1 not needed)\n",
			       rdn_mc_load_microcode(card, mc, msz));
	}
	fw.pfp = read_fw("TURKS_pfp.bin", &fw.pfp_size);
	fw.me = read_fw("TURKS_me.bin", &fw.me_size);
	snprintf(path, sizeof(path), "%s/SUMO_uvd.bin", fw_dir);
	f = fopen(path, "rb");
	if (!fw.pfp || !fw.me || !f) {
		perror(f ? "microcode" : path);
		return -1;
	}
	fw_size = (uint32_t)fread(uvd_fw, 1, sizeof(uvd_fw), f);
	fclose(f);
	fd = sysfs_open(lc, "resource0", O_RDWR);
	if (fd < 0 || fstat(fd, &st))
		return -1;
	aperture = mmap(NULL, st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (aperture == MAP_FAILED) {
		perror("mmap aperture");
		return -1;
	}
	r = rdn_accel_init(&accel, card, aperture, (uint32_t)st.st_size, &fw,
			   ACCEL_RING_OFFSET, ACCEL_RING_BYTES,
			   RDN_BIG_ENDIAN ? !other_order : other_order);
	printf("accel init returned %d\n", r);
	if (r)
		return r;
	if (rdn_uvd_ref_clock(card, &ref)) {
		fprintf(stderr, "no PLL reference clock in the VBIOS\n");
		return -1;
	}
	printf("uvd: firmware %u bytes, reference clock %u0 kHz, flags 0x%x\n", (unsigned)fw_size,
	       (unsigned)ref, (unsigned)flags);
	if (!getenv("RDN_RLC_FIRST"))
		load_rlc(card);
	/*
	 * Experiment: RDN_PM=1 moves the card from its slow boot state to the VBIOS's fastest PowerPlay state
	 * (voltage, engine clock, memory clock), as radeon does before it brings UVD up (journal 2026-10-18).
	 */
	if (getenv("RDN_PM")) {
		struct rdn_pm_state from, to;
		int pr = rdn_pm_boot_state(card, &from);
		uint32_t sc = 0, mc2 = 0;

		if (!pr)
			pr = rdn_pm_performance_state(card, &to);
		if (!pr)
			pr = rdn_pm_set(card, &from, &to, RDN_PM_ALL);
		rdn_pm_get_clocks(card, &sc, &mc2);
		printf("power: boot sclk %u mclk %u -> sclk %u mclk %u vddc %u (result %d), now sclk %u mclk %u\n",
		       (unsigned)from.sclk, (unsigned)from.mclk, (unsigned)to.sclk, (unsigned)to.mclk,
		       (unsigned)to.vddc, pr, (unsigned)sc, (unsigned)mc2);
	}
	/* Experiment: RDN_GART=1 switches the GART on as radeon has it at this point (journal 2026-10-18) */
	if (getenv("RDN_GART")) {
		static struct rdn_gart gart;
		int gr = rdn_gart_enable(&gart, &accel, 48u << 20, 0xa0000);

		printf("gart enable returned %d\n", gr);
	}
	/*
	 * radeon's evergreen_irq_init() writes (journal 2026-10-18): the interrupt handler ring and its
	 * write-back word in VRAM instead of GART, enabled but with nothing routed to it.
	 * RDN_NO_IH=1 skips it.
	 */
	if (!getenv("RDN_NO_IH")) {
		uint32_t ring = 40u << 20, wb = (41u << 20) + 0x800, k;
		uint64_t ring_mc = rdn_vram_addr(&accel, ring), wb_mc = rdn_vram_addr(&accel, wb);

		for (k = 0; k < 16384; k += 4)
			rdn_vram_write32(&accel, ring + k, 0);
		rdn_vram_write32(&accel, wb, 0);
		rdn_hdp_flush(&accel);
		rdn_wreg(card, 0x546c, 0x00ffffe0);
		rdn_wreg(card, 0x5468, rdn_rreg(card, 0x5468) & 0x10 ? 0x10 : 0);
		rdn_wreg(card, 0x3e04, (uint32_t)(ring_mc >> 8));
		rdn_wreg(card, 0x3e14, (uint32_t)wb_mc);
		rdn_wreg(card, 0x3e10, (uint32_t)(wb_mc >> 32) & 0xff);
		rdn_wreg(card, 0x3e00, 0x8001011c);
		rdn_wreg(card, 0x3e08, 0);
		rdn_wreg(card, 0x3e0c, 0);
		rdn_wreg(card, 0x3e18, 0x01080010);
		printf("IH ring set up at MC %llx, write-back %llx\n", (unsigned long long)ring_mc,
		       (unsigned long long)wb_mc);
	}
	/*
	 * Experiment: RDN_REPLAY=file RDN_REPLAY_RANGES=lo-hi[,lo-hi...] (hex byte offsets): write the
	 * "offset value" lines of the file that fall in a range, just before the UVD start. For finding
	 * which of the registers Linux sets that ours does not matter to UVD.
	 */
	if (getenv("RDN_REPLAY")) {
		FILE *rf = fopen(getenv("RDN_REPLAY"), "r");
		const char *rg = getenv("RDN_REPLAY_RANGES");
		char line[128];
		unsigned n = 0;

		while (rf && rg && fgets(line, sizeof(line), rf)) {
			uint32_t off = (uint32_t)strtoul(line, NULL, 0), val;
			char *sp = strchr(line, ' ');
			const char *c = rg;
			int hit = 0;

			if (!sp)
				continue;
			val = (uint32_t)strtoul(sp, NULL, 0);
			while (*c) {
				char *e1, *e2;
				uint32_t lo = (uint32_t)strtoul(c, &e1, 16), hi;

				if (*e1 != '-')
					break;
				hi = (uint32_t)strtoul(e1 + 1, &e2, 16);
				if (off >= lo && off <= hi)
					hit = 1;
				c = *e2 == ',' ? e2 + 1 : e2;
			}
			if (hit) {
				rdn_wreg(card, off, val);
				n++;
			}
		}
		if (rf)
			fclose(rf);
		printf("replayed %u register writes\n", n);
	}
	/*
	 * Experiment: RDN_RELOCATE=file writes Linux's VRAM relocation ("offset value" lines, in order)
	 * and moves the VRAM base to MC address 0 as radeon does.
	 */
	if (getenv("RDN_RELOCATE")) {
		FILE *rf = fopen(getenv("RDN_RELOCATE"), "r");
		char line[128];

		while (rf && fgets(line, sizeof(line), rf)) {
			char *sp = strchr(line, ' ');

			if (sp)
				rdn_wreg(card, (uint32_t)strtoul(line, NULL, 0), (uint32_t)strtoul(sp, NULL, 0));
		}
		if (rf)
			fclose(rf);
		accel.vram_base = 0;
		printf("relocated: FB_LOCATION %08x\n", (unsigned)rdn_rreg(card, 0x2024));
	}
	r = rdn_uvd_init(&uvd, &accel, ref, uvd_fw, fw_size, RDN_UVD_DEFAULT_HANDLES,
			 getenv("RDN_VCPU_OFF") ? (uint32_t)strtoul(getenv("RDN_VCPU_OFF"), NULL, 0) : 36u << 20,
			 (36u << 20) + 0x122000, 4096, flags);
	printf("uvd init returned %d; UVD_STATUS %08x\n", r, (unsigned)rdn_rreg(card, UVD_STATUS));
	if (!r) {
		uint32_t ib = (36u << 20) + 0x140000;

		rdn_vram_write32(&accel, ib, RDN_UVD_PACKET0(UVD_CONTEXT_ID, 0));
		rdn_vram_write32(&accel, ib + 4, 0x600DF00D);
		r = rdn_uvd_wake(&uvd);
		if (!r)
			r = rdn_uvd_ib_submit(&uvd, rdn_vram_addr(&accel, ib), 2, &seq);
		if (!r)
			r = rdn_uvd_fence_wait(&uvd, seq, 1000);
		printf("uvd IB: %d, UVD_CONTEXT_ID %08x\n", r, (unsigned)rdn_rreg(card, UVD_CONTEXT_ID));
		(void)rdn_uvd_idle(&uvd);
		rdn_uvd_fini(&uvd);
	}
	munmap((void *)aperture, st.st_size);
	close(fd);
	return r;
}

/* "offset value" lines of a file written to the card, for the uvdonly bisection (journal 2026-10-18) */
static unsigned apply_file(struct rdn_card *card, const char *file)
{
	FILE *f = file ? fopen(file, "r") : NULL;
	char line[128];
	unsigned n = 0;

	while (f && fgets(line, sizeof(line), f)) {
		char *sp = strchr(line, ' ');

		if (sp) {
			rdn_wreg(card, (uint32_t)strtoul(line, NULL, 0), (uint32_t)strtoul(sp, NULL, 0));
			n++;
		}
	}
	if (f)
		fclose(f);
	return n;
}

/*
 * Experiment: our UVD start on a card that radeon has brought up (journal 2026-10-18). No accel
 * init: only the UVD block is touched, the firmware goes where radeon has its own copy
 * (RDN_VCPU_OFF, default 0x40000) and the ring 200 MB into video memory.
 */
static int do_uvdonly(struct linux_card *lc, struct rdn_card *card, uint32_t flags)
{
	static uint8_t uvd_fw[300000];
	struct rdn_accel accel;
	struct rdn_uvd uvd;
	struct stat st;
	char path[512];
	uint32_t ref = 0, fw_size, seq = 0;
	FILE *f;
	int fd, r;

	memset(&accel, 0, sizeof(accel));
	snprintf(path, sizeof(path), "%s/SUMO_uvd.bin", fw_dir);
	f = fopen(path, "rb");
	if (!f) {
		perror(path);
		return -1;
	}
	fw_size = (uint32_t)fread(uvd_fw, 1, sizeof(uvd_fw), f);
	fclose(f);
	fd = sysfs_open(lc, "resource0", O_RDWR);
	if (fd < 0 || fstat(fd, &st))
		return -1;
	accel.aperture = mmap(NULL, st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (accel.aperture == MAP_FAILED)
		return -1;
	accel.card = card;
	accel.aperture_size = (uint32_t)st.st_size;
	accel.vram_base = (uint64_t)(rdn_rreg(card, 0x2024) & 0xffff) << 24;
	if (rdn_uvd_ref_clock(card, &ref)) {
		fprintf(stderr, "no PLL reference clock in the VBIOS\n");
		return -1;
	}
	if (getenv("RDN_RLC"))
		load_rlc(card);
	printf("uvdonly: vram base %llx, UVD_STATUS before %08x\n", (unsigned long long)accel.vram_base,
	       (unsigned)rdn_rreg(card, UVD_STATUS));
	if (getenv("RDN_APPLY"))
		printf("uvdonly: applied %u writes of %s\n", apply_file(card, getenv("RDN_APPLY")), getenv("RDN_APPLY"));
	if (getenv("RDN_MIN")) {
		/* the 9 writes a cold-start bisection of radeon's stream left (journal 2026-10-18): VRAM at MC 0, RLC on */
		if (!getenv("RDN_MIN_NORELOC")) {
			rdn_wreg(card, 0x2024, 0x003f0000);	/* MC_VM_FB_LOCATION */
			rdn_wreg(card, 0x2c04, 0);		/* HDP_NONSURFACE_BASE */
		}
		os_delay_us(NULL, 200000);
		flags |= RDN_UVD_F_MINIMAL | (getenv("RDN_MIN_LMI") ? RDN_UVD_F_LMI : 0);
	}
	accel.vram_base = (uint64_t)(rdn_rreg(card, 0x2024) & 0xffff) << 24;
	if (getenv("RDN_KILL_VCPU")) {
		/* hold the VCPU in reset and see that its status drops: only then is a later 2 a real boot */
		uint32_t cg = rdn_rreg(card, UVD_CGC_GATE);

		rdn_wreg(card, UVD_CGC_GATE, 0);
		rdn_wreg(card, UVD_SOFT_RESET, 0x20ef);
		os_delay_us(NULL, 10000);
		rdn_wreg(card, UVD_STATUS, 0);
		printf("uvdonly: VCPU held in reset: UVD_STATUS %08x (CGC_GATE was %08x)\n",
		       (unsigned)rdn_rreg(card, UVD_STATUS), (unsigned)cg);
	}
	r = rdn_uvd_init(&uvd, &accel, ref, uvd_fw, fw_size, RDN_UVD_DEFAULT_HANDLES,
			 getenv("RDN_VCPU_OFF") ? (uint32_t)strtoul(getenv("RDN_VCPU_OFF"), NULL, 0) : 0x40000,
			 200u << 20, 4096, flags);
	printf("uvd init returned %d; UVD_STATUS %08x\n", r, (unsigned)rdn_rreg(card, UVD_STATUS));
	if (getenv("RDN_MIN")) {
		/* Linux's firmware changes 40 bytes at offset 320 of its image and writes a word after it when it runs */
		uint32_t base = getenv("RDN_VCPU_OFF") ? (uint32_t)strtoul(getenv("RDN_VCPU_OFF"), NULL, 0) : 0x40000, k;

		printf("fw window after the start: +fw_size %08x; offset 320..:", (unsigned)rdn_vram_read32(&accel, base + fw_size));
		for (k = 0; k < 10; k++)
			printf(" %08x", (unsigned)rdn_vram_read32(&accel, base + 320 + k * 4));
		printf("\n");
	}
	if (!r) {
		uint32_t ib = (200u << 20) + 0x20000;

		printf("fence word after init: %08x\n", (unsigned)rdn_vram_read32(&accel, (200u << 20) + 4096));
		rdn_vram_write32(&accel, (200u << 20) + 4096, 0);
		rdn_vram_write32(&accel, ib, RDN_UVD_PACKET0(UVD_CONTEXT_ID, 0));
		rdn_vram_write32(&accel, ib + 4, 0x600DF00D);
		r = rdn_uvd_wake(&uvd);
		if (!r)
			r = rdn_uvd_ib_submit(&uvd, rdn_vram_addr(&accel, ib), 2, &seq);
		if (!r)
			r = rdn_uvd_fence_wait(&uvd, seq, 1000);
		printf("uvd IB: %d, UVD_CONTEXT_ID %08x\n", r, (unsigned)rdn_rreg(card, UVD_CONTEXT_ID));
		printf("uvd IB regs: RPTR %x WPTR %x IB_BASE %08x IB_SIZE %x RB_CNTL %08x fence word %08x seq %u\n",
		       (unsigned)rdn_rreg(card, 0xf690), (unsigned)rdn_rreg(card, 0xf694),
		       (unsigned)rdn_rreg(card, 0xf684), (unsigned)rdn_rreg(card, 0xf688),
		       (unsigned)rdn_rreg(card, 0xf6a4), (unsigned)rdn_vram_read32(&accel, (200u << 20) + 4096),
		       (unsigned)seq);
		(void)rdn_uvd_idle(&uvd);
		rdn_uvd_fini(&uvd);
	}
	if (getenv("RDN_RESTORE"))
		printf("uvdonly: restored %u writes\n", apply_file(card, getenv("RDN_RESTORE")));
	return r;
}

/*
 * Save what CRTC 0 scans out (32 bpp, at aperture offset 0) as a PPM file,
 * so that the picture can be looked at without the monitor.
 */
static int do_grab(struct linux_card *lc, struct rdn_card *card, const char *path)
{
	uint32_t w = rdn_rreg(card, EVERGREEN_GRPH_X_END);
	uint32_t h = rdn_rreg(card, EVERGREEN_GRPH_Y_END);
	uint32_t pitch = rdn_rreg(card, EVERGREEN_GRPH_PITCH);
	volatile uint32_t *pix;
	uint32_t x, y;
	struct stat st;
	FILE *f;
	int fd;

	fd = sysfs_open(lc, "resource0", O_RDWR);
	if (fd < 0 || fstat(fd, &st) || !w || !h || pitch < w ||
	    (uint64_t)pitch * h * 4 > (uint64_t)st.st_size)
		return -1;
	pix = mmap(NULL, st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	f = fopen(path, "wb");
	if (pix == MAP_FAILED || !f) {
		perror(path);
		return -1;
	}
	/* See rdn_hdp_flush(): make the card's read cache current. */
	rdn_wreg(card, 0x5480, 1);
	fprintf(f, "P6\n%u %u\n255\n", (unsigned)w, (unsigned)h);
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++) {
			uint32_t v = pix[y * pitch + x];

			fputc((v >> 16) & 0xff, f);
			fputc((v >> 8) & 0xff, f);
			fputc(v & 0xff, f);
		}
	fclose(f);
	munmap((void *)pix, st.st_size);
	close(fd);
	printf("saved %ux%u to %s\n", (unsigned)w, (unsigned)h, path);
	return 0;
}

/* Probe every I2C line the VBIOS lists; a display answers with its EDID. */
static int edid_probe(struct rdn_card *card, const char *save)
{
	uint8_t edid[RDN_EDID_MAX_SIZE];
	struct rdn_i2c_bus bus;
	int i, r, found = 0;
	FILE *f;

	for (i = 0; rdn_i2c_bus_by_index(card, i, &bus); i++) {
		if (!bus.valid)
			continue;
		r = rdn_edid_read(card, &bus, edid);
		if (r < 0) {
			printf("line %d (id %02x, regs %04x): no EDID (%d)\n", i,
			       bus.i2c_id, (unsigned)bus.mask_clk_reg, r);
			continue;
		}
		printf("line %d (id %02x, regs %04x): EDID, %d bytes, "
		       "preferred mode %ux%u\n", i, bus.i2c_id,
		       (unsigned)bus.mask_clk_reg, r,
		       edid[56] | ((edid[58] >> 4) << 8),
		       edid[59] | ((edid[61] >> 4) << 8));
		if (save && !found) {
			f = fopen(save, "wb");
			if (!f || fwrite(edid, 1, r, f) != (size_t)r)
				perror(save);
			if (f)
				fclose(f);
		}
		found++;
	}
	return found ? 0 : 1;
}

int main(int argc, char **argv)
{
	const char *addr = "0000:10:00.0", *bios_file = NULL, *trace = NULL;
	const char *cmd;
	struct linux_card lc;
	struct rdn_card card;
	struct rdn_os os;
	uint32_t command;
	void *bios;
	int opt, fd, no_io = 0, ret = 0;

	while ((opt = getopt(argc, argv, "s:b:t:f:ndR")) != -1) {
		switch (opt) {
		case 's': addr = optarg; break;
		case 'b': bios_file = optarg; break;
		case 't': trace = optarg; break;
		case 'f': fw_dir = optarg; break;
		case 'n': no_io = 1; break;
		case 'R': other_order = 1; break;
		case 'd': force_dvi = 1; break;
		default: return 2;
		}
	}
	if (optind >= argc) {
		fprintf(stderr, "usage: %s [-s addr] [-b vbios] [-t trace] [-f fwdir] [-n] "
			"status|post|vramtest|edid [file]|modeset|accel|uvd [flags]|grab [file]|peek REG...\n", argv[0]);
		return 2;
	}
	cmd = argv[optind];

	memset(&lc, 0, sizeof(lc));
	snprintf(lc.sysfs, sizeof(lc.sysfs), "/sys/bus/pci/devices/%s", addr);
	lc.cfg_fd = sysfs_open(&lc, "config", O_RDWR);
	if (lc.cfg_fd < 0 || check_identity(&lc))
		return 1;

	/* Let the kernel wake the device and route its resources. */
	fd = sysfs_open(&lc, "enable", O_WRONLY);
	if (fd >= 0) {
		if (write(fd, "1", 1) != 1)
			perror("enable");
		close(fd);
	}
	command = os_cfg_read32(&lc, PCI_COMMAND);
	lc.saved_command = command & 0xffff;
	if ((command & (PCI_COMMAND_IO | PCI_COMMAND_MEMORY)) !=
	    (PCI_COMMAND_IO | PCI_COMMAND_MEMORY)) {
		uint16_t c = rdn_swap_le16(lc.saved_command | PCI_COMMAND_IO |
					   PCI_COMMAND_MEMORY);
		if (pwrite(lc.cfg_fd, &c, 2, PCI_COMMAND) != 2)
			perror("PCI command");
	}

	fd = sysfs_open(&lc, "resource2", O_RDWR);
	if (fd < 0)
		return 1;
	lc.mmio = mmap(NULL, MMIO_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (lc.mmio == MAP_FAILED) {
		perror("mmap registers");
		return 1;
	}
	lc.io_fd = no_io ? -1 : sysfs_open(&lc, "resource4", O_RDWR);

	memset(&os, 0, sizeof(os));
	os.cookie = &lc;
	os.mmio_read32 = os_mmio_read32;
	os.mmio_write32 = os_mmio_write32;
	if (lc.io_fd >= 0) {
		os.io_read32 = os_io_read32;
		os.io_write32 = os_io_write32;
	}
	os.cfg_read32 = os_cfg_read32;
	os.cfg_write32 = os_cfg_write32;
	os.delay_us = os_delay_us;
	os.time_ms = os_time_ms;
	os.alloc = os_alloc;
	os.free = os_free;
	os.log = os_log;

	bios = read_rom(&lc, bios_file);
	if (!bios || rdn_card_init(&card, &os, bios)) {
		fprintf(stderr, "no usable VBIOS\n");
		return 1;
	}

	if (!strcmp(cmd, "status")) {
		print_status(&card);
	} else if (!strcmp(cmd, "post")) {
		if (trace) {
			lc.trace = fopen(trace, "w");
			if (!lc.trace)
				perror(trace);
		}
		ret = rdn_card_post(&card);
		if (lc.trace) {
			fclose(lc.trace);
			lc.trace = NULL;
		}
		printf("post returned %d\n", ret);
		print_status(&card);
	} else if (!strcmp(cmd, "modeset")) {
		if (trace) {
			lc.trace = fopen(trace, "w");
			if (!lc.trace)
				perror(trace);
		}
		ret = do_modeset(&lc, &card);
		if (lc.trace) {
			fclose(lc.trace);
			lc.trace = NULL;
		}
	} else if (!strcmp(cmd, "accel")) {
		if (trace) {
			lc.trace = fopen(trace, "w");
			if (!lc.trace)
				perror(trace);
		}
		ret = do_accel(&lc, &card);
		if (lc.trace) {
			fclose(lc.trace);
			lc.trace = NULL;
		}
	} else if (!strcmp(cmd, "replay")) {
		/* replay FILE FROM TO */
		ret = do_replay(&lc, &card, argv[optind + 1], (unsigned)strtoul(argv[optind + 2], NULL, 0),
				(unsigned)strtoul(argv[optind + 3], NULL, 0));
	} else if (!strcmp(cmd, "uvdonly")) {
		if (trace)
			lc.trace = fopen(trace, "w");
		ret = do_uvdonly(&lc, &card, optind + 1 < argc ? (uint32_t)strtoul(argv[optind + 1], NULL, 0) : 0);
		if (lc.trace) {
			fclose(lc.trace);
			lc.trace = NULL;
		}
	} else if (!strcmp(cmd, "uvd")) {
		if (trace) {
			lc.trace = fopen(trace, "w");
			if (!lc.trace)
				perror(trace);
		}
		ret = do_uvd(&lc, &card, optind + 1 < argc ? (uint32_t)strtoul(argv[optind + 1], NULL, 0) : 0);
		if (lc.trace) {
			fclose(lc.trace);
			lc.trace = NULL;
		}
	} else if (!strcmp(cmd, "grab")) {
		ret = do_grab(&lc, &card, optind + 1 < argc ? argv[optind + 1] : "build/grab.ppm");
	} else if (!strcmp(cmd, "peek")) {
		int i;

		for (i = optind + 1; i < argc; i++) {
			uint32_t reg = (uint32_t)strtoul(argv[i], NULL, 16);

			printf("%05x = %08x\n", reg, rdn_rreg(&card, reg));
		}
	} else if (!strcmp(cmd, "edid")) {
		ret = edid_probe(&card, optind + 1 < argc ? argv[optind + 1] : NULL);
	} else if (!strcmp(cmd, "vramtest")) {
		print_status(&card);
		ret = vram_test(&lc);
	} else {
		fprintf(stderr, "unknown command %s\n", cmd);
		ret = 2;
	}

	rdn_card_fini(&card);
	free(bios);
	return ret ? 1 : 0;
}
