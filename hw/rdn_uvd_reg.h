/*
 * UVD register definitions (Evergreen and Northern Islands). Names and
 * values are those of the Linux radeon driver (r600d.h, evergreend.h), so
 * that code and traces can be compared directly.
 *
 * Copyright 2010 Advanced Micro Devices, Inc.
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
 * Authors: Alex Deucher, Christian KÃ¶nig
 */

#ifndef RDN_UVD_REG_H
#define RDN_UVD_REG_H

#define SRBM_SOFT_RESET_UVD_REG				0x0E60
#define		SOFT_RESET_UVD					(1 << 18)

/* The UVD PLL (evergreend.h, "discrete uvd clocks") */
#define CG_UPLL_FUNC_CNTL				0x718
#	define UPLL_RESET_MASK				0x00000001
#	define UPLL_SLEEP_MASK				0x00000002
#	define UPLL_BYPASS_EN_MASK			0x00000004
#	define UPLL_CTLREQ_MASK				0x00000008
#	define UPLL_REF_DIV_MASK			0x003F0000
#	define UPLL_VCO_MODE_MASK			0x00000200
#	define UPLL_CTLACK_MASK				0x40000000
#	define UPLL_CTLACK2_MASK			0x80000000
#define CG_UPLL_FUNC_CNTL_2				0x71c
#	define UPLL_PDIV_A(x)				((x) << 0)
#	define UPLL_PDIV_A_MASK				0x0000007F
#	define UPLL_PDIV_B(x)				((x) << 8)
#	define UPLL_PDIV_B_MASK				0x00007F00
#	define VCLK_SRC_SEL(x)				((x) << 20)
#	define VCLK_SRC_SEL_MASK			0x01F00000
#	define DCLK_SRC_SEL(x)				((x) << 25)
#	define DCLK_SRC_SEL_MASK			0x3E000000
#define CG_UPLL_FUNC_CNTL_3				0x720
#	define UPLL_FB_DIV(x)				((x) << 0)
#	define UPLL_FB_DIV_MASK				0x01FFFFFF
#define CG_UPLL_FUNC_CNTL_4				0x854
#	define UPLL_SPARE_ISPARE9			0x00020000
#define CG_UPLL_SPREAD_SPECTRUM				0x79c
#	define SSEN_MASK				0x00000001

/* The UVD block itself (r600d.h) */
#define UVD_SEMA_ADDR_LOW				0xef00
#define UVD_SEMA_ADDR_HIGH				0xef04
#define UVD_SEMA_CMD					0xef08
#define UVD_GPCOM_VCPU_CMD				0xef0c
#define UVD_GPCOM_VCPU_DATA0				0xef10
#define UVD_GPCOM_VCPU_DATA1				0xef14
#define UVD_ENGINE_CNTL					0xef18
#define UVD_NO_OP					0xeffc
#define UVD_SEMA_CNTL					0xf400
#define UVD_RB_ARB_CTRL					0xf480
#define UVD_LMI_EXT40_ADDR				0xf498
#define UVD_CGC_GATE					0xf4a8
#define UVD_LMI_CTRL2					0xf4f4
#define UVD_MASTINT_EN					0xf500
#define UVD_FW_START					0xf51C
#define UVD_LMI_ADDR_EXT				0xf594
#define UVD_LMI_CTRL					0xf598
#define UVD_LMI_SWAP_CNTL				0xf5b4
#define UVD_MP_SWAP_CNTL				0xf5bC
#define UVD_MPC_CNTL					0xf5dC
#define UVD_MPC_SET_MUXA0				0xf5e4
#define UVD_MPC_SET_MUXA1				0xf5e8
#define UVD_MPC_SET_MUXB0				0xf5eC
#define UVD_MPC_SET_MUXB1				0xf5f0
#define UVD_MPC_SET_MUX					0xf5f4
#define UVD_MPC_SET_ALU					0xf5f8
#define UVD_VCPU_CHIP_ID_REG				0xf4d4
/*
 * Where the VCPU's three memory windows are set: Evergreen's layout
 * (rv770d.h, which uvd_v2_2.c uses; a Linux trace of this card writes
 * 0xf4d8-0xf4ec), and the R6xx one (r600d.h, uvd_v1_0.c) kept for the
 * RDN_UVD_F_V1_MEM variant. Using the second on this chip left the VCPU
 * without a firmware address: it never answered.
 */
