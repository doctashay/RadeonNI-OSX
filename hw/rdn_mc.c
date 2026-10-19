/*
 * The memory controller's microcode: ni_mc_load_microcode() of the Linux
 * radeon driver (ni.c), for Turks.
 *
 * Copyright 2010 Advanced Micro Devices, Inc.
 * Copyright (c) 2026 kouta-kun and Claude
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "rdn_mc.h"
#include "rdn_reg.h"
#include "atom/atom_port.h"

#ifndef ETIMEDOUT
#define ETIMEDOUT 110
#endif

#define MC_SEQ_SUP_CNTL				0x28c8
#define		RUN_MASK			(1 << 0)
#define MC_SEQ_SUP_PGM				0x28cc
#define MC_IO_PAD_CNTL_D0			0x29d0
#define		MEM_FALL_OUT_CMD		(1 << 8)
#define MC_SEQ_MISC0				0x2a00
#define		MC_SEQ_MISC0_GDDR5_SHIFT	28
#define		MC_SEQ_MISC0_GDDR5_MASK		0xf0000000
#define		MC_SEQ_MISC0_GDDR5_VALUE	5
#define MC_SEQ_IO_DEBUG_INDEX			0x2a44
#define MC_SEQ_IO_DEBUG_DATA			0x2a48

#define BTC_IO_MC_REGS_SIZE	29
#define TRAINING_TIMEOUT_US	100000

static const uint32_t turks_io_mc_regs[BTC_IO_MC_REGS_SIZE][2] = {
	{0x00000077, 0xff010100},
	{0x00000078, 0x00000000},
	{0x00000079, 0x00001434},
	{0x0000007a, 0xcc08ec08},
	{0x0000007b, 0x00040000},
	{0x0000007c, 0x000080c0},
	{0x0000007d, 0x09000000},
	{0x0000007e, 0x00210404},
	{0x00000081, 0x08a8e800},
	{0x00000082, 0x00030444},
	{0x00000083, 0x00000000},
	{0x00000085, 0x00000001},
	{0x00000086, 0x00000002},
	{0x00000087, 0x48490000},
	{0x00000088, 0x20244647},
	{0x00000089, 0x00000005},
	{0x0000008b, 0x66030000},
	{0x0000008c, 0x00006603},
	{0x0000008d, 0x00000100},
	{0x0000008f, 0x00001c0a},
	{0x00000090, 0xff000001},
	{0x00000094, 0x00101101},
	{0x00000095, 0x00000fff},
	{0x00000096, 0x00116fff},
	{0x00000097, 0x60010000},
	{0x00000098, 0x10010000},
	{0x00000099, 0x00006000},
	{0x0000009a, 0x00001000},
	{0x0000009f, 0x00936a00}
};

int rdn_mc_load_microcode(struct rdn_card *card, const void *fw, size_t size)
{
	const uint8_t *p = fw;
	uint32_t mem_type, running;
	int i;

	if (!fw || size != RDN_MC_UCODE_WORDS * 4)
		return -EINVAL;

	mem_type = (rdn_rreg(card, MC_SEQ_MISC0) & MC_SEQ_MISC0_GDDR5_MASK) >>
		   MC_SEQ_MISC0_GDDR5_SHIFT;
	running = rdn_rreg(card, MC_SEQ_SUP_CNTL) & RUN_MASK;
	if (mem_type != MC_SEQ_MISC0_GDDR5_VALUE || running)
		return 1;

	/* reset the engine and set to writable */
	rdn_wreg(card, MC_SEQ_SUP_CNTL, 0x00000008);
	rdn_wreg(card, MC_SEQ_SUP_CNTL, 0x00000010);

	/* load mc io regs */
	for (i = 0; i < BTC_IO_MC_REGS_SIZE; i++) {
		rdn_wreg(card, MC_SEQ_IO_DEBUG_INDEX, turks_io_mc_regs[i][0]);
		rdn_wreg(card, MC_SEQ_IO_DEBUG_DATA, turks_io_mc_regs[i][1]);
	}
	/* load the MC ucode: big-endian words in the file */
	for (i = 0; i < RDN_MC_UCODE_WORDS; i++, p += 4)
		rdn_wreg(card, MC_SEQ_SUP_PGM,
			 ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
			 ((uint32_t)p[2] << 8) | p[3]);

	/* put the engine back into the active state */
	rdn_wreg(card, MC_SEQ_SUP_CNTL, 0x00000008);
	rdn_wreg(card, MC_SEQ_SUP_CNTL, 0x00000004);
	rdn_wreg(card, MC_SEQ_SUP_CNTL, 0x00000001);

	/* wait for training to complete */
	for (i = 0; i < TRAINING_TIMEOUT_US; i++) {
		if (rdn_rreg(card, MC_IO_PAD_CNTL_D0) & MEM_FALL_OUT_CMD)
			return 0;
		card->os->delay_us(card->os->cookie, 1);
	}
	return -ETIMEDOUT;
}

