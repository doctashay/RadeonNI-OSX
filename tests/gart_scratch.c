/* Reserved GART fault page and resume invariants. SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../hw/rdn_gart.h"

#define APERTURE (8u << 20)
#define TABLE (2u << 20)
#define SCRATCH 0x1000u
static unsigned checks, failures, writes;
static uint32_t regs[0x20000 / 4];

static void check(int good, const char *name)
{
	checks++;
	if (!good) { failures++; printf("FAIL %u: %s\n", checks, name); }
}
static uint32_t read_reg(void *cookie, uint32_t offset)
{
	(void)cookie;
	/* VM_CONTEXT0_REQUEST_RESPONSE: the TLB request completed. */
	if (offset == 0x1470) return 0x10;
	return regs[offset / 4];
}
static void write_reg(void *cookie, uint32_t offset, uint32_t value)
{ (void)cookie; writes++; regs[offset / 4] = value; }
static void delay(void *cookie, uint32_t us) { (void)cookie; (void)us; }
static void log_message(void *cookie, enum rdn_log_level level, const char *fmt, va_list ap)
{ (void)cookie; (void)level; (void)fmt; (void)ap; }

static int unchanged(const uint8_t *data, uint8_t value, uint32_t start, uint32_t length)
{
	uint32_t i;
	for (i = start; i < start + length; i++) if (data[i] != value) return 0;
	return 1;
}