#define UVD_VCPU_CACHE_OFFSET0				0xf4d8
#define UVD_VCPU_CACHE_SIZE0				0xf4dc
#define UVD_VCPU_CACHE_OFFSET1				0xf4e0
#define UVD_VCPU_CACHE_SIZE1				0xf4e4
#define UVD_VCPU_CACHE_OFFSET2				0xf4e8
#define UVD_VCPU_CACHE_SIZE2				0xf4ec
#define UVD_R6XX_VCPU_CACHE_OFFSET0			0xf608
#define UVD_R6XX_VCPU_CACHE_SIZE0			0xf60c
#define UVD_R6XX_VCPU_CACHE_OFFSET1			0xf610
#define UVD_R6XX_VCPU_CACHE_SIZE1			0xf614
#define UVD_R6XX_VCPU_CACHE_OFFSET2			0xf618
#define UVD_R6XX_VCPU_CACHE_SIZE2			0xf61c
/* The decoder's tiling follows the 3D engine's (evergreen_gpu_init()). */
#define UVD_UDEC_ADDR_CONFIG				0xef4c
#define UVD_UDEC_DB_ADDR_CONFIG				0xef50
#define UVD_UDEC_DBW_ADDR_CONFIG			0xef54
#define UVD_VCPU_CNTL					0xf660
#define UVD_SOFT_RESET					0xf680
#define		RBC_SOFT_RESET					(1<<0)
#define		LBSI_SOFT_RESET					(1<<1)
#define		LMI_SOFT_RESET					(1<<2)
#define		VCPU_SOFT_RESET					(1<<3)
#define		CSM_SOFT_RESET					(1<<5)
#define		CXW_SOFT_RESET					(1<<6)
#define		TAP_SOFT_RESET					(1<<7)
#define		LMI_UMC_SOFT_RESET				(1<<13)
#define UVD_RBC_IB_BASE					0xf684
#define UVD_RBC_IB_SIZE					0xf688
#define UVD_RBC_RB_BASE					0xf68c
#define UVD_RBC_RB_RPTR					0xf690
#define UVD_RBC_RB_WPTR					0xf694
#define UVD_RBC_RB_WPTR_CNTL				0xf698
#define UVD_STATUS					0xf6bc
#define UVD_SEMA_TIMEOUT_STATUS				0xf6c0
#define UVD_SEMA_WAIT_INCOMPLETE_TIMEOUT_CNTL		0xf6c4
#define UVD_SEMA_WAIT_FAULT_TIMEOUT_CNTL		0xf6c8
#define UVD_SEMA_SIGNAL_INCOMPLETE_TIMEOUT_CNTL		0xf6cc
#define UVD_RBC_RB_CNTL					0xf6a4
#define UVD_RBC_RB_RPTR_ADDR				0xf6a8
#define UVD_CONTEXT_ID					0xf6f4

/* Type 0 packet: write `n + 1` registers from `reg` (rdn_cp.h has type 3). */
#define RDN_UVD_PACKET0(reg, n)	((((reg) >> 2) & 0xFFFF) | (((n) & 0x3FFF) << 16))

#define RLC_CNTL                         0x3f00
#define RLC_ENABLE                       (1u << 0)
#define RLC_HB_CNTL                      0x3f0c
#define RLC_HB_BASE                      0x3f10
#define RLC_HB_RPTR                      0x3f20
#define RLC_HB_WPTR                      0x3f1c
#define RLC_HB_WPTR_LSB_ADDR              0x3f14
#define RLC_HB_WPTR_MSB_ADDR              0x3f18
#define RLC_UCODE_ADDR                   0x3f2c
#define RLC_UCODE_DATA                   0x3f30
#define RLC_MC_CNTL                      0x3f44
#define RLC_UCODE_CNTL                   0x3f48
#define UVD_CGC_STATUS					0xf4ac
#define UVD_CGC_CTRL					0xf4b0
#define UVD_LMI_STATUS					0xf59c
#define UVD_VCPU_CACHE_OFFSET_MASK			0x01ffffffu

#endif /* RDN_UVD_REG_H */
