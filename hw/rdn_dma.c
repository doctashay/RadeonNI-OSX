/*
 * Copyright 2010, 2013 Advanced Micro Devices, Inc.
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


/* From Linux r600_dma.c/evergreen_dma.c/ni_dma.c, 21a7f44fd0765fcc5194130b28767786eba1d1e7. */
#include "rdn_dma.h"
#include "linux/nid.h"

#ifndef EBUSY
#define EBUSY 16
#endif
#ifndef ETIMEDOUT
#define ETIMEDOUT 110
#endif
#define RADEON_PACKET_TYPE3 3
#define RDN_SEM_WAIT_ON_SIGNAL (1u << 12)
#define RDN_SEM_SIGNAL (6u << 29)
#define RDN_SEM_WAIT (7u << 29)

static bool dma_address(uint64_t address, uint64_t bytes)
{
 return !(address & 3) && address < (1ull << 40) && bytes <= (1ull << 40) - address;
}

static void dma_emit(struct rdn_dma *dma, uint32_t word)
{
 struct rdn_accel *a = dma->accel;
 /* DMA_RB_SWAP_ENABLE consumes native-endian words on the G5. */
 a->aperture[dma->ring_offset / 4 + dma->wptr] = word;
 dma->wptr = (dma->wptr + 1) & (dma->ring_words - 1);
}

static int dma_reserve(struct rdn_dma *dma, uint32_t words)
{
 struct rdn_os *os = dma->accel->card->os;
 uint64_t start = os->time_ms(os->cookie);
 if (!dma->ready || words + 15 >= dma->ring_words) return -EINVAL;
 for (;;) {
  uint32_t rptr = (rdn_rreg(dma->accel->card, DMA_RB_RPTR + dma->reg_offset) & 0x3fffc) >> 2;
  uint32_t free_words = (rptr - dma->wptr - 1) & (dma->ring_words - 1);
  if (free_words >= words + 15) return 0;
  if (os->time_ms(os->cookie) - start >= 100) return -EBUSY;
  os->delay_us(os->cookie, 1);
 }
}

static void dma_commit(struct rdn_dma *dma)
{
 while (dma->wptr & 15) dma_emit(dma, DMA_PACKET(DMA_PACKET_NOP, 0, 0, 0));
 rdn_hdp_flush(dma->accel);
 rdn_wreg(dma->accel->card, DMA_RB_WPTR + dma->reg_offset, dma->wptr << 2);
 (void)rdn_rreg(dma->accel->card, DMA_RB_WPTR + dma->reg_offset);
}

void rdn_dma_stop(struct rdn_dma *dma)
{
 if (!dma || !dma->accel) return;
 uint32_t reg = DMA_RB_CNTL + dma->reg_offset;
 rdn_wreg(dma->accel->card, reg, rdn_rreg(dma->accel->card, reg) & ~DMA_RB_ENABLE);
 dma->ready = false;
 dma->epoch++;
}

int rdn_dma_quiesce(struct rdn_dma *dma, uint32_t timeout_ms)
{
 if (!dma || !dma->accel || !dma->ready) return -EINVAL;
 struct rdn_dma_fence last = {dma, dma->epoch, dma->emitted};
 int error = rdn_dma_wait(dma, &last, timeout_ms);
 if (error) return error;
 struct rdn_os *os = dma->accel->card->os;
 uint64_t start = os->time_ms(os->cookie);
 while (!(rdn_rreg(dma->accel->card, DMA_STATUS_REG + dma->reg_offset) & DMA_IDLE)) {
  if (os->time_ms(os->cookie) - start >= timeout_ms) return -ETIMEDOUT;
  os->delay_us(os->cookie, 1);
 }
 rdn_dma_stop(dma);
 return 0;
}

bool rdn_dma_done(struct rdn_dma *dma, const struct rdn_dma_fence *fence)
{
 return dma && dma->ready && fence && fence->owner == dma && fence->epoch == dma->epoch &&
        (int32_t)(rdn_vram_read32(dma->accel, dma->fence_offset) - fence->sequence) >= 0;
}

