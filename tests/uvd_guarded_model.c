/*
 * Test of the UVD bring-up (hw/rdn_uvd.c) against a model of the block:
 * the PLL handshake, the VCPU coming out of reset, the memory window, the
 * ring and indirect buffer packets (read byte-swapped when the swap register
 * says so) and the VCPU's fence write. It checks the order of what the code
 * does, not what the real card does; nothing here has run on a card.
 *
 * Run natively and under qemu-ppc: the output must be identical.
 *
 * Copyright (c) 2026 kouta-kun and Claude
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hw/rdn_uvd.h"
#include "../hw/rdn_uvd_reg.h"

#define APERTURE	(8u << 20)
#define VRAM_BASE	0xF00000000ull
#define FW_SIZE		4096u
#define RING_BYTES	4096u
#define VCPU_OFFSET	0x100000u
#define RING_OFFSET	0x10000u

static uint32_t regs[0x20000 / 4];
static uint8_t *mem;		/* the aperture, bytes as the card stores them */
static unsigned writes;
static int failures;
static uint32_t ring_read;	/* the model's read pointer, dwords */
static uint32_t unaligned_publications;
static unsigned reads, delays;
static uint64_t model_time;
static int no_pll, no_vcpu, stall_ring, no_fence, frozen_time;
static int bad_rlc_readback;
static unsigned diagnostic_reads;
static int diagnostic_unclocked;
static struct { uint32_t reg, value; } write_log[4096];
static unsigned write_log_count;
static uint64_t last_fence_gpu;
static uint32_t last_fence_value, ib_context_value;
static unsigned bad_fence_addresses;

static uint32_t bswap32(uint32_t v)
{
	return (v << 24) | ((v & 0xff00) << 8) | ((v >> 8) & 0xff00) | (v >> 24);
}

static uint32_t mem_le32(uint32_t offset)
{
	return mem[offset] | (mem[offset + 1] << 8) | (mem[offset + 2] << 16) |
	       ((uint32_t)mem[offset + 3] << 24);
}

/* A word of the ring or an IB as the VCPU sees it. */
static uint32_t cmd_word(uint32_t offset)
{
	uint32_t v = mem_le32(offset);

	return regs[UVD_LMI_SWAP_CNTL / 4] == 0xa ? bswap32(v) : v;
}

static void reg_write(uint32_t reg, uint32_t val);

static void run_packets(uint32_t offset, uint32_t words)
{
	uint32_t i = 0;

	while (i < words) {
		uint32_t w = cmd_word(offset + 4 * i++);
		uint32_t n, reg;

		if ((w >> 30) != 0) {
			printf("FAIL: packet type %u at word %u\n", w >> 30, i - 1);
			failures++;
			continue;
		}
		reg = (w & 0xffff) << 2;
		if (reg == UVD_NO_OP)
			continue;
		for (n = ((w >> 16) & 0x3fff) + 1; n && i < words; n--, reg += 4) {
			uint32_t value = cmd_word(offset + 4 * i++);
			if (reg == UVD_CONTEXT_ID)
				ib_context_value = value;
			reg_write(reg, value);
		}
	}
}

static void reg_write(uint32_t reg, uint32_t val)
{
	if (getenv("UVD_MODEL_TRACE"))
		fprintf(stderr, "W mmio %05x %08x 4\n", (unsigned)reg, (unsigned)val);
	writes++;
	if (write_log_count < sizeof(write_log) / sizeof(write_log[0])) {
		write_log[write_log_count].reg = reg;
		write_log[write_log_count++].value = val;
	}
	regs[reg / 4] = val;
	switch (reg) {
	case UVD_RBC_RB_WPTR: {
		uint32_t ring_words = RING_BYTES / 4, base = regs[UVD_RBC_RB_BASE / 4];
		uint32_t rptr = ring_read;

		if (stall_ring)
			break;
		if (val & 15) {
			unaligned_publications++;
			break;
		}

		while (rptr != (val & (ring_words - 1))) {
			/* one packet at a time: ring words are walked as packets */
			uint32_t w = cmd_word(base + 4 * rptr);
			uint32_t n = ((w >> 16) & 0x3fff) + 1, r = (w & 0xffff) << 2, k;

			rptr = (rptr + 1) & (ring_words - 1);
			if (r == UVD_NO_OP)
				continue;
			for (k = 0; k < n; k++, r += 4) {
				uint32_t v = cmd_word(base + 4 * rptr);

				rptr = (rptr + 1) & (ring_words - 1);
				reg_write(r, v);
			}
		}
		ring_read = rptr;
		regs[UVD_RBC_RB_RPTR / 4] = rptr;
		break;
	}
	case UVD_RBC_IB_SIZE:
		run_packets(regs[UVD_RBC_IB_BASE / 4], val);
		break;
	case UVD_GPCOM_VCPU_CMD:
		if (val == 0 && !no_fence) {	/* write the fence */
			uint32_t high = regs[UVD_GPCOM_VCPU_DATA1 / 4];
			uint64_t address = ((uint64_t)(high & 0xff) << 32) |
				regs[UVD_GPCOM_VCPU_DATA0 / 4];
			uint32_t off, v = regs[UVD_CONTEXT_ID / 4];

			if ((high & ~0xffu) || address < VRAM_BASE ||
			    address > VRAM_BASE + APERTURE - 4) {
				bad_fence_addresses++;
				break;
			}
			off = (uint32_t)(address - VRAM_BASE);
			mem[off] = v; mem[off + 1] = v >> 8; mem[off + 2] = v >> 16; mem[off + 3] = v >> 24;
			last_fence_gpu = address;
			last_fence_value = v;
		}
		break;
	case UVD_RBC_RB_RPTR:
		ring_read = val;
		break;
	}
}

static uint32_t reg_read(uint32_t reg)
{
	uint32_t v = regs[reg / 4];
	reads++;

	switch (reg) {
	case UVD_CGC_CTRL:
	case UVD_CGC_STATUS:
	case UVD_LMI_STATUS:
		diagnostic_reads |= reg == UVD_CGC_CTRL ? 1u : reg == UVD_CGC_STATUS ? 2u : 4u;
		if (regs[CG_UPLL_FUNC_CNTL / 4] & (UPLL_BYPASS_EN_MASK | UPLL_SLEEP_MASK))
			diagnostic_unclocked = 1;
		break;
	case RLC_CNTL:
		return bad_rlc_readback && v == RLC_ENABLE ? 0 : v;
	case CG_UPLL_FUNC_CNTL:
		/* the PLL acknowledges a control request */
		if ((v & UPLL_CTLREQ_MASK) && !no_pll)
			v |= UPLL_CTLACK_MASK | UPLL_CTLACK2_MASK;
		return v;
	case UVD_STATUS:
		/* the VCPU runs once it is out of reset, clocked, with the UMC unstalled */
		return (!no_vcpu && !regs[UVD_SOFT_RESET / 4] && (regs[UVD_VCPU_CNTL / 4] & (1 << 9)) &&
			!(regs[UVD_LMI_CTRL2 / 4] & (1 << 8))) ? 2 : 0;
	}
	return v;
}

