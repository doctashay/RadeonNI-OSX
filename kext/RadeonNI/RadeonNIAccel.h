/*
 * The accelerator service of the Radeon HD 7570 driver, and its user
 * client.
 *
 * The service names the OpenGL driver bundle and links the framebuffer to
 * itself the way OpenGL and the window server expect. It starts the 3D
 * engine and the command processor (hw/rdn_accel.h) on the framebuffer's
 * card and owns the allocator for the video memory behind the screen.
 * User space reaches all of that through RadeonNIUserClient (hw/rdn_user.h).
 *
 * Copyright (c) 2026 kouta-kun and Claude
 * SPDX-License-Identifier: MIT
 */

#ifndef RADEONNIACCEL_H
#define RADEONNIACCEL_H

#include <IOKit/IOLocks.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/graphics/IOAccelerator.h>
#include <IOKit/pci/IOAGPDevice.h>

#include <IOKit/graphics/IOAccelSurfaceConnect.h>

extern "C" {
#include "rdn_accel.h"
#include "rdn_gart.h"
#include "rdn_mem.h"
#include "rdn_pm.h"
#include "rdn_user.h"
#include "rdn_uvd.h"
}

class RadeonNI;
class RadeonNIVideoUserClient;
struct RadeonNIVideoSession;
#include "rdn_video.h"

#define RDN_UVD_REBOOT_PROPERTY "RadeonNIUVDRebootRequired"

/*
 * What Tiger's window server wants to see before it considers compositing
 * with OpenGL (Quartz Extreme), found by reading its checks:
 *  - some registered service that is an IOAGPDevice, with a "model"
 *    property that does not start with "ATY,Rage128";
 *  - an object whose class name is exactly IOAGPDevice among the
 *    accelerator's ancestors in the registry (the stock configuration's
 *    GLCompositorRequiredClasses).
 * A PCI or PCI Express card has neither. RadeonNIAGPShim is the first: a
 * registered nub that no driver can match. The second is a bare
 * IOAGPDevice the accelerator is attached under; it is never registered,
 * so nothing ever probes or calls it.
 */
class RadeonNIAGPShim : public IOAGPDevice
{
	OSDeclareDefaultStructors(RadeonNIAGPShim)

public:
	/*
	 * Everything IOKit's generic code calls on a registered nub that
	 * IOPCIDevice would pass to its PCI bridge. The shim has none.
	 */
	virtual bool matchPropertyTable(OSDictionary *table, SInt32 *score);
	virtual bool matchPropertyTable(OSDictionary *table);
	virtual bool compareName(OSString *name, OSString **matched = 0) const;
	virtual IOReturn getResources(void);
};

class RadeonNIAccel : public IOAccelerator
{
	OSDeclareDefaultStructors(RadeonNIAccel)

public:
	/* Create, attach under the PCI device and publish. */
	static RadeonNIAccel *withFramebuffer(RadeonNI *fb, IOService *provider);
	virtual void free(void);
	void prepareToRetire(void);
	bool videoCharge(UInt32 bytes);
	void videoUncharge(UInt32 bytes);
	void videoQuery(bool administrator, struct rdn_video_query *out);
	IOReturn videoCreate(RadeonNIVideoUserClient *owner, bool administrator,
		const struct rdn_video_create *in, struct rdn_video_session *out);
	IOReturn videoDecode(RadeonNIVideoUserClient *owner,
		const struct rdn_video_decode *in, struct rdn_video_output *out);
	IOReturn videoRelease(RadeonNIVideoUserClient *owner,
		const struct rdn_video_release *in);
	IOReturn videoDestroy(RadeonNIVideoUserClient *owner,
		const struct rdn_video_destroy *in);
	void videoClose(RadeonNIVideoUserClient *owner);
	IOReturn videoMemory(RadeonNIVideoUserClient *owner, UInt32 type,
		IOOptionBits *options, IOMemoryDescriptor **memory);
	void retire(IOService *provider);

	virtual IOReturn newUserClient(task_t owningTask, void *securityID,
				       UInt32 type, IOUserClient **handler);