int rdn_dma_wait(struct rdn_dma *dma, const struct rdn_dma_fence *fence, uint32_t timeout_ms)
{
 if (!dma || !dma->ready || !fence || fence->owner != dma || fence->epoch != dma->epoch) return -EINVAL;
 struct rdn_os *os = dma->accel->card->os;
 uint64_t start = os->time_ms(os->cookie);
 while (!rdn_dma_done(dma, fence)) {
  if (os->time_ms(os->cookie) - start >= timeout_ms) return -ETIMEDOUT;
  os->delay_us(os->cookie, 1);
 }
 rdn_hdp_flush(dma->accel);
 return 0;
}

int rdn_dma_init(struct rdn_dma *dma, struct rdn_accel *a, unsigned engine,
                 uint32_t ring_offset, uint32_t ring_bytes, uint32_t fence_offset)
{
 if (!dma || !a || !a->ready ||
     !rdn_family_has_acceleration(a->card->family) || engine >= rdn_family_caps(a->card->family)->dma_engines ||
     ring_bytes < 4096 || ring_bytes > 65536 || (ring_bytes & (ring_bytes - 1)) ||
     (ring_offset & 255) || ring_offset > a->aperture_size ||
     ring_bytes > a->aperture_size - ring_offset || (fence_offset & 7) ||
     fence_offset > a->aperture_size || a->aperture_size - fence_offset < 8 ||
     (fence_offset < ring_offset + ring_bytes && fence_offset + 8 > ring_offset))
  return -EINVAL;
 if (dma->ready) return -EBUSY;
 uint32_t epoch = dma->epoch + 1;
 *dma = (struct rdn_dma){0};
 dma->epoch = epoch;
 dma->accel = a; dma->reg_offset = engine ? DMA1_REGISTER_OFFSET : DMA0_REGISTER_OFFSET;
 dma->ring_offset = ring_offset; dma->ring_words = ring_bytes / 4; dma->fence_offset = fence_offset;
 uint32_t rb = 0, words = dma->ring_words, reg = dma->reg_offset;
 while (words > 1) { rb++; words >>= 1; }
 rb <<= 1;
 if (RDN_BIG_ENDIAN) rb |= DMA_RB_SWAP_ENABLE | DMA_RPTR_WRITEBACK_SWAP_ENABLE;
 rdn_wreg(a->card, DMA_SEM_INCOMPLETE_TIMER_CNTL + reg, 0);
 rdn_wreg(a->card, DMA_SEM_WAIT_FAIL_TIMER_CNTL + reg, 0);
 rdn_wreg(a->card, DMA_RB_CNTL + reg, rb);
 rdn_wreg(a->card, DMA_RB_RPTR + reg, 0);
 rdn_wreg(a->card, DMA_RB_WPTR + reg, 0);
 uint64_t address = rdn_vram_addr(a, ring_offset);
 rdn_wreg(a->card, DMA_RB_BASE + reg, address >> 8);
 /* MMIO read pointers until writeback coherency is qualified. */
 rdn_wreg(a->card, DMA_RB_RPTR_ADDR_HI + reg, 0);
 rdn_wreg(a->card, DMA_RB_RPTR_ADDR_LO + reg, 0);
 uint32_t ib = DMA_IB_ENABLE;
 if (rdn_family_engine(a->card->family) == RDN_ENGINE_CAYMAN) ib |= CMD_VMID_FORCE;
 if (RDN_BIG_ENDIAN) ib |= DMA_IB_SWAP_ENABLE;
 rdn_wreg(a->card, DMA_IB_CNTL + reg, ib);
 rdn_wreg(a->card, DMA_CNTL + reg, rdn_rreg(a->card, DMA_CNTL + reg) & ~CTXEMPTY_INT_ENABLE);
 if (rdn_family_engine(a->card->family) == RDN_ENGINE_EVERGREEN) rdn_wreg(a->card, DMA_MODE, 1);
 rdn_vram_write32(a, fence_offset, 0);
 rdn_hdp_flush(a);
 rdn_wreg(a->card, DMA_RB_CNTL + reg, rb | DMA_RB_ENABLE);
 dma->ready = true;
 address = rdn_vram_addr(a, fence_offset);
 dma_emit(dma, DMA_PACKET(DMA_PACKET_WRITE, 0, 0, 1));
 dma_emit(dma, (uint32_t)address);
 dma_emit(dma, (address >> 32) & 0xff);
 dma_emit(dma, 1);
 dma_commit(dma);
 struct rdn_dma_fence test = {dma, dma->epoch, 1};
 int error = rdn_dma_wait(dma, &test, 100);
 if (error) rdn_dma_stop(dma);
 else dma->emitted = 1;
 return error;
}

