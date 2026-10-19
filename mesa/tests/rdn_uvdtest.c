/*
 * rdn_uvdtest: decode an H.264 stream on the card's video decoder through rdnvd and compare every
 * picture with a reference.
 *
 *   rdn_uvdtest stream.h264 reference.yuv [-n count] [-o last.yuv]
 *
 * stream.h264: Annex B; reference.yuv: the same stream decoded by ffmpeg as yuv420p, all the
 * pictures in display order (scripts/uvd-testvec.sh makes both). H.264 decodes bit-exactly, so every
 * sample must be identical. -n repeats the whole stream; -o writes the last pass's pictures. -L n opens n more
 * decoder sessions first and leaves them open when the program ends; -R n asks n times whether the card has room for a
 * 1280x720 decoder (what the QuickTime component does when a program opens a movie: each asks takes the memory and gives it back).
 * -A decodes one access unit at a time with rdnvd_h264_decode_au and reads each picture out as it is decoded, as the QuickTime
 * component does (pictures come in decoding order and are compared with the reference by their order of appearance in the stream's
 * display order, so only streams without B pictures can be checked: the result then says how many came out right); -P ms waits that long
 * after every access unit and prints the time of day and the verdict of each (one session for a long time, to watch it while other things
 * happen on the machine); -g ms waits that
 * long after the first access unit (QuickTime Player decodes a movie's first picture when it opens it, and the rest when play is pressed) (what QuickTime programs do: the card's
 * firmware holds ten sessions, and the kext starts the block again when the last program that used it is gone).
 *
 * Copyright (c) 2026 kouta-kun and Claude
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

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
	int leak = 0, room = 0, per_au = 0, gap_ms = 0, pace_ms = 0;
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
		else if (!strcmp(argv[a], "-L") && a + 1 < argc)
			leak = atoi(argv[++a]);
		else if (!strcmp(argv[a], "-R") && a + 1 < argc)
			room = atoi(argv[++a]);
		else if (!strcmp(argv[a], "-A"))
			per_au = 1;
		else if (!strcmp(argv[a], "-g") && a + 1 < argc)
			gap_ms = atoi(argv[++a]);
		else if (!strcmp(argv[a], "-P") && a + 1 < argc)
			pace_ms = atoi(argv[++a]);
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
	/* sessions that are made, decode a picture and are never destroyed */
	for (a = 0; a < leak; a++) {
		struct rdnvd_session *s = rdnvd_h264_open(d, NULL, 0, NULL, NULL, &nal_length_size);
		struct rdnvd_frame *frame = NULL;

		if (!s || rdnvd_h264_decode_au(s, stream, stream_size, 0, &frame))
			fprintf(stderr, "uvdtest: leaked session %d did not decode\n", a);
		if (frame)
			rdnvd_frame_release(frame);
	}
	if (leak)
		printf("%d sessions left open\n", leak);
	for (a = 0; a < room; a++)
		if (!rdnvd_h264_room(d, 1280, 720))
			fprintf(stderr, "uvdtest: no room for a 1280x720 decoder (ask %d)\n", a);
	if (per_au) {
		/* Annex B split into access units: one slice NAL that starts a picture (first_mb_in_slice 0) ends the one before */
		struct rdnvd_session *s = rdnvd_h264_open(d, NULL, 0, NULL, NULL, &nal_length_size);
		size_t pos = 0, au_start = 0;
		unsigned au = 0, right = 0, wrong = 0, nonzero = 0;
		size_t frame_bytes = 0;

		if (!s) {
			fprintf(stderr, "uvdtest: cannot open a decoder session\n");
			return 1;
		}
		/* NAL starts */
		while (pos + 4 < stream_size) {
			size_t next = pos + 3;
			size_t nal_at;
			int starts_picture = 0;
			struct rdnvd_frame *frame = NULL;

			/* find the start code at pos (00 00 01 or 00 00 00 01) */
			if (!(stream[pos] == 0 && stream[pos + 1] == 0 && (stream[pos + 2] == 1 || (stream[pos + 2] == 0 && stream[pos + 3] == 1)))) {
				pos++;
				continue;
			}
			nal_at = pos + (stream[pos + 2] == 1 ? 3 : 4);
			{
				unsigned type = stream[nal_at] & 0x1f;

				starts_picture = (type == 1 || type == 5) && (stream[nal_at + 1] & 0x80);
			}
			/* where this NAL ends: the next start code */
			next = nal_at + 1;
			while (next + 3 < stream_size && !(stream[next] == 0 && stream[next + 1] == 0 && (stream[next + 2] == 1 || (stream[next + 2] == 0 && stream[next + 3] == 1))))
				next++;
			if (next + 3 >= stream_size)
				next = stream_size;
			/* a slice that starts a picture is its own access unit with whatever parameter sets came before it */
			if (starts_picture) {
				size_t len = next - au_start;
				double tdec = now_ms();

				r = rdnvd_h264_decode_au(s, stream + au_start, len, 0, &frame);
				if (r)
					fprintf(stderr, "uvdtest: decoding access unit %u failed (%d)\n", au, r);
				if (frame) {
					unsigned w = frame->width, h = frame->height;
					uint8_t *y, *uv;
					unsigned i, zeros = 0;

					frame_bytes = (size_t)w * h * 3 / 2;
					y = malloc(frame_bytes);
					uv = y + (size_t)w * h;
					if (y && !rdnvd_frame_read(frame, y, w, uv, w)) {
						for (i = 0; i < w * h; i += 97)
							zeros += y[i] == 0;
						nonzero += zeros < (w * h / 97) / 2;
						/* decoding order equals display order in a stream without B pictures */
						if ((size_t)(au + 1) * frame_bytes <= ref_size &&
						    !memcmp(y, ref + (size_t)au * frame_bytes, (size_t)w * h))
							right++;
						else
							wrong++;
					}
					free(y);
					rdnvd_frame_release(frame);
				}
				if (au == 0 && gap_ms) {
					printf("first access unit decoded in %.1f ms; waiting %d ms\n", now_ms() - tdec, gap_ms);
					fflush(stdout);
					usleep((useconds_t)gap_ms * 1000);
				}
				if (pace_ms) {
					/* one session for a long time, no restart of the block: a line with the time of day for each access unit */
					struct timeval tv;
					struct tm *t;

					gettimeofday(&tv, NULL);
					t = localtime(&tv.tv_sec);
					printf("%02d:%02d:%02d.%03d au %u %s\n", t->tm_hour, t->tm_min, t->tm_sec, (int)(tv.tv_usec / 1000), au,
					       r || !frame ? "NOT DECODED" : "ok");
					fflush(stdout);
					usleep((useconds_t)pace_ms * 1000);
				}
				au++;
				au_start = next;
			}
			pos = next;
		}
		printf("%u access units: %u luma planes right, %u wrong (%u with picture content)\n", au, right, wrong, nonzero);
		printf("%s\n", wrong || !right ? "FAIL" : "PASS: every picture identical to ffmpeg's");
		rdnvd_h264_close(s);
		rdnvd_close(d);
		return wrong != 0 || !right;
	}
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
