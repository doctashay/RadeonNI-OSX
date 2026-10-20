/*
 * RadeonNI AVC: a QuickTime image decompressor for H.264 ('avc1') that decodes on the card's video decoder
 * (UVD) through rdnvd (mesa/target/rdnvd.h).
 *
 * It is a sub-codec of QuickTime's base image decompressor, written the way QuickTime 7 wants a codec with
 * frame reordering written: BeginBand/DecodeBand get the samples in decoding order, DrawBand the pictures
 * in display order, so each sample's decoded picture is kept in the per-frame record until it is drawn.
 * Pictures leave as packed 4:2:2 ('2vuy'), the pixel format Apple's own H.264 decoder offers.
 *
 * A stream the card cannot decode (other than 4:2:0 at 8 bits, interlaced coding, too large) is declined in
 * Preflight, with no card or no kext too, so that QuickTime uses another decompressor.
 *
 * Copyright (c) 2026 kouta-kun and Claude
 * SPDX-License-Identifier: MIT
 */

#include <CoreServices/CoreServices.h>
#include <crt_externs.h>
#include <QuickTime/QuickTime.h>
#include <QuickTime/ImageCodec.h>
#include <CoreServices/Components.k.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "rdnvd.h"

#define kRDNAVCVersion	0x00010000

/* What QuickTime keeps for each frame between BeginBand and EndBand (sized in Initialize). */
typedef struct {
	struct rdnvd_frame *frame;	/* the sample's decoded picture, from DecodeBand until EndBand */
	long dataSize;
	long long display;		/* the display time QuickTime gave the sample, for the log */
	long width, height;
	Boolean decoded;
	Boolean failed;
	UInt8 pad[2];
} RDNAVCDecompressRecord;

typedef struct {
	ComponentInstance self, delegate, target;
	struct rdnvd_session *session;
	unsigned nalLengthSize;
	Boolean prepared;
	long width, height;
	OSType **wantedPixelTypes;
} RDNAVCGlobals;

/* rdnvd is not thread safe, and QuickTime may call from several threads. */
static pthread_mutex_t gLock = PTHREAD_MUTEX_INITIALIZER;
static struct rdnvd *gDevice;
static int gDeviceTried;

/* Log every band: RDNAVC_DEBUG in the environment, or (for a program started from the Finder) the file /tmp/rdnavc.debug
 * when the program starts using the component. */
static int debug(void)
{
	static int on = -1;

	if (on < 0)
		on = getenv("RDNAVC_DEBUG") != NULL || access("/tmp/rdnavc.debug", F_OK) == 0;
	return on;
}

static void say(const char *format, ...)
{
	char line[256];
	va_list ap;

	va_start(ap, format);
	vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	syslog(LOG_NOTICE, "RadeonNIAVC: %s", line);
}

/* The standard calls and the sub-codec calls this component implements (the table is in the dispatch file). */
#define IMAGECODEC_BASENAME()	RDNAVC_
#define IMAGECODEC_GLOBALS()	RDNAVCGlobals *storage
#define CALLCOMPONENT_BASENAME()	IMAGECODEC_BASENAME()
#define CALLCOMPONENT_GLOBALS()		IMAGECODEC_GLOBALS()
#include <QuickTime/ImageCodec.k.h>

pascal ComponentResult RDNAVC_Open(RDNAVCGlobals *storage, ComponentInstance self);
pascal ComponentResult RDNAVC_CanDo(RDNAVCGlobals *storage, short ftnNumber);
pascal ComponentResult RDNAVC_Close(RDNAVCGlobals *storage, ComponentInstance self);
pascal ComponentResult RDNAVC_Version(RDNAVCGlobals *storage);
pascal ComponentResult RDNAVC_Register(RDNAVCGlobals *storage);
pascal ComponentResult RDNAVC_Target(RDNAVCGlobals *storage, ComponentInstance target);

#define COMPONENT_UPP_SELECT_ROOT()	ImageCodec
#define COMPONENT_DISPATCH_FILE		"RadeonNIAVCDispatch.h"
#define GET_DELEGATE_COMPONENT()	((storage) ? (storage)->delegate : NULL)
#include <QuickTime/ComponentDispatchHelper.c>

/* ---- standard calls ------------------------------------------------------------------ */

