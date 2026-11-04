/*
 * Copyright 2010 Advanced Micro Devices, Inc.
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
 * Authors: Alex Deucher
 */

#include "rdn_accel.h"
#include "rdn_gart.h"
#define u32 uint32_t
#include "linux/nid.h"
#define WREG32(reg, value) rdn_wreg(card, (reg), (value))
#define RREG32(reg) rdn_rreg(card, (reg))

static const u32 cayman_golden_registers[] = {
	0x5eb4, 0xffffffff, 0x00000002,
	0x5e78, 0x8f311ff1, 0x001000f0,
	0x3f90, 0xffff0000, 0xff000000,
	0x9148, 0xffff0000, 0xff000000,
	0x3f94, 0xffff0000, 0xff000000,
	0x914c, 0xffff0000, 0xff000000,
	0xc78, 0x00000080, 0x00000080,
	0xbd4, 0x70073777, 0x00011003,
	0xd02c, 0xbfffff1f, 0x08421000,
	0xd0b8, 0x73773777, 0x02011003,
	0x5bc0, 0x00200000, 0x50100000,
	0x98f8, 0x33773777, 0x02011003,
	0x98fc, 0xffffffff, 0x76541032,
	0x7030, 0x31000311, 0x00000011,
	0x2f48, 0x33773777, 0x42010001,
	0x6b28, 0x00000010, 0x00000012,
	0x7728, 0x00000010, 0x00000012,
	0x10328, 0x00000010, 0x00000012,
	0x10f28, 0x00000010, 0x00000012,
	0x11b28, 0x00000010, 0x00000012,
	0x12728, 0x00000010, 0x00000012,
	0x240c, 0x000007ff, 0x00000000,
	0x8a14, 0xf000001f, 0x00000007,
	0x8b24, 0x3fff3fff, 0x00ff0fff,
	0x8b10, 0x0000ff0f, 0x00000000,
	0x28a4c, 0x07ffffff, 0x06000000,
	0x10c, 0x00000001, 0x00010003,
	0xa02c, 0xffffffff, 0x0000009b,
	0x913c, 0x0000010f, 0x01000100,
	0x8c04, 0xf8ff00ff, 0x40600060,
	0x28350, 0x00000f01, 0x00000000,
	0x9508, 0x3700001f, 0x00000002,
	0x960c, 0xffffffff, 0x54763210,
	0x88c4, 0x001f3ae3, 0x00000082,
	0x88d0, 0xffffffff, 0x0f40df40,
	0x88d4, 0x0000001f, 0x00000010,
	0x8974, 0xffffffff, 0x00000000
};

static const u32 cayman_golden_registers2[] = {
	0x3e5c, 0xffffffff, 0x00000000,
	0x3e48, 0xffffffff, 0x00000000,
	0x3e4c, 0xffffffff, 0x00000000,
	0x3e64, 0xffffffff, 0x00000000,
	0x3e50, 0xffffffff, 0x00000000,
	0x3e60, 0xffffffff, 0x00000000
};

