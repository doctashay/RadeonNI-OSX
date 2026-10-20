/*
 * UVD bring-up for Turks. Ported from the MIT-licensed Linux radeon files
 * uvd_v1_0.c, uvd_v2_2.c, radeon_uvd.c and evergreen.c; the register
 * sequences are theirs, the structure is ours.
 *
 * Copyright 2010 Jerome Glisse
 * Copyright 2013 Advanced Micro Devices, Inc.
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
 *
 * Authors: Christian König, Jerome Glisse
 */

#include <string.h>

#include "rdn_uvd.h"
#include "rdn_uvd_reg.h"
#include "atom/atombios.h"

/* Linux's usec_timeout. */
#define RDN_UVD_USEC_TIMEOUT	100000

/* VCPU_CHIP_ID for Turks (uvd_v2_2.c). */
#define RDN_UVD_CHIP_ID_TURKS	0x01000011

/* The kernel has no <errno.h>; the values are the usual ones. */
#ifndef EIO
#define EIO 5
#endif
#ifndef ENOMEM
#define ENOMEM 12
#endif
#ifndef EBUSY
#define EBUSY 16
#endif
#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef ETIMEDOUT
#define ETIMEDOUT 60
#endif

#define PAGE_UP(x)		(((x) + 4095u) & ~4095u)

static uint32_t rd(struct rdn_card *card, uint32_t reg)
{
	return rdn_rreg(card, reg);
}

static void wr(struct rdn_card *card, uint32_t reg, uint32_t val)
{
	rdn_wreg(card, reg, val);
}

/* Linux's WREG32_P: keep the bits of `mask`, set the others from `val`. */
static void wr_p(struct rdn_card *card, uint32_t reg, uint32_t val, uint32_t mask)
{
	uint32_t tmp = rd(card, reg);

	tmp &= mask;
	tmp |= val & ~mask;
	wr(card, reg, tmp);
}

static void mdelay(struct rdn_card *card, uint32_t ms)
{
	card->os->delay_us(card->os->cookie, ms * 1000);
}

static uint32_t bswap32(uint32_t v)
{
	return (v << 24) | ((v & 0xff00) << 8) | ((v >> 8) & 0xff00) | (v >> 24);
}

/* radeon_atom_get_clock_info(): FirmwareInfo's reference clock, 10 kHz units. */
int rdn_uvd_ref_clock(struct rdn_card *card, uint32_t *ref_freq)
{
	struct atom_context *ctx = card->atom.ctx;
	uint16_t size = 0, offset = 0;
	const uint8_t *info;

	if (!ctx || !atom_parse_data_header(ctx, GetIndexIntoMasterTable(DATA, FirmwareInfo),
					    &size, NULL, NULL, &offset) ||
	    size < offsetof(ATOM_FIRMWARE_INFO, usReferenceClock) + 2)
		return -EINVAL;
	info = (const uint8_t *)ctx->bios + offset;
	*ref_freq = info[offsetof(ATOM_FIRMWARE_INFO, usReferenceClock)] |
		    (info[offsetof(ATOM_FIRMWARE_INFO, usReferenceClock) + 1] << 8);
	return *ref_freq ? 0 : -EINVAL;
}

uint32_t rdn_uvd_vcpu_bytes(uint32_t fw_size, uint32_t handles)
{
	return PAGE_UP(fw_size + 8) + RDN_UVD_STACK_SIZE + RDN_UVD_HEAP_SIZE +
	       RDN_UVD_SESSION_SIZE * handles;
}

/* ---- clocks ------------------------------------------------------------ */

static unsigned calc_post_div(unsigned vco_freq, unsigned target_freq,
			      unsigned pd_min, unsigned pd_even)
{
	unsigned post_div = vco_freq / target_freq;

	/* adjust to post divider minimum value */
	if (post_div < pd_min)
		post_div = pd_min;
	/* we always need a frequency less than or equal the target */
	if ((vco_freq / post_div) > target_freq)
		post_div += 1;
	/* post dividers above a certain value must be even */
	if (post_div > pd_even && post_div % 2)
		post_div += 1;
	return post_div;
}

int rdn_uvd_calc_dividers(uint32_t ref_freq, uint32_t vclk, uint32_t dclk,
			  uint32_t *fb_div, uint32_t *vclk_div,
			  uint32_t *dclk_div)
{
	/* The Evergreen limits of evergreen_set_uvd_clocks(). */
	const unsigned vco_max = 250000, fb_factor = 16384, pd_min = 0,
		       pd_max = 128, pd_even = 5;
	const uint32_t fb_mask = 0x03FFFFFF;
	unsigned vco_min = 125000, vco_freq;
	unsigned optimal_score = ~0u;

	if (!ref_freq || !vclk || !dclk)
		return -EINVAL;
	if (vclk > vco_min)
		vco_min = vclk;
	if (dclk > vco_min)
		vco_min = dclk;
	for (vco_freq = vco_min; vco_freq <= vco_max; vco_freq += 100) {
		uint64_t fb = (uint64_t)vco_freq * fb_factor / ref_freq;
		unsigned vdiv, ddiv, score;

		if (fb > fb_mask)
			break;
		fb &= fb_mask;
		vdiv = calc_post_div(vco_freq, vclk, pd_min, pd_even);
		if (vdiv > pd_max)
			break;
		ddiv = calc_post_div(vco_freq, dclk, pd_min, pd_even);
		if (ddiv > pd_max)
			break;
		score = vclk - (vco_freq / vdiv) + dclk - (vco_freq / ddiv);
		if (score < optimal_score) {
			*fb_div = (uint32_t)fb;
			*vclk_div = vdiv;
			*dclk_div = ddiv;
			optimal_score = score;
			if (optimal_score == 0)
				break;
		}
	}
	return optimal_score == ~0u ? -EINVAL : 0;
}

static int send_upll_ctlreq(struct rdn_card *card, uint32_t reg)
{
	unsigned i;

	/* make sure UPLL_CTLREQ is deasserted */
	wr_p(card, reg, 0, ~UPLL_CTLREQ_MASK);
	mdelay(card, 10);
	/* assert UPLL_CTLREQ */
	wr_p(card, reg, UPLL_CTLREQ_MASK, ~UPLL_CTLREQ_MASK);
	/* wait for CTLACK and CTLACK2 to get asserted */
	for (i = 0; i < 100; ++i) {
		uint32_t mask = UPLL_CTLACK_MASK | UPLL_CTLACK2_MASK;

		if ((rd(card, reg) & mask) == mask)
			break;
		mdelay(card, 10);
	}
	/* deassert UPLL_CTLREQ */
	wr_p(card, reg, 0, ~UPLL_CTLREQ_MASK);
	if (i == 100) {
		rdn_log(card->os, RDN_LOG_ERROR, "timeout setting UVD clocks");
		return -ETIMEDOUT;
	}
	return 0;
}

