/*
 * Cold address relocation following Linux evergreen_mc_stop/program/resume.
 * The first diagnostic supports only the HD 7570's 1 GiB DVI/TMDS layout.
 * SPDX-License-Identifier: MIT
 */
#ifdef KERNEL
#include <sys/errno.h>
#include <libkern/libkern.h>
#else
#include <errno.h>
#include <string.h>
#endif
#include "rdn_mc_layout.h"
#include "rdn_reg.h"

#define OLD_BASE 0xf00000000ull
#define VRAM_BYTES 0x40000000u
#define WAIT_US 100000u
#define CRTC_READ_DISABLE (1u << 24)
#define CRTC_STATUS 0x6e8cu
#define CRTC_FRAME 0x6e98u
#define CRTC_LOCK 0x6ed4u
#define GRPH_UPDATE 0x6844u
#define MASTER_LOCK 0x6ef4u
#define CUR_CONTROL 0x6998u
#define DP_STREAM 0x730cu
#define VGA_RENDER 0x300u
#define VGA_HDP 0x328u
#define VGA_DISABLE (1u << 4)
#define VGA_BASE 0x310u
#define VGA_HIGH 0x324u
#define BIF_FB 0x5490u
#define BLACKOUT 0x20acu
#define CP_CONTROL 0x86d8u
#define CP_HALTED ((1u << 26) | (1u << 28))
#define AGP_BOUND_MAX 0x0003ffffu

static const uint32_t offsets[6] = {0, 0xc00, 0x9800, 0xa400, 0xb000, 0xbc00};
/* SYS low/high/default, FB, HDP base/info/size, disabled AGP base/top/bottom. */
static const uint32_t layout_regs[10] = {
	0x2034, 0x2038, 0x203c, 0x2024, 0x2c04,
	0x2c08, 0x2c0c, 0x2030, 0x2028, 0x202c
};
static const uint32_t address_regs[4] = {0x681c, 0x6820, 0x6810, 0x6814};

struct layout_save {
	uint32_t layout[10], addresses[6][4], control[6];
	uint32_t vga_render, vga_hdp, vga_low, vga_high, bif, blackout;
};

static uint32_t rd(struct rdn_card *c, uint32_t r) { return rdn_rreg(c, r); }
static void wr(struct rdn_card *c, uint32_t r, uint32_t v) { rdn_wreg(c, r, v); }

static int equal(struct rdn_card *c, struct rdn_mc_layout_result *out,
		 uint32_t reg, uint32_t value, uint32_t mask)
{
	uint32_t actual = rd(c, reg);
	if ((actual & mask) == (value & mask))
		return 1;
	if (!out->failed_reg) {
		out->failed_reg = reg;
		out->expected = value;
		out->actual = actual;
	}
	return 0;
}

static int write_check(struct rdn_card *c, struct rdn_mc_layout_result *out,
		       uint32_t reg, uint32_t value)
{
	wr(c, reg, value);
	return equal(c, out, reg, value, UINT32_MAX);
}

static int wait_bits(struct rdn_card *c, uint32_t reg, uint32_t mask, uint32_t value)
{
	uint32_t i;
	for (i = 0; i < WAIT_US; i++) {
		if ((rd(c, reg) & mask) == value)
			return 0;
		c->os->delay_us(c->os->cookie, 1);
	}
	return -ETIMEDOUT;
}

static int next_frame(struct rdn_card *c, uint32_t offset)
{
	uint32_t i, first = rd(c, CRTC_FRAME + offset);
	for (i = 0; i < WAIT_US; i++) {
		if (rd(c, CRTC_FRAME + offset) != first)
			return 0;
		c->os->delay_us(c->os->cookie, 1);
	}
	return -ETIMEDOUT;
}

static int set_control(struct rdn_card *c, struct rdn_mc_layout_result *out,
		       uint32_t offset, uint32_t value)
{
	int ok;
	if (!write_check(c, out, CRTC_LOCK + offset, 1))
		return 0;
	wr(c, EVERGREEN_CRTC_CONTROL + offset, value);
	wr(c, CRTC_LOCK + offset, 0);
	ok = equal(c, out, CRTC_LOCK + offset, 0, UINT32_MAX);
	return equal(c, out, EVERGREEN_CRTC_CONTROL + offset, value, UINT32_MAX) && ok;
}

static int restore_display(struct rdn_card *c, const struct layout_save *s,
			   struct rdn_mc_layout_result *out)
{
	unsigned i;
	int ok = 1;
	for (i = 0; i < 6; i++)
		if (!set_control(c, out, offsets[i], s->control[i]))
			ok = 0;
	if (!write_check(c, out, VGA_HDP, s->vga_hdp))
		ok = 0;
	if (!write_check(c, out, VGA_RENDER, s->vga_render))
		ok = 0;
	return ok;
}

static void write_mapping(struct rdn_card *c, const struct layout_save *s)
{
	unsigned i, j;
	for (i = 0; i < 10; i++)
		wr(c, layout_regs[i], s->layout[i]);
	for (i = 0; i < 6; i++)
		for (j = 0; j < 4; j++)
			wr(c, address_regs[j] + offsets[i], s->addresses[i][j]);
	wr(c, VGA_HIGH, s->vga_high);
	wr(c, VGA_BASE, s->vga_low);
}

