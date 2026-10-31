#ifndef RDN_CAYMAN_GATING_H
#define RDN_CAYMAN_GATING_H
#include "rdn_card.h"
struct rdn_cayman_gate_saved { uint32_t reg,value,gfx,cg,srbm; };
struct rdn_cayman_gating {
 struct rdn_cayman_gate_saved saved[640];
 unsigned count;
 uint32_t gfx,cg,srbm;
 bool prepared, enabled;
};
int rdn_cayman_gating_prepare(struct rdn_card *,struct rdn_cayman_gating *);
int rdn_cayman_gating_enable(struct rdn_card *,struct rdn_cayman_gating *);
void rdn_cayman_gating_restore(struct rdn_card *,struct rdn_cayman_gating *);
#endif
