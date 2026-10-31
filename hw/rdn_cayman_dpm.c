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

/* Adapted from Linux v6.12 ni_dpm.c, rv770_dpm.c and radeon_atombios.c. */
#include <string.h>
#include "rdn_cayman_dpm.h"
#include "linux/nid.h"
#include "rdn_dpm_reg.h"
#include "linux/ppsmc.h"
#ifndef EIO
#define EIO 5
#endif
#ifndef ETIMEDOUT
#define ETIMEDOUT 110
#endif
#define RREG32(r) rdn_rreg(card,r)
#define WREG32(r,v) rdn_wreg(card,r,v)
static uint32_t cg_read(struct rdn_card *card,uint32_t r){WREG32(CG_IND_ADDR,r);return RREG32(CG_IND_DATA);}
static void cg_write(struct rdn_card *card,uint32_t r,uint32_t v){WREG32(CG_IND_ADDR,r);WREG32(CG_IND_DATA,v);}
#define RREG32_CG(r) cg_read(card,r)
#define WREG32_CG(r,v) cg_write(card,r,v)
static uint32_t be32(uint32_t v){return RDN_BIG_ENDIAN?v:(v<<24)|((v&0xff00)<<8)|((v>>8)&0xff00)|(v>>24);}
static uint16_t be16(uint16_t v){return RDN_BIG_ENDIAN?v:(uint16_t)((v<<8)|(v>>8));}
static void update(struct rdn_card *card,uint32_t reg,uint32_t mask,uint32_t bits){WREG32(reg,(RREG32(reg)&~mask)|bits);}

#define NISLANDS_DCCAC_MAX_LEVELS 8
struct ni_cac_weights {
	uint32_t weight_tcp_sig0;
	uint32_t weight_tcp_sig1;
	uint32_t weight_ta_sig;
	uint32_t weight_tcc_en0;
	uint32_t weight_tcc_en1;
	uint32_t weight_tcc_en2;
	uint32_t weight_cb_en0;
	uint32_t weight_cb_en1;
	uint32_t weight_cb_en2;
	uint32_t weight_cb_en3;
	uint32_t weight_db_sig0;
	uint32_t weight_db_sig1;
	uint32_t weight_db_sig2;
	uint32_t weight_db_sig3;
	uint32_t weight_sxm_sig0;
	uint32_t weight_sxm_sig1;
	uint32_t weight_sxm_sig2;
	uint32_t weight_sxs_sig0;
	uint32_t weight_sxs_sig1;
	uint32_t weight_xbr_0;
	uint32_t weight_xbr_1;
	uint32_t weight_xbr_2;
	uint32_t weight_spi_sig0;
	uint32_t weight_spi_sig1;
	uint32_t weight_spi_sig2;
	uint32_t weight_spi_sig3;
	uint32_t weight_spi_sig4;
	uint32_t weight_spi_sig5;
	uint32_t weight_lds_sig0;
	uint32_t weight_lds_sig1;
	uint32_t weight_sc;
	uint32_t weight_bif;
	uint32_t weight_cp;
	uint32_t weight_pa_sig0;
	uint32_t weight_pa_sig1;
	uint32_t weight_vgt_sig0;
	uint32_t weight_vgt_sig1;
	uint32_t weight_vgt_sig2;
	uint32_t weight_dc_sig0;
	uint32_t weight_dc_sig1;
	uint32_t weight_dc_sig2;
	uint32_t weight_dc_sig3;
	uint32_t weight_uvd_sig0;
	uint32_t weight_uvd_sig1;
	uint32_t weight_spare0;
	uint32_t weight_spare1;
	uint32_t weight_sq_vsp;
	uint32_t weight_sq_vsp0;
	uint32_t weight_sq_gpr;
	uint32_t ovr_mode_spare_0;
	uint32_t ovr_val_spare_0;
	uint32_t ovr_mode_spare_1;
	uint32_t ovr_val_spare_1;
	uint32_t vsp;
	uint32_t vsp0;
	uint32_t gpr;
	uint8_t mc_read_weight;
	uint8_t mc_write_weight;
	uint32_t tid_cnt;
	uint32_t tid_unit;
	uint32_t l2_lta_window_size;
	uint32_t lts_truncate;
	uint32_t dc_cac[NISLANDS_DCCAC_MAX_LEVELS];
	uint32_t pcie_cac[SMC_NISLANDS_BIF_LUT_NUM_OF_ENTRIES];
	bool enable_power_containment_by_default;
};
static const struct ni_cac_weights cac_weights_cayman_le =
{
	0x7,
	0xE,
	0x1,
	0xA,
	0x1,
	0x3F,
	0x2,
	0x18,
	0x10,
	0x1A,
	0x1,
	0x3F,
	0x1,
	0xE,
	0x6,
	0x6,
	0x6,
	0x2,
	0x4,
	0x9,
	0x1A,
	0x1A,
	0x2C,
	0xA,
	0x11,
	0x8,
	0x19,
	0x19,
	0x1,
	0x1,
	0x1A,
	0,
	0x8,
	0x5,
	0x8,
	0x1,
	0x3,
	0x1,
	0,
	0,
	0,
	0,
	0,
	0,
	0x38,
	0x38,
	0x239,
	0x3,
	0x18A,
	1,
	1,
	1,
	1,
	12,
	12,
	12,
	0x15,
	0x22,
	132,
	6,
	6,
	0,
	{ 0, 0, 0, 0, 0, 0, 0, 0 },
	{ 0, 0, 0, 0 },
	true
};

#define NISLANDS_MGCG_SEQUENCE  300

