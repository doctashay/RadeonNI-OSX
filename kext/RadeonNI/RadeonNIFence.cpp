/*
 * Fence completion interrupts for Evergreen and Cayman.
 * Register programming follows r600_irq_init in Linux.
 *
 * Copyright 2008 Advanced Micro Devices, Inc.
 * Copyright 2008 Red Hat Inc.
 * Copyright 2009 Jerome Glisse.
 * Copyright 2010 Advanced Micro Devices, Inc.
 * Copyright (c) 2026 kouta-kun and Claude
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
 * Authors: Dave Airlie
 *          Alex Deucher
 *          Jerome Glisse
 */
#include "RadeonNI.h"
#include "RadeonNIAccel.h"
#include <IOKit/IOLib.h>
#include <kern/clock.h>

#define IH_RB_CNTL 0x3e00
#define IH_RB_BASE 0x3e04
#define IH_RB_RPTR 0x3e08
#define IH_RB_WPTR 0x3e0c
#define IH_CNTL 0x3e18
#define INTERRUPT_CNTL 0x5468
#define INTERRUPT_CNTL2 0x546c
#define CP_INT_CNTL 0xc124
#define IH_BYTES 65536u

bool RadeonNIAccel::fenceInterruptFilter(OSObject *owner, IOFilterInterruptEventSource *)
{
	RadeonNIAccel *self = (RadeonNIAccel *)owner;
	return self->fFenceIRQEnabled &&
		(rdn_rreg(self->fAccel.card, IH_RB_WPTR) != self->fFenceRptr);
}

void RadeonNIAccel::fenceInterruptAction(OSObject *owner, IOInterruptEventSource *, int)
{
	RadeonNIAccel *self = (RadeonNIAccel *)owner;
	IOLockLock(self->fLock);
	if (self->fFenceIRQEnabled) {
		struct rdn_card *card = self->fAccel.card;
		/* Publish progress while draining, then check arrivals during the drain. */
		for (unsigned pass = 0; pass < 4; ++pass) {
			UInt32 wptr = rdn_rreg(card, IH_RB_WPTR);
			if (wptr & 1) {
				self->fFenceRptr = ((wptr & ~15u) + 16) & (IH_BYTES - 1);
				rdn_wreg(card, IH_RB_CNTL, rdn_rreg(card, IH_RB_CNTL) | (1u << 31));
				self->setProperty("FenceIRQOverflow", true);
			}
			wptr &= IH_BYTES - 16;
			if (self->fFenceRptr == wptr) break;
			/* IH records are little-endian, including on PowerPC. */
			__asm__ __volatile__("sync" : : : "memory");
			while (self->fFenceRptr != wptr) {
				UInt32 source = rdn_vram_read32(&self->fAccel,
					self->fFenceRingOffset + self->fFenceRptr) & 255;
				if (source == 176 || source == 181) self->fFenceEvents++;
				self->fFenceRptr = (self->fFenceRptr + 16) & (IH_BYTES - 1);
				rdn_wreg(card, IH_RB_RPTR, self->fFenceRptr);
			}
		}
		IOLockWakeup(self->fLock, &self->fFenceEvents, false);
	}
	IOLockUnlock(self->fLock);
}

