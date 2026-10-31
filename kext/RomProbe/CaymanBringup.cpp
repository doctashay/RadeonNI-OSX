/* Secondary Cayman validation; does not register a framebuffer or accelerator. */
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/OSByteOrder.h>
#include <kern/clock.h>
extern "C" {
#include "rdn_accel.h"
#include "rdn_mc.h"
#include "rdn_pm.h"
#include "rdn_gart.h"
#include "rdn_mode.h"
#include "rdn_i2c.h"
#include "rdn_accel_reg.h"
}

struct ProbeContext { IOPCIDevice *dev; volatile UInt8 *regs; };
static uint32_t read_reg(void *cookie, uint32_t reg)
{
	return OSReadLittleInt32(((ProbeContext *)cookie)->regs, reg);
}
static void write_reg(void *cookie, uint32_t reg, uint32_t value)
{
	__asm__ __volatile__("sync" : : : "memory");
	OSWriteLittleInt32(((ProbeContext *)cookie)->regs, reg, value);
}
static uint32_t read_pci(void *cookie, uint32_t reg)
{
	return ((ProbeContext *)cookie)->dev->configRead32(reg);
}
static void write_pci(void *cookie, uint32_t reg, uint32_t value)
{
	((ProbeContext *)cookie)->dev->configWrite32(reg, value);
}
static void delay_us(void *, uint32_t us) { IODelay(us); }
static uint64_t time_ms(void *)
{
	uint64_t time, ns;
	clock_get_uptime(&time);
	absolutetime_to_nanoseconds(time, &ns);
	return ns / 1000000;
}
static void *alloc_mem(void *, size_t size)
{
	UInt32 *p = (UInt32 *)IOMalloc(size + 8);
	if (!p) return 0;
	bzero(p, size + 8);
	p[0] = size + 8;
	return p + 2;
}
static void free_mem(void *, void *ptr)
{
	if (ptr) { UInt32 *p = (UInt32 *)ptr - 2; IOFree(p, p[0]); }
}
static void log_msg(void *, enum rdn_log_level level, const char *fmt, va_list ap)
{
	char text[256];
	if (level == RDN_LOG_DEBUG) return;
	vsnprintf(text, sizeof(text), fmt, ap);
	IOLog("CaymanProbe: %s\n", text);
}