static int initialize_cac_manager(struct rdn_card *card)
{
	const struct ni_cac_weights *weights = &cac_weights_cayman_le;
	uint32_t reg;

	reg = RREG32_CG(CG_CAC_REGION_1_WEIGHT_0) & ~(WEIGHT_TCP_SIG0_MASK |
						      WEIGHT_TCP_SIG1_MASK |
						      WEIGHT_TA_SIG_MASK);
	reg |= (WEIGHT_TCP_SIG0(weights->weight_tcp_sig0) |
		WEIGHT_TCP_SIG1(weights->weight_tcp_sig1) |
		WEIGHT_TA_SIG(weights->weight_ta_sig));
	WREG32_CG(CG_CAC_REGION_1_WEIGHT_0, reg);

	reg = RREG32_CG(CG_CAC_REGION_1_WEIGHT_1) & ~(WEIGHT_TCC_EN0_MASK |
						      WEIGHT_TCC_EN1_MASK |
						      WEIGHT_TCC_EN2_MASK);
	reg |= (WEIGHT_TCC_EN0(weights->weight_tcc_en0) |
		WEIGHT_TCC_EN1(weights->weight_tcc_en1) |
		WEIGHT_TCC_EN2(weights->weight_tcc_en2));
	WREG32_CG(CG_CAC_REGION_1_WEIGHT_1, reg);

	reg = RREG32_CG(CG_CAC_REGION_2_WEIGHT_0) & ~(WEIGHT_CB_EN0_MASK |
						      WEIGHT_CB_EN1_MASK |
						      WEIGHT_CB_EN2_MASK |
						      WEIGHT_CB_EN3_MASK);
	reg |= (WEIGHT_CB_EN0(weights->weight_cb_en0) |
		WEIGHT_CB_EN1(weights->weight_cb_en1) |
		WEIGHT_CB_EN2(weights->weight_cb_en2) |
		WEIGHT_CB_EN3(weights->weight_cb_en3));
	WREG32_CG(CG_CAC_REGION_2_WEIGHT_0, reg);

	reg = RREG32_CG(CG_CAC_REGION_2_WEIGHT_1) & ~(WEIGHT_DB_SIG0_MASK |
						      WEIGHT_DB_SIG1_MASK |
						      WEIGHT_DB_SIG2_MASK |
						      WEIGHT_DB_SIG3_MASK);
	reg |= (WEIGHT_DB_SIG0(weights->weight_db_sig0) |
		WEIGHT_DB_SIG1(weights->weight_db_sig1) |
		WEIGHT_DB_SIG2(weights->weight_db_sig2) |
		WEIGHT_DB_SIG3(weights->weight_db_sig3));
	WREG32_CG(CG_CAC_REGION_2_WEIGHT_1, reg);

	reg = RREG32_CG(CG_CAC_REGION_2_WEIGHT_2) & ~(WEIGHT_SXM_SIG0_MASK |
						      WEIGHT_SXM_SIG1_MASK |
						      WEIGHT_SXM_SIG2_MASK |
						      WEIGHT_SXS_SIG0_MASK |
						      WEIGHT_SXS_SIG1_MASK);
	reg |= (WEIGHT_SXM_SIG0(weights->weight_sxm_sig0) |
		WEIGHT_SXM_SIG1(weights->weight_sxm_sig1) |
		WEIGHT_SXM_SIG2(weights->weight_sxm_sig2) |
		WEIGHT_SXS_SIG0(weights->weight_sxs_sig0) |
		WEIGHT_SXS_SIG1(weights->weight_sxs_sig1));
	WREG32_CG(CG_CAC_REGION_2_WEIGHT_2, reg);

	reg = RREG32_CG(CG_CAC_REGION_3_WEIGHT_0) & ~(WEIGHT_XBR_0_MASK |
						      WEIGHT_XBR_1_MASK |
						      WEIGHT_XBR_2_MASK |
						      WEIGHT_SPI_SIG0_MASK);
	reg |= (WEIGHT_XBR_0(weights->weight_xbr_0) |
		WEIGHT_XBR_1(weights->weight_xbr_1) |
		WEIGHT_XBR_2(weights->weight_xbr_2) |
		WEIGHT_SPI_SIG0(weights->weight_spi_sig0));
	WREG32_CG(CG_CAC_REGION_3_WEIGHT_0, reg);

	reg = RREG32_CG(CG_CAC_REGION_3_WEIGHT_1) & ~(WEIGHT_SPI_SIG1_MASK |
						      WEIGHT_SPI_SIG2_MASK |
						      WEIGHT_SPI_SIG3_MASK |
						      WEIGHT_SPI_SIG4_MASK |
						      WEIGHT_SPI_SIG5_MASK);
	reg |= (WEIGHT_SPI_SIG1(weights->weight_spi_sig1) |
		WEIGHT_SPI_SIG2(weights->weight_spi_sig2) |
		WEIGHT_SPI_SIG3(weights->weight_spi_sig3) |
		WEIGHT_SPI_SIG4(weights->weight_spi_sig4) |
		WEIGHT_SPI_SIG5(weights->weight_spi_sig5));
	WREG32_CG(CG_CAC_REGION_3_WEIGHT_1, reg);

	reg = RREG32_CG(CG_CAC_REGION_4_WEIGHT_0) & ~(WEIGHT_LDS_SIG0_MASK |
						      WEIGHT_LDS_SIG1_MASK |
						      WEIGHT_SC_MASK);
	reg |= (WEIGHT_LDS_SIG0(weights->weight_lds_sig0) |
		WEIGHT_LDS_SIG1(weights->weight_lds_sig1) |
		WEIGHT_SC(weights->weight_sc));
	WREG32_CG(CG_CAC_REGION_4_WEIGHT_0, reg);

	reg = RREG32_CG(CG_CAC_REGION_4_WEIGHT_1) & ~(WEIGHT_BIF_MASK |
						      WEIGHT_CP_MASK |
						      WEIGHT_PA_SIG0_MASK |
						      WEIGHT_PA_SIG1_MASK |
						      WEIGHT_VGT_SIG0_MASK);
	reg |= (WEIGHT_BIF(weights->weight_bif) |
		WEIGHT_CP(weights->weight_cp) |
		WEIGHT_PA_SIG0(weights->weight_pa_sig0) |
		WEIGHT_PA_SIG1(weights->weight_pa_sig1) |
		WEIGHT_VGT_SIG0(weights->weight_vgt_sig0));
	WREG32_CG(CG_CAC_REGION_4_WEIGHT_1, reg);

	reg = RREG32_CG(CG_CAC_REGION_4_WEIGHT_2) & ~(WEIGHT_VGT_SIG1_MASK |
						      WEIGHT_VGT_SIG2_MASK |
						      WEIGHT_DC_SIG0_MASK |
						      WEIGHT_DC_SIG1_MASK |
						      WEIGHT_DC_SIG2_MASK);
	reg |= (WEIGHT_VGT_SIG1(weights->weight_vgt_sig1) |
		WEIGHT_VGT_SIG2(weights->weight_vgt_sig2) |
		WEIGHT_DC_SIG0(weights->weight_dc_sig0) |
		WEIGHT_DC_SIG1(weights->weight_dc_sig1) |
		WEIGHT_DC_SIG2(weights->weight_dc_sig2));
	WREG32_CG(CG_CAC_REGION_4_WEIGHT_2, reg);

	reg = RREG32_CG(CG_CAC_REGION_4_WEIGHT_3) & ~(WEIGHT_DC_SIG3_MASK |
						      WEIGHT_UVD_SIG0_MASK |
						      WEIGHT_UVD_SIG1_MASK |
						      WEIGHT_SPARE0_MASK |
						      WEIGHT_SPARE1_MASK);
	reg |= (WEIGHT_DC_SIG3(weights->weight_dc_sig3) |
		WEIGHT_UVD_SIG0(weights->weight_uvd_sig0) |
		WEIGHT_UVD_SIG1(weights->weight_uvd_sig1) |
		WEIGHT_SPARE0(weights->weight_spare0) |
		WEIGHT_SPARE1(weights->weight_spare1));
	WREG32_CG(CG_CAC_REGION_4_WEIGHT_3, reg);

	reg = RREG32_CG(CG_CAC_REGION_5_WEIGHT_0) & ~(WEIGHT_SQ_VSP_MASK |
						      WEIGHT_SQ_VSP0_MASK);
	reg |= (WEIGHT_SQ_VSP(weights->weight_sq_vsp) |
		WEIGHT_SQ_VSP0(weights->weight_sq_vsp0));
	WREG32_CG(CG_CAC_REGION_5_WEIGHT_0, reg);

	reg = RREG32_CG(CG_CAC_REGION_5_WEIGHT_1) & ~(WEIGHT_SQ_GPR_MASK);
	reg |= WEIGHT_SQ_GPR(weights->weight_sq_gpr);
	WREG32_CG(CG_CAC_REGION_5_WEIGHT_1, reg);

	reg = RREG32_CG(CG_CAC_REGION_4_OVERRIDE_4) & ~(OVR_MODE_SPARE_0_MASK |
							OVR_VAL_SPARE_0_MASK |
							OVR_MODE_SPARE_1_MASK |
							OVR_VAL_SPARE_1_MASK);
	reg |= (OVR_MODE_SPARE_0(weights->ovr_mode_spare_0) |
		OVR_VAL_SPARE_0(weights->ovr_val_spare_0) |
		OVR_MODE_SPARE_1(weights->ovr_mode_spare_1) |
		OVR_VAL_SPARE_1(weights->ovr_val_spare_1));
	WREG32_CG(CG_CAC_REGION_4_OVERRIDE_4, reg);

	reg = RREG32(SQ_CAC_THRESHOLD) & ~(VSP_MASK |
					   VSP0_MASK |
					   GPR_MASK);
	reg |= (VSP(weights->vsp) |
		VSP0(weights->vsp0) |
		GPR(weights->gpr));
	WREG32(SQ_CAC_THRESHOLD, reg);

	reg = (MCDW_WR_ENABLE |
	       MCDX_WR_ENABLE |
	       MCDY_WR_ENABLE |
	       MCDZ_WR_ENABLE |
	       INDEX(0x09D4));
	WREG32(MC_CG_CONFIG, reg);

	reg = (READ_WEIGHT(weights->mc_read_weight) |
	       WRITE_WEIGHT(weights->mc_write_weight) |
	       ALLOW_OVERFLOW);
	WREG32(MC_CG_DATAPORT, reg);

	return 0;
}