IOReturn RadeonNIAccel::startFenceInterrupts()
{
	if (fFenceIRQEnabled) return kIOReturnSuccess;
	if (!fEngineUp || fRetiring || !fFramebuffer || !fFramebuffer->device())
		return kIOReturnNotReady;
	if (!rdn_family_caps(fAccel.card->family)->rlc_words)
		return kIOReturnUnsupported;
	if (!fFenceRlcPrepared || !fGartDummy) return kIOReturnNotReady;
	IOByteCount dummyLength = 0;
	IOPhysicalAddress dummyBus = fGartDummy->getPhysicalSegment(0, &dummyLength);
	if (!dummyBus || dummyLength < 256 || (dummyBus & 255)) return kIOReturnNotReady;
	/* Configure only an unowned IH block. */
	if ((rdn_rreg(fAccel.card, IH_CNTL) | rdn_rreg(fAccel.card, IH_RB_CNTL)) & 1)
		return kIOReturnBusy;
	if (!fFenceLoop) fFenceLoop = IOWorkLoop::workLoop();
	if (!fFenceLoop) return kIOReturnNoMemory;
	if (!fFenceSource) {
		fFenceSource = IOFilterInterruptEventSource::filterInterruptEventSource(this,
			fenceInterruptAction, fenceInterruptFilter, fFramebuffer->device(), 0);
		if (!fFenceSource) return kIOReturnUnsupported;
		IOReturn result = fFenceLoop->addEventSource(fFenceSource);
		if (result != kIOReturnSuccess) {
			fFenceSource->release(); fFenceSource = 0;
			return result;
		}
	}
	IOLockLock(fLock);
	/* Configure interrupts only after every previous fence has retired. */
	if (!rdn_fence_done(&fAccel, fAccel.fence_emitted)) {
		IOLockUnlock(fLock); return kIOReturnBusy;
	}
	if (!fFenceRingOffset) {
		uint64_t offset;
		if (rdn_mem_alloc(&fMem, IH_BYTES + PAGE_SIZE, IH_BYTES, &offset)) {
			IOLockUnlock(fLock); return kIOReturnNoMemory;
		}
		fFenceRingOffset = (UInt32)offset;
	}
	for (UInt32 i = 0; i < IH_BYTES; i += 4)
		rdn_vram_write32(&fAccel, fFenceRingOffset + i, 0);
	/* Turks retains register fences; enabling IRQs must not change completion storage. */
	__asm__ __volatile__("sync" : : : "memory");
	struct rdn_card *card = fAccel.card;
	fFenceSavedControl = rdn_rreg(card, INTERRUPT_CNTL);
	fFenceSavedCPControl = rdn_rreg(card, CP_INT_CNTL);
	fFenceSavedDummy = rdn_rreg(card, INTERRUPT_CNTL2);
	/* Legacy INTx uses a DMA read to order interrupt delivery. */
	rdn_wreg(card, INTERRUPT_CNTL2, (UInt32)(dummyBus >> 8));
	rdn_wreg(card, INTERRUPT_CNTL, (fFenceSavedControl | 8u) & ~1u);
	rdn_wreg(card, IH_RB_BASE, (UInt32)(rdn_vram_addr(&fAccel, fFenceRingOffset) >> 8));
	rdn_wreg(card, IH_RB_CNTL, (1u << 16) | (1u << 31) | (14u << 1));
	rdn_wreg(card, IH_RB_RPTR, 0);
	rdn_wreg(card, IH_RB_WPTR, 0);
	fFenceRptr = 0;
	/* Legacy INTx: RPTR_REARM is for MSI only. */
	rdn_wreg(card, IH_CNTL, (16u << 15) | (16u << 20));
	fFenceIRQEnabled = true;
	fFenceSource->enable();
	rdn_wreg(card, CP_INT_CNTL, (1u << 19) | (1u << 20) |
		(fAccel.fence_memory ? 1u << 26 : 1u << 31));
	rdn_wreg(card, IH_CNTL, (16u << 15) | (16u << 20) | 1u);
	rdn_wreg(card, IH_RB_CNTL, (1u << 16) | (14u << 1) | 1u);
	fAccel.fence_interrupt = true;
	setProperty("FenceInterrupts", true);
	IOLockUnlock(fLock);
	return kIOReturnSuccess;
}

void RadeonNIAccel::stopFenceInterrupts()
{
	if (fFenceControlLock) IOLockLock(fFenceControlLock);
	stopFenceInterruptsUnlocked();
	if (fFenceControlLock) IOLockUnlock(fFenceControlLock);
}

void RadeonNIAccel::stopFenceInterruptsUnlocked()
{
	if (fLock) IOLockLock(fLock);
	if (fFenceIRQEnabled) {
		fAccel.fence_interrupt = false;
		rdn_wreg(fAccel.card, CP_INT_CNTL, fFenceSavedCPControl);
		rdn_wreg(fAccel.card, IH_RB_CNTL, rdn_rreg(fAccel.card, IH_RB_CNTL) & ~1u);
		rdn_wreg(fAccel.card, IH_CNTL, rdn_rreg(fAccel.card, IH_CNTL) & ~1u);
		fFenceIRQEnabled = false;
		rdn_wreg(fAccel.card, INTERRUPT_CNTL, fFenceSavedControl);
		rdn_wreg(fAccel.card, INTERRUPT_CNTL2, fFenceSavedDummy);
		IOLockWakeup(fLock, &fFenceEvents, false);
		setProperty("FenceInterrupts", false);
	}
	if (fLock) IOLockUnlock(fLock);
	if (fFenceSource) {
		fFenceSource->disable();
		fFenceLoop->removeEventSource(fFenceSource);
		fFenceSource->release(); fFenceSource = 0;
	}
	if (fFenceLoop) { fFenceLoop->release(); fFenceLoop = 0; }
	/* The ring remains reserved until device teardown, including late DMA. */
}

IOReturn RadeonNIAccel::fenceControl(UInt32 mode, UInt32 *active,
                                     UInt32 *events, UInt32 *fallbacks)
{
	IOReturn result = kIOReturnSuccess;
	if (mode == 1) {
		IOLockLock(fFenceControlLock);
		if (!waitForCPU()) result = startFenceInterrupts();
		else result = kIOReturnBusy;
		IOLockUnlock(fFenceControlLock);
	} else if (mode == 2) stopFenceInterrupts();
	else if (mode != 0) result = kIOReturnBadArgument;
	IOLockLock(fLock);
	*active = (fFenceIRQEnabled ? 1u : 0u) | 2u |
		(fSurfaceGpuStalled ? 4u : 0u); *events = fFenceEvents; *fallbacks = fFenceFallbacks;
	IOLockUnlock(fLock);
	return result;
}
