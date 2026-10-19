/*
 * qtdecode: decode the first video track of an H.264 movie through QuickTime's Image Compression Manager
 * (ICMDecompressionSession, as QuickTime Player's own decoding goes) into packed 4:2:2 ('2vuy') pixel
 * buffers and compare every picture, in display order, with ffmpeg's decode of the same stream (yuv420p).
 * Nothing is shown on the screen.
 *
 *   [QTDECODE_PAUSE=seconds [QTDECODE_PAUSE_AFTER=samples]] qtdecode movie.mp4 reference.yuv|- [-c RadeonNIAVC.component] [-a] [-t]  (- : no reference, only count and time;
 *   -t: the frames are scheduled against a time base that runs in real time, as when a movie plays: pictures come late
 *   or not at all, each is checked by its own display time)
 *
 * -c  registers that component for this program only (nothing is installed) and makes the session use it;
 * -a  makes it use Apple's own H.264 decoder (a control: the program's own checks must pass with it too);
 * neither: QuickTime's choice, so what an installed component does to a program that knows nothing of it.
 * Chroma of 4:2:2 is compared with the 4:2:0 reference at its own rows (the two luma rows a chroma row
 * serves must carry the same chroma: that is how the component draws).
 * Builds on the G5 (its Xcode):
 *   gcc-4.0 -arch ppc -isysroot /Developer/SDKs/MacOSX10.4u.sdk -mmacosx-version-min=10.4 -o qtdecode qtdecode.c \
 *       -framework QuickTime -framework CoreServices -framework CoreVideo -framework CoreFoundation
 *
 * Copyright (c) 2026 kouta-kun and Claude
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include <QuickTime/QuickTime.h>
#include <CoreServices/CoreServices.h>
#include <CoreVideo/CoreVideo.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>

static const unsigned char *gRef;
static size_t gRefSize;
static unsigned gFrames, gBad, gWidth, gHeight;
static unsigned gErrors;
static int gNoRef, gSched;
static TimeValue64 *gSorted;
static unsigned gNSorted;
static double gT0;

static double now_ms(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

static void emit(void *refcon, OSStatus result, ICMDecompressionTrackingFlags flags, CVPixelBufferRef pb,
		 TimeValue64 displayTime, TimeValue64 displayDuration, ICMValidTimeFlags validTime, void *reserved,
		 void *sourceFrameRefCon)
{
	/* the source data is not needed any more */
	if ((flags & kICMDecompressionTracking_ReleaseSourceData) && sourceFrameRefCon)
		free(sourceFrameRefCon);
	if (getenv("QTDECODE_TRACE"))
		fprintf(stderr, "tracking: result %ld flags 0x%lx pb %p time %lld valid 0x%lx\n", (long)result,
			(unsigned long)flags, (void *)pb, (long long)displayTime, (unsigned long)validTime);
	if (result) {
		gErrors++;
		fprintf(stderr, "tracking callback: error %ld\n", (long)result);
		return;
	}
	if ((flags & kICMDecompressionTracking_EmittingFrame) && pb) {
		unsigned w = gWidth, h = gHeight, x, row;
		unsigned index = gFrames;
		size_t frame_bytes = (size_t)w * h * 3 / 2;
		const unsigned char *ry, *ru, *rv;
		unsigned long bad = 0, bad_y = 0, bad_c = 0, bad_r = 0;
		OSType fmt = CVPixelBufferGetPixelFormatType(pb);

		if (gSched) {
			/* scheduled playback: pictures may be late or dropped, so the picture's own time says which one it is */
			unsigned lo = 0, hi = gNSorted;

			while (lo < hi) {
				unsigned mid = (lo + hi) / 2;

				if (gSorted[mid] < displayTime)
					lo = mid + 1;
				else
					hi = mid;
			}
			if (lo >= gNSorted || gSorted[lo] != displayTime) {
				gBad++;
				gFrames++;
				return;
			}
			index = lo;
		}
		if (gNoRef) {
			gFrames++;
			return;
		}
		if ((size_t)(index + 1) * frame_bytes > gRefSize) {
			gFrames++;
			gBad++;
			return;
		}
		ry = gRef + (size_t)index * frame_bytes;
		ru = ry + (size_t)w * h;
		rv = ru + (size_t)w * h / 4;
		if (getenv("QTDECODE_NOATTRS") && gFrames == 0)
			fprintf(stderr, "emitted: format %.4s (0x%lx) %lux%lu\n", (char *)&fmt, (unsigned long)fmt,
				(unsigned long)CVPixelBufferGetWidth(pb), (unsigned long)CVPixelBufferGetHeight(pb));
		if (fmt != k422YpCbCr8PixelFormat || CVPixelBufferGetWidth(pb) != w || CVPixelBufferGetHeight(pb) != h) {
			fprintf(stderr, "picture %u: pixel format %.4s %lux%lu\n", gFrames, (char *)&fmt,
				(unsigned long)CVPixelBufferGetWidth(pb), (unsigned long)CVPixelBufferGetHeight(pb));
			gBad++;
			gFrames++;
			return;
		}
		CVPixelBufferLockBaseAddress(pb, 0);
		{
			const unsigned char *base = CVPixelBufferGetBaseAddress(pb);
			size_t rb = CVPixelBufferGetBytesPerRow(pb);

			for (row = 0; row < h; row++) {
				const unsigned char *p = base + row * rb;
				const unsigned char *pe = base + (row & ~1u) * rb;	/* the even row of its pair */

				for (x = 0; x < w / 2; x++) {
					unsigned u = p[4 * x], y0 = p[4 * x + 1], v = p[4 * x + 2], y1 = p[4 * x + 3];

					bad_y += y0 != ry[row * w + 2 * x] || y1 != ry[row * w + 2 * x + 1];
					bad_c += u != ru[(row / 2) * (w / 2) + x] || v != rv[(row / 2) * (w / 2) + x];
					bad_r += pe[4 * x] != u || pe[4 * x + 2] != v;	/* the pair of rows alike */
				}
			}
		}
		if (getenv("QTDECODE_DUMP") && gFrames == 0) {
			const unsigned char *base = CVPixelBufferGetBaseAddress(pb);

			fprintf(stderr, "bytes per row %lu; first 16 bytes of row 0:", (unsigned long)CVPixelBufferGetBytesPerRow(pb));
			for (x = 0; x < 16; x++)
				fprintf(stderr, " %02x", base[x]);
			fprintf(stderr, "\n row 0 of the reference (luma):");
			for (x = 0; x < 8; x++)
				fprintf(stderr, " %02x", ry[x]);
			fprintf(stderr, "; u:");
			for (x = 0; x < 4; x++)
				fprintf(stderr, " %02x", ru[x]);
			fprintf(stderr, "; v:");
			for (x = 0; x < 4; x++)
				fprintf(stderr, " %02x", rv[x]);
			fprintf(stderr, "\n");
		}
		CVPixelBufferUnlockBaseAddress(pb, 0);
		bad = bad_y + bad_c + bad_r;
		if (bad) {
			if (gBad < 8)
				printf("picture %u (display time %lld): luma pairs %lu, chroma pairs %lu, chroma rows unlike %lu "
				       "(of %u pairs)\n", index, (long long)displayTime, bad_y, bad_c, bad_r, w / 2 * h);
			gBad++;
		}
		gFrames++;
	}
}