static void dma_semaphore(struct rdn_dma *dma, uint64_t address, bool signal)
{
 dma_emit(dma, DMA_PACKET(DMA_PACKET_SEMAPHORE, 0, signal ? 1 : 0, 0));
 dma_emit(dma, (uint32_t)address);
 dma_emit(dma, (address >> 32) & 0xff);
}

int rdn_dma_copy(struct rdn_dma *dma, uint64_t src, uint64_t dst, uint32_t bytes,
                 uint64_t wait_semaphore, uint64_t signal_semaphore, struct rdn_dma_fence *fence)
{
 if (!dma || !dma->ready || !fence || !bytes || (bytes & 3) ||
     !dma_address(src, bytes) || !dma_address(dst, bytes) ||
     (src < dst + bytes && dst < src + bytes) ||
     (wait_semaphore && ((wait_semaphore & 7) || !dma_address(wait_semaphore, 8))) ||
     (signal_semaphore && ((signal_semaphore & 7) || !dma_address(signal_semaphore, 8))))
  return -EINVAL;
 uint32_t dwords = bytes / 4, packets = (dwords + 0xffffe) / 0xfffff;
 int error = dma_reserve(dma, packets * 5 + 8 + (wait_semaphore ? 3 : 0) + (signal_semaphore ? 3 : 0));
 if (error) return error;
 if (wait_semaphore) dma_semaphore(dma, wait_semaphore, false);
 while (dwords) {
  uint32_t count = dwords > 0xfffff ? 0xfffff : dwords;
  dma_emit(dma, DMA_PACKET(DMA_PACKET_COPY, 0, 0, count));
  dma_emit(dma, (uint32_t)dst); dma_emit(dma, (uint32_t)src);
  dma_emit(dma, (dst >> 32) & 0xff);
  dma_emit(dma, (src >> 32) & 0xff);
  src += count * 4; dst += count * 4; dwords -= count;
 }
 if (signal_semaphore) dma_semaphore(dma, signal_semaphore, true);
 uint64_t address = rdn_vram_addr(dma->accel, dma->fence_offset);
 uint32_t seq = ++dma->emitted;
 dma_emit(dma, DMA_PACKET(DMA_PACKET_FENCE, 0, 0, 0));
 dma_emit(dma, (uint32_t)address); dma_emit(dma, (address >> 32) & 0xff); dma_emit(dma, seq);
 dma_emit(dma, DMA_PACKET(DMA_PACKET_TRAP, 0, 0, 0));
 dma_emit(dma, DMA_PACKET(DMA_PACKET_SRBM_WRITE, 0, 0, 0));
 dma_emit(dma, (0xf << 16) | (0x5480 >> 2));
 dma_emit(dma, 1);
 dma_commit(dma);
 *fence = (struct rdn_dma_fence){dma, dma->epoch, seq};
 return 0;
}

/* The owner must retain this semaphore until both engines have retired it. */
int rdn_cp_semaphore(struct rdn_accel *a, uint64_t address, bool wait)
{
 if (!a || !a->ready || (address & 7) || !dma_address(address, 8)) return -EINVAL;
 int error = rdn_ring_begin(a, wait ? 5 : 3);
 if (error) return error;
 uint32_t select = wait ? RDN_SEM_WAIT : RDN_SEM_SIGNAL;
 if (rdn_family_engine(a->card->family) != RDN_ENGINE_CAYMAN) select |= RDN_SEM_WAIT_ON_SIGNAL;
 rdn_ring_emit(a, PACKET3(PACKET3_MEM_SEMAPHORE, 1));
 rdn_ring_emit(a, (uint32_t)address);
 rdn_ring_emit(a, ((address >> 32) & 0xff) | select);
 if (wait) {
  rdn_ring_emit(a, PACKET3(PACKET3_PFP_SYNC_ME, 0));
  rdn_ring_emit(a, 0);
 }
 rdn_ring_commit(a);
 return 0;
}