static int check_mapping(struct rdn_card *c, const struct layout_save *s,
			 struct rdn_mc_layout_result *out)
{
	unsigned i, j;
	int ok = 1;
	for (i = 0; i < 10; i++)
		if (!equal(c, out, layout_regs[i], s->layout[i], UINT32_MAX))
			ok = 0;
	for (i = 0; i < 6; i++)
		for (j = 0; j < 4; j++)
			if (!equal(c, out, address_regs[j] + offsets[i], s->addresses[i][j], UINT32_MAX))
				ok = 0;
	if (!equal(c, out, VGA_HIGH, s->vga_high, UINT32_MAX) ||
	    !equal(c, out, VGA_BASE, s->vga_low, UINT32_MAX))
		ok = 0;
	return ok;
}

static int restore_access(struct rdn_card *c, const struct layout_save *s,
			  struct rdn_mc_layout_result *out)
{
	if (!write_check(c, out, BLACKOUT, s->blackout))
		return 0;
	if (!write_check(c, out, BIF_FB, s->bif)) {
		wr(c, BIF_FB, 0);
		wr(c, BLACKOUT, (s->blackout & ~7u) | 1u);
		return 0;
	}
	out->access_restored = 1;
	return 1;
}

int rdn_mc_layout_rebase_zero(struct rdn_card *c, uint32_t conditions,
			    uint32_t scratch_offset,
			    struct rdn_mc_layout_result *out)
{
	struct layout_save old, next;
	unsigned i, j;
	uint64_t end, primary, secondary;
	int r = -EINVAL, blackout_attempted = 0, mapping_written = 0;

