/* End-of-pipe fences: packet ABI, completion ordering and sequence wrap. */
#include <stdio.h>
#include <string.h>
#include "../hw/rdn_accel.h"
#include "../hw/rdn_accel_reg.h"
static unsigned failures, checks;
static uint32_t aperture[2048],rlc,ucode_words,first_word,last_word;
static int reject_rlc;
static uint32_t ring_rptr;
static uint32_t read_reg(void *cookie,uint32_t reg){return reg==0x3f00?rlc:reg==CP_RB_RPTR?ring_rptr:0;}
static void write_reg(void *cookie,uint32_t reg,uint32_t value){if(reg==0x3f00&&!reject_rlc)rlc=value;if(reg==0x3f30){if(!ucode_words)first_word=value;last_word=value;ucode_words++;}}
static void delay(void *cookie,uint32_t us){}
static void check(int ok,const char *name){checks++;if(!ok){printf("FAIL: %s\n",name);failures++;}}
static uint32_t word(struct rdn_accel *a,unsigned i){uint32_t x=rdn_vram_read32(a,i*4);return a->swapped?__builtin_bswap32(x):x;}
int main(void)
{
 struct rdn_os os={0};struct rdn_card card={0};struct rdn_accel a;uint32_t seq;
 os.mmio_read32=read_reg;os.mmio_write32=write_reg;os.delay_us=delay;card.os=&os;
 for(unsigned family=0;family<2;family++)for(unsigned swapped=0;swapped<2;swapped++)for(unsigned irq=0;irq<2;irq++) {
  memset(&a,0,sizeof(a));memset(aperture,0,sizeof(aperture));card.family=family?RDN_FAMILY_CAYMAN:RDN_FAMILY_TURKS;
  a.card=&card;a.aperture=aperture;a.aperture_size=sizeof(aperture);a.ring_words=1024;
  a.vram_base=0x120000000ull;a.fence_offset=4096;a.fence_memory=true;a.fence_interrupt=irq;a.swapped=swapped;
  check(!rdn_fence_emit(&a,&seq)&&seq==1,"emit succeeds and returns a pending sequence");
  unsigned eop=5;
  check(word(&a,eop)==(uint32_t)PACKET3(PACKET3_EVENT_WRITE_EOP,4),"EOP follows the cache synchronization packet");
  check(word(&a,eop+1)==(0x14u|(5u<<8)),"cache-flush timestamp event is selected");
  check(word(&a,eop+2)==0x20001000u&&word(&a,eop+3)==(1u|(1u<<29)|(irq?2u<<24:0)),"64-bit GPU address, 32-bit store and optional timestamp interrupt");
  check(word(&a,eop+4)==1&&word(&a,eop+5)==0&&a.wptr==16,"completion payload and ring alignment");
  check(!rdn_fence_done(&a,seq),"submission alone does not signal completion");
  rdn_vram_write32(&a,a.fence_offset,1);check(rdn_fence_done(&a,seq),"GPU completion word signals the fence");
  rdn_vram_write32(&a,a.fence_offset,0xffffffffu);check(!rdn_fence_done(&a,0),"wrapped pending sequence remains pending");
  rdn_vram_write32(&a,a.fence_offset,0);check(rdn_fence_done(&a,0xffffffffu),"wrapped completed sequence is retired");
 }
 for(unsigned swapped=0;swapped<2;swapped++)for(unsigned irq=0;irq<2;irq++) {
  memset(&a,0,sizeof(a));memset(aperture,0,sizeof(aperture));card.family=RDN_FAMILY_TURKS;
  a.card=&card;a.aperture=aperture;a.aperture_size=sizeof(aperture);a.ring_words=1024;a.fence_interrupt=irq;a.swapped=swapped;
  check(!rdn_fence_emit(&a,&seq)&&seq==1&&a.wptr==16,"register fence fits the reserved ring space");
  check(word(&a,7)==(uint32_t)PACKET3(PACKET3_SET_CONFIG_REG,1)&&word(&a,9)==(WAIT_3D_IDLE_bit|WAIT_3D_IDLECLEAN_bit),"register fence retains GPU idle ordering");
  check(word(&a,10)==(uint32_t)PACKET3(PACKET3_SET_CONFIG_REG,1)&&word(&a,12)==1,"register sequence is written before the interrupt");
  check(word(&a,13)==(irq?PACKET0(CP_INT_STATUS,0):0x80000000u)&&word(&a,14)==(irq?(uint32_t)RB_INT_STAT:0x80000000u),"register fence conditionally emits a ring interrupt");
 }
 for(unsigned family=0;family<2;++family) {
  memset(&a,0,sizeof(a));memset(aperture,0,sizeof(aperture));
  card.family=family?RDN_FAMILY_CAYMAN:RDN_FAMILY_TURKS;
  a.card=&card;a.aperture=aperture;a.aperture_size=sizeof(aperture);a.ring_words=1024;
  a.fence_memory=true;a.fence_offset=4096;seq=99;ring_rptr=18;
  check(rdn_ib_submit(&a,8192,16,&seq)<0,"IB plus fence rejects insufficient combined space");
  check(a.wptr==0&&a.fence_emitted==0&&seq==99,"failed reservation publishes neither work nor completion");
  ring_rptr=64;
  check(!rdn_ib_submit(&a,8192,16,&seq)&&seq==1&&a.wptr==32,"IB and completion commit after one reservation");
 }
 ring_rptr=0;
 unsigned char firmware[4096];memset(firmware,0,sizeof(firmware));
 firmware[0]=1;firmware[1]=2;firmware[2]=3;firmware[3]=4;
 card.family=RDN_FAMILY_TURKS;rlc=0;ucode_words=0;
 check(rdn_rlc_start(&a,firmware,4096)<0&&!ucode_words,"wrong-family firmware size rejects before programming");
 check(!rdn_rlc_start(&a,firmware,3072)&&ucode_words==768&&first_word==0x01020304&&last_word==0,"Turks RLC uses 768 big-endian words");
 check(!rdn_rlc_start(&a,0,0)&&ucode_words==768,"running RLC is preserved for existing clients");
 card.family=RDN_FAMILY_CAYMAN;rlc=0;ucode_words=0;
 check(!rdn_rlc_start(&a,firmware,4096)&&ucode_words==1024,"Cayman RLC uses its own firmware length");
 rlc=0;reject_rlc=1;check(rdn_rlc_start(&a,firmware,4096)<0,"failed RLC enable is reported");
 printf("%s: %u fence and RLC checks\n",failures?"FAIL":"PASS",checks);return failures!=0;
}

