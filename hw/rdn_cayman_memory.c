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

/* Adapted from Linux Radeon ni_dpm.c, cypress_dpm.c and rv740_dpm.c. */
#include <string.h>
#include "rdn_cayman_memory.h"
#include "linux/nid.h"
#include "rdn_dpm_reg.h"
#define RREG32(r) rdn_rreg(card,r)
#define WREG32(r,v) rdn_wreg(card,r,v)
static uint32_t be32(uint32_t v){return RDN_BIG_ENDIAN?v:(v<<24)|((v&0xff00)<<8)|((v>>8)&0xff00)|(v>>24);}
static uint16_t be16(uint16_t v){return RDN_BIG_ENDIAN?v:(uint16_t)((v<<8)|(v>>8));}
static unsigned le16(const uint8_t *p){return p[0]|(p[1]<<8);}
static uint32_t le32(const uint8_t *p){return le16(p)|((uint32_t)le16(p+2)<<16);}
static const uint8_t *table(struct rdn_card *card,unsigned index,unsigned minimum,uint16_t *size)
{
 uint16_t at;
 if(!atom_parse_data_header(card->atom.ctx,index,size,0,0,&at)||*size<minimum)return 0;
 return (const uint8_t *)card->atom.ctx->bios+at;
}
static const uint16_t low_pairs[][2]={
 {MC_SEQ_RAS_TIMING,MC_SEQ_RAS_TIMING_LP},{MC_SEQ_CAS_TIMING,MC_SEQ_CAS_TIMING_LP},
 {MC_SEQ_MISC_TIMING,MC_SEQ_MISC_TIMING_LP},{MC_SEQ_MISC_TIMING2,MC_SEQ_MISC_TIMING2_LP},
 {MC_SEQ_RD_CTL_D0,MC_SEQ_RD_CTL_D0_LP},{MC_SEQ_RD_CTL_D1,MC_SEQ_RD_CTL_D1_LP},
 {MC_SEQ_WR_CTL_D0,MC_SEQ_WR_CTL_D0_LP},{MC_SEQ_WR_CTL_D1,MC_SEQ_WR_CTL_D1_LP},
 {MC_PMG_CMD_EMRS,MC_SEQ_PMG_CMD_EMRS_LP},{MC_PMG_CMD_MRS,MC_SEQ_PMG_CMD_MRS_LP},
 {MC_PMG_CMD_MRS1,MC_SEQ_PMG_CMD_MRS1_LP},{MC_SEQ_PMG_TIMING,MC_SEQ_PMG_TIMING_LP},
 {MC_PMG_CMD_MRS2,MC_SEQ_PMG_CMD_MRS2_LP}
};
static uint16_t low_address(uint16_t reg)
{
 for(unsigned i=0;i<13;i++)if(reg==low_pairs[i][0]/4)return low_pairs[i][1]/4;
 return reg;
}
int rdn_cayman_memory_prepare(struct rdn_card *card,struct rdn_cayman_memory *m,
                              const struct rdn_pm_state *levels,unsigned count)
{
 uint16_t size;const uint8_t *p=table(card,GetIndexIntoMasterTable(DATA,VRAM_Info),20,&size);
 if(!p || card->family!=RDN_FAMILY_CAYMAN || p[2]!=2 || p[3]!=1 || count!=3)return -EINVAL;
 unsigned module=(RREG32(BIOS_SCRATCH_4)>>16)&255,block=le16(p+6);
 if(module>=p[16] || block+4>size)return -EINVAL;
 unsigned indexBytes=le16(p+block),rowBytes=le16(p+block+2);
 if(indexBytes<6 || indexBytes%3 || rowBytes<4 || block+4+indexBytes>size)return -EINVAL;
 unsigned entries=indexBytes/3-1,last=0;uint16_t address[32];uint8_t access[32];
 uint32_t baseline[32]={0},changed[32]={0};
 if(entries>32)return -EINVAL;
 for(unsigned i=0;i<entries;i++) {
  const uint8_t *q=p+block+4+i*3;if(q[2]&0x80)break;
  address[last]=le16(q);access[last]=q[2]&15;
  if(access[last]!=4 && (access[last]!=0 || !last))return -EINVAL;
  if((unsigned)address[last]*4>=RDN_MMIO_SIZE)return -EINVAL;
  last++;
 }
 if(!last)return -EINVAL;
 memset(m,0,sizeof(*m));m->initial_sclk=levels[0].sclk;m->initial_mclk=levels[count-1].mclk;
 const unsigned clock_regs[]={MPLL_AD_FUNC_CNTL,MPLL_AD_FUNC_CNTL_2,MPLL_DQ_FUNC_CNTL,MPLL_DQ_FUNC_CNTL_2,MCLK_PWRMGT_CNTL,DLL_CNTL,MPLL_SS1,MPLL_SS2};
 for(unsigned i=0;i<8;i++)m->clock_registers[i]=RREG32(clock_regs[i]);
 m->spread_enabled=(RREG32(CG_MPLL_SPREAD_SPECTRUM)&SSEN_MASK)!=0;
 unsigned selected[3]={0,0,0},ranges=0,at=block+4+indexBytes;uint32_t best[3]={~0u,~0u,~0u};
 for(;at+4<=size;at+=rowBytes) {
  uint32_t key=le32(p+at);if(!key)break;
  if(at+rowBytes>size)return -EINVAL;
  if((key>>24)!=module)continue;
  if(++ranges>32)return -EINVAL;
  unsigned word=1;uint32_t previous=0;
  for(unsigned i=0;i<last;i++) {
   uint32_t value=previous;
   if(access[i]==4){if(word*4+4>rowBytes)return -EINVAL;value=le32(p+at+word++*4);}
   if(ranges==1)baseline[i]=value;else changed[i]|=baseline[i]^value;
   previous=value;
  }
  for(unsigned j=0;j<count;j++)if(levels[j].mclk<=(key&0xffffff) && (key&0xffffff)<best[j]) {
   best[j]=key&0xffffff;selected[j]=at;
  }
 }
 if(at+4>size || le32(p+at)!=0)return -EINVAL;
 for(unsigned j=0;j<count;j++) {
  if(!selected[j])return -EINVAL;
  unsigned word=1;
  for(unsigned i=0;i<last;i++) {
   uint32_t value;
   if(access[i]==4){if(word*4+4>rowBytes)return -EINVAL;value=le32(p+selected[j]+word++*4);}
   else value=be32(m->regs.data[2+j].value[i-1]);
   m->regs.data[2+j].value[i]=be32(value);
  }
 }
 unsigned total=last;
 for(unsigned i=0;i<last;i++) {
  m->regs.address[i].s1=be16(address[i]);m->regs.address[i].s0=be16(low_address(address[i]));
  uint16_t extra[2];unsigned extras=0;
  if(address[i]==MC_SEQ_MISC1/4){extra[0]=MC_PMG_CMD_EMRS;extra[1]=MC_PMG_CMD_MRS;extras=2;}
  if(address[i]==MC_SEQ_RESERVE_M/4){extra[0]=MC_PMG_CMD_MRS1;extras=1;}
  for(unsigned e=0;e<extras;e++) {
   if(total>=32)return -EINVAL;
   uint16_t reg=extra[e];m->regs.address[total].s1=be16(reg/4);m->regs.address[total].s0=be16(low_address(reg/4));
   uint32_t upper=RREG32(reg)&0xffff0000;
   for(unsigned j=0;j<count;j++) {
    uint32_t v=be32(m->regs.data[2+j].value[i]);
    m->regs.data[2+j].value[total]=be32(upper|(e==0 && address[i]==MC_SEQ_MISC1/4?v>>16:v&0xffff));
   }
   changed[total]=e==0 && address[i]==MC_SEQ_MISC1/4?changed[i]>>16:changed[i]&0xffff;
   total++;
  }
 }
 unsigned packed=0;
 for(unsigned i=0;i<total;i++)if(changed[i]) {
  m->regs.address[packed]=m->regs.address[i];
  for(unsigned j=0;j<count;j++)m->regs.data[2+j].value[packed]=m->regs.data[2+j].value[i];
  packed++;
 }
 memset(m->regs.address+packed,0,(32-packed)*sizeof(m->regs.address[0]));
 for(unsigned j=0;j<count;j++)memset(m->regs.data[2+j].value+packed,0,(32-packed)*sizeof(uint32_t));
 m->regs.last=packed;m->regs.data[1]=m->regs.data[2];m->regs.data[0]=m->regs.data[4];
 m->prepared=true;return 0;
}
static unsigned decoded_ref(unsigned ref)
{
 switch(ref){case 0:return 1;case 16:case 18:return 2;case 17:case 19:return 3;case 20:return 4;case 21:return 5;default:return 0;}
}
static unsigned ratio(unsigned clock,bool strobe)
{
 if(strobe)return clock<10000?0:clock>47500?15:(clock-10000)/2500;
 return clock<65000?0:clock>135000?15:(clock-60000)/5000;
}
int rdn_cayman_memory_value(struct rdn_card *card,const struct rdn_cayman_memory *base,uint32_t clock,NISLANDS_SMC_HW_PERFORMANCE_LEVEL *level)
{
 COMPUTE_MEMORY_ENGINE_PLL_PARAMETERS_V5 args;uint8_t f,c;uint16_t size;
 const uint8_t *fw=table(card,GetIndexIntoMasterTable(DATA,FirmwareInfo),sizeof(ATOM_FIRMWARE_INFO_V2_2),&size);
 if(!fw || fw[2]!=2 || fw[3]!=2 || !clock)return -EINVAL;
 unsigned ref=le16(fw+offsetof(ATOM_FIRMWARE_INFO_V2_2,usMemoryReferenceClock));
 if(ref!=2700 && ref!=10000)return -EINVAL;
 unsigned threshold=(RREG32(MC_SEQ_MISC7)&(1u<<31))?55000:40000;
 bool strobe=clock<=threshold;
 unsigned index=GetIndexIntoMasterTable(COMMAND,ComputeMemoryEnginePLL);
 if(!atom_parse_cmd_header(card->atom.ctx,index,&f,&c) || f!=1 || (c!=3 && c!=5))return -EINVAL;
 memset(&args,0,sizeof(args));args.ulClockParams=cpu_to_le32((COMPUTE_MEMORY_PLL_PARAM<<24)|clock);
 args.ucInputFlag=strobe?ATOM_PLL_INPUT_FLAG_PLL_STROBE_MODE_EN:0;
 int err=atom_execute_table(card->atom.ctx,index,(uint32_t *)&args,sizeof(args));if(err)return err;
 unsigned whole=le16_to_cpu(args.ulFbDiv.usFbDiv),frac=le16_to_cpu(args.ulFbDiv.usFbDivFrac);
 unsigned post=args.ucPostDiv,decode=decoded_ref(args.ucRefDiv);
 if(!strobe && (RREG32(MC_SEQ_MISC7)&0x08000000))post=1;
 if(!decode || post>3 || !whole || whole>127 || frac>31)return -EINVAL;
 uint64_t actual=(uint64_t)ref*(whole*8+frac)/(8*decode*(1u<<(post*2)));
 if(actual>clock+100 || actual+100<clock)return -EINVAL;
 unsigned vco=whole*ref,ibias;
 if(ref==10000)ibias=vco>500000?0xc6:vco>400000?0x9d:vco>330000?0x6c:vco>250000?0x2b:vco>160000?0x5b:vco>120000?0x0a:0x4b;
 else ibias=vco>250000?0x8b:vco>200000?0xcc:vco>150000?0x9b:0x6b;
 uint32_t mask=CLKR_MASK|YCLK_POST_DIV_MASK|CLKF_MASK|CLKFRAC_MASK|IBIAS_MASK;
 uint32_t bits=CLKR(args.ucRefDiv)|YCLK_POST_DIV(post)|CLKF(whole)|CLKFRAC(frac)|IBIAS(ibias);
 uint32_t ad=(base->clock_registers[0]&~mask)|bits,dq=(base->clock_registers[2]&~mask)|bits;
 uint32_t ad2=base->clock_registers[1],dq2=base->clock_registers[3];
 if(args.ucCntlFlag&ATOM_PLL_CNTL_FLAG_MPLL_VCO_MODE){ad2|=VCO_MODE;dq2|=VCO_MODE;}else{ad2&=~VCO_MODE;dq2&=~VCO_MODE;}
 if(strobe)dq&=~PDNB;else dq|=PDNB;
 uint32_t ss1=base->clock_registers[6],ss2=base->clock_registers[7];
 if(base->spread_enabled) {
  const uint8_t *ss=table(card,GetIndexIntoMasterTable(DATA,ASIC_InternalSS_Info),16,&size);bool found=false;
  if(!ss || ss[2]!=3)return -EINVAL;
  for(unsigned off=4;off+12<=size;off+=12)if(ss[off+8]==ASIC_INTERNAL_MEMORY_SS && clock*post<=le32(ss+off)) {
   unsigned rate=le16(ss+off+6)/100,percent=le16(ss+off+4);if(!rate)return -EINVAL;
   unsigned clks=ref*5/(decode*rate);if(!clks || clks>0xfff)return -EINVAL;
   uint64_t clkv=(uint64_t)percent*(0x4000*whole+0x800*frac)/(clks*625);if(clkv>0x3ffffff)return -EINVAL;
   ss1=(ss1&~CLKV_MASK)|CLKV(clkv);ss2=(ss2&~CLKS_MASK)|CLKS(clks);found=true;break;
  }
  if(!found)return -EINVAL;
 }
 static const unsigned bounds[]={320,270,240,200,180,160,140,120,110,95,85,78,70,65,60,42,0};
 unsigned rate=clock*4/1000,speed=15;
 for(unsigned i=0;i<16;i++)if(rate>bounds[i+1] && rate<=bounds[i]){speed=15-i;break;}
 bool dll=strobe && (((ratio(clock,true)>=(RREG32(MC_SEQ_MISC7)>>16&15)?RREG32(MC_SEQ_MISC5):RREG32(MC_SEQ_MISC6))>>1)&1);
 uint32_t power=(base->clock_registers[4]&~(DLL_SPEED_MASK|MPLL_PWRMGT_OFF))|DLL_SPEED(speed);
 if(dll)power|=0xff00;else power&=~0xff00u;
 if(!card->power_memory_dpm)power|=MPLL_PWRMGT_OFF;
 NISLANDS_SMC_MCLK_VALUE *m=&level->mclk;memset(m,0,sizeof(*m));
 m->vMPLL_AD_FUNC_CNTL=be32(ad);m->vMPLL_AD_FUNC_CNTL_2=be32(ad2);m->vMPLL_DQ_FUNC_CNTL=be32(dq);m->vMPLL_DQ_FUNC_CNTL_2=be32(dq2);
 m->vMCLK_PWRMGT_CNTL=be32(power);m->vDLL_CNTL=be32(base->clock_registers[5]);m->vMPLL_SS=be32(ss1);m->vMPLL_SS2=be32(ss2);m->mclk_value=be32(clock);
 level->strobeMode=ratio(clock,strobe)|(strobe?NISLANDS_SMC_STROBE_ENABLE:0);
 level->mcFlags=clock>threshold?NISLANDS_SMC_MC_EDC_RD_FLAG|NISLANDS_SMC_MC_EDC_WR_FLAG|NISLANDS_SMC_MC_RTT_ENABLE:0;
 return 0;
}
int rdn_cayman_memory_timings(struct rdn_card *card,struct rdn_cayman_memory *m,
                              const struct rdn_pm_state *levels,unsigned count)
{
 uint8_t f,c;unsigned index=GetIndexIntoMasterTable(COMMAND,DynamicMemorySettings);
 if(!m->prepared || count!=3 || !atom_parse_cmd_header(card->atom.ctx,index,&f,&c))return -EINVAL;
 uint32_t old1=RREG32(MC_ARB_DRAM_TIMING),old2=RREG32(MC_ARB_DRAM_TIMING2),oldBurst=RREG32(MC_ARB_BURST_TIME);
 int err=0;memset(&m->arb,0,sizeof(m->arb));m->arb.arb_current=0x0b;
 for(unsigned i=0;i<20;i++){m->arb.data[i].mc_arb_dram_timing=be32(old1);m->arb.data[i].mc_arb_dram_timing2=be32(old2);m->arb.data[i].mc_arb_rfsh_rate=RREG32(MC_ARB_RFSH_RATE)&255;}
 if(!card->power_memory_dpm)return 0;
 for(unsigned i=0;i<=count;i++) {
  uint32_t core=i?levels[i-1].sclk:m->initial_sclk,memory=i?levels[i-1].mclk:m->initial_mclk;
  unsigned slot=i?2+i:0;
  SET_ENGINE_CLOCK_PS_ALLOCATION args;memset(&args,0,sizeof(args));
  args.ulTargetEngineClock=cpu_to_le32((COMPUTE_ENGINE_PLL_PARAM<<24)|core);args.sReserved.ulClock=cpu_to_le32(memory);
  err=atom_execute_table(card->atom.ctx,index,(uint32_t *)&args,sizeof(args));if(err)break;
  unsigned rows=1u<<(((RREG32(MC_ARB_RAMCFG)>>3)&7)+10),refresh=1u<<((RREG32(MC_SEQ_MISC0)&3)+3);
  uint64_t cycles=(uint64_t)core*10*refresh/rows;
  if(cycles<32 || (cycles-32)/64>255){err=-EINVAL;break;}
  m->arb.data[slot].mc_arb_dram_timing=be32(RREG32(MC_ARB_DRAM_TIMING));m->arb.data[slot].mc_arb_dram_timing2=be32(RREG32(MC_ARB_DRAM_TIMING2));m->arb.data[slot].mc_arb_rfsh_rate=(cycles-32)/64;
 }
 WREG32(MC_ARB_DRAM_TIMING,old1);WREG32(MC_ARB_DRAM_TIMING2,old2);WREG32(MC_ARB_BURST_TIME,oldBurst);
 if(err)return err;
 if(!m->touched) {
  m->saved_gap=RREG32(CG_DISPLAY_GAP_CNTL);m->saved_select=RREG32(0x4fc);
  for(unsigned i=0;i<13;i++){m->lp[i]=low_pairs[i][1];m->saved_lp[i]=RREG32(m->lp[i]);WREG32(m->lp[i],RREG32(low_pairs[i][0]));}m->lp_count=13;m->touched=true;
 }
 return 0;
}
bool rdn_cayman_memory_display(struct rdn_card *card,unsigned *mask,unsigned *blank_us)
{
 unsigned active=0,count=0,blank=~0u;
 for(unsigned i=0;i<RDN_NUM_CRTC;i++) {
  struct rdn_card *p=card->display_peers?card->display_peers[i]:(i==card->display_crtc?card:0);
  if(!p || !p->crtc_on)continue;
  active|=1u<<p->display_crtc;count++;
  unsigned us=0;
  if(p->wm_clock && p->wm_vtotal>p->wm_vdisplay)us=(uint64_t)p->wm_htotal*(p->wm_vtotal-p->wm_vdisplay)*1000/p->wm_clock;
  if(us<blank)blank=us;
 }
 if(mask)*mask=active;
 if(blank_us)*blank_us=count?blank:0;
 return count==0 || (count==1 && blank>=460);
}
void rdn_cayman_memory_restore(struct rdn_card *card,struct rdn_cayman_memory *m)
{
 if(!m->touched)return;
 for(unsigned i=0;i<m->lp_count;i++)WREG32(m->lp[i],m->saved_lp[i]);
 WREG32(CG_DISPLAY_GAP_CNTL,m->saved_gap);WREG32(0x4fc,m->saved_select);m->touched=false;
}
