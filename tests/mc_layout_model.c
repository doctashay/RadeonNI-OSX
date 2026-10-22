/* Cold layout transaction model; no hardware access. SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include "../hw/rdn_mc_layout.h"

static uint32_t regs[0x20000 / 4], original[0x20000 / 4];
static const uint32_t off[6] = {0, 0xc00, 0x9800, 0xa400, 0xb000, 0xbc00};
static const uint32_t maps[10] = {0x2034,0x2038,0x203c,0x2024,0x2c04,0x2c08,0x2c0c,0x2030,0x2028,0x202c};
static const uint32_t address[4] = {0x681c,0x6820,0x6810,0x6814};
static unsigned checks, failures, writes, reads, delays, illegal, frame[6];
static unsigned mapped, checking, fault_used, rollback_drop, bad_resume, agp_reserved;
static uint32_t fault_reg, drop_reg, drop_value, pci;
static int frozen_vblank, frozen_frame, busy_mc, busy_after_mapping, busy_after_rollback;

static void check(int ok, const char *name)
{
	checks++;
	if (!ok) { failures++; printf("FAIL %u: %s\n", checks, name); }
}

static int mapping_reg(uint32_t r)
{
	unsigned i, j;
	for (i = 0; i < 10; i++) if (r == maps[i]) return 1;
	for (i = 0; i < 6; i++) for (j = 0; j < 4; j++)
		if (r == address[j] + off[i]) return 1;
	return r == 0x310 || r == 0x324;
}

static uint32_t read32(void *cookie, uint32_t r)
{
	unsigned i;
	(void)cookie;
	reads++;
	if ((r & 3) || r >= sizeof(regs)) { illegal++; return 0; }
	for (i = 0; i < 6; i++) {
		if (r == 0x6e8c + off[i]) return frozen_vblank ? 0 : 1;
		if (r == 0x6e98 + off[i]) return frozen_frame ? frame[i] : frame[i]++;
	}
	if (r == 0xe50 && (busy_mc || (busy_after_mapping && mapped) ||
	    (busy_after_rollback && mapped >= 72))) return 0x1f00;
	if (checking && r == fault_reg && !fault_used) {
		fault_used = 1;
		return regs[r / 4] ^ 1;
	}
	return regs[r / 4];
}

static void write32(void *cookie, uint32_t r, uint32_t value)
{
	unsigned i;
	int allowed = mapping_reg(r);
	(void)cookie;
	writes++;
	if ((r & 3) || r >= sizeof(regs)) { illegal++; return; }
	if (mapping_reg(r)) {
		if (regs[0x5490 / 4] || (regs[0x20ac / 4] & 7) != 1) illegal++;
		for (i = 0; i < 6; i++) if (regs[(0x6e70 + off[i]) / 4] & 1) illegal++;
		mapped++;
	}
	for (i = 0; i < 6; i++) {
		if (r == 0x6e70 + off[i]) {
			allowed = 1;
			if (regs[(0x6ed4 + off[i]) / 4] != 1) illegal++;
			if (!(value & 1) && (regs[r / 4] & 1) && !(regs[r / 4] & (1u << 24))) illegal++;
		}
		if (r == 0x6ed4 + off[i]) allowed = 1;
	}
	if (r == 0x300 || r == 0x328 || r == 0x5490 || r == 0x20ac) allowed = 1;
	if (!allowed) illegal++;
	if (r == 0x5490 && value == 3 && (regs[0x20ac / 4] & 7)) bad_resume++;
	if (r == drop_reg && value == drop_value) {
		drop_reg = 0;
		return;
	}
	if (rollback_drop && fault_used && r == 0x2024 && value == 0x0f3f0f00) return;
	if (r == 0x2028 || r == 0x202c || r == 0x2030) {
		if (value & ~0x0003ffffu) agp_reserved++;
		value &= 0x0003ffffu;
	}
	regs[r / 4] = value;
	if (r == 0x310 && regs[0x2024 / 4] == 0x003f0000) checking = 1;
}

static uint32_t cfg_read(void *cookie, uint32_t r)
{
	(void)cookie;
	if (r) illegal++;
	return pci;
}
static void delay(void *cookie, uint32_t us) { (void)cookie; delays += us; }
static struct rdn_os os;
static struct rdn_card card;

static void setup(unsigned active)
{
	unsigned i;
	memset(regs, 0, sizeof(regs));
	memset(&os, 0, sizeof(os)); memset(&card, 0, sizeof(card));
	os.mmio_read32 = read32; os.mmio_write32 = write32;
	os.cfg_read32 = cfg_read; os.delay_us = delay; card.os = &os;
	writes = reads = delays = illegal = mapped = checking = fault_used = 0;
	rollback_drop = bad_resume = fault_reg = drop_reg = drop_value = 0;
	agp_reserved = 0;
	frozen_vblank = frozen_frame = busy_mc = busy_after_mapping = 0;
	busy_after_rollback = 0;
	pci = 0x675d1002;
	regs[0x5428/4] = 1024; regs[0x86d8/4] = 0x14000000;
	regs[0x5490/4] = 3; regs[0x20ac/4] = 0x80;
	regs[0x300/4] = 0x101; regs[0x328/4] = 0x80;
	regs[0x310/4] = 0x100000; regs[0x324/4] = 15;
	regs[0x2034/4] = 0xf00000; regs[0x2038/4] = 0xf3ffff;
	regs[0x203c/4] = 0xf021ff; regs[0x2024/4] = 0x0f3f0f00;
	regs[0x2c04/4] = 0x0f000000; regs[0x2c08/4] = 0x40000100;
	regs[0x2c0c/4] = 0x3fffffff;
	regs[0x2030/4] = 0; regs[0x2028/4] = regs[0x202c/4] = 0x0003ffff;
	regs[0x2c00/4] = 0x5a5a1234; /* Host endian configuration must survive. */
	for (i = 0; i < 6; i++) {
		regs[(0x6e70+off[i])/4] = 0x100 | ((active & (1u<<i)) ? 1 : 0);
		regs[(0x681c+off[i])/4] = regs[(0x6820+off[i])/4] = 15;
		regs[(0x6810+off[i])/4] = 0x100000 + i * 0x10000;
		regs[(0x6814+off[i])/4] = 0x200000 + i * 0x10000;
		frame[i] = 10;
	}
	memcpy(original, regs, sizeof(regs));
}