static uint32_t os_read(void *c, uint32_t off) { (void)c; return reg_read(off); }
static void os_write(void *c, uint32_t off, uint32_t v) { (void)c; reg_write(off, v); }
static void os_delay(void *c, uint32_t us) { (void)c; (void)us; delays++; }
static uint64_t os_time(void *c) { (void)c; if (!frozen_time) model_time++; return model_time; }
static void *os_alloc(void *c, size_t n) { (void)c; return calloc(1, n); }
static void os_free(void *c, void *p) { (void)c; free(p); }
static void os_log(void *c, enum rdn_log_level l, const char *f, va_list ap)
{
	(void)c;
	/* Raw swap-register and ring-byte diagnostics differ between hosts. */
	if (l == RDN_LOG_ERROR && getenv("UVD_MODEL_TRACE")) {
		printf("log: ");
		vprintf(f, ap);
		printf("\n");
	}
}

static void check(int ok, const char *what)
{
	printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok)
		failures++;
}

static void reset_model(struct rdn_uvd *uvd)
{
	memset(uvd, 0, sizeof(*uvd));
	memset(regs, 0, sizeof(regs));
	memset(mem, 0, APERTURE);
	reads = writes = delays = ring_read = unaligned_publications = 0;
	model_time = 0;
	no_pll = no_vcpu = stall_ring = no_fence = frozen_time = 0;
	bad_rlc_readback = 0;
	diagnostic_reads = 0;
	diagnostic_unclocked = 0;
	write_log_count = 0;
	last_fence_gpu = 0;
	last_fence_value = ib_context_value = bad_fence_addresses = 0;
}

static int init_model(struct rdn_uvd *uvd, struct rdn_accel *accel, const uint8_t *fw)
{
	return rdn_uvd_init_checked(uvd, accel, 2700, fw, FW_SIZE, RDN_UVD_DEFAULT_HANDLES,
		VCPU_OFFSET, RING_OFFSET, RING_BYTES, 0);
}

static void store_command(uint32_t offset, uint32_t value)
{
	if (RDN_BIG_ENDIAN)
		value = bswap32(value);
	mem[offset] = value;
	mem[offset + 1] = value >> 8;
	mem[offset + 2] = value >> 16;
	mem[offset + 3] = value >> 24;
}

static void validation_tests(struct rdn_uvd *uvd, struct rdn_accel *accel, const uint8_t *fw)
{
	unsigned i;
	uint32_t size = rdn_uvd_vcpu_bytes(FW_SIZE, RDN_UVD_DEFAULT_HANDLES);
	int r;

	reset_model(uvd);
	check(!rdn_uvd_vcpu_bytes(0, 1) && !rdn_uvd_vcpu_bytes(3, 1) &&
	      !rdn_uvd_vcpu_bytes(0xfffffffcu, 1) && !rdn_uvd_vcpu_bytes(FW_SIZE, 0) &&
	      !rdn_uvd_vcpu_bytes(FW_SIZE, 11) && !rdn_uvd_vcpu_bytes(FW_SIZE, 0xffffffffu),
	      "invalid firmware sizes and session counts cannot overflow allocation");
	for (i = 0; i < 32; i++)
		check(rdn_uvd_init_checked(uvd, accel, 2700, fw, FW_SIZE, 10,
			VCPU_OFFSET, RING_OFFSET, RING_BYTES, 1u << i) < 0,
		      "each nonzero diagnostic/unknown flag is rejected");
	check(rdn_uvd_init_checked(uvd, accel, 0, fw, FW_SIZE, 10,
		VCPU_OFFSET, RING_OFFSET, RING_BYTES, 0) < 0, "invalid reference clock rejected before MMIO");
	check(rdn_uvd_set_clocks(accel->card, 2700, 1, 0) < 0,
	      "partial clock request rejected before MMIO");
	check(rdn_uvd_init_checked(uvd, accel, 2700, fw, FW_SIZE, 10,
		VCPU_OFFSET, VCPU_OFFSET, RING_BYTES, 0) < 0, "overlapping engine buffers rejected");
	check(rdn_uvd_init_checked(uvd, accel, 2700, fw, FW_SIZE, 10,
		VCPU_OFFSET, 0xfffff000u, 0x80000000u, 0) < 0, "ring extent overflow rejected");
	check(rdn_uvd_init_mem_checked(uvd, accel, 2700, fw, FW_SIZE, 10,
		mem + VCPU_OFFSET, 0x10000000000ull, mem + RING_OFFSET, VRAM_BASE + RING_OFFSET,
		RING_BYTES, 0) < 0, "GPU addresses beyond 40 bits rejected");
	check(rdn_uvd_init_mem_checked(uvd, accel, 2700, fw, FW_SIZE, 10,
		mem + VCPU_OFFSET, VRAM_BASE + 0xffff000, mem + RING_OFFSET, VRAM_BASE + RING_OFFSET,
		RING_BYTES, 0) < 0, "VCPU extent crossing 256 MB rejected");
	check(rdn_uvd_init_mem_checked(uvd, accel, 2700, fw, FW_SIZE, 10,
		mem + VCPU_OFFSET, VRAM_BASE + VCPU_OFFSET, mem + RING_OFFSET, VRAM_BASE + 0xfffff000u,
		RING_BYTES, 0) < 0, "ring fence crossing 4 GB rejected");
	check(rdn_uvd_init_mem_checked(uvd, accel, 2700, fw, FW_SIZE, 10,
		mem + VCPU_OFFSET, VRAM_BASE + VCPU_OFFSET, mem + VCPU_OFFSET + size - 4096,
		VRAM_BASE + RING_OFFSET, RING_BYTES, 0) < 0, "aliased CPU buffers rejected");
	check(rdn_uvd_init_mem_checked(uvd, accel, 2700, fw, FW_SIZE, 10,
		mem + VCPU_OFFSET, VRAM_BASE + VCPU_OFFSET, mem + RING_OFFSET + 1,
		VRAM_BASE + RING_OFFSET, RING_BYTES, 0) < 0, "unaligned CPU fence storage rejected");
	accel->vram_base = 0xfffffffffffff000ull;
	r = init_model(uvd, accel, fw);
	accel->vram_base = VRAM_BASE;
	check(r < 0, "VRAM base addition overflow rejected");
	check(!reads && !writes && !delays && rdn_uvd_storage_releasable(uvd),
	      "all validation failures leave hardware untouched and storage releasable");
}

