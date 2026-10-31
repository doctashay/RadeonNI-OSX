/* Verify indexed and per-SE gating state survives teardown exactly. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../hw/rdn_cayman_gating.h"
static uint32_t regs[0x20000/4],banks[2][0x20000/4],cg[256];
static uint32_t old_regs[0x20000/4],old_banks[2][0x20000/4],old_cg[256];
static uint32_t read32(void *cookie,uint32_t reg)
{
 if(reg==0x8fc)return cg[regs[0x8f8/4]&255];
 if(reg>=0x8000 && reg!=0x802c)return banks[(regs[0x802c/4]>>16)&1][reg/4];
 return regs[reg/4];
}
static void write32(void *cookie,uint32_t reg,uint32_t value)
{
 if(reg==0x8fc){cg[regs[0x8f8/4]&255]=value;return;}
 if(reg>=0x8000 && reg!=0x802c) {
  if(regs[0x802c/4]&(1u<<30))banks[0][reg/4]=banks[1][reg/4]=value;
  else banks[(regs[0x802c/4]>>16)&1][reg/4]=value;
  return;
 }
 regs[reg/4]=value;
}
int main(void)
{
 struct rdn_os os={0};struct rdn_card card={0};struct rdn_cayman_gating *g=calloc(1,sizeof(*g));
 os.mmio_read32=read32;os.mmio_write32=write32;card.os=&os;card.family=RDN_FAMILY_CAYMAN;
 for(unsigned i=0;i<0x20000/4;i++){regs[i]=i^0x12340000;banks[0][i]=i^0x45670000;banks[1][i]=i^0x89ab0000;}
 for(unsigned i=0;i<256;i++)cg[i]=i^0xfedc0000;
 regs[0x802c/4]=0xc0000000;regs[0x8f8/4]=0xa5;
 memcpy(old_regs,regs,sizeof(regs));memcpy(old_banks,banks,sizeof(banks));memcpy(old_cg,cg,sizeof(cg));
 if(rdn_cayman_gating_prepare(&card,g)||!g->prepared||g->enabled)return 1;
 if(regs[0x802c/4]!=old_regs[0x802c/4]||regs[0x8f8/4]!=old_regs[0x8f8/4]||regs[0x3fc4/4]!=old_regs[0x3fc4/4])return 1;
 if(rdn_cayman_gating_enable(&card,g)||!g->enabled||g->count<100)return 1;
 if(regs[0x802c/4]!=old_regs[0x802c/4]||regs[0x8f8/4]!=old_regs[0x8f8/4]||regs[0x3fc4/4]!=old_regs[0x3fc4/4])return 1;
 if(cg[0x10]!=0||cg[0x20]!=0xffffffff)return 1;
 rdn_cayman_gating_restore(&card,g);
 if(g->enabled||g->count||memcmp(regs,old_regs,sizeof(regs))||memcmp(banks,old_banks,sizeof(banks))||memcmp(cg,old_cg,sizeof(cg))){puts("FAIL gating restoration");return 1;}
 card.family=RDN_FAMILY_TURKS;
 if(!rdn_cayman_gating_enable(&card,g)||memcmp(regs,old_regs,sizeof(regs)))return 1;
 free(g);puts("PASS Cayman indexed/per-SE gating restoration");return 0;
}
