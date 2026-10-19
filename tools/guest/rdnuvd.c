/*
 * rdnuvd: ask the kext to bring UVD up and look at the result.
 *
 *   rdnuvd info          the engine's state (starts nothing)
 *   rdnuvd init [flags]  RDN_UC_UVD_INIT: PLL, firmware, VCPU, ring test; flags are
 *                        RDN_UVD_F_* of hw/rdn_uvd.h (1 UDEC config, 2 clocks before the memory window, 4 v1
 *                        window, 8 low clocks, 16 no PLL, 32 keep the clocks up on failure,
 *                        64 offsets instead of GPU addresses, 128 GENERAL_PWRMGT as Linux,
 *                        256 SCLK_PWRMGT_CNTL as Linux, 512 memory through the GART)
 *   rdnuvd test          init if needed, then run one indirect buffer that
 *                        writes UVD_CONTEXT_ID, wait for its fence, read it back
 *   rdnuvd regs          UVD_STATUS and the registers rdn_uvd.c programs
 *   rdnuvd watch <s>     for s seconds read the decoder's whole working memory (the VCPU region: firmware, heap, stack and
 *                        session area) once a second and say which bytes changed since the last look
 *   rdnuvd beat <s> [ms] for s seconds, every ms milliseconds (100) run an indirect buffer of nothing at all through the
 *                        decoder's ring and wait for its fence: work for the firmware while no program is decoding
 *   rdnuvd fwcheck <fw>  compare the firmware in the running decoder's own memory with the file (it changes a few
 *                        bytes of itself; thousands of other differences mean something wrote over it)
 *   rdnuvd bw            how fast the CPU reads and writes video memory through the aperture, by access width
 *   rdnuvd dump <fw>     after a failed init: allocate the same amount of video
 *                        memory (the kext freed its region, and freeing does not
 *                        clear it) and compare its start with the firmware file
 *
 * The kext does nothing about UVD unless its personality has FW_UVD, and
 * only when a program calls init: this tool is that program. Builds on the
 * G5 itself (IOKit only; needs hw/rdn_user.h and hw/rdn_uvd_reg.h):
 *   gcc -I<repo>/hw -o rdnuvd rdnuvd.c -framework IOKit -framework CoreFoundation
 *
 * Copyright (c) 2026 kouta-kun and Claude
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/time.h>
#include <mach/mach.h>
#include <IOKit/IOKitLib.h>
#include "rdn_user.h"
#include "rdn_uvd_reg.h"

static io_connect_t conn;
static struct rdn_user_info info;
static volatile uint32_t *aperture;

static int reg(uint32_t off, uint32_t *v)
{
	int out = 0;
	int kr = IOConnectMethodScalarIScalarO(conn, RDN_UC_REG_READ, 1, 1, (int)off, &out);

	*v = (uint32_t)out;
	return kr;
}

static int open_accel(void)
{
	io_service_t service = IOServiceGetMatchingService(kIOMasterPortDefault,
		IOServiceMatching(RDN_UC_SERVICE_CLASS));
	IOByteCount size = sizeof(info);
	vm_address_t addr = 0;
	vm_size_t len = 0;

	if (!service || IOServiceOpen(service, mach_task_self(), RDN_UC_TYPE, &conn) ||
	    IOConnectMethodScalarIStructureO(conn, RDN_UC_GET_INFO, 0, &size, &info) ||
	    info.version != RDN_USER_VERSION ||
	    IOConnectMapMemory(conn, RDN_UC_MEMORY_APERTURE, mach_task_self(), &addr, &len,
			       kIOMapAnywhere)) {
		fprintf(stderr, "cannot reach the accelerator\n");
		return -1;
	}
	aperture = (volatile uint32_t *)addr;
	return 0;
}

static void show_info(void)
{
	int up = 0, vcpu = 0, ring = 0;
	int kr = IOConnectMethodScalarIScalarO(conn, RDN_UC_UVD_INFO, 0, 3, &up, &vcpu, &ring);

	if (kr)
		printf("UVD info: 0x%x (a kext without UVD methods)\n", kr);
	else
		printf("UVD %s; VCPU region at aperture offset 0x%x, ring at 0x%x\n",
		       up ? "up" : "not started", (unsigned)vcpu, (unsigned)ring);
}

static void show_regs(void)
{
	static const struct { const char *name; uint32_t off; } r[] = {
		{ "UVD_STATUS", UVD_STATUS }, { "UVD_SOFT_RESET", UVD_SOFT_RESET },
		{ "UVD_VCPU_CNTL", UVD_VCPU_CNTL }, { "UVD_RBC_RB_RPTR", UVD_RBC_RB_RPTR },
		{ "UVD_RBC_RB_WPTR", UVD_RBC_RB_WPTR }, { "UVD_CONTEXT_ID", UVD_CONTEXT_ID },
		{ "UVD_RBC_RB_BASE", UVD_RBC_RB_BASE }, { "UVD_RBC_RB_CNTL", UVD_RBC_RB_CNTL },
		{ "UVD_LMI_CTRL", UVD_LMI_CTRL }, { "UVD_LMI_CTRL2", UVD_LMI_CTRL2 },
		{ "UVD_LMI_ADDR_EXT", UVD_LMI_ADDR_EXT }, { "UVD_LMI_EXT40_ADDR", UVD_LMI_EXT40_ADDR },
		{ "UVD_LMI_SWAP_CNTL", UVD_LMI_SWAP_CNTL }, { "UVD_MP_SWAP_CNTL", UVD_MP_SWAP_CNTL },
		{ "UVD_VCPU_CACHE_OFFSET0", UVD_VCPU_CACHE_OFFSET0 }, { "UVD_VCPU_CACHE_SIZE0", UVD_VCPU_CACHE_SIZE0 },
		{ "UVD_VCPU_CACHE_OFFSET1", UVD_VCPU_CACHE_OFFSET1 }, { "UVD_VCPU_CACHE_OFFSET2", UVD_VCPU_CACHE_OFFSET2 },
		{ "UVD_VCPU_CHIP_ID", UVD_VCPU_CHIP_ID_REG }, { "UVD_RB_ARB_CTRL", UVD_RB_ARB_CTRL },
		{ "UVD_CGC_GATE", UVD_CGC_GATE }, { "UVD_MASTINT_EN", UVD_MASTINT_EN },
		{ "SRBM_SOFT_RESET", SRBM_SOFT_RESET_UVD_REG },
		{ "CG_UPLL_FUNC_CNTL", CG_UPLL_FUNC_CNTL }, { "CG_UPLL_FUNC_CNTL_2", CG_UPLL_FUNC_CNTL_2 },
		{ "CG_UPLL_FUNC_CNTL_3", CG_UPLL_FUNC_CNTL_3 },
	};
	unsigned i;
	uint32_t v;

	for (i = 0; i < sizeof(r) / sizeof(r[0]); i++)
		if (!reg(r[i].off, &v))
			printf("  %-18s 0x%08x\n", r[i].name, (unsigned)v);
}

static int do_dump(const char *path)
{
	/* what rdn_uvd_vcpu_bytes() gives for a 205080-byte firmware and 10 handles */
	int off = 0;
	FILE *f = fopen(path, "rb");
	static uint8_t fw[300000];
	size_t n, i, bad = 0, first = (size_t)-1;
	volatile uint8_t *m;

	if (!f || !(n = fread(fw, 1, sizeof(fw), f))) {
		fprintf(stderr, "cannot read %s\n", path);
		return 1;
	}
	fclose(f);
	if (IOConnectMethodScalarIScalarO(conn, RDN_UC_ALLOC, 2, 1, 1187840, 4096, &off)) {
		fprintf(stderr, "no video memory\n");
		return 1;
	}
	m = (volatile uint8_t *)aperture + off;
	for (i = 0; i < n; i++)
		if (m[i] != fw[i]) {
			if (first == (size_t)-1)
				first = i;
			bad++;
		}
	printf("region at aperture offset 0x%x: %lu of %lu firmware bytes differ", (unsigned)off,
	       (unsigned long)bad, (unsigned long)n);
	if (bad)
		printf(" (first at %lu: have %02x want %02x)", (unsigned long)first, m[first], fw[first]);
	printf("\nfirst words there: %02x%02x%02x%02x %02x%02x%02x%02x; file: %02x%02x%02x%02x %02x%02x%02x%02x\n",
	       m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], fw[0], fw[1], fw[2], fw[3], fw[4], fw[5], fw[6], fw[7]);
	IOConnectMethodScalarIScalarO(conn, RDN_UC_FREE, 1, 0, off);
	return bad != 0;
}