static void failure_tests(struct rdn_uvd *uvd, struct rdn_accel *accel, const uint8_t *fw)
{
	unsigned i, before;
	uint32_t seq, ib = 0x200000;

	for (i = 0; i < 3; i++) {
		reset_model(uvd);
		no_pll = i == 0;
		no_vcpu = i == 1;
		stall_ring = i == 2;
		check(init_model(uvd, accel, fw) < 0 && uvd->failed && !uvd->ready &&
		      !rdn_uvd_storage_releasable(uvd) && delays < 200000,
		      "PLL/VCPU/ring startup stall is bounded and quarantines storage");
		check(uvd->status_snapshot == (i == 2 ? 2u : 0u), "status snapshot survives failed startup");
		check(diagnostic_reads == (i ? 7u : 0u) && !diagnostic_unclocked,
		      "CGC and LMI diagnostics are read only after the PLL handshake");
		before = writes;
		check(init_model(uvd, accel, fw) < 0 && writes == before,
		      "failed hardware instance cannot be reinitialized");
		rdn_uvd_fini(uvd);
		check(!rdn_uvd_storage_releasable(uvd), "stop cannot erase quarantine");
	}
	reset_model(uvd);
	check(init_model(uvd, accel, fw) == 0, "fresh model recovers only after hardware reset");
	store_command(ib, RDN_UVD_PACKET0(UVD_CONTEXT_ID, 0));
	store_command(ib + 4, 0x12345678);
	no_fence = 1;
	check(rdn_uvd_ib_submit(uvd, VRAM_BASE + ib, 2, &seq) == 0,
	      "model can hold an unfinished fence");
	check(rdn_uvd_fence_wait(uvd, seq, 0) < 0 && !uvd->failed,
	      "zero-time poll does not poison a pending engine");
	check(rdn_uvd_idle(uvd) < 0 && !uvd->failed, "pending work prevents clock gating");
	frozen_time = 1;
	before = delays;
	check(rdn_uvd_fence_wait(uvd, seq, 3) < 0 && uvd->failed && delays - before <= 30,
	      "stopped monotonic clock cannot make fence wait unbounded");
	before = writes;
	check(rdn_uvd_ib_submit(uvd, VRAM_BASE + ib, 2, &seq) < 0 && writes == before,
	      "timed-out engine rejects further submissions");
	reset_model(uvd);
	check(init_model(uvd, accel, fw) == 0, "fresh model for ring-space failure");
	regs[UVD_RBC_RB_RPTR / 4] = (uvd->wptr + 1) & (uvd->ring_words - 1);
	before = delays;
	check(rdn_uvd_ib_submit(uvd, VRAM_BASE + ib, 2, &seq) < 0 && uvd->failed &&
	      delays - before == 100000, "full-ring wait is bounded and poisons the engine");
	reset_model(uvd);
	check(init_model(uvd, accel, fw) == 0, "fresh model for invalid ring read pointer");
	regs[UVD_RBC_RB_RPTR / 4] = uvd->ring_words;
	before = writes;
	check(rdn_uvd_ib_submit(uvd, VRAM_BASE + ib, 2, &seq) < 0 && uvd->failed &&
	      writes == before, "invalid hardware read pointer is not masked into a free ring");
	reset_model(uvd);
	check(init_model(uvd, accel, fw) == 0, "fresh model for clock wrap");
	store_command(ib, RDN_UVD_PACKET0(UVD_CONTEXT_ID, 0));
	store_command(ib + 4, 0x12345678);
	no_fence = 1;
	check(rdn_uvd_ib_submit(uvd, VRAM_BASE + ib, 2, &seq) == 0, "pending fence before clock wrap");
	model_time = ~0ull - 1;
	before = delays;
	check(rdn_uvd_fence_wait(uvd, seq, 3) < 0 && uvd->failed && delays - before <= 3,
	      "monotonic counter wrap preserves bounded timeout");
}

static unsigned first_write(uint32_t reg)
{
	unsigned i;
	for (i = 0; i < write_log_count; i++)
		if (write_log[i].reg == reg)
			return i;
	return write_log_count;
}

static void cache_window_tests(struct rdn_uvd *uvd, struct rdn_accel *accel)
{
	static uint8_t fw[205080];
	static const uint64_t gpu[] = { 0xf0269d000ull, 0xf1269d000ull };
	static const uint32_t ext28[] = { 0, 0x1001 };
	unsigned i;
	int r;
	uint64_t upper;

	for (i = 0; i < sizeof(fw); i++)
		fw[i] = (uint8_t)(i * 13 + 7);
	for (i = 0; i < sizeof(gpu) / sizeof(gpu[0]); i++) {
		reset_model(uvd);
		/* Stop before ring execution: CPU buffers model the high GPU mapping. */
		no_pll = 1;
		r = rdn_uvd_init_mem_checked(uvd, accel, 2700, fw, sizeof(fw), 10,
			mem + VCPU_OFFSET, gpu[i], mem + RING_OFFSET, gpu[i] + 0x122000,
			RING_BYTES, 0);
		check(r < 0 && uvd->started && uvd->failed && !uvd->ready &&
		      !rdn_uvd_storage_releasable(uvd) &&
		      first_write(UVD_RBC_RB_BASE) == write_log_count,
		      "high-address window test stops at the injected PLL failure");
		check(regs[UVD_VCPU_CACHE_OFFSET0 / 4] == 0x004d3a00 &&
		      regs[UVD_VCPU_CACHE_OFFSET1 / 4] == 0x004da000 &&
		      regs[UVD_VCPU_CACHE_OFFSET2 / 4] == 0x004e2000,
		      "all three cache offsets contain only physical address bits 3-27");
		check(regs[UVD_VCPU_CACHE_SIZE0 / 4] == 0x6600 &&
		      regs[UVD_VCPU_CACHE_SIZE1 / 4] == 0x8000 &&
		      regs[UVD_VCPU_CACHE_SIZE2 / 4] == 0x15e00,
		      "actual firmware length retains firmware, heap and session window sizes");
		check(regs[UVD_LMI_ADDR_EXT / 4] == ext28[i] &&
		      regs[UVD_LMI_EXT40_ADDR / 4] == 0x8009000f,
		      "address bits 28-39 remain in the original LMI extension fields");
		upper = ((uint64_t)(regs[UVD_LMI_EXT40_ADDR / 4] & 0xff) << 32) |
			((uint64_t)(regs[UVD_LMI_ADDR_EXT / 4] & 0xf) << 28);
		check((upper | ((uint64_t)regs[UVD_VCPU_CACHE_OFFSET0 / 4] << 3)) == gpu[i] &&
		      (upper | ((uint64_t)regs[UVD_VCPU_CACHE_OFFSET1 / 4] << 3)) == gpu[i] + 0x33000 &&
		      (upper | ((uint64_t)regs[UVD_VCPU_CACHE_OFFSET2 / 4] << 3)) == gpu[i] + 0x73000,
		      "cache offsets plus LMI extensions reconstruct all three 40-bit addresses");
		check(accel->vram_base == VRAM_BASE &&
		      first_write(0x2024) == write_log_count && first_write(0x2c04) == write_log_count &&
		      memcmp(mem + VCPU_OFFSET, fw, sizeof(fw)) == 0,
		      "masking preserves VRAM configuration and byte-exact firmware storage");
	}
}

static int init_rlc_model(struct rdn_uvd *uvd, struct rdn_accel *accel,
			  const uint8_t *fw, const uint8_t *rlc, uint32_t size)
{
	return rdn_uvd_init_rlc(uvd, accel, 2700, fw, FW_SIZE, 10,
		VCPU_OFFSET, RING_OFFSET, RING_BYTES, 0, rlc, size);
}