int rdn_uvd_set_clocks(struct rdn_card *card, uint32_t ref_freq,
		       uint32_t vclk, uint32_t dclk)
{
	uint32_t fb_div = 0, vclk_div = 0, dclk_div = 0;
	int r;

	/* bypass vclk and dclk with bclk */
	wr_p(card, CG_UPLL_FUNC_CNTL_2, VCLK_SRC_SEL(1) | DCLK_SRC_SEL(1),
	     ~(VCLK_SRC_SEL_MASK | DCLK_SRC_SEL_MASK));
	/* put PLL in bypass mode */
	wr_p(card, CG_UPLL_FUNC_CNTL, UPLL_BYPASS_EN_MASK, ~UPLL_BYPASS_EN_MASK);

	if (!vclk || !dclk) {
		/* keep the bypass mode, put PLL to sleep */
		wr_p(card, CG_UPLL_FUNC_CNTL, UPLL_SLEEP_MASK, ~UPLL_SLEEP_MASK);
		return 0;
	}
	r = rdn_uvd_calc_dividers(ref_freq, vclk, dclk, &fb_div, &vclk_div,
				  &dclk_div);
	if (r)
		return r;

	/* set VCO_MODE to 1 */
	wr_p(card, CG_UPLL_FUNC_CNTL, UPLL_VCO_MODE_MASK, ~UPLL_VCO_MODE_MASK);
	/* toggle UPLL_SLEEP to 1 then back to 0 */
	wr_p(card, CG_UPLL_FUNC_CNTL, UPLL_SLEEP_MASK, ~UPLL_SLEEP_MASK);
	wr_p(card, CG_UPLL_FUNC_CNTL, 0, ~UPLL_SLEEP_MASK);
	/* deassert UPLL_RESET */
	wr_p(card, CG_UPLL_FUNC_CNTL, 0, ~UPLL_RESET_MASK);
	mdelay(card, 1);

	r = send_upll_ctlreq(card, CG_UPLL_FUNC_CNTL);
	if (r)
		return r;

	/* assert UPLL_RESET again */
	wr_p(card, CG_UPLL_FUNC_CNTL, UPLL_RESET_MASK, ~UPLL_RESET_MASK);
	/* disable spread spectrum */
	wr_p(card, CG_UPLL_SPREAD_SPECTRUM, 0, ~SSEN_MASK);
	/* set feedback divider */
	wr_p(card, CG_UPLL_FUNC_CNTL_3, UPLL_FB_DIV(fb_div), ~UPLL_FB_DIV_MASK);
	/* set ref divider to 0 */
	wr_p(card, CG_UPLL_FUNC_CNTL, 0, ~UPLL_REF_DIV_MASK);

	if (fb_div < 307200)
		wr_p(card, CG_UPLL_FUNC_CNTL_4, 0, ~UPLL_SPARE_ISPARE9);
	else
		wr_p(card, CG_UPLL_FUNC_CNTL_4, UPLL_SPARE_ISPARE9, ~UPLL_SPARE_ISPARE9);

	/* set PDIV_A and PDIV_B */
	wr_p(card, CG_UPLL_FUNC_CNTL_2, UPLL_PDIV_A(vclk_div) | UPLL_PDIV_B(dclk_div),
	     ~(UPLL_PDIV_A_MASK | UPLL_PDIV_B_MASK));

	/* give the PLL some time to settle */
	mdelay(card, 15);
	/* deassert PLL_RESET */
	wr_p(card, CG_UPLL_FUNC_CNTL, 0, ~UPLL_RESET_MASK);
	mdelay(card, 15);
	/* switch from bypass mode to normal mode */
	wr_p(card, CG_UPLL_FUNC_CNTL, 0, ~UPLL_BYPASS_EN_MASK);

	r = send_upll_ctlreq(card, CG_UPLL_FUNC_CNTL);
	if (r)
		return r;

	/* switch VCLK and DCLK selection */
	wr_p(card, CG_UPLL_FUNC_CNTL_2, VCLK_SRC_SEL(2) | DCLK_SRC_SEL(2),
	     ~(VCLK_SRC_SEL_MASK | DCLK_SRC_SEL_MASK));
	mdelay(card, 100);
	return 0;
}

/* What the block is told for a place: its GPU address, or (a variant) the offset in video memory. */
static uint64_t uvd_addr(const struct rdn_uvd *uvd, uint64_t gpu)
{
	return ((uvd->flags & RDN_UVD_F_ADDR_OFFSET) && uvd->in_vram) ? gpu - uvd->accel->vram_base : gpu;
}