static const uint8_t *data(struct rdn_card *card,unsigned index,unsigned minimum,uint16_t *size)
{
 uint16_t offset;uint8_t f,c;
 if(!atom_parse_data_header(card->atom.ctx,index,size,&f,&c,&offset)||*size<minimum)return 0;
 return (const uint8_t *)card->atom.ctx->bios+offset;
}
static int voltage_table(struct rdn_card *card,unsigned type,uint16_t *voltage,uint32_t *gpio,uint32_t *mask,uint8_t *count)
{
 uint16_t size;const uint8_t *p=data(card,GetIndexIntoMasterTable(DATA,VoltageObjectInfo),4,&size);
 unsigned off=4;uint8_t f,c;
 if(!p||p[2]!=1||p[3]!=2)return -EINVAL;
 if(!atom_parse_cmd_header(card->atom.ctx,GetIndexIntoMasterTable(COMMAND,SetVoltage),&f,&c)||c!=2)return -EINVAL;
 while(off+22<=size) {
  unsigned n=p[off+18],bytes=22+4*n;
  if(!n||n>32||bytes>size-off)return -EINVAL;
  if(p[off]==type) {
   if(p[off+2]!=VOLTAGE_CONTROLLED_BY_GPIO)return -EINVAL;
   for(unsigned i=0;i<n;i++) {
    uint32_t args[8]={0};int err;uint32_t m;
    voltage[i]=rdn_get_le16(p,off+24+4*i);
    if(voltage[i]<500||voltage[i]>1500||(i&&voltage[i]<voltage[i-1]))return -EINVAL;
    args[0]=cpu_to_le32(type|(SET_ASIC_VOLTAGE_MODE_GET_GPIOMASK<<8)|((uint32_t)voltage[i]<<16));
    err=atom_execute_table(card->atom.ctx,GetIndexIntoMasterTable(COMMAND,SetVoltage),args,sizeof(args));if(err)return err;
    m=le32_to_cpu(args[0]);
    if(!m||(i&&*mask!=m))return -EINVAL;
    *mask=m;
    memset(args,0,sizeof(args));args[0]=cpu_to_le32(type|(SET_ASIC_VOLTAGE_MODE_GET_GPIOVAL<<8)|((uint32_t)voltage[i]<<16));
    err=atom_execute_table(card->atom.ctx,GetIndexIntoMasterTable(COMMAND,SetVoltage),args,sizeof(args));if(err)return err;
    gpio[i]=le32_to_cpu(args[0]);if(gpio[i]&~m)return -EINVAL;
   }
   *count=n;return 0;
  }
  off+=bytes;
 }
 return -EINVAL;
}
int rdn_cayman_calibration(struct rdn_card *card,struct rdn_cayman_calibration *result)
{
 struct rdn_cayman_calibration c;uint16_t size;const uint8_t *p;
 if(card->family!=RDN_FAMILY_CAYMAN||!result)return -EINVAL;
 memset(&c,0,sizeof(c));
 p=data(card,GetIndexIntoMasterTable(DATA,PowerPlayInfo),88,&size);
 if(!p||p[2]!=5||rdn_get_le16(p,19)<88||p[25]!=15)return -EINVAL;
 c.caps=rdn_get_le32(p,21);c.tdp=rdn_get_le32(p,66);c.near_tdp=rdn_get_le32(p,70);c.sq_threshold=rdn_get_le32(p,74);
 c.backbias_delay=rdn_get_le16(p,15);c.voltage_delay=rdn_get_le16(p,17);
 unsigned off=rdn_get_le16(p,78);if(!off||off>=size)return -EINVAL;
 unsigned n=p[off];if(!n||n>16||1+6*n>size-off)return -EINVAL;
 c.leakage_count=n;
 for(unsigned i=0;i<n;i++) {
  c.std_voltage[i]=rdn_get_le16(p,off+1+i*6);c.leakage[i]=rdn_get_le32(p,off+3+i*6);
  if(c.std_voltage[i]<500||c.std_voltage[i]>1500||!c.leakage[i]||c.leakage[i]>c.tdp)return -EINVAL;
 }
 if(!c.tdp||c.tdp>150||!c.near_tdp||c.near_tdp>c.tdp||!c.sq_threshold)return -EINVAL;
 p=data(card,GetIndexIntoMasterTable(DATA,FirmwareInfo),sizeof(ATOM_FIRMWARE_INFO_V2_2),&size);
 if(!p||p[2]!=2||p[3]!=2)return -EINVAL;
 c.reference_clock=rdn_get_le16(p,offsetof(ATOM_FIRMWARE_INFO_V2_2,usCoreReferenceClock));
 if(c.reference_clock<1000||c.reference_clock>10000)return -EINVAL;
 c.xclk=(RREG32(CG_CLKPIN_CNTL)&XTALIN_DIVIDE)?c.reference_clock/4:c.reference_clock;
 if(RREG32(CG_CLKPIN_CNTL)&MUX_TCLK_TO_XCLK)return -EINVAL;
 int err=voltage_table(card,SET_VOLTAGE_TYPE_ASIC_VDDC,c.voltage,c.gpio,&c.gpio_mask,&c.voltage_count);if(err)return err;
 err=voltage_table(card,SET_VOLTAGE_TYPE_ASIC_VDDCI,c.vddci,c.vddci_gpio,&c.vddci_mask,&c.vddci_count);if(err)return err;
 if(c.leakage_count!=c.voltage_count||(c.gpio_mask&c.vddci_mask))return -EINVAL;
 *result=c;return 0;
}
static int voltage_value(const uint16_t *values,unsigned n,unsigned mv,NISLANDS_SMC_VOLTAGE_VALUE *value)
{
 for(unsigned i=0;i<n;i++)if(mv<=values[i]){value->index=i;value->value=be16(values[i]);return 0;}
 return -EINVAL;
}
static void sample_period(unsigned interval,unsigned ref,uint32_t *p,uint32_t *u)
{
 unsigned ic=interval*ref/100,bc=0,tmp=ic>>16;while(tmp){bc++;tmp>>=1;}
 *u=(bc+1)/2;*p=ic/(1u<<(2*(*u)));
}
static int sclk_value(struct rdn_card *card,struct rdn_cayman_dpm *pm,uint32_t clock,NISLANDS_SMC_SCLK_VALUE *value)
{
 COMPUTE_MEMORY_ENGINE_PLL_PARAMETERS_V3 args;uint8_t f,c;uint32_t fbdiv,v[7];int err;
 unsigned index=GetIndexIntoMasterTable(COMMAND,ComputeMemoryEnginePLL);
 if(!atom_parse_cmd_header(card->atom.ctx,index,&f,&c)||(c!=3&&c!=5))return -EINVAL;
 memset(&args,0,sizeof(args));args.ulClockParams=cpu_to_le32((COMPUTE_ENGINE_PLL_PARAM<<24)|clock);
 err=atom_execute_table(card->atom.ctx,index,(uint32_t *)&args,sizeof(args));if(err)return err;
 if(!args.ucPostDiv||args.ucPostDiv>127||args.ucRefDiv>63)return -EINVAL;
 /* ATOM returns the same Q14 divider used by SetEngineClock. */
 unsigned whole=le16_to_cpu(args.ulFbDiv.usFbDiv),fraction=le16_to_cpu(args.ulFbDiv.usFbDivFrac);
 if(whole>4095||fraction>=16384)return -EINVAL;
 fbdiv=(whole<<14)|fraction;
 if(clock && (!fbdiv || (uint64_t)fbdiv*pm->calibration.reference_clock/((1+args.ucRefDiv)*args.ucPostDiv*16384u)>clock))return -EINVAL;
 v[0]=(RREG32(CG_SPLL_FUNC_CNTL)&~(SPLL_PDIV_A_MASK|SPLL_REF_DIV_MASK))|SPLL_REF_DIV(args.ucRefDiv)|SPLL_PDIV_A(args.ucPostDiv);
 v[1]=(RREG32(CG_SPLL_FUNC_CNTL_2)&~SCLK_MUX_SEL_MASK)|SCLK_MUX_SEL(2);
 v[2]=(RREG32(CG_SPLL_FUNC_CNTL_3)&~SPLL_FB_DIV_MASK)|SPLL_FB_DIV(fbdiv)|SPLL_DITHEN;
 v[3]=RREG32(CG_SPLL_FUNC_CNTL_4);v[4]=RREG32(CG_SPLL_SPREAD_SPECTRUM);v[5]=RREG32(CG_SPLL_SPREAD_SPECTRUM_2);v[6]=clock;
 if(v[4]&SSEN) {
  uint16_t size;const uint8_t *ss=data(card,GetIndexIntoMasterTable(DATA,ASIC_InternalSS_Info),16,&size);bool found=false;
  if(!ss||ss[2]!=3)return -EINVAL;
  for(unsigned off=4;off+12<=size;off+=12)if(ss[off+8]==ASIC_INTERNAL_ENGINE_SS&&clock*args.ucPostDiv<=rdn_get_le32(ss,off)) {
   unsigned rate=rdn_get_le16(ss,off+6)/100,percent=rdn_get_le16(ss,off+4);
   if(!rate)return -EINVAL;
   unsigned clks=pm->calibration.reference_clock*5/((1+args.ucRefDiv)*rate);
   if(!clks||clks>4095)return -EINVAL;
   unsigned clkv=(uint64_t)4*percent*fbdiv/(clks*10000);
   v[4]=(v[4]&~CLK_S_MASK)|CLK_S(clks)|SSEN;v[5]=(v[5]&~CLK_V_MASK)|CLK_V(clkv);found=true;break;
  }
  if(!found)return -EINVAL;
 }
 uint32_t *out=(uint32_t *)value;for(unsigned i=0;i<7;i++)out[i]=be32(v[i]);return 0;
}
static void memory_snapshot(struct rdn_card *card,NISLANDS_SMC_MCLK_VALUE *m,unsigned clock)
{
 memset(m,0,sizeof(*m));
 m->vMPLL_AD_FUNC_CNTL=be32(RREG32(MPLL_AD_FUNC_CNTL));m->vMPLL_AD_FUNC_CNTL_2=be32(RREG32(MPLL_AD_FUNC_CNTL_2));
 m->vMPLL_DQ_FUNC_CNTL=be32(RREG32(MPLL_DQ_FUNC_CNTL));m->vMPLL_DQ_FUNC_CNTL_2=be32(RREG32(MPLL_DQ_FUNC_CNTL_2));
 m->vMCLK_PWRMGT_CNTL=be32(RREG32(MCLK_PWRMGT_CNTL)|MPLL_PWRMGT_OFF);m->vDLL_CNTL=be32(RREG32(DLL_CNTL));
 m->vMPLL_SS=be32(RREG32(MPLL_SS1));m->vMPLL_SS2=be32(RREG32(MPLL_SS2));m->mclk_value=be32(clock);
}
static void initial_clock_snapshot(struct rdn_card *card,struct rdn_cayman_dpm *pm)
{
 NISLANDS_SMC_SCLK_VALUE *v=&pm->table.initialState.level.sclk;
 v->vCG_SPLL_FUNC_CNTL=be32(RREG32(CG_SPLL_FUNC_CNTL));
 v->vCG_SPLL_FUNC_CNTL_2=be32(RREG32(CG_SPLL_FUNC_CNTL_2));
 v->vCG_SPLL_FUNC_CNTL_3=be32(RREG32(CG_SPLL_FUNC_CNTL_3));
 v->vCG_SPLL_FUNC_CNTL_4=be32(RREG32(CG_SPLL_FUNC_CNTL_4));
 v->vCG_SPLL_SPREAD_SPECTRUM=be32(RREG32(CG_SPLL_SPREAD_SPECTRUM));
 v->vCG_SPLL_SPREAD_SPECTRUM_2=be32(RREG32(CG_SPLL_SPREAD_SPECTRUM_2));
 v->sclk_value=be32(pm->idle.sclk);
 memory_snapshot(card,&pm->table.initialState.level.mclk,pm->performance.mclk);
 pm->table.initialState.level.vddci=pm->table.dpmLevels[2].vddci;
 pm->table.initialState.level.ACIndex=0;
 pm->table.initialState.level.mcFlags=pm->table.dpmLevels[2].mcFlags;
 pm->table.initialState.level.strobeMode=pm->table.dpmLevels[2].strobeMode;
 pm->table.ACPIState=pm->table.initialState;
 pm->table.ULVState=pm->table.initialState;
}
static void prepare_arb_handoff(struct rdn_card *card,struct rdn_cayman_dpm *pm)
{
 pm->saved_mc_cg=RREG32(MC_CG_CONFIG);
 pm->saved_arb_timing1=RREG32(MC_ARB_DRAM_TIMING_1);
 pm->saved_arb_timing2_1=RREG32(MC_ARB_DRAM_TIMING2_1);
 pm->saved_arb_burst=RREG32(MC_ARB_BURST_TIME);pm->saved_arb_refresh=RREG32(MC_ARB_RFSH_RATE);
 /* Firmware owns F1 onward; preserve the static F0 timing bank. */
 WREG32(MC_ARB_DRAM_TIMING_1,RREG32(MC_ARB_DRAM_TIMING));
 WREG32(MC_ARB_DRAM_TIMING2_1,RREG32(MC_ARB_DRAM_TIMING2));
 update(card,MC_ARB_BURST_TIME,STATE1_MASK,STATE1((pm->saved_arb_burst&STATE0_MASK)>>STATE0_SHIFT));
 WREG32(MC_CG_CONFIG,pm->saved_mc_cg|15);
 update(card,MC_ARB_CG,CG_ARB_REQ_MASK,CG_ARB_REQ(0x0b));
 pm->arb_switched=true;
}
int rdn_cayman_dpm_prepare(struct rdn_card *card,struct rdn_cayman_dpm *pm)
{
 int err;uint32_t current_sclk,current_mclk;NISLANDS_SMC_STATETABLE *t=&pm->table;
 memset(pm,0,sizeof(*pm));
 err=rdn_cayman_calibration(card,&pm->calibration);if(err)return err;
 err=rdn_pm_get_clocks(card,&current_sclk,&current_mclk);if(err)return err;
 unsigned mclk=(current_mclk+50)/100*100;
 err=rdn_pm_bios_state(card,RDN_PM_BIOS_PERFORMANCE,&pm->bios_performance);if(err)return err;
 if(pm->bios_performance.count!=3)return -EINVAL;
 pm->video_supported=!rdn_pm_bios_state(card,RDN_PM_BIOS_VIDEO,&pm->bios_video) && pm->bios_video.count==3;
 pm->ulv_supported=!rdn_pm_bios_state(card,RDN_PM_BIOS_ULV,&pm->bios_ulv);
 if(pm->video_supported){card->power_video_vclk=pm->bios_video.vclk;card->power_video_dclk=pm->bios_video.dclk;}
 memcpy(pm->levels,pm->bios_performance.level,sizeof(pm->levels));
 if(!card->power_memory_dpm){pm->levels[1].sclk=55000;pm->bios_performance.level[1].sclk=55000;}
 pm->idle=pm->levels[0];pm->intermediate=pm->levels[1];pm->performance=pm->levels[2];
 if(pm->performance.mclk!=mclk || pm->idle.sclk>=pm->intermediate.sclk || pm->intermediate.sclk>=pm->performance.sclk)return -EINVAL;
 unsigned mask,blank;bool displayMemory=rdn_cayman_memory_display(card,&mask,&blank);
 pm->memory_enabled=card->power_memory_dpm && displayMemory;pm->display_mask=mask;
 if(!pm->memory_enabled)for(unsigned i=0;i<3;i++){pm->levels[i].mclk=mclk;pm->levels[i].vddci=pm->performance.vddci;}
 pm->idle=pm->levels[0];pm->intermediate=pm->levels[1];
 err=rdn_cayman_memory_prepare(card,&pm->memory,pm->levels,3);if(err)return err;
 if(!card->power_memory_dpm)pm->memory.regs.last=0;
 pm->fan_percent=100;
 t->thermalProtectType=PPSMC_THERMAL_PROTECT_TYPE_INTERNAL;t->systemFlags=PPSMC_SYSTEMFLAG_GDDR5;
 if(pm->calibration.caps&0x10000)t->systemFlags|=PPSMC_SYSTEMFLAG_REGULATOR_HOT;
 if(pm->calibration.caps&128)t->systemFlags|=PPSMC_SYSTEMFLAG_STEPVDDC;
 for(unsigned i=0;i<pm->calibration.voltage_count;i++)t->lowSMIO[i]=be32(pm->calibration.gpio[i]);
 for(unsigned i=0;i<pm->calibration.vddci_count;i++)t->lowSMIO[i]|=be32(pm->calibration.vddci_gpio[i]);
 t->voltageMaskTable.lowMask[0]=be32(pm->calibration.gpio_mask);t->voltageMaskTable.lowMask[2]=be32(pm->calibration.vddci_mask);
 sample_period(1000,pm->calibration.xclk,&pm->bsp,&current_sclk);pm->dsp=pm->bsp|(current_sclk<<16);
 sample_period(400000,pm->calibration.xclk,&pm->pbsp,&current_sclk);pm->psp=pm->pbsp|(current_sclk<<16);
 struct rdn_pm_state *levels[]={&pm->levels[0],&pm->levels[1],&pm->levels[2]};
 for(unsigned i=0;i<3;i++) {
  struct rdn_pm_state *state=levels[i];
  NISLANDS_SMC_HW_PERFORMANCE_LEVEL *l=&t->dpmLevels[i];
  err=sclk_value(card,pm,state->sclk,&l->sclk);if(err)return err;
  err=rdn_cayman_memory_value(card,&pm->memory,state->mclk,l);if(err)return err;
  if(!card->power_memory_dpm)memory_snapshot(card,&l->mclk,state->mclk);
  err=voltage_value(pm->calibration.voltage,pm->calibration.voltage_count,state->vddc,&l->vddc);if(err)return err;
  err=voltage_value(pm->calibration.vddci,pm->calibration.vddci_count,state->vddci,&l->vddci);if(err)return err;
  l->std_vddc.index=l->vddc.index;l->std_vddc.value=be16(pm->calibration.std_voltage[l->vddc.index]);l->mvdd.value=be16(1500);
  l->ACIndex=card->power_memory_dpm?2+i:0;
  l->displayWatermark=0;l->arbRefreshState=3+i;l->bSP=be32(i==2?pm->psp:pm->dsp);
  l->SQPowerThrottle=be32(MAX_POWER_MASK|MIN_POWER_MASK);l->SQPowerThrottle_2=be32(MAX_POWER_DELTA_MASK|STI_SIZE_MASK|LTI_RATIO_MASK);
  if(i) {
   l->dpm2.MaxPS=256u*(state->sclk-levels[i-1]->sclk)/state->sclk;l->dpm2.NearTDPDec=10;l->dpm2.AboveSafeInc=5;l->dpm2.BelowSafeInc=20;
   if(state->sclk>=pm->calibration.sq_threshold){l->SQPowerThrottle=be32(MAX_POWER(0x3fff)|MIN_POWER(0x12));l->SQPowerThrottle_2=be32(MAX_POWER_DELTA(0x15)|STI_SIZE(0x1e)|LTI_RATIO(0xf));}
  }
 }
 t->maxVDDCIndexInPPTable=t->dpmLevels[2].vddc.index;
 /* Adjacent-level thresholds from ni_populate_smc_t and r600_calculate_at. */
 for(unsigned i=0;i<2;i++) {
  unsigned period=1000*(i+1),k=100*levels[i+1]->sclk/levels[i]->sclk;
  unsigned t1=period*(k-100),a=(1000*(50000+t1))/(10000+t1/100);a=(a+5)/10;
  unsigned ah=(a*period+5000)/10000,al=a-ah,th=period-ah,tl=period+al;
  unsigned value=be32(t->dpmLevels[i].aT)&0xffff0000u;
  t->dpmLevels[i].aT=be32(value|(tl*pm->bsp/20000));
  t->dpmLevels[i+1].aT=be32(0xffff|((th*(i==1?pm->pbsp:pm->bsp)/20000)<<16));
 }
 t->initialState.flags=PPSMC_SWSTATE_FLAG_DC;t->initialState.levelCount=1;t->initialState.level=t->dpmLevels[0];
 t->initialState.level.arbRefreshState=0;
 t->initialState.level.aT=be32(0xffff);t->initialState.level.bSP=be32(pm->dsp);
 t->ACPIState=t->initialState;t->ULVState=t->initialState;t->driverState.flags=PPSMC_SWSTATE_FLAG_DC;t->driverState.levelCount=3;
 for(unsigned i=0,clock=0;i<256;i++,clock+=512) {
  NISLANDS_SMC_SCLK_VALUE s;err=sclk_value(card,pm,clock,&s);if(err)return err;
  uint32_t pd=(be32(s.vCG_SPLL_FUNC_CNTL)&SPLL_PDIV_A_MASK)>>SPLL_PDIV_A_SHIFT;
  uint32_t fb=(be32(s.vCG_SPLL_FUNC_CNTL_3)&SPLL_FB_DIV_MASK);fb=(fb&~0x1fffu)>>1;
  uint32_t cs=(be32(s.vCG_SPLL_SPREAD_SPECTRUM)&CLK_S_MASK)>>CLK_S_SHIFT;
  uint32_t cv=(be32(s.vCG_SPLL_SPREAD_SPECTRUM_2)&CLK_V_MASK)>>6;
  if(pd>127||fb>0x1ffffff||cv>0xfffff||cs>4095)return -EINVAL;
  pm->spll.freq[i]=be32(fb|(pd<<25));pm->spll.ss[i]=be32(cv|(cs<<20));
 }
 pm->prepared=true;return 0;
}

