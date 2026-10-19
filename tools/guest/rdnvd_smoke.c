/*
 * rdnvd_smoke: the smallest consumer of librdnvd.dylib: decode an Annex B file and count the pictures,
 * writing the first one as a PGM (luma). Builds on the G5 itself:
 *   gcc -I<repo>/mesa/target -o rdnvd_smoke rdnvd_smoke.c -L. -lrdnvd
 *   DYLD_LIBRARY_PATH=. ./rdnvd_smoke stream.h264 [first.pgm]
 *
 * Copyright (c) 2026 kouta-kun and Claude
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdnvd.h"

static unsigned pictures;
static const char *pgm_path;

static void on_frame(void *user, struct rdnvd_frame *f)
{
	if (!pictures && pgm_path) {
		uint8_t *y = malloc((size_t)f->width * f->height), *uv = malloc((size_t)f->width * f->height / 2 + 2);
		FILE *o = fopen(pgm_path, "wb");

		if (y && uv && o && !rdnvd_frame_read(f, y, f->width, uv, f->width)) {
			fprintf(o, "P5\n%u %u\n255\n", f->width, f->height);
			fwrite(y, 1, (size_t)f->width * f->height, o);
		}
		if (o)
			fclose(o);
		free(y);
		free(uv);
	}
	pictures++;
	rdnvd_frame_release(f);
}

int main(int argc, char **argv)
{
	FILE *f;
	uint8_t *data;
	long size;
	struct rdnvd *d;
	struct rdnvd_session *s;
	unsigned nal = 0;
	int r;

	if (argc < 2 || !(f = fopen(argv[1], "rb")))
		return 2;
	pgm_path = argc > 2 ? argv[2] : NULL;
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	data = malloc(size);
	if (!data || fread(data, 1, size, f) != (size_t)size)
		return 1;
	fclose(f);
	d = rdnvd_open();
	if (!d) {
		fprintf(stderr, "no decoder\n");
		return 1;
	}
	s = rdnvd_h264_open(d, NULL, 0, on_frame, NULL, &nal);
	r = rdnvd_h264_feed(s, data, size, 0, 0, 0);
	if (!r)
		r = rdnvd_h264_flush(s, 0);
	rdnvd_h264_close(s);
	rdnvd_close(d);
	printf("%u pictures, result %d\n", pictures, r);
	return r != 0;
}
