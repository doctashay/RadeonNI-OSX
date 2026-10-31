/* Interpret emitted packets to check DMA addressing and ring ownership. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rdn_dma.h"
#include "linux/nid.h"

static uint32_t registers[0x10000 / 4], memory[4 * 1024 * 1024];
static struct rdn_accel accel;
static uint64_t ticks;
static unsigned copies, signals, waits, cp_count;
static uint32_t cp[16];
static bool stalled;

static void *mapped(uint64_t address, unsigned bytes)
{
   assert(address >= accel.vram_base && address - accel.vram_base <= sizeof(memory) - bytes);
   return (char *)memory + (address - accel.vram_base);
}

static uint32_t read_reg(void *cookie, uint32_t reg) { return registers[reg / 4]; }
static void write_reg(void *cookie, uint32_t reg, uint32_t value)
{
   registers[reg / 4] = value;
   unsigned bank = reg == DMA_RB_WPTR ? 0 : reg == DMA_RB_WPTR + 0x800 ? 0x800 : ~0u;
   if (bank == ~0u || stalled || !(registers[(DMA_RB_CNTL + bank) / 4] & DMA_RB_ENABLE)) return;
   unsigned words = 1u << ((registers[(DMA_RB_CNTL + bank) / 4] >> 1) & 0x1f);
   uint32_t *ring = mapped((uint64_t)registers[(DMA_RB_BASE + bank) / 4] << 8, words * 4);
   unsigned p = registers[(DMA_RB_RPTR + bank) / 4] / 4;
#define NEXT() ring[(p++) & (words - 1)]
   while ((p & (words - 1)) != value / 4) {
      uint32_t header = NEXT(), type = header >> 28;
      if (type == DMA_PACKET_COPY) {
         unsigned bytes = (header & 0xfffff) * 4;
         uint64_t dst = NEXT(), src = NEXT();
         dst |= (uint64_t)NEXT() << 32; src |= (uint64_t)NEXT() << 32;
         memcpy(mapped(dst, bytes), mapped(src, bytes), bytes); copies++;
      } else if (type == DMA_PACKET_WRITE || type == DMA_PACKET_FENCE) {
         uint64_t address = NEXT(); address |= (uint64_t)NEXT() << 32;
         *(uint32_t *)mapped(address, 4) = rdn_swap_le32(NEXT());
      } else if (type == DMA_PACKET_SEMAPHORE) {
         (void)NEXT(); (void)NEXT();
         if (header & (1u << 22)) signals++; else waits++;
      } else if (type == DMA_PACKET_SRBM_WRITE) {
         (void)NEXT(); (void)NEXT();
      } else assert(type == DMA_PACKET_NOP || type == DMA_PACKET_TRAP);
   }
   registers[(DMA_RB_RPTR + bank) / 4] = value;
   registers[(DMA_STATUS_REG + bank) / 4] = DMA_IDLE;
#undef NEXT
}
static void delay(void *cookie, uint32_t us) { ticks++; }
static uint64_t time_ms(void *cookie) { return ticks; }
void rdn_hdp_flush(struct rdn_accel *a) { }
int rdn_ring_begin(struct rdn_accel *a, uint32_t words) { cp_count = 0; return 0; }
void rdn_ring_emit(struct rdn_accel *a, uint32_t word) { assert(cp_count < 16); cp[cp_count++] = word; }
void rdn_ring_commit(struct rdn_accel *a) { }

int main(void)
{
   struct rdn_os os = {.mmio_read32=read_reg,.mmio_write32=write_reg,.delay_us=delay,.time_ms=time_ms};
   struct rdn_card card = {.os=&os,.family=RDN_FAMILY_CAYMAN};
   accel.card=&card; accel.ready=true; accel.aperture=memory;
   accel.aperture_size=sizeof(memory); accel.vram_base=0xf00000000ull;
   struct rdn_dma dma={0}; struct rdn_dma_fence fence;
   assert(!rdn_dma_init(&dma,&accel,1,0x10000,65536,0x20000));
   assert(dma.emitted==1 && !registers[DMA_RB_CNTL/4]);
   memset((char *)memory+0x100000,0x5a,0x400004);
   assert(!rdn_dma_copy(&dma,accel.vram_base+0x100000,accel.vram_base+0x700000,
                        0x400004,accel.vram_base+0x20008,accel.vram_base+0x20010,&fence));
   assert(copies==2 && waits==1 && signals==1 && rdn_dma_done(&dma,&fence));
   assert(!memcmp((char *)memory+0x100000,(char *)memory+0x700000,0x400004));
   assert(rdn_dma_copy(&dma,accel.vram_base,accel.vram_base+4,8,0,0,&fence));
   assert(rdn_dma_copy(&dma,(1ull<<40)-4,accel.vram_base,8,0,0,&fence));
   assert(!rdn_cp_semaphore(&accel,accel.vram_base+0x20008,true));
   assert(cp_count==5 && cp[2]==(0xe0000000u|15) && cp[3]==0xc0004200);
   card.family=RDN_FAMILY_TURKS;
   assert(!rdn_cp_semaphore(&accel,accel.vram_base+0x20008,false));
   assert(cp_count==3 && cp[2]==(0xc0001000u|15));
   registers[(DMA_STATUS_REG + 0x800)/4]=0;
   assert(rdn_dma_quiesce(&dma,5) && dma.ready);
   registers[(DMA_STATUS_REG + 0x800)/4]=DMA_IDLE;
   assert(!rdn_dma_quiesce(&dma,5) && !dma.ready);
   assert(!rdn_dma_done(&dma,&fence));
   assert(rdn_dma_init(&dma,&accel,1,0x10000,65536,0x20000));
   assert(!rdn_dma_init(&dma,&accel,0,0x10000,65536,0x20000));
   assert(!rdn_dma_done(&dma,&fence));
   rdn_dma_stop(&dma);
   stalled=true;
   assert(rdn_dma_init(&dma,&accel,0,0x10000,65536,0x20000));
   assert(!dma.ready && !(registers[DMA_RB_CNTL/4]&DMA_RB_ENABLE));
   puts("PASS DMA high addresses, split copies, engine bank, semaphore packets, timeout and stop");
}
