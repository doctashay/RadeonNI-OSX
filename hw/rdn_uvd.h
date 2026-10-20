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

/*
 * Start-up variants for finding out what a card needs (rdn_uvd_init's
 * `flags`); 0 is Linux's sequence for Turks.
 */
#define RDN_UVD_F_UDEC_CONFIG	(1 << 0)	/* (unused: the tiling registers are always written) */
#define RDN_UVD_F_PLL_FIRST	(1 << 1)	/* raise the clocks before programming the memory window (Linux does it the other way) */
#define RDN_UVD_F_V1_MEM	(1 << 2)	/* uvd_v1_0_resume()'s window: +16, UVD_FW_START */
#define RDN_UVD_F_LOW_CLOCKS	(1 << 3)	/* 100/100 MHz, as for pre-RV740 */
#define RDN_UVD_F_NO_PLL	(1 << 4)	/* leave the PLL in bypass: the VCPU runs on the base clock */
#define RDN_UVD_F_KEEP_CLOCKS	(1 << 5)	/* on failure leave the clocks up (registers can then be read) */
#define RDN_UVD_F_ADDR_OFFSET	(1 << 6)	/* give the block video memory offsets, not GPU addresses (bits 32-39 zero) */
#define RDN_UVD_F_PM_GLOBAL	(1 << 7)	/* GENERAL_PWRMGT |= GLOBAL_PWRMGT_EN | STATIC_PM_EN, as in Linux's trace; restored on failure */
#define RDN_UVD_F_PM_SCLK	(1 << 8)	/* SCLK_PWRMGT_CNTL |= DYN_GFX_CLK_OFF_EN | DYN_LIGHT_SLEEP_EN, as in Linux's trace; restored on failure */
#define RDN_UVD_F_MINIMAL	(1 << 10)	/* only the register writes a bisection of radeon showed necessary; the caller moves VRAM to MC address 0 and starts the RLC first */
#define RDN_UVD_F_LMI		(1 << 11)	/* with MINIMAL: also the LMI control, swap and MPC mux registers */
#define RDN_UVD_F_EXT		(1 << 12)	/* with MINIMAL: also the address extension registers (bits 28-39 of the window) */
#define RDN_UVD_F_SWAP4		(1 << 13)	/* test: copy the firmware with the bytes of each 32-bit word reversed (addr ^ 3) */
#define RDN_UVD_F_SWAP2		(1 << 14)	/* test: copy the firmware with the 16-bit halves of each word exchanged (addr ^ 2) */
#define RDN_UVD_F_BLANK		(1 << 15)	/* test (kext): the display controllers stop reading video memory while UVD starts */
#define RDN_UVD_F_PLL		(1 << 16)	/* with MINIMAL: the clocks go up (Linux's 533/400 MHz) at the first command stream, rdn_uvd_wake(), and down at rdn_uvd_idle(); without it the block runs on the bypass clock */
#define RDN_UVD_F_WINDOWS	(1 << 17)	/* with MINIMAL: also the heap and the stack/session windows of the VCPU's memory (uvd_v2_2_resume() writes all three); without them the firmware's heap and stack are at video memory address 0 */
#define RDN_UVD_F_SEGMENT	(1 << 18)	/* (kext) the block in a 256 MB segment beyond the aperture (docs/UVD.md); alone it means the default start with it */
#define RDN_UVD_F_SEGMENT_AT	20		/* (kext) with SEGMENT: bits 20 to 22 name the segment to use, 1 to 6 (hidden) or 7 (the aperture's own, through the register window: to test the plumbing); 0 picks */
#define RDN_UVD_F_NO_DMA		(1u << 29)	/* (kext, tests) with SEGMENT: big regions go through the register window, not the command processor's DMA */
#define RDN_UVD_F_NO_FLUSH	(1u << 30)	/* (kext, tests) with SEGMENT: no dummy write and HDP flush before the VCPU is released */
#define RDN_UVD_F_PUSH_APERTURE	(1u << 28)	/* (kext, tests) with SEGMENT: the regions that lie in the aperture are written through it */
#define RDN_UVD_F_APERTURE	(1 << 19)	/* (kext) a started block that idles is started again in the aperture (tests: to compare the two) */
#define RDN_UVD_F_GART		(1 << 9)	/* (kext) the VCPU region and ring in GART memory: addresses with bits 32-39 zero */