pascal ComponentResult RDNAVC_Open(RDNAVCGlobals *storage, ComponentInstance self)
{
	RDNAVCGlobals *glob = calloc(1, sizeof(*glob));
	ComponentResult err;

	if (!glob)
		return memFullErr;
	glob->self = self;
	glob->target = self;
	SetComponentInstanceStorage(self, (Handle)glob);
	/* The base image decompressor does the rest of the work: scaling, matrices, buffers, scheduling. */
	err = OpenADefaultComponent(decompressorComponentType, kBaseCodecType, &glob->delegate);
	if (err) {
		free(glob);
		SetComponentInstanceStorage(self, NULL);
		return err;
	}
	ComponentSetTarget(glob->delegate, self);
	return noErr;
}

pascal ComponentResult RDNAVC_Close(RDNAVCGlobals *storage, ComponentInstance self)
{
	if (storage) {
		if (debug())
			say("Close: inst %p session %p prepared %d", (void *)storage, (void *)storage->session,
			    (int)storage->prepared);
		pthread_mutex_lock(&gLock);
		if (storage->session)
			rdnvd_h264_close(storage->session);
		pthread_mutex_unlock(&gLock);
		if (storage->delegate)
			CloseComponent(storage->delegate);
		if (storage->wantedPixelTypes)
			DisposeHandle((Handle)storage->wantedPixelTypes);
		free(storage);
	}
	return noErr;
}

pascal ComponentResult RDNAVC_Version(RDNAVCGlobals *storage)
{
	return kRDNAVCVersion;
}

pascal ComponentResult RDNAVC_Register(RDNAVCGlobals *storage)
{
	return noErr;
}

pascal ComponentResult RDNAVC_Target(RDNAVCGlobals *storage, ComponentInstance target)
{
	storage->target = target;
	return noErr;
}

/* ---- image codec calls ----------------------------------------------------------------- */

pascal ComponentResult RDNAVC_GetCodecInfo(RDNAVCGlobals *storage, CodecInfo *info)
{
	static const unsigned char name[] = "\pH.264 (RadeonNI, UVD)";

	if (!info)
		return paramErr;
	memset(info, 0, sizeof(*info));
	memcpy(info->typeName, name, sizeof(name));
	info->version = 1;
	info->revisionLevel = 1;
	info->vendor = 'RdNI';
	/* the same as the thng resource's flags: 32-bit pixels, temporal compression */
	info->decompressFlags = codecInfoDoes32 | codecInfoDoesTemporal;
	info->formatFlags = codecInfoDepth24;
	info->decompressionAccuracy = 200;
	info->decompressionSpeed = 5;
	info->compressionLevel = 200;
	info->minimumHeight = 16;
	info->minimumWidth = 16;
	return noErr;
}

/*
 * How long before a picture is to be shown it may be decoded: the pictures a decoder holds back. QuickTime
 * takes a frame whose display time is further from its decoding time than this to be undisplayable.
 * (Nominal 24 pictures a second, in milliseconds.)
 */
pascal ComponentResult RDNAVC_GetDecompressLatency(RDNAVCGlobals *storage, TimeRecord *latency)
{
	unsigned pictures = storage->session ? rdnvd_h264_reorder_delay(storage->session) : 16;

	if (!latency)
		return paramErr;
	memset(latency, 0, sizeof(*latency));
	latency->value.lo = (pictures + 1) * 1000 / 24;
	latency->scale = 1000;
	if (debug())
		say("GetDecompressLatency: %u pictures, %ld ms", pictures, (long)latency->value.lo);
	return noErr;
}

pascal ComponentResult RDNAVC_Initialize(RDNAVCGlobals *storage, ImageSubCodecDecompressCapabilities *cap)
{
	cap->decompressRecordSize = sizeof(RDNAVCDecompressRecord);
	cap->canAsync = true;
	/* pictures decoded ahead of the one being drawn: a B pyramid holds several back */
	cap->suggestedQueueSize = 16;
	if (debug())
		say("Initialize: record size %ld (needs more than %ld for the QuickTime 7 fields)", (long)cap->recordSize,
		    (long)offsetof(ImageSubCodecDecompressCapabilities, baseCodecShouldCallDecodeBandForAllFrames));
	if (cap->recordSize > (long)offsetof(ImageSubCodecDecompressCapabilities, baseCodecShouldCallDecodeBandForAllFrames)) {
		/* always draws into the baseAddr and rowBytes DrawBand is given, every pixel of it */
		cap->subCodecIsMultiBufferAware = true;
		/* samples come in decoding order, pictures are drawn in display order */
		cap->subCodecSupportsOutOfOrderDisplayTimes = true;
		/* every sample is decoded (a picture may be a reference), including the ones that are not drawn */
		cap->baseCodecShouldCallDecodeBandForAllFrames = true;
		/*
		 * Each frame keeps its own picture until it is drawn, so a reference frame may be decoded before the
		 * one before it is drawn, and a frame may be drawn in decoding order.
		 */
		cap->subCodecSupportsDecodeSmoothing = true;
		cap->subCodecSupportsDrawInDecodeOrder = true;
		/*
		 * Without this QuickTime takes pictures decoded long before they are displayed (the anchor of a
		 * pyramid of B pictures, five pictures ahead here) as undisplayable and drops them (found by
		 * trying the capabilities one by one: every other one makes no difference). The pictures are
		 * kept apart, so it is true.
		 */
		cap->subCodecSupportsScheduledBackwardsPlaybackWithDifferenceFrames = true;
	}
	return noErr;
}