	if (!out)
		return -EINVAL;
	memset(out, 0, sizeof(*out));
	out->access_restored = 1;
	if (!c || !c->os || !c->os->mmio_read32 || !c->os->mmio_write32 ||
	    !c->os->cfg_read32 || !c->os->delay_us ||
	    conditions != RDN_MC_LAYOUT_CONDITIONS ||
	    scratch_offset != RDN_MC_LAYOUT_SCRATCH_OFFSET ||
	    (scratch_offset & 4095) || scratch_offset > VRAM_BYTES - 4096)
		return -EINVAL;
	if (c->os->cfg_read32(c->os->cookie, 0) != 0x675d1002u ||
	    !equal(c, out, 0x5428, 1024, UINT32_MAX))
		return -EINVAL;
	for (i = 0; i < 10; i++)
		old.layout[i] = rd(c, layout_regs[i]);
	out->old_base = (uint64_t)(old.layout[3] & 0xffff) << 24;
	end = ((uint64_t)(old.layout[3] >> 16) << 24) | 0xffffff;
	if (out->old_base != OLD_BASE || end != OLD_BASE + VRAM_BYTES - 1) {
		out->failed_reg = 0x2024;
		out->expected = 0x0f3f0f00;
		out->actual = old.layout[3];
		return -EINVAL;
	}
	if (!equal(c, out, 0x2c04, (uint32_t)(OLD_BASE >> 8), UINT32_MAX) ||
	    !equal(c, out, CP_CONTROL, CP_HALTED, CP_HALTED) ||
	    !equal(c, out, GRBM_STATUS, 0, 0x80000000u) ||
	    !equal(c, out, SRBM_STATUS2, 0, DMA_BUSY) ||
	    !equal(c, out, 0xd000, 0, 1) || !equal(c, out, 0x3f00, 0, UINT32_MAX) ||
	    !equal(c, out, 0x1410, 0, 1) || !equal(c, out, 0x1414, 0, 1))
		return -EBUSY;
	old.bif = rd(c, BIF_FB);
	old.blackout = rd(c, BLACKOUT);
	if (old.bif != 3 || (old.blackout & 7))
		return -EBUSY;
	old.vga_render = rd(c, VGA_RENDER);
	old.vga_hdp = rd(c, VGA_HDP);
	old.vga_low = rd(c, VGA_BASE);
	old.vga_high = rd(c, VGA_HIGH);
	for (i = 0; i < 6; i++) {
		uint32_t o = offsets[i];
		old.control[i] = rd(c, EVERGREEN_CRTC_CONTROL + o);
		/* DP rollback needs link setup; leave that outside this first trial. */
		if (!equal(c, out, DP_STREAM + o, 0, 0x10001) ||
		    !equal(c, out, CUR_CONTROL + o, 0, 1) ||
		    !equal(c, out, CRTC_LOCK + o, 0, UINT32_MAX) ||
		    !equal(c, out, MASTER_LOCK + o, 0, UINT32_MAX) ||
		    !equal(c, out, GRPH_UPDATE + o, 0, (1u << 16) | 4))
			return -EBUSY;
		for (j = 0; j < 4; j++)
			old.addresses[i][j] = rd(c, address_regs[j] + o);
		primary = ((uint64_t)old.addresses[i][0] << 32) | old.addresses[i][2];
		secondary = ((uint64_t)old.addresses[i][1] << 32) | old.addresses[i][3];
		if ((old.control[i] & EVERGREEN_CRTC_MASTER_EN) &&
		    ((primary & 255) || (secondary & 255) || primary < OLD_BASE ||
		     primary > end || secondary < OLD_BASE || secondary > end))
			return -EINVAL;
	}
	next = old;
	next.layout[0] = 0;
	next.layout[1] = (VRAM_BYTES - 1) >> 12;
	next.layout[2] = scratch_offset >> 12;
	next.layout[3] = 0x003f0000;
	next.layout[4] = 0;
	next.layout[5] = (2u << 7) | (1u << 30);
	next.layout[6] = 0x3fffffff;
	next.layout[7] = 0;
	/* AGP bounds implement 18 bits; reserved write bits do not read back. */
	next.layout[8] = next.layout[9] = AGP_BOUND_MAX;
	for (i = 0; i < 6; i++) {
		if (old.control[i] & EVERGREEN_CRTC_MASTER_EN) {
			next.addresses[i][2] = (uint32_t)((((uint64_t)old.addresses[i][0] << 32) |
				old.addresses[i][2]) - OLD_BASE);
			next.addresses[i][3] = (uint32_t)((((uint64_t)old.addresses[i][1] << 32) |
				old.addresses[i][3]) - OLD_BASE);
		} else {
			next.addresses[i][2] = next.addresses[i][3] = 0;
		}
		next.addresses[i][0] = next.addresses[i][1] = 0;
	}
	next.vga_high = next.vga_low = 0;
	out->stage = RDN_MC_LAYOUT_DISPLAY_STOP;
	out->modeset_required = 1;
	if (!write_check(c, out, VGA_RENDER, 0)) { r = -EIO; goto rollback; }
	for (i = 0; i < 6; i++) {
		uint32_t control = old.control[i], o = offsets[i];
		if (!(control & EVERGREEN_CRTC_MASTER_EN))
			continue;
		if (!(control & CRTC_READ_DISABLE)) {
			r = wait_bits(c, CRTC_STATUS + o, 1, 1);
			if (r) goto rollback;
			control |= CRTC_READ_DISABLE;
			if (!set_control(c, out, o, control)) { r = -EIO; goto rollback; }
		}
		r = next_frame(c, o);
		if (r) goto rollback;
		if (!set_control(c, out, o, control & ~EVERGREEN_CRTC_MASTER_EN)) {
			r = -EIO; goto rollback;
		}
	}
	out->stage = RDN_MC_LAYOUT_IDLE;
	r = wait_bits(c, SRBM_STATUS, 0x1f00, 0);
	if (r) goto rollback;
	out->stage = RDN_MC_LAYOUT_BLACKOUT;
	blackout_attempted = 1;
	out->access_restored = 0;
	if (!write_check(c, out, BIF_FB, 0) ||
	    !write_check(c, out, BLACKOUT, (old.blackout & ~7u) | 1u)) {
		r = -EIO; goto rollback;
	}
	c->os->delay_us(c->os->cookie, 100);
	r = wait_bits(c, SRBM_STATUS, 0x1f00, 0);
	if (r) goto rollback;
	if (!write_check(c, out, VGA_HDP, old.vga_hdp | VGA_DISABLE)) {
		r = -EIO; goto rollback;
	}
	out->stage = RDN_MC_LAYOUT_PROGRAM;
	out->changed = 1;
	mapping_written = 1;
	write_mapping(c, &next);
	if (!check_mapping(c, &next, out)) { r = -EIO; goto rollback; }
	r = wait_bits(c, SRBM_STATUS, 0x1f00, 0);
	if (r) goto rollback;
	out->stage = RDN_MC_LAYOUT_RESTORE;
	if (!restore_access(c, &old, out)) { r = -EIO; goto rollback; }
	/* Like Linux, keep VGA rendering and every CRTC off until the next modeset. */
	if (!write_check(c, out, VGA_HDP, old.vga_hdp)) { r = -EIO; goto rollback; }
	out->stage = RDN_MC_LAYOUT_COMPLETE;
	return 0;

rollback:
	if (mapping_written) {
		/* Reblock before rollback even if the failure occurred during resume. */
		out->access_restored = 0;
		if (!write_check(c, out, BIF_FB, 0) ||
		    !write_check(c, out, BLACKOUT, (old.blackout & ~7u) | 1u))
			return -EIO;
		c->os->delay_us(c->os->cookie, 100);
		if (wait_bits(c, SRBM_STATUS, 0x1f00, 0))
			return -ETIMEDOUT;
		write_mapping(c, &old);
		if (!check_mapping(c, &old, out))
			return -EIO;
		out->changed = 0;
		if (wait_bits(c, SRBM_STATUS, 0x1f00, 0))
			return -ETIMEDOUT;
	}
	if (blackout_attempted && !restore_access(c, &old, out))
		return -EIO;
	if (!restore_display(c, &old, out))
		return -EIO;
	out->rollback_ok = 1;
	out->modeset_required = 0;
	return r;
}
