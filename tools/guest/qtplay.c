/*
 * qtplay: play an H.264 movie in real time the way QuickTime Player's own pipeline does it (a movie with a
 * visual context: pixel buffers by movie time, decoded ahead by the movie's own scheduling, on QuickTime's
 * threads) and check every picture that comes out against ffmpeg's decode of the same stream. Nothing is shown
 * on the screen. Pictures QuickTime does not ask for in time are simply not compared.
 *
 *   qtplay movie.mp4 reference.yuv|- [fps] [-s seconds] [-v]
 *
 * reference.yuv is yuv420p at the movie's size, one picture per 1/fps second (25 by default); the picture at
 * movie time t is compared with number round(t * fps). The decompressor is whatever QuickTime chooses, so it
 * tests what an installed component does to a program that knows nothing of it. QuickTime converts the pixel
 * buffers it hands out, so the comparison with the reference is not exact; -v lists every picture shown with how
 * many of its bytes are zero (a picture the decoder never wrote is all zeros: green) and a checksum, which is
 * enough to see by eye whether a static clip stays the same picture; "-" as the reference does only that. Builds
 * on the G5:
 *   gcc-4.0 -arch ppc -isysroot /Developer/SDKs/MacOSX10.4u.sdk -mmacosx-version-min=10.4 -o qtplay qtplay.c \
 *       -framework QuickTime -framework CoreServices -framework CoreVideo -framework CoreFoundation
 *
 * Copyright (c) 2026 kouta-kun and Claude
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/time.h>
#include <unistd.h>
#include <QuickTime/QuickTime.h>
#include <CoreServices/CoreServices.h>
#include <CoreVideo/CoreVideo.h>

static double now_s(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return tv.tv_sec + tv.tv_usec / 1e6;
}

int main(int argc, char **argv)
{
	const char *movie_path = NULL, *ref_path = NULL;
	double fps = 25, limit = 0;
	int a, verbose = 0;
	FILE *f;
	long size;
	unsigned char *ref;
	Movie movie = NULL;
	Handle dataRef = NULL;
	OSType dataRefType;
	short resId = 0;
	OSStatus err;
	CFStringRef path;
	QTVisualContextRef ctx = NULL;
	CFMutableDictionaryRef attrs, pbattrs;
	unsigned w, h, shown = 0, bad = 0, compared = 0, skipped = 0, wrong_format = 0;
	double t0, last_time = -1;
	TimeScale scale;
	size_t frame_bytes;
	Track track;
	Media media;
	Handle sd;
	TimeValue duration;

	for (a = 1; a < argc; a++) {
		if (!strcmp(argv[a], "-s") && a + 1 < argc)
			limit = atof(argv[++a]);
		else if (!strcmp(argv[a], "-v"))
			verbose = 1;
		else if (!movie_path)
			movie_path = argv[a];
		else if (!ref_path)
			ref_path = argv[a];
		else
			fps = atof(argv[a]);
	}
	if (!movie_path || !ref_path) {
		fprintf(stderr, "usage: qtplay movie.mp4 reference.yuv [fps] [-s seconds]\n");
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
	} else {
		ref = NULL;
		size = 0;
	}

	if (EnterMovies())
		return 1;
	path = CFStringCreateWithCString(NULL, movie_path, kCFStringEncodingUTF8);
	err = QTNewDataReferenceFromFullPathCFString(path, kQTNativeDefaultPathStyle, 0, &dataRef, &dataRefType);
	if (!err)
		err = NewMovieFromDataRef(&movie, newMovieActive, &resId, dataRef, dataRefType);
	if (err || !movie) {
		fprintf(stderr, "cannot open %s (%ld)\n", movie_path, (long)err);
		return 1;
	}
	track = GetMovieIndTrackType(movie, 1, VideoMediaType, movieTrackMediaType);
	media = GetTrackMedia(track);
	sd = NewHandle(0);
	GetMediaSampleDescription(media, 1, (SampleDescriptionHandle)sd);
	w = (**(ImageDescriptionHandle)sd).width;
	h = (**(ImageDescriptionHandle)sd).height;
	frame_bytes = (size_t)w * h * 3 / 2;
	scale = GetMovieTimeScale(movie);
	duration = GetMovieDuration(movie);
	printf("%ux%u, %.2f s, %lu pictures in the reference\n", w, h, (double)duration / scale,
	       (unsigned long)(size / frame_bytes));
	if (verbose && !ref)
		printf("(no reference: listing only)\n");

	attrs = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	pbattrs = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	{
		SInt32 fmt = k422YpCbCr8PixelFormat, ww = w, hh = h;
		CFNumberRef nf = CFNumberCreate(NULL, kCFNumberSInt32Type, &fmt);
		CFNumberRef nw = CFNumberCreate(NULL, kCFNumberSInt32Type, &ww);
		CFNumberRef nh = CFNumberCreate(NULL, kCFNumberSInt32Type, &hh);

		CFDictionarySetValue(pbattrs, kCVPixelBufferPixelFormatTypeKey, nf);
		CFDictionarySetValue(pbattrs, kCVPixelBufferWidthKey, nw);
		CFDictionarySetValue(pbattrs, kCVPixelBufferHeightKey, nh);
		CFDictionarySetValue(attrs, kQTVisualContextPixelBufferAttributesKey, pbattrs);
	}
	err = QTPixelBufferContextCreate(NULL, attrs, &ctx);
	if (err || !ctx) {
		fprintf(stderr, "QTPixelBufferContextCreate: %ld\n", (long)err);
		return 1;
	}
	err = SetMovieVisualContext(movie, ctx);
	if (err) {
		fprintf(stderr, "SetMovieVisualContext: %ld\n", (long)err);
		return 1;
	}
	GoToBeginningOfMovie(movie);
	SetMovieRate(movie, 0x00010000);
	StartMovie(movie);
	t0 = now_s();
	while (!IsMovieDone(movie) && (limit <= 0 || now_s() - t0 < limit)) {
		MoviesTask(movie, 0);
		QTVisualContextTask(ctx);
		if (QTVisualContextIsNewImageAvailable(ctx, NULL)) {
			CVImageBufferRef img = NULL;
			CFDictionaryRef tv;
			double t = -1;

			QTVisualContextCopyImageForTime(ctx, NULL, NULL, &img);
			if (!img)
				continue;
			tv = CVBufferGetAttachment(img, kCVBufferMovieTimeKey, NULL);
			if (tv) {
				CFNumberRef v = CFDictionaryGetValue(tv, kCVBufferTimeValueKey);
				CFNumberRef s = CFDictionaryGetValue(tv, kCVBufferTimeScaleKey);
				SInt64 value = 0;
				SInt32 sc = 1;

				if (v && s) {
					CFNumberGetValue(v, kCFNumberSInt64Type, &value);
					CFNumberGetValue(s, kCFNumberSInt32Type, &sc);
					t = (double)value / sc;
				}
			}
			shown++;
			if (verbose && CVPixelBufferGetPixelFormatType(img) == k422YpCbCr8PixelFormat) {
				const unsigned char *base;
				size_t rb = CVPixelBufferGetBytesPerRow(img), nz = 0, tot = 0, row, x;
				unsigned long sum = 0;

				CVPixelBufferLockBaseAddress(img, 0);
				base = CVPixelBufferGetBaseAddress(img);
				for (row = 0; row < CVPixelBufferGetHeight(img); row++)
					for (x = 0; x < CVPixelBufferGetWidth(img) * 2; x++, tot++) {
						unsigned char c = base[row * rb + x];

						nz += c == 0;
						sum = sum * 31 + c;
					}
				CVPixelBufferUnlockBaseAddress(img, 0);
				printf("t %.3f  zero bytes %3lu%%  sum %08lx\n", t, (unsigned long)(nz * 100 / (tot ? tot : 1)), sum);
			}
			if (!ref) {
				/* nothing to compare with */
			} else if (CVPixelBufferGetPixelFormatType(img) != k422YpCbCr8PixelFormat) {
				wrong_format++;
			} else if (t >= 0) {
				long n = (long)floor(t * fps + 0.5);
				unsigned long bad_y = 0, bad_c = 0;
				unsigned row, x;

				if (t == last_time)
					skipped++;
				last_time = t;
				if (n >= 0 && (size_t)(n + 1) * frame_bytes <= (size_t)size &&
				    CVPixelBufferGetWidth(img) == w && CVPixelBufferGetHeight(img) == h) {
					const unsigned char *ry = ref + (size_t)n * frame_bytes, *ru = ry + (size_t)w * h,
							    *rv = ru + (size_t)w * h / 4;
					const unsigned char *base;
					size_t rb;

					CVPixelBufferLockBaseAddress(img, 0);
					base = CVPixelBufferGetBaseAddress(img);
					rb = CVPixelBufferGetBytesPerRow(img);
					for (row = 0; row < h; row++) {
						const unsigned char *p = base + row * rb;

						for (x = 0; x < w / 2; x++) {
							unsigned u = p[4 * x], y0 = p[4 * x + 1], v = p[4 * x + 2], y1 = p[4 * x + 3];

							bad_y += y0 != ry[row * w + 2 * x] || y1 != ry[row * w + 2 * x + 1];
							bad_c += u != ru[(row / 2) * (w / 2) + x] || v != rv[(row / 2) * (w / 2) + x];
						}
					}
					CVPixelBufferUnlockBaseAddress(img, 0);
					compared++;
					if (bad_y || bad_c) {
						if (bad < 6)
							printf("movie time %.3f s (picture %ld): luma pairs %lu, chroma pairs %lu differ\n", t, n,
							       bad_y, bad_c);
						bad++;
					}
				}
			}
			CVBufferRelease(img);
		}
		usleep(1000);
	}
	printf("%u pictures shown in %.2f s (%u compared, %u wrong, %u wrong format)\n", shown, now_s() - t0, compared,
	       bad, wrong_format);
	if (!ref)
		return 0;
	printf("%s\n", bad || wrong_format || !compared ? "FAIL" : "PASS: every picture shown is identical to ffmpeg's");
	return bad != 0 || wrong_format != 0 || !compared;
}
