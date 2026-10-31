/* Virtual voltage ABI, idle selection and submission gating. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../hw/rdn_pm.h"
#include "../hw/rdn_accel.h"
#include "../hw/atom/atombios.h"
static unsigned checks,failures,calls,query,crtc_writes,layout_writes,fan_writes,drop_fan;
static uint32_t fan0,fan1,fan2,memory_misc,sequencer;
static uint32_t tach=270000,edges=3,pin;
static uint32_t answer=1100,sclk=25000;
static int fail_command,hook_error;
static char order[32];static unsigned used;
static void put16(uint8_t *p,unsigned v){p[0]=v;p[1]=v>>8;}
static void put24(uint8_t *p,unsigned v){put16(p,v);p[2]=v>>16;}
static void check(int good,const char *name){checks++;if(!good){failures++;printf("FAIL %s\n",name);}}
static uint32_t read_reg(void *c,uint32_t r){if(r==0x660)return pin;if(r==0x788)return edges;if(r==0x78c)return tach;if(r==0x2a00)return memory_misc;if(r==0x28c8)return sequencer;if(r==0x76c)return fan0;if(r==0x770)return fan1;if(r==0x774)return fan2;return 0;}
static void write_reg(void *c,uint32_t r,uint32_t v){if(r==0x76c){fan_writes++;fan0=v;}if(r==0x774){fan_writes++;if(drop_fan)drop_fan=0;else fan2=v;}if(r==0x6e70)crtc_writes++;if(r==0x6b0c||r==0xca0)layout_writes++;}
static void log_msg(void *c,enum rdn_log_level l,const char *f,va_list a){}
static void delay(void *c,uint32_t v){}
int __wrap_atom_execute_table(struct atom_context *ctx,int index,uint32_t *params,int bytes)
{
 uint32_t v=le32_to_cpu(params[0]);calls++;
 if(fail_command)return -5;
 if(index==GetIndexIntoMasterTable(COMMAND,SetVoltage)){
  if((v&0xffff)==6||(v&0xffff)==0x601){query=v;params[0]=cpu_to_le32((answer<<16)|(v&0xffff));}
  else {order[used++]='V';}
 }else if(index==GetIndexIntoMasterTable(COMMAND,SetEngineClock)){order[used++]='C';sclk=v;}
 else if(index==GetIndexIntoMasterTable(COMMAND,GetEngineClock))params[0]=cpu_to_le32(sclk);
 else if(index==GetIndexIntoMasterTable(COMMAND,GetMemoryClock))params[0]=cpu_to_le32(125000);
 return 0;
}
static int hook(void *p){(*(unsigned *)p)++;return hook_error;}
int main(void)
{
 uint8_t bios[2048]={0};struct atom_context ctx={0};struct rdn_os os={0};
 struct rdn_card card={0};struct rdn_pm_state state,original,boot={25000,125000,900,1150},perf={72500,125000,0xff01,1150};
 unsigned i,before,hook_calls=0;struct rdn_accel accel={0};uint32_t aperture[2048]={0};
 os.mmio_read32=read_reg;os.mmio_write32=write_reg;os.delay_us=delay;os.log=log_msg;card.os=&os;card.family=RDN_FAMILY_CAYMAN;card.atom.ctx=&ctx;
 ctx.bios=bios;ctx.cmd_table=0x40;ctx.data_table=0x140;
 put16(bios+0x40+4+2*GetIndexIntoMasterTable(COMMAND,SetVoltage),0x240);bios[0x242]=1;
 for(i=2;i<=3;i++){
  bios[0x243]=i;state=perf;query=0;
  check(!rdn_pm_resolve_state(&card,&state)&&state.vddc==1100,"virtual ID resolves to ASIC voltage");
  check(query==(i==2?6:0x601),"revision-specific upstream query ABI and zero maximum ID");
 }
 original=perf;
 for(i=0;i<4;i++){
  answer=i==0?0:i==1?0xff01:i==2?499:1501;state=perf;
  check(rdn_pm_resolve_state(&card,&state)<0&&!memcmp(&state,&original,sizeof(state)),"invalid voltage preserves caller state");
 }
 answer=1100;state=perf;state.vddc=0xff02;before=calls;
 check(rdn_pm_resolve_state(&card,&state)<0&&calls==before,"unsupported virtual IDs reject before execution");
 bios[0x243]=4;state=perf;before=calls;
 check(rdn_pm_resolve_state(&card,&state)<0&&calls==before,"unsupported table revision rejects before execution");
 bios[0x243]=3;fail_command=1;state=perf;
 check(rdn_pm_resolve_state(&card,&state)<0&&state.vddc==0xff01,"interpreter errors preserve virtual state");fail_command=0;
 answer=0;used=0;check(rdn_pm_set(&card,&boot,&perf,RDN_PM_ALL)<0&&!used,"unresolved voltage never raises clocks");
 card.crtc_on=true;card.wm_clock=268000;card.wm_hdisplay=2560;card.wm_htotal=2720;crtc_writes=layout_writes=0;
 answer=1100;used=0;check(!rdn_pm_set(&card,&boot,&perf,RDN_PM_VOLTAGE|RDN_PM_SCLK)&&used==2&&order[0]=='V'&&order[1]=='C',"voltage rises before core clock");
 perf.vddc=1100;used=0;check(!rdn_pm_set(&card,&perf,&boot,RDN_PM_VOLTAGE|RDN_PM_SCLK)&&used==2&&order[0]=='C'&&order[1]=='V',"core clock falls before voltage");
 check(!crtc_writes&&!layout_writes,"core-only transitions preserve scanout reads and live FIFO allocation");
 put16(bios+0x140+4+2*GetIndexIntoMasterTable(DATA,PowerPlayInfo),0x300);
 put16(bios+0x300,128);bios[0x305]=2;bios[0x306]=2;bios[0x307]=12;bios[0x308]=2;
 put16(bios+0x309,32);put16(bios+0x30b,40);put16(bios+0x30d,80);
 bios[0x320]=0;bios[0x321]=0;bios[0x322]=1;bios[0x323]=1;
 put24(bios+0x328,25000);put24(bios+0x32b,15000);put16(bios+0x32e,900);put16(bios+0x330,950);
 put24(bios+0x334,42000);put24(bios+0x337,125000);put16(bios+0x33a,950);put16(bios+0x33c,1150);
 put16(bios+0x350,5);put16(bios+0x352,0x400);
 check(!rdn_pm_idle_state(&card,&state)&&state.sclk==25000&&state.mclk==15000&&state.vddc==900,"UI idle state selected from BIOS rather than boot defaults");
 check(rdn_pm_display_idle_state(&card,125000,&state)<0,"non-UI states cannot supply a display idle voltage");
 put16(bios+0x352,5);
 check(!rdn_pm_display_idle_state(&card,125000,&state)&&state.sclk==42000&&state.vddc==950,"display idle keeps a BIOS-paired core voltage and memory clock");
 put16(bios+0x352,0x400);
 put16(bios+0x350,8);check(rdn_pm_idle_state(&card,&state)<0,"boot-only state is not an idle fallback");
 accel.card=&card;accel.aperture=aperture;accel.ring_words=1024;accel.before_submit=hook;accel.submit_cookie=&hook_calls;
 hook_error=-5;check(rdn_ring_begin(&accel,16)==-5&&hook_calls==1&&!accel.wptr&&!aperture[0],"failed clock restore blocks submission without touching ring");
 hook_error=0;check(!rdn_ring_begin(&accel,16)&&hook_calls==2,"successful clock gate permits submission");
 fan0=0x01815e51;fan1=0x002a1e87;fan2=0x00cd1437;
 check(!rdn_pm_fan_full(&card)&&fan0==0x01815e87&&fan2==0x00cd0c00,"maximum fan PWM preserves unrelated controller bits");
 fan0=0x01815e51;fan2=0x00cd1437;drop_fan=1;
 check(rdn_pm_fan_full(&card)<0&&fan0==0x01815e51&&fan2==0x00cd1437,"failed fan readback restores original controller configuration");
 fan1=0;before=fan_writes;check(rdn_pm_fan_full(&card)<0&&fan_writes==before,"missing PWM calibration rejects without writes");
 fan1=0x87;card.family=RDN_FAMILY_TURKS;before=fan_writes;
 check(rdn_pm_fan_full(&card)<0&&fan_writes==before,"Cayman fan controls never touch Turks");
 memory_misc=3u<<28;sequencer=0;check(rdn_pm_memory_clock_ready(&card),"DDR3 memory clocks do not require the GDDR5 sequencer");
 memory_misc=5u<<28;check(!rdn_pm_memory_clock_ready(&card),"GDDR5 memory clock is blocked before sequencer startup");
 sequencer=1;check(rdn_pm_memory_clock_ready(&card),"GDDR5 memory clock is allowed after sequencer startup");
 memory_misc=3u<<28;card.family=RDN_FAMILY_TURKS;
 put16(bios+0x350,5);put16(bios+0x352,5);
 put24(bios+0x328,10000);put24(bios+0x32b,15000);put16(bios+0x32e,900);
 put24(bios+0x334,80000);put24(bios+0x337,60000);put16(bios+0x33a,1050);
 before=calls;
 check(!rdn_pm_tuning_state(&card,800,600,&state)&&state.vddc==1050,"stock tuning preserves the BIOS voltage");
 check(!rdn_pm_tuning_state(&card,820,615,&state)&&state.vddc==1050&&state.vddci==1150,"small tuning changes preserve both BIOS voltages");
 check(!rdn_pm_tuning_state(&card,840,630,&state),"five-percent tuning ceiling is accepted");original=state;
 check(rdn_pm_tuning_state(&card,841,630,&state)<0&&!memcmp(&state,&original,sizeof(state)),"core ceiling rejects without changing caller state");
 check(rdn_pm_tuning_state(&card,840,631,&state)<0,"memory ceiling rejects");
 check(rdn_pm_tuning_state(&card,799,600,&state)<0&&rdn_pm_tuning_state(&card,800,599,&state)<0,"tuning cannot select an unpaired low-voltage state");
 check(rdn_pm_tuning_state(&card,0xffffffffu,600,&state)<0,"overflowing tuning input rejects");
 memory_misc=5u<<28;check(rdn_pm_tuning_state(&card,820,615,&state)<0,"unvalidated GDDR5 tuning rejects");
 memory_misc=3u<<28;card.family=RDN_FAMILY_CAYMAN;check(rdn_pm_tuning_state(&card,820,615,&state)<0,"Cayman tuning is disabled");
 check(calls==before,"tuning validation never executes a voltage or clock command");
 struct rdn_user_board_sensors sensors;
 memset(bios,0,sizeof(bios));
 put16(bios+0x140+4+2*GetIndexIntoMasterTable(DATA,PowerPlayInfo),0x300);
 put16(bios+0x300,88);bios[0x302]=5;bios[0x319]=15;bios[0x31c]=4;bios[0x31d]=7;bios[0x31e]=66;
 put16(bios+0x140+4+2*GetIndexIntoMasterTable(DATA,FirmwareInfo),0x400);
 put16(bios+0x400,sizeof(ATOM_FIRMWARE_INFO_V2_2));bios[0x402]=2;bios[0x403]=2;
 put16(bios+0x400+offsetof(ATOM_FIRMWARE_INFO_V2_2,usCoreReferenceClock),2700);
 put16(bios+0x140+4+2*GetIndexIntoMasterTable(DATA,VoltageObjectInfo),0x500);
 put16(bios+0x500,22);bios[0x502]=1;bios[0x503]=2;bios[0x504]=1;bios[0x505]=18;bios[0x506]=0x80;
 before=fan_writes;
 rdn_pm_board_sensors(&card,&sensors);
 check(sensors.fan_status==RDN_SENSOR_VALID&&sensors.fan_rpm==6000,"tach period converts to RPM using board reference clock");
 check(sensors.rail_status==RDN_SENSOR_NOT_IMPLEMENTED&&!sensors.rail_microvolts&&sensors.voltage_control_id==0x80,"GPIO VID is not reported as measured rail voltage");
 check(sensors.fan_min_rpm==700&&sensors.fan_max_rpm==6600,"BIOS fan limits decode hundreds of RPM");
 pin=1u<<9;rdn_pm_board_sensors(&card,&sensors);check(sensors.fan_rpm==1500,"divided reference clock is applied");
 pin=1u<<8;rdn_pm_board_sensors(&card,&sensors);check(sensors.fan_status==RDN_SENSOR_INVALID,"unknown external tach clock rejects");pin=0;
 tach=0;rdn_pm_board_sensors(&card,&sensors);check(sensors.fan_status==RDN_SENSOR_INVALID&&!sensors.fan_rpm,"zero counter is unavailable, not zero RPM");
 tach=0xffffffff;rdn_pm_board_sensors(&card,&sensors);check(sensors.fan_status==RDN_SENSOR_INVALID,"inaccessible tach register rejects");tach=270000;
 edges=1;rdn_pm_board_sensors(&card,&sensors);check(sensors.fan_status==RDN_SENSOR_INVALID,"pulse count mismatch rejects");edges=3;
 card.family=RDN_FAMILY_TURKS;rdn_pm_board_sensors(&card,&sensors);check(sensors.fan_status==RDN_SENSOR_NOT_IMPLEMENTED,"unvalidated ASIC never uses Cayman tach map");
 bios[0x31c]=0x80;rdn_pm_board_sensors(&card,&sensors);check(sensors.fan_status==RDN_SENSOR_NOT_PRESENT,"BIOS no-fan flag is exposed");
 check(fan_writes==before,"sensor reads never program fan controls");
 printf("%s: %u voltage, ordering, idle and submission checks\n",failures?"FAIL":"PASS",checks);return failures!=0;
}
