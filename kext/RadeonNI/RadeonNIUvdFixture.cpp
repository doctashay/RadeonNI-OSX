/* Fixed H.264 bring-up diagnostic. No client-supplied GPU commands. */
#include <IOKit/IOLib.h>
#include <libkern/libkern.h>
#include <sys/errno.h>
#include "RadeonNI.h"
#include "RadeonNIAccel.h"
extern "C" {
#include "rdn_uvd_h264.h"
#include "../../tests/uvd_decode_fixture.h"
}

enum { kFixturePage = 4096, kFixtureSpans = 6, kFixtureTimeout = 1000 };
enum { kMessage, kFeedback, kIB, kBitstream, kDPB, kTarget };
static const UInt32 fixtureSizes[kFixtureSpans] = {
	4096, 4096, 4096, 4096, 15925248, 737280
};
static const UInt32 fixtureSnapshotBytes =
	RDN_UVD_FIXTURE_TARGET_BYTES + RDN_UVD_H264_FEEDBACK_BYTES;

static void fixtureDefaults(struct rdn_uvd_h264_config *config,
	struct rdn_uvd_h264_picture *picture)
{
	bzero(config, sizeof(*config));
	config->version = RDN_UVD_H264_VERSION;
	config->profile_idc = 100;
	config->level_idc = 30;
	config->coded_width = RDN_UVD_FIXTURE_WIDTH;
	config->coded_height = RDN_UVD_FIXTURE_HEIGHT;
	config->max_num_ref_frames = 2;
	config->chroma_format_idc = 1;
	config->frame_mbs_only_flag = 1;
	bzero(picture, sizeof(*picture));
	picture->version = RDN_UVD_H264_VERSION;
	picture->sps_info_flags = 5;
	picture->pps_info_flags = 0x88;
	picture->log2_max_frame_num_minus4 = 1;
	picture->log2_max_pic_order_cnt_lsb_minus4 = 3;
	picture->pic_init_qp_minus26 = 2;
	memset(picture->scaling_list_4x4, 16, sizeof(picture->scaling_list_4x4));
	memset(picture->scaling_list_8x8, 16, sizeof(picture->scaling_list_8x8));
}

static void fixtureWrite(volatile UInt8 *dst, const void *src, UInt32 bytes)
{
	const UInt8 *input = (const UInt8 *)src;
	for (UInt32 i = 0; i < bytes; ++i)
		dst[i] = input[i];
}

static void fixtureFill(volatile UInt8 *dst, UInt32 bytes, UInt32 word)
{
	volatile UInt32 *words = (volatile UInt32 *)dst;
	for (UInt32 i = 0; i < bytes / sizeof(*words); ++i)
		words[i] = word;
}

static bool fixtureGuards(volatile UInt8 *base, const UInt32 *offsets)
{
	for (unsigned i = 0; i < kFixtureSpans; ++i) {
		for (UInt32 j = 0; j < kFixturePage; ++j)
			if (base[offsets[i] - kFixturePage + j] != 0xa5 ||
			    base[offsets[i] + fixtureSizes[i] + j] != 0xa5)
				return false;
	}
	return true;
}