int main(void)
{
	struct rdn_os os = {0}; struct rdn_card card = {0};
	struct rdn_accel accel = {0}; struct rdn_gart gart = {0};
	uint8_t *mem = malloc(APERTURE), *saved = malloc(RDN_GART_TABLE_BYTES);
	uint32_t i, before, bad[] = {TABLE, TABLE + 4096, TABLE + RDN_GART_TABLE_BYTES - 4096,
		SCRATCH + 1, APERTURE, 0xfffff000};
	if (!mem || !saved) return 2;
	os.mmio_read32 = read_reg; os.mmio_write32 = write_reg;
	os.delay_us = delay; os.log = log_message; card.os = &os;
	accel.card = &card; accel.aperture = (volatile uint32_t *)mem;
	accel.aperture_size = APERTURE; accel.vram_size = 1ull << 30;
	for (i = 0; i < 2; i++) {
		accel.vram_base = i ? 0xf00000000ull : 0;
		memset(mem, 0xa5, APERTURE); memset(mem + SCRATCH, 0, 4096);
		check(!rdn_gart_enable_with_scratch(&gart, &accel, TABLE, 0x12345000, SCRATCH) &&
		      gart.ready && gart.default_offset == SCRATCH && gart.separate_default == 1 &&
		      regs[0x203c / 4] == (uint32_t)((accel.vram_base + SCRATCH) >> 12),
		      "explicit default uses the reserved VRAM page at both low and high GPU bases");
		check(unchanged(mem, 0, SCRATCH, 4096) && unchanged(mem, 0xa5, 0, SCRATCH) &&
		      unchanged(mem, 0xa5, SCRATCH + 4096, TABLE - SCRATCH - 4096) &&
		      unchanged(mem, 0xa5, TABLE + RDN_GART_TABLE_BYTES, APERTURE - TABLE - RDN_GART_TABLE_BYTES),
		      "table initialization preserves the reserved fault page and every surrounding guard");
		check(mem[TABLE] == 0x67 && mem[TABLE+1] == 0x50 && mem[TABLE+2] == 0x34 &&
		      mem[TABLE+3] == 0x12 && !mem[TABLE+4] && !mem[TABLE+5] && !mem[TABLE+6] && !mem[TABLE+7],
		      "PTE bytes remain little endian without changing the existing translation format");
		rdn_gart_set_page(&gart, 17, 0x87654000); memcpy(saved, mem + TABLE, RDN_GART_TABLE_BYTES);
		rdn_gart_disable(&gart); regs[0x203c / 4] = 0xdeadbeef;
		check(!rdn_gart_resume(&gart) && gart.ready &&
		      regs[0x203c / 4] == (uint32_t)((accel.vram_base + SCRATCH) >> 12) &&
		      !memcmp(saved, mem + TABLE, RDN_GART_TABLE_BYTES) && unchanged(mem, 0, SCRATCH, 4096),
		      "resume restores the reserved default without replacing live PTEs or scratch bytes");
	}
	accel.vram_base = 0;
	for (i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) {
		memset(mem, 0xa5, APERTURE); before = writes;
		check(rdn_gart_enable_with_scratch(&gart, &accel, TABLE, 0x1000, bad[i]) < 0 &&
		      writes == before && unchanged(mem, 0xa5, 0, APERTURE),
		      "overlap, misalignment and out-of-aperture default pages reject before table or MMIO writes");
	}
	before = writes;
	check(rdn_gart_enable_with_scratch(NULL, &accel, TABLE, 0x1000, SCRATCH) < 0 &&
	      rdn_gart_enable_with_scratch(&gart, NULL, TABLE, 0x1000, SCRATCH) < 0 && writes == before,
	      "invalid owner pointers reject without access");
	accel.vram_size = TABLE + RDN_GART_TABLE_BYTES;
	check(rdn_gart_enable_with_scratch(&gart, &accel, TABLE, 0x1000, APERTURE - 4096) < 0 && writes == before,
	      "a CPU-mapped scratch page beyond physical VRAM is rejected");
	accel.vram_size = (1ull << 40); accel.vram_base = 4096;
	check(rdn_gart_enable_with_scratch(&gart, &accel, TABLE, 0x1000, SCRATCH) < 0 && writes == before,
	      "GPU address overflow is rejected before initialization");
	accel.vram_size = 1ull << 30; accel.vram_base = 1;
	check(rdn_gart_enable_with_scratch(&gart, &accel, TABLE, 0x1000, SCRATCH) < 0 && writes == before,
	      "unaligned GPU VRAM base cannot truncate the default-page address");
	accel.vram_base = 0;
	check(rdn_gart_enable_with_scratch(&gart, &accel, 0xfff00000, 0x1000, SCRATCH) < 0 && writes == before,
	      "table extent overflow cannot wrap into the CPU aperture");
	check(!rdn_gart_enable_with_scratch(&gart, &accel, TABLE, 0x1000, TABLE - 4096),
	      "scratch page ending exactly at the table boundary is disjoint");
	check(!rdn_gart_enable_with_scratch(&gart, &accel, TABLE, 0x1000, TABLE + RDN_GART_TABLE_BYTES),
	      "scratch page starting exactly after the table is disjoint");
	check(!rdn_gart_enable_with_scratch(&gart, &accel, TABLE, 0x1000, APERTURE - 4096),
	      "last complete CPU aperture page is accepted");
	rdn_gart_disable(&gart); before = writes; gart.default_offset = TABLE;
	check(rdn_gart_resume(&gart) < 0 && writes == before && !gart.ready,
	      "resume rejects a corrupted saved default that now overlaps the table");
	gart.default_offset = APERTURE;
	check(rdn_gart_resume(&gart) < 0 && writes == before,
	      "resume revalidates the whole saved default page");
	gart.default_offset = SCRATCH; accel.aperture_size = SCRATCH;
	check(rdn_gart_resume(&gart) < 0 && writes == before,
	      "resume rejects a reduced aperture before register programming");
	accel.aperture_size = APERTURE;
	check(!rdn_gart_enable(&gart, &accel, TABLE, 0x1000) && !gart.separate_default &&
	      regs[0x203c/4] == TABLE / 4096,
	      "legacy enable retains its existing table-as-default behavior");
	rdn_gart_set_page(&gart, 7, 0x98765000); memcpy(saved, mem + TABLE, RDN_GART_TABLE_BYTES);
	rdn_gart_disable(&gart); gart.default_offset = 0xdeadbeef;
	check(!rdn_gart_resume(&gart) && regs[0x203c/4] == TABLE/4096 &&
	      !memcmp(saved, mem + TABLE, RDN_GART_TABLE_BYTES),
	      "legacy resume preserves mappings and does not consult the separate-default field");
	free(saved); free(mem);
	printf("%s: %u GART scratch checks\n", failures ? "FAIL" : "PASS", checks);
	return failures ? 1 : 0;
}
