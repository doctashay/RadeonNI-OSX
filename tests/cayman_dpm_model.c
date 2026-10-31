/* Cayman SMC setup and rollback with modeled MMIO and ATOM commands. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../hw/rdn_cayman_dpm.h"
#include "../hw/linux/ppsmc.h"
static uint32_t registers[0x10000/4],ram[0xc000/4],addr,sclk=25000,mclk=125000;
static unsigned checks,failures,writes,fail_message,clock_writes,voltage_writes;
static unsigned display_notified,fail_once,enabled_levels;
static uint32_t drop_sram_address;
static uint64_t time_now;
static uint64_t now(void *c){return time_now;}
static void check(int ok,const char *name){checks++;if(!ok){failures++;printf("FAIL %s\n",name);}}
static void p16(uint8_t *p,unsigned v){p[0]=v;p[1]=v>>8;}
static void p24(uint8_t *p,unsigned v){p16(p,v);p[2]=v>>16;}
static void p32(uint8_t *p,uint32_t v){p24(p,v);p[3]=v>>24;}
static uint32_t read_reg(void *c,uint32_t r){if(r==0x204)return ram[addr/4];return registers[r/4];}
static void write_reg(void *c,uint32_t r,uint32_t v){writes++;if(r==0x200)addr=v&0xffff;else if(r==0x204){if(addr==drop_sram_address)drop_sram_address=0;else ram[addr/4]=v;}else if(r==0x20c){if((v&255)==PPSMC_MSG_HasDisplay||(v&255)==PPSMC_MSG_NoDisplay)display_notified++;registers[r/4]=(v&~0xff00u)|((((v&255)==fail_message || ((v&255)==PPSMC_MSG_SetEnabledLevels && registers[0x81c/4]>1))?255:1)<<8);if((v&255)==PPSMC_MSG_SetEnabledLevels)enabled_levels=registers[0x81c/4];if((v&255)==PPSMC_MSG_Halt)registers[0x208/4]|=4;if(fail_once&&(v&255)==fail_message){fail_message=0;fail_once=0;}}else registers[r/4]=v;}
static void delay(void *c,uint32_t us){}
static void log_msg(void *c,enum rdn_log_level level,const char *f,va_list a){if(getenv("RDN_TEST_VERBOSE")){vfprintf(stderr,f,a);fputc(10,stderr);}}
int __wrap_atom_execute_table(struct atom_context *ctx,int index,uint32_t *params,int size)
{
 uint32_t v=le32_to_cpu(params[0]);
 if(index==GetIndexIntoMasterTable(COMMAND,GetEngineClock))params[0]=cpu_to_le32(sclk);
 else if(index==GetIndexIntoMasterTable(COMMAND,GetMemoryClock))params[0]=cpu_to_le32(mclk);
 else if(index==GetIndexIntoMasterTable(COMMAND,SetEngineClock)){sclk=v;registers[0x600/4]=0x12345678;registers[0x604/4]=0x87654321;clock_writes++;}
 else if(index==GetIndexIntoMasterTable(COMMAND,SetMemoryClock)){mclk=v;clock_writes++;}
 else if(index==GetIndexIntoMasterTable(COMMAND,DynamicMemorySettings)){registers[0x2774/4]=(v&0xffffff)^le32_to_cpu(params[1]);registers[0x2778/4]=(v&0xffffff)+le32_to_cpu(params[1]);}
 else if(index==GetIndexIntoMasterTable(COMMAND,SetVoltage)) {
  unsigned type=v&255,mode=(v>>8)&255,mv=v>>16;
  if(type==6)params[0]=cpu_to_le32((1040u<<16)|6);
  else if(mode==2)params[0]=cpu_to_le32(type==1?0x88000:0x4800);
  else if(mode==1)params[0]=cpu_to_le32(type==1?(mv>=1040?0x80000:mv==990?0x88000:mv==950?0:0x8000):(mv>=1150?0x4800:mv>=1100?0x800:mv>=1000?0:0x4000));
  else voltage_writes++;
 }else if(index==GetIndexIntoMasterTable(COMMAND,ComputeMemoryEnginePLL)) {
  if((v>>24)==COMPUTE_MEMORY_PLL_PARAM) {
   COMPUTE_MEMORY_ENGINE_PLL_PARAMETERS_V5 *m=(void *)params;unsigned clock=v&0xffffff,post=clock<40000?2:1;
   memset(m,0,sizeof(*m));m->ucPostDiv=post;m->ulFbDiv.usFbDiv=cpu_to_le16(clock*(1u<<(2*post))/10000);return 0;
  }
  COMPUTE_MEMORY_ENGINE_PLL_PARAMETERS_V3 *args=(void *)params;unsigned clock=v&0xffffff;uint32_t fb=(uint64_t)clock*4*16384/2700;memset(args,0,sizeof(*args));args->ucPostDiv=4;args->ulFbDiv.usFbDiv=cpu_to_le16(fb>>14);args->ulFbDiv.usFbDivFrac=cpu_to_le16(fb&16383);
 }
 return 0;
}
int main(void)
{
 uint8_t bios[4096]={0};struct atom_context ctx={0};struct rdn_card card={0};struct rdn_os os={0};
 struct rdn_cayman_dpm *pm=calloc(1,sizeof(*pm));uint8_t *fw=calloc(1,RDN_CAYMAN_SMC_SIZE);struct rdn_cayman_calibration c;
 ctx.bios=bios;ctx.cmd_table=0x40;ctx.data_table=0x140;card.os=&os;card.family=RDN_FAMILY_CAYMAN;card.atom.ctx=&ctx;
 os.mmio_read32=read_reg;os.mmio_write32=write_reg;os.delay_us=delay;os.log=log_msg;os.time_ms=now;
 unsigned vi=GetIndexIntoMasterTable(COMMAND,SetVoltage),ci=GetIndexIntoMasterTable(COMMAND,ComputeMemoryEnginePLL);
 p16(bios+0x40+4+vi*2,0x240);bios[0x242]=1;bios[0x243]=2;p16(bios+0x40+4+ci*2,0x248);bios[0x24a]=1;bios[0x24b]=3;p16(bios+0x40+4+2*GetIndexIntoMasterTable(COMMAND,DynamicMemorySettings),0x250);bios[0x252]=2;bios[0x253]=1;
 p16(bios+0x140+4+2*GetIndexIntoMasterTable(DATA,PowerPlayInfo),0x300);
 uint8_t *pp=bios+0x300;p16(pp,256);pp[2]=5;pp[3]=1;pp[5]=2;pp[6]=4;pp[7]=12;pp[8]=24;
 p16(pp+9,88);p16(pp+11,96);p16(pp+13,144);p16(pp+19,88);pp[25]=15;pp[28]=4;pp[29]=7;pp[30]=66;
 p32(pp+66,95);p32(pp+70,50);p32(pp+74,60000);p16(pp+78,200);pp[200]=4;
 pp[88]=0;pp[89]=2;pp[90]=0;pp[91]=1;pp[92]=1;pp[93]=0;pp[94]=0;pp[95]=1;
 p24(pp+120,25000);p24(pp+123,15000);p16(pp+126,900);p16(pp+128,950);
 p24(pp+96,42000);p24(pp+99,125000);p16(pp+102,950);p16(pp+104,1150);
 p24(pp+108,72500);p24(pp+111,125000);p16(pp+114,0xff01);p16(pp+116,1150);p16(pp+144,5);p16(pp+168,0x400);p32(pp+179,53300);p32(pp+183,40000);
 unsigned volts[]={900,950,990,1040},std[]={900,950,1040,1041},leak[]={9,11,16,16},vio[]={950,1000,1100,1150};
 for(unsigned i=0;i<4;i++){p16(pp+201+6*i,std[i]);p32(pp+203+6*i,leak[i]);}
 p16(bios+0x140+4+2*GetIndexIntoMasterTable(DATA,FirmwareInfo),0x600);p16(bios+0x600,sizeof(ATOM_FIRMWARE_INFO_V2_2));bios[0x602]=2;bios[0x603]=2;
 p16(bios+0x600+offsetof(ATOM_FIRMWARE_INFO_V2_2,usCoreReferenceClock),2700);
 p16(bios+0x600+offsetof(ATOM_FIRMWARE_INFO_V2_2,usMemoryReferenceClock),10000);
 p16(bios+0x140+4+2*GetIndexIntoMasterTable(DATA,VRAM_Info),0x900);
 uint8_t *vr=bios+0x900;p16(vr,80);vr[2]=2;vr[3]=1;vr[16]=1;p16(vr+6,20);
 p16(vr+20,9);p16(vr+22,12);p16(vr+24,0x2a04/4);vr[26]=4;p16(vr+27,0x28a0/4);vr[29]=4;p16(vr+30,0xffff);vr[32]=0x80;
 p32(vr+33,20000);p32(vr+37,0x12345678);p32(vr+41,0x100);
 p32(vr+45,150000);p32(vr+49,0x87654321);p32(vr+53,0x200);
 p16(bios+0x140+4+2*GetIndexIntoMasterTable(DATA,VoltageObjectInfo),0x700);p16(bios+0x700,80);bios[0x702]=1;bios[0x703]=2;
 for(unsigned j=0;j<2;j++){uint8_t *obj=bios+0x704+j*38;obj[0]=j?4:1;obj[1]=38;obj[2]=128;obj[18]=4;for(unsigned i=0;i<4;i++)p16(obj+24+4*i,j?vio[i]:volts[i]);}
 FILE *f=fopen("firmware/CAYMAN_smc.bin","rb");if(!f||fread(fw,1,RDN_CAYMAN_SMC_SIZE,f)!=RDN_CAYMAN_SMC_SIZE)return 2;fclose(f);
 card.power_memory_dpm=true;card.power_graphics_gating=true;
 check(!rdn_cayman_calibration(&card,&c)&&c.tdp==95&&c.near_tdp==50&&c.gpio_mask==0x88000&&c.vddci_mask==0x4800,"BIOS voltage, leakage and TDP calibration");
 check(!writes&&!clock_writes&&!voltage_writes,"calibration performs no hardware writes");
 check(!rdn_cayman_dpm_prepare(&card,pm)&&pm->prepared&&pm->table.driverState.levelCount==3,"SMC states prepare without changing clocks");
 check(!writes&&!clock_writes&&!voltage_writes,"table preparation performs no hardware writes");
 const uint8_t *serialized=(const void *)&pm->table.dpmLevels[2].sclk.sclk_value;
 check(serialized[0]==0&&serialized[1]==1&&serialized[2]==0x1b&&serialized[3]==0x34,"SMC clock fields have big-endian storage");
 const uint8_t *fb=(const void *)&pm->table.dpmLevels[2].sclk.vCG_SPLL_FUNC_CNTL_3;
 uint32_t divider=(((uint32_t)fb[0]<<24)|((uint32_t)fb[1]<<16)|((uint32_t)fb[2]<<8)|fb[3])&0x3ffffff;
 check((uint64_t)divider*2700/(4*16384)==72499,"BIOS divider respects the requested maximum clock");
 check(pm->table.dpmLevels[0].vddc.index==0&&pm->table.dpmLevels[2].vddc.index==3,"SMC voltage indices match calibrated LUT order");
 unsigned before=writes;check(rdn_cayman_dpm_enable(&card,pm,fw,4)<0&&writes==before,"invalid firmware rejected before writes");
 registers[0x27e8/4]=0x0c;before=writes;
 check(rdn_cayman_dpm_enable(&card,pm,fw,RDN_CAYMAN_SMC_SIZE)<0&&writes==before,"unexpected initial arbitration bank rejects before mutation");
 registers[0x27e8/4]=0;
 fw[RDN_CAYMAN_SMC_SIZE-1]^=1;before=writes;
 check(rdn_cayman_dpm_enable(&card,pm,fw,RDN_CAYMAN_SMC_SIZE)<0&&writes==before,"corrupt firmware rejected before hardware writes");fw[RDN_CAYMAN_SMC_SIZE-1]^=1;
 registers[0x600/4]=0x12345678;registers[0x604/4]=0x87654321;
 registers[0x2774/4]=0x11223344;registers[0x2778/4]=0x55667788;
 registers[0x27f0/4]=0xabcdef01;registers[0x27fc/4]=0xabcdef02;
 registers[0x2808/4]=0x123456;registers[0x25bc/4]=0x110;card.crtc_on=true;
 registers[0x63c/4]=0x7604;registers[0x644/4]=1;registers[0x648/4]=32;
 int enableResult=rdn_cayman_dpm_enable(&card,pm,fw,RDN_CAYMAN_SMC_SIZE);if(enableResult)printf("enable result=%d state=%x soft=%x cac=%x spll=%x arb=%x mc=%x header=%x\n",enableResult,pm->state_addr,pm->soft_addr,pm->cac_addr,pm->spll_addr,pm->arb_addr,pm->memory.address,ram[0x120/4]);
 check(!enableResult&&pm->enabled,"SMC firmware and power containment enable");
 check(display_notified==2,"SMC receives display state before ownership transfer");
 check(registers[0x27f0/4]==0x11223344&&registers[0x27fc/4]==0x55667788&&(registers[0x27e8/4]&255)==0x0b,"SMC starts on copied F1 arbitration timing");
 check((ram[pm->arb_addr/4]>>24)==0x0b,"firmware arbitration bank matches hardware handoff");
 const uint8_t *initial=(const void *)&pm->table.initialState.level.sclk.vCG_SPLL_FUNC_CNTL;
 check(initial[0]==0x12&&initial[1]==0x34&&initial[2]==0x56&&initial[3]==0x78,"initial PLL image comes from ATOM-programmed hardware");
 check(pm->table.initialState.level.arbRefreshState==0&&pm->table.dpmLevels[0].arbRefreshState==3&&pm->table.dpmLevels[1].arbRefreshState==4,"initial and driver states use separate arbitration entries");
 check((registers[0x63c/4]&5)==1&&!(registers[0x648/4]&32)&&!(registers[0x644/4]&1),"thermal protection and core/memory DPM enabled");
 check((registers[0x72c/4]&0x3fc007)==((90u<<14)|2),"digital thermal source and 90 C default");
 check(!rdn_cayman_dpm_force_low(&card,true)&&registers[0x81c/4]==1,"manual low cap uses SMC level restriction");
 uint32_t thermal_high=registers[0x72c/4],power_high=registers[0x63c/4];
 check(!rdn_cayman_dpm_force_high(&card)&&enabled_levels==0&&registers[0x81c/4]==1&&(registers[0x20c/4]&255)==PPSMC_MSG_SetForcedLevels,"high mode requests firmware forced high state");
 check(registers[0x72c/4]==thermal_high&&registers[0x63c/4]==power_high,"high mode preserves thermal and power controls");
 fail_message=PPSMC_MSG_SetEnabledLevels;
 check(rdn_cayman_dpm_force_high(&card)<0&&(registers[0x20c/4]&255)==PPSMC_MSG_SetEnabledLevels,"high request stops on rejected level-enable command");
 fail_message=0;check(!rdn_cayman_dpm_force_low(&card,false)&&registers[0x81c/4]==0,"automatic mode clears forced high state");
 before=writes;check(rdn_cayman_dpm_temperature_limit(&card,79)<0&&writes==before,"unsafe thermal threshold rejected before writes");
 check(!rdn_cayman_dpm_temperature_limit(&card,90)&&(registers[0x72c/4]&0x3fc007)==((90u<<14)|2),"90 C performance threshold programmed");
 before=writes;check(rdn_cayman_dpm_temperature_limit(&card,101)<0&&writes==before,"threshold above 100 C rejected without writes");
 uint32_t thermal_before=registers[0x72c/4];
 check(!rdn_cayman_dpm_temperature_limit(&card,100)&&(registers[0x72c/4]&~0x3fc007u)==(thermal_before&~0x3fc007u),"thermal test threshold preserves unrelated sensor controls");
 check(!rdn_cayman_dpm_temperature_limit(&card,85),"normal thermal threshold restored");
 check(ram[pm->state_addr/4]==0x00040300,"uploaded state table header and maximum voltage index");
 check(sclk==25000&&mclk==125000,"handover uses BIOS low core with unchanged memory clock");
 check(pm->idle.sclk==25000&&pm->idle.vddc==900&&pm->intermediate.sclk==42000&&pm->intermediate.vddc==950,"BIOS low state and BIOS intermediate core retain distinct VIDs");
 check(pm->idle.mclk==15000&&pm->idle.vddci==950,"low state retains BIOS memory clock and I/O VID");
 pm->thermal_stage=1;
 check(!rdn_cayman_dpm_policy(&card,pm,true,false)&&pm->table.driverState.levelCount==2&&enabled_levels==0&&registers[0x81c/4]==1,"warm policy excludes high level before forcing highest remaining state");
 uint32_t state_header=pm->state_addr+offsetof(NISLANDS_SMC_STATETABLE,driverState);
 check(((ram[state_header/4]>>16)&255)==2,"warm level count reaches firmware SRAM");
 fail_message=PPSMC_MSG_EnableCac;fail_once=1;pm->thermal_stage=0;
 check(rdn_cayman_dpm_policy(&card,pm,true,false)<0&&!pm->voltage_fault&&pm->table.driverState.levelCount==2&&enabled_levels==1,"failed state expansion restores previous count and low cap");
 check(!rdn_cayman_dpm_policy(&card,pm,true,false)&&pm->table.driverState.levelCount==3,"cool policy restores all three levels");
 check(rdn_cayman_thermal_stage(84999,90,0,false)==0&&rdn_cayman_thermal_stage(85000,90,0,false)==1,"warm threshold selects intermediate at 85 C");
 check(rdn_cayman_thermal_stage(90000,90,1,false)==2&&rdn_cayman_thermal_stage(86000,90,2,false)==2,"hot state remains low until recovery");
 check(rdn_cayman_thermal_stage(85000,90,2,false)==1&&rdn_cayman_thermal_stage(80001,90,1,false)==1&&rdn_cayman_thermal_stage(80000,90,1,false)==0,"five-degree hysteresis across both thermal stages");
 for(unsigned limit=80;limit<=100;limit++) {
  check(rdn_cayman_thermal_stage(limit*1000,limit,0,false)==2,"every allowed limit enters stage 2 at its boundary");
  check(rdn_cayman_thermal_stage((limit-5)*1000+1,limit,2,false)==2&&rdn_cayman_thermal_stage((limit-5)*1000,limit,2,false)==1,"every limit retains five-degree stage-2 hysteresis");
  check(rdn_cayman_thermal_stage((limit-10)*1000,limit,1,false)==0,"every limit releases stage 1 ten degrees below limit");
 }
 check(rdn_cayman_thermal_stage(20000,90,0,true)==2,"invalid sensor selects deepest cooling state");
 struct rdn_pm_bios_state selected;
 check(!rdn_pm_bios_state(&card,RDN_PM_BIOS_PERFORMANCE,&selected)&&selected.count==3&&selected.level[0].mclk==15000&&selected.level[2].vddc==1040,"BIOS performance selection resolves all three levels and virtual voltage");
 check(!rdn_pm_bios_state(&card,RDN_PM_BIOS_VIDEO,&selected)&&selected.classification==0x400&&selected.vclk==53300&&selected.dclk==40000,"BIOS video selection retains decoder clocks");
 check(rdn_pm_bios_state(&card,RDN_PM_BIOS_ULV,&selected)<0&&!pm->ulv_supported,"missing BIOS ULV state is not invented");
 check(pm->gating.enabled&&pm->gating.count>100,"upstream graphics and medium-grain gating sequences are active");
 check(pm->memory.regs.last==4&&pm->table.dpmLevels[0].ACIndex==2&&pm->table.dpmLevels[2].ACIndex==4,"MC timing image includes derived mode registers and per-level indices");
 NISLANDS_SMC_HW_PERFORMANCE_LEVEL regenerated={0};uint32_t savedPll=registers[0x628/4],savedDll=registers[0x60c/4];
 registers[0x628/4]^=0x40000000;registers[0x60c/4]^=0x10000000;
 check(!rdn_cayman_memory_value(&card,&pm->memory,pm->levels[2].mclk,&regenerated)&&!memcmp(&regenerated.mclk,&pm->table.dpmLevels[2].mclk,sizeof(regenerated.mclk)),"memory clock targets use the immutable boot PLL image");
 registers[0x628/4]=savedPll;registers[0x60c/4]=savedDll;
 const unsigned char *lowMemory=(const void *)&pm->table.dpmLevels[0].mclk.mclk_value;
 check(lowMemory[0]==0&&lowMemory[1]==0&&lowMemory[2]==0x3a&&lowMemory[3]==0x98,"low memory clock is serialized as 150 MHz");
 unsigned savedClockWrites=clock_writes,savedVoltageWrites=voltage_writes;
 card.wm_clock=148500;card.wm_htotal=2200;card.wm_vdisplay=1080;card.wm_vtotal=1125;
 unsigned displayMask=0,blank=0;
 check(rdn_cayman_memory_display(&card,&displayMask,&blank)&&displayMask==1&&blank==666,"single-display blanking budget allows memory switching");
 check(!rdn_cayman_dpm_context(&card,pm,true,false)&&pm->video&&pm->levels[0].sclk==42000&&pm->levels[0].mclk==125000,"video context selects BIOS core and memory floor");
 check((registers[0x714/4]&0xf00000f)==0xd00000c,"active display uses vblank or watermark and memory waits for vblank");
 check(!rdn_cayman_dpm_policy(&card,pm,false,false),"video context admits normal firmware policy");
 pm->thermal_stage=2;
 check(!rdn_cayman_dpm_context(&card,pm,true,false)&&pm->levels[0].sclk==25000&&pm->levels[0].mclk==125000,"critical cooling overrides video core floor while retaining video memory floor");
 pm->thermal_stage=0;
 check(!rdn_cayman_dpm_context(&card,pm,false,true)&&!pm->memory_enabled&&pm->levels[0].mclk==125000&&pm->levels[0].vddci==1150,"modeset pin raises memory and I/O VID at every core level");
 struct rdn_card second=card;struct rdn_card *peers[RDN_NUM_CRTC]={&card,&second};second.display_crtc=1;card.display_peers=peers;
 check(!rdn_cayman_dpm_context(&card,pm,false,false)&&pm->display_mask==3&&!pm->memory_enabled&&pm->levels[0].mclk==125000,"unsynchronized multi-display configuration retains high memory floor");
 second.crtc_on=false;
 check(!rdn_cayman_dpm_context(&card,pm,false,false)&&pm->memory_enabled&&pm->levels[0].mclk==15000&&pm->levels[0].vddci==950,"removing the second display restores BIOS low memory and I/O VID");
 card.display_peers=0;card.wm_vtotal=1081;
 check(!rdn_cayman_dpm_context(&card,pm,false,false)&&!pm->memory_enabled,"insufficient blanking retains high memory even when display mask is unchanged");
 card.wm_vtotal=1125;
 check(!rdn_cayman_dpm_context(&card,pm,false,false)&&pm->memory_enabled,"adequate blanking restores memory DPM");
 fail_message=PPSMC_MSG_EnableCac;fail_once=1;
 check(rdn_cayman_dpm_context(&card,pm,true,false)<0&&!pm->voltage_fault&&!pm->video&&pm->memory_enabled&&enabled_levels==1,"failed video-state activation restores prior memory tables and low cap");
 check(!rdn_cayman_dpm_policy(&card,pm,false,false),"restored context can resume automatic selection");
 uint32_t drop=pm->memory.address+offsetof(SMC_NIslands_MCRegisters,data)+2*sizeof(SMC_NIslands_MCRegisterSet);
 uint32_t oldMc=ram[drop/4];drop_sram_address=drop;
 check(rdn_cayman_dpm_context(&card,pm,true,false)<0&&!pm->voltage_fault&&!pm->video&&ram[drop/4]==oldMc,"partial MC-table upload restores original memory contents before resuming");
 check(!rdn_cayman_dpm_policy(&card,pm,false,false),"partial-upload recovery retains a usable policy");
 pm->idle_requested=true;
 check(!rdn_cayman_dpm_policy(&card,pm,false,false)&&enabled_levels==1,"idle policy selects the BIOS low core/memory state");
 pm->idle_requested=false;
 check(!rdn_cayman_dpm_policy(&card,pm,false,false)&&enabled_levels==0,"new work releases the idle cap");
 check(clock_writes==savedClockWrites&&voltage_writes==savedVoltageWrites,"runtime context changes never program static clock or voltage tables");
 registers[0x788/4]=3;registers[0x78c/4]=270000;registers[0x76c/4]=0x01815e87;registers[0x770/4]=0x87;registers[0x774/4]=0x00cd0c00;
 check(!rdn_cayman_dpm_fan(&card,pm,40000,90,false)&&pm->fan_percent==98,"fan cooling decreases by at most two percentage points per second");
 time_now=100;check(!rdn_cayman_dpm_fan(&card,pm,40000,90,false)&&pm->fan_percent==98,"fan decrease respects its rate limit");
 for(unsigned i=0;i<25;i++){time_now+=1000;(void)rdn_cayman_dpm_fan(&card,pm,40000,90,false);}
 check(pm->fan_percent==60&&(registers[0x76c/4]&255)==81,"cool fan policy retains a calibrated sixty-percent minimum");
 check(!rdn_cayman_dpm_fan(&card,pm,90000,100,false)&&pm->fan_percent==100,"high temperature immediately requests full fan even with a 100 C clock limit");
 pm->thermal_stage=1;
 check(!rdn_cayman_dpm_fan(&card,pm,84000,90,false)&&pm->fan_percent==100,"thermal recovery retains full fan until the clock cap clears");
 pm->thermal_stage=0;
 pm->fan_percent=60;registers[0x78c/4]=0;
 check(!rdn_cayman_dpm_fan(&card,pm,40000,90,false)&&pm->fan_percent==100,"missing tachometer immediately restores full fan");
 for(unsigned i=0;i<7;i++)(void)rdn_cayman_dpm_fan(&card,pm,40000,90,false);
 check(rdn_cayman_dpm_fan(&card,pm,40000,90,false)<0&&pm->fan_percent==100,"persistent missing tachometer reports a fault while retaining full fan");
 pm->fan_percent=60;registers[0x78c/4]=270000;
 check(!rdn_cayman_dpm_fan(&card,pm,40000,90,true)&&pm->fan_percent==100,"invalid temperature forces full fan");
 before=writes;check(rdn_pm_fan_percent(&card,59)<0&&rdn_pm_fan_percent(&card,101)<0&&writes==before,"out-of-range fan requests cannot write controls");
 before=writes;check(rdn_cayman_dpm_voltage(&card,pm,1000)<0&&writes==before,"non-BIOS undervolt rejected before writes");
 before=writes;check(rdn_cayman_dpm_voltage(&card,pm,900)<0&&writes==before,"excessive high-state undervolt rejected before writes");
 before=writes;check(rdn_cayman_dpm_voltage(&card,pm,1100)<0&&writes==before,"overvoltage rejected before writes");
 uint32_t uvthermal=registers[0x72c/4];
 check(!rdn_cayman_dpm_voltage(&card,pm,990)&&pm->table.dpmLevels[2].vddc.index==2,"990 mV experiment selects BIOS VID");
 check(registers[0x72c/4]==uvthermal&&pm->table.dpmLevels[0].vddc.index==0,"undervolt preserves thermal limit and idle VID");
 check(!rdn_cayman_dpm_voltage(&card,pm,950)&&pm->table.dpmLevels[2].vddc.index==1,"950 mV high-state trial selects the BIOS idle VID");
 check(!rdn_cayman_dpm_voltage(&card,pm,1040)&&pm->table.dpmLevels[2].vddc.index==3,"stock VID restoration succeeds");
 fail_message=PPSMC_MSG_EnableCac;fail_once=1;
 check(rdn_cayman_dpm_voltage(&card,pm,990)<0&&!pm->voltage_fault&&pm->table.dpmLevels[2].vddc.index==3,"transient re-enable failure restores stock table");
 fail_message=PPSMC_MSG_Halt;
 check(rdn_cayman_dpm_voltage(&card,pm,990)<0&&pm->voltage_fault,"unrecoverable firmware error blocks further experiments");
 fail_message=PPSMC_MSG_Halt;pm->voltage_fault=false;
 check(rdn_cayman_dpm_disable(&card,pm)<0&&pm->enabled&&pm->touched&&!pm->gating.enabled,"failed teardown retains firmware ownership with clock gating disabled");
 fail_message=0;pm->voltage_fault=false;
 rdn_cayman_dpm_disable(&card,pm);check(!pm->enabled&&registers[0x63c/4]==0x7604&&!(registers[0x208/4]&0x801),"disable stops firmware and restores static power controls");
 check((registers[0x27e8/4]&255)==0x0a&&registers[0x27f0/4]==0xabcdef01&&registers[0x27fc/4]==0xabcdef02&&registers[0x2808/4]==0x123456&&registers[0x25bc/4]==0x110,"disable returns to F0 and restores saved F1 timing controls");
 fail_message=PPSMC_TDPClampingActive;
 check(rdn_cayman_dpm_enable(&card,pm,fw,RDN_CAYMAN_SMC_SIZE)<0&&!pm->enabled&&registers[0x63c/4]==0x7604&&sclk==25000,"failed power containment acknowledgement rolls back to low core");
 fail_message=PPSMC_MSG_NoDisplay;card.crtc_on=false;
 check(rdn_cayman_dpm_enable(&card,pm,fw,RDN_CAYMAN_SMC_SIZE)<0&&!pm->enabled&&!pm->arb_switched&&(registers[0x27e8/4]&255)==0x0a,"display notification failure restores static arbitration ownership");
 p32(vr+49,0x12345678);mclk=125000;
 check(!rdn_cayman_dpm_prepare(&card,pm)&&pm->memory.regs.last==1,"constant MC registers and derived mode commands are excluded from transitions");
 p32(vr+49,0x87654321);
 card.power_memory_dpm=false;card.power_graphics_gating=false;fail_message=0;mclk=125000;
 check(!rdn_cayman_dpm_prepare(&card,pm)&&!pm->memory_enabled,"memory DPM requires explicit diagnostic opt-in");
 check(pm->levels[0].mclk==125000&&pm->levels[0].vddci==1150,"recovery state retains high memory clock and I/O voltage");
 check(pm->intermediate.sclk==55000,"fixed-memory recovery retains the previously deployed middle core clock");
 check(pm->memory.regs.data[2].value[0]==pm->memory.regs.data[4].value[0],"clamped memory levels use matching MC timing rows");
 check(!pm->memory.regs.last&&!pm->table.dpmLevels[0].ACIndex&&!pm->table.dpmLevels[2].ACIndex,"fixed memory mode does not switch MC register tables");
 check(!rdn_cayman_dpm_enable(&card,pm,fw,RDN_CAYMAN_SMC_SIZE)&&!pm->gating.enabled,"recovery policy enables core DPM without new clock gating");
 check((registers[0x714/4]&0xf00000f)==0xf00000f,"inactive display channels and fixed-memory gap are ignored");
 check((registers[0x648/4]&0x20)!=0,"fixed memory policy disables firmware memory PLL control");
 pm->thermal_stage=1;before=writes;
 check(!rdn_cayman_dpm_context(&card,pm,false,false)&&writes==before,"non-video thermal caps do not rebuild unchanged firmware context");
 check(!pm->table.dpmLevels[2].displayWatermark,"fixed-memory recovery retains the established display watermark set");
 fail_message=PPSMC_MSG_SetForcedLevels;
 check(!rdn_cayman_dpm_force_low(&card,true)&&enabled_levels==1,"low cap uses NoForcedLevel rather than blocking SetForcedLevels");
 fail_message=0;
 check(!rdn_cayman_dpm_disable(&card,pm),"recovery policy tears down cleanly");
 pp[200]=17;before=writes;check(rdn_cayman_calibration(&card,&c)<0&&writes==before,"oversized leakage table rejected without hardware writes");pp[200]=4;
 p32(pp+66,151);check(rdn_cayman_calibration(&card,&c)<0,"unreasonable BIOS power limit rejected");
 free(pm);free(fw);printf("%s: %u Cayman DPM checks\n",failures?"FAIL":"PASS",checks);return failures!=0;
}
