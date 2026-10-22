/*
 * UVD, the card's video decoder: clocks, microcode, VCPU start, the
 * ring and its fence. What Linux radeon does in uvd_v1_0.c, uvd_v2_2.c,
 * radeon_uvd.c (resume) and evergreen_set_uvd_clocks(), for Turks.
 *
 * Nothing here decodes: a client builds UVD messages and bitstream
 * buffers in video memory and submits them as an indirect buffer
 * (rdn_uvd_ib_submit); the engine's own VCPU runs them. Memory model as
 * for the 3D engine (rdn_accel.h): everything is in video memory, reached
 * through the aperture; completion is polled.
 *
 * Byte order: the engine's ring and indirect buffers are read byte-swapped
 * on a big-endian host (UVD_LMI_SWAP_CNTL 0xa, as Linux sets it), so their
 * words are in host order; messages and bitstreams are not swapped and are
 * little-endian words / plain bytes.
 *
 * Copyright (c) 2026 kouta-kun and Claude
 * SPDX-License-Identifier: MIT
 */

#ifndef RDN_UVD_H
#define RDN_UVD_H

#include "rdn_accel.h"

#define RDN_UVD_STACK_SIZE	(200 * 1024)
#define RDN_UVD_HEAP_SIZE	(256 * 1024)
#define RDN_UVD_SESSION_SIZE	(50 * 1024)
#define RDN_UVD_DEFAULT_HANDLES	10

/* Clocks while decoding (10 kHz units), as Linux sets them. */
#define RDN_UVD_VCLK		53300
#define RDN_UVD_DCLK		40000

/* Upstream diagnostic flag values; this core accepts flags == 0 only. */
#define RDN_UVD_F_UDEC_CONFIG (1u << 0)
#define RDN_UVD_F_PLL_FIRST (1u << 1)
#define RDN_UVD_F_V1_MEM (1u << 2)
#define RDN_UVD_F_LOW_CLOCKS (1u << 3)
#define RDN_UVD_F_NO_PLL (1u << 4)
#define RDN_UVD_F_KEEP_CLOCKS (1u << 5)
#define RDN_UVD_F_ADDR_OFFSET (1u << 6)
#define RDN_UVD_F_PM_GLOBAL (1u << 7)
#define RDN_UVD_F_PM_SCLK (1u << 8)
#define RDN_UVD_F_GART (1u << 9)
#define RDN_UVD_F_MINIMAL (1u << 10)

#define RDN_UVD_MAX_RING_BYTES (1u << 20)
#define RDN_UVD_MAX_TIMEOUT_MS 10000u
#define RDN_UVD_RLC_BYTES 3072u

struct rdn_uvd {
	struct rdn_accel *accel;
	uint32_t ref_freq;	/* the PLL's reference clock, 10 kHz units */
	uint32_t handles;	/* sessions the firmware may hold */
	uint32_t fw_size;
	uint32_t flags;

	/*
	 * Where things are: the CPU's view and the block's address. In video
	 * memory (rdn_uvd_init) the CPU view is the aperture and the offsets
	 * are in it. init_mem accepts caller-supplied mappings for diagnostics;
	 * Turks VCPU storage must still be VRAM, not GART/system memory.
	 */
	volatile uint8_t *vcpu_cpu, *ring_cpu;
	uint64_t vcpu_gpu, ring_gpu;
	bool in_vram;
	uint32_t vcpu_offset;	/* firmware, heap, stack, sessions (video memory only) */
	uint32_t vcpu_bytes;
	uint32_t ring_offset;	/* (video memory only) */
	uint32_t ring_words;
	uint32_t wptr;
	uint32_t fence_emitted;
	bool swapped;
	bool ready;
	bool started; /* Sticky: hardware has been given these buffers. */
	bool failed;
	bool stopped;
	uint32_t status_snapshot; /* Last boot status read while clocked. */
	uint32_t rlc_cntl_snapshot;
	bool rlc_loaded;
	bool prepared;
};

/*
 * Bytes of video memory the engine's own use needs for a firmware of
 * `fw_size` bytes (page aligned; the firmware is copied to its start).
 * `vcpu_offset` of rdn_uvd_init() is a multiple of 4096.
 */
/* Returns zero for invalid sizes or more than ten handles. */
uint32_t rdn_uvd_vcpu_bytes(uint32_t fw_size, uint32_t handles);

/* The PLL's reference clock from the VBIOS (FirmwareInfo), 10 kHz units. */
int rdn_uvd_ref_clock(struct rdn_card *card, uint32_t *ref_freq);

/* The ring takes `ring_bytes` (a power of two) and 64 bytes after it. */
#define RDN_UVD_RING_EXTRA	64