/* The decoder, once per process. */
static struct rdnvd *device(void)
{
	if (!gDevice && !gDeviceTried) {
		gDeviceTried = 1;
		gDevice = rdnvd_open();
		if (!gDevice)
			say("no video decoder (no card, a kext without the UVD firmware, or the card is busy)");
	}
	return gDevice;
}

/*
 * Programs that decode one picture of a movie for a thumbnail and then keep the movie open (Finder's icon preview, Quick Look,
 * Spotlight's importer) get QuickTime's own decoder: a decoder on the card would sit in video memory, and in a firmware session slot,
 * for as long as the preview is shown. The names, by the program's file name, are these and those of the file
 * /Library/Application Support/RadeonNI/software-decode (one per line; a line "-name" takes a name off the list).
 */
static int software_only(const char **name)
{
	static int known = -1;
	static char base[128];
	static const char *const builtin[] = { "Finder", "quicklookd", "QuickLookSatellite", "mdworker", "mdimport", NULL };

	if (known < 0) {
		char **argv = *_NSGetArgv();
		const char *path = argv && argv[0] ? argv[0] : "";
		const char *slash = strrchr(path, '/');
		FILE *f;
		char line[160];
		int i;

		snprintf(base, sizeof(base), "%s", slash ? slash + 1 : path);
		known = 0;
		for (i = 0; builtin[i]; i++)
			if (!strcmp(base, builtin[i]))
				known = 1;
		f = fopen("/Library/Application Support/RadeonNI/software-decode", "r");
		if (f) {
			while (fgets(line, sizeof(line), f)) {
				size_t n = strlen(line);

				while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' '))
					line[--n] = 0;
				if (line[0] == '-' && !strcmp(line + 1, base))
					known = 0;
				else if (line[0] && line[0] != '#' && !strcmp(line, base))
					known = 1;
			}
			fclose(f);
		}
	}
	*name = base;
	return known;
}

pascal ComponentResult RDNAVC_Preflight(RDNAVCGlobals *storage, CodecDecompressParams *p)
{
	CodecCapabilities *cap = p->capabilities;
	ImageDescriptionHandle desc = p->imageDescription;
	Handle avcc = NULL;
	unsigned nal = 0;
	OSErr err;
	int room;

	cap->bandMin = (**desc).height;
	cap->bandInc = cap->bandMin;
	cap->wantedPixelSize = 0;		/* the pixel types below say it */
	cap->extendWidth = 0;
	cap->extendHeight = 0;
	/* pictures are drawn in another order than they are decoded (B pictures) */
	cap->flags2 |= codecSupportsOutOfOrderDisplayTimes | codecSupportsScheduledBackwardsPlaybackWithDifferenceFrames;

	if ((**desc).cType != 'avc1')
		return codecUnimpErr;
	{
		const char *who;

		if (software_only(&who)) {
			say("declined: %s shows previews with QuickTime's own decoder", who);
			return codecUnimpErr;
		}
	}
	err = GetImageDescriptionExtension(desc, &avcc, 'avcC', 1);
	if (err || !avcc) {
		say("declined: no avcC in the image description (%d)", (int)err);
		return codecUnimpErr;
	}

	/* a stream the card does not take is declined without touching the card */
	HLock(avcc);
	if (!rdnvd_h264_supported((const uint8_t *)*avcc, (size_t)GetHandleSize(avcc), NULL, NULL)) {
		HUnlock(avcc);
		say("declined: a %ldx%ld stream the card does not decode", (long)(**desc).width, (long)(**desc).height);
		return codecUnimpErr;
	}
	HUnlock(avcc);

	pthread_mutex_lock(&gLock);
	if (storage->session) {
		rdnvd_h264_close(storage->session);
		storage->session = NULL;
	}
	if (device()) {
		HLock(avcc);
		storage->session = rdnvd_h264_open(gDevice, (const uint8_t *)*avcc, (size_t)GetHandleSize(avcc), NULL,
						   NULL, &nal);
		HUnlock(avcc);
	}
	pthread_mutex_unlock(&gLock);
	if (!storage->session) {
		say("declined: %ldx%ld stream not taken by the card", (long)(**desc).width, (long)(**desc).height);
		return codecUnimpErr;
	}
	/* another decoder (this program's or another's) may have the card's video memory the decoder reaches */
	pthread_mutex_lock(&gLock);
	room = rdnvd_h264_room(gDevice, (unsigned)(**desc).width, (unsigned)(**desc).height);
	pthread_mutex_unlock(&gLock);
	if (!room) {
		say("declined: no video memory for a %ldx%ld decoder now (another decoder has it)", (long)(**desc).width,
		    (long)(**desc).height);
		pthread_mutex_lock(&gLock);
		rdnvd_h264_close(storage->session);
		storage->session = NULL;
		pthread_mutex_unlock(&gLock);
		return codecUnimpErr;
	}
	storage->nalLengthSize = nal;
	storage->width = (**desc).width;
	storage->height = (**desc).height;
	/* QuickTime opens several instances to ask about one movie: the decoder (100 MB of video memory) waits for the first picture */
	say("taking a %ldx%ld stream (NAL length size %u) inst %p", storage->width, storage->height, nal, (void *)storage);

	if (!storage->wantedPixelTypes) {
		storage->wantedPixelTypes = (OSType **)NewHandleClear(2 * sizeof(OSType));
		if (!storage->wantedPixelTypes)
			return memFullErr;
	}
	(*storage->wantedPixelTypes)[0] = k422YpCbCr8PixelFormat;	/* '2vuy' */
	(*storage->wantedPixelTypes)[1] = 0;
	p->wantedDestinationPixelTypes = storage->wantedPixelTypes;
	return noErr;
}