void rdn_cayman_gpu_init(struct rdn_accel *accel)
{
	struct rdn_card *card = accel->card;
	struct rdn_gpu_config *cfg = &accel->cfg;
	u32 gb_addr_config = 0;
	u32 mc_arb_ramcfg;
	u32 cgts_tcc_disable;
	u32 sx_debug_1;
	u32 smx_dc_ctl0;
	u32 cgts_sm_ctrl_reg;
	u32 hdp_host_path_cntl;
	u32 tmp;
	u32 disabled_rb_mask;
	int i, j;

	for (i = 0; i < (int)(sizeof(cayman_golden_registers) / sizeof(u32)); i += 3) {
		u32 reg = cayman_golden_registers[i];
		WREG32(reg, (RREG32(reg) & ~cayman_golden_registers[i + 1]) |
			cayman_golden_registers[i + 2]);
	}
	for (i = 0; i < (int)(sizeof(cayman_golden_registers2) / sizeof(u32)); i += 3) {
		u32 reg = cayman_golden_registers2[i];
		WREG32(reg, (RREG32(reg) & ~cayman_golden_registers2[i + 1]) |
			cayman_golden_registers2[i + 2]);
	}

	cfg->num_ses = 2;
	cfg->max_pipes = 4;
	cfg->max_tile_pipes = 8;
	cfg->max_simds = 12;
	cfg->max_backends = 8;
	cfg->max_texture_channel_caches = 8;
	cfg->max_gprs = 256;
	cfg->max_threads = 256;
	cfg->max_gs_threads = 32;
	cfg->max_stack_entries = 512;
	cfg->sx_num_of_sets = 8;
	cfg->sx_max_export_size = 256;
	cfg->sx_max_export_pos_size = 64;
	cfg->sx_max_export_smx_size = 192;
	cfg->max_hw_contexts = 8;
	cfg->sq_num_cf_insts = 2;

	cfg->sc_prim_fifo_size = 0x100;
	cfg->sc_hiz_tile_fifo_size = 0x30;
	cfg->sc_earlyz_tile_fifo_size = 0x130;
	gb_addr_config = CAYMAN_GB_ADDR_CONFIG_GOLDEN;

	/* Initialize HDP */
	for (i = 0, j = 0; i < 32; i++, j += 0x18) {
		WREG32((0x2c14 + j), 0x00000000);
		WREG32((0x2c18 + j), 0x00000000);
		WREG32((0x2c1c + j), 0x00000000);
		WREG32((0x2c20 + j), 0x00000000);
		WREG32((0x2c24 + j), 0x00000000);
	}

	WREG32(GRBM_CNTL, GRBM_READ_TIMEOUT(0xff));
	WREG32(SRBM_INT_CNTL, 0x1);
	WREG32(SRBM_INT_ACK, 0x1);


	RREG32(MC_SHARED_CHMAP);
	mc_arb_ramcfg = RREG32(MC_ARB_RAMCFG);

	tmp = (mc_arb_ramcfg & NOOFCOLS_MASK) >> NOOFCOLS_SHIFT;
	cfg->mem_row_size_in_kb = (4 * (1 << (8 + tmp))) / 1024;
	if (cfg->mem_row_size_in_kb > 4)
		cfg->mem_row_size_in_kb = 4;
	/* XXX use MC settings? */
	cfg->shader_engine_tile_size = 32;
	cfg->num_gpus = 1;
	cfg->multi_gpu_tile_size = 64;

	tmp = (gb_addr_config & NUM_PIPES_MASK) >> NUM_PIPES_SHIFT;
	cfg->num_tile_pipes = (1 << tmp);
	tmp = (gb_addr_config & PIPE_INTERLEAVE_SIZE_MASK) >> PIPE_INTERLEAVE_SIZE_SHIFT;
	cfg->mem_max_burst_length_bytes = (tmp + 1) * 256;
	tmp = (gb_addr_config & NUM_SHADER_ENGINES_MASK) >> NUM_SHADER_ENGINES_SHIFT;
	cfg->num_ses = tmp + 1;
	tmp = (gb_addr_config & NUM_GPUS_MASK) >> NUM_GPUS_SHIFT;
	cfg->num_gpus = tmp + 1;
	tmp = (gb_addr_config & MULTI_GPU_TILE_SIZE_MASK) >> MULTI_GPU_TILE_SIZE_SHIFT;
	cfg->multi_gpu_tile_size = 1 << tmp;
	tmp = (gb_addr_config & ROW_SIZE_MASK) >> ROW_SIZE_SHIFT;
	cfg->mem_row_size_in_kb = 1 << tmp;


	/* setup tiling info dword.  gb_addr_config is not adequate since it does
	 * not have bank info, so create a custom tiling dword.
	 * bits 3:0   num_pipes
	 * bits 7:4   num_banks
	 * bits 11:8  group_size
	 * bits 15:12 row_size
	 */
	cfg->tile_config = 0;
	switch (cfg->num_tile_pipes) {
	case 1:
	default:
		cfg->tile_config |= (0 << 0);
		break;
	case 2:
		cfg->tile_config |= (1 << 0);
		break;
	case 4:
		cfg->tile_config |= (2 << 0);
		break;
	case 8:
		cfg->tile_config |= (3 << 0);
		break;
	}

	/* num banks is 8 on all fusion asics. 0 = 4, 1 = 8, 2 = 16 */
	{
		switch ((mc_arb_ramcfg & NOOFBANK_MASK) >> NOOFBANK_SHIFT) {
		case 0: /* four banks */
			cfg->tile_config |= 0 << 4;
			break;
		case 1: /* eight banks */
			cfg->tile_config |= 1 << 4;
			break;
		case 2: /* sixteen banks */
		default:
			cfg->tile_config |= 2 << 4;
			break;
		}
	}
	cfg->tile_config |=
		((gb_addr_config & PIPE_INTERLEAVE_SIZE_MASK) >> PIPE_INTERLEAVE_SIZE_SHIFT) << 8;
	cfg->tile_config |=
		((gb_addr_config & ROW_SIZE_MASK) >> ROW_SIZE_SHIFT) << 12;

	tmp = 0;
	for (i = (cfg->num_ses - 1); i >= 0; i--) {
		u32 rb_disable_bitmap;

		WREG32(GRBM_GFX_INDEX, INSTANCE_BROADCAST_WRITES | SE_INDEX(i));
		WREG32(RLC_GFX_INDEX, INSTANCE_BROADCAST_WRITES | SE_INDEX(i));
		rb_disable_bitmap = (RREG32(CC_RB_BACKEND_DISABLE) & 0x00ff0000) >> 16;
		tmp <<= 4;
		tmp |= rb_disable_bitmap;
	}
	/* enabled rb are just the one not disabled :) */
	disabled_rb_mask = tmp;
	tmp = 0;
	for (i = 0; i < (cfg->max_backends); i++)
		tmp |= (1 << i);
	/* if all the backends are disabled, fix it up here */
	if ((disabled_rb_mask & tmp) == tmp) {
		for (i = 0; i < (cfg->max_backends); i++)
			disabled_rb_mask &= ~(1 << i);
	}

	for (i = 0; i < cfg->num_ses; i++) {
		u32 simd_disable_bitmap;

		WREG32(GRBM_GFX_INDEX, INSTANCE_BROADCAST_WRITES | SE_INDEX(i));
		WREG32(RLC_GFX_INDEX, INSTANCE_BROADCAST_WRITES | SE_INDEX(i));
		simd_disable_bitmap = (RREG32(CC_GC_SHADER_PIPE_CONFIG) & 0xffff0000) >> 16;
		simd_disable_bitmap |= 0xffffffff << cfg->max_simds;
		tmp <<= 16;
		tmp |= simd_disable_bitmap;
	}
	cfg->active_simds = __builtin_popcount(~tmp);

	WREG32(GRBM_GFX_INDEX, INSTANCE_BROADCAST_WRITES | SE_BROADCAST_WRITES);
	WREG32(RLC_GFX_INDEX, INSTANCE_BROADCAST_WRITES | SE_BROADCAST_WRITES);

	WREG32(GB_ADDR_CONFIG, gb_addr_config);
	WREG32(DMIF_ADDR_CONFIG, gb_addr_config);
	WREG32(HDP_ADDR_CONFIG, gb_addr_config);
	WREG32(DMA_TILING_CONFIG + DMA0_REGISTER_OFFSET, gb_addr_config);
	WREG32(DMA_TILING_CONFIG + DMA1_REGISTER_OFFSET, gb_addr_config);
	WREG32(UVD_UDEC_ADDR_CONFIG, gb_addr_config);
	WREG32(UVD_UDEC_DB_ADDR_CONFIG, gb_addr_config);
	WREG32(UVD_UDEC_DBW_ADDR_CONFIG, gb_addr_config);

	tmp = gb_addr_config & NUM_PIPES_MASK;
	tmp = rdn_remap_render_backend(tmp,
		cfg->max_backends, CAYMAN_MAX_BACKENDS, disabled_rb_mask);

	cfg->backend_map = tmp;
	WREG32(GB_BACKEND_MAP, tmp);

	cgts_tcc_disable = 0xffff0000;
	for (i = 0; i < cfg->max_texture_channel_caches; i++)
		cgts_tcc_disable &= ~(1 << (16 + i));
	WREG32(CGTS_TCC_DISABLE, cgts_tcc_disable);
	WREG32(CGTS_SYS_TCC_DISABLE, cgts_tcc_disable);
	WREG32(CGTS_USER_SYS_TCC_DISABLE, cgts_tcc_disable);
	WREG32(CGTS_USER_TCC_DISABLE, cgts_tcc_disable);

	/* reprogram the shader complex */
	cgts_sm_ctrl_reg = RREG32(CGTS_SM_CTRL_REG);
	for (i = 0; i < 16; i++)
		WREG32(CGTS_SM_CTRL_REG, OVERRIDE);
	WREG32(CGTS_SM_CTRL_REG, cgts_sm_ctrl_reg);

	/* set HW defaults for 3D engine */
	WREG32(CP_MEQ_THRESHOLDS, MEQ1_START(0x30) | MEQ2_START(0x60));

	sx_debug_1 = RREG32(SX_DEBUG_1);
	sx_debug_1 |= ENABLE_NEW_SMX_ADDRESS;
	WREG32(SX_DEBUG_1, sx_debug_1);

	smx_dc_ctl0 = RREG32(SMX_DC_CTL0);
	smx_dc_ctl0 &= ~NUMBER_OF_SETS(0x1ff);
	smx_dc_ctl0 |= NUMBER_OF_SETS(cfg->sx_num_of_sets);
	WREG32(SMX_DC_CTL0, smx_dc_ctl0);

	WREG32(SPI_CONFIG_CNTL_1, VTX_DONE_DELAY(4) | CRC_SIMD_ID_WADDR_DISABLE);

	/* need to be explicitly zero-ed */
	WREG32(VGT_OFFCHIP_LDS_BASE, 0);
	WREG32(SQ_LSTMP_RING_BASE, 0);
	WREG32(SQ_HSTMP_RING_BASE, 0);
	WREG32(SQ_ESTMP_RING_BASE, 0);
	WREG32(SQ_GSTMP_RING_BASE, 0);
	WREG32(SQ_VSTMP_RING_BASE, 0);
	WREG32(SQ_PSTMP_RING_BASE, 0);

	WREG32(TA_CNTL_AUX, DISABLE_CUBE_ANISO);

	WREG32(SX_EXPORT_BUFFER_SIZES, (COLOR_BUFFER_SIZE((cfg->sx_max_export_size / 4) - 1) |
					POSITION_BUFFER_SIZE((cfg->sx_max_export_pos_size / 4) - 1) |
					SMX_BUFFER_SIZE((cfg->sx_max_export_smx_size / 4) - 1)));

	WREG32(PA_SC_FIFO_SIZE, (SC_PRIM_FIFO_SIZE(cfg->sc_prim_fifo_size) |
				 SC_HIZ_TILE_FIFO_SIZE(cfg->sc_hiz_tile_fifo_size) |
				 SC_EARLYZ_TILE_FIFO_SIZE(cfg->sc_earlyz_tile_fifo_size)));


	WREG32(VGT_NUM_INSTANCES, 1);

	WREG32(CP_PERFMON_CNTL, 0);

	WREG32(SQ_MS_FIFO_SIZES, (CACHE_FIFO_SIZE(16 * cfg->sq_num_cf_insts) |
				  FETCH_FIFO_HIWATER(0x4) |
				  DONE_FIFO_HIWATER(0xe0) |
				  ALU_UPDATE_FIFO_HIWATER(0x8)));

	WREG32(SQ_GPR_RESOURCE_MGMT_1, NUM_CLAUSE_TEMP_GPRS(4));
	WREG32(SQ_CONFIG, (VC_ENABLE |
			   EXPORT_SRC_C |
			   GFX_PRIO(0) |
			   CS1_PRIO(0) |
			   CS2_PRIO(1)));
	WREG32(SQ_DYN_GPR_CNTL_PS_FLUSH_REQ, DYN_GPR_ENABLE);

	WREG32(PA_SC_FORCE_EOV_MAX_CNTS, (FORCE_EOV_MAX_CLK_CNT(4095) |
					  FORCE_EOV_MAX_REZ_CNT(255)));

	WREG32(VGT_CACHE_INVALIDATION, CACHE_INVALIDATION(VC_AND_TC) |
	       AUTO_INVLD_EN(ES_AND_GS_AUTO));

	WREG32(VGT_GS_VERTEX_REUSE, 16);
	WREG32(PA_SC_LINE_STIPPLE_STATE, 0);

	WREG32(CB_PERF_CTR0_SEL_0, 0);
	WREG32(CB_PERF_CTR0_SEL_1, 0);
	WREG32(CB_PERF_CTR1_SEL_0, 0);
	WREG32(CB_PERF_CTR1_SEL_1, 0);
	WREG32(CB_PERF_CTR2_SEL_0, 0);
	WREG32(CB_PERF_CTR2_SEL_1, 0);
	WREG32(CB_PERF_CTR3_SEL_0, 0);
	WREG32(CB_PERF_CTR3_SEL_1, 0);

	tmp = RREG32(HDP_MISC_CNTL);
	tmp |= HDP_FLUSH_INVALIDATE_CACHE;
	WREG32(HDP_MISC_CNTL, tmp);

	hdp_host_path_cntl = RREG32(HDP_HOST_PATH_CNTL);
	WREG32(HDP_HOST_PATH_CNTL, hdp_host_path_cntl);

	WREG32(PA_CL_ENHANCE, CLIP_VTX_REORDER_ENA | NUM_CLIP_SEQ(3));

	card->os->delay_us(card->os->cookie, 50);

}