struct rdn_uvd {
	struct rdn_accel *accel;
	uint32_t ref_freq;	/* the PLL's reference clock, 10 kHz units */
	uint32_t handles;	/* sessions the firmware may hold */
	uint32_t fw_size;
	uint32_t flags;

	/*
	 * Where things are: the CPU's view and the block's address. In video
	 * memory (rdn_uvd_init) the CPU view is the aperture and the offsets
	 * are in it; rdn_uvd_init_mem() takes any memory the GPU can address.
	 */
	volatile uint8_t *vcpu_cpu, *ring_cpu;
	uint64_t vcpu_gpu, ring_gpu;
	bool in_vram;
	/*
	 * Video memory the CPU has no mapping of (beyond the aperture): vcpu_cpu and ring_cpu are copies in
	 * system memory, kept in step through the register window (rdn_uvd_init_window()).
	 */
	bool windowed;
	uint32_t ring_mark;	/* ring_begin(): where the words of this submission start */
	uint32_t vcpu_offset;	/* firmware, heap, stack, sessions (video memory only) */
	uint32_t vcpu_bytes;
	uint32_t ring_offset;	/* (video memory only) */
	uint32_t ring_words;
	uint32_t pm_general, pm_sclk;	/* registers a power-management variant changed */
	uint32_t wptr;
	uint32_t fence_emitted;
	bool swapped;
	bool ready;
};

/*
 * Bytes of video memory the engine's own use needs for a firmware of
 * `fw_size` bytes (page aligned; the firmware is copied to its start).
 * `vcpu_offset` of rdn_uvd_init() is a multiple of 4096.
 */
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
 * Copy the firmware (as in the file), raise the clocks, program the
 * memory window, boot the VCPU, test the ring, lower the clocks again.
 * `flags`: RDN_UVD_F_*. On failure the registers are logged (with the
 * clocks still up) before the block is stopped.
 * `accel` must have the aperture mapped (rdn_accel_init() does not have to
 * have been called). All three regions must lie in one 4 GB segment of
 * video memory and `vcpu_offset` within the first 256 MB. Returns 0 or
 * -errno; -EIO if the VCPU does not answer.
 */
/*
 * The same with memory of the caller's: `vcpu_cpu`/`vcpu_gpu` is
 * rdn_uvd_vcpu_bytes() bytes (page aligned), `ring_cpu`/`ring_gpu`
 * `ring_bytes` + RDN_UVD_RING_EXTRA, both zero-filled by this function.
 * The two GPU addresses must share bits 32-39 and be 4096-aligned. IB
 * submission needs its buffers in the same 4 GB segment.
 */
int rdn_uvd_init_mem(struct rdn_uvd *uvd, struct rdn_accel *accel,
		     uint32_t ref_freq, const uint8_t *fw, uint32_t fw_size,
		     uint32_t handles, volatile uint8_t *vcpu_cpu, uint64_t vcpu_gpu,
		     volatile uint8_t *ring_cpu, uint64_t ring_gpu,
		     uint32_t ring_bytes, uint32_t flags);
/*
 * The same with the two regions in video memory the CPU cannot map, at `vcpu_offset` and `ring_offset` (4096-aligned, in
 * one 256 MB segment of video memory beyond the aperture). `vcpu_shadow` and `ring_shadow` are memory of the
 * caller's of the same sizes, which stay valid until rdn_uvd_fini(); the library keeps them in step with video memory
 * (through rdn_vram_put and rdn_vram_get). The decoder's address extension registers are programmed for the segment
 * (RDN_UVD_F_EXT is added).
 */
int rdn_uvd_init_window(struct rdn_uvd *uvd, struct rdn_accel *accel,
			uint32_t ref_freq, const uint8_t *fw, uint32_t fw_size,
			uint32_t handles, volatile uint8_t *vcpu_shadow, uint32_t vcpu_offset,
			volatile uint8_t *ring_shadow, uint32_t ring_offset,
			uint32_t ring_bytes, uint32_t flags);

/*
 * Video memory through the register window (MM_INDEX with its top bit set, MM_DATA): any byte of it, at a few MB a
 * second. `offset` counts from the start of video memory and stays below 2 GB. The window is one register pair of
 * the card: the caller serialises it with every other use of the index/data pair (the library does, as it is not
 * thread-safe).
 */
int rdn_vram_put(struct rdn_card *card, uint32_t offset, const void *src, uint32_t bytes);
int rdn_vram_get(struct rdn_card *card, uint32_t offset, void *dst, uint32_t bytes);