/* Little-endian words in the memory the block shares, whatever the host. */
static void put_le32(volatile uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_le32(volatile uint8_t *p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- video memory through the register window --------------------------- */

#define MM_INDEX_HI		0x18
#define MM_INDEX_FB		0x80000000u	/* in MM_INDEX: the address is in video memory, not a register */

static void window_index(struct rdn_card *card, uint32_t at)
{
	wr(card, RDN_MMIO_INDEX, at | MM_INDEX_FB);
}

static void window_write(struct rdn_card *card, uint32_t at, uint32_t value)
{
	window_index(card, at);
	wr(card, RDN_MMIO_DATA, value);
}

static uint32_t window_read(struct rdn_card *card, uint32_t at)
{
	window_index(card, at);
	return rd(card, RDN_MMIO_DATA);
}

/* Linux's radeon_ttm_vram_read() and _write() do the same for the debugfs file "radeon_vram". */
int rdn_vram_put(struct rdn_card *card, uint32_t offset, const void *src, uint32_t bytes)
{
	const uint8_t *p = (const uint8_t *)src;
	uint32_t end = offset + bytes, at;

	if (!bytes)
		return 0;
	if (end < offset || end > 0x7ffffffcu)
		return -EINVAL;
	wr(card, MM_INDEX_HI, 0);
	for (at = offset & ~3u; at < end; at += 4) {
		uint32_t v = 0, i;

		if (at < offset || at + 4 > end)
			v = window_read(card, at);
		for (i = 0; i < 4; i++) {
			uint32_t b = at + i;

			if (b >= offset && b < end)
				v = (v & ~(0xffu << (8 * i))) | ((uint32_t)p[b - offset] << (8 * i));
		}
		window_write(card, at, v);
	}
	/* a read comes back after the writes before it */
	(void)window_read(card, (end - 1) & ~3u);
	return 0;
}

int rdn_vram_get(struct rdn_card *card, uint32_t offset, void *dst, uint32_t bytes)
{
	uint8_t *p = (uint8_t *)dst;
	uint32_t end = offset + bytes, at;

	if (!bytes)
		return 0;
	if (end < offset || end > 0x7ffffffcu)
		return -EINVAL;
	wr(card, MM_INDEX_HI, 0);
	for (at = offset & ~3u; at < end; at += 4) {
		uint32_t v = window_read(card, at), i;

		for (i = 0; i < 4; i++) {
			uint32_t b = at + i;

			if (b >= offset && b < end)
				p[b - offset] = (uint8_t)(v >> (8 * i));
		}
	}
	return 0;
}

/*
 * Experiment (docs/UVD.md): write the regions through the aperture's CPU mapping instead of the register window where they lie in it
 * (segment 7 of the kext, a test). The first start after a boot works with the window, a restart does not, and a restart in the aperture does.
 */
static bool push_via_aperture;

/*
 * Experiment: a restart whose firmware window was written by anything but the CPU through the aperture never runs the firmware (the window
 * stays as the file has it, no 40 bytes patched, though the ring works). A write through the aperture followed by the HDP flush may make the
 * decoder drop what it has cached: the kext gives a page of the aperture to write zeros to before the VCPU is released.
 */
static uint32_t coherency_page;
static bool coherency_on;

void rdn_uvd_set_coherency_page(bool on, uint32_t aperture_offset)
{
	coherency_on = on;
	coherency_page = aperture_offset;
}

static void coherency_touch(struct rdn_uvd *uvd)
{
	struct rdn_accel *accel = uvd->accel;
	volatile uint8_t *p;
	uint32_t i;

	if (!coherency_on || !accel->aperture || (uint64_t)coherency_page + 4096 > accel->aperture_size)
		return;
	p = (volatile uint8_t *)accel->aperture + coherency_page;
	for (i = 0; i < 4096; i++)
		p[i] = 0;
	rdn_hdp_flush(accel);
	(void)p[0];
}

/* A better way to write big regions of video memory than the register window, if the caller has one (the kext: the command processor's DMA). */
static int (*put_hook)(void *cookie, uint32_t offset, const void *src, uint32_t bytes);
static void *put_hook_cookie;

void rdn_uvd_set_put_hook(int (*hook)(void *cookie, uint32_t offset, const void *src, uint32_t bytes), void *cookie)
{
	put_hook = hook;
	put_hook_cookie = cookie;
}

void rdn_uvd_set_push_via_aperture(bool on)
{
	push_via_aperture = on;
}

static int uvd_put(struct rdn_uvd *uvd, uint32_t offset, const void *src, uint32_t bytes)
{
	struct rdn_accel *accel = uvd->accel;

	if (push_via_aperture && accel->aperture && (uint64_t)offset + bytes <= accel->aperture_size) {
		volatile uint8_t *to = (volatile uint8_t *)accel->aperture + offset;
		const uint8_t *from = (const uint8_t *)src;
		uint32_t i;

		for (i = 0; i < bytes; i++)
			to[i] = from[i];
		rdn_hdp_flush(accel);
		return 0;
	}
	if (put_hook && bytes >= 4096)
		return put_hook(put_hook_cookie, offset, src, bytes);
	return rdn_vram_put(accel->card, offset, src, bytes);
}

/* ---- the ring ---------------------------------------------------------- */

static void ring_put(struct rdn_uvd *uvd, uint32_t value)
{
	if (uvd->swapped)
		value = bswap32(value);
	put_le32(uvd->ring_cpu + uvd->wptr * 4, value);
	uvd->wptr = (uvd->wptr + 1) & (uvd->ring_words - 1);
}

static int ring_begin(struct rdn_uvd *uvd, uint32_t words)
{
	struct rdn_card *card = uvd->accel->card;
	uint32_t i;

	if (words >= uvd->ring_words)
		return -ENOMEM;
	for (i = 0; i < RDN_UVD_USEC_TIMEOUT; i++) {
		uint32_t rptr = rd(card, UVD_RBC_RB_RPTR) & (uvd->ring_words - 1);
		uint32_t used = (uvd->wptr - rptr) & (uvd->ring_words - 1);

		if (uvd->ring_words - 1 - used >= words) {
			uvd->ring_mark = uvd->wptr;
			return 0;
		}
		card->os->delay_us(card->os->cookie, 1);
	}
	return -EBUSY;
}

/* A ring with no CPU view of its memory (windowed): the words written since ring_begin() go to video memory. */
static void ring_push(struct rdn_uvd *uvd)
{
	struct rdn_card *card = uvd->accel->card;
	uint32_t from = uvd->ring_mark, to = uvd->wptr;

	if (!uvd->windowed || from == to)
		return;
	(void)card;
	if (to > from) {
		uvd_put(uvd, uvd->ring_offset + from * 4, (const uint8_t *)uvd->ring_cpu + from * 4, (to - from) * 4);
	} else {
		uvd_put(uvd, uvd->ring_offset + from * 4, (const uint8_t *)uvd->ring_cpu + from * 4,
			(uvd->ring_words - from) * 4);
		if (to)
			uvd_put(uvd, uvd->ring_offset, (const uint8_t *)uvd->ring_cpu, to * 4);
	}
	uvd->ring_mark = to;
}

static void ring_commit(struct rdn_uvd *uvd)
{
	struct rdn_card *card = uvd->accel->card;

	/* radeon pads a submission to 16 dwords with no-ops (ring->align_mask); the controller fetches in such blocks */
	while (uvd->wptr & 15)
		ring_put(uvd, RDN_UVD_PACKET0(UVD_NO_OP, 0));
	ring_push(uvd);
	rdn_hdp_flush(uvd->accel);
	wr(card, UVD_RBC_RB_WPTR, uvd->wptr);
	(void)rd(card, UVD_RBC_RB_WPTR);
}

static int ring_test(struct rdn_uvd *uvd)
{
	struct rdn_card *card = uvd->accel->card;
	uint32_t tmp = 0;
	unsigned i;
	int r;

	wr(card, UVD_CONTEXT_ID, 0xCAFEDEAD);
	r = ring_begin(uvd, 2);
	if (r)
		return r;
	ring_put(uvd, RDN_UVD_PACKET0(UVD_CONTEXT_ID, 0));
	ring_put(uvd, 0xDEADBEEF);
	ring_commit(uvd);
	for (i = 0; i < RDN_UVD_USEC_TIMEOUT; i++) {
		tmp = rd(card, UVD_CONTEXT_ID);
		if (tmp == 0xDEADBEEF)
			break;
		card->os->delay_us(card->os->cookie, 1);
	}
	if (i >= RDN_UVD_USEC_TIMEOUT) {
		rdn_log(card->os, RDN_LOG_ERROR,
			"uvd ring test failed (UVD_CONTEXT_ID=0x%08X)", (unsigned)tmp);
		return -EINVAL;
	}
	rdn_log(card->os, RDN_LOG_INFO, "uvd ring test succeeded in %u usecs", i);
	return 0;
}

/*
 * The RLC's microcode (BTC_rlc.bin, 768 big-endian words), as evergreen_rlc_resume() loads it. The card keeps it across a
 * warm reboot, so a card that has been through other drivers works with RLC_CNTL alone and a card that lost power does
 * not: set it (rdn_uvd_set_rlc_ucode) and the start loads it first. Not copied: the caller keeps it alive.
 */
static const uint8_t *rlc_ucode;
static uint32_t rlc_ucode_size;

static unsigned reset_level;

/*
 * UVD_LMI_ADDR_EXT holds the 256 MB segment (address bits 28 to 31) in four nibbles, as far as is known: bits 0 to 3 for the VCPU,
 * 4 to 7 and 8 to 11 for other clients of the memory interface (the decoder's own reads and writes of bitstream and pictures,
 * presumably), bits 12 to 15 for the ring. Linux sets the first and the last and only uses segment 0, where the others do not
 * matter. Which nibbles are set (bit i of the mask for nibble i): all four unless a test says otherwise.
 */
static unsigned ext_nibbles = 0xf;

void rdn_uvd_set_ext_nibbles(unsigned mask)
{
	ext_nibbles = mask & 0xf;
}

/*
 * Split mode (docs/UVD.md): the firmware window, ring, messages and feedback stay where the CPU writes them properly (the aperture), and
 * the decoder's own buffers (reference pictures, output, and what else its other clients read) are in a segment beyond it: nibbles 1 and 2
 * of UVD_LMI_ADDR_EXT name that one. -1: the same segment as the firmware.
 */
static int decode_segment = -1;

void rdn_uvd_set_decode_segment(int segment)
{
	decode_segment = segment;
}

static uint32_t ext_value(uint32_t segment)
{
	uint32_t v = 0, i;

	for (i = 0; i < 4; i++)
		if (ext_nibbles & (1u << i)) {
			uint32_t n = (decode_segment >= 0 && (i == 1 || i == 2)) ? (uint32_t)decode_segment : segment;

			v |= (n & 0xf) << (4 * i);
		}
	return v;
}

void rdn_uvd_set_reset_level(unsigned level)
{
	reset_level = level;
}

void rdn_uvd_set_rlc_ucode(const void *fw, uint32_t size)
{
	rlc_ucode = size == 768 * 4 ? fw : NULL;
	rlc_ucode_size = rlc_ucode ? size : 0;
}

static void rlc_load(struct rdn_card *card)
{
	static const uint32_t zero_regs[] = { 0x3f0c, 0x3f10, 0x3f20, 0x3f1c, 0x3f14, 0x3f18, 0x3f44, 0x3f48 };
	unsigned k;

	wr(card, 0x3f00, 0);				/* RLC_CNTL: stop */
	for (k = 0; k < sizeof(zero_regs) / sizeof(zero_regs[0]); k++)
		wr(card, zero_regs[k], 0);
	for (k = 0; k < rlc_ucode_size / 4; k++) {
		wr(card, 0x3f2c, k);			/* RLC_UCODE_ADDR */
		wr(card, 0x3f30, ((uint32_t)rlc_ucode[k * 4] << 24) | ((uint32_t)rlc_ucode[k * 4 + 1] << 16) |
		   ((uint32_t)rlc_ucode[k * 4 + 2] << 8) | rlc_ucode[k * 4 + 3]);	/* RLC_UCODE_DATA */
	}
	wr(card, 0x3f2c, 0);
}

/* ---- start and stop ---------------------------------------------------- */

/* uvd_v2_2_resume(): tell the UVD memory controller where things are. */
static void program_memory(struct rdn_uvd *uvd)
{
	struct rdn_card *card = uvd->accel->card;

	if (uvd->flags & RDN_UVD_F_MINIMAL) {
		/*
		 * What a cold-start bisection of radeon's register stream left (docs/JOURNAL.md 2026-10-18): the
		 * first memory window and the chip id; the firmware's address is below 4 GB (VRAM sits at 0).
		 */
		uint64_t at = uvd_addr(uvd, uvd->vcpu_gpu);

		wr(card, UVD_VCPU_CACHE_OFFSET0, (uint32_t)(at >> 3) & 0x0fffffff);
		wr(card, UVD_VCPU_CACHE_SIZE0, PAGE_UP(uvd->fw_size + 4) >> 3);
		wr(card, UVD_VCPU_CHIP_ID_REG, RDN_UVD_CHIP_ID_TURKS);
		if (uvd->flags & RDN_UVD_F_WINDOWS) {
			/* the heap, then the stack and the sessions, follow the firmware (radeon: uvd_v2_2_resume()) */
			uint32_t off = (uint32_t)(at >> 3) & 0x0fffffff, size = PAGE_UP(uvd->fw_size + 4) >> 3;

			off += size;
			size = RDN_UVD_HEAP_SIZE >> 3;
			wr(card, UVD_VCPU_CACHE_OFFSET1, off);
			wr(card, UVD_VCPU_CACHE_SIZE1, size);
			off += size;
			size = (RDN_UVD_STACK_SIZE + RDN_UVD_SESSION_SIZE * uvd->handles) >> 3;
			wr(card, UVD_VCPU_CACHE_OFFSET2, off);
			wr(card, UVD_VCPU_CACHE_SIZE2, size);
		}
		/*
		 * The segment is always written: a block started again in the aperture after it ran in a segment would keep the
		 * segment's value (the registers survive a restart) and fetch its firmware from the wrong place.
		 */
		wr(card, UVD_LMI_ADDR_EXT, ext_value((uint32_t)(at >> 28)));
		if (uvd->flags & RDN_UVD_F_EXT)
			wr(card, UVD_LMI_EXT40_ADDR, (uint32_t)((at >> 32) & 0xFF) | (0x9 << 16) | (0x1u << 31));
		return;
	}
	uint64_t gpu = uvd_addr(uvd, uvd->vcpu_gpu);
	bool v1 = uvd->flags & RDN_UVD_F_V1_MEM;
	/* uvd_v1_0_resume() starts 16 units (128 bytes) in and sizes the firmware without the +4 */
	uint64_t addr = (gpu >> 3) + (v1 ? 16 : 0);
	uint32_t size = PAGE_UP(v1 ? uvd->fw_size : uvd->fw_size + 4) >> 3;

	/* the VCPU memory controller bits 0-27 */
	wr(card, v1 ? UVD_R6XX_VCPU_CACHE_OFFSET0 : UVD_VCPU_CACHE_OFFSET0, (uint32_t)addr);
	wr(card, v1 ? UVD_R6XX_VCPU_CACHE_SIZE0 : UVD_VCPU_CACHE_SIZE0, size);

	addr += size;
	size = RDN_UVD_HEAP_SIZE >> 3;
	wr(card, v1 ? UVD_R6XX_VCPU_CACHE_OFFSET1 : UVD_VCPU_CACHE_OFFSET1, (uint32_t)addr);
	wr(card, v1 ? UVD_R6XX_VCPU_CACHE_SIZE1 : UVD_VCPU_CACHE_SIZE1, size);

	addr += size;
	size = (RDN_UVD_STACK_SIZE + RDN_UVD_SESSION_SIZE * uvd->handles) >> 3;
	wr(card, v1 ? UVD_R6XX_VCPU_CACHE_OFFSET2 : UVD_VCPU_CACHE_OFFSET2, (uint32_t)addr);
	wr(card, v1 ? UVD_R6XX_VCPU_CACHE_SIZE2 : UVD_VCPU_CACHE_SIZE2, size);

	/* bits 28-31 */
	addr = (gpu >> 28) & 0xF;
	wr(card, UVD_LMI_ADDR_EXT, ext_value((uint32_t)addr));

	/* bits 32-39 */
	addr = (gpu >> 32) & 0xFF;
	wr(card, UVD_LMI_EXT40_ADDR, (uint32_t)(addr | (0x9 << 16) | (0x1u << 31)));

	if (v1) {
		/* the firmware's first word */
		volatile uint8_t *b = uvd->vcpu_cpu;

		wr(card, UVD_FW_START, b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24));
	} else {
		/* tell firmware which hardware it is running on */
		wr(card, UVD_VCPU_CHIP_ID_REG, RDN_UVD_CHIP_ID_TURKS);
	}
}

/* evergreen_gpu_init(): the decoder's tiling follows the 3D engine's */
static void program_tiling(struct rdn_uvd *uvd)
{
	struct rdn_card *card = uvd->accel->card;
	uint32_t cfg = rd(card, 0x98f8);	/* GB_ADDR_CONFIG */

	wr(card, UVD_UDEC_ADDR_CONFIG, cfg);
	wr(card, UVD_UDEC_DB_ADDR_CONFIG, cfg);
	wr(card, UVD_UDEC_DBW_ADDR_CONFIG, cfg);
}

/*
 * The segment, again. After a restart the sub-blocks are held in reset when program_memory() writes the address extension
 * and come out of it later (vcpu_start): a value written to a block in reset may be lost. In the aperture that does not show (segment 0 is
 * the reset value); beyond it the decoder's own reads and writes went to segment 0 and a decode never finished.
 */
static void program_ext(struct rdn_uvd *uvd)
{
	struct rdn_card *card = uvd->accel->card;

	if (reset_level & RDN_UVD_RESET_NO_PROGRAM)
		return;
	if (uvd->flags & RDN_UVD_F_MINIMAL)
		wr(card, UVD_LMI_ADDR_EXT, ext_value((uint32_t)(uvd_addr(uvd, uvd->vcpu_gpu) >> 28)));
}

/* uvd_v1_0_start() */
static int vcpu_start(struct rdn_uvd *uvd)
{
	struct rdn_card *card = uvd->accel->card;
	uint32_t lmi_swap_cntl = 0, mp_swap_cntl = 0;
	uint64_t ring_gpu = uvd_addr(uvd, uvd->ring_gpu);
	uint32_t rb_bufsz, status = 0;
	int i, j, r = 0;

	if (uvd->flags & RDN_UVD_F_MINIMAL) {
		/*
		 * The RLC (evergreen_rlc_start() writes RLC_ENABLE) after the memory window, and time to come up. Not when the block is started
		 * again (level 2): it is running from the first start, and stopping and loading it while the window server is busy breaks the decoder.
		 */
		if (!(reset_level & RDN_UVD_RESET_NO_RLC)) {
			if (rlc_ucode)
				rlc_load(card);
			wr(card, 0x3f00, 1);
			mdelay(card, 300);
		}
		if ((uvd->flags & RDN_UVD_F_LMI) && !(reset_level & RDN_UVD_RESET_NO_PROGRAM)) {
			/* uvd_v1_0_start(): initialize the UVD memory controller and the MPC muxes */
			wr(card, UVD_LMI_CTRL, 0x40 | (1 << 8) | (1 << 13) | (1 << 21) | (1 << 9) | (1 << 20));
			wr(card, UVD_LMI_SWAP_CNTL, uvd->swapped ? 0xa : 0);
			wr(card, UVD_MP_SWAP_CNTL, 0);
			wr(card, UVD_MPC_SET_MUXA0, 0x40c2040);
			wr(card, UVD_MPC_SET_MUXA1, 0x0);
			wr(card, UVD_MPC_SET_MUXB0, 0x40c2040);
			wr(card, UVD_MPC_SET_MUXB1, 0x0);
			wr(card, UVD_MPC_SET_ALU, 0);
			wr(card, UVD_MPC_SET_MUX, 0x88);
		}
		/* after a reset of the sub-blocks they come out of it first, the VCPU last (uvd_v1_0_start()) */
		if (reset_level & RDN_UVD_RESET_SUB) {
			wr(card, UVD_SOFT_RESET, VCPU_SOFT_RESET);
			mdelay(card, 5);
			/*
			 * Everything written while the sub-blocks were in reset (the memory window, the tiling, the memory interface and
			 * the MPC muxes, all done before this point) may have been lost with the reset: once more, now that they are out of it.
			 */
			if (!(reset_level & RDN_UVD_RESET_NO_PROGRAM)) {
				program_tiling(uvd);
				program_memory(uvd);
				if (uvd->flags & RDN_UVD_F_LMI) {
					wr(card, UVD_LMI_CTRL, 0x40 | (1 << 8) | (1 << 13) | (1 << 21) | (1 << 9) | (1 << 20));
					wr(card, UVD_LMI_SWAP_CNTL, uvd->swapped ? 0xa : 0);
					wr(card, UVD_MP_SWAP_CNTL, 0);
					wr(card, UVD_MPC_SET_MUXA0, 0x40c2040);
					wr(card, UVD_MPC_SET_MUXA1, 0x0);
					wr(card, UVD_MPC_SET_MUXB0, 0x40c2040);
					wr(card, UVD_MPC_SET_MUXB1, 0x0);
					wr(card, UVD_MPC_SET_ALU, 0);
					wr(card, UVD_MPC_SET_MUX, 0x88);
				}
			}
		}
		program_ext(uvd);
		coherency_touch(uvd);
		/* clock gating off, VCPU clock on, VCPU out of reset */
		wr(card, UVD_CGC_GATE, 0);
		wr(card, UVD_VCPU_CNTL, 1 << 9);
		wr(card, UVD_SOFT_RESET, 0);
		mdelay(card, 10);
		for (i = 0; i < 100; i++) {
			status = rd(card, UVD_STATUS);
			if (status & 2)
				break;
			mdelay(card, 10);
		}
		if (!(status & 2)) {
			rdn_log(card->os, RDN_LOG_ERROR, "uvd not responding (status 0x%08x)", (unsigned)status);
			return -EIO;
		}
		program_ext(uvd);
		goto ring;
	}

	/* disable clock gating */
	wr(card, UVD_CGC_GATE, 0);
	/* disable interrupt */
	wr_p(card, UVD_MASTINT_EN, 0, ~(1 << 1));
	/* stall UMC and register bus before resetting VCPU */
	wr_p(card, UVD_LMI_CTRL2, 1 << 8, ~(1 << 8));
	wr_p(card, UVD_RB_ARB_CTRL, 1 << 3, ~(1 << 3));
	mdelay(card, 1);

	/* put LMI, VCPU, RBC etc... into reset */
	wr(card, UVD_SOFT_RESET, LMI_SOFT_RESET | VCPU_SOFT_RESET |
	   LBSI_SOFT_RESET | RBC_SOFT_RESET | CSM_SOFT_RESET |
	   CXW_SOFT_RESET | TAP_SOFT_RESET | LMI_UMC_SOFT_RESET);
	mdelay(card, 5);

	/* take UVD block out of reset */
	wr_p(card, SRBM_SOFT_RESET_UVD_REG, 0, ~SOFT_RESET_UVD);
	mdelay(card, 5);

	/* initialize UVD memory controller */
	wr(card, UVD_LMI_CTRL, 0x40 | (1 << 8) | (1 << 13) | (1 << 21) |
	   (1 << 9) | (1 << 20));

	if (uvd->swapped) {
		/* swap (8 in 32) RB and IB */
		lmi_swap_cntl = 0xa;
		mp_swap_cntl = 0;
	}
	wr(card, UVD_LMI_SWAP_CNTL, lmi_swap_cntl);
	wr(card, UVD_MP_SWAP_CNTL, mp_swap_cntl);

	wr(card, UVD_MPC_SET_MUXA0, 0x40c2040);
	wr(card, UVD_MPC_SET_MUXA1, 0x0);
	wr(card, UVD_MPC_SET_MUXB0, 0x40c2040);
	wr(card, UVD_MPC_SET_MUXB1, 0x0);
	wr(card, UVD_MPC_SET_ALU, 0);
	wr(card, UVD_MPC_SET_MUX, 0x88);

	/* take all subblocks out of reset, except VCPU */
	wr(card, UVD_SOFT_RESET, VCPU_SOFT_RESET);
	mdelay(card, 5);

	/* enable VCPU clock */
	wr(card, UVD_VCPU_CNTL, 1 << 9);
	/* enable UMC */
	wr_p(card, UVD_LMI_CTRL2, 0, ~(1 << 8));
	wr_p(card, UVD_RB_ARB_CTRL, 0, ~(1 << 3));

	/* boot up the VCPU */
	wr(card, UVD_SOFT_RESET, 0);
	mdelay(card, 10);

	for (i = 0; i < 10; ++i) {
		for (j = 0; j < 100; ++j) {
			status = rd(card, UVD_STATUS);
			if (status & 2)
				break;
			mdelay(card, 10);
		}
		r = 0;
		if (status & 2)
			break;
		rdn_log(card->os, RDN_LOG_ERROR,
			"uvd not responding (status 0x%08x), resetting the VCPU", (unsigned)status);
		wr_p(card, UVD_SOFT_RESET, VCPU_SOFT_RESET, ~VCPU_SOFT_RESET);
		mdelay(card, 10);
		wr_p(card, UVD_SOFT_RESET, 0, ~VCPU_SOFT_RESET);
		mdelay(card, 10);
		r = -EIO;
	}
	if (r) {
		rdn_log(card->os, RDN_LOG_ERROR, "uvd not responding, giving up");
		return r;
	}

ring:
	/* enable interrupt (the mask only; nothing is routed) */
	wr_p(card, UVD_MASTINT_EN, 3 << 1, ~(3 << 1));
	/* force RBC into idle state */
	wr(card, UVD_RBC_RB_CNTL, 0x11010101);
	/* set the write pointer delay */
	wr(card, UVD_RBC_RB_WPTR_CNTL, 0);
	/* program the 4GB memory segment for rptr and ring buffer */
	wr(card, UVD_LMI_EXT40_ADDR,
	   (uint32_t)((ring_gpu >> 32) | (0x7 << 16) | (0x1u << 31)));
	/* initialize the ring buffer's read and write pointers */
	wr(card, UVD_RBC_RB_RPTR, 0x0);
	uvd->wptr = rd(card, UVD_RBC_RB_RPTR);
	wr(card, UVD_RBC_RB_WPTR, uvd->wptr);
	/* set the ring address */
	wr(card, UVD_RBC_RB_BASE, (uint32_t)ring_gpu);
	/* set ring buffer size (log2 of bytes) */
	for (rb_bufsz = 0; (1u << rb_bufsz) < uvd->ring_words * 4; rb_bufsz++)
		;
	wr_p(card, UVD_RBC_RB_CNTL, (1 << 8) | rb_bufsz, ~0x11f1f);
	return 0;
}

/* uvd_v1_0_stop() */
static void vcpu_stop(struct rdn_uvd *uvd)
{
	struct rdn_card *card = uvd->accel->card;

	/* force RBC into idle state */
	wr(card, UVD_RBC_RB_CNTL, 0x11010101);
	/* stall UMC and register bus before resetting VCPU */
	if (!(reset_level & RDN_UVD_RESET_NO_STALL)) {
		wr_p(card, UVD_LMI_CTRL2, 1 << 8, ~(1 << 8));
		wr_p(card, UVD_RB_ARB_CTRL, 1 << 3, ~(1 << 3));
	}
	mdelay(card, 1);
	/* put VCPU into reset */
	wr(card, UVD_SOFT_RESET, VCPU_SOFT_RESET);
	mdelay(card, 5);
	if (reset_level & RDN_UVD_RESET_SUB) {
		/* every sub-block, with the memory controller and the register bus stalled, as uvd_v1_0_start() does */
		wr(card, UVD_SOFT_RESET, LMI_SOFT_RESET | VCPU_SOFT_RESET | LBSI_SOFT_RESET | RBC_SOFT_RESET |
		   CSM_SOFT_RESET | CXW_SOFT_RESET | TAP_SOFT_RESET | LMI_UMC_SOFT_RESET);
		mdelay(card, 5);
	}
	/* disable VCPU clock */
	wr(card, UVD_VCPU_CNTL, 0x0);
	/* unstall UMC and register bus */
	if (!(reset_level & RDN_UVD_RESET_NO_STALL)) {
		wr_p(card, UVD_LMI_CTRL2, 0, ~(1 << 8));
		wr_p(card, UVD_RB_ARB_CTRL, 0, ~(1 << 3));
	}
}

void rdn_uvd_debug_reset(struct rdn_uvd *uvd, unsigned what)
{
	struct rdn_card *card = uvd->accel->card;

	if (what == 1) {
		wr_p(card, UVD_LMI_CTRL2, 1 << 8, ~(1 << 8));
		wr_p(card, UVD_RB_ARB_CTRL, 1 << 3, ~(1 << 3));
		mdelay(card, 1);
		wr(card, UVD_SOFT_RESET, LMI_SOFT_RESET | VCPU_SOFT_RESET | LBSI_SOFT_RESET | RBC_SOFT_RESET |
		   CSM_SOFT_RESET | CXW_SOFT_RESET | TAP_SOFT_RESET | LMI_UMC_SOFT_RESET);
		mdelay(card, 5);
		wr(card, UVD_SOFT_RESET, 0);
		wr_p(card, UVD_LMI_CTRL2, 0, ~(1 << 8));
		wr_p(card, UVD_RB_ARB_CTRL, 0, ~(1 << 3));
	} else if (what == 2) {
		wr_p(card, SRBM_SOFT_RESET_UVD_REG, SOFT_RESET_UVD, ~SOFT_RESET_UVD);
		mdelay(card, 1);
		wr_p(card, SRBM_SOFT_RESET_UVD_REG, 0, ~SOFT_RESET_UVD);
	}
	uvd->ready = false;
}

int rdn_uvd_wake(struct rdn_uvd *uvd)
{
	if ((uvd->flags & RDN_UVD_F_NO_PLL) ||
	    ((uvd->flags & RDN_UVD_F_MINIMAL) && !(uvd->flags & RDN_UVD_F_PLL)))
		return 0;
	if (uvd->flags & RDN_UVD_F_LOW_CLOCKS)
		return rdn_uvd_set_clocks(uvd->accel->card, uvd->ref_freq, 10000, 10000);
	return rdn_uvd_set_clocks(uvd->accel->card, uvd->ref_freq, RDN_UVD_VCLK,
				  RDN_UVD_DCLK);
}

/* What the block says when the VCPU does not come up (or, from the kext, when a stream's fence does not come). */
void rdn_uvd_dump(struct rdn_uvd *uvd, const char *why)
{
	struct rdn_card *card = uvd->accel->card;
	static const struct { const char *name; uint32_t reg; } r[] = {
		{ "SRBM_STATUS", 0x0E50 }, { "SRBM_STATUS2", 0x0EC4 },
		{ "UVD_STATUS", UVD_STATUS }, { "UVD_SOFT_RESET", UVD_SOFT_RESET },
		{ "UVD_VCPU_CNTL", UVD_VCPU_CNTL }, { "UVD_LMI_CTRL", UVD_LMI_CTRL },
		{ "UVD_LMI_CTRL2", UVD_LMI_CTRL2 }, { "UVD_RB_ARB_CTRL", UVD_RB_ARB_CTRL },
		{ "UVD_MASTINT_EN", UVD_MASTINT_EN }, { "UVD_CGC_GATE", UVD_CGC_GATE },
		{ "UVD_SEMA_TIMEOUT_STATUS", UVD_SEMA_TIMEOUT_STATUS },
		{ "CG_UPLL_FUNC_CNTL", CG_UPLL_FUNC_CNTL },
		{ "CG_UPLL_FUNC_CNTL_2", CG_UPLL_FUNC_CNTL_2 },
		{ "CG_UPLL_FUNC_CNTL_3", CG_UPLL_FUNC_CNTL_3 },
		/* the memory controller: does the block's access fault? */
		{ "MC_VM_FB_LOCATION", 0x2024 }, { "MC_VM_SYSTEM_APERTURE_LOW", 0x2034 },
		{ "MC_VM_SYSTEM_APERTURE_HIGH", 0x2038 }, { "MC_VM_SYSTEM_APERTURE_DEFAULT", 0x203C },
		{ "VM_L2_CNTL", 0x1400 }, { "VM_L2_STATUS", 0x140C }, { "VM_CONTEXT0_CNTL", 0x1410 },
		{ "VM_CONTEXT1_PROTECTION_FAULT_STATUS", 0x14DC },
		{ "VM_CONTEXT1_PROTECTION_FAULT_ADDR", 0x14FC },
		{ "VM_CONTEXT0_PROTECTION_FAULT_DEFAULT_ADDR", 0x1518 },
	};
	unsigned i;

	rdn_log(card->os, RDN_LOG_ERROR, "uvd registers (%s, flags 0x%x):", why, (unsigned)uvd->flags);
	for (i = 0; i < sizeof(r) / sizeof(r[0]); i++)
		rdn_log(card->os, RDN_LOG_ERROR, "  %-24s 0x%04x = 0x%08x", r[i].name,
			(unsigned)r[i].reg, (unsigned)rd(card, r[i].reg));
	for (i = 0xf680; i <= 0xf6f8; i += 4)
		rdn_log(card->os, RDN_LOG_ERROR, "  0x%04x = 0x%08x", i, (unsigned)rd(card, i));
}

int rdn_uvd_idle(struct rdn_uvd *uvd)
{
	if ((uvd->flags & RDN_UVD_F_MINIMAL) && !(uvd->flags & RDN_UVD_F_PLL))
		return 0;
	return rdn_uvd_set_clocks(uvd->accel->card, uvd->ref_freq, 0, 0);
}

/*
 * `window`: the two regions are in video memory the CPU has no view of; vcpu_cpu and ring_cpu are then copies in system
 * memory and the bytes that change go to video memory through the register window. `vcpu_off` and `ring_off` are
 * their offsets in video memory (the aperture's, or beyond it).
 */
static int init_common(struct rdn_uvd *uvd, struct rdn_accel *accel,
		       uint32_t ref_freq, const uint8_t *fw, uint32_t fw_size,
		       uint32_t handles, volatile uint8_t *vcpu_cpu, uint64_t vcpu_gpu,
		       volatile uint8_t *ring_cpu, uint64_t ring_gpu,
		       uint32_t ring_bytes, uint32_t flags, bool window,
		       uint32_t vcpu_off, uint32_t ring_off)
{
	struct rdn_card *card = accel->card;
	uint32_t vcpu_bytes = rdn_uvd_vcpu_bytes(fw_size, handles);
	uint32_t i;
	int r;

	if (!vcpu_cpu || !ring_cpu || !fw || !fw_size || (fw_size & 3) || !handles ||
	    (vcpu_gpu & 4095) || (ring_gpu & 4095) ||
	    ring_bytes < 256 || (ring_bytes & (ring_bytes - 1)))
		return -EINVAL;
	/* the ring and the VCPU region must share the upper address bits */
	if ((vcpu_gpu >> 32) != (ring_gpu >> 32))
		return -EINVAL;
	/* and, for the decoder's own addressing, the 256 MB segment (bits 28 to 31) */
	if (((vcpu_gpu + vcpu_bytes - 1) >> 28) != (vcpu_gpu >> 28) || (vcpu_gpu >> 28) != (ring_gpu >> 28) ||
	    ((ring_gpu + ring_bytes + RDN_UVD_RING_EXTRA - 1) >> 28) != (ring_gpu >> 28))
		return -EINVAL;

	memset(uvd, 0, sizeof(*uvd));
	uvd->windowed = window;
	if (window) {
		uvd->in_vram = true;
		uvd->vcpu_offset = vcpu_off;
		uvd->ring_offset = ring_off;
		/* the segment must be written to the address extension registers */
		flags |= RDN_UVD_F_EXT;
	}
	uvd->accel = accel;
	uvd->ref_freq = ref_freq;
	uvd->handles = handles;
	uvd->fw_size = fw_size;
	uvd->flags = flags;
	uvd->vcpu_cpu = vcpu_cpu;
	uvd->vcpu_gpu = vcpu_gpu;
	uvd->vcpu_bytes = vcpu_bytes;
	uvd->ring_cpu = ring_cpu;
	uvd->ring_gpu = ring_gpu;
	uvd->ring_words = ring_bytes / 4;
	uvd->swapped = RDN_BIG_ENDIAN;

	/* radeon_uvd_resume(): the firmware as in the file, the rest zero */
	if (!(reset_level & RDN_UVD_RESET_NO_REWRITE)) {
		uint32_t x = (flags & RDN_UVD_F_SWAP4) ? 3 : (flags & RDN_UVD_F_SWAP2) ? 2 : 0;

		for (i = 0; i < fw_size; i++)
			vcpu_cpu[i ^ x] = fw[i];
		for (; i < vcpu_bytes; i++)
			vcpu_cpu[i] = 0;
		for (i = 0; i < ring_bytes + RDN_UVD_RING_EXTRA; i++)
			ring_cpu[i] = 0;
	}
	if (window && !(reset_level & RDN_UVD_RESET_NO_REWRITE)) {
		r = uvd_put(uvd, vcpu_off, (const void *)vcpu_cpu, vcpu_bytes);
		if (!r)
			r = uvd_put(uvd, ring_off, (const void *)ring_cpu, ring_bytes + RDN_UVD_RING_EXTRA);
		if (r)
			return r;
	}
	rdn_hdp_flush(accel);
	{
		/* read the firmware back from where the VCPU will fetch it */
		uint32_t bad = 0, first = 0;

		if (window) {
			uint8_t chunk[256];
			uint32_t at, n;

			for (at = 0; at < fw_size; at += n) {
				n = fw_size - at < sizeof(chunk) ? fw_size - at : (uint32_t)sizeof(chunk);
				if (rdn_vram_get(card, vcpu_off + at, chunk, n)) {
					bad = fw_size;
					break;
				}
				for (i = 0; i < n; i++)
					if (chunk[i] != fw[at + i]) {
						if (!bad)
							first = at + i;
						bad++;
					}
			}
		} else {
			for (i = 0; i < fw_size; i++)
				if (vcpu_cpu[i ^ ((flags & RDN_UVD_F_SWAP4) ? 3 : (flags & RDN_UVD_F_SWAP2) ? 2 : 0)] != fw[i]) {
					if (!bad)
						first = i;
					bad++;
				}
		}
		rdn_log(card->os, RDN_LOG_INFO, "uvd: firmware readback: %u of %u bytes differ (first at %u)",
			(unsigned)bad, (unsigned)fw_size, (unsigned)first);
		if (window) {
			/* all of both regions, not only the firmware: the heap, stack and session areas were cleared too */
			uint8_t chunk[256];
			uint32_t at, n, off, wrong = 0, where = 0;

			for (at = 0; at < vcpu_bytes + ring_bytes + RDN_UVD_RING_EXTRA; at += n) {
				const volatile uint8_t *want;

				if (at < vcpu_bytes) {
					n = vcpu_bytes - at < sizeof(chunk) ? vcpu_bytes - at : (uint32_t)sizeof(chunk);
					off = vcpu_off + at;
					want = vcpu_cpu + at;
				} else {
					uint32_t k = at - vcpu_bytes;

					n = ring_bytes + RDN_UVD_RING_EXTRA - k < sizeof(chunk) ? ring_bytes + RDN_UVD_RING_EXTRA - k : (uint32_t)sizeof(chunk);
					off = ring_off + k;
					want = ring_cpu + k;
				}
				if (rdn_vram_get(card, off, chunk, n)) {
					wrong = ~0u;
					break;
				}
				for (i = 0; i < n; i++)
					if (chunk[i] != want[i]) {
						if (!wrong)
							where = at + i;
						wrong++;
					}
			}
			rdn_log(card->os, RDN_LOG_INFO, "uvd: the whole region read back: %u bytes differ (first at %u)", (unsigned)wrong,
				(unsigned)where);
			/* experiment: do the bytes pushed through the window need time to reach the memory the decoder reads? */
			mdelay(card, 500);
		}
	}

	if (window)
		coherency_touch(uvd);
	if (!(reset_level & RDN_UVD_RESET_NO_PROGRAM))
		program_tiling(uvd);
	if (flags & (RDN_UVD_F_PM_GLOBAL | RDN_UVD_F_PM_SCLK)) {
		/* what Linux's trace has set that ours has not (journal 2026-10-17) */
		uvd->pm_general = rd(card, 0x63c);	/* GENERAL_PWRMGT */
		uvd->pm_sclk = rd(card, 0x644);		/* SCLK_PWRMGT_CNTL */
		if (flags & RDN_UVD_F_PM_GLOBAL)
			wr(card, 0x63c, uvd->pm_general | 0x3);
		if (flags & RDN_UVD_F_PM_SCLK)
			wr(card, 0x644, uvd->pm_sclk | 0x4080);
	}
	/* uvd_v2_2_resume() runs before uvd_v1_0_init() raises the clocks */
	if (!(flags & RDN_UVD_F_PLL_FIRST) && !(reset_level & RDN_UVD_RESET_NO_PROGRAM))
		program_memory(uvd);
	/* raise clocks while booting up the VCPU (the minimal start leaves that to the first command stream) */
	r = (flags & RDN_UVD_F_MINIMAL) ? 0 : rdn_uvd_wake(uvd);
	if (r)
		goto done;
	if (flags & RDN_UVD_F_PLL_FIRST)
		program_memory(uvd);
	r = vcpu_start(uvd);
	if (r) {
		rdn_uvd_dump(uvd, "the VCPU did not answer");
		goto done;
	}
	r = ring_test(uvd);
	if (r) {
		rdn_uvd_dump(uvd, "ring test failed");
		goto done;
	}

	r = ring_begin(uvd, 10);
	if (r)
		goto done;
	ring_put(uvd, RDN_UVD_PACKET0(UVD_SEMA_WAIT_FAULT_TIMEOUT_CNTL, 0));
	ring_put(uvd, 0xFFFFF);
	ring_put(uvd, RDN_UVD_PACKET0(UVD_SEMA_WAIT_INCOMPLETE_TIMEOUT_CNTL, 0));
	ring_put(uvd, 0xFFFFF);
	ring_put(uvd, RDN_UVD_PACKET0(UVD_SEMA_SIGNAL_INCOMPLETE_TIMEOUT_CNTL, 0));
	ring_put(uvd, 0xFFFFF);
	/* clear timeout status bits */
	ring_put(uvd, RDN_UVD_PACKET0(UVD_SEMA_TIMEOUT_STATUS, 0));
	ring_put(uvd, 0x8);
	ring_put(uvd, RDN_UVD_PACKET0(UVD_SEMA_CNTL, 0));
	ring_put(uvd, 3);
	ring_commit(uvd);
	uvd->ready = true;
	rdn_log(card->os, RDN_LOG_INFO, "uvd initialized");
done:
	if (r && (flags & (RDN_UVD_F_PM_GLOBAL | RDN_UVD_F_PM_SCLK))) {
		wr(card, 0x63c, uvd->pm_general);
		wr(card, 0x644, uvd->pm_sclk);
	}
	/* lower clocks again */
	if (!(flags & RDN_UVD_F_MINIMAL) && !(r && (flags & RDN_UVD_F_KEEP_CLOCKS)))
		(void)rdn_uvd_idle(uvd);
	return r;
}

int rdn_uvd_init_mem(struct rdn_uvd *uvd, struct rdn_accel *accel,
		     uint32_t ref_freq, const uint8_t *fw, uint32_t fw_size,
		     uint32_t handles, volatile uint8_t *vcpu_cpu, uint64_t vcpu_gpu,
		     volatile uint8_t *ring_cpu, uint64_t ring_gpu,
		     uint32_t ring_bytes, uint32_t flags)
{
	return init_common(uvd, accel, ref_freq, fw, fw_size, handles, vcpu_cpu, vcpu_gpu, ring_cpu, ring_gpu,
			   ring_bytes, flags, false, 0, 0);
}

int rdn_uvd_init_window(struct rdn_uvd *uvd, struct rdn_accel *accel,
			uint32_t ref_freq, const uint8_t *fw, uint32_t fw_size,
			uint32_t handles, volatile uint8_t *vcpu_shadow, uint32_t vcpu_offset,
			volatile uint8_t *ring_shadow, uint32_t ring_offset,
			uint32_t ring_bytes, uint32_t flags)
{
	return init_common(uvd, accel, ref_freq, fw, fw_size, handles, vcpu_shadow, accel->vram_base + vcpu_offset,
			   ring_shadow, accel->vram_base + ring_offset, ring_bytes, flags, true, vcpu_offset,
			   ring_offset);
}

int rdn_uvd_init(struct rdn_uvd *uvd, struct rdn_accel *accel,
		 uint32_t ref_freq, const uint8_t *fw, uint32_t fw_size,
		 uint32_t handles, uint32_t vcpu_offset, uint32_t ring_offset,
		 uint32_t ring_bytes, uint32_t flags)
{
	uint32_t vcpu_bytes = rdn_uvd_vcpu_bytes(fw_size, handles);
	volatile uint8_t *bytes = (volatile uint8_t *)accel->aperture;
	int r;

	if (!accel->aperture || (vcpu_offset & 4095) || (ring_offset & 4095) ||
	    (uint64_t)vcpu_offset + vcpu_bytes > accel->aperture_size ||
	    (uint64_t)ring_offset + ring_bytes + RDN_UVD_RING_EXTRA > accel->aperture_size ||
	    (vcpu_offset < ring_offset + ring_bytes + RDN_UVD_RING_EXTRA &&
	     ring_offset < vcpu_offset + vcpu_bytes))
		return -EINVAL;
	r = rdn_uvd_init_mem(uvd, accel, ref_freq, fw, fw_size, handles,
			     bytes + vcpu_offset, accel->vram_base + vcpu_offset,
			     bytes + ring_offset, accel->vram_base + ring_offset,
			     ring_bytes, flags);
	if (!uvd->accel)
		return r;
	uvd->in_vram = true;
	uvd->vcpu_offset = vcpu_offset;
	uvd->ring_offset = ring_offset;
	return r;
}

void rdn_uvd_fini(struct rdn_uvd *uvd)
{
	if (!uvd->accel)
		return;
	vcpu_stop(uvd);
	uvd->ready = false;
	if (!(reset_level & RDN_UVD_RESET_NO_IDLE))
		(void)rdn_uvd_idle(uvd);
}

/* ---- work -------------------------------------------------------------- */

int rdn_uvd_ib_submit(struct rdn_uvd *uvd, uint64_t gpu_addr, uint32_t words,
		      uint32_t *seq)
{
	uint64_t ring_gpu = uvd_addr(uvd, uvd->ring_gpu);
	uint32_t fence_low = (uint32_t)(ring_gpu + uvd->ring_words * 4);
	int r;

	if (!uvd->ready || !words)
		return -EINVAL;
	if ((gpu_addr >> 32) != (uvd->ring_gpu >> 32))
		return -EINVAL;
	r = ring_begin(uvd, 32);
	if (r)
		return r;
	*seq = ++uvd->fence_emitted;
	ring_put(uvd, RDN_UVD_PACKET0(UVD_RBC_IB_BASE, 0));
	ring_put(uvd, (uint32_t)gpu_addr);
	ring_put(uvd, RDN_UVD_PACKET0(UVD_RBC_IB_SIZE, 0));
	ring_put(uvd, words);
	/*
	 * the fence, as uvd_v2_2_fence_emit() (the evergreen and later hardware): UVD_CONTEXT_ID gets the
	 * sequence number, the VCPU writes that register's value to the address in DATA0/DATA1 (command 0), then
	 * traps (command 2)
	 */
	ring_put(uvd, RDN_UVD_PACKET0(UVD_CONTEXT_ID, 0));
	ring_put(uvd, *seq);
	ring_put(uvd, RDN_UVD_PACKET0(UVD_GPCOM_VCPU_DATA0, 0));
	ring_put(uvd, fence_low);
	ring_put(uvd, RDN_UVD_PACKET0(UVD_GPCOM_VCPU_DATA1, 0));
	ring_put(uvd, (uint32_t)(ring_gpu >> 32) & 0xff);
	ring_put(uvd, RDN_UVD_PACKET0(UVD_GPCOM_VCPU_CMD, 0));
	ring_put(uvd, 0);
	ring_put(uvd, RDN_UVD_PACKET0(UVD_GPCOM_VCPU_DATA0, 0));
	ring_put(uvd, 0);
	ring_put(uvd, RDN_UVD_PACKET0(UVD_GPCOM_VCPU_DATA1, 0));
	ring_put(uvd, 0);
	ring_put(uvd, RDN_UVD_PACKET0(UVD_GPCOM_VCPU_CMD, 0));
	ring_put(uvd, 2);
	ring_commit(uvd);
	return 0;
}

void rdn_uvd_set_fence_base(struct rdn_uvd *uvd, uint32_t base)
{
	uvd->fence_emitted = base;
	put_le32(uvd->ring_cpu + uvd->ring_words * 4, base);
	if (uvd->windowed)
		uvd_put(uvd, uvd->ring_offset + uvd->ring_words * 4, (const uint8_t *)uvd->ring_cpu + uvd->ring_words * 4, 4);
	rdn_hdp_flush(uvd->accel);
}

bool rdn_uvd_fence_done(struct rdn_uvd *uvd, uint32_t seq)
{
	rdn_hdp_flush(uvd->accel);
	if (uvd->windowed) {
		uint8_t word[4];

		if (rdn_vram_get(uvd->accel->card, uvd->ring_offset + uvd->ring_words * 4, word, 4))
			return false;
		return (int32_t)(get_le32(word) - seq) >= 0;
	}
	return (int32_t)(get_le32(uvd->ring_cpu + uvd->ring_words * 4) - seq) >= 0;
}

int rdn_uvd_fence_wait(struct rdn_uvd *uvd, uint32_t seq, uint32_t timeout_ms)
{
	struct rdn_card *card = uvd->accel->card;
	uint64_t end = card->os->time_ms(card->os->cookie) + timeout_ms;

	while (!rdn_uvd_fence_done(uvd, seq)) {
		if (card->os->time_ms(card->os->cookie) >= end)
			return -ETIMEDOUT;
		card->os->delay_us(card->os->cookie, 100);
	}
	return 0;
}