void rdn_cayman_ring_setup(struct rdn_accel *accel)
{
	struct rdn_card *card = accel->card;
	uint64_t addr = rdn_vram_addr(accel, accel->ring_offset);
	const uint32_t cntl[3] = { CP_RB0_CNTL, CP_RB1_CNTL, CP_RB2_CNTL };
	const uint32_t base[3] = { CP_RB0_BASE, CP_RB1_BASE, CP_RB2_BASE };
	const uint32_t rptr[3] = { CP_RB0_RPTR, CP_RB1_RPTR, CP_RB2_RPTR };
	const uint32_t wptr[3] = { CP_RB0_WPTR, CP_RB1_WPTR, CP_RB2_WPTR };
	const uint32_t raddr[3] = { CP_RB0_RPTR_ADDR, CP_RB1_RPTR_ADDR, CP_RB2_RPTR_ADDR };
	const uint32_t raddr_hi[3] = { CP_RB0_RPTR_ADDR_HI, CP_RB1_RPTR_ADDR_HI, CP_RB2_RPTR_ADDR_HI };
	uint32_t size = 0, words = accel->ring_words / 2, value;
	int i;
	while ((1u << size) < words)
		size++;
	value = size | (9u << 8) | RB_NO_UPDATE;
	if (accel->swapped)
		value |= BUF_SWAP_32BIT;
	WREG32(GRBM_SOFT_RESET, SOFT_RESET_CP | SOFT_RESET_PA |
		SOFT_RESET_SH | SOFT_RESET_VGT | SOFT_RESET_SPI | SOFT_RESET_SX);
	RREG32(GRBM_SOFT_RESET);
	card->os->delay_us(card->os->cookie, 15000);
	WREG32(GRBM_SOFT_RESET, 0);
	RREG32(GRBM_SOFT_RESET);
	WREG32(CP_SEM_WAIT_TIMER, 0);
	WREG32(CP_SEM_INCOMPLETE_TIMER_CNTL, 0);
	WREG32(CP_RB_WPTR_DELAY, 0);
	WREG32(CP_DEBUG, 1u << 27);
	WREG32(SCRATCH_ADDR, (uint32_t)(addr >> 8));
	WREG32(SCRATCH_UMSK, 0);
	for (i = 0; i < 3; i++) {
		WREG32(cntl[i], value);
		WREG32(raddr[i], (uint32_t)addr & 0xfffffffc);
		WREG32(raddr_hi[i], (uint32_t)(addr >> 32) & 0xff);
	}
	for (i = 0; i < 3; i++)
		WREG32(base[i], (uint32_t)(addr >> 8));
	for (i = 0; i < 3; i++) {
		WREG32(cntl[i], value | RB_RPTR_WR_ENA);
		WREG32(rptr[i], 0);
		WREG32(wptr[i], 0);
		card->os->delay_us(card->os->cookie, 1000);
		WREG32(cntl[i], value);
	}
	accel->wptr = 0;
}