/*
 * Regions of 4 KB or more of the windowed block's memory (the firmware window, the ring at the start) are written with `hook` instead of
 * the register window, if one is set: the window's writes do not reach the decoder the way the CPU's through the aperture and the GPU's do
 * (a restart of a block whose firmware window was rewritten through the register window hangs it; docs/UVD.md).
 */
void rdn_uvd_set_put_hook(int (*hook)(void *cookie, uint32_t offset, const void *src, uint32_t bytes), void *cookie);

/* Test (rdn_uvd.c): before the VCPU is released, write 4 KB of zeros at this aperture offset and flush the HDP. */
void rdn_uvd_set_coherency_page(bool on, uint32_t aperture_offset);

/* Test: write the windowed regions through the aperture's mapping where they lie in it (rdn_uvd.c). */
void rdn_uvd_set_push_via_aperture(bool on);

/* Which nibbles of UVD_LMI_ADDR_EXT get the segment (rdn_uvd.c): bit i of `mask` for bits 4i..4i+3; the default is all four. For tests. */
void rdn_uvd_set_ext_nibbles(unsigned mask);

/* Log the registers that tell how the block is doing (the clocks must be up). */
void rdn_uvd_dump(struct rdn_uvd *uvd, const char *why);

int rdn_uvd_init(struct rdn_uvd *uvd, struct rdn_accel *accel,
		 uint32_t ref_freq, const uint8_t *fw, uint32_t fw_size,
		 uint32_t handles, uint32_t vcpu_offset, uint32_t ring_offset,
		 uint32_t ring_bytes, uint32_t flags);
void rdn_uvd_fini(struct rdn_uvd *uvd);

/*
 * Run an indirect buffer of `words` words (host order) at GPU address
 * `gpu_addr`, in the same 4 GB segment as the ring, followed by a fence.
 * Raises the clocks if they were down. `*seq` is the fence to wait for.
 */
int rdn_uvd_ib_submit(struct rdn_uvd *uvd, uint64_t gpu_addr, uint32_t words,
		      uint32_t *seq);
bool rdn_uvd_fence_done(struct rdn_uvd *uvd, uint32_t seq);
/*
 * Right after rdn_uvd_init() of a block that was used before: continue the fence numbers from `base` (the last one the
 * old start emitted), so that a program that has seen fence `base` reached does not take a newer one for reached.
 */
void rdn_uvd_set_fence_base(struct rdn_uvd *uvd, uint32_t base);
int rdn_uvd_fence_wait(struct rdn_uvd *uvd, uint32_t seq, uint32_t timeout_ms);

/* The clocks down (nothing in flight) and up again. */
int rdn_uvd_idle(struct rdn_uvd *uvd);
int rdn_uvd_wake(struct rdn_uvd *uvd);

/* The RLC microcode (BTC_rlc.bin, 3072 bytes) the minimal start loads before enabling the RLC; NULL for none. */
void rdn_uvd_set_rlc_ucode(const void *fw, uint32_t size);

/*
 * How the next stop and start of the block are done (a global, like the RLC microcode: set it before rdn_uvd_fini() and rdn_uvd_init(),
 * back to 0 after). Bits, 0 being the start as it was first: only the VCPU is reset, the memory interface and register bus are stalled
 * around it and the RLC is loaded again.
 */
#define RDN_UVD_RESET_SUB	1	/* also reset every sub-block (UVD_SOFT_RESET), as Linux's uvd_v1_0_start does */
#define RDN_UVD_RESET_NO_RLC	2	/* leave the RLC alone (it runs since the first start) */
#define RDN_UVD_RESET_NO_STALL	4	/* do not stall the memory interface and the register bus around the reset */
#define RDN_UVD_RESET_NO_IDLE	8	/* do not lower the clocks when the block is stopped (rdn_uvd_fini) */
#define RDN_UVD_RESET_NO_REWRITE 16	/* do not write the firmware and clear the VCPU region and ring again */
#define RDN_UVD_RESET_NO_PROGRAM 32	/* do not program the decoder's address, memory window, LMI and MPC registers again */
void rdn_uvd_set_reset_level(unsigned level);

/* For debugging: 1 reset every sub-block (with the memory controller and the register bus stalled around it), 2 reset the whole block (SRBM_SOFT_RESET). */
void rdn_uvd_debug_reset(struct rdn_uvd *uvd, unsigned what);

#endif /* RDN_UVD_H */