static int fixtureBuild(UInt32 type, UInt32 order,
	const struct rdn_uvd_h264_config *config,
	const struct rdn_uvd_h264_picture *picture,
	const struct rdn_uvd_h264_layout *layout,
	const struct rdn_uvd_h264_ib_buffers *all,
	UInt8 *message, UInt8 *ib, uint32_t *words)
{
	struct rdn_uvd_h264_ib_buffers buffers = *all;
	int r;
	if (type == RDN_UVD_H264_MSG_CREATE)
		r = rdn_uvd_h264_pack_create(config, layout, 1, message,
			RDN_UVD_H264_MESSAGE_BYTES);
	else if (type == RDN_UVD_H264_MSG_DECODE)
		r = rdn_uvd_h264_pack_decode(config, picture, layout, 1, 1,
			RDN_UVD_FIXTURE_BITSTREAM_BYTES, message, RDN_UVD_H264_MESSAGE_BYTES);
	else
		r = rdn_uvd_h264_pack_destroy(1, message, RDN_UVD_H264_MESSAGE_BYTES);
	if (r)
		return -EINVAL;
	if (type != RDN_UVD_H264_MSG_DECODE) {
		bzero(&buffers.dpb, sizeof(buffers.dpb));
		bzero(&buffers.bitstream, sizeof(buffers.bitstream));
		bzero(&buffers.target, sizeof(buffers.target));
		bzero(&buffers.feedback, sizeof(buffers.feedback));
	}
	return rdn_uvd_h264_build_ib(config, layout, type, order,
		type == RDN_UVD_H264_MSG_DECODE ? RDN_UVD_FIXTURE_BITSTREAM_BYTES : 0,
		&buffers, ib, RDN_UVD_H264_IB_MAX_BYTES, words) ? -EINVAL : 0;
}

