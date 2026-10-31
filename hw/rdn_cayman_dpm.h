/* Cayman SMC thermal protection and power containment. */
#ifndef RDN_CAYMAN_DPM_H
#define RDN_CAYMAN_DPM_H
#include "rdn_pm.h"
#define RDN_THERMAL_LIMIT_MIN 80
#define RDN_THERMAL_LIMIT_MAX 100
#define RDN_THERMAL_LIMIT_DEFAULT 90
#include "rdn_smc.h"
#include "rdn_cayman_memory.h"
#include "rdn_cayman_gating.h"
#include "linux/nislands_smc.h"
struct rdn_cayman_calibration {
 uint32_t tdp, near_tdp, sq_threshold, caps, leakage[16];
 uint16_t std_voltage[16], voltage[32], vddci[32];
 uint32_t gpio[32], gpio_mask, vddci_gpio[32], vddci_mask;
 uint16_t voltage_delay, backbias_delay, reference_clock, xclk;
 uint8_t leakage_count, voltage_count, vddci_count;
};
struct rdn_cayman_dpm {
 struct rdn_cayman_calibration calibration;
 struct rdn_cayman_memory memory;
 struct rdn_cayman_gating gating;
 struct rdn_pm_bios_state bios_performance,bios_video,bios_ulv;
 struct rdn_pm_state levels[3];
 NISLANDS_SMC_STATETABLE pending_table;
 struct rdn_cayman_memory pending_memory;
 bool memory_enabled,video,display_changing,idle_requested,video_supported,ulv_supported;
 unsigned display_mask,context_thermal_stage,fan_percent,fan_last_temp,fan_stall_count;
 uint64_t fan_next_change;
 int memory_error;
 NISLANDS_SMC_STATETABLE table;
 PP_NIslands_CACTABLES cac;
 SMC_NISLANDS_SPLL_DIV_TABLE spll;
 uint16_t state_addr, soft_addr, cac_addr, spll_addr, arb_addr;
 uint32_t saved_general, saved_sclk, saved_mclk, saved_thermal;
 uint32_t saved_bsp, saved_git, saved_tpc, saved_ssp, saved_ftv;
 uint32_t saved_ffct[15], saved_cac;
 uint32_t saved_mc_cg, saved_arb_timing1, saved_arb_timing2_1, saved_arb_burst, saved_arb_refresh;
 bool arb_switched;
 bool prepared, touched;
 uint32_t dsp, psp, bsp, pbsp;
 struct rdn_pm_state idle, intermediate, performance;
 unsigned thermal_stage;
 bool enabled, voltage_fault;
 uint64_t undervolt_deadline;
};
int rdn_cayman_dpm_context(struct rdn_card *,struct rdn_cayman_dpm *,bool video,bool display_changing);
int rdn_cayman_dpm_fan(struct rdn_card *,struct rdn_cayman_dpm *,int temp,unsigned limit,bool invalid);
int rdn_cayman_calibration(struct rdn_card *,struct rdn_cayman_calibration *);
int rdn_cayman_dpm_prepare(struct rdn_card *,struct rdn_cayman_dpm *);
int rdn_cayman_dpm_enable(struct rdn_card *,struct rdn_cayman_dpm *,const void *,size_t);
int rdn_cayman_dpm_disable(struct rdn_card *,struct rdn_cayman_dpm *);
unsigned rdn_cayman_thermal_stage(int temp,unsigned limit,unsigned previous,bool invalid);
int rdn_cayman_dpm_policy(struct rdn_card *,struct rdn_cayman_dpm *,bool high,bool low);
int rdn_cayman_dpm_voltage(struct rdn_card *,struct rdn_cayman_dpm *,unsigned mv);
int rdn_cayman_dpm_force_high(struct rdn_card *);
int rdn_cayman_dpm_force_low(struct rdn_card *,bool low);
int rdn_cayman_dpm_temperature_limit(struct rdn_card *,unsigned degrees);
#endif