static int run(struct rdn_mc_layout_result *out)
{
	return rdn_mc_layout_rebase_zero(&card, RDN_MC_LAYOUT_CONDITIONS,
		RDN_MC_LAYOUT_SCRATCH_OFFSET, out);
}

static int restored(void) { return memcmp(regs, original, sizeof(regs)) == 0; }

int main(void)
{
	struct rdn_mc_layout_result out;
	unsigned i, j;
	uint32_t corrupt_regs[] = {0x5428,0x86d8,0x8010,0xec4,0xd000,0x3f00,0x1410,0x1414,0x5490,0x20ac,0x2024,0x2c04};
	uint32_t corrupt_vals[] = {512,0,0x80000000,32,1,1,1,1,0,1,0x001f0000,0};
	setup(63);
	check(!run(&out) && out.stage == RDN_MC_LAYOUT_COMPLETE && out.changed &&
	      out.access_restored && out.modeset_required && !out.rollback_ok && !illegal && !bad_resume,
	      "six-controller cold transaction completes with access restored and modeset required");
	check(out.old_base == 0xf00000000ull && out.new_base == 0 &&
	      regs[0x2024/4] == 0x003f0000 && regs[0x2034/4] == 0 &&
	      regs[0x2038/4] == 0x3ffff && regs[0x203c/4] == 0x21ff &&
	      !regs[0x2c04/4] && regs[0x2c08/4] == 0x40000100 && regs[0x2c0c/4] == 0x3fffffff,
	      "literal 1GiB target windows and reserved scratch page match the intended layout");
	check(!agp_reserved && !regs[0x2030/4] && regs[0x2028/4] == 0x0003ffff &&
	      regs[0x202c/4] == 0x0003ffff,
	      "disabled AGP bounds use only the physically implemented 18-bit fields");
	for (i = 0; i < 6; i++)
		check(!(regs[(0x6e70+off[i])/4] & 1) && !regs[(0x681c+off[i])/4] &&
		      !regs[(0x6820+off[i])/4] && regs[(0x6810+off[i])/4] == 0x100000+i*0x10000 &&
		      regs[(0x6814+off[i])/4] == 0x200000+i*0x10000 && !regs[(0x6ed4+off[i])/4],
		      "each active surface keeps its VRAM offset with its controller disabled");
	check(!regs[0x300/4] && !regs[0x310/4] && !regs[0x324/4] && regs[0x328/4] == 0x80 &&
	      regs[0x5490/4] == 3 && regs[0x20ac/4] == 0x80 && regs[0x2c00/4] == 0x5a5a1234,
	      "VGA addresses, CPU access and host-endian state have the required final values");
	setup(0);
	check(!run(&out) && !illegal && delays == 100, "no-display path needs no vblank and only the blackout settling delay");
	setup(1);
	check(rdn_mc_layout_rebase_zero(&card, 3, RDN_MC_LAYOUT_SCRATCH_OFFSET, &out) < 0 && !writes,
	      "missing permanent scratch reservation rejects before writes");
	check(rdn_mc_layout_rebase_zero(&card, 6, RDN_MC_LAYOUT_SCRATCH_OFFSET, &out) < 0 && !reads && !writes,
	      "CPU aperture writer quiescence is mandatory before even reading hardware");
	check(rdn_mc_layout_rebase_zero(&card, 5, RDN_MC_LAYOUT_SCRATCH_OFFSET, &out) < 0 && !reads && !writes,
	      "absence of GPU clients is mandatory before even reading hardware");
	check(rdn_mc_layout_rebase_zero(&card, 7, 0, &out) < 0 && !writes,
	      "framebuffer zero cannot become the system-aperture default");
	check(rdn_mc_layout_rebase_zero(&card, 15, RDN_MC_LAYOUT_SCRATCH_OFFSET, &out) < 0 && !writes,
	      "unknown precondition flags reject before writes");
	pci = 0x67581002;
	check(run(&out) < 0 && !writes, "other ASIC identity is outside the first diagnostic scope");
	for (i = 0; i < sizeof(corrupt_regs)/sizeof(corrupt_regs[0]); i++) {
		setup(1); regs[corrupt_regs[i]/4] = corrupt_vals[i];
		check(run(&out) < 0 && !writes && !delays, "unsupported memory or inherited engine state rejects before mutation");
	}
	for (i = 0; i < 6; i++) {
		setup(1); regs[(0x730c+off[i])/4] = 1;
		check(run(&out) < 0 && !writes, "active DP stream on every frontend rejects before mutation");
		setup(1); regs[(0x6998+off[i])/4] = 1;
		check(run(&out) < 0 && !writes, "inherited cursor on every controller rejects before mutation");
	}
	setup(1); regs[0x730c/4] = 0x10000;
	check(run(&out) < 0 && !writes && out.failed_reg == 0x730c,
	      "DP stream still draining is rejected even after its enable bit cleared");
	setup(1); regs[0x681c/4] = 0;
	check(run(&out) < 0 && !writes, "active surface outside old VRAM rejects before mutation");
	setup(1); regs[0x6814/4] |= 1;
	check(run(&out) < 0 && !writes, "unsupported secondary surface flag rejects before mutation");
	for (i = 0; i < 3; i++) {
		setup(63); frozen_vblank = i == 0; frozen_frame = i == 1; busy_mc = i == 2;
		check(run(&out) == -ETIMEDOUT && restored() && out.rollback_ok && out.access_restored &&
		      !out.changed && !out.modeset_required && delays <= 100000 && !illegal,
		      "vblank, frame and MC-idle timeouts are bounded and restore the original scanout");
	}
	/* Inject one failed readback at every mapping register, including every pipe. */
	for (i = 0; i < 10 + 24 + 2; i++) {
		setup(63);
		fault_reg = i < 10 ? maps[i] : i < 34 ? address[(i-10)%4] + off[(i-10)/4] : i == 34 ? 0x310 : 0x324;
		check(run(&out) == -EIO && fault_used && restored() && out.rollback_ok && out.access_restored &&
		      !out.changed && !illegal && !bad_resume,
		      "mapping readback failure rolls every address and control back before restoring access");
	}
	for (i = 0; i < 5; i++) {
		static const uint32_t rr[] = {0x300,0x6e70,0x5490,0x20ac,0x328};
		static const uint32_t vv[] = {0,0x01000101,0,0x81,0x90};
		setup(1); drop_reg = rr[i]; drop_value = vv[i];
		check(run(&out) == -EIO && restored() && out.rollback_ok && out.access_restored && !illegal,
		      "failed display-stop or blackout write restores original state");
	}
	setup(1); drop_reg = 0x2024; drop_value = 0x003f0000;
	check(run(&out) == -EIO && restored() && out.rollback_ok && out.access_restored && !illegal,
	      "a lost MC-base write cannot leave the newly programmed HDP and display mapping active");
	setup(1); fault_reg = 0x2024; rollback_drop = 1;
	check(run(&out) == -EIO && fault_used && !out.rollback_ok && !out.access_restored && out.changed &&
	      !regs[0x5490/4] && (regs[0x20ac/4]&7) == 1 && !(regs[0x6e70/4]&1) && !illegal,
	      "unverifiable mapping rollback leaves CPU and display access blocked");
	setup(1); drop_reg = 0x5490; drop_value = 3;
	check(run(&out) == -EIO && restored() && out.rollback_ok && out.access_restored && !illegal && !bad_resume,
	      "CPU access resume failure reblocks and rolls back before retrying original access");
	setup(1); busy_after_mapping = 1;
	check(run(&out) == -ETIMEDOUT && !out.rollback_ok && !out.access_restored && out.changed &&
	      !regs[0x5490/4] && (regs[0x20ac/4]&7) == 1 && mapped == 36 &&
	      delays <= 200200 && !illegal,
	      "MC activity after programming prevents unsafe rollback writes and keeps access blocked");
	setup(1); fault_reg = 0x2024; busy_after_rollback = 1;
	check(run(&out) == -ETIMEDOUT && !out.rollback_ok && !out.access_restored && !out.changed &&
	      !regs[0x5490/4] && (regs[0x20ac/4]&7) == 1 && mapped == 72 &&
	      delays <= 100200 && !illegal,
	      "restored mapping must reach MC idle before CPU or scanout access can resume");
	/* The only writes permitted by the model are layout, display stop and access controls. */
	for (j = 0; j < 6; j++) { setup(1u << j); check(!run(&out) && !illegal, "single-controller placement works for all six pipes"); }
	printf("%s: %u cold-layout checks\n", failures ? "FAIL" : "PASS", checks);
	return failures ? 1 : 0;
}
