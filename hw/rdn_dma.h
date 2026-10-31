/* Async DMA rings. Callers serialize access and pin every referenced range. */
#ifndef RDN_DMA_H
#define RDN_DMA_H
#include "rdn_accel.h"
struct rdn_dma {
 struct rdn_accel *accel;
 uint32_t reg_offset, ring_offset, ring_words, wptr, fence_offset, emitted, epoch;
 bool ready;
};
struct rdn_dma_fence {
 const struct rdn_dma *owner;
 uint32_t epoch, sequence;
};
/* Zero-initialize the ring once; keep its epoch across stop/restart. */
int rdn_dma_init(struct rdn_dma *, struct rdn_accel *, unsigned engine,
                 uint32_t ring_offset, uint32_t ring_bytes, uint32_t fence_offset);
/* Stop fetch only. Retain storage until quiesce succeeds or a GPU reset completes. */
void rdn_dma_stop(struct rdn_dma *);
int rdn_dma_quiesce(struct rdn_dma *, uint32_t timeout_ms);
int rdn_dma_copy(struct rdn_dma *, uint64_t src, uint64_t dst, uint32_t bytes,
                 uint64_t wait_semaphore, uint64_t signal_semaphore, struct rdn_dma_fence *fence);
bool rdn_dma_done(struct rdn_dma *, const struct rdn_dma_fence *fence);
int rdn_dma_wait(struct rdn_dma *, const struct rdn_dma_fence *fence, uint32_t timeout_ms);
int rdn_cp_semaphore(struct rdn_accel *, uint64_t address, bool wait);
#endif
