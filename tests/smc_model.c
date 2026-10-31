/* SMC SRAM byte order, bounds, firmware readback and mailbox failures. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../hw/rdn_smc.h"
#include "../hw/linux/ppsmc.h"
static uint32_t ram[0xc000/4],regs[256],addr;
static unsigned writes,delays,checks,failures,reply=1;
static int corrupt;
static void check(int ok,const char *name){checks++;if(!ok){failures++;printf("FAIL %s\n",name);}}
static uint32_t read_reg(void *c,uint32_t r){if(r==0x204)return ram[addr/4]^(corrupt?1:0);return regs[(r-0x200)/4];}
static void write_reg(void *c,uint32_t r,uint32_t v){writes++;if(r==0x200)addr=v&0xffff;else if(r==0x204)ram[addr/4]=v;else if(r==0x20c)regs[3]=(v&~0xff00u)|(reply<<8);else regs[(r-0x200)/4]=v;}
static void delay(void *c,uint32_t us){delays++;}
int main(void)
{
 struct rdn_os os={0};struct rdn_card card={0};unsigned before;uint32_t value=0;
 uint8_t bytes[]={0x12,0x34,0x56,0x78,0x9a};uint8_t *fw=malloc(RDN_CAYMAN_SMC_SIZE);
 os.mmio_read32=read_reg;os.mmio_write32=write_reg;os.delay_us=delay;card.os=&os;card.family=RDN_FAMILY_CAYMAN;
 ram[0x104/4]=0x00abcdef;
 check(!rdn_smc_copy(&card,0x100,bytes,5)&&ram[0x100/4]==0x12345678&&ram[0x104/4]==0x9aabcdef,"SRAM is big endian with preserved trailing bytes");
 check(!rdn_smc_read(&card,0x100,&value)&&value==0x12345678,"SRAM dword roundtrip");
 before=writes;check(rdn_smc_copy(&card,0xbffc,bytes,5)<0&&writes==before,"overflow rejected before writes");
 check(rdn_smc_copy(&card,0x102,bytes,4)<0&&writes==before,"unaligned SRAM rejected before writes");
 card.family=RDN_FAMILY_TURKS;check(rdn_smc_write(&card,0,1)<0&&writes==before,"other ASICs untouched");card.family=RDN_FAMILY_CAYMAN;
 check(rdn_smc_message(&card,PPSMC_MSG_HasDisplay)<0&&writes==before,"stopped SMC rejects mailbox");
 regs[2]=0x801;reply=1;check(!rdn_smc_message(&card,PPSMC_MSG_HasDisplay),"mailbox acknowledges success");
 reply=255;check(rdn_smc_message(&card,PPSMC_MSG_HasDisplay)<0,"firmware rejection propagates");
 reply=1;regs[2]|=4;check(!rdn_smc_halt(&card),"halt requires stopped-mode acknowledgement");
 regs[2]&=~4u;delays=0;check(rdn_smc_halt(&card)<0&&delays==100000,"halt timeout is bounded even after mailbox acknowledgement");
 reply=0;delays=0;check(rdn_smc_message(&card,PPSMC_MSG_HasDisplay)<0&&delays==100000,"mailbox timeout is bounded");
 for(unsigned i=0;i<RDN_CAYMAN_SMC_SIZE;i++)fw[i]=(i*17+5)&255;
 before=writes;check(rdn_smc_load(&card,fw,RDN_CAYMAN_SMC_SIZE-4)<0&&writes==before,"wrong firmware size cannot stop SMC");
 reply=1;check(!rdn_smc_load(&card,fw,RDN_CAYMAN_SMC_SIZE)&&!(regs[2]&0x801),"firmware loads and verifies while stopped");
 check(ram[0x100/4]==0x05162738&&ram[0xbffc/4]==0,"firmware placement and SRAM clearing");
 check(regs[(0x218-0x200)/4]==0x12051205&&regs[(0x23c-0x200)/4]==0x08720872,"Cayman interrupt vector window");
 corrupt=1;check(rdn_smc_load(&card,fw,RDN_CAYMAN_SMC_SIZE)<0&&!(regs[2]&0x801),"failed firmware readback leaves SMC stopped");
 free(fw);printf("%s: %u SMC checks\n",failures?"FAIL":"PASS",checks);return failures!=0;
}