static void rlc_tests(struct rdn_uvd *uvd, struct rdn_accel *accel, const uint8_t *fw)
{
	static const uint32_t zero_regs[] = {
		RLC_CNTL, RLC_HB_CNTL, RLC_HB_BASE, RLC_HB_RPTR, RLC_HB_WPTR,
		RLC_HB_WPTR_LSB_ADDR, RLC_HB_WPTR_MSB_ADDR, RLC_MC_CNTL, RLC_UCODE_CNTL
	};
	static const uint32_t bad_sizes[] = { 0, 1, 3071, 3073, 4096, 0xffffffffu };
	static const uint32_t configured[] = { RLC_ENABLE, 2, 0xffffffffu };
	uint8_t storage[RDN_UVD_RLC_BYTES + 2];
	uint8_t *rlc = storage + 1;
	unsigned i, start, cursor, before;
	int good, r;

	for (i = 0; i < sizeof(storage); i++)
		storage[i] = (uint8_t)(i * 31 + i / 7 + 0x83);
	reset_model(uvd);
	mem[VCPU_OFFSET] = mem[RING_OFFSET] = 0xa5;
	for (i = 0; i < sizeof(bad_sizes) / sizeof(bad_sizes[0]); i++)
		check(init_rlc_model(uvd, accel, fw, rlc, bad_sizes[i]) < 0,
		      "RLC requires exactly 3072 firmware bytes");
	check(init_rlc_model(uvd, accel, fw, NULL, RDN_UVD_RLC_BYTES) < 0,
	      "null RLC firmware rejected");
	check(init_rlc_model(uvd, accel, fw, (const uint8_t *)(UINTPTR_MAX - 7),
		RDN_UVD_RLC_BYTES) < 0, "RLC pointer extent overflow rejected");
	check(init_rlc_model(uvd, accel, fw, mem + VCPU_OFFSET, RDN_UVD_RLC_BYTES) < 0 &&
	      init_rlc_model(uvd, accel, fw, mem + RING_OFFSET, RDN_UVD_RLC_BYTES) < 0,
	      "RLC firmware cannot alias either output buffer");
	check(rdn_uvd_init_rlc(uvd, accel, 2700, fw, FW_SIZE, 10,
		VCPU_OFFSET, RING_OFFSET, RING_BYTES, RDN_UVD_F_MINIMAL,
		rlc, RDN_UVD_RLC_BYTES) < 0, "RLC path still rejects minimal initialization");
	check(!reads && !writes && !delays && mem[VCPU_OFFSET] == 0xa5 &&
	      mem[RING_OFFSET] == 0xa5 && rdn_uvd_storage_releasable(uvd),
	      "malformed RLC input leaves hardware and output buffers untouched");

	for (i = 0; i < sizeof(configured) / sizeof(configured[0]); i++) {
		reset_model(uvd);
		regs[RLC_CNTL / 4] = configured[i];
		mem[VCPU_OFFSET] = mem[RING_OFFSET] = 0xa5;
		check(init_rlc_model(uvd, accel, fw, rlc, RDN_UVD_RLC_BYTES) == -16 &&
		      reads == 1 && !writes && !delays && !uvd->started && !uvd->failed &&
		      uvd->rlc_cntl_snapshot == configured[i] &&
		      regs[RLC_CNTL / 4] == configured[i] &&
		      mem[VCPU_OFFSET] == 0xa5 && mem[RING_OFFSET] == 0xa5 &&
		      rdn_uvd_storage_releasable(uvd),
		      "configured RLC is preserved before any hardware write");
	}

	reset_model(uvd);
	regs[SRBM_SOFT_RESET_UVD_REG / 4] = SOFT_RESET_UVD | (1u << 12);
	r = init_rlc_model(uvd, accel, fw, rlc, RDN_UVD_RLC_BYTES);
	check(!r && uvd->ready && uvd->rlc_loaded && uvd->rlc_cntl_snapshot == RLC_ENABLE,
	      "unaligned big-endian RLC image initializes successfully");
	start = first_write(RLC_CNTL);
	cursor = start;
	good = cursor + 9 + 2 * (RDN_UVD_RLC_BYTES / 4) + 2 < write_log_count;
	for (i = 0; good && i < sizeof(zero_regs) / sizeof(zero_regs[0]); i++, cursor++)
		good = write_log[cursor].reg == zero_regs[i] && !write_log[cursor].value;
	check(good, "discrete RLC stops and clears all history/MC/microcode controls in order");
	for (i = 0; good && i < RDN_UVD_RLC_BYTES / 4; i++, cursor += 2) {
		uint32_t word = (uint32_t)rlc[4 * i] * 0x1000000u +
			(uint32_t)rlc[4 * i + 1] * 0x10000u +
			(uint32_t)rlc[4 * i + 2] * 0x100u + rlc[4 * i + 3];
		good = write_log[cursor].reg == RLC_UCODE_ADDR &&
		       write_log[cursor].value == i &&
		       write_log[cursor + 1].reg == RLC_UCODE_DATA &&
		       write_log[cursor + 1].value == word;
	}
	check(good && i == 768, "all 768 RLC words reach MMIO in firmware big-endian order");
	check(good && write_log[cursor].reg == RLC_UCODE_ADDR &&
	      write_log[cursor].value == 0 && write_log[cursor + 1].reg == RLC_CNTL &&
	      write_log[cursor + 1].value == RLC_ENABLE,
	      "RLC upload resets its address and enables only the discrete RLC bit");
	check(first_write(UVD_VCPU_CACHE_OFFSET0) < start &&
	      first_write(UVD_VCPU_CHIP_ID_REG) < start &&
	      first_write(CG_UPLL_FUNC_CNTL_2) > cursor + 1 &&
	      first_write(UVD_SOFT_RESET) > cursor + 1,
	      "memory windows precede RLC and RLC precedes PLL/VCPU startup");
	check(first_write(0x2024) == write_log_count && first_write(0x2c04) == write_log_count &&
	      accel->vram_base == VRAM_BASE &&
	      regs[UVD_VCPU_CACHE_OFFSET0 / 4] == (VCPU_OFFSET >> 3),
	      "RLC path preserves the existing VRAM base and aperture configuration");
	check(first_write(SRBM_SOFT_RESET_UVD_REG) > cursor + 1 &&
	      regs[SRBM_SOFT_RESET_UVD_REG / 4] == (1u << 12),
	      "full UVD startup deasserts only its SRBM reset after RLC startup");
	check(regs[UVD_LMI_SWAP_CNTL / 4] == (RDN_BIG_ENDIAN ? 0xa : 0) &&
	      !regs[UVD_MP_SWAP_CNTL / 4] && memcmp(mem + VCPU_OFFSET, fw, FW_SIZE) == 0,
	      "RLC path retains full UVD endian setup and byte-preserving firmware copy");
	rdn_uvd_fini(uvd);
	check(regs[RLC_CNTL / 4] == RLC_ENABLE && uvd->rlc_loaded &&
	      !rdn_uvd_storage_releasable(uvd), "UVD fini does not restore or disable shared RLC state");

	reset_model(uvd);
	bad_rlc_readback = 1;
	r = init_rlc_model(uvd, accel, fw, rlc, RDN_UVD_RLC_BYTES);
	check(r == -5 && uvd->failed && uvd->started && !uvd->ready && !uvd->rlc_loaded &&
	      !uvd->rlc_cntl_snapshot && !rdn_uvd_storage_releasable(uvd),
	      "RLC enable mismatch fails sticky and quarantines hardware-visible storage");
	check(first_write(CG_UPLL_FUNC_CNTL_2) == write_log_count &&
	      first_write(UVD_SOFT_RESET) == write_log_count && !delays,
	      "RLC readback failure does not start PLL/VCPU or wait indefinitely");
	before = writes;
	rdn_uvd_fini(uvd);
	check(init_rlc_model(uvd, accel, fw, rlc, RDN_UVD_RLC_BYTES) < 0 &&
	      writes == before && regs[RLC_CNTL / 4] == RLC_ENABLE,
	      "failed RLC cannot be retried or blindly restored by fini");

	for (i = 0; i < 2; i++) {
		reset_model(uvd);
		no_pll = i == 0;
		no_vcpu = i == 1;
		r = init_rlc_model(uvd, accel, fw, rlc, RDN_UVD_RLC_BYTES);
		check(r < 0 && uvd->started && uvd->failed && !uvd->ready &&
		      uvd->rlc_loaded && regs[RLC_CNTL / 4] == RLC_ENABLE &&
		      !rdn_uvd_storage_releasable(uvd) && delays < 1200,
		      "post-RLC PLL/VCPU failure remains bounded and retains both engine states");
	}
}

