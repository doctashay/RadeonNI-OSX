/* Evergreen immediate-fill packet and boundary model. SPDX-License-Identifier: MIT */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "../hw/rdn_cp_dma_readback.h"

static unsigned checks,failures;
static void check(int okay,const char *label)
{ ++checks;if(!okay){++failures;printf("FAIL %u: %s\n",checks,label);} }

static void valid_case(uint64_t address,uint32_t bytes,uint32_t value)
{
 struct rdn_cp_dma_fill fill={{address,bytes,0},value,0};
 struct rdn_cp_dma_span ib={0x9000000000ull,4096,0};
 uint8_t raw[4098];uint32_t words[1024],count=0,cursor=5,consumed=0;
 memset(raw,0x5a,sizeof(raw));
 check(!rdn_cp_dma_fill_build_list(&fill,1,&ib,raw+1,4096,&count),"valid aligned span with unaligned CPU output");
 memcpy(words,raw+1,count*4);
 check(count==7+6*((bytes+1048575)/1048576),"one MiB chunk count");
 check(raw[0]==0x5a&&raw[1+count*4]==0x5a&&raw[4097]==0x5a,"output canaries");
 check(words[0]==0xc0034300u&&words[1]==0x1980003cu&&words[2]==0xffffffffu&&words[3]==0&&words[4]==10,"literal synchronization prefix");
 while(cursor+6<=count-2){
  uint32_t part=words[cursor+5];uint64_t dst=words[cursor+3]|((uint64_t)words[cursor+4]<<32);
  check(words[cursor]==0xc0044100u&&words[cursor+1]==value&&
    words[cursor+2]==(cursor+6==count-2?0xc0000000u:0x40000000u)&&
    dst==address+consumed&&part&&part<=1048576&&part<=bytes-consumed&&!(part&3),
    "immediate source, exact destination and final-only CP_SYNC");
  consumed+=part;cursor+=6;
 }
 check(consumed==bytes&&words[cursor]==0xc0004200u&&words[cursor+1]==0,"exact extent and PFP synchronization");
}