IOReturn RadeonNIAccel::uvdFixtureRun(struct rdn_user_uvd_fixture *result)
{
	struct rdn_uvd_h264_config config;
	struct rdn_uvd_h264_picture picture;
	struct rdn_uvd_h264_layout layout;
	struct rdn_uvd_h264_ib_buffers buffers;
	UInt8 message[RDN_UVD_H264_MESSAGE_BYTES], ib[RDN_UVD_H264_IB_MAX_BYTES];
	UInt32 offsets[kFixtureSpans], bytes = 0, order, waited;
	uint32_t words = 0, fence = 0;
	uint64_t allocation = 0, address = 0;
	volatile UInt8 *base = 0;
	bool submitted = false, allocated = false, changed = false;
	int error = 0;
	IOReturn callResult = kIOReturnSuccess;
	if (!result || !fLock)
		return kIOReturnBadArgument;
	IOLockLock(fUvdLock);
	IOLockLock(fLock);
	if (fRetiring || fVideoBusy || !fFramebuffer) {
		callResult = kIOReturnNotReady;
		goto out;
	}
	if (fFramebuffer->getProperty("EnableUVDDiagnostics") != kOSBooleanTrue) {
		callResult = kIOReturnNotPermitted;
		goto out;
	}
	if (fUvdFixture.state != RDN_UVD_FIXTURE_IDLE)
		goto out;
	if (!fEngineUp || fUvdState != RDN_UVD_DIAGNOSTIC_RING_READY ||
	    !fUvd.ready || fUvd.failed || fUvd.stopped || !fUvdProvider ||
	    !fAccel.aperture || (uintptr_t)fAccel.aperture > UINTPTR_MAX - fAccel.aperture_size ||
	    fUvdProvider->getProperty(RDN_UVD_REBOOT_PROPERTY) != kOSBooleanTrue) {
		callResult = kIOReturnNotReady;
		goto out;
	}
	bzero(&fUvdFixture, sizeof(fUvdFixture));
	fUvdFixture.version = RDN_UVD_FIXTURE_VERSION;
	fUvdFixture.fixture_id = RDN_UVD_FIXTURE_ID;
	fUvdFixture.state = RDN_UVD_FIXTURE_RUNNING;
	fUvdFixture.generation = 1;
	fUvdFixture.width = RDN_UVD_FIXTURE_WIDTH;
	fUvdFixture.height = RDN_UVD_FIXTURE_HEIGHT;
	fUvdFixture.target_pitch = RDN_UVD_FIXTURE_PITCH;
	fUvdFixture.target_bytes = RDN_UVD_FIXTURE_TARGET_BYTES;
	fUvdFixture.snapshot_bytes = fixtureSnapshotBytes;
	fUvdFixture.feedback_offset = RDN_UVD_FIXTURE_TARGET_BYTES;
	fUvdFixture.feedback_bytes = RDN_UVD_H264_FEEDBACK_BYTES;
	fUvdFixture.message_variant = RDN_UVD_FIXTURE_PACKED_MESSAGE;
	fixtureDefaults(&config, &picture);
	if (rdn_uvd_h264_layout_init(&config, RDN_UVD_H264_TARGET_FIELDS, &layout) ||
	    layout.dpb_allocation_bytes != fixtureSizes[kDPB] ||
	    layout.target_allocation_bytes != fixtureSizes[kTarget] ||
	    fUvd.ring_words > RDN_UVD_MAX_RING_BYTES / 4) {
		error = -EINVAL;
		goto finish;
	}
	for (unsigned i = 0; i < kFixtureSpans; ++i) {
		offsets[i] = bytes + kFixturePage;
		bytes += 2 * kFixturePage + fixtureSizes[i];
	}
	if (rdn_mem_alloc(&fMem, bytes, kFixturePage, &allocation)) {
		error = -ENOMEM;
		goto finish;
	}
	allocated = true;
	if (allocation > fAccel.aperture_size || bytes > fAccel.aperture_size - allocation ||
	    fAccel.vram_base >= (1ull << 40) ||
	    allocation >= (1ull << 40) - fAccel.vram_base) {
		error = -EINVAL;
		goto finish;
	}
	address = fAccel.vram_base + allocation;
	if (bytes > (1ull << 40) - address) {
		error = -EINVAL;
		goto finish;
	}
	bzero(&buffers, sizeof(buffers));
	buffers.vcpu.address = fUvd.vcpu_gpu;
	buffers.vcpu.bytes = fUvd.vcpu_bytes;
	buffers.ring.address = fUvd.ring_gpu;
	buffers.ring.bytes = fUvd.ring_words * 4 + RDN_UVD_RING_EXTRA;
	buffers.message.address = address + offsets[kMessage]; buffers.message.bytes = fixtureSizes[kMessage];
	buffers.feedback.address = address + offsets[kFeedback]; buffers.feedback.bytes = fixtureSizes[kFeedback];
	buffers.ib.address = address + offsets[kIB]; buffers.ib.bytes = fixtureSizes[kIB];
	buffers.bitstream.address = address + offsets[kBitstream]; buffers.bitstream.bytes = fixtureSizes[kBitstream];
	buffers.dpb.address = address + offsets[kDPB]; buffers.dpb.bytes = fixtureSizes[kDPB];
	buffers.target.address = address + offsets[kTarget]; buffers.target.bytes = fixtureSizes[kTarget];
	order = fUvd.swapped ? RDN_UVD_H264_IB_BE : RDN_UVD_H264_IB_LE;
	/* Validate the entire decode layout before writing any GPU-visible byte. */
	error = fixtureBuild(RDN_UVD_H264_MSG_DECODE, order, &config, &picture,
		&layout, &buffers, message, ib, &words);
	if (error)
		goto finish;
	if ((address < buffers.vcpu.address + buffers.vcpu.bytes &&
	     buffers.vcpu.address < address + bytes) ||
	    (address < buffers.ring.address + buffers.ring.bytes &&
	     buffers.ring.address < address + bytes)) {
		error = -EINVAL;
		goto finish;
	}
	fUvdFixtureSnapshot = (UInt8 *)IOMalloc(fixtureSnapshotBytes);
	if (!fUvdFixtureSnapshot) {
		error = -ENOMEM;
		goto finish;
	}
	error = rdn_fence_emit(&fAccel, &fence);
	if (error)
		goto finish;
	for (waited = 0; waited < kFixtureTimeout; ++waited) {
		if (rdn_fence_done(&fAccel, fence))
			break;
		IOSleep(1);
	}
	if (waited == kFixtureTimeout && !rdn_fence_done(&fAccel, fence)) {
		error = -ETIMEDOUT;
		goto finish;
	}
	base = (volatile UInt8 *)fAccel.aperture + (UInt32)allocation;
	fixtureFill(base, bytes, 0xa5a5a5a5u);
	for (unsigned i = 0; i < kFixtureSpans; ++i)
		fixtureFill(base + offsets[i], fixtureSizes[i], 0);
	fixtureFill(base + offsets[kTarget], fixtureSizes[kTarget], 0xcdcdcdcdu);
	fixtureWrite(base + offsets[kBitstream], rdn_uvd_fixture_bitstream,
		RDN_UVD_FIXTURE_BITSTREAM_BYTES);
	base[offsets[kFeedback] + 1] = 8; /* LE feedback size2048. */
	fUvdFixtureOffset = (UInt32)allocation;
	fUvdFixtureBytes = bytes;
	error = rdn_uvd_wake(&fUvd);
	if (error)
		goto finish;
	for (UInt32 type = RDN_UVD_H264_MSG_CREATE; type <= RDN_UVD_H264_MSG_DESTROY; ++type) {
		uint32_t *sequence = type == RDN_UVD_H264_MSG_CREATE ? &fUvdFixture.create_fence :
			type == RDN_UVD_H264_MSG_DECODE ? &fUvdFixture.decode_fence : &fUvdFixture.destroy_fence;
		fUvdFixture.stage = type == RDN_UVD_H264_MSG_CREATE ? RDN_UVD_FIXTURE_CREATE_WAIT :
			type == RDN_UVD_H264_MSG_DECODE ? RDN_UVD_FIXTURE_DECODE_WAIT : RDN_UVD_FIXTURE_DESTROY_WAIT;
		error = fixtureBuild(type, order, &config, &picture, &layout, &buffers, message, ib, &words);
		if (error)
			goto finish;
		fixtureWrite(base + offsets[kMessage], message, sizeof(message));
		fixtureWrite(base + offsets[kIB], ib, words * 4);
		rdn_hdp_flush(&fAccel);
		submitted = true;
		error = rdn_uvd_ib_submit(&fUvd, buffers.ib.address, words, sequence);
		if (error)
			goto finish;
		error = rdn_uvd_fence_wait(&fUvd, *sequence, kFixtureTimeout);
		if (error)
			goto finish;
	}
	fUvdFixture.stage = RDN_UVD_FIXTURE_SNAPSHOT;
	rdn_hdp_flush(&fAccel);
	if (!fixtureGuards(base, offsets)) {
		error = -EIO;
	} else {
		fUvdFixture.flags |= RDN_UVD_FIXTURE_GUARDS_OK;
	}
	for (UInt32 i = 0; i < RDN_UVD_FIXTURE_TARGET_BYTES; ++i) {
		UInt8 value = base[offsets[kTarget] + i];
		fUvdFixtureSnapshot[i] = value;
		changed = changed || value != 0xcd;
	}
	if (!changed) {
		error = -EIO;
	}
	for (UInt32 i = 0; i < RDN_UVD_H264_FEEDBACK_BYTES; ++i)
		fUvdFixtureSnapshot[RDN_UVD_FIXTURE_TARGET_BYTES + i] = base[offsets[kFeedback] + i];
	if (!error)
		fUvdFixture.idle_error = rdn_uvd_idle(&fUvd);
	fUvdFixture.flags |= RDN_UVD_FIXTURE_OUTPUT_READY;
	fUvdFixture.stage = RDN_UVD_FIXTURE_FINISHED;
finish:
	fUvdFixture.error = error;
	fUvdFixture.state = error ? RDN_UVD_FIXTURE_FAILED : RDN_UVD_FIXTURE_COMPLETE;
	if (error && fUvdFixtureSnapshot &&
	    !(fUvdFixture.flags & RDN_UVD_FIXTURE_OUTPUT_READY)) {
		IOFree(fUvdFixtureSnapshot, fixtureSnapshotBytes);
		fUvdFixtureSnapshot = 0;
	}
	if (allocated && !submitted) {
		rdn_mem_free(&fMem, allocation);
		fUvdFixtureOffset = fUvdFixtureBytes = 0;
	}
	if ((error && submitted) || fUvd.failed || fUvdFixture.idle_error) {
		fUvd.failed = true;
		fUvd.ready = false;
		fUvdState = RDN_UVD_DIAGNOSTIC_FAULTED;
		fUvdError = error ? error : fUvdFixture.idle_error;
	}
	IOLog("RadeonNI: fixed H264 fixture state %lu stage %lu error %d fences %lu/%lu/%lu retained %lu\n",
	      (unsigned long)fUvdFixture.state, (unsigned long)fUvdFixture.stage, error,
	      (unsigned long)fUvdFixture.create_fence, (unsigned long)fUvdFixture.decode_fence,
	      (unsigned long)fUvdFixture.destroy_fence, (unsigned long)fUvdFixtureBytes);
out:
	*result = fUvdFixture;
	IOLockUnlock(fLock);
	IOLockUnlock(fUvdLock);
	return callResult;
}