/* The frame type of a sample from its first slice: QuickTime schedules and drops by it. */
static UInt8 frame_type(const UInt8 *data, long size, unsigned nalLengthSize)
{
	long pos = 0;

	while (nalLengthSize && pos + (long)nalLengthSize < size) {
		unsigned i;
		unsigned long len = 0;
		UInt8 header;

		for (i = 0; i < nalLengthSize; i++)
			len = len << 8 | data[pos + i];
		pos += nalLengthSize;
		if (!len || pos >= size)
			break;
		header = data[pos];
		if ((header & 0x1f) == 5)
			return kCodecFrameTypeKey;
		if ((header & 0x1f) == 1)
			return (header & 0x60) ? kCodecFrameTypeDifference : kCodecFrameTypeDroppableDifference;
		pos += len;
	}
	return kCodecFrameTypeUnknown;
}

pascal ComponentResult RDNAVC_BeginBand(RDNAVCGlobals *storage, CodecDecompressParams *p,
					ImageSubCodecDecompressRecord *drp, long flags)
{
	RDNAVCDecompressRecord *rec = (RDNAVCDecompressRecord *)drp->userDecompressRecord;

	if (!storage->session)
		return codecErr;
	if (!storage->prepared) {
		int r;

		pthread_mutex_lock(&gLock);
		r = rdnvd_h264_prepare(storage->session);
		pthread_mutex_unlock(&gLock);
		if (r) {
			say("no room for the decoder (%d)", r);
			return codecErr;
		}
		storage->prepared = true;
		if (debug())
			say("prepared: inst %p session %p", (void *)storage, (void *)storage->session);
	}
	rec->width = (**p->imageDescription).width;
	rec->height = (**p->imageDescription).height;
	rec->dataSize = p->bufferSize;
	rec->frame = NULL;
	rec->failed = false;
	/* already decoded: its display was cancelled and it is back */
	rec->decoded = p->frameTime && (p->frameTime->flags & icmFrameAlreadyDecoded) != 0;
	rec->display = p->frameTime ? (long long)p->frameTime->value.lo : -1LL;
	drp->frameType = frame_type((const UInt8 *)drp->codecData, rec->dataSize, storage->nalLengthSize);
	if (debug())
		say("BeginBand: inst %p frame %ld display %lld decode %lld flags 0x%lx type %d size %ld already-decoded %d",
		    (void *)storage,
		    p->frameTime ? (long)p->frameTime->frameNumber : -1L,
		    p->frameTime ? (long long)p->frameTime->value.lo : -1LL,
		    p->frameTime ? (long long)p->frameTime->decodeTime : -1LL,
		    p->frameTime ? (unsigned long)p->frameTime->flags : 0UL, (int)drp->frameType, rec->dataSize,
		    (int)rec->decoded);
	return noErr;
}