static int prepare_rlc_model(struct rdn_uvd *uvd, struct rdn_accel *accel,
			     const uint8_t *fw, const uint8_t *rlc)
{
	return rdn_uvd_prepare_rlc(uvd, accel, 2700, fw, FW_SIZE, 10,
		VCPU_OFFSET, RING_OFFSET, RING_BYTES, 0, rlc, RDN_UVD_RLC_BYTES);
}

static void split_start_tests(struct rdn_uvd *uvd, struct rdn_accel *accel,
			      const uint8_t *fw)
{
	uint8_t rlc[RDN_UVD_RLC_BYTES];
	uint32_t seq = 0;
	unsigned i, boundary, before_reads, before_writes, before_delays;
	int r, untouched;

	for (i = 0; i < sizeof(rlc); i++)
		rlc[i] = (uint8_t)(i * 17 + 0x29);
	reset_model(uvd);
	check(rdn_uvd_start_prepared(NULL) < 0 && rdn_uvd_start_prepared(uvd) < 0 &&
	      !reads && !writes && !delays, "start requires an owned prepared instance");
	check(rdn_uvd_prepare_rlc(uvd, accel, 2700, NULL, FW_SIZE, 10,
		VCPU_OFFSET, RING_OFFSET, RING_BYTES, 0, rlc, sizeof(rlc)) < 0 &&
	      rdn_uvd_prepare_rlc(uvd, accel, 2700, fw, 3, 10,
		VCPU_OFFSET, RING_OFFSET, RING_BYTES, 0, rlc, sizeof(rlc)) < 0 &&
	      rdn_uvd_prepare_rlc(uvd, accel, 2700, fw, FW_SIZE, 10,
		VCPU_OFFSET, RING_OFFSET, RING_BYTES, 0, rlc, sizeof(rlc) - 1) < 0 &&
	      !uvd->prepared && !reads && !writes && !delays && rdn_uvd_storage_releasable(uvd),
	      "split preparation rejects malformed firmware before touching hardware");
	check(!prepare_rlc_model(uvd, accel, fw, rlc) && uvd->prepared && uvd->started &&
	      !uvd->ready && !uvd->failed && uvd->rlc_loaded && uvd->in_vram &&
	      !rdn_uvd_storage_releasable(uvd), "preparation owns copied storage and loaded RLC without starting VCPU");
	check(first_write(UVD_VCPU_CACHE_OFFSET0) < first_write(RLC_CNTL) &&
	      first_write(CG_UPLL_FUNC_CNTL) == write_log_count &&
	      first_write(CG_UPLL_FUNC_CNTL_2) == write_log_count &&
	      first_write(UVD_SOFT_RESET) == write_log_count &&
	      first_write(UVD_RBC_RB_BASE) == write_log_count && !delays,
	      "preparation stops after windows and RLC before PLL, reset or ring startup");
	before_reads = reads; before_writes = writes; before_delays = delays;
	check(prepare_rlc_model(uvd, accel, fw, rlc) < 0 &&
	      rdn_uvd_init_rlc(uvd, accel, 2700, fw, FW_SIZE, 10,
		VCPU_OFFSET, RING_OFFSET, RING_BYTES, 0, rlc, sizeof(rlc)) < 0 &&
	      reads == before_reads && writes == before_writes && delays == before_delays,
	      "duplicate preparation or combined init cannot overwrite prepared storage");
	check(rdn_uvd_wake(uvd) < 0 && rdn_uvd_idle(uvd) < 0 &&
	      rdn_uvd_ib_submit(uvd, VRAM_BASE + 0x200000, 2, &seq) < 0 &&
	      reads == before_reads && writes == before_writes && delays == before_delays,
	      "prepared instances reject clock and command APIs without MMIO");
	/* The caller initializes CP between the two UVD calls. */
	boundary = write_log_count;
	reg_write(0xc150, 0); /* CP_PFP_UCODE_ADDR */
	reg_write(0x86d8, 0); /* CP_ME_CNTL */
	mem[VCPU_OFFSET] ^= 0xff;
	mem[VCPU_OFFSET + FW_SIZE + 16] = 0x6d;
	r = rdn_uvd_start_prepared(uvd);
	check(!r && uvd->ready && !uvd->prepared && !uvd->failed,
	      "prepared VCPU starts once after the caller's CP initialization boundary");
	check(first_write(RLC_CNTL) < boundary && first_write(0xc150) == boundary &&
	      first_write(CG_UPLL_FUNC_CNTL_2) > boundary + 1 &&
	      first_write(UVD_SOFT_RESET) > boundary + 1,
	      "register ordering is windows, RLC, caller CP, then PLL and VCPU");
	untouched = 1;
	for (i = boundary + 2; i < write_log_count; i++)
		if ((write_log[i].reg >= 0x3f00 && write_log[i].reg <= 0x3f4c) ||
		    write_log[i].reg == UVD_VCPU_CACHE_OFFSET0)
			untouched = 0;
	check(untouched && mem[VCPU_OFFSET] == (uint8_t)(fw[0] ^ 0xff) &&
	      mem[VCPU_OFFSET + FW_SIZE + 16] == 0x6d,
	      "starting does not recopy firmware, clear storage, reprogram windows or reload RLC");
	before_reads = reads; before_writes = writes;
	check(rdn_uvd_start_prepared(uvd) < 0 && reads == before_reads && writes == before_writes,
	      "ready instance rejects a second prepared start without MMIO");
	rdn_uvd_fini(uvd);

	reset_model(uvd);
	check(!prepare_rlc_model(uvd, accel, fw, rlc), "fresh preparation for cancellation");
	before_reads = reads; before_writes = writes; before_delays = delays;
	rdn_uvd_fini(uvd);
	check(!uvd->prepared && uvd->stopped && uvd->started && !uvd->ready &&
	      !rdn_uvd_storage_releasable(uvd) && regs[RLC_CNTL / 4] == RLC_ENABLE &&
	      reads == before_reads && writes == before_writes && delays == before_delays,
	      "prepared fini cancels locally without waking, reading or stopping UVD/RLC");
	rdn_uvd_fini(uvd);
	check(rdn_uvd_start_prepared(uvd) < 0 && rdn_uvd_idle(uvd) < 0 &&
	      rdn_uvd_wake(uvd) < 0 && prepare_rlc_model(uvd, accel, fw, rlc) < 0 &&
	      reads == before_reads && writes == before_writes && delays == before_delays,
	      "cancelled preparation cannot restart and repeated fini is inert");

	reset_model(uvd);
	check(!prepare_rlc_model(uvd, accel, fw, rlc), "fresh preparation for mapping checks");
	before_reads = reads; before_writes = writes;
	accel->vram_base += 0x100000000ull;
	check(rdn_uvd_start_prepared(uvd) < 0 && uvd->prepared,
	      "prepared start rejects a changed GPU VRAM mapping");
	accel->vram_base = VRAM_BASE;
	accel->aperture_size = VCPU_OFFSET;
	check(rdn_uvd_start_prepared(uvd) < 0 && uvd->prepared,
	      "prepared start rejects an aperture that lost its storage extent");
	accel->aperture_size = APERTURE;
	accel->aperture++;
	check(rdn_uvd_start_prepared(uvd) < 0 && uvd->prepared,
	      "prepared start rejects a changed CPU aperture mapping");
	accel->aperture--;
	check(reads == before_reads && writes == before_writes,
	      "mapping rejection does not access hardware or consume preparation");
	rdn_uvd_fini(uvd);

	reset_model(uvd);
	bad_rlc_readback = 1;
	check(prepare_rlc_model(uvd, accel, fw, rlc) < 0 && uvd->failed &&
	      uvd->started && !uvd->prepared && !rdn_uvd_storage_releasable(uvd),
	      "failed RLC preparation cannot become startable or release storage");
	before_reads = reads; before_writes = writes;
	rdn_uvd_fini(uvd);
	check(rdn_uvd_start_prepared(uvd) < 0 && reads == before_reads && writes == before_writes,
	      "failed preparation rejects start and fini without MMIO");
	for (i = 0; i < 2; i++) {
		reset_model(uvd);
		check(!prepare_rlc_model(uvd, accel, fw, rlc), "fresh preparation for changed RLC");
		regs[RLC_CNTL / 4] = i ? RLC_ENABLE | 2u : 0;
		before_reads = reads; before_writes = writes; before_delays = delays;
		check(rdn_uvd_start_prepared(uvd) == -5 && !uvd->prepared && uvd->failed &&
		      uvd->started && !uvd->ready && uvd->rlc_cntl_snapshot == regs[RLC_CNTL / 4] &&
		      !rdn_uvd_storage_releasable(uvd) && reads == before_reads + 1 &&
		      writes == before_writes && delays == before_delays,
		      "changed RLC consumes preparation and fails without UVD MMIO or clock access");
		rdn_uvd_fini(uvd);
		check(rdn_uvd_start_prepared(uvd) < 0 && reads == before_reads + 1 &&
		      writes == before_writes && delays == before_delays,
		      "changed RLC failure cannot be retried or stopped through unclocked UVD");
	}
	for (i = 0; i < 3; i++) {
		reset_model(uvd);
		check(!prepare_rlc_model(uvd, accel, fw, rlc), "fresh preparation for start failure");
		no_pll = i == 0; no_vcpu = i == 1; stall_ring = i == 2;
		check(rdn_uvd_start_prepared(uvd) < 0 && !uvd->prepared &&
		      uvd->failed && !uvd->ready && !rdn_uvd_storage_releasable(uvd) && delays < 200000,
		      "PLL/VCPU/ring failure consumes preparation and retains storage with a bounded wait");
		before_reads = reads; before_writes = writes;
		check(rdn_uvd_start_prepared(uvd) < 0 && reads == before_reads && writes == before_writes,
		      "failed start cannot replay the prepared state");
	}
}