#include <sys/time.h>

static double now_s(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return tv.tv_sec + tv.tv_usec / 1e6;
}

/* Reads and writes of one video-memory buffer by the CPU, MB/s, for each access width. */
static int do_bw(void)
{
	enum { BYTES = 4 << 20 };
	int off = 0;
	volatile uint8_t *m8;
	volatile uint32_t *m32;
	volatile double *m64;
	double t;
	unsigned long sum = 0;
	size_t i;

	if (IOConnectMethodScalarIScalarO(conn, RDN_UC_ALLOC, 2, 1, BYTES, 4096, &off)) {
		fprintf(stderr, "no video memory\n");
		return 1;
	}
	m8 = (volatile uint8_t *)aperture + off;
	m32 = (volatile uint32_t *)m8;
	m64 = (volatile double *)m8;
	t = now_s();
	for (i = 0; i < BYTES; i++)
		m8[i] = (uint8_t)i;
	printf("write, 8-bit stores:  %6.1f MB/s\n", BYTES / (now_s() - t) / 1e6);
	t = now_s();
	for (i = 0; i < BYTES / 4; i++)
		m32[i] = (uint32_t)i;
	printf("write, 32-bit stores: %6.1f MB/s\n", BYTES / (now_s() - t) / 1e6);
	t = now_s();
	for (i = 0; i < BYTES / 8; i++)
		m64[i] = (double)i;
	printf("write, 64-bit stores: %6.1f MB/s\n", BYTES / (now_s() - t) / 1e6);
	t = now_s();
	for (i = 0; i < BYTES / 16; i++)
		sum += m8[i];
	printf("read, 8-bit loads:    %6.1f MB/s\n", BYTES / 16 / (now_s() - t) / 1e6);
	t = now_s();
	for (i = 0; i < BYTES / 4 / 4; i++)
		sum += m32[i];
	printf("read, 32-bit loads:   %6.1f MB/s\n", BYTES / 4 / (now_s() - t) / 1e6);
	t = now_s();
	for (i = 0; i < BYTES / 8 / 2; i++)
		sum += (unsigned long)m64[i];
	printf("read, 64-bit loads:   %6.1f MB/s  (sum %lu)\n", BYTES / 2 / (now_s() - t) / 1e6, sum);
	IOConnectMethodScalarIScalarO(conn, RDN_UC_FREE, 1, 0, off);
	return 0;
}