/* ---- moving video memory ------------------------------------------------ */

#define MC_RELOC_VGA_RENDER_CONTROL		0x0300
#define MC_RELOC_VGA_HDP_CONTROL		0x0328
#define		MC_RELOC_VGA_MEMORY_DISABLE	(1 << 4)
#define MC_RELOC_VGA_MEMORY_BASE_ADDRESS	0x0310
#define MC_RELOC_VGA_MEMORY_BASE_ADDRESS_HIGH	0x0324
#define MC_RELOC_SRBM_STATUS			0x0e50
#define MC_RELOC_MC_SHARED_BLACKOUT_CNTL	0x20ac
#define		MC_RELOC_BLACKOUT_MODE_MASK	0x7
#define MC_RELOC_BIF_FB_EN			0x5490
#define		MC_RELOC_FB_READ_EN		(1 << 0)
#define		MC_RELOC_FB_WRITE_EN		(1 << 1)
#define MC_RELOC_CONFIG_MEMSIZE			0x5428
#define MC_RELOC_FB_LOCATION			0x2024
#define MC_RELOC_AGP_TOP			0x2028
#define MC_RELOC_AGP_BOT			0x202c
#define MC_RELOC_AGP_BASE			0x2030
#define MC_RELOC_APERTURE_LOW			0x2034
#define MC_RELOC_APERTURE_HIGH			0x2038
#define MC_RELOC_APERTURE_DEFAULT		0x203c
#define MC_RELOC_HDP_NONSURFACE_BASE		0x2c04
#define MC_RELOC_HDP_NONSURFACE_INFO		0x2c08
#define MC_RELOC_HDP_NONSURFACE_SIZE		0x2c0c
/* the display controllers, per CRTC (rdn_reg.h has the offsets) */
#define MC_RELOC_CRTC_CONTROL			0x6e70
#define		MC_RELOC_CRTC_MASTER_EN		(1 << 0)
#define		MC_RELOC_CRTC_READ_REQ_DISABLE	(1 << 24)
#define MC_RELOC_CRTC_UPDATE_LOCK		0x6ed4
#define MC_RELOC_CRTC_FRAME_COUNT		0x6e98
#define MC_RELOC_GRPH_PRIMARY			0x6810
#define MC_RELOC_GRPH_SECONDARY			0x6814
#define MC_RELOC_GRPH_PRIMARY_HIGH		0x681c
#define MC_RELOC_GRPH_SECONDARY_HIGH		0x6820

static const uint32_t reloc_crtc[RDN_NUM_CRTC] = {
	EVERGREEN_CRTC0_REGISTER_OFFSET, EVERGREEN_CRTC1_REGISTER_OFFSET,
	EVERGREEN_CRTC2_REGISTER_OFFSET, EVERGREEN_CRTC3_REGISTER_OFFSET,
	EVERGREEN_CRTC4_REGISTER_OFFSET, EVERGREEN_CRTC5_REGISTER_OFFSET
};

static void reloc_wait_idle(struct rdn_card *card)
{
	unsigned i;

	for (i = 0; i < 100000; i++) {
		if (!(rdn_rreg(card, MC_RELOC_SRBM_STATUS) & 0x1f00))
			return;
		card->os->delay_us(card->os->cookie, 1);
	}
	rdn_log(card->os, RDN_LOG_ERROR, "mc: wait for the memory controller to idle timed out");
}