static void fence_packet_tests(struct rdn_uvd *uvd, struct rdn_accel *accel,
			       const uint8_t *fw)
{
	uint32_t ib = 0x200000, fence = RING_OFFSET + RING_BYTES;
	uint32_t seq = 0, start, i, before;
	int good;
	static const uint32_t expected[] = {
		0x3da1, 0x200000, 0x3da2, 2, 0x3dbd, 0x01020304,
		0x3bc4, RING_OFFSET + RING_BYTES, 0x3bc5, 0xf,
		0x3bc3, 0, 0x3bc4, 0, 0x3bc5, 0, 0x3bc3, 2
	};

	reset_model(uvd);
	check(!init_model(uvd, accel, fw), "fresh UVD 2.2 fence model");
	reg_write(UVD_GPCOM_VCPU_DATA0, fence);
	reg_write(UVD_GPCOM_VCPU_DATA1, 0x01020304);
	reg_write(UVD_GPCOM_VCPU_CMD, 0);
	check(bad_fence_addresses == 1 && !mem_le32(fence),
	      "independent 40-bit fence model rejects the old sequence-as-address packet");
	reg_write(UVD_GPCOM_VCPU_DATA1, 0);
	reg_write(UVD_GPCOM_VCPU_CMD, 0);
	check(bad_fence_addresses == 2 && !mem_le32(fence),
	      "omitting the fence address high byte cannot alias valid VRAM");
	store_command(ib, RDN_UVD_PACKET0(UVD_CONTEXT_ID, 0));
	store_command(ib + 4, 0x89abcdef);
	uvd->fence_emitted = 0x01020303;
	start = uvd->wptr;
	check(!rdn_uvd_ib_try_submit(uvd, VRAM_BASE + ib, 2, &seq) && seq == 0x01020304 &&
	      !rdn_uvd_fence_wait(uvd, seq, 10) && ib_context_value == 0x89abcdef,
	      "UVD 2.2 writes the context sequence after executing its IB");
	good = uvd->wptr == ((start + 32) & (uvd->ring_words - 1));
	for (i = 0; i < sizeof(expected) / sizeof(expected[0]); i++)
		if (cmd_word(RING_OFFSET + 4 * ((start + i) & (uvd->ring_words - 1))) != expected[i])
			good = 0;
	check(good, "literal 18-dword Turks IB/fence/trap packet matches the hardware ABI");
	for (i = 18; i < 32; i++)
		if (cmd_word(RING_OFFSET + 4 * ((start + i) & (uvd->ring_words - 1))) != 0x3bff)
			good = 0;
	check(good && !unaligned_publications, "fence packet tail contains one-word UVD NOPs through its 16-dword boundary");
	check(last_fence_gpu == VRAM_BASE + fence && last_fence_value == 0x01020304 &&
	      mem[fence] == 4 && mem[fence + 1] == 3 && mem[fence + 2] == 2 && mem[fence + 3] == 1,
	      "40-bit fence target receives exact little-endian sequence bytes");

	reset_model(uvd);
	check(!init_model(uvd, accel, fw), "fresh model for exact fence reservation");
	store_command(ib, RDN_UVD_PACKET0(UVD_CONTEXT_ID, 0));
	store_command(ib + 4, 0x12345678);
	regs[UVD_RBC_RB_RPTR / 4] = (uvd->wptr + 33) & (uvd->ring_words - 1);
	check(!rdn_uvd_ib_submit(uvd, VRAM_BASE + ib, 2, &seq) && !uvd->failed,
	      "exactly 32 available ring words admit the 18-word packet and its padding");
	regs[UVD_RBC_RB_RPTR / 4] = (uvd->wptr + 32) & (uvd->ring_words - 1);
	before = writes;
	start = uvd->wptr;
	seq = 0x55;
	{
		unsigned old_delays = delays;
		uint32_t old_fence = uvd->fence_emitted;
		uint8_t old_ring[RING_BYTES];
		memcpy(old_ring, mem + RING_OFFSET, sizeof(old_ring));
		check(rdn_uvd_ib_try_submit(uvd, VRAM_BASE + ib, 2, &seq) == -16 &&
		      !uvd->failed && uvd->ready && delays == old_delays && writes == before,
		      "nonblocking full ring returns busy without delay or hardware writes");
		check(uvd->wptr == start && uvd->fence_emitted == old_fence && seq == 0x55 &&
		      !memcmp(old_ring, mem + RING_OFFSET, sizeof(old_ring)),
		      "busy preserves exact ring bytes, write pointer and both fence outputs");
	}
	check(rdn_uvd_ib_submit(uvd, VRAM_BASE + ib, 2, &seq) < 0 && uvd->failed &&
	      writes == before && uvd->wptr == start && seq == 0x55,
	      "31 available words cannot partially publish the packet or overwrite unread data with padding");

	reset_model(uvd);
	check(!init_model(uvd, accel, fw), "fresh model for a padded packet crossing the ring end");
	store_command(ib, RDN_UVD_PACKET0(UVD_CONTEXT_ID, 0));
	store_command(ib + 4, 0x76543210);
	uvd->wptr = ring_read = uvd->ring_words - 16;
	regs[UVD_RBC_RB_RPTR / 4] = ring_read;
	check(!rdn_uvd_ib_submit(uvd, VRAM_BASE + ib, 2, &seq) && uvd->wptr == 16 &&
	      !rdn_uvd_fence_wait(uvd, seq, 10) && ib_context_value == 0x76543210 &&
	      !unaligned_publications,
	      "payload and NOP padding wrap without losing a fence or changing byte order");
	good = 1;
	for (i = 2; i < 16; i++)
		if (cmd_word(RING_OFFSET + 4 * i) != 0x3bff)
			good = 0;
	check(good && mem[RING_OFFSET + 8] == (RDN_BIG_ENDIAN ? 0 : 0xff) &&
	      mem[RING_OFFSET + 11] == (RDN_BIG_ENDIAN ? 0xff : 0),
	      "wrapped NOPs retain the exact host-order bytes expected by LMI swap");

	reset_model(uvd);
	check(!init_model(uvd, accel, fw), "fresh model for an incomplete fetch block");
	start = uvd->wptr;
	store_command(RING_OFFSET + start * 4, 0x3dbd);
	store_command(RING_OFFSET + (start + 1) * 4, 0x12345678);
	for (i = 2; i < 16; i++)
		store_command(RING_OFFSET + (start + i) * 4, 0x3bff);
	before = regs[UVD_CONTEXT_ID / 4];
	reg_write(UVD_RBC_RB_WPTR, start + 2);
	check(unaligned_publications == 1 && ring_read == start &&
	      regs[UVD_CONTEXT_ID / 4] == before,
	      "a two-dword publication cannot satisfy the model's complete fetch block");
	reg_write(UVD_RBC_RB_WPTR, start + 16);
	check(ring_read == start + 16 && regs[UVD_CONTEXT_ID / 4] == 0x12345678,
	      "publishing the padded block executes its payload and every one-word NOP");
}