static int do_fwcheck(const char *path)
{
	int up = 0, vcpu = 0, ring = 0;
	static uint8_t fw[300000];
	FILE *f = fopen(path, "rb");
	size_t n, i, bad = 0, first = (size_t)-1, shown = 0;
	volatile uint8_t *m;

	if (!f || !(n = fread(fw, 1, sizeof(fw), f))) {
		fprintf(stderr, "cannot read %s\n", path);
		return 1;
	}
	fclose(f);
	if (IOConnectMethodScalarIScalarO(conn, RDN_UC_UVD_INFO, 0, 3, &up, &vcpu, &ring) || !up) {
		printf("UVD is not up\n");
		return 1;
	}
	m = (volatile uint8_t *)aperture + vcpu;
	for (i = 0; i < n; i++)
		if (m[i] != fw[i]) {
			if (first == (size_t)-1)
				first = i;
			bad++;
			if (shown < 12 && i % 4 == 0 && (shown == 0 || 1)) {
				printf("  byte %lu: have %02x want %02x\n", (unsigned long)i, m[i], fw[i]);
				shown++;
			}
		}
	printf("firmware window at aperture offset 0x%x: %lu of %lu bytes differ (first at %lu)\n", (unsigned)vcpu,
	       (unsigned long)bad, (unsigned long)n, first == (size_t)-1 ? 0UL : (unsigned long)first);
	return 0;
}

static int init_flags;

static int do_init(void)
{
	int status = 0;
	int kr = IOConnectMethodScalarIScalarO(conn, RDN_UC_UVD_INIT, 1, 1, init_flags, &status);

	printf("UVD init (flags 0x%x): kern_return 0x%x, UVD_STATUS 0x%08x\n", init_flags, kr,
	       (unsigned)status);
	return kr;
}

static int do_test(void)
{
	int off = 0, fence = 0, reached = 0;
	uint32_t v = 0;
	int kr;

	if (do_init())
		return 1;
	if (IOConnectMethodScalarIScalarO(conn, RDN_UC_ALLOC, 2, 1, 4096, 4096, &off)) {
		fprintf(stderr, "no video memory for the buffer\n");
		return 1;
	}
	/* in the ring's byte order: the host's own, stored with one 32-bit store each */
	aperture[off / 4] = RDN_UVD_PACKET0(UVD_CONTEXT_ID, 0);
	aperture[off / 4 + 1] = 0x600DF00D;
	kr = IOConnectMethodScalarIScalarO(conn, RDN_UC_UVD_SUBMIT, 2, 1, off, 2, &fence);
	printf("UVD submit: kern_return 0x%x, fence %d\n", kr, fence);
	if (kr)
		return 1;
	kr = IOConnectMethodScalarIScalarO(conn, RDN_UC_UVD_FENCE_WAIT, 2, 1, fence, 1000, &reached);
	printf("fence wait: kern_return 0x%x, %s\n", kr, reached ? "reached" : "NOT reached in 1 s");
	if (!reg(UVD_CONTEXT_ID, &v))
		/* the fence packets that follow the buffer (uvd_v2_2_fence_emit) leave their sequence number here */
		printf("UVD_CONTEXT_ID = 0x%08x (%s)\n", (unsigned)v,
		       v == (unsigned)fence ? "the fence ran, after the buffer" : "the fence did not run");
	IOConnectMethodScalarIScalarO(conn, RDN_UC_FREE, 1, 0, off);
	return reached && v == (unsigned)fence ? 0 : 1;
}

