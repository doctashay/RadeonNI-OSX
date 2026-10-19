/*
 * h264dump: feed an Annex B file to the H.264 layer (rdnvd_h264.c) and print what it decides: each
 * picture in decoding order (type, frame number, order counts, references) and then the order
 * the pictures leave in. No card needed; scripts/uvd-parse-check.sh compares the output order with ffmpeg's.
 *   h264dump [-q] file.h264
 *
 * Copyright (c) 2026 kouta-kun and Claude
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../rdnvd_h264.h"

static int quiet;
static unsigned decoded, outputs, released, live;

static int on_sequence(void *user, const struct rdnvd_h264_sps *sps)
{
	printf("sequence: profile %u level %u %ux%u MBs (crop %u %u %u %u) refs %u dpb %u poc type %u%s frame_mbs_only %u mbaff %u\n",
	       sps->profile_idc, sps->level_idc, sps->width_mbs, sps->height_mbs, sps->crop_left, sps->crop_right,
	       sps->crop_top, sps->crop_bottom, sps->max_num_ref_frames, sps->dpb_frames, sps->pic_order_cnt_type,
	       sps->vui_bitstream_restriction ? " (VUI reorder)" : "", sps->frame_mbs_only_flag,
	       sps->mb_adaptive_frame_field_flag);
	return 0;
}

static int on_decode(void *user, const struct rdnvd_h264_picture *pic, void **surface)
{
	unsigned i;

	if (!quiet) {
		printf("decode #%u: %c%s frame_num %u poc %d/%d slices %u refs", decoded,
		       pic->slice_type == 2 ? 'I' : pic->slice_type == 0 ? 'P' : 'B', pic->idr ? " (IDR)" : "",
		       pic->frame_num, (int)pic->poc[0], (int)pic->poc[1], pic->num_slices);
		for (i = 0; i < pic->num_refs; i++)
			printf(" %s%u(poc %d)", pic->refs[i].is_long_term ? "L" : "", pic->refs[i].frame_num,
			       (int)pic->refs[i].poc[0]);
		printf("%s\n", pic->is_reference ? "" : "  [non-ref]");
	}
	*surface = (void *)(uintptr_t)(++decoded);
	live++;
	return 0;
}

static void on_output(void *user, void *surface, uint64_t pts, const struct rdnvd_h264_sps *sps)
{
	if (!quiet)
		printf("output #%u (decoded #%u)\n", outputs, (unsigned)(uintptr_t)surface);
	else
		printf("%u\n", (unsigned)(uintptr_t)surface);
	outputs++;
}

static void on_release(void *user, void *surface)
{
	released++;
	live--;
}

int main(int argc, char **argv)
{
	static const struct rdnvd_h264_callbacks cb = { on_sequence, on_decode, on_output, on_release };
	struct rdnvd_h264 *h;
	FILE *f;
	uint8_t *data;
	long size;
	int r;

	if (argc > 1 && !strcmp(argv[1], "-q")) {
		quiet = 1;
		argc--;
		argv++;
	}
	if (argc < 2 || !(f = fopen(argv[1], "rb"))) {
		fprintf(stderr, "usage: h264dump [-q] file.h264\n");
		return 2;
	}
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	data = malloc(size);
	if (!data || fread(data, 1, size, f) != (size_t)size)
		return 1;
	fclose(f);
	h = rdnvd_h264_new(&cb, NULL);
	r = rdnvd_h264_feed_annexb(h, data, size, 0);
	if (r)
		fprintf(stderr, "feed: %d\n", r);
	r = rdnvd_h264_flush(h, 0);
	if (r)
		fprintf(stderr, "flush: %d\n", r);
	rdnvd_h264_free(h);
	fprintf(stderr, "decoded %u, output %u, released %u, still held %u\n", decoded, outputs, released, live);
	return decoded == outputs && !live ? 0 : 1;
}