static int firmware_region(struct rdn_card *card,unsigned header,size_t bytes,uint16_t *address)
{
 uint32_t value=0;int err=rdn_smc_read(card,0x100+header,&value);
 if(err)return err;
 if(!value||(value&3)||value>=RDN_SMC_RAM_END||bytes>RDN_SMC_RAM_END-value)return -EINVAL;
 *address=value;return 0;
}
static int upload_verify(struct rdn_card *card,unsigned address,const void *data,size_t bytes)
{
 int err=rdn_smc_copy(card,address,data,bytes);const uint8_t *p=data;if(err)return err;
 for(size_t i=0;i<bytes;i+=4) {
  uint32_t actual=0;err=rdn_smc_read(card,address+i,&actual);if(err)return err;
  unsigned n=bytes-i<4?bytes-i:4;uint32_t expected=0,mask=0;
  for(unsigned j=0;j<n;j++){expected|=(uint32_t)p[i+j]<<(24-8*j);mask|=255u<<(24-8*j);}
  if((actual&mask)!=expected)return -EIO;
 }
 return 0;
}
int rdn_cayman_dpm_temperature_limit(struct rdn_card *card,unsigned degrees)
{
 if(card->family!=RDN_FAMILY_CAYMAN||degrees<RDN_THERMAL_LIMIT_MIN||degrees>RDN_THERMAL_LIMIT_MAX)return -EINVAL;
 update(card,CG_THERMAL_CTRL,DPM_EVENT_SRC_MASK|DIG_THERM_DPM_MASK,DPM_EVENT_SRC(2)|DIG_THERM_DPM(degrees));
 return (RREG32(CG_THERMAL_CTRL)&(DPM_EVENT_SRC_MASK|DIG_THERM_DPM_MASK))==(DPM_EVENT_SRC(2)|DIG_THERM_DPM(degrees))?0:-EIO;
}
static int upload_cac(struct rdn_card *card,struct rdn_cayman_dpm *pm)
{
 PP_NIslands_CACTABLES *t=&pm->cac;struct rdn_cayman_calibration *c=&pm->calibration;
 unsigned scale=cac_weights_cayman_le.tid_cnt*((1000000000u/c->xclk)/10000u);
 if(!scale)return -EINVAL;
 memset(t,0,sizeof(*t));unsigned maximum=0;
 for(unsigned j=0;j<16;j++) {
  unsigned leakage=j<c->leakage_count?c->leakage[j]:maximum;if(leakage>maximum)maximum=leakage;
  for(unsigned i=0;i<16;i++)t->cac_lkge_lut[i][j]=be32((leakage*scale)<<2);
 }
 t->MCWrWeight=cac_weights_cayman_le.mc_write_weight;t->MCRdWeight=cac_weights_cayman_le.mc_read_weight;
 t->l2numWin_TDP=cac_weights_cayman_le.l2_lta_window_size;t->lts_truncate_n=cac_weights_cayman_le.lts_truncate;
 pm->table.dpm2Params.TDPLimit=be32((c->tdp*scale)<<2);
 pm->table.dpm2Params.NearTDPLimit=be32((c->near_tdp*scale)<<2);
 pm->table.dpm2Params.SafePowerLimit=be32(((c->near_tdp*80/100)*scale)<<2);
 pm->table.dpm2Params.PowerBoostLimit=0;
 update(card,CG_CAC_CTRL,TID_CNT_MASK|TID_UNIT_MASK,TID_CNT(cac_weights_cayman_le.tid_cnt)|TID_UNIT(cac_weights_cayman_le.tid_unit));
 return upload_verify(card,pm->cac_addr,t,sizeof(*t));
}
static int response_times(struct rdn_card *card,struct rdn_cayman_dpm *pm)
{
 struct rdn_cayman_calibration *c=&pm->calibration;unsigned x=c->xclk;
 const unsigned offsets[]={NI_SMC_SOFT_REGISTER_mvdd_chg_time,NI_SMC_SOFT_REGISTER_delay_vreg,NI_SMC_SOFT_REGISTER_delay_bbias,
  NI_SMC_SOFT_REGISTER_delay_acpi,NI_SMC_SOFT_REGISTER_mclk_chg_timeout,NI_SMC_SOFT_REGISTER_mc_block_delay,NI_SMC_SOFT_REGISTER_mclk_switch_lim};
 unsigned values[]={1,(c->voltage_delay?c->voltage_delay:1000)*x/1600,(c->backbias_delay?c->backbias_delay:1000)*x/1600,
  15000*x/1600,100000*x/1600,0xaa,460*x/100};
 for(unsigned i=0;i<7;i++){int err=rdn_smc_write(card,pm->soft_addr+offsets[i],values[i]);if(err)return err;}
 return 0;
}
int rdn_cayman_dpm_disable(struct rdn_card *card,struct rdn_cayman_dpm *pm)
{
 if(!pm->touched)return 0;
 rdn_cayman_gating_restore(card,&pm->gating);
 if(pm->enabled) {
  int err=rdn_cayman_dpm_force_low(card,true);if(err)return err;
  err=rdn_smc_halt(card);if(err)return err;
  NISLANDS_SMC_HW_PERFORMANCE_LEVEL pin=pm->table.dpmLevels[0];
  pin.mclk=pm->table.dpmLevels[2].mclk;pin.vddci=pm->table.dpmLevels[2].vddci;
  pin.mcFlags=pm->table.dpmLevels[2].mcFlags;pin.strobeMode=pm->table.dpmLevels[2].strobeMode;
  pin.ACIndex=0;pin.arbRefreshState=0;
  uint32_t address=pm->state_addr+offsetof(NISLANDS_SMC_STATETABLE,dpmLevels);
  err=upload_verify(card,address,&pin,sizeof(pin));if(err)return err;
  err=rdn_smc_message(card,PPSMC_MSG_Resume);if(err)return err;
  err=rdn_smc_message(card,PPSMC_MSG_SwitchToSwState);if(err)return err;
  err=rdn_cayman_dpm_force_low(card,true);if(err)return err;
  unsigned i;
  for(i=0;i<1000;i++) {
   uint32_t sc=0,mc=0;if(!rdn_pm_get_clocks(card,&sc,&mc) && mc+100>=pm->performance.mclk)break;
   card->os->delay_us(card->os->cookie,100);
  }
  if(i==1000)return -ETIMEDOUT;
 }
 if((RREG32(0x208)&0x801)==0x801) {
  (void)rdn_cayman_dpm_force_low(card,true);
  int err=rdn_smc_halt(card);if(err)return err;
 }
 /* Stop firmware ownership before restoring static clock control. */
 update(card,GENERAL_PWRMGT,GLOBAL_PWRMGT_EN,0);rdn_smc_stop(card);
 rdn_cayman_gating_restore(card,&pm->gating);
 if(pm->arb_switched) {
  update(card,MC_ARB_CG,CG_ARB_REQ_MASK,CG_ARB_REQ(0x0a));
  WREG32(MC_ARB_DRAM_TIMING_1,pm->saved_arb_timing1);
  WREG32(MC_ARB_DRAM_TIMING2_1,pm->saved_arb_timing2_1);
  WREG32(MC_ARB_BURST_TIME,pm->saved_arb_burst);WREG32(MC_ARB_RFSH_RATE,pm->saved_arb_refresh);
  WREG32(MC_CG_CONFIG,pm->saved_mc_cg);
  pm->arb_switched=false;
 }
 rdn_cayman_memory_restore(card,&pm->memory);
 WREG32(SCLK_PWRMGT_CNTL,pm->saved_sclk);WREG32(MCLK_PWRMGT_CNTL,pm->saved_mclk);
 WREG32(CG_THERMAL_CTRL,pm->saved_thermal);WREG32(CG_BSP,pm->saved_bsp);WREG32(CG_GIT,pm->saved_git);
 WREG32(CG_TPC,pm->saved_tpc);WREG32(CG_SSP,pm->saved_ssp);WREG32(CG_FTV,pm->saved_ftv);WREG32(CG_CAC_CTRL,pm->saved_cac);
 for(unsigned i=0;i<15;i++)WREG32(CG_FFCT_0+4*i,pm->saved_ffct[i]);
 WREG32(GENERAL_PWRMGT,pm->saved_general&~GLOBAL_PWRMGT_EN);
 pm->enabled=false;pm->touched=false;return 0;
}
int rdn_cayman_dpm_force_low(struct rdn_card *card,bool low)
{
 int err=rdn_smc_message(card,PPSMC_MSG_NoForcedLevel);if(err)return err;
 WREG32(SMC_SCRATCH0,low?1:0);return rdn_smc_message(card,PPSMC_MSG_SetEnabledLevels);
}
/* Change only the high core VID while the firmware is restricted to low. */
int rdn_cayman_dpm_voltage(struct rdn_card *card,struct rdn_cayman_dpm *pm,unsigned mv)
{
 if(card->family!=RDN_FAMILY_CAYMAN || !pm || !pm->enabled || pm->voltage_fault)return -EINVAL;
 unsigned stock=pm->performance.vddc,index=pm->calibration.voltage_count;
 for(unsigned i=0;i<pm->calibration.voltage_count;i++)if(pm->calibration.voltage[i]==mv)index=i;
 if(index>=pm->calibration.voltage_count || mv>stock || stock-mv>100 || mv<pm->idle.vddc)return -EINVAL;
 NISLANDS_SMC_HW_PERFORMANCE_LEVEL old=pm->table.dpmLevels[2],next=old;
 next.vddc.index=index;next.vddc.value=be16(mv);
 next.std_vddc.index=index;next.std_vddc.value=be16(pm->calibration.std_voltage[index]);
 int err=rdn_cayman_dpm_force_low(card,true);if(err)return err;
 unsigned tries;
 for(tries=0;tries<10000;tries++) {
  if(((RREG32(0x66c)>>4)&15)==0)break;
  card->os->delay_us(card->os->cookie,10);
 }
 if(tries==10000)return -ETIMEDOUT;
 err=rdn_smc_message(card,PPSMC_TDPClampingInactive);if(err)return err;
 err=rdn_smc_message(card,PPSMC_MSG_DisableCac);
 if(!err)err=rdn_smc_halt(card);
 uint32_t address=pm->state_addr+offsetof(NISLANDS_SMC_STATETABLE,dpmLevels)+2*sizeof(old);
 if(!err)err=upload_verify(card,address,&next,sizeof(next));
 if(!err)err=rdn_smc_message(card,PPSMC_MSG_Resume);
 if(!err)err=rdn_smc_message(card,PPSMC_MSG_SwitchToSwState);
 if(!err)err=rdn_smc_message(card,PPSMC_MSG_EnableCac);
 if(!err)err=rdn_smc_message(card,PPSMC_TDPClampingActive);
 if(!err){pm->table.dpmLevels[2]=next;return 0;}
 /* Restore the previous table before admitting another high-state request. */
 int restored=rdn_smc_halt(card);
 if(!restored)restored=upload_verify(card,address,&old,sizeof(old));
 if(!restored)restored=rdn_smc_message(card,PPSMC_MSG_Resume);
 if(!restored)restored=rdn_smc_message(card,PPSMC_MSG_SwitchToSwState);
 if(!restored)restored=rdn_smc_message(card,PPSMC_MSG_EnableCac);
 if(!restored)restored=rdn_smc_message(card,PPSMC_TDPClampingActive);
 pm->voltage_fault=restored!=0;
 return err;
}
int rdn_cayman_dpm_force_high(struct rdn_card *card)
{
 if(card->family!=RDN_FAMILY_CAYMAN)return -EINVAL;
 WREG32(SMC_SCRATCH0,0);
 int err=rdn_smc_message(card,PPSMC_MSG_SetForcedLevels);if(err)return err;
 WREG32(SMC_SCRATCH0,0);
 err=rdn_smc_message(card,PPSMC_MSG_SetEnabledLevels);if(err)return err;
 WREG32(SMC_SCRATCH0,1);
 return rdn_smc_message(card,PPSMC_MSG_SetForcedLevels);
}
/* Bound the software state's highest level before releasing the low cap. */
static int set_level_count(struct rdn_card *card,struct rdn_cayman_dpm *pm,unsigned count)
{
 if(!pm || !pm->enabled || pm->voltage_fault || count<2 || count>3)return -EINVAL;
 if(pm->table.driverState.levelCount==count)return 0;
 NISLANDS_SMC_SWSTATE old=pm->table.driverState,next=old;
 next.levelCount=count;
 int err=rdn_cayman_dpm_force_low(card,true);if(err)return err;
 unsigned tries;
 for(tries=0;tries<10000;tries++) {
  if(((RREG32(0x66c)>>4)&15)==0)break;
  card->os->delay_us(card->os->cookie,10);
 }
 if(tries==10000)return -ETIMEDOUT;
 err=rdn_smc_message(card,PPSMC_TDPClampingInactive);if(err)return err;
 err=rdn_smc_message(card,PPSMC_MSG_DisableCac);
 if(!err)err=rdn_smc_halt(card);
 uint32_t address=pm->state_addr+offsetof(NISLANDS_SMC_STATETABLE,driverState);
 if(!err)err=upload_verify(card,address,&next,sizeof(next));
 if(!err)err=rdn_smc_message(card,PPSMC_MSG_Resume);
 if(!err)err=rdn_smc_message(card,PPSMC_MSG_SwitchToSwState);
 if(!err)err=rdn_smc_message(card,PPSMC_MSG_EnableCac);
 if(!err)err=rdn_smc_message(card,PPSMC_TDPClampingActive);
 if(!err){pm->table.driverState=next;return 0;}
 int restored=rdn_smc_halt(card);
 if(!restored)restored=upload_verify(card,address,&old,sizeof(old));
 if(!restored)restored=rdn_smc_message(card,PPSMC_MSG_Resume);
 if(!restored)restored=rdn_smc_message(card,PPSMC_MSG_SwitchToSwState);
 if(!restored)restored=rdn_smc_message(card,PPSMC_MSG_EnableCac);
 if(!restored)restored=rdn_smc_message(card,PPSMC_TDPClampingActive);
 if(!restored)restored=rdn_cayman_dpm_force_low(card,true);
 pm->voltage_fault=restored!=0;
 return err;
}
unsigned rdn_cayman_thermal_stage(int temp,unsigned limit,unsigned previous,bool invalid)
{
 if(invalid || limit<RDN_THERMAL_LIMIT_MIN || limit>RDN_THERMAL_LIMIT_MAX || temp>=(int)limit*1000)return 2;
 if(previous==2 && temp>(int)(limit-5)*1000)return 2;
 if(temp>=(int)(limit-5)*1000 || (previous && temp>(int)(limit-10)*1000))return 1;
 return 0;
}
int rdn_cayman_dpm_policy(struct rdn_card *card,struct rdn_cayman_dpm *pm,bool high,bool low)
{
 if(low || pm->idle_requested || pm->thermal_stage==2)return rdn_cayman_dpm_force_low(card,true);
 int err=set_level_count(card,pm,pm->thermal_stage==1?2:3);if(err)return err;
 if(pm->thermal_stage==1)return rdn_cayman_dpm_force_high(card);
 return high?rdn_cayman_dpm_force_high(card):rdn_cayman_dpm_force_low(card,false);
}
static void display_gap(struct rdn_card *card,unsigned mask,bool memory)
{
 unsigned pipe=0;while(pipe<5 && !(mask&(1u<<pipe)))pipe++;
 uint32_t gap=RREG32(CG_DISPLAY_GAP_CNTL);
 gap&=~(DISP1_GAP_MASK|DISP2_GAP_MASK|DISP1_GAP_MCHG_MASK|DISP2_GAP_MCHG_MASK);
 enum { GAP_VBLANK_OR_WM=0, GAP_VBLANK=1, GAP_IGNORE=3 };
 gap|=DISP1_GAP(mask?GAP_VBLANK_OR_WM:GAP_IGNORE)|DISP2_GAP(GAP_IGNORE);
 gap|=DISP1_GAP_MCHG(mask && memory?GAP_VBLANK:GAP_IGNORE)|DISP2_GAP_MCHG(GAP_IGNORE);
 WREG32(CG_DISPLAY_GAP_CNTL,gap);
 if(mask)WREG32(0x4fc,(RREG32(0x4fc)&~7u)|pipe);
}
static int resume_policy(struct rdn_card *card)
{
 int err=rdn_smc_message(card,PPSMC_MSG_Resume);
 if(!err)err=rdn_smc_message(card,PPSMC_MSG_SwitchToSwState);
 if(!err)err=rdn_smc_message(card,PPSMC_MSG_EnableCac);
 if(!err)err=rdn_smc_message(card,PPSMC_TDPClampingActive);
 return err;
}
int rdn_cayman_dpm_context(struct rdn_card *card,struct rdn_cayman_dpm *pm,
                           bool video,bool display_changing)
{
 unsigned mask,blank;bool displayMemory=rdn_cayman_memory_display(card,&mask,&blank);
 bool memory=card->power_memory_dpm && displayMemory && !display_changing;
 if(!pm || !pm->enabled || pm->voltage_fault)return -EINVAL;
 if(video && !pm->video_supported)return -EINVAL;
 const struct rdn_pm_bios_state *bios=video?&pm->bios_video:&pm->bios_performance;
 if((bios->caps&1) && mask && (mask&(mask-1)))return -EINVAL;
 if(pm->video==video && pm->memory_enabled==memory && pm->display_mask==mask &&
    pm->display_changing==display_changing && pm->context_thermal_stage==(video && pm->thermal_stage==2?2:0))return 0;
 struct rdn_pm_state levels[3];memcpy(levels,bios->level,sizeof(levels));
 if(!memory)for(unsigned i=0;i<3;i++){levels[i].mclk=pm->performance.mclk;levels[i].vddci=pm->performance.vddci;}
 if(video && pm->thermal_stage==2){levels[0].sclk=pm->idle.sclk;levels[0].vddc=pm->idle.vddc;}
 pm->pending_table=pm->table;
 struct rdn_cayman_memory *next=&pm->pending_memory;
 int err=rdn_cayman_dpm_force_low(card,true);if(err)return err;
 unsigned tries;
 for(tries=0;tries<10000;tries++){if(((RREG32(0x66c)>>4)&15)==0)break;card->os->delay_us(card->os->cookie,10);}
 if(tries==10000)return -ETIMEDOUT;
 err=rdn_smc_message(card,PPSMC_TDPClampingInactive);
 if(!err)err=rdn_smc_message(card,PPSMC_MSG_DisableCac);
 if(!err)err=rdn_smc_halt(card);
 if(!err)err=rdn_cayman_memory_prepare(card,next,levels,3);
 if(!err) {
  if(!card->power_memory_dpm)next->regs.last=0;
  memcpy(next->clock_registers,pm->memory.clock_registers,sizeof(next->clock_registers));next->spread_enabled=pm->memory.spread_enabled;
  next->initial_sclk=pm->memory.initial_sclk;next->initial_mclk=pm->memory.initial_mclk;
  next->address=pm->memory.address;next->saved_gap=pm->memory.saved_gap;next->saved_select=pm->memory.saved_select;
  memcpy(next->saved_lp,pm->memory.saved_lp,sizeof(next->saved_lp));memcpy(next->lp,pm->memory.lp,sizeof(next->lp));next->lp_count=pm->memory.lp_count;next->touched=pm->memory.touched;
 }
 for(unsigned i=0;i<3 && !err;i++) {
  NISLANDS_SMC_HW_PERFORMANCE_LEVEL *l=&pm->pending_table.dpmLevels[i];
  err=sclk_value(card,pm,levels[i].sclk,&l->sclk);
  if(!err)err=rdn_cayman_memory_value(card,&pm->memory,levels[i].mclk,l);
  if(!err && !card->power_memory_dpm)memory_snapshot(card,&l->mclk,levels[i].mclk);
  if(!err)err=voltage_value(pm->calibration.voltage,pm->calibration.voltage_count,levels[i].vddc,&l->vddc);
  if(!err)err=voltage_value(pm->calibration.vddci,pm->calibration.vddci_count,levels[i].vddci,&l->vddci);
  if(err)break;
  l->std_vddc.index=l->vddc.index;l->std_vddc.value=be16(pm->calibration.std_voltage[l->vddc.index]);
  l->ACIndex=card->power_memory_dpm?2+i:0;l->displayWatermark=0;
  l->dpm2.MaxPS=i?256u*(levels[i].sclk-levels[i-1].sclk)/levels[i].sclk:0;
  l->aT=be32(0xffff);
 }
 for(unsigned i=0;i<2 && !err;i++) {
  unsigned period=1000*(i+1),k=100*levels[i+1].sclk/levels[i].sclk;
  unsigned t1=period*(k-100),a=(1000*(50000+t1))/(10000+t1/100);a=(a+5)/10;
  unsigned ah=(a*period+5000)/10000,al=a-ah,th=period-ah,tl=period+al;
  uint32_t value=be32(pm->pending_table.dpmLevels[i].aT)&0xffff0000u;
  pm->pending_table.dpmLevels[i].aT=be32(value|(tl*pm->bsp/20000));
  pm->pending_table.dpmLevels[i+1].aT=be32(0xffff|((th*(i==1?pm->pbsp:pm->bsp)/20000)<<16));
 }
 if(!err)err=rdn_cayman_memory_timings(card,next,levels,3);
 uint32_t offset=offsetof(NISLANDS_SMC_STATETABLE,driverState);
 unsigned bytes=sizeof(NISLANDS_SMC_SWSTATE)+3*sizeof(NISLANDS_SMC_HW_PERFORMANCE_LEVEL);
 if(!err)err=upload_verify(card,pm->state_addr+offset,(uint8_t *)&pm->pending_table+offset,bytes);
 if(!err)err=upload_verify(card,next->address,&next->regs,sizeof(next->regs));
 if(!err)err=upload_verify(card,pm->arb_addr,&next->arb,sizeof(next->arb));
 if(!err)err=rdn_smc_write(card,pm->soft_addr+NI_SMC_SOFT_REGISTER_uvd_enabled,video?1:0);
 if(!err) {
  if(!card->wm_low_mclk || levels[0].mclk<card->wm_low_mclk)card->wm_low_mclk=levels[0].mclk;
  if(!card->wm_low_sclk || levels[0].sclk<card->wm_low_sclk)card->wm_low_sclk=levels[0].sclk;
  rdn_bandwidth_update_clocks(card);display_gap(card,mask,memory);
  err=rdn_smc_message(card,mask?PPSMC_MSG_HasDisplay:PPSMC_MSG_NoDisplay);
 }
 if(!err)err=resume_policy(card);
 if(!err) {
  pm->table=pm->pending_table;pm->memory=*next;memcpy(pm->levels,levels,sizeof(levels));
  pm->video=video;pm->display_changing=display_changing;pm->memory_enabled=memory;pm->display_mask=mask;
  pm->context_thermal_stage=video && pm->thermal_stage==2?2:0;pm->undervolt_deadline=0;
  card->wm_low_sclk=levels[0].sclk;card->wm_low_mclk=levels[0].mclk;rdn_bandwidth_update_clocks(card);
  return 0;
 }
 int restored=rdn_smc_halt(card);
 if(!restored)restored=upload_verify(card,pm->state_addr+offset,(uint8_t *)&pm->table+offset,bytes);
 if(!restored)restored=upload_verify(card,pm->memory.address,&pm->memory.regs,sizeof(pm->memory.regs));
 if(!restored)restored=upload_verify(card,pm->arb_addr,&pm->memory.arb,sizeof(pm->memory.arb));
 if(!restored)restored=rdn_smc_write(card,pm->soft_addr+NI_SMC_SOFT_REGISTER_uvd_enabled,pm->video?1:0);
 display_gap(card,pm->display_mask,pm->memory_enabled);
 if(!restored)restored=resume_policy(card);
 if(!restored)restored=rdn_cayman_dpm_force_low(card,true);
 pm->voltage_fault=restored!=0;return err;
}
int rdn_cayman_dpm_fan(struct rdn_card *card,struct rdn_cayman_dpm *pm,
                       int temp,unsigned limit,bool invalid)
{
 struct rdn_user_board_sensors sensors;rdn_pm_board_sensors(card,&sensors);
 unsigned duty=100;
 bool tachBad=sensors.fan_status!=RDN_SENSOR_VALID || sensors.fan_rpm<(sensors.fan_min_rpm?sensors.fan_min_rpm:500);
 if(tachBad){if(pm->fan_stall_count<8)pm->fan_stall_count++;}else pm->fan_stall_count=0;
 bool emergency=invalid || pm->thermal_stage!=0 || temp<0 || temp>=(int)(limit-5)*1000 || temp>=85000 || tachBad;
 if(!emergency) {
  duty=temp<=50000?60:temp>=80000?100:temp<70000?60+(temp-50000)/1000:80+(temp-70000)/500;
  uint16_t size;const uint8_t *p=data(card,GetIndexIntoMasterTable(DATA,PowerPlayInfo),46,&size);
  unsigned off=p?rdn_get_le16(p,42):0;
  if(off && off+14<=size && p[off]<=3) {
   unsigned t[3],pwm[3];for(unsigned i=0;i<3;i++){t[i]=rdn_get_le16(p,off+2+i*2)*10;pwm[i]=rdn_get_le16(p,off+8+i*2);}
   if(t[0]<t[1] && t[1]<t[2] && pwm[0]<=pwm[1] && pwm[1]<=pwm[2] && pwm[2]<=10000) {
    if(temp<=(int)t[0])duty=(pwm[0]+99)/100;
    else if(temp>=(int)t[2])duty=(pwm[2]+99)/100;
    else {unsigned j=temp<(int)t[1]?0:1;duty=(pwm[j]+(uint64_t)(pwm[j+1]-pwm[j])*(temp-t[j])/(t[j+1]-t[j])+99)/100;}
   }
  }
  if(duty<60)duty=60;
  if(duty>100)duty=100;
 }
 uint64_t now=card->os->time_ms?card->os->time_ms(card->os->cookie):0;
 if(duty<pm->fan_percent && now<pm->fan_next_change)return 0;
 if(duty<pm->fan_percent && pm->fan_percent-duty>2)duty=pm->fan_percent-2;
 if(duty==pm->fan_percent) {
  unsigned scale=RREG32(0x770)&255,expected=(scale*duty+99)/100;
  if(scale && (RREG32(0x76c)&255)==expected && (RREG32(0x774)&(255u|(7u<<11)))==(1u<<11))return pm->fan_stall_count>=8?-EIO:0;
 }
 int err=rdn_pm_fan_percent(card,duty);
 if(err){(void)rdn_pm_fan_full(card);pm->fan_percent=100;return err;}
 pm->fan_percent=duty;pm->fan_next_change=now+1000;return pm->fan_stall_count>=8?-EIO:0;
}
static bool firmware_valid(const void *firmware,size_t size)
{
 if(!firmware||size!=RDN_CAYMAN_SMC_SIZE)return false;
 const uint8_t *p=firmware;uint32_t crc=~0u;
 for(size_t i=0;i<size;i++){crc^=p[i];for(unsigned b=0;b<8;b++)crc=(crc>>1)^((crc&1)?0xedb88320u:0);}
 return ~crc==0x552fd2aau;
}
int rdn_cayman_dpm_enable(struct rdn_card *card,struct rdn_cayman_dpm *pm,const void *firmware,size_t size)
{
 int err;struct rdn_pm_state from;
 if(!pm->prepared||pm->enabled||pm->touched||!firmware_valid(firmware,size))return -EINVAL;
 if(RREG32(GENERAL_PWRMGT)&GLOBAL_PWRMGT_EN)return -EINVAL;
 if((RREG32(0x208)&0x801)==0x801)return -16;
 if(RREG32(GRBM_STATUS)&GUI_ACTIVE)return -16;
 unsigned arb=RREG32(MC_ARB_CG)&CG_ARB_REQ_MASK;
 if(arb && arb!=0x0a)return -EINVAL;
 pm->saved_general=RREG32(GENERAL_PWRMGT);pm->saved_sclk=RREG32(SCLK_PWRMGT_CNTL);pm->saved_mclk=RREG32(MCLK_PWRMGT_CNTL);
 pm->saved_thermal=RREG32(CG_THERMAL_CTRL);pm->saved_bsp=RREG32(CG_BSP);pm->saved_git=RREG32(CG_GIT);
 pm->saved_tpc=RREG32(CG_TPC);pm->saved_ssp=RREG32(CG_SSP);pm->saved_ftv=RREG32(CG_FTV);pm->saved_cac=RREG32(CG_CAC_CTRL);
 for(unsigned i=0;i<15;i++)pm->saved_ffct[i]=RREG32(CG_FFCT_0+4*i);
 /* Static firmware queries cannot tell us the current voltage: retain the safe upper value. */
 from=pm->performance;err=rdn_pm_get_clocks(card,&from.sclk,&from.mclk);if(err)return err;
 if((from.mclk+50)/100*100!=pm->performance.mclk)return -EINVAL;
 struct rdn_pm_state handover=pm->idle;handover.mclk=from.mclk;handover.vddci=pm->performance.vddci;
 err=rdn_pm_set(card,&from,&handover,RDN_PM_SCLK|RDN_PM_VOLTAGE);if(err)return err;
 pm->touched=true;
 initial_clock_snapshot(card,pm);
 if(card->power_graphics_gating){err=rdn_cayman_gating_prepare(card,&pm->gating);if(err)goto fail;}
 rdn_log(card->os,RDN_LOG_INFO,"Cayman SMC: upload firmware");
 err=rdn_smc_load(card,firmware,size);if(err)goto fail;
 err=firmware_region(card,NISLANDS_SMC_FIRMWARE_HEADER_stateTable,sizeof(pm->table),&pm->state_addr);if(err)goto fail;
 err=firmware_region(card,NISLANDS_SMC_FIRMWARE_HEADER_softRegisters,0xa0,&pm->soft_addr);if(err)goto fail;
 err=firmware_region(card,NISLANDS_SMC_FIRMWARE_HEADER_cacTable,sizeof(pm->cac),&pm->cac_addr);if(err)goto fail;
 err=firmware_region(card,NISLANDS_SMC_FIRMWARE_HEADER_spllTable,sizeof(pm->spll),&pm->spll_addr);if(err)goto fail;
 err=firmware_region(card,NISLANDS_SMC_FIRMWARE_HEADER_mcArbDramAutoRefreshTable,sizeof(SMC_NIslands_MCArbDramTimingRegisters),&pm->arb_addr);if(err)goto fail;
 err=firmware_region(card,NISLANDS_SMC_FIRMWARE_HEADER_mcRegisterTable,sizeof(pm->memory.regs),&pm->memory.address);if(err)goto fail;
 err=rdn_cayman_memory_timings(card,&pm->memory,pm->levels,3);if(err)goto fail;
 prepare_arb_handoff(card,pm);
 err=upload_verify(card,pm->arb_addr,&pm->memory.arb,sizeof(pm->memory.arb));if(err)goto fail;
 err=upload_verify(card,pm->memory.address,&pm->memory.regs,sizeof(pm->memory.regs));if(err)goto fail;
 err=rdn_smc_write(card,pm->soft_addr+NI_SMC_SOFT_REGISTER_seq_index,1);if(err)goto fail;
 rdn_log(card->os,RDN_LOG_INFO,"Cayman SMC: upload power tables");
 err=upload_cac(card,pm);if(err)goto fail;
 err=upload_verify(card,pm->state_addr,&pm->table,sizeof(pm->table));if(err)goto fail;
 err=upload_verify(card,pm->spll_addr,&pm->spll,sizeof(pm->spll));if(err)goto fail;
 err=response_times(card,pm);if(err)goto fail;
 display_gap(card,pm->display_mask,pm->memory_enabled);
 err=initialize_cac_manager(card);if(err)goto fail;
 WREG32(CG_BSP,pm->dsp);update(card,CG_GIT,CG_GICST_MASK,CG_GICST(0x200));
 for(unsigned i=0;i<15;i++){unsigned v=i?0x22:0x24;WREG32(CG_FFCT_0+4*i,UTC_0(v)|DTC_0(v));}
 update(card,SCLK_PWRMGT_CNTL,FIR_FORCE_TREND_SEL|FIR_TREND_MODE,0);
 WREG32(CG_TPC,0x200);WREG32(CG_SSP,0xc8);WREG32(CG_FTV,0xc00033);
 err=rdn_cayman_dpm_temperature_limit(card,RDN_THERMAL_LIMIT_DEFAULT);if(err)goto fail;
 update(card,GENERAL_PWRMGT,THERMAL_PROTECTION_DIS|VOLT_PWRMGT_EN|STATIC_PM_EN,VOLT_PWRMGT_EN|STATIC_PM_EN);
 /* Both display watermark sets cover the minimum SMC core clock. */
 card->sclk=pm->idle.sclk;card->mclk=pm->performance.mclk;card->wm_low_sclk=pm->levels[0].sclk;card->wm_low_mclk=pm->levels[0].mclk;
 rdn_bandwidth_update_clocks(card);
 update(card,MCLK_PWRMGT_CNTL,MPLL_PWRMGT_OFF,card->power_memory_dpm?0:MPLL_PWRMGT_OFF);
 rdn_log(card->os,RDN_LOG_INFO,"Cayman SMC: start firmware");
 err=rdn_smc_start(card);if(err)goto fail;
 err=rdn_smc_message(card,card->crtc_on?PPSMC_MSG_HasDisplay:PPSMC_MSG_NoDisplay);if(err)goto fail;
 rdn_log(card->os,RDN_LOG_INFO,"Cayman SMC: enable core DPM");
 update(card,SCLK_PWRMGT_CNTL,SCLK_PWRMGT_OFF,0);
 update(card,GENERAL_PWRMGT,GLOBAL_PWRMGT_EN,GLOBAL_PWRMGT_EN);
 err=rdn_smc_message(card,PPSMC_MSG_SwitchToSwState);if(err)goto fail;
 pm->enabled=true;
 err=rdn_smc_message(card,PPSMC_MSG_CollectCAC_PowerCorreln);if(err)goto fail;
 err=rdn_smc_message(card,PPSMC_CACLongTermAvgEnable);if(err)goto fail;
 rdn_log(card->os,RDN_LOG_INFO,"Cayman SMC: enable CAC");
 err=rdn_smc_message(card,PPSMC_MSG_EnableCac);if(err)goto fail;
 rdn_log(card->os,RDN_LOG_INFO,"Cayman SMC: enable TDP clamping");
 err=rdn_smc_message(card,PPSMC_TDPClampingActive);if(err)goto fail;
 err=rdn_cayman_dpm_force_low(card,false);if(err)goto fail;
 if((RREG32(GENERAL_PWRMGT)&(GLOBAL_PWRMGT_EN|THERMAL_PROTECTION_DIS))!=GLOBAL_PWRMGT_EN){err=-EIO;goto fail;}
 if(card->power_graphics_gating){err=rdn_cayman_gating_enable(card,&pm->gating);if(err)goto fail;}
 pm->enabled=true;
 rdn_log(card->os,RDN_LOG_INFO,"Cayman SMC active: thermal 90 C, TDP %u W, near TDP %u W, core %u-%u MHz, memory %u MHz",
  pm->calibration.tdp,pm->calibration.near_tdp,pm->idle.sclk/100,pm->performance.sclk/100,pm->idle.mclk/100);
 return 0;
fail:;
 int stopped=rdn_cayman_dpm_disable(card,pm);if(stopped)return stopped;
 /* Leave a verified low state after a partial SMC start. */
 from=pm->performance;rdn_pm_get_clocks(card,&from.sclk,&from.mclk);
 (void)rdn_pm_set(card,&from,&pm->idle,RDN_PM_SCLK|RDN_PM_VOLTAGE);
 return err;
}
