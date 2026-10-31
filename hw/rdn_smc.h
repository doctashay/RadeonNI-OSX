/* Cayman SMC access. The caller serializes all MMIO and SRAM operations. */
#ifndef RDN_SMC_H
#define RDN_SMC_H
#include "rdn_card.h"
#define RDN_SMC_RAM_END 0xc000u
#define RDN_CAYMAN_SMC_SIZE 0x79ecu
int rdn_smc_read(struct rdn_card *, uint32_t address, uint32_t *value);
int rdn_smc_write(struct rdn_card *, uint32_t address, uint32_t value);
int rdn_smc_copy(struct rdn_card *, uint32_t address, const void *, size_t size);
int rdn_smc_load(struct rdn_card *, const void *firmware, size_t size);
int rdn_smc_message(struct rdn_card *, uint8_t message);
int rdn_smc_halt(struct rdn_card *);
int rdn_smc_start(struct rdn_card *);
void rdn_smc_stop(struct rdn_card *);
#endif