int main(int argc, char **argv)
{
	const char *movie_path = NULL, *ref_path = NULL, *component_path = NULL;
	int force_apple = 0, a;
	FILE *f;
	long size;
	unsigned char *ref;
	Movie movie = NULL;
	Track track;
	Media media;
	Handle dataRef = NULL, sd;
	OSType dataRefType;
	short resId = 0;
	OSStatus err;
	ICMDecompressionSessionRef session = NULL;
	ICMDecompressionSessionOptionsRef options = NULL;
	ICMDecompressionTrackingCallbackRecord cb = { emit, NULL };
	CFMutableDictionaryRef attrs;
	TimeValue64 decodeTime = 0, mediaDuration;
	TimeScale scale;
	ItemCount sdIndex, count;
	unsigned char *buffer;
	ByteCount maxSize = 4 << 20;
	unsigned samples = 0, i, j;
	CFStringRef path;
	TimeValue64 *times = calloc(65536, sizeof(*times)), *sorted = calloc(65536, sizeof(*sorted));
	unsigned nTimes = 0, next = 0;
	TimeBase timebase = NULL;

	for (a = 1; a < argc; a++) {
		if (!strcmp(argv[a], "-c") && a + 1 < argc)
			component_path = argv[++a];
		else if (!strcmp(argv[a], "-a"))
			force_apple = 1;
		else if (!strcmp(argv[a], "-t"))
			gSched = 1;
		else if (!movie_path)
			movie_path = argv[a];
		else if (!ref_path)
			ref_path = argv[a];
	}
	if (!movie_path || !ref_path) {
		fprintf(stderr, "usage: qtdecode movie.mp4 reference.yuv [-c component] [-a]\n");
		return 2;
	}
	if (strcmp(ref_path, "-")) {
		f = fopen(ref_path, "rb");
		if (!f)
			return 1;
		fseek(f, 0, SEEK_END);
		size = ftell(f);
		fseek(f, 0, SEEK_SET);
		ref = malloc(size);
		if (!ref || fread(ref, 1, size, f) != (size_t)size)
			return 1;
		fclose(f);
		gRef = ref;
		gRefSize = size;
	} else {
		gNoRef = 1;	/* only count the pictures and time them */
	}

	err = EnterMovies();
	if (err) {
		fprintf(stderr, "EnterMovies: %ld\n", (long)err);
		return 1;
	}

	if (component_path) {
		FSRef fsref;

		if (FSPathMakeRef((const UInt8 *)component_path, &fsref, NULL) ||
		    (err = RegisterComponentFileRef(&fsref, 0)) < 0) {
			fprintf(stderr, "cannot register %s (%ld)\n", component_path, (long)err);
			return 1;
		}
		printf("RegisterComponentFileRef: %ld\n", (long)err);
	}

	path = CFStringCreateWithCString(NULL, movie_path, kCFStringEncodingUTF8);
	err = QTNewDataReferenceFromFullPathCFString(path, kQTNativeDefaultPathStyle, 0, &dataRef, &dataRefType);
	if (!err)
		err = NewMovieFromDataRef(&movie, newMovieActive, &resId, dataRef, dataRefType);
	if (err || !movie) {
		fprintf(stderr, "cannot open %s (%ld)\n", movie_path, (long)err);
		return 1;
	}
	track = GetMovieIndTrackType(movie, 1, VideoMediaType, movieTrackMediaType);
	if (!track) {
		fprintf(stderr, "no video track\n");
		return 1;
	}
	media = GetTrackMedia(track);
	scale = GetMediaTimeScale(media);
	mediaDuration = GetMediaDuration(media);
	sd = NewHandle(0);
	GetMediaSampleDescription(media, 1, (SampleDescriptionHandle)sd);
	{
		ImageDescriptionHandle desc = (ImageDescriptionHandle)sd;

		gWidth = (**desc).width;
		gHeight = (**desc).height;
		printf("video track: '%.4s' %ux%u, %lu samples\n", (char *)&(**desc).cType, gWidth, gHeight,
		       (unsigned long)GetMediaSampleCount(media));
	}

	ICMDecompressionSessionOptionsCreate(NULL, &options);
	{
		Boolean order = true;

		ICMDecompressionSessionOptionsSetProperty(options, kQTPropertyClass_ICMDecompressionSessionOptions,
							  kICMDecompressionSessionOptionsPropertyID_DisplayOrderRequired,
							  sizeof(order), &order);
	}
	{
		/* Apple's decoder skips work (the loop filter on some pictures) at lower accuracy: ask for all of it */
		CodecQ accuracy = codecMaxQuality;

		ICMDecompressionSessionOptionsSetProperty(options, kQTPropertyClass_ICMDecompressionSessionOptions,
							  kICMDecompressionSessionOptionsPropertyID_Accuracy,
							  sizeof(accuracy), &accuracy);
	}
	if (component_path || force_apple) {
		ComponentDescription cd = { decompressorComponentType, 'avc1', component_path ? 'RdNI' : 'appl', 0, 0 };
		Component c = FindNextComponent(NULL, &cd);

		if (!c) {
			fprintf(stderr, "no such decompressor\n");
			return 1;
		}
		ICMDecompressionSessionOptionsSetProperty(options, kQTPropertyClass_ICMDecompressionSessionOptions,
							  kICMDecompressionSessionOptionsPropertyID_DecompressorComponent,
							  sizeof(c), &c);
	}
	attrs = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	{
		SInt32 fmt = k422YpCbCr8PixelFormat, w = gWidth, h = gHeight;
		CFNumberRef nf = CFNumberCreate(NULL, kCFNumberSInt32Type, &fmt);
		CFNumberRef nw = CFNumberCreate(NULL, kCFNumberSInt32Type, &w);
		CFNumberRef nh = CFNumberCreate(NULL, kCFNumberSInt32Type, &h);

		CFDictionarySetValue(attrs, kCVPixelBufferPixelFormatTypeKey, nf);
		CFDictionarySetValue(attrs, kCVPixelBufferWidthKey, nw);
		CFDictionarySetValue(attrs, kCVPixelBufferHeightKey, nh);
	}
	if (getenv("QTDECODE_NOATTRS")) {
		CFRelease(attrs);
		attrs = NULL;
	}
	err = ICMDecompressionSessionCreate(NULL, (ImageDescriptionHandle)sd, options, attrs, &cb, &session);
	if (err) {
		fprintf(stderr, "ICMDecompressionSessionCreate: %ld\n", (long)err);
		return 1;
	}

	buffer = malloc(maxSize);
	/* A first pass over the sample table for the display times, so that pictures can be asked for as soon as they can be shown. */
	{
		TimeValue64 t = 0;

		while (t < mediaDuration && nTimes < 65536) {
			TimeValue64 st = 0, du = 0, of = 0;
			ByteCount sz = 0;
			ItemCount k = 0, idx = 0;
			MediaSampleFlags fl = 0;

			if (GetMediaSample2(media, NULL, 0, &sz, t, &st, &du, &of, NULL, &idx, 1, &k, &fl) || !k)
				break;
			sorted[nTimes++] = st + of;
			t = st + du;
		}
		for (i = 1; i < nTimes; i++) {
			TimeValue64 v = sorted[i];

			for (j = i; j > 0 && sorted[j - 1] > v; j--)
				sorted[j] = sorted[j - 1];
			sorted[j] = v;
		}
		gSorted = sorted;
		gNSorted = nTimes;
		if (gSched) {
			/* the display times the callback reports are those given, 0.3 s later */
			for (i = 0; i < nTimes; i++)
				sorted[i] += scale * 3 / 10;
			timebase = NewTimeBase();
			SetTimeBaseRate(timebase, 0);
			SetTimeBaseValue(timebase, 0, scale);
		}
	}
	if (getenv("QTDECODE_GL")) {
		/* QuickTime Player has an OpenGL context (its window) in the same program as the decoder: make one */
		CGLPixelFormatAttribute attrs[] = { kCGLPFAAccelerated, kCGLPFAColorSize, (CGLPixelFormatAttribute)24, (CGLPixelFormatAttribute)0 };
		CGLPixelFormatObj pix = NULL;
		CGLContextObj ctx = NULL;
		GLint npix = 0;
		CGLError ge = CGLChoosePixelFormat(attrs, &pix, &npix);

		if (!ge && pix)
			ge = CGLCreateContext(pix, NULL, &ctx);
		if (!ge && ctx) {
			CGLSetCurrentContext(ctx);
			glClearColor(0.2f, 0.4f, 0.6f, 1.0f);
			glClear(GL_COLOR_BUFFER_BIT);
			glFinish();
			printf("OpenGL context made: %s\n", (const char *)glGetString(GL_RENDERER));
		} else {
			printf("OpenGL context failed (%d)\n", (int)ge);
		}
	}
	gT0 = now_ms();
	while (decodeTime < mediaDuration) {
		ByteCount got = 0;
		TimeValue64 sampleTime = 0, duration = 0, offset = 0;
		MediaSampleFlags sflags = 0;
		ICMFrameTimeRecord ft;
		ItemCount n = 0;

		err = GetMediaSample2(media, buffer, maxSize, &got, decodeTime, &sampleTime, &duration, &offset,
				      (SampleDescriptionHandle)sd, &sdIndex, 1, &n, &sflags);
		if (err || !n || !got)
			break;
		memset(&ft, 0, sizeof(ft));
		ft.recordSize = sizeof(ft);
		ft.scale = scale;
		ft.rate = 0x00010000;
		ft.duration = (long)duration;
		ft.frameNumber = (long)samples + 1;
		ft.value.hi = (SInt32)((sampleTime + offset) >> 32);
		ft.value.lo = (UInt32)(sampleTime + offset);
		ft.decodeTime = sampleTime;
		ft.flags = icmFrameTimeIsNonScheduledDisplayTime | icmFrameTimeHasDecodeTime;
		if (gSched) {
			ft.flags = icmFrameTimeHasDecodeTime;
			ft.base = timebase;
			/* shown 0.3 s after the time base starts, so that the first pictures are not late */
			ft.value.lo = (UInt32)(sampleTime + offset + scale * 3 / 10);
			ft.value.hi = 0;
		}
		if (getenv("QTDECODE_NOFLAGS"))
			ft.flags = 0;
		if (getenv("QTDECODE_NSONLY"))
			ft.flags = icmFrameTimeIsNonScheduledDisplayTime;
		/* the session reads the data later, when it decodes: every sample has its own buffer, kept until the session lets go */
		{
			unsigned char *own = malloc(got);

			memcpy(own, buffer, got);
			err = ICMDecompressionSessionDecodeFrame(session, own, got, NULL, &ft, own);
		}
		if (err) {
			fprintf(stderr, "decode frame %u: %ld\n", samples, (long)err);
			gErrors++;
		}
		if (samples < 65536)
			times[samples] = sampleTime + offset;
		samples++;
		decodeTime = sampleTime + duration;
		/* a picture can be shown once every picture that is displayed before it has been queued */
		while (next < nTimes && !getenv("QTDECODE_NOFLAGS")) {
			unsigned q;
			int queued = 0;

			for (q = 0; q < samples; q++)
				if (times[q] == sorted[next])
					queued = 1;
			if (!queued)
				break;
			ICMDecompressionSessionSetNonScheduledDisplayTime(session, sorted[next], scale, 0);
			next++;
		}
		/* QTDECODE_PAUSE=seconds[,after]: stop feeding for a while after that many samples (QuickTime Player decodes the first
		 * picture when a movie opens and the next ones when the user presses play) */
		if (getenv("QTDECODE_PAUSE") && samples == (getenv("QTDECODE_PAUSE_AFTER") ? (unsigned)atoi(getenv("QTDECODE_PAUSE_AFTER")) : 1)) {
			printf("pausing %d s after %u samples\n", atoi(getenv("QTDECODE_PAUSE")), samples);
			fflush(stdout);
			sleep(atoi(getenv("QTDECODE_PAUSE")));
		}
	}
	if (gSched) {
		/* play: the time base runs for as long as the movie, and a second more */
		TimeValue end = (TimeValue)(sorted[nTimes - 1] + scale), tbnow = 0;
		double start;

		SetTimeBaseRate(timebase, 0x00010000);
		start = now_ms();
		while (tbnow < end && now_ms() - start < 120000.0) {
			tbnow = GetTimeBaseTime(timebase, scale, NULL);
			usleep(2000);
		}
	} else {
		for (; next < nTimes && !getenv("QTDECODE_NOFLAGS"); next++)
			ICMDecompressionSessionSetNonScheduledDisplayTime(session, sorted[next], scale, 0);
	}
	ICMDecompressionSessionFlush(session);
	if (gSched)
		gErrors = 0;	/* the flush reports the pictures that were never shown as errors */
	printf("%u samples fed, %u pictures out, %u wrong, %u errors, %.1f ms (%.2f ms a sample)\n", samples, gFrames,
	       gBad, gErrors, now_ms() - gT0, samples ? (now_ms() - gT0) / samples : 0.0);
	if (gSched)
		printf("%u of %u pictures were shown in time\n", gFrames, samples);
	printf("%s\n", gBad || gErrors || (!gSched && gFrames != samples) ? "FAIL" : "PASS: every picture identical to ffmpeg's");
	ICMDecompressionSessionRelease(session);
	return gBad || gErrors || (!gSched && gFrames != samples);
}
