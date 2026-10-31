/*
 * Copyright 2012 Advanced Micro Devices, Inc.
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
 */

/* Indexed register state is retained for teardown and failed startup. */
#include "rdn_cayman_gating.h"
#include "linux/cayman_power_gating.h"
static int sequence(struct rdn_card *card,struct rdn_cayman_gating *g,const uint32_t *p,unsigned words)
{
 for(unsigned i=0;i<words;i+=3) {
  unsigned reg=p[i];uint32_t value=rdn_rreg(card,reg);
  if(reg!=0x802c && reg!=0x8f8 && reg!=0x3fc4) {
   uint32_t gfx=rdn_rreg(card,0x802c),cg=rdn_rreg(card,0x8f8),srbm=rdn_rreg(card,0x3fc4);
   unsigned banks=(gfx&(1u<<30)) && reg>=0x8000?2:1;
   if(g->count+banks>640)return -EINVAL;
   for(unsigned bank=0;bank<banks;bank++) {
    uint32_t context=banks==2?(gfx&~((1u<<30)|(0xffu<<16)))|(bank<<16):gfx;
    rdn_wreg(card,0x802c,context);
    struct rdn_cayman_gate_saved *s=&g->saved[g->count++];
    s->reg=reg;s->value=rdn_rreg(card,reg);s->gfx=context;s->cg=cg;s->srbm=srbm;
   }
   rdn_wreg(card,0x802c,gfx);
  }
  rdn_wreg(card,reg,(value&~p[i+2])|(p[i+1]&p[i+2]));
 }
 return 0;
}
static void selectors(struct rdn_card *card,const struct rdn_cayman_gating *g)
{
 rdn_wreg(card,0x802c,g->gfx);rdn_wreg(card,0x8f8,g->cg);rdn_wreg(card,0x3fc4,g->srbm);
}
static void disable_sequence(struct rdn_card *card,const uint32_t *p,unsigned words)
{
 for(unsigned i=0;i<words;i+=3)rdn_wreg(card,p[i],(rdn_rreg(card,p[i])&~p[i+2])|(p[i+1]&p[i+2]));
}
void rdn_cayman_gating_restore(struct rdn_card *card,struct rdn_cayman_gating *g)
{
 if(!g->count && !g->prepared && !g->enabled)return;
 if(g->enabled) {
  disable_sequence(card,cayman_cgcg_cgls_disable,sizeof(cayman_cgcg_cgls_disable)/sizeof(uint32_t));
  disable_sequence(card,cayman_mgcg_disable,sizeof(cayman_mgcg_disable)/sizeof(uint32_t));
 }
 while(g->count) {
  const struct rdn_cayman_gate_saved *s=&g->saved[--g->count];
  rdn_wreg(card,0x802c,s->gfx);rdn_wreg(card,0x8f8,s->cg);rdn_wreg(card,0x3fc4,s->srbm);rdn_wreg(card,s->reg,s->value);
 }
 selectors(card,g);g->prepared=g->enabled=false;
}
int rdn_cayman_gating_prepare(struct rdn_card *card,struct rdn_cayman_gating *g)
{
 if(card->family!=RDN_FAMILY_CAYMAN || g->prepared || g->enabled || g->count)return -EINVAL;
 g->gfx=rdn_rreg(card,0x802c);g->cg=rdn_rreg(card,0x8f8);g->srbm=rdn_rreg(card,0x3fc4);
 int err=sequence(card,g,cayman_cgcg_cgls_default,sizeof(cayman_cgcg_cgls_default)/sizeof(uint32_t));
 if(!err)err=sequence(card,g,cayman_mgcg_default,sizeof(cayman_mgcg_default)/sizeof(uint32_t));
 if(err){rdn_cayman_gating_restore(card,g);return err;}
 selectors(card,g);g->prepared=true;return 0;
}
int rdn_cayman_gating_enable(struct rdn_card *card,struct rdn_cayman_gating *g)
{
 if(card->family!=RDN_FAMILY_CAYMAN || g->enabled)return -EINVAL;
 int err=0;if(!g->prepared)err=rdn_cayman_gating_prepare(card,g);
 if(!err)err=sequence(card,g,cayman_cgcg_cgls_enable,sizeof(cayman_cgcg_cgls_enable)/sizeof(uint32_t));
 if(!err)err=sequence(card,g,cayman_mgcg_enable,sizeof(cayman_mgcg_enable)/sizeof(uint32_t));
 if(err){rdn_cayman_gating_restore(card,g);return err;}
 selectors(card,g);g->enabled=true;return 0;
}