int rdn_cayman_gart_setup(struct rdn_gart *gart)
{
	struct rdn_accel *accel = gart->accel;
	struct rdn_card *card = accel->card;
	WREG32(MC_VM_MX_L1_TLB_CNTL, (0xa << 7) | ENABLE_L1_TLB |
		ENABLE_L1_FRAGMENT_PROCESSING | SYSTEM_ACCESS_MODE_NOT_IN_SYS |
		ENABLE_ADVANCED_DRIVER_MODEL | SYSTEM_APERTURE_UNMAPPED_ACCESS_PASS_THRU);
	WREG32(VM_L2_CNTL, ENABLE_L2_CACHE | ENABLE_L2_FRAGMENT_PROCESSING |
		ENABLE_L2_PTE_CACHE_LRU_UPDATE_BY_WRITE |
		ENABLE_L2_PDE0_CACHE_LRU_UPDATE_BY_WRITE | EFFECTIVE_L2_QUEUE_SIZE(7) |
		CONTEXT1_IDENTITY_ACCESS_MODE(1));
	WREG32(VM_L2_CNTL2, INVALIDATE_ALL_L1_TLBS | INVALIDATE_L2_CACHE);
	WREG32(VM_L2_CNTL3, L2_CACHE_BIGK_ASSOCIATIVITY | BANK_SELECT(6) |
		L2_CACHE_BIGK_FRAGMENT_SIZE(6));
	WREG32(VM_CONTEXT0_PAGE_TABLE_START_ADDR, (uint32_t)(rdn_gart_gpu_start(accel) >> 12));
	WREG32(VM_CONTEXT0_PAGE_TABLE_END_ADDR,
		(uint32_t)(rdn_gart_gpu_start(accel) >> 12) + RDN_GART_PAGES - 1);
	WREG32(VM_CONTEXT0_PAGE_TABLE_BASE_ADDR,
		(uint32_t)(rdn_vram_addr(accel, gart->table_offset) >> 12));
	WREG32(VM_CONTEXT0_PROTECTION_FAULT_DEFAULT_ADDR, (uint32_t)(gart->dummy >> 12));
	WREG32(VM_CONTEXT0_CNTL2, 0);
	WREG32(VM_CONTEXT0_CNTL, ENABLE_CONTEXT | PAGE_TABLE_DEPTH(0) |
		RANGE_PROTECTION_FAULT_ENABLE_DEFAULT);
	WREG32(0x15d4, 0);
	WREG32(0x15d8, 0);
	WREG32(0x15dc, 0);
	WREG32(VM_CONTEXT1_CNTL, 0);
	return rdn_gart_flush_checked(gart);
}

