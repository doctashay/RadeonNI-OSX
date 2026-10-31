/* Cayman GDDR5 tables and display constraints. */
#ifndef RDN_CAYMAN_MEMORY_H
#define RDN_CAYMAN_MEMORY_H
#include "rdn_pm.h"
#include "linux/nislands_smc.h"
struct rdn_cayman_memory {
 SMC_NIslands_MCRegisters regs;
 SMC_NIslands_MCArbDramTimingRegisters arb;
 uint32_t clock_registers[8];
 bool spread_enabled;
 uint16_t address, lp[13];
 uint32_t saved_lp[13], saved_gap, saved_select, initial_sclk, initial_mclk;
 unsigned lp_count;
 bool prepared, touched;
};
int rdn_cayman_memory_prepare(struct rdn_card *,struct rdn_cayman_memory *,const struct rdn_pm_state *,unsigned);
int rdn_cayman_memory_value(struct rdn_card *,const struct rdn_cayman_memory *,uint32_t,NISLANDS_SMC_HW_PERFORMANCE_LEVEL *);
int rdn_cayman_memory_timings(struct rdn_card *,struct rdn_cayman_memory *,const struct rdn_pm_state *,unsigned);
bool rdn_cayman_memory_display(struct rdn_card *,unsigned *,unsigned *);
void rdn_cayman_memory_restore(struct rdn_card *,struct rdn_cayman_memory *);
#endif