	/* For the user client. All of these take the lock themselves. */
	void getInfo(struct rdn_user_info *info);
	IOReturn allocVram(UInt32 size, UInt32 align, UInt32 *offset,
			   bool hidden = false);
	void hiddenInfo(UInt32 *offset, UInt32 *size);
	/* A program's memory in the GART (rdn_user.h). */
	void gartInfo(UInt32 *on, UInt32 *gpuStart, UInt32 *pages);
	IOReturn gartBind(task_t task, UInt64 address, UInt32 size, void *owner,
			  UInt32 *offset);
	IOReturn gartUnbind(UInt32 offset, void *owner);
	void gartUnbindAll(void *owner);
	void releaseClientMemory(const UInt32 *offsets, UInt32 count, void *owner);
	void freeVram(UInt32 offset);
	IOReturn submit(UInt32 offset, UInt32 words, UInt32 *fence);
	bool fenceWait(UInt32 fence, UInt32 timeoutMs);
	void syncForCPU(void);
	IOReturn waitForCPU(void);
	IOReturn power(UInt32 state, UInt32 what, UInt32 *sclk, UInt32 *mclk,
		       UInt32 *temperature);
	IOReturn regRead(UInt32 offset, UInt32 *value);
	void uvdDiagnosticInfo(struct rdn_user_uvd_diagnostic *info);
	IOReturn uvdDiagnosticInit(UInt32 *status);
	IOReturn uvdFixtureRun(struct rdn_user_uvd_fixture *result);
	IOReturn uvdFixtureRead(UInt32 generation, UInt32 chunk, void *bytes);
	IOMemoryDescriptor *apertureMemory(void);
	UInt32 apertureBytes(void);
	/* The shapes of the window server's surfaces, by surface ID. */
	bool setSurfaceRegion(UInt32 wid, const IOAccelDeviceRegion *rgn,
			      UInt32 rects);
	bool getSurfaceRegion(UInt32 wid, struct rdn_user_region *region);
	void forgetSurface(UInt32 wid);
	/*
	 * With the gate on (personality key SurfaceGate), setIDMode takes
	 * only surfaces whose owner has announced them with
	 * RDN_UC_SURFACE_BUFFER (width 0 is enough): the programs'
	 * OpenGL windows, not the window server's own windows of 10.5.
	 */
	bool surfaceGate(void) { return fSurfaceGate; }
	bool isGLSurface(UInt32 wid);
	bool setSurfaceBacking(UInt32 wid, task_t task, const struct rdn_user_backing *backing);
	bool getSurfaceBacking(UInt32 wid, task_t task, struct rdn_user_backing *backing);
	bool setSurfaceBuffer(UInt32 wid, UInt32 offset, UInt32 rowBytes,
			      UInt32 width, UInt32 height, void *owner);
	bool getSurfaceBuffer(UInt32 wid, UInt32 *offset, UInt32 *rowBytes,
			      UInt32 *width, UInt32 *height);
	void listSurfaces(struct rdn_user_surfaces *list);
	void querySurface(UInt32 wid, struct rdn_user_surface_info *info);
	/* Put a surface's picture on the screen, inside its shape. */
	IOReturn flushSurface(UInt32 wid);
	/*
	 * Fill a rectangle of the screen, and copy one to another place on
	 * it, with the GPU (rdn_user.h). Queued; `fence` is what to wait for.
	 */
	IOReturn screenFill(SInt32 x, SInt32 y, SInt32 w, SInt32 h, UInt32 colour,
			    UInt32 *fence);
	IOReturn screenCopy(SInt32 sx, SInt32 sy, SInt32 dx, SInt32 dy,
			    SInt32 w, SInt32 h, UInt32 *fence);
	/* The surface that is read-locked now (0: none). */
	void setReadLocked(UInt32 wid) { fReadLocked = wid; }
	UInt32 readLocked(void) { return fReadLocked; }

private:
	RadeonNI *fFramebuffer;
	IOService *fUvdProvider;
	IOLock *fLock;
	IOLock *fUvdLock;
	RadeonNIVideoSession *fVideoSession;
	UInt64 fVideoEpoch;
	volatile UInt32 fVideoCpuBytes;
	bool fVideoBusy;
	bool videoReady(bool administrator, bool validation);
	int videoCommand(UInt32 type, const struct rdn_uvd_h264_picture *picture,
		UInt32 bitstreamBytes);
	void videoCleanup(bool destroy);
	int videoBindReadback(void);
	int videoUnbindReadback(void);
	int videoReadback(void);
	int videoSubmitGfx(UInt32 words,bool poisonOnBusy);
	int videoInitializeVram(void);
	void videoPoison(int error);
	bool fRetiring;
	struct rdn_accel fAccel;
	struct rdn_mem fMem;
	/* Video memory beyond the aperture; fHiddenSize 0 if there is none. */
	struct rdn_mem fHidden;
	UInt32 fHiddenOffset, fHiddenSize;
	void *fPfp, *fMe;
	bool fEngineUp;
	struct rdn_uvd fUvd;
	UInt32 fUvdState, fUvdOffset, fUvdBytes, fUvdStatus;
	SInt32 fUvdError;
	bool fUvdColdAttempted;
	struct rdn_user_uvd_fixture fUvdFixture;
	UInt32 fUvdFixtureOffset, fUvdFixtureBytes;
	UInt8 *fUvdFixtureSnapshot;
	/* The power state the card was last put in (hw/rdn_pm.h). */
	struct rdn_pm_state fPower;
	bool fPowerKnown;
	/* Hand out surface clients (RadeonNISurface.h)? */
	bool fSurfaces;
	bool fSurfaceGate;
	struct SurfaceRecord {
		UInt32 wid;
		bool used;
		/* announced by its owner (RDN_UC_SURFACE_BUFFER) */
		bool gl;
		struct rdn_user_region region;
		/* The surface's picture, if its owner has said where it is. */
		UInt32 bufOffset, bufRowBytes, bufWidth, bufHeight;
		void *owner;
		task_t backingTask;
		struct rdn_user_backing backing;
	};
	SurfaceRecord *fShapes;
	UInt32 fShapeCapacity;
	int surfaceSlot(UInt32 wid, bool create);
	UInt32 fReadLocked;
	/* flushSurface(): the last copy's fence, and its rectangles. */
	bool fBlitPending;
	UInt32 fBlitFence;
	struct rdn_blit_rect fBlitRects[RDN_BLIT_MAX_RECTS];
	/*
	 * screenFill() and screenCopy(): work areas used in turn, each with
	 * the fence that frees it, and the newest of those fences.
	 */
	enum { kScreenSlots = 3 };
	struct {
		bool pending;
		UInt32 fence;
	} fScreenSlot[kScreenSlots];
	UInt32 fScreenNext;
	bool fScreenPending;
	UInt32 fScreenFence;
	/* screenCopy()'s scratch surface; fMoveBytes 0 before the first copy. */
	UInt32 fMoveOffset, fMoveBytes;
	bool fMoveHidden;
	IOReturn screenBegin(struct rdn_selftest_target *screen, UInt32 *slot);
	void screenEnd(UInt32 slot);
	IOReturn moveScratch(const struct rdn_selftest_target *screen,
			     struct rdn_draw_surface *scratch);
	/* See RadeonNIAGPShim. */
	IOAGPDevice *fAncestor;
	RadeonNIAGPShim *fShim;