/*
 * Set the UVD PLL for VCLK and DCLK (10 kHz units); 0, 0 puts it to sleep.
 * `ref_freq` is FirmwareInfo's reference clock. Returns 0 or -errno.
 */
int rdn_uvd_set_clocks(struct rdn_card *card, uint32_t ref_freq,
		       uint32_t vclk, uint32_t dclk);

/*
 * The dividers rdn_uvd_set_clocks() would use (Linux
 * radeon_uvd_calc_upll_dividers() with the Evergreen limits): for tests.
 */
int rdn_uvd_calc_dividers(uint32_t ref_freq, uint32_t vclk, uint32_t dclk,
			  uint32_t *fb_div, uint32_t *vclk_div,
			  uint32_t *dclk_div);

/*
 * Zero-initialize the state before its first use; serialize all entry.
 * Only flags == 0 is accepted. Caller owns the memory and keeps it pinned.
 * VCPU storage fits one 256 MB segment; ring/fence and VCPU share a
 * 40-bit-addressable 4 GB segment. CPU and GPU ranges must not overlap.
 * init_mem requires vcpu_bytes() and ring_bytes + RING_EXTRA writable bytes.
 * Failure after hardware access is sticky; retain storage even after fini.
 */
int rdn_uvd_init_mem(struct rdn_uvd *uvd, struct rdn_accel *accel,
		     uint32_t ref_freq, const uint8_t *fw, uint32_t fw_size,
		     uint32_t handles, volatile uint8_t *vcpu_cpu, uint64_t vcpu_gpu,
		     volatile uint8_t *ring_cpu, uint64_t ring_gpu,
		     uint32_t ring_bytes, uint32_t flags);
int rdn_uvd_init(struct rdn_uvd *uvd, struct rdn_accel *accel,
		 uint32_t ref_freq, const uint8_t *fw, uint32_t fw_size,
		 uint32_t handles, uint32_t vcpu_offset, uint32_t ring_offset,
		 uint32_t ring_bytes, uint32_t flags);
/* Diagnostic only: caller drains graphics and serializes all engine access.
 * Requires the exact BTC_rlc image and an initially disabled RLC. RLC remains
 * enabled after UVD failure/fini; restoring its prior state is not proven safe.
 */
int rdn_uvd_init_rlc(struct rdn_uvd *uvd, struct rdn_accel *accel,
		     uint32_t ref_freq, const uint8_t *fw, uint32_t fw_size,
		     uint32_t handles, uint32_t vcpu_offset, uint32_t ring_offset,
		     uint32_t ring_bytes, uint32_t flags,
		     const uint8_t *rlc_fw, uint32_t rlc_size);
/* Split cold startup: prepare windows/RLC, initialize CP, then start VCPU.
 * Keep accel and its VRAM mapping stable across both calls. Prepared storage
 * remains quarantined; fini cancels preparation without accessing UVD MMIO.
 */
int rdn_uvd_prepare_rlc(struct rdn_uvd *uvd, struct rdn_accel *accel,
		     uint32_t ref_freq, const uint8_t *fw, uint32_t fw_size,
		     uint32_t handles, uint32_t vcpu_offset, uint32_t ring_offset,
		     uint32_t ring_bytes, uint32_t flags,
		     const uint8_t *rlc_fw, uint32_t rlc_size);
int rdn_uvd_start_prepared(struct rdn_uvd *uvd);
void rdn_uvd_fini(struct rdn_uvd *uvd);
/* True only before hardware access; reset does not prove DMA quiescence. */
bool rdn_uvd_storage_releasable(const struct rdn_uvd *uvd);

/*
 * Run an indirect buffer of `words` words (host order) at GPU address
 * `gpu_addr`, in the same 4 GB segment as the ring, followed by a fence.
 * Caller wakes the engine first. `*seq` is the fence to wait for.
 */
int rdn_uvd_ib_submit(struct rdn_uvd *uvd, uint64_t gpu_addr, uint32_t words,
		      uint32_t *seq);
/* No ring-space wait. EBUSY leaves ring and fence state unchanged. */
int rdn_uvd_ib_try_submit(struct rdn_uvd *uvd, uint64_t gpu_addr, uint32_t words,
                          uint32_t *seq);
bool rdn_uvd_fence_done(struct rdn_uvd *uvd, uint32_t seq);
/* A zero timeout polls; a positive timeout failure poisons the engine. */
int rdn_uvd_fence_wait(struct rdn_uvd *uvd, uint32_t seq, uint32_t timeout_ms);

/* The clocks down (nothing in flight) and up again. */
int rdn_uvd_idle(struct rdn_uvd *uvd);
int rdn_uvd_wake(struct rdn_uvd *uvd);

#endif /* RDN_UVD_H */