void rdn_cayman_gart_disable(struct rdn_gart *gart)
{
	struct rdn_card *card = gart->accel->card;
	WREG32(VM_CONTEXT0_CNTL, 0);
	WREG32(VM_CONTEXT1_CNTL, 0);
	WREG32(MC_VM_MX_L1_TLB_CNTL, SYSTEM_ACCESS_MODE_NOT_IN_SYS | (0xa << 7));
	WREG32(VM_L2_CNTL, ENABLE_L2_FRAGMENT_PROCESSING |
		ENABLE_L2_PTE_CACHE_LRU_UPDATE_BY_WRITE |
		ENABLE_L2_PDE0_CACHE_LRU_UPDATE_BY_WRITE | EFFECTIVE_L2_QUEUE_SIZE(7));
	WREG32(VM_L2_CNTL2, 0);
	WREG32(VM_L2_CNTL3, L2_CACHE_BIGK_ASSOCIATIVITY | BANK_SELECT(6) |
		L2_CACHE_BIGK_FRAGMENT_SIZE(6));
}

/*
 * Hang detection and soft reset, from Linux cayman_gpu_check_soft_reset()
 * and cayman_gpu_soft_reset() (ni.c, MIT). Only the graphics, CP, GRBM and
 * DMA engines are considered: the memory controller is usually only busy,
 * and resetting it would also stop scanout.
 */