int rdn_mc_relocate(struct rdn_card *card, uint64_t new_base)
{
	uint32_t old_loc = rdn_rreg(card, MC_RELOC_FB_LOCATION);
	uint64_t old_base = (uint64_t)(old_loc & 0xffff) << 24;
	uint64_t size = (uint64_t)rdn_rreg(card, MC_RELOC_CONFIG_MEMSIZE) << 20;
	uint32_t default_off, vga_render, vga_hdp, blackout, tmp, frames;
	uint64_t new_end;
	unsigned i, j;

	if (!size || (new_base & 0xffffff))
		return -EINVAL;
	if (new_base == old_base)
		return 1;
	new_end = new_base + size - 1;
	default_off = (uint32_t)(((uint64_t)rdn_rreg(card, MC_RELOC_APERTURE_DEFAULT) << 12) - old_base);

	/* evergreen_mc_stop(): VGA render off, blank the display controllers */
	vga_render = rdn_rreg(card, MC_RELOC_VGA_RENDER_CONTROL);
	vga_hdp = rdn_rreg(card, MC_RELOC_VGA_HDP_CONTROL);
	rdn_wreg(card, MC_RELOC_VGA_RENDER_CONTROL, 0);
	for (i = 0; i < RDN_NUM_CRTC; i++) {
		uint32_t off = reloc_crtc[i];

		if (!(rdn_rreg(card, MC_RELOC_CRTC_CONTROL + off) & MC_RELOC_CRTC_MASTER_EN))
			continue;
		tmp = rdn_rreg(card, MC_RELOC_CRTC_CONTROL + off);
		if (!(tmp & MC_RELOC_CRTC_READ_REQ_DISABLE)) {
			rdn_wreg(card, MC_RELOC_CRTC_UPDATE_LOCK + off, 1);
			rdn_wreg(card, MC_RELOC_CRTC_CONTROL + off, tmp | MC_RELOC_CRTC_READ_REQ_DISABLE);
			rdn_wreg(card, MC_RELOC_CRTC_UPDATE_LOCK + off, 0);
		}
		/* wait for the next frame, then switch the controller off */
		frames = rdn_rreg(card, MC_RELOC_CRTC_FRAME_COUNT + off);
		for (j = 0; j < 100000; j++) {
			if (rdn_rreg(card, MC_RELOC_CRTC_FRAME_COUNT + off) != frames)
				break;
			card->os->delay_us(card->os->cookie, 1);
		}
		rdn_wreg(card, MC_RELOC_CRTC_UPDATE_LOCK + off, 1);
		tmp = rdn_rreg(card, MC_RELOC_CRTC_CONTROL + off);
		rdn_wreg(card, MC_RELOC_CRTC_CONTROL + off, tmp & ~MC_RELOC_CRTC_MASTER_EN);
		rdn_wreg(card, MC_RELOC_CRTC_UPDATE_LOCK + off, 0);
	}
	reloc_wait_idle(card);
	blackout = rdn_rreg(card, MC_RELOC_MC_SHARED_BLACKOUT_CNTL);
	if ((blackout & MC_RELOC_BLACKOUT_MODE_MASK) != 1) {
		rdn_wreg(card, MC_RELOC_BIF_FB_EN, 0);			/* block CPU access */
		rdn_wreg(card, MC_RELOC_MC_SHARED_BLACKOUT_CNTL,
			 (blackout & ~MC_RELOC_BLACKOUT_MODE_MASK) | 1);	/* black out the MC */
	}
	card->os->delay_us(card->os->cookie, 100);

	/* evergreen_mc_program(): the new place */
	rdn_wreg(card, MC_RELOC_VGA_HDP_CONTROL, MC_RELOC_VGA_MEMORY_DISABLE);
	rdn_wreg(card, MC_RELOC_APERTURE_LOW, (uint32_t)(new_base >> 12));
	rdn_wreg(card, MC_RELOC_APERTURE_HIGH, (uint32_t)(new_end >> 12));
	rdn_wreg(card, MC_RELOC_APERTURE_DEFAULT, (uint32_t)((new_base + default_off) >> 12));
	rdn_wreg(card, MC_RELOC_FB_LOCATION, (uint32_t)(((new_end >> 24) & 0xffff) << 16) |
		 (uint32_t)((new_base >> 24) & 0xffff));
	rdn_wreg(card, MC_RELOC_HDP_NONSURFACE_BASE, (uint32_t)(new_base >> 8));
	rdn_wreg(card, MC_RELOC_HDP_NONSURFACE_INFO, (2 << 7) | (1u << 30));
	rdn_wreg(card, MC_RELOC_HDP_NONSURFACE_SIZE, 0x3fffffff);
	rdn_wreg(card, MC_RELOC_AGP_BASE, 0);
	rdn_wreg(card, MC_RELOC_AGP_TOP, 0x0fffffff);
	rdn_wreg(card, MC_RELOC_AGP_BOT, 0x0fffffff);
	reloc_wait_idle(card);

	/* evergreen_mc_resume(): the display controllers' surfaces at the new base, then let the MC and the CPU back */
	for (i = 0; i < RDN_NUM_CRTC; i++) {
		rdn_wreg(card, MC_RELOC_GRPH_PRIMARY_HIGH + reloc_crtc[i], (uint32_t)(new_base >> 32));
		rdn_wreg(card, MC_RELOC_GRPH_SECONDARY_HIGH + reloc_crtc[i], (uint32_t)(new_base >> 32));
		rdn_wreg(card, MC_RELOC_GRPH_PRIMARY + reloc_crtc[i], (uint32_t)new_base);
		rdn_wreg(card, MC_RELOC_GRPH_SECONDARY + reloc_crtc[i], (uint32_t)new_base);
	}
	rdn_wreg(card, MC_RELOC_VGA_MEMORY_BASE_ADDRESS_HIGH, (uint32_t)(new_base >> 32));
	rdn_wreg(card, MC_RELOC_VGA_MEMORY_BASE_ADDRESS, (uint32_t)new_base);
	tmp = rdn_rreg(card, MC_RELOC_MC_SHARED_BLACKOUT_CNTL);
	rdn_wreg(card, MC_RELOC_MC_SHARED_BLACKOUT_CNTL, tmp & ~MC_RELOC_BLACKOUT_MODE_MASK);
	rdn_wreg(card, MC_RELOC_BIF_FB_EN, MC_RELOC_FB_READ_EN | MC_RELOC_FB_WRITE_EN);
	rdn_wreg(card, MC_RELOC_VGA_HDP_CONTROL, vga_hdp);
	card->os->delay_us(card->os->cookie, 1000);
	rdn_wreg(card, MC_RELOC_VGA_RENDER_CONTROL, vga_render & 0xfffcffff);	/* rv515_vga_render_disable(): VGA_VSTATUS_CNTL cleared */
	rdn_log(card->os, RDN_LOG_INFO, "mc: video memory moved from 0x%llx to 0x%llx",
		(unsigned long long)old_base, (unsigned long long)new_base);
	return 0;
}

/*
 * Stop (or let go again) the display controllers' reads of video memory, without touching anything else: the
 * test for whether display traffic gets in the way of another block's memory access.
 */
void rdn_display_hold(struct rdn_card *card, bool hold)
{
	unsigned i;

	for (i = 0; i < RDN_NUM_CRTC; i++) {
		uint32_t off = reloc_crtc[i], tmp = rdn_rreg(card, MC_RELOC_CRTC_CONTROL + off);

		if (!(tmp & MC_RELOC_CRTC_MASTER_EN))
			continue;
		rdn_wreg(card, MC_RELOC_CRTC_UPDATE_LOCK + off, 1);
		rdn_wreg(card, MC_RELOC_CRTC_CONTROL + off,
			 hold ? tmp | MC_RELOC_CRTC_READ_REQ_DISABLE : tmp & ~MC_RELOC_CRTC_READ_REQ_DISABLE);
		rdn_wreg(card, MC_RELOC_CRTC_UPDATE_LOCK + off, 0);
	}
}
