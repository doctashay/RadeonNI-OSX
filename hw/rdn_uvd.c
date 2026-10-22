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
#define RDN_UVD_RING_ALIGN_MASK	15u

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

static bool valid_accel(const struct rdn_accel *accel)
{
	const struct rdn_os *os = accel && accel->card ? accel->card->os : NULL;

	return os && os->mmio_read32 && os->mmio_write32 && os->delay_us &&
	       os->time_ms && os->log;
}

static bool valid_range(uint64_t addr, uint64_t bytes, unsigned segment_shift)
{
	return bytes && addr < (1ull << 40) && bytes <= (1ull << 40) - addr &&
	       (addr >> segment_shift) == ((addr + bytes - 1) >> segment_shift);
}

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
	struct atom_context *ctx = card ? card->atom.ctx : NULL;
	uint16_t size = 0, offset = 0;
	const uint8_t *info;

	if (!ref_freq || !ctx || !atom_parse_data_header(ctx, GetIndexIntoMasterTable(DATA, FirmwareInfo),
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
	uint64_t bytes;

	if (!fw_size || (fw_size & 3) || !handles || handles > RDN_UVD_DEFAULT_HANDLES)
		return 0;
	bytes = (((uint64_t)fw_size + 8 + 4095) & ~4095ull) +
		RDN_UVD_STACK_SIZE + RDN_UVD_HEAP_SIZE +
		(uint64_t)RDN_UVD_SESSION_SIZE * handles;
	return bytes <= 0xffffffffu ? (uint32_t)bytes : 0;
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

	if (!ref_freq || !vclk || !dclk || !fb_div || !vclk_div || !dclk_div)
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

	if (!card || !card->os || !card->os->mmio_read32 ||
	    !card->os->mmio_write32 || !card->os->delay_us || !card->os->log ||
	    (!vclk != !dclk))
		return -EINVAL;
	if (vclk) {
		r = rdn_uvd_calc_dividers(ref_freq, vclk, dclk, &fb_div,
					  &vclk_div, &dclk_div);
		if (r)
			return r;
	}

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
	/* The aligned fence must be sampled in one load while the GPU writes it. */
	return rdn_swap_le32(*(volatile const uint32_t *)p);
}

/* ---- the ring ---------------------------------------------------------- */

static void ring_put(struct rdn_uvd *uvd, uint32_t value)
{
	if (uvd->swapped)
		value = bswap32(value);
	put_le32(uvd->ring_cpu + uvd->wptr * 4, value);
	uvd->wptr = (uvd->wptr + 1) & (uvd->ring_words - 1);
}

static int ring_begin_impl(struct rdn_uvd *uvd, uint32_t words, bool wait)
{
	struct rdn_card *card = uvd->accel->card;
	uint32_t i;

	if (words > UINT32_MAX - RDN_UVD_RING_ALIGN_MASK)
		return -ENOMEM;
	words = (words + RDN_UVD_RING_ALIGN_MASK) & ~RDN_UVD_RING_ALIGN_MASK;
	if (words >= uvd->ring_words)
		return -ENOMEM;
	for (i = 0; i < RDN_UVD_USEC_TIMEOUT; i++) {
		uint32_t rptr = rd(card, UVD_RBC_RB_RPTR);
		if (rptr >= uvd->ring_words)
			return -EIO;
		uint32_t used = (uvd->wptr - rptr) & (uvd->ring_words - 1);

		if (uvd->ring_words - 1 - used >= words)
			return 0;
		if (!wait)
			return -EBUSY;
		card->os->delay_us(card->os->cookie, 1);
	}
	return -EBUSY;
}

static int ring_begin(struct rdn_uvd *uvd, uint32_t words)
{
 return ring_begin_impl(uvd, words, true);
}

static void ring_commit(struct rdn_uvd *uvd)
{
	struct rdn_card *card = uvd->accel->card;

	/* Match the RBC fetch size before publishing the write pointer. */
	while (uvd->wptr & RDN_UVD_RING_ALIGN_MASK)
		ring_put(uvd, RDN_UVD_PACKET0(UVD_NO_OP, 0));
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

/* ---- start and stop ---------------------------------------------------- */

/* uvd_v2_2_resume(): tell the UVD memory controller where things are. */
static void program_memory(struct rdn_uvd *uvd)
{
	struct rdn_card *card = uvd->accel->card;

	uint64_t gpu = uvd->vcpu_gpu;
	uint64_t addr = gpu >> 3;
	uint32_t size = PAGE_UP(uvd->fw_size + 4) >> 3;

	/* Cache offsets hold address bits 3-27; LMI extends the upper bits. */
	wr(card, UVD_VCPU_CACHE_OFFSET0, (uint32_t)addr & UVD_VCPU_CACHE_OFFSET_MASK);
	wr(card, UVD_VCPU_CACHE_SIZE0, size);
	addr += size;
	size = RDN_UVD_HEAP_SIZE >> 3;
	wr(card, UVD_VCPU_CACHE_OFFSET1, (uint32_t)addr & UVD_VCPU_CACHE_OFFSET_MASK);
	wr(card, UVD_VCPU_CACHE_SIZE1, size);
	addr += size;
	size = (RDN_UVD_STACK_SIZE + RDN_UVD_SESSION_SIZE * uvd->handles) >> 3;
	wr(card, UVD_VCPU_CACHE_OFFSET2, (uint32_t)addr & UVD_VCPU_CACHE_OFFSET_MASK);
	wr(card, UVD_VCPU_CACHE_SIZE2, size);

	/* bits 28-31 */
	addr = (gpu >> 28) & 0xF;
	wr(card, UVD_LMI_ADDR_EXT, (uint32_t)((addr << 12) | (addr << 0)));

	/* bits 32-39 */
	addr = (gpu >> 32) & 0xFF;
	wr(card, UVD_LMI_EXT40_ADDR, (uint32_t)(addr | (0x9 << 16) | (0x1u << 31)));

	wr(card, UVD_VCPU_CHIP_ID_REG, RDN_UVD_CHIP_ID_TURKS);
}

/* evergreen_rlc_resume() for discrete Turks, after its UVD memory windows. */
static int load_rlc(struct rdn_uvd *uvd, const uint8_t *fw)
{
	struct rdn_card *card = uvd->accel->card;
	uint32_t i;

	wr(card, RLC_CNTL, 0);
	wr(card, RLC_HB_CNTL, 0);
	wr(card, RLC_HB_BASE, 0);
	wr(card, RLC_HB_RPTR, 0);
	wr(card, RLC_HB_WPTR, 0);
	wr(card, RLC_HB_WPTR_LSB_ADDR, 0);
	wr(card, RLC_HB_WPTR_MSB_ADDR, 0);
	wr(card, RLC_MC_CNTL, 0);
	wr(card, RLC_UCODE_CNTL, 0);
	for (i = 0; i < RDN_UVD_RLC_BYTES / 4; i++) {
		const uint8_t *p = fw + i * 4;
		uint32_t word = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
			       ((uint32_t)p[2] << 8) | p[3];

		wr(card, RLC_UCODE_ADDR, i);
		wr(card, RLC_UCODE_DATA, word);
	}
	wr(card, RLC_UCODE_ADDR, 0);
	wr(card, RLC_CNTL, RLC_ENABLE);
	uvd->rlc_cntl_snapshot = rd(card, RLC_CNTL);
	rdn_log(card->os, RDN_LOG_INFO, "uvd: RLC enable readback 0x%08x",
		(unsigned)uvd->rlc_cntl_snapshot);
	if (uvd->rlc_cntl_snapshot != RLC_ENABLE)
		return -EIO;
	uvd->rlc_loaded = true;
	return 0;
}

/* uvd_v1_0_start() */
static int vcpu_start(struct rdn_uvd *uvd)
{
	struct rdn_card *card = uvd->accel->card;
	uint32_t lmi_swap_cntl = 0, mp_swap_cntl = 0;
	uint64_t ring_gpu = uvd->ring_gpu;
	uint32_t rb_bufsz, status = 0;
	int i, j, r = 0;

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
			uvd->status_snapshot = status;
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
	wr_p(card, UVD_LMI_CTRL2, 1 << 8, ~(1 << 8));
	wr_p(card, UVD_RB_ARB_CTRL, 1 << 3, ~(1 << 3));
	mdelay(card, 1);
	/* put VCPU into reset */
	wr(card, UVD_SOFT_RESET, VCPU_SOFT_RESET);
	mdelay(card, 5);
	/* disable VCPU clock */
	wr(card, UVD_VCPU_CNTL, 0x0);
	/* unstall UMC and register bus */
	wr_p(card, UVD_LMI_CTRL2, 0, ~(1 << 8));
	wr_p(card, UVD_RB_ARB_CTRL, 0, ~(1 << 3));
}

int rdn_uvd_wake(struct rdn_uvd *uvd)
{
	int r;

	if (!uvd || !uvd->accel || uvd->failed || uvd->stopped || uvd->prepared)
		return -EINVAL;
	r = rdn_uvd_set_clocks(uvd->accel->card, uvd->ref_freq, RDN_UVD_VCLK,
				  RDN_UVD_DCLK);
	if (r) {
		uvd->failed = true;
		uvd->ready = false;
	}
	return r;
}

/* What the block says when the VCPU does not come up. */
static void uvd_dump(struct rdn_uvd *uvd, const char *why)
{
	struct rdn_card *card = uvd->accel->card;
	static const struct { const char *name; uint32_t reg; } r[] = {
		{ "SRBM_STATUS", 0x0E50 }, { "SRBM_STATUS2", 0x0EC4 },
		{ "UVD_STATUS", UVD_STATUS }, { "UVD_SOFT_RESET", UVD_SOFT_RESET },
		{ "UVD_VCPU_CACHE_OFFSET0", UVD_VCPU_CACHE_OFFSET0 },
		{ "UVD_VCPU_CACHE_SIZE0", UVD_VCPU_CACHE_SIZE0 },
		{ "UVD_VCPU_CACHE_OFFSET1", UVD_VCPU_CACHE_OFFSET1 },
		{ "UVD_VCPU_CACHE_SIZE1", UVD_VCPU_CACHE_SIZE1 },
		{ "UVD_VCPU_CACHE_OFFSET2", UVD_VCPU_CACHE_OFFSET2 },
		{ "UVD_VCPU_CACHE_SIZE2", UVD_VCPU_CACHE_SIZE2 },
		{ "UVD_LMI_ADDR_EXT", UVD_LMI_ADDR_EXT },
		{ "UVD_LMI_EXT40_ADDR", UVD_LMI_EXT40_ADDR },
		{ "UVD_VCPU_CHIP_ID", UVD_VCPU_CHIP_ID_REG },
		{ "UVD_VCPU_CNTL", UVD_VCPU_CNTL }, { "UVD_LMI_CTRL", UVD_LMI_CTRL },
		{ "UVD_LMI_CTRL2", UVD_LMI_CTRL2 }, { "UVD_RB_ARB_CTRL", UVD_RB_ARB_CTRL },
		{ "UVD_MASTINT_EN", UVD_MASTINT_EN }, { "UVD_CGC_GATE", UVD_CGC_GATE },
		{ "UVD_CGC_CTRL", UVD_CGC_CTRL }, { "UVD_CGC_STATUS", UVD_CGC_STATUS },
		{ "UVD_LMI_STATUS", UVD_LMI_STATUS },
		{ "UVD_LMI_SWAP_CNTL", UVD_LMI_SWAP_CNTL },
		{ "UVD_RBC_RB_BASE", UVD_RBC_RB_BASE },
		{ "UVD_RBC_RB_CNTL", UVD_RBC_RB_CNTL },
		{ "UVD_RBC_RB_RPTR", UVD_RBC_RB_RPTR },
		{ "UVD_RBC_RB_WPTR", UVD_RBC_RB_WPTR },
		{ "UVD_RBC_RB_WPTR_CNTL", UVD_RBC_RB_WPTR_CNTL },
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
	rdn_log(card->os, RDN_LOG_ERROR,
		"uvd buffers: vcpu 0x%08x%08x, ring 0x%08x%08x, RLC 0x%08x loaded %u",
		(unsigned)(uvd->vcpu_gpu >> 32), (unsigned)uvd->vcpu_gpu,
		(unsigned)(uvd->ring_gpu >> 32), (unsigned)uvd->ring_gpu,
		(unsigned)uvd->rlc_cntl_snapshot, (unsigned)uvd->rlc_loaded);
	for (i = 0; i < sizeof(r) / sizeof(r[0]); i++)
		rdn_log(card->os, RDN_LOG_ERROR, "  %-24s 0x%04x = 0x%08x", r[i].name,
			(unsigned)r[i].reg, (unsigned)rd(card, r[i].reg));
	if (uvd->ring_cpu && uvd->ring_words >= 4)
		for (i = 0; i < 16; i += 4)
			rdn_log(card->os, RDN_LOG_ERROR, "  ring bytes %02u: %02x %02x %02x %02x", i,
				(unsigned)uvd->ring_cpu[i], (unsigned)uvd->ring_cpu[i + 1],
				(unsigned)uvd->ring_cpu[i + 2], (unsigned)uvd->ring_cpu[i + 3]);
}

int rdn_uvd_idle(struct rdn_uvd *uvd)
{
	if (!uvd || !uvd->accel || uvd->failed || uvd->stopped || uvd->prepared)
		return -EINVAL;
	if (uvd->ready && uvd->fence_emitted &&
	    !rdn_uvd_fence_done(uvd, uvd->fence_emitted))
		return -EBUSY;
	return rdn_uvd_set_clocks(uvd->accel->card, uvd->ref_freq, 0, 0);
}

static int prepare_mem(struct rdn_uvd *uvd, struct rdn_accel *accel,
		     uint32_t ref_freq, const uint8_t *fw, uint32_t fw_size,
		     uint32_t handles, volatile uint8_t *vcpu_cpu, uint64_t vcpu_gpu,
		     volatile uint8_t *ring_cpu, uint64_t ring_gpu,
		     uint32_t ring_bytes, uint32_t flags,
		     const uint8_t *rlc_fw, uint32_t rlc_size)
{
	struct rdn_card *card;
	uint32_t vcpu_bytes = rdn_uvd_vcpu_bytes(fw_size, handles);
	uint64_t ring_extent = (uint64_t)ring_bytes + RDN_UVD_RING_EXTRA;
	uint32_t fb, vd, dd;
	uint32_t i;
	int r;

	if (!uvd || !valid_accel(accel) || flags || !vcpu_bytes ||
	    !vcpu_cpu || !ring_cpu || !fw ||
	    ((uintptr_t)ring_cpu & 3) ||
	    (vcpu_gpu & 4095) || (ring_gpu & 4095) ||
	    ring_bytes < 256 || ring_bytes > RDN_UVD_MAX_RING_BYTES ||
	    (ring_bytes & (ring_bytes - 1)) ||
	    !valid_range(vcpu_gpu, vcpu_bytes, 28) ||
	    !valid_range(ring_gpu, ring_extent, 32) ||
	    (vcpu_gpu < ring_gpu + ring_extent && ring_gpu < vcpu_gpu + vcpu_bytes) ||
	    (uintptr_t)vcpu_cpu > UINTPTR_MAX - vcpu_bytes ||
	    (uintptr_t)ring_cpu > UINTPTR_MAX - ring_extent ||
	    (uintptr_t)fw > UINTPTR_MAX - fw_size ||
	    ((uintptr_t)vcpu_cpu < (uintptr_t)ring_cpu + ring_extent &&
	     (uintptr_t)ring_cpu < (uintptr_t)vcpu_cpu + vcpu_bytes) ||
	    ((uintptr_t)fw < (uintptr_t)vcpu_cpu + vcpu_bytes &&
	     (uintptr_t)vcpu_cpu < (uintptr_t)fw + fw_size) ||
	    ((uintptr_t)fw < (uintptr_t)ring_cpu + ring_extent &&
	     (uintptr_t)ring_cpu < (uintptr_t)fw + fw_size) ||
	    rdn_uvd_calc_dividers(ref_freq, RDN_UVD_VCLK, RDN_UVD_DCLK, &fb, &vd, &dd))
		return -EINVAL;
	/* the ring and the VCPU region must share the upper address bits */
	if ((vcpu_gpu >> 32) != (ring_gpu >> 32))
		return -EINVAL;
	if (uvd->started || uvd->failed || uvd->stopped || uvd->prepared)
		return -EBUSY;
	if (rlc_fw || rlc_size) {
		if (!rlc_fw || rlc_size != RDN_UVD_RLC_BYTES ||
		    (uintptr_t)rlc_fw > UINTPTR_MAX - rlc_size ||
		    ((uintptr_t)rlc_fw < (uintptr_t)vcpu_cpu + vcpu_bytes &&
		     (uintptr_t)vcpu_cpu < (uintptr_t)rlc_fw + rlc_size) ||
		    ((uintptr_t)rlc_fw < (uintptr_t)ring_cpu + ring_extent &&
		     (uintptr_t)ring_cpu < (uintptr_t)rlc_fw + rlc_size))
			return -EINVAL;
		uvd->rlc_cntl_snapshot = rd(accel->card, RLC_CNTL);
		if (uvd->rlc_cntl_snapshot) {
			rdn_log(accel->card->os, RDN_LOG_ERROR,
				"uvd: refusing configured RLC 0x%08x",
				(unsigned)uvd->rlc_cntl_snapshot);
			return -EBUSY;
		}
	}

	memset(uvd, 0, sizeof(*uvd));
	card = accel->card;
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
	for (i = 0; i < fw_size; i++)
		vcpu_cpu[i] = fw[i];
	for (; i < vcpu_bytes; i++)
		vcpu_cpu[i] = 0;
	for (i = 0; i < ring_bytes + RDN_UVD_RING_EXTRA; i++)
		ring_cpu[i] = 0;
	rdn_hdp_flush(accel);
	{
		/* read the firmware back from where the VCPU will fetch it */
		uint32_t bad = 0, first = 0;

		for (i = 0; i < fw_size; i++)
			if (vcpu_cpu[i] != fw[i]) {
				if (!bad)
					first = i;
				bad++;
			}
		if (bad) {
			rdn_log(card->os, RDN_LOG_ERROR, "uvd: firmware readback differs at %u (%u bytes)",
				(unsigned)first, (unsigned)bad);
			uvd->failed = true;
			return -EIO;
		}
	}

	uvd->started = true;
	{
		/* evergreen_gpu_init(): the decoder's tiling follows the 3D engine's */
		uint32_t cfg = rd(card, 0x98f8);	/* GB_ADDR_CONFIG */

		wr(card, UVD_UDEC_ADDR_CONFIG, cfg);
		wr(card, UVD_UDEC_DB_ADDR_CONFIG, cfg);
		wr(card, UVD_UDEC_DBW_ADDR_CONFIG, cfg);
	}
	program_memory(uvd);
	if (rlc_fw) {
		r = load_rlc(uvd, rlc_fw);
		if (r) {
			uvd->failed = true;
			return r;
		}
	}
	uvd->prepared = true;
	return 0;
}

int rdn_uvd_start_prepared(struct rdn_uvd *uvd)
{
	struct rdn_card *card;
	struct rdn_accel *accel;
	uint32_t i;
	bool clocked = false;
	int r;

	if (!uvd || !uvd->prepared || !uvd->started || uvd->ready ||
	    uvd->failed || uvd->stopped || !valid_accel(uvd->accel))
		return -EINVAL;
	accel = uvd->accel;
	if (uvd->in_vram &&
	    (!accel->aperture ||
	     (uint64_t)uvd->vcpu_offset + uvd->vcpu_bytes > accel->aperture_size ||
	     (uint64_t)uvd->ring_offset + uvd->ring_words * 4 + RDN_UVD_RING_EXTRA > accel->aperture_size ||
	     accel->vram_base >= (1ull << 40) ||
	     uvd->vcpu_gpu != accel->vram_base + uvd->vcpu_offset ||
	     uvd->ring_gpu != accel->vram_base + uvd->ring_offset ||
	     uvd->vcpu_cpu != (volatile uint8_t *)accel->aperture + uvd->vcpu_offset ||
	     uvd->ring_cpu != (volatile uint8_t *)accel->aperture + uvd->ring_offset))
		return -EINVAL;
	uvd->prepared = false;
	card = accel->card;
	if (uvd->rlc_loaded) {
		uvd->rlc_cntl_snapshot = rd(card, RLC_CNTL);
		if (uvd->rlc_cntl_snapshot != RLC_ENABLE) {
			rdn_log(card->os, RDN_LOG_ERROR,
				"uvd: prepared RLC changed to 0x%08x",
				(unsigned)uvd->rlc_cntl_snapshot);
			uvd->failed = true;
			return -EIO;
		}
	}
	r = rdn_uvd_wake(uvd);
	if (r)
		goto done;
	clocked = true;
	r = vcpu_start(uvd);
	if (r) {
		uvd_dump(uvd, "the VCPU did not answer");
		goto done;
	}
	r = ring_test(uvd);
	if (r) {
		uvd_dump(uvd, "ring test failed");
		goto done;
	}

	r = ring_begin(uvd, 10);
	if (r) {
		uvd_dump(uvd, "semaphore setup reservation failed");
		goto done;
	}
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
	for (i = 0; i < RDN_UVD_USEC_TIMEOUT; i++) {
		if (rd(card, UVD_RBC_RB_RPTR) == uvd->wptr)
			break;
		card->os->delay_us(card->os->cookie, 1);
	}
	if (i == RDN_UVD_USEC_TIMEOUT) {
		r = -ETIMEDOUT;
		uvd_dump(uvd, "semaphore setup did not drain");
		goto done;
	}
	uvd->ready = true;
	rdn_log(card->os, RDN_LOG_INFO, "uvd initialized");
done:
	if (r) {
		if (clocked) {
			vcpu_stop(uvd);
			uvd->stopped = true;
		}
		uvd->failed = true;
		uvd->ready = false;
	}
	(void)rdn_uvd_set_clocks(card, uvd->ref_freq, 0, 0);
	return r;
}

int rdn_uvd_init_mem(struct rdn_uvd *uvd, struct rdn_accel *accel,
		     uint32_t ref_freq, const uint8_t *fw, uint32_t fw_size,
		     uint32_t handles, volatile uint8_t *vcpu_cpu, uint64_t vcpu_gpu,
		     volatile uint8_t *ring_cpu, uint64_t ring_gpu,
		     uint32_t ring_bytes, uint32_t flags)
{
	int r = prepare_mem(uvd, accel, ref_freq, fw, fw_size, handles,
		vcpu_cpu, vcpu_gpu, ring_cpu, ring_gpu, ring_bytes, flags, NULL, 0);
	return r ? r : rdn_uvd_start_prepared(uvd);
}

static int prepare_vram(struct rdn_uvd *uvd, struct rdn_accel *accel,
		 uint32_t ref_freq, const uint8_t *fw, uint32_t fw_size,
		 uint32_t handles, uint32_t vcpu_offset, uint32_t ring_offset,
		 uint32_t ring_bytes, uint32_t flags,
		 const uint8_t *rlc_fw, uint32_t rlc_size)
{
	uint32_t vcpu_bytes = rdn_uvd_vcpu_bytes(fw_size, handles);
	volatile uint8_t *bytes;
	uint64_t ring_end = (uint64_t)ring_offset + ring_bytes + RDN_UVD_RING_EXTRA;
	uint64_t vcpu_end = (uint64_t)vcpu_offset + vcpu_bytes;
	int r;

	if (!uvd || !valid_accel(accel) || !vcpu_bytes || !accel->aperture ||
	    (vcpu_offset & 4095) || (ring_offset & 4095) ||
	    vcpu_end > accel->aperture_size || ring_end > accel->aperture_size ||
	    accel->vram_base >= (1ull << 40) ||
	    vcpu_end > (1ull << 40) - accel->vram_base ||
	    ring_end > (1ull << 40) - accel->vram_base ||
	    (vcpu_offset < ring_end && ring_offset < vcpu_end))
		return -EINVAL;
	if (uvd->started || uvd->failed || uvd->stopped || uvd->prepared)
		return -EBUSY;
	bytes = (volatile uint8_t *)accel->aperture;
	r = prepare_mem(uvd, accel, ref_freq, fw, fw_size, handles,
			     bytes + vcpu_offset, accel->vram_base + vcpu_offset,
			     bytes + ring_offset, accel->vram_base + ring_offset,
			     ring_bytes, flags, rlc_fw, rlc_size);
	if (!uvd->started || uvd->in_vram)
		return r;
	uvd->in_vram = true;
	uvd->vcpu_offset = vcpu_offset;
	uvd->ring_offset = ring_offset;
	return r;
}

int rdn_uvd_init(struct rdn_uvd *uvd, struct rdn_accel *accel,
		 uint32_t ref_freq, const uint8_t *fw, uint32_t fw_size,
		 uint32_t handles, uint32_t vcpu_offset, uint32_t ring_offset,
		 uint32_t ring_bytes, uint32_t flags)
{
	int r = prepare_vram(uvd, accel, ref_freq, fw, fw_size, handles,
		vcpu_offset, ring_offset, ring_bytes, flags, NULL, 0);
	return r ? r : rdn_uvd_start_prepared(uvd);
}

int rdn_uvd_init_rlc(struct rdn_uvd *uvd, struct rdn_accel *accel,
		     uint32_t ref_freq, const uint8_t *fw, uint32_t fw_size,
		     uint32_t handles, uint32_t vcpu_offset, uint32_t ring_offset,
		     uint32_t ring_bytes, uint32_t flags,
		     const uint8_t *rlc_fw, uint32_t rlc_size)
{
	int r = rdn_uvd_prepare_rlc(uvd, accel, ref_freq, fw, fw_size, handles,
		vcpu_offset, ring_offset, ring_bytes, flags, rlc_fw, rlc_size);
	return r ? r : rdn_uvd_start_prepared(uvd);
}

int rdn_uvd_prepare_rlc(struct rdn_uvd *uvd, struct rdn_accel *accel,
		     uint32_t ref_freq, const uint8_t *fw, uint32_t fw_size,
		     uint32_t handles, uint32_t vcpu_offset, uint32_t ring_offset,
		     uint32_t ring_bytes, uint32_t flags,
		     const uint8_t *rlc_fw, uint32_t rlc_size)
{
	if (!rlc_fw || rlc_size != RDN_UVD_RLC_BYTES)
		return -EINVAL;
	return prepare_vram(uvd, accel, ref_freq, fw, fw_size, handles,
		vcpu_offset, ring_offset, ring_bytes, flags, rlc_fw, rlc_size);
}

void rdn_uvd_fini(struct rdn_uvd *uvd)
{
	if (!uvd || !uvd->started || uvd->stopped)
		return;
	if (uvd->prepared) {
		uvd->prepared = false;
		uvd->stopped = true;
		return;
	}
	if (uvd->failed)
		return;
	if (uvd->ready && uvd->fence_emitted &&
	    !rdn_uvd_fence_done(uvd, uvd->fence_emitted)) {
		uvd->failed = true;
		uvd->ready = false;
		return;
	}
	if (rdn_uvd_wake(uvd))
		return;
	vcpu_stop(uvd);
	uvd->ready = false;
	uvd->stopped = true;
	(void)rdn_uvd_set_clocks(uvd->accel->card, uvd->ref_freq, 0, 0);
}

bool rdn_uvd_storage_releasable(const struct rdn_uvd *uvd)
{
	/* Reset readback alone does not prove that the DMA engine has drained. */
	return uvd && !uvd->started;
}

/* ---- work -------------------------------------------------------------- */

static int ib_submit(struct rdn_uvd *uvd, uint64_t gpu_addr, uint32_t words,
		     uint32_t *seq, bool wait)
{
	uint64_t fence_gpu;
	int r;

	if (!uvd || !uvd->ready || uvd->failed || !seq || !words ||
	    (gpu_addr & 3) || !valid_range(gpu_addr, (uint64_t)words * 4, 32))
		return -EINVAL;
	if ((gpu_addr >> 32) != (uvd->ring_gpu >> 32))
		return -EINVAL;
	if (uvd->fence_emitted == 0xffffffffu)
		return -EBUSY;
	fence_gpu = uvd->ring_gpu + uvd->ring_words * 4;
	r = ring_begin_impl(uvd, 18, wait);
	if (r) {
		if (!wait && r == -EBUSY)
			return r;
		uvd->failed = true;
		uvd->ready = false;
		return r;
	}
	*seq = ++uvd->fence_emitted;
	ring_put(uvd, RDN_UVD_PACKET0(UVD_RBC_IB_BASE, 0));
	ring_put(uvd, (uint32_t)gpu_addr);
	ring_put(uvd, RDN_UVD_PACKET0(UVD_RBC_IB_SIZE, 0));
	ring_put(uvd, words);
	/* Turks uses the UVD 2.2 fence: context sequence and a 40-bit address. */
	ring_put(uvd, RDN_UVD_PACKET0(UVD_CONTEXT_ID, 0));
	ring_put(uvd, *seq);
	ring_put(uvd, RDN_UVD_PACKET0(UVD_GPCOM_VCPU_DATA0, 0));
	ring_put(uvd, (uint32_t)fence_gpu);
	ring_put(uvd, RDN_UVD_PACKET0(UVD_GPCOM_VCPU_DATA1, 0));
	ring_put(uvd, (uint32_t)(fence_gpu >> 32) & 0xff);
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

int rdn_uvd_ib_submit(struct rdn_uvd *uvd, uint64_t gpu_addr, uint32_t words,
                      uint32_t *seq)
{
 return ib_submit(uvd, gpu_addr, words, seq, true);
}

int rdn_uvd_ib_try_submit(struct rdn_uvd *uvd, uint64_t gpu_addr, uint32_t words,
                          uint32_t *seq)
{
 return ib_submit(uvd, gpu_addr, words, seq, false);
}

bool rdn_uvd_fence_done(struct rdn_uvd *uvd, uint32_t seq)
{
	uint32_t completed;

	if (!uvd || !uvd->ready || uvd->failed || !seq || seq > uvd->fence_emitted)
		return false;
	rdn_hdp_flush(uvd->accel);
	completed = get_le32(uvd->ring_cpu + uvd->ring_words * 4);
	return completed >= seq && completed <= uvd->fence_emitted;
}

int rdn_uvd_fence_wait(struct rdn_uvd *uvd, uint32_t seq, uint32_t timeout_ms)
{
	struct rdn_card *card;
	uint64_t start;
	uint32_t polls = 0;

	if (!uvd || !uvd->ready || uvd->failed || !seq || seq > uvd->fence_emitted ||
	    timeout_ms > RDN_UVD_MAX_TIMEOUT_MS)
		return -EINVAL;
	card = uvd->accel->card;
	start = card->os->time_ms(card->os->cookie);

	while (!rdn_uvd_fence_done(uvd, seq)) {
		if (polls++ >= timeout_ms * 10 ||
		    card->os->time_ms(card->os->cookie) - start >= timeout_ms) {
			if (timeout_ms) {
				uvd->failed = true;
				uvd->ready = false;
			}
			return -ETIMEDOUT;
		}
		card->os->delay_us(card->os->cookie, 100);
	}
	return 0;
}