int main(void)
{
 const uint32_t sizes[]={4,4096,1048572,1048576,1048580,87764992,134217728};
 const uint32_t literal[]={0xc0034300u,0x1980003cu,0xffffffffu,0,10,
  0xc0044100u,0x01234567u,0xc0000000u,0x1234000u,0xf,16,0xc0004200u,0};
 struct rdn_cp_dma_span ib={0x9000000000ull,4096,0},saved_ib;
 struct rdn_cp_dma_fill fills[18],saved[18];
 uint32_t out[1026],before[1026],count,i,total=0;
 for(i=0;i<sizeof(sizes)/sizeof(sizes[0]);++i){
  valid_case(0x02200000,sizes[i],0);
  valid_case(0xf01234000ull,sizes[i],0xa5a5a5a5u);
 }
 valid_case((1ull<<40)-4,4,0x01234567u);
 memset(fills,0,sizeof(fills));fills[0].destination.address=0xf01234000ull;
 fills[0].destination.bytes=16;fills[0].value=0x01234567;
 check(!rdn_cp_dma_fill_build_list(fills,1,&ib,out,sizeof(out),&count)&&count==13&&!memcmp(out,literal,sizeof(literal)),"literal Mesa-compatible immediate fill");
 memset(fills,0,sizeof(fills));
 for(i=0;i<18;++i){
  uint32_t bytes=i?4:RDN_CP_DMA_FILL_MAX_BYTES-17*4;
  fills[i].destination.address=0x2200000+total;fills[i].destination.bytes=bytes;
  fills[i].value=i;total+=bytes;
 }
 check(!rdn_cp_dma_fill_build_list(fills,18,&ib,out,sizeof(out),&count)&&count==877,"max fragmentation fits one4KiB IB");
 memcpy(saved,fills,sizeof(saved));saved_ib=ib;
 memset(out,0x5a,sizeof(out));memcpy(before,out,sizeof(out));count=0xbeef;
#define REJECT(f,n,b,o,c,w) do { \
 check(rdn_cp_dma_fill_build_list((f),(n),(b),(o),(c),(w))==-EINVAL,"malformed fill rejected"); \
 check(!memcmp(out,before,sizeof(out))&&count==0xbeef,"failure leaves output/count unchanged"); \
} while(0)
 REJECT(fills,0,&ib,out,sizeof(out),&count);
 REJECT(fills,19,&ib,out,sizeof(out),&count);
 REJECT(NULL,1,&ib,out,sizeof(out),&count);
 REJECT(fills,1,NULL,out,sizeof(out),&count);
 REJECT((void *)(UINTPTR_MAX-8),18,&ib,out,sizeof(out),&count);
 REJECT(fills,1,(void *)(UINTPTR_MAX-8),out,sizeof(out),&count);
 REJECT(fills,18,&ib,NULL,sizeof(out),&count);
 REJECT(fills,18,&ib,out,877*4-1,&count);
 REJECT(fills,18,&ib,out,sizeof(out),NULL);
 REJECT(fills,18,&ib,out,sizeof(out),out);
 REJECT(fills,18,&ib,fills,sizeof(out),&count);
 REJECT(fills,18,&ib,&ib,sizeof(out),&count);
 REJECT(fills,18,&ib,out,sizeof(out),(uint32_t *)fills);
 REJECT(fills,18,&ib,out,sizeof(out),(uint32_t *)&ib);
 REJECT(fills,18,&ib,(void *)(UINTPTR_MAX-8),sizeof(out),&count);
 fills[0].reserved=1;REJECT(fills,18,&ib,out,sizeof(out),&count);memcpy(fills,saved,sizeof(saved));
 fills[0].destination.reserved=1;REJECT(fills,18,&ib,out,sizeof(out),&count);memcpy(fills,saved,sizeof(saved));
 fills[0].destination.bytes=0;REJECT(fills,18,&ib,out,sizeof(out),&count);memcpy(fills,saved,sizeof(saved));
 fills[0].destination.bytes++;REJECT(fills,18,&ib,out,sizeof(out),&count);memcpy(fills,saved,sizeof(saved));
 fills[0].destination.address++;REJECT(fills,18,&ib,out,sizeof(out),&count);memcpy(fills,saved,sizeof(saved));
 fills[0].destination.bytes+=4;REJECT(fills,18,&ib,out,sizeof(out),&count);memcpy(fills,saved,sizeof(saved));
 fills[0].destination.address=1ull<<40;REJECT(fills,18,&ib,out,sizeof(out),&count);memcpy(fills,saved,sizeof(saved));
 fills[0].destination.address=(1ull<<40)-4;REJECT(fills,18,&ib,out,sizeof(out),&count);memcpy(fills,saved,sizeof(saved));
 fills[1].destination.address=fills[0].destination.address;REJECT(fills,18,&ib,out,sizeof(out),&count);memcpy(fills,saved,sizeof(saved));
 fills[0].destination.address=ib.address;REJECT(fills,18,&ib,out,sizeof(out),&count);memcpy(fills,saved,sizeof(saved));
 fills[0].destination.address=ib.address-4;REJECT(fills,18,&ib,out,sizeof(out),&count);memcpy(fills,saved,sizeof(saved));
 ib.address=fills[0].destination.address+fills[0].destination.bytes-64;REJECT(fills,18,&ib,out,sizeof(out),&count);ib=saved_ib;
 ib.reserved=1;REJECT(fills,18,&ib,out,sizeof(out),&count);ib=saved_ib;
 ib.address++;REJECT(fills,18,&ib,out,sizeof(out),&count);ib=saved_ib;
 ib.address=1ull<<40;REJECT(fills,18,&ib,out,sizeof(out),&count);ib=saved_ib;
 ib.bytes=4097;REJECT(fills,18,&ib,out,sizeof(out),&count);ib=saved_ib;
 ib.bytes=3504;REJECT(fills,18,&ib,out,sizeof(out),&count);ib=saved_ib;
 ib.bytes=0;REJECT(fills,18,&ib,out,sizeof(out),&count);
#undef REJECT
 printf("%s: %u immediate-fill checks\n",failures?"FAIL":"PASS",checks);
 return !!failures;
}