pascal ComponentResult RDNAVC_DecodeBand(RDNAVCGlobals *storage, ImageSubCodecDecompressRecord *drp,
					 unsigned long flags)
{
	RDNAVCDecompressRecord *rec = (RDNAVCDecompressRecord *)drp->userDecompressRecord;
	int r;

	pthread_mutex_lock(&gLock);
	r = rdnvd_h264_decode_au(storage->session, (const uint8_t *)drp->codecData, (size_t)rec->dataSize,
				 storage->nalLengthSize, &rec->frame);
	pthread_mutex_unlock(&gLock);
	rec->decoded = true;
	if (debug())
		say("DecodeBand: inst %p result %d, picture %p", (void *)storage, r, (void *)rec->frame);
	if (r) {
		rec->failed = true;
		say("decoding a sample failed (%d)", r);
		return codecBadDataErr;
	}
	return noErr;
}

pascal ComponentResult RDNAVC_DrawBand(RDNAVCGlobals *storage, ImageSubCodecDecompressRecord *drp)
{
	RDNAVCDecompressRecord *rec = (RDNAVCDecompressRecord *)drp->userDecompressRecord;
	ComponentResult err = noErr;

	if (debug())
		say("DrawBand: inst %p decoded %d picture %p", (void *)storage, (int)rec->decoded, (void *)rec->frame);
	if (!rec->decoded)
		err = RDNAVC_DecodeBand(storage, drp, 0);
	if (err)
		return err;
	if (rec->frame && rec->frame->width == (unsigned)rec->width && rec->frame->height == (unsigned)rec->height) {
		int r;

		pthread_mutex_lock(&gLock);
		r = rdnvd_frame_read_2vuy(rec->frame, (uint8_t *)drp->baseAddr, (size_t)drp->rowBytes);
		pthread_mutex_unlock(&gLock);
		if (r) {
			say("reading a picture out failed (%d)", r);
			return codecErr;
		}
		if (debug()) {
			/* what was written: a picture that is all zeros (green) was never decoded; the sum is compared with ffmpeg's
			 * decode of the same stream by scripts/uvd-picsums.py */
			long row, x;
			unsigned long sum = 2166136261UL, zero = 0;

			for (row = 0; row < rec->height; row++) {
				const UInt8 *in = (const UInt8 *)drp->baseAddr + row * drp->rowBytes;

				for (x = 0; x < rec->width * 2; x++) {
					zero += in[x] == 0;
					sum = (sum ^ in[x]) * 16777619UL;
				}
			}
			say("DrawBand: display %lld picture %p sum %08lx zero %lu%% inst %p", rec->display, (void *)rec->frame,
			    sum & 0xffffffffUL, zero * 100 / ((unsigned long)rec->width * 2 * rec->height), (void *)storage);
			/* /tmp/rdnavc.dump: also keep every tenth picture (by display time in 40 ms steps) as a raw 2vuy file */
			if (access("/tmp/rdnavc.dump", F_OK) == 0 && rec->display >= 0 && (rec->display / 40) % 10 == 0) {
				char name[64];
				FILE *f;

				snprintf(name, sizeof(name), "/tmp/avc-%lld.2vuy", rec->display);
				f = fopen(name, "wb");
				if (f) {
					for (row = 0; row < rec->height; row++)
						fwrite((const UInt8 *)drp->baseAddr + row * drp->rowBytes, 1, (size_t)rec->width * 2, f);
					fclose(f);
				}
			}
		}
	} else {
		/* no picture (the sample held none, or it is not one to start from): black */
		long row, x;

		for (row = 0; row < rec->height; row++) {
			UInt32 *out = (UInt32 *)((UInt8 *)drp->baseAddr + row * drp->rowBytes);

			for (x = 0; x < rec->width / 2; x++)
				out[x] = 0x80108010;
		}
	}
	return noErr;
}

pascal ComponentResult RDNAVC_EndBand(RDNAVCGlobals *storage, ImageSubCodecDecompressRecord *drp, OSErr result,
				      long flags)
{
	RDNAVCDecompressRecord *rec = (RDNAVCDecompressRecord *)drp->userDecompressRecord;

	if (debug())
		say("EndBand: inst %p result %d picture %p", (void *)storage, (int)result, (void *)rec->frame);
	if (rec->frame) {
		pthread_mutex_lock(&gLock);
		rdnvd_frame_release(rec->frame);
		pthread_mutex_unlock(&gLock);
		rec->frame = NULL;
	}
	return noErr;
}

pascal ComponentResult RDNAVC_QueueStarting(RDNAVCGlobals *storage)
{
	return noErr;
}

pascal ComponentResult RDNAVC_QueueStopping(RDNAVCGlobals *storage)
{
	return noErr;
}