uint32_t rdn_cayman_hang_mask(struct rdn_accel *accel, struct rdn_hang_regs *regs)
{
	struct rdn_card *card = accel->card;
	uint32_t mask = 0, grbm = RREG32(GRBM_STATUS), srbm = RREG32(SRBM_STATUS);

	if (grbm & (PA_BUSY | SC_BUSY | SH_BUSY | SX_BUSY | TA_BUSY | VGT_BUSY |
		    DB_BUSY | CB_BUSY | GDS_BUSY | SPI_BUSY | IA_BUSY | IA_BUSY_NO_DMA))
		mask |= RDN_HANG_GFX;
	if (grbm & (CF_RQ_PENDING | PF_RQ_PENDING | CP_BUSY | CP_COHERENCY_BUSY))
		mask |= RDN_HANG_CP;
	if (grbm & GRBM_EE_BUSY)
		mask |= RDN_HANG_GRBM | RDN_HANG_GFX | RDN_HANG_CP;
	if (srbm & GRBM_RQ_PENDING)
		mask |= RDN_HANG_GRBM;
	if (regs) {
		regs->grbm_status = grbm;
		regs->grbm_se0 = RREG32(GRBM_STATUS_SE0);
		regs->grbm_se1 = RREG32(GRBM_STATUS_SE1);
		regs->srbm_status = srbm;
		regs->cp_rptr = RREG32(CP_RB0_RPTR);
		regs->cp_wptr = RREG32(CP_RB0_WPTR);
		regs->fence = accel->fence_memory ? rdn_vram_read32(accel, accel->fence_offset) : 0;
	}
	return mask;
}