void RadeonNIAccel::releaseUvdFixtureSnapshot(void)
{
	IOLockLock(fUvdLock);
	IOLockLock(fLock);
	if (fUvdFixtureSnapshot) {
		IOFree(fUvdFixtureSnapshot, fUvdFixture.snapshot_bytes);
		fUvdFixtureSnapshot = 0;
	}
	fUvdFixture.flags &= ~RDN_UVD_FIXTURE_OUTPUT_READY;
	IOLockUnlock(fLock);
	IOLockUnlock(fUvdLock);
}

IOReturn RadeonNIAccel::uvdFixtureRead(UInt32 generation, UInt32 chunk, void *bytes)
{
	if (!bytes || !fLock)
		return kIOReturnBadArgument;
	bzero(bytes, RDN_UVD_FIXTURE_CHUNK_BYTES);
	IOLockLock(fUvdLock);
	IOLockLock(fLock);
	IOReturn result = kIOReturnNotReady;
	if (fRetiring || !fUvdFixtureSnapshot ||
	    !(fUvdFixture.flags & RDN_UVD_FIXTURE_OUTPUT_READY))
		goto out;
	if (!generation || generation != fUvdFixture.generation ||
	    chunk >= (fixtureSnapshotBytes + RDN_UVD_FIXTURE_CHUNK_BYTES - 1) / RDN_UVD_FIXTURE_CHUNK_BYTES) {
		result = kIOReturnBadArgument;
		goto out;
	}
	{
		UInt32 offset = chunk * RDN_UVD_FIXTURE_CHUNK_BYTES;
		UInt32 count = fixtureSnapshotBytes - offset;
		if (count > RDN_UVD_FIXTURE_CHUNK_BYTES)
			count = RDN_UVD_FIXTURE_CHUNK_BYTES;
		memcpy(bytes, fUvdFixtureSnapshot + offset, count);
	}
	result = kIOReturnSuccess;
out:
	IOLockUnlock(fLock);
	IOLockUnlock(fUvdLock);
	return result;
}

IOReturn RadeonNIUserClient::methodUvdFixtureRun(UInt32 reserved,
	struct rdn_user_uvd_fixture *result, IOByteCount *size)
{
	if (!fAccel || !result || !size || *size != sizeof(*result) || reserved)
		return kIOReturnBadArgument;
	bzero(result, sizeof(*result));
	if (!fUvdAdministrator)
		return kIOReturnNotPrivileged;
	return fAccel->uvdFixtureRun(result);
}

IOReturn RadeonNIUserClient::methodUvdFixtureRead(UInt32 generation, UInt32 chunk,
	void *bytes, IOByteCount *size)
{
	if (!fAccel || !bytes || !size || *size != RDN_UVD_FIXTURE_CHUNK_BYTES)
		return kIOReturnBadArgument;
	bzero(bytes, RDN_UVD_FIXTURE_CHUNK_BYTES);
	if (!fUvdAdministrator)
		return kIOReturnNotPrivileged;
	return fAccel->uvdFixtureRead(generation, chunk, bytes);
}
