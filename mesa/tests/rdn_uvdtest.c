/*
 * rdn_uvdtest: decode an H.264 stream on the card's video decoder through rdnvd and compare every
 * picture with a reference.
 *
 *   rdn_uvdtest stream.h264 reference.yuv [-n count] [-o last.yuv]
 *
 * stream.h264: Annex B; reference.yuv: the same stream decoded by ffmpeg as yuv420p, all the
 * pictures in display order (scripts/uvd-testvec.sh makes both). H.264 decodes bit-exactly, so every
 * sample must be identical. -n repeats the whole stream; -o writes the last pass's pictures.
 *
 * Copyright (c) 2026 kouta-kun and Claude
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/time.h>

#include "rdnvd.h"

static double now_ms(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

static uint8_t *read_file(const char *path, size_t *size)
{
	FILE *f = fopen(path, "rb");
	uint8_t *data;

	if (!f) {
		fprintf(stderr, "uvdtest: cannot read %s\n", path);
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	*size = ftell(f);
	fseek(f, 0, SEEK_SET);
	data = malloc(*size + 1);
	if (data && fread(data, 1, *size, f) != *size) {
		free(data);
		data = NULL;
	}
	fclose(f);
	return data;
}

struct test {
	const uint8_t *ref;
	size_t ref_size;
	FILE *out;
	unsigned pictures, bad_pictures, size_errors;
	unsigned long bad_samples;
	double compare_ms, read_ms;
	unsigned width, height;
};

static void on_frame(void *user, struct rdnvd_frame *frame)
{
	struct test *t = user;
	unsigned w = frame->width, h = frame->height;
	size_t frame_bytes = (size_t)w * h * 3 / 2;
	uint8_t *y = malloc(frame_bytes), *uv = y + (size_t)w * h;
	const uint8_t *ref_y, *ref_u, *ref_v;
	unsigned long bad = 0, first = (unsigned long)-1;
	unsigned x, row;
	double t0 = now_ms();
	int r;

	if (!t->width) {
		t->width = w;
		t->height = h;
	}
	if ((size_t)(t->pictures + 1) * frame_bytes > t->ref_size) {
		if (!t->size_errors++)
			fprintf(stderr, "uvdtest: the reference has too few pictures\n");
		goto done;
	}
	{
		double tr = now_ms();

		r = rdnvd_frame_read(frame, y, w, uv, w);
		t->read_ms += now_ms() - tr;
	}
	if (r) {
		fprintf(stderr, "uvdtest: reading picture %u failed (%d)\n", t->pictures, r);
		t->bad_pictures++;
		goto done;
	}
	ref_y = t->ref + (size_t)t->pictures * frame_bytes;
	ref_u = ref_y + (size_t)w * h;
	ref_v = ref_u + (size_t)w * h / 4;
	for (row = 0; row < h; row++)
		for (x = 0; x < w; x++)
			if (y[row * w + x] != ref_y[row * w + x]) {
				if (first == (unsigned long)-1)
					first = row * w + x;
				bad++;
			}
	for (row = 0; row < h / 2; row++)
		for (x = 0; x < w / 2; x++) {
			bad += uv[row * w + 2 * x] != ref_u[row * (w / 2) + x];
			bad += uv[row * w + 2 * x + 1] != ref_v[row * (w / 2) + x];
		}
	if (bad) {
		if (t->bad_pictures < 8)
			printf("picture %u (display order): %lu samples differ%s\n", t->pictures, bad,
			       first != (unsigned long)-1 ? ", luma too" : ", chroma only");
		t->bad_pictures++;
		t->bad_samples += bad;
	}
	if (t->out)
		fwrite(y, 1, frame_bytes, t->out);
done:
	free(y);
	t->pictures++;
	t->compare_ms += now_ms() - t0;
	rdnvd_frame_release(frame);
}

int main(int argc, char **argv)
{
	const char *stream_path = NULL, *ref_path = NULL, *out_path = NULL;
	unsigned count = 1, pass, nal_length_size = 0, pictures = 0;
	size_t stream_size, ref_size;
	uint8_t *stream, *ref;
	struct rdnvd *d;
	struct test t;
	double t0, total_ms;
	int a, r = 0;

	for (a = 1; a < argc; a++) {
		if (!strcmp(argv[a], "-n") && a + 1 < argc)
			count = atoi(argv[++a]);
		else if (!strcmp(argv[a], "-o") && a + 1 < argc)
			out_path = argv[++a];
		else if (!stream_path)
			stream_path = argv[a];
		else if (!ref_path)
			ref_path = argv[a];
	}
	if (!stream_path || !ref_path) {
		fprintf(stderr, "usage: rdn_uvdtest stream.h264 reference.yuv [-n count] [-o last.yuv]\n");
		return 2;
	}
	stream = read_file(stream_path, &stream_size);
	ref = read_file(ref_path, &ref_size);
	if (!stream || !ref)
		return 1;

	d = rdnvd_open();
	if (!d) {
		fprintf(stderr, "uvdtest: no video decoder (no card, or a kext without the UVD firmware, "
			"or RDN_NO_UVD set)\n");
		return 1;
	}

	memset(&t, 0, sizeof(t));
	t.ref = ref;
	t.ref_size = ref_size;
	t0 = now_ms();
	for (pass = 0; pass < count && !r; pass++) {
		struct rdnvd_session *s = rdnvd_h264_open(d, NULL, 0, on_frame, &t, &nal_length_size);

		if (!s) {
			fprintf(stderr, "uvdtest: cannot open a decoder session\n");
			return 1;
		}
		t.pictures = 0;
		if (pass == count - 1 && out_path)
			t.out = fopen(out_path, "wb");
		r = rdnvd_h264_feed(s, stream, stream_size, 0, 0, 0);
		if (!r)
			r = rdnvd_h264_flush(s, 0);
		if (r)
			fprintf(stderr, "uvdtest: the decoder refused the stream (%d)%s\n", r,
				r == RDNVD_UNSUPPORTED ? ": a feature the card cannot decode" : "");
		rdnvd_h264_close(s);
		if (t.out) {
			fclose(t.out);
			t.out = NULL;
		}
		pictures += t.pictures;
		if (!r && t.width && (size_t)t.pictures * t.width * t.height * 3 / 2 != ref_size) {
			fprintf(stderr, "uvdtest: %u pictures came out, the reference has %lu\n", t.pictures,
				(unsigned long)(ref_size / ((size_t)t.width * t.height * 3 / 2)));
			t.bad_pictures++;
		}
	}
	total_ms = now_ms() - t0;
	printf("%ux%u, %u pictures in %u pass(es): %.1f ms in all; decoding %.2f ms a picture (%.0f a second), "
	       "reading a picture out %.2f ms, comparing it %.2f ms\n",
	       t.width, t.height, pictures, pass, total_ms, pictures ? (total_ms - t.compare_ms) / pictures : 0.0,
	       pictures ? 1000.0 * pictures / (total_ms - t.compare_ms) : 0.0, pictures ? t.read_ms / pictures : 0.0,
	       pictures ? (t.compare_ms - t.read_ms) / pictures : 0.0);
	printf("%s\n", r || t.bad_pictures ? "FAIL" : "PASS: every picture identical to ffmpeg's");
	rdnvd_close(d);
	return r || t.bad_pictures;
}