int main(int argc, char **argv)
{
	struct rdn_os os;
	struct rdn_card card;
	struct rdn_accel accel;
	struct rdn_uvd uvd;
	uint8_t fw[FW_SIZE];
	uint32_t fb, vd, dd, seq, i, ib_off = 0x200000;
	int r;

	/* the divider search against Linux's algorithm (run in Python on the same inputs) */
	check(!rdn_uvd_calc_dividers(2700, 53300, 40000, &fb, &vd, &dd) &&
	      fb == 970296 && vd == 3 && dd == 4, "dividers for 533/400 MHz at 27 MHz: fb 970296, 3, 4");
	check(!rdn_uvd_calc_dividers(2700, 10000, 10000, &fb, &vd, &dd) &&
	      fb == 849540 && vd == 14 && dd == 14, "dividers for 100/100 MHz: fb 849540, 14, 14");
	check(rdn_uvd_calc_dividers(0, 10000, 10000, &fb, &vd, &dd) < 0, "no reference clock: error");

	if (argc > 1) {
		/* the PLL reference clock out of the real VBIOS (10 kHz units) */
		FILE *f = fopen(argv[1], "rb");
		static uint8_t bios[1 << 18];
		struct rdn_os vos = { 0 };
		struct rdn_card vcard;
		uint32_t ref = 0;

		vos.mmio_read32 = os_read;
		vos.mmio_write32 = os_write;
		vos.delay_us = os_delay;
		vos.time_ms = os_time;
		vos.log = os_log;
		vos.alloc = os_alloc;
		vos.free = os_free;
		memset(&vcard, 0, sizeof(vcard));
		if (!f || !fread(bios, 1, sizeof(bios), f)) {
			printf("FAIL: cannot read %s\n", argv[1]);
			return 2;
		}
		fclose(f);
		check(!rdn_card_init(&vcard, &vos, bios), "VBIOS accepted");
		check(!rdn_uvd_ref_clock(&vcard, &ref), "FirmwareInfo has a reference clock");
		printf("reference clock %u0 kHz\n", (unsigned)ref);
		check(ref == 2700, "27 MHz");
	}

	mem = calloc(1, APERTURE);
	memset(&os, 0, sizeof(os));
	os.mmio_read32 = os_read;
	os.mmio_write32 = os_write;
	os.delay_us = os_delay;
	os.time_ms = os_time;
	os.log = os_log;
	os.alloc = os_alloc;
	os.free = os_free;
	memset(&card, 0, sizeof(card));
	card.os = &os;
	memset(&accel, 0, sizeof(accel));
	accel.card = &card;
	accel.aperture = (volatile uint32_t *)mem;
	accel.aperture_size = APERTURE;
	accel.vram_base = VRAM_BASE;
	for (i = 0; i < FW_SIZE; i++)
		fw[i] = (uint8_t)(i * 7 + 1);
	validation_tests(&uvd, &accel, fw);
	reset_model(&uvd);

	r = rdn_uvd_init_checked(&uvd, &accel, 2700, fw, FW_SIZE, RDN_UVD_DEFAULT_HANDLES,
			 VCPU_OFFSET, RING_OFFSET, RING_BYTES, 0);
	check(r == 0, "init succeeds (VCPU boots, ring test passes)");
	check(uvd.ready, "engine ready");
	check(!uvd.rlc_loaded && first_write(RLC_CNTL) == write_log_count,
	      "existing initialization does not read or program RLC");
	check(uvd.status_snapshot == 2, "ready status captured while clocked");
	check(memcmp(mem + VCPU_OFFSET, fw, FW_SIZE) == 0, "firmware copied byte for byte");
	check(mem[VCPU_OFFSET + FW_SIZE] == 0 && mem[VCPU_OFFSET + uvd.vcpu_bytes - 1] == 0,
	      "the rest of the VCPU region is zero");
	check(regs[UVD_VCPU_CHIP_ID_REG / 4] == 0x01000011, "chip id of Turks");
	check(regs[UVD_VCPU_CACHE_OFFSET0 / 4] == (VCPU_OFFSET >> 3),
	      "cache 0 at the firmware");
	check(regs[UVD_VCPU_CACHE_SIZE0 / 4] == (4096 + 4096) / 8, "cache 0 size: page aligned firmware + 4");
	check(regs[UVD_VCPU_CACHE_OFFSET1 / 4] == regs[UVD_VCPU_CACHE_OFFSET0 / 4] + regs[UVD_VCPU_CACHE_SIZE0 / 4],
	      "heap follows the firmware");
	check(regs[UVD_LMI_ADDR_EXT / 4] == ((0u << 12) | 0u) && regs[UVD_LMI_EXT40_ADDR / 4] == (0xFu | (0x7u << 16) | (1u << 31)),
	      "address bits 28-31 and 32-39 (ring's segment last)");
	check(regs[UVD_LMI_SWAP_CNTL / 4] == (RDN_BIG_ENDIAN ? 0xa : 0),
	      "ring and IB swapped (0xa) on a big-endian host only");
	check(regs[UVD_MP_SWAP_CNTL / 4] == 0, "firmware/messages/bitstream swap is disabled");
	check(mem[RING_OFFSET + 4] == (RDN_BIG_ENDIAN ? 0xde : 0xef) &&
	      mem[RING_OFFSET + 7] == (RDN_BIG_ENDIAN ? 0xef : 0xde),
	      "ring test word has exact host-order bytes");
	check((regs[CG_UPLL_FUNC_CNTL / 4] & (UPLL_BYPASS_EN_MASK | UPLL_SLEEP_MASK)) ==
	      (UPLL_BYPASS_EN_MASK | UPLL_SLEEP_MASK), "clocks down again after init (PLL bypassed and asleep)");

	/* two indirect buffers, each writes UVD_CONTEXT_ID; the fence follows each */
	r = rdn_uvd_wake(&uvd);
	check(r == 0 && (regs[CG_UPLL_FUNC_CNTL / 4] & UPLL_BYPASS_EN_MASK) == 0 &&
	      regs[CG_UPLL_FUNC_CNTL_3 / 4] == 970296, "wake: PLL out of bypass, fb divider 970296");
	for (i = 0; i < 2; i++) {
		uint32_t w0 = RDN_UVD_PACKET0(UVD_CONTEXT_ID, 0), w1 = 0x1000 + i;

		/* the IB's words are in the ring's byte order */
		if (uvd.swapped) {
			w0 = bswap32(w0);
			w1 = bswap32(w1);
		}
		rdn_vram_write32(&accel, ib_off, w0);
		rdn_vram_write32(&accel, ib_off + 4, w1);
		r = rdn_uvd_ib_submit(&uvd, VRAM_BASE + ib_off, 2, &seq);
		check(r == 0 && seq == i + 1, "IB submitted, fence numbered in turn");
		check(rdn_uvd_fence_wait(&uvd, seq, 10) == 0, "fence reached");
		check(ib_context_value == 0x1000 + i, "the IB ran before the fence updated context");
	}
	check(!rdn_uvd_fence_done(&uvd, 3), "a fence not yet emitted is not done");
	check(!rdn_uvd_fence_done(&uvd, 0) && !rdn_uvd_fence_done(&uvd, 0xffffffffu),
	      "zero and wrapped future fence values are rejected");
	check(mem[RING_OFFSET + RING_BYTES] == 2 &&
	      mem[RING_OFFSET + RING_BYTES + 1] == 0 &&
	      mem[RING_OFFSET + RING_BYTES + 2] == 0 &&
	      mem[RING_OFFSET + RING_BYTES + 3] == 0,
	      "hardware fence bytes remain little endian on both hosts");
	i = writes;
	check(rdn_uvd_ib_submit(&uvd, VRAM_BASE + ib_off + 1, 2, &seq) < 0 &&
	      rdn_uvd_ib_submit(&uvd, VRAM_BASE + 0xfffffffcu, 2, &seq) < 0 &&
	      rdn_uvd_ib_submit(&uvd, VRAM_BASE + ib_off, 0xffffffffu, &seq) < 0 &&
	      rdn_uvd_ib_submit(&uvd, VRAM_BASE + ib_off, 2, NULL) < 0 && writes == i,
	      "misaligned/overflowing/null-output submissions do not touch hardware");
	check(rdn_uvd_fence_wait(&uvd, 2, RDN_UVD_MAX_TIMEOUT_MS + 1) < 0 && !uvd.failed,
	      "excessive timeout rejected without poisoning valid work");
	uvd.fence_emitted = 0xffffffffu;
	check(rdn_uvd_ib_submit(&uvd, VRAM_BASE + ib_off, 2, &seq) < 0 && writes == i,
	      "fence sequence cannot wrap to zero");
	uvd.fence_emitted = 2;
	check(rdn_uvd_ib_submit(&uvd, 0x100000000ull + ib_off, 2, &seq) < 0, "an IB outside the ring's 4 GB segment is refused");
	r = 0;
	for (i = 0; i < 140; i++) {
		store_command(ib_off, RDN_UVD_PACKET0(UVD_CONTEXT_ID, 0));
		store_command(ib_off + 4, 0x12340000u + i);
		if (rdn_uvd_ib_submit(&uvd, VRAM_BASE + ib_off, 2, &seq) ||
		    seq != i + 3 || rdn_uvd_fence_wait(&uvd, seq, 10) ||
		    ib_context_value != 0x12340000u + i)
			r = -1;
	}
	check(!r, "commands and fences survive multiple ring wraps");
	rdn_uvd_fini(&uvd);
	check(!uvd.ready && regs[UVD_VCPU_CNTL / 4] == 0, "stopped: VCPU clock off");
	i = writes;
	rdn_uvd_fini(&uvd);
	check(writes == i && rdn_uvd_wake(&uvd) < 0 && writes == i,
	      "stopped engine cannot be restarted or stopped twice");
	check(!rdn_uvd_storage_releasable(&uvd), "engine storage remains pinned until external quiescence proof");
	failure_tests(&uvd, &accel, fw);
	cache_window_tests(&uvd, &accel);
	rlc_tests(&uvd, &accel, fw);
	split_start_tests(&uvd, &accel, fw);
	fence_packet_tests(&uvd, &accel, fw);

	free(mem);
	printf("%s\n", failures ? "FAIL" : "PASS");
	return failures != 0;
}
