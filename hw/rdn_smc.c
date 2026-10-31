/*
 * Copyright 2011 Advanced Micro Devices, Inc.
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
 * Authors: Alex Deucher
 */

#include "rdn_smc.h"
#include "linux/ppsmc.h"
#ifndef EIO
#define EIO 5
#endif
#ifndef ETIMEDOUT
#define ETIMEDOUT 110
#endif
#define SMC_SRAM_ADDR 0x200
#define SMC_SRAM_DATA 0x204
#define SMC_IO 0x208
#define SMC_MSG 0x20c
#define SMC_ISR 0x218
#define SMC_RST_N 1u
#define SMC_STOP_MODE 4u
#define SMC_CLK_EN (1u<<11)
#define RREG(r) rdn_rreg(card,r)
#define WREG(r,v) rdn_wreg(card,r,v)

static const uint8_t cayman_smc_int_vectors[] = {
	0x12, 0x05, 0x12, 0x05,
	0x12, 0x05, 0x12, 0x05,
	0x12, 0x05, 0x12, 0x05,
	0x12, 0x05, 0x12, 0x05,
	0x12, 0x05, 0x12, 0x05,
	0x12, 0x05, 0x12, 0x05,
	0x12, 0x05, 0x12, 0x05,
	0x12, 0x05, 0x12, 0x05,
	0x12, 0x05, 0x12, 0x05,
	0x12, 0x05, 0x12, 0x05,
	0x12, 0x05, 0x12, 0x05,
	0x12, 0x05, 0x12, 0x05,
	0x12, 0x05, 0x18, 0xEA,
	0x12, 0x20, 0x1C, 0x34,
	0x1C, 0x34, 0x08, 0x72,
	0x08, 0x72, 0x08, 0x72
};


static int address(struct rdn_card *card,uint32_t addr,size_t size)
{
 if(card->family!=RDN_FAMILY_CAYMAN || (addr&3) || addr>=RDN_SMC_RAM_END ||
    size>RDN_SMC_RAM_END-addr)return -EINVAL;
 WREG(SMC_SRAM_ADDR,addr|(1u<<16));return 0;
}
int rdn_smc_read(struct rdn_card *card,uint32_t addr,uint32_t *value)
{
 int r;if(!value)return -EINVAL;r=address(card,addr,4);if(r)return r;
 *value=RREG(SMC_SRAM_DATA);return 0;
}
int rdn_smc_write(struct rdn_card *card,uint32_t addr,uint32_t value)
{
 int r=address(card,addr,4);if(r)return r;WREG(SMC_SRAM_DATA,value);return 0;
}
int rdn_smc_copy(struct rdn_card *card,uint32_t addr,const void *data,size_t size)
{
 const uint8_t *p=data;uint32_t value;size_t n;int r;
 if(!data || !size)return -EINVAL;
 r=address(card,addr,size);if(r)return r;
 while(size) {
  n=size<4?size:4;value=0;
  if(n<4){r=rdn_smc_read(card,addr,&value);if(r)return r;}
  for(size_t i=0;i<n;i++)value=(value&~(255u<<(24-8*i)))|((uint32_t)p[i]<<(24-8*i));
  r=rdn_smc_write(card,addr,value);if(r)return r;
  p+=n;addr+=4;size-=n;
 }
 return 0;
}
void rdn_smc_stop(struct rdn_card *card)
{
 if(card->family==RDN_FAMILY_CAYMAN)WREG(SMC_IO,RREG(SMC_IO)&~(SMC_RST_N|SMC_CLK_EN));
}
int rdn_smc_load(struct rdn_card *card,const void *firmware,size_t size)
{
 uint32_t r,i;int err;
 if(card->family!=RDN_FAMILY_CAYMAN || !firmware || size!=RDN_CAYMAN_SMC_SIZE)return -EINVAL;
 rdn_smc_stop(card);
 for(i=0;i<RDN_SMC_RAM_END;i+=4)rdn_smc_write(card,i,0);
 err=rdn_smc_copy(card,0x100,firmware,size);if(err)return err;
 /* The first 24 vector bytes precede the MMIO vector window. */
 for(i=24;i<64;i+=4) {
  const uint8_t *p=cayman_smc_int_vectors+i;
  r=((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
  WREG(SMC_ISR+i-24,r);
 }
 for(i=0;i<size;i+=4) {
  const uint8_t *p=(const uint8_t *)firmware+i;
  r=((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
  uint32_t actual=0;err=rdn_smc_read(card,0x100+i,&actual);
  if(err || actual!=r){rdn_smc_stop(card);return -EIO;}
 }
 return 0;
}
int rdn_smc_message(struct rdn_card *card,uint8_t message)
{
 if(card->family!=RDN_FAMILY_CAYMAN || (RREG(SMC_IO)&(SMC_RST_N|SMC_CLK_EN))!=(SMC_RST_N|SMC_CLK_EN))return -EIO;
 WREG(SMC_MSG,(RREG(SMC_MSG)&~255u)|message);
 for(unsigned i=0;i<100000;i++) {
  unsigned result=(RREG(SMC_MSG)>>8)&255;
  if(result)return result==PPSMC_Result_OK?0:-EIO;
  card->os->delay_us(card->os->cookie,1);
 }
 return -ETIMEDOUT;
}
int rdn_smc_start(struct rdn_card *card)
{
 if(card->family!=RDN_FAMILY_CAYMAN)return -EINVAL;
 WREG(SMC_IO,RREG(SMC_IO)|SMC_RST_N);
 WREG(SMC_IO,RREG(SMC_IO)|SMC_CLK_EN);
 card->os->delay_us(card->os->cookie,1000);
 return rdn_smc_message(card,PPSMC_MSG_HasDisplay);
}

int rdn_smc_halt(struct rdn_card *card)
{
 int err=rdn_smc_message(card,PPSMC_MSG_Halt);if(err)return err;
 for(unsigned i=0;i<100000;i++) {
  if(RREG(SMC_IO)&SMC_STOP_MODE)return 0;
  card->os->delay_us(card->os->cookie,1);
 }
 return -ETIMEDOUT;
}
