/* Cold Turks VRAM address layout. SPDX-License-Identifier: MIT */
#ifndef RDN_MC_LAYOUT_H
#define RDN_MC_LAYOUT_H

#include "rdn_card.h"

#define RDN_MC_LAYOUT_CPU_QUIESCED 1u
#define RDN_MC_LAYOUT_NO_GPU_CLIENTS 2u
#define RDN_MC_LAYOUT_SCRATCH_RESERVED 4u
#define RDN_MC_LAYOUT_CONDITIONS 7u
#define RDN_MC_LAYOUT_SCRATCH_OFFSET ((34u << 20) - 4096u)

enum rdn_mc_layout_stage {
	RDN_MC_LAYOUT_PREFLIGHT,
	RDN_MC_LAYOUT_DISPLAY_STOP,
	RDN_MC_LAYOUT_IDLE,
	RDN_MC_LAYOUT_BLACKOUT,
	RDN_MC_LAYOUT_PROGRAM,
	RDN_MC_LAYOUT_RESTORE,
	RDN_MC_LAYOUT_COMPLETE
};

struct rdn_mc_layout_result {
	uint64_t old_base, new_base;
	uint32_t stage, changed, rollback_ok, access_restored;
	uint32_t modeset_required, failed_reg, expected, actual;
};

/* Diagnostic cold start only: 1002:675d, 1 GiB at 0xf00000000, DVI/TMDS.
 * Caller stops every CPU aperture writer and owns scratch_offset permanently.
 * No clients, active VM/RLC/DMA, or running CP are allowed. DP and cursors are
 * rejected before writes. Success leaves CRTCs off and requires a full modeset.
 * If access_restored is zero on error, do not resume console/aperture access.
 * No PCI BAR, memory timing, arbitration, or host-endian register is changed.
 */
int rdn_mc_layout_rebase_zero(struct rdn_card *card, uint32_t conditions,
			    uint32_t scratch_offset,
			    struct rdn_mc_layout_result *result);

#endif