extern "C" int rdn_probe_bringup(IOService *owner, IOPCIDevice *dev, void *bios)
{
	ProbeContext context;
	struct rdn_os os;
	struct rdn_card card;
	struct rdn_accel accel;
	struct rdn_accel_fw fw;
	struct rdn_pm_state boot, performance;
	uint32_t sclk = 0, mclk = 0;
	struct rdn_gart gart;
	IOBufferMemoryDescriptor *dummy = 0, *test = 0;
	IOByteCount segmentLength = 0;
	IOPhysicalAddress dummyBus = 0, testBus = 0;
	struct rdn_draw_surface surface, source;
	struct rdn_blit_rect copy_rect;
	struct rdn_fill_rect rect;
	IOMemoryMap *regs = 0, *aperture = 0;
	IODeviceMemory *window = 0, *memory = 0;
	OSData *pfp = OSDynamicCast(OSData, owner->getProperty("FW_PFP"));
	OSData *me = OSDynamicCast(OSData, owner->getProperty("FW_ME"));
	OSData *mc = OSDynamicCast(OSData, owner->getProperty("FW_MC"));
	bool bound = false, started = false;
	uint32_t seq = 0, mismatches = 0;
	uint32_t stats[22];
	uint64_t stats_addr = 0;
	int r = -22;
	bzero(&os, sizeof(os));
	bzero(&accel, sizeof(accel));
	bzero(&gart, sizeof(gart));
	if (!pfp || !me || !mc || dev->configRead32(0) != 0x67041002)
		return r;
	regs = dev->mapDeviceMemoryWithRegister(0x18);
	memory = dev->getDeviceMemoryWithRegister(0x10);
	if (!regs || !memory || memory->getLength() < 32 * 1024 * 1024)
		goto out;
	window = IODeviceMemory::withRange(memory->getPhysicalAddress(), 32 * 1024 * 1024);
	if (!window || !(aperture = window->map()))
		goto out;
	context.dev = dev;
	context.regs = (volatile UInt8 *)regs->getVirtualAddress();
	os.cookie = &context;
	os.mmio_read32 = read_reg;
	os.mmio_write32 = write_reg;
	os.cfg_read32 = read_pci;
	os.cfg_write32 = write_pci;
	os.delay_us = delay_us;
	os.time_ms = time_ms;
	os.alloc = alloc_mem;
	os.free = free_mem;
	os.log = log_msg;
	owner->setProperty("BringupStage", 1, 32);
	r = rdn_card_init(&card, &os, bios);
	if (r) goto out;
	bound = true;
	owner->setProperty("BringupStage", 2, 32);
	r = rdn_card_post(&card);
	if (r) goto out;
	owner->setProperty("BringupStage", 3, 32);

	r = rdn_mc_load_microcode(&card, mc->getBytesNoCopy(), mc->getLength());
	owner->setProperty("MemoryFirmwareResult", (UInt64)(SInt64)r, 32);
	owner->setProperty("VideoMemoryMB", rdn_rreg(&card, 0x5428), 32);
	if (r < 0) goto out;
	r = rdn_pm_boot_state(&card, &boot);
	if (!r) r = rdn_pm_performance_state(&card, &performance);
	if (!r) r = rdn_pm_set(&card, &boot, &performance, RDN_PM_ALL);
	owner->setProperty("PowerResult", (UInt64)(SInt64)r, 32);
	if (r) goto out;
	rdn_pm_get_clocks(&card, &sclk, &mclk);
	owner->setProperty("EngineClock10KHz", sclk, 32);
	owner->setProperty("MemoryClock10KHz", mclk, 32);
	fw.pfp = (const uint8_t *)pfp->getBytesNoCopy(); fw.pfp_size = pfp->getLength();
	fw.me = (const uint8_t *)me->getBytesNoCopy(); fw.me_size = me->getLength();
	owner->setProperty("BringupStage", 4, 32);
	started = true;
	r = rdn_accel_init(&accel, &card, (volatile void *)aperture->getVirtualAddress(),
		32 * 1024 * 1024, &fw, 0x100000, 65536, true);
	owner->setProperty("RingResult", (UInt64)(SInt64)r, 32);
	if (r) goto out;
	owner->setProperty("BringupStage", 5, 32);
	r = rdn_ib_selftest(&accel, rdn_vram_addr(&accel, 0x200000),
		(uint32_t *)(aperture->getVirtualAddress() + 0x200000));
	owner->setProperty("IBResult", (UInt64)(SInt64)r, 32);
	if (r) goto out;
	dummy = IOBufferMemoryDescriptor::withOptions(kIOMemoryPhysicallyContiguous, 4096, 4096);
	test = IOBufferMemoryDescriptor::withOptions(kIOMemoryPhysicallyContiguous, 4096, 4096);
	if (!dummy || !test || dummy->prepare() || test->prepare()) { r = -12; goto out; }
	bzero(dummy->getBytesNoCopy(), 4096);
	bzero(test->getBytesNoCopy(), 4096);
	dummyBus = dummy->getPhysicalSegment(0, &segmentLength);
	testBus = test->getPhysicalSegment(0, &segmentLength);
	if (!dummyBus || !testBus || segmentLength < 4096) { r = -12; goto out; }
	dev->setBusMasterEnable(true);
	r = rdn_gart_enable(&gart, &accel, 0x800000, dummyBus);
	owner->setProperty("GartEnableResult", (UInt64)(SInt64)r, 32);
	if (r) goto out;
	rdn_gart_set_page(&gart, 0, testBus);
	r = rdn_gart_flush_checked(&gart);
	if (!r) r = rdn_ib_selftest(&accel, rdn_gart_addr(&accel, 0), (uint32_t *)test->getBytesNoCopy());
	owner->setProperty("GartIBResult", (UInt64)(SInt64)r, 32);
	if (r) goto out;
	owner->setProperty("BringupStage", 6, 32);
	surface.gpu_addr = rdn_vram_addr(&accel, 0x400000);
	surface.width = surface.height = surface.pitch_pixels = 64;
	rect.x = rect.y = 0; rect.width = rect.height = 64;
	for (uint32_t i = 0; i < 4096; i++)
		rdn_vram_write32(&accel, 0x400000 + 4 * i, 0);
	stats_addr = rdn_vram_addr(&accel, 0xc00000);
	for (uint32_t i = 0; i < 22; i++) rdn_vram_write32(&accel, 0xc00000 + i * 4, 0xabababab);
	rdn_ring_begin(&accel, 2);
	rdn_ring_emit(&accel, PACKET3(PACKET3_EVENT_WRITE, 0));
	rdn_ring_emit(&accel, EVENT_TYPE(25));
	rdn_ring_commit(&accel);
	r = rdn_blit_fill(&accel, &surface, &rect, 1, 0xff214365, false, 0x600000, &seq);
	if (!r) {
		rdn_ring_begin(&accel, 4);
		rdn_ring_emit(&accel, PACKET3(PACKET3_EVENT_WRITE, 2));
		rdn_ring_emit(&accel, EVENT_TYPE(30) | EVENT_INDEX(2));
		rdn_ring_emit(&accel, (uint32_t)stats_addr);
		rdn_ring_emit(&accel, (uint32_t)(stats_addr >> 32));
		rdn_ring_commit(&accel);
		r = rdn_fence_emit(&accel, &seq);
	}
	if (!r) r = rdn_fence_wait(&accel, seq, 1000);
	if (!r) {
		for (uint32_t i = 0; i < 4096; i++)
			if (rdn_vram_read32(&accel, 0x400000 + 4 * i) != 0xff214365)
				mismatches++;
		if (mismatches) r = -5;
	}
	for (uint32_t i = 0; i < 22; i++) stats[i] = rdn_vram_read32(&accel, 0xc00000 + i * 4);
	owner->setProperty("PipelineCounters", stats, sizeof(stats));
	owner->setProperty("VSStart", rdn_rreg(&card, 0x2885c), 32);
	owner->setProperty("CBInfo", rdn_rreg(&card, 0x28c70), 32);
	owner->setProperty("SQThreads", rdn_rreg(&card, 0x8e20), 32);
	owner->setProperty("FillMismatches", mismatches, 32);
	owner->setProperty("FirstPixel", rdn_vram_read32(&accel, 0x400000), 32);
	owner->setProperty("FaultStatus", rdn_rreg(&card, 0x14d8), 32);
	owner->setProperty("FaultAddress", rdn_rreg(&card, 0x14f8), 32);
	owner->setProperty("BackendMap", accel.cfg.backend_map, 32);
	owner->setProperty("FillResult", (UInt64)(SInt64)r, 32);
	if (!r) {
		owner->setProperty("BringupStage", 7, 32);
		source = surface;
		source.gpu_addr = rdn_vram_addr(&accel, 0x500000);
		for (uint32_t y = 0; y < 64; y++) {
			for (uint32_t x = 0; x < 64; x++) {
				uint32_t v = 0xff000000 | (x << 16) | (y << 8) | (x ^ y);
				rdn_vram_write32(&accel, 0x500000 + 4 * (y * 64 + x), v);
				rdn_vram_write32(&accel, 0x400000 + 4 * (y * 64 + x), 0x19375bdf);
			}
		}
		copy_rect.src_x = 5; copy_rect.src_y = 7;
		copy_rect.dst_x = 3; copy_rect.dst_y = 2;
		copy_rect.width = 29; copy_rect.height = 31;
		r = rdn_blit(&accel, &surface, &source, &copy_rect, 1, 0x600000, &seq);
		if (!r) r = rdn_fence_wait(&accel, seq, 1000);
		mismatches = 0;
		if (!r) for (uint32_t y = 0; y < 64; y++) {
			for (uint32_t x = 0; x < 64; x++) {
				uint32_t expected = 0x19375bdf;
				if (x >= 3 && x < 32 && y >= 2 && y < 33) {
					uint32_t sx = x + 2, sy = y + 5;
					expected = 0xff000000 | (sx << 16) | (sy << 8) | (sx ^ sy);
				}
				if (rdn_vram_read32(&accel, 0x400000 + 4 * (y * 64 + x)) != expected)
					mismatches++;
			}
		}
		if (mismatches) r = -5;
		owner->setProperty("CopyMismatches", mismatches, 32);
		owner->setProperty("CopyResult", (UInt64)(SInt64)r, 32);
	}
	if (!r) {
		owner->setProperty("BringupStage", 8, 32);
		r = rdn_blit_fill(&accel, &surface, &rect, 1, 0xff214365, true, 0x600000, &seq);
		if (!r) r = rdn_fence_wait(&accel, seq, 1000);
		mismatches = 0;
		if (!r) for (uint32_t i = 0; i < 4096; i++) {
			volatile uint32_t *pixels = (volatile uint32_t *)(aperture->getVirtualAddress() + 0x400000);
			if (pixels[i] != 0xff214365) mismatches++;
		}
		if (mismatches) r = -5;
		owner->setProperty("BigEndianFillMismatches", mismatches, 32);
		owner->setProperty("BigEndianFillResult", (UInt64)(SInt64)r, 32);
	}
	if (!r) {
		uint8_t *bytes = (uint8_t *)test->getBytesNoCopy();
		volatile uint8_t *vram = (volatile uint8_t *)(aperture->getVirtualAddress() + 0x700000);
		for (uint32_t i = 0; i < 4096; i++) bytes[i] = (uint8_t)(i * 37 + 11);
		__asm__ __volatile__("sync" : : : "memory");
		r = rdn_cp_dma(&accel, rdn_gart_addr(&accel, 0), rdn_vram_addr(&accel, 0x700000), 4096, &seq);
		if (!r) r = rdn_fence_wait(&accel, seq, 1000);
		mismatches = 0;
		if (!r) for (uint32_t i = 0; i < 4096; i++)
			if (vram[i] != (uint8_t)(i * 37 + 11)) mismatches++;
		if (mismatches) r = -5;
		owner->setProperty("HostDmaMismatches", mismatches, 32);
		owner->setProperty("HostDmaResult", (UInt64)(SInt64)r, 32);
	}
	if (!r) {
		r = rdn_cp_dma(&accel, rdn_vram_addr(&accel, 0x700000), rdn_vram_addr(&accel, 0x710000), 4096, &seq);
		if (!r) r = rdn_fence_wait(&accel, seq, 1000);
		mismatches = 0;
		volatile uint8_t *vram = (volatile uint8_t *)(aperture->getVirtualAddress() + 0x710000);
		if (!r) for (uint32_t i = 0; i < 4096; i++)
			if (vram[i] != (uint8_t)(i * 37 + 11)) mismatches++;
		if (mismatches) r = -5;
		owner->setProperty("VramDmaMismatches", mismatches, 32);
		owner->setProperty("VramDmaResult", (UInt64)(SInt64)r, 32);
	}
	if (!r) {
		for (uint32_t i = 0; i < 2048 && !r; i++) {
			r = rdn_fence_emit(&accel, &seq);
			if (!r && !(i & 31)) r = rdn_fence_wait(&accel, seq, 1000);
		}
		if (!r) r = rdn_fence_wait(&accel, seq, 1000);
		owner->setProperty("RingWrapResult", (UInt64)(SInt64)r, 32);
	}
	if (!r) {
		surface.gpu_addr = rdn_vram_addr(&accel, 0x1000000);
		surface.width = surface.pitch_pixels = 2048;
		surface.height = 1536;
		rect.width = surface.width; rect.height = surface.height;
		r = rdn_blit_fill(&accel, &surface, &rect, 1, 0xff7193b5, false, 0x600000, &seq);
		if (!r) r = rdn_fence_wait(&accel, seq, 1000);
		mismatches = 0;
		if (!r) for (uint32_t y = 0; y < surface.height; y++)
			for (uint32_t x = 0; x < surface.width; x++)
				if (rdn_vram_read32(&accel, 0x1000000 + 4 * (y * surface.pitch_pixels + x)) != 0xff7193b5)
					mismatches++;
		if (mismatches) r = -5;
		owner->setProperty("LargeFillMismatches", mismatches, 32);
		owner->setProperty("LargeFillResult", (UInt64)(SInt64)r, 32);
	}
	if (!r) {
		uint8_t edid[RDN_EDID_MAX_SIZE];
		rdn_output_assume(&card);
		r = rdn_display_init(&card);
		owner->setProperty("DisplayInitResult", (UInt64)(SInt64)r, 32);
		owner->setProperty("BoardOutputs", card.num_board, 32);
		if (!r) {
			int detect = rdn_output_detect(&card, edid);
			owner->setProperty("DisplayDetectResult", (UInt64)(SInt64)detect, 32);
			if (detect > 0) {
				struct rdn_mode mode;
				struct rdn_fb fb;
				const uint32_t colors[8] = { 0xffffffff, 0xffffff00, 0xff00ffff, 0xff00ff00,
					0xffff00ff, 0xffff0000, 0xff0000ff, 0xff000000 };
				owner->setProperty("DetectedEDID", edid, detect);
				if (!rdn_edid_preferred_mode(edid, &mode)) r = -22;
				for (int i = 1; i < 4 && !r; i++) {
					struct rdn_mode candidate;
					if (rdn_edid_detailed_mode(edid, i, &candidate) &&
					    candidate.hdisplay * candidate.vdisplay > mode.hdisplay * mode.vdisplay)
						mode = candidate;
				}
				if (!r) {
					bzero(&fb, sizeof(fb));
					fb.width = mode.hdisplay; fb.height = mode.vdisplay;
					fb.pitch_pixels = (fb.width + 63) & ~63u;
					fb.aperture_offset = 0x1000000; fb.bpp = 32; fb.big_endian_pixels = true;
					if (fb.pitch_pixels * fb.height * 4 > 16 * 1024 * 1024) r = -22;
					if (!r) {
						surface.gpu_addr = rdn_vram_addr(&accel, fb.aperture_offset);
						surface.width = fb.width; surface.height = fb.height; surface.pitch_pixels = fb.pitch_pixels;
						for (uint32_t i = 0; i < 8 && !r; i++) {
							rect.x = fb.width * i / 8; rect.y = 0;
							rect.width = fb.width * (i + 1) / 8 - rect.x; rect.height = fb.height;
							r = rdn_blit_fill(&accel, &surface, &rect, 1, colors[i], true, 0x600000, &seq);
							if (!r) r = rdn_fence_wait(&accel, seq, 1000);
						}
						if (!r) r = rdn_modeset(&card, &mode, &fb, false);
					}
					owner->setProperty("ScanoutWidth", fb.width, 32);
					owner->setProperty("ScanoutHeight", fb.height, 32);
					owner->setProperty("ScanoutResult", (UInt64)(SInt64)r, 32);
				}
			}
		}
	}
	if (!r) owner->setProperty("BringupStage", 9, 32);
out:
	if (started) rdn_accel_fini(&accel);
	if (gart.ready) rdn_gart_disable(&gart);
	if (!r) {
		if (dummy) { dummy->complete(); dummy->release(); }
		if (test) { test->complete(); test->release(); }
	} else if (dummy || test) {
		owner->setProperty("HostPagesQuarantined", true);
	}
	if (bound) rdn_card_fini(&card);
	if (aperture) aperture->release();
	if (window) window->release();
	if (regs) regs->release();
	IOLog("CaymanProbe: bring-up returned %d\n", r);
	return r;
}