void rdn_cayman_soft_reset(struct rdn_accel *accel, uint32_t mask)
{
	struct rdn_card *card = accel->card;
	uint32_t grbm = 0, srbm = 0, tmp;

	/* Disable CP parsing and prefetching before resetting anything. */
	WREG32(CP_ME_CNTL, CP_ME_HALT | CP_PFP_HALT);
	card->os->delay_us(card->os->cookie, 50);
	if (mask & (RDN_HANG_GFX | RDN_HANG_CP))
		grbm = SOFT_RESET_CB | SOFT_RESET_DB | SOFT_RESET_GDS | SOFT_RESET_PA |
		       SOFT_RESET_SC | SOFT_RESET_SPI | SOFT_RESET_SH | SOFT_RESET_SX |
		       SOFT_RESET_TC | SOFT_RESET_TA | SOFT_RESET_VGT | SOFT_RESET_IA;
	if (mask & RDN_HANG_CP) {
		grbm |= SOFT_RESET_CP | SOFT_RESET_VGT;
		srbm |= SOFT_RESET_GRBM;
	}
	if (mask & RDN_HANG_GRBM)
		srbm |= SOFT_RESET_GRBM;
	if (grbm) {
		tmp = RREG32(GRBM_SOFT_RESET) | grbm;
		WREG32(GRBM_SOFT_RESET, tmp);
		RREG32(GRBM_SOFT_RESET);
		card->os->delay_us(card->os->cookie, 50);
		WREG32(GRBM_SOFT_RESET, tmp & ~grbm);
		RREG32(GRBM_SOFT_RESET);
	}
	if (srbm) {
		tmp = RREG32(SRBM_SOFT_RESET) | srbm;
		WREG32(SRBM_SOFT_RESET, tmp);
		RREG32(SRBM_SOFT_RESET);
		card->os->delay_us(card->os->cookie, 50);
		WREG32(SRBM_SOFT_RESET, tmp & ~srbm);
		RREG32(SRBM_SOFT_RESET);
	}
	card->os->delay_us(card->os->cookie, 50);
}