	bool startEngine(void);
	static void prepareUvdBeforeCP(void *cookie);
	IOReturn uvdDiagnosticInitImpl(UInt32 *status, bool prepareOnly);
	void releaseUvdFixtureSnapshot(void);
	/* rdn_gart=1: the page table for system memory, and a test of it. */
	void startGart(void);
	struct rdn_gart fGart;
	IOBufferMemoryDescriptor *fGartDummy, *fGartTest;
	UInt32 fGartTable;
	/* What programs have bound: byte offsets of the GART's range. */
	struct rdn_mem fGartSpace;
	enum { kMaxGartBindings = 512 };
	struct {
		IOMemoryDescriptor *memory;
		UInt32 offset, size;
		void *owner;
	} fGartBound[kMaxGartBindings];
	void *copyFirmware(const char *key, UInt32 *size);
};

class RadeonNIUserClient : public IOUserClient
{
	/* Mesa asks for every buffer by itself: a game has tens of thousands. */
	enum { kMaxAllocations = 65536 };

	OSDeclareDefaultStructors(RadeonNIUserClient)

public:
	virtual bool initWithTask(task_t owningTask, void *securityID, UInt32 type);
	virtual bool start(IOService *provider);
	virtual IOReturn clientClose(void);
	virtual IOReturn clientDied(void);
	virtual void stop(IOService *provider);
	virtual IOExternalMethod *getTargetAndMethodForIndex(IOService **target,
							     UInt32 index);
	virtual IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options,
					     IOMemoryDescriptor **memory);

	/* Methods, in the order of hw/rdn_user.h. */
	IOReturn methodGetInfo(struct rdn_user_info *info, IOByteCount *size);
	IOReturn methodAlloc(UInt32 size, UInt32 align, UInt32 *offset);
	IOReturn methodFree(UInt32 offset);
	IOReturn methodSubmit(UInt32 offset, UInt32 words, UInt32 *fence);
	IOReturn methodFenceWait(UInt32 fence, UInt32 timeoutMs, UInt32 *reached);
	IOReturn methodSyncForCPU(void);
	IOReturn methodSurfaceRegion(UInt32 wid, struct rdn_user_region *region,
				     IOByteCount *size);
	IOReturn methodSurfaceBuffer(UInt32 wid, UInt32 offset, UInt32 rowBytes,
				     UInt32 width, UInt32 height);
	IOReturn methodSurfaceBacking(UInt32 wid, struct rdn_user_backing *backing, IOByteCount *size);
	IOReturn methodSurfaceQuery(UInt32 wid, struct rdn_user_surface_info *info, IOByteCount *size);
	IOReturn methodSurfaceList(struct rdn_user_surfaces *list, IOByteCount *size);
	IOReturn methodSurfaceLocked(UInt32 *wid);
	IOReturn methodRegRead(UInt32 offset, UInt32 *value);
	IOReturn methodUvdDiagnosticInfo(struct rdn_user_uvd_diagnostic *info,
				       IOByteCount *size);
	IOReturn methodUvdDiagnosticInit(UInt32 reserved, UInt32 *status);
	IOReturn methodUvdFixtureRun(UInt32 reserved, struct rdn_user_uvd_fixture *result,
				    IOByteCount *size);
	IOReturn methodUvdFixtureRead(UInt32 generation, UInt32 chunk, void *bytes,
				     IOByteCount *size);
	IOReturn methodAllocHidden(UInt32 size, UInt32 align, UInt32 *offset);
	IOReturn methodHiddenInfo(UInt32 *offset, UInt32 *size);
	IOReturn methodGartInfo(UInt32 *on, UInt32 *gpuStart, UInt32 *pages);
	IOReturn methodGartBind(UInt32 address, UInt32 size, UInt32 *offset);
	IOReturn methodGartUnbind(UInt32 offset);
	IOReturn methodGartBind64(UInt32 low, UInt32 high, UInt32 size, UInt32 *offset);
	IOReturn methodPower(UInt32 state, UInt32 what, UInt32 *sclk,
			     UInt32 *mclk, UInt32 *temperature);
	IOReturn methodScreenFill(UInt32 x, UInt32 y, UInt32 width, UInt32 height,
				  UInt32 colour, UInt32 *fence);
	IOReturn methodScreenCopy(UInt32 sx, UInt32 sy, UInt32 dx, UInt32 dy,
				  UInt32 size, UInt32 *fence);

private:
	RadeonNIAccel *fAccel;
	task_t fTask;
	bool fUvdAdministrator;
	/* What this client allocated, so that it can be freed when it goes. */
	UInt32 *fOffsets;
	UInt32 fCount;

	void freeAll(void);
};

#endif /* RADEONNIACCEL_H */