static int do_watch(int seconds)
{
	int up = 0, vcpu = 0, ring = 0, t;
	uint32_t *a, *b, *tmp;
	size_t words, i;

	if (IOConnectMethodScalarIScalarO(conn, RDN_UC_UVD_INFO, 0, 3, &up, &vcpu, &ring) || !up || ring <= vcpu) {
		fprintf(stderr, "UVD is not up\n");
		return 1;
	}
	words = (size_t)(ring - vcpu) / 4;
	a = malloc(words * 4);
	b = malloc(words * 4);
	if (!a || !b)
		return 1;
	for (i = 0; i < words; i++)
		a[i] = aperture[vcpu / 4 + i];
	printf("watching %lu KB at 0x%x\n", (unsigned long)(words * 4 / 1024), vcpu);
	for (t = 1; t <= seconds; t++) {
		unsigned changed = 0, ranges = 0;
		size_t first = 0, last = 0;
		int open = 0;

		sleep(1);
		for (i = 0; i < words; i++)
			b[i] = aperture[vcpu / 4 + i];
		printf("t=%d:", t);
		for (i = 0; i < words; i++) {
			if (a[i] != b[i]) {
				changed++;
				if (!open) {
					open = 1;
					first = i;
				}
				last = i;
			} else if (open && i > last + 16) {
				if (ranges++ < 6)
					printf(" 0x%lx-0x%lx", (unsigned long)(first * 4), (unsigned long)(last * 4 + 3));
				open = 0;
			}
		}
		if (open && ranges++ < 6)
			printf(" 0x%lx-0x%lx", (unsigned long)(first * 4), (unsigned long)(last * 4 + 3));
		printf("  (%u words changed in %u ranges)\n", changed, ranges);
		fflush(stdout);
		tmp = a;
		a = b;
		b = tmp;
	}
	return 0;
}

static int do_beat(double seconds, int ms)
{
	int off = 0, fence = 0, reached = 0, i, kr;
	struct timeval t0, t;
	unsigned n = 0, missed = 0;

	if (IOConnectMethodScalarIScalarO(conn, RDN_UC_ALLOC, 2, 1, 4096, 4096, &off)) {
		fprintf(stderr, "no video memory for the buffer\n");
		return 1;
	}
	for (i = 0; i < 16; i++)
		aperture[off / 4 + i] = 0x80000000;	/* type 2 packets: no operation */
	gettimeofday(&t0, NULL);
	do {
		kr = IOConnectMethodScalarIScalarO(conn, RDN_UC_UVD_SUBMIT, 2, 1, off, 16, &fence);
		if (!kr)
			kr = IOConnectMethodScalarIScalarO(conn, RDN_UC_UVD_FENCE_WAIT, 2, 1, fence, 1000, &reached);
		if (kr || !reached)
			missed++;
		n++;
		usleep((useconds_t)ms * 1000);
		gettimeofday(&t, NULL);
	} while ((t.tv_sec - t0.tv_sec) + (t.tv_usec - t0.tv_usec) / 1e6 < seconds);
	printf("%u empty buffers run, %u without a fence\n", n, missed);
	IOConnectMethodScalarIScalarO(conn, RDN_UC_FREE, 1, 0, off);
	return missed != 0;
}

int main(int argc, char **argv)
{
	const char *cmd = argc > 1 ? argv[1] : "info";

	if (argc > 2 && !strcmp(cmd, "init"))
		init_flags = (int)strtoul(argv[2], NULL, 0);
	if (open_accel())
		return 1;
	if (!strcmp(cmd, "info"))
		show_info();
	else if (!strcmp(cmd, "init"))
		return do_init() ? 1 : 0;
	else if (!strcmp(cmd, "test"))
		return do_test();
	else if (!strcmp(cmd, "dump") && argc > 2)
		return do_dump(argv[2]);
	else if (!strcmp(cmd, "fwcheck") && argc > 2)
		return do_fwcheck(argv[2]);
	else if (!strcmp(cmd, "bw"))
		return do_bw();
	else if (!strcmp(cmd, "regs"))
		show_regs();
	else if (!strcmp(cmd, "watch") && argc > 2)
		return do_watch(atoi(argv[2]));
	else if (!strcmp(cmd, "beat") && argc > 2)
		return do_beat(atof(argv[2]), argc > 3 ? atoi(argv[3]) : 100);
	else {
		fprintf(stderr, "usage: rdnuvd info | init | test | regs | bw | dump <firmware>\n");
		return 2;
	}
	return 0;
}
