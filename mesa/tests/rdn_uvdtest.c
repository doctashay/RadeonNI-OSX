/*
 * rdn_uvdtest: decode one H.264 picture on the card's video decoder (UVD)
 * through Mesa's pipe_video_codec and compare it with a reference.
 *
 *   rdn_uvdtest stream.h264 reference.yuv [-l level] [-o out.nv12]
 *
 * stream.h264: Annex B with one SPS, one PPS and one IDR slice (4:2:0, 8
 * bits, CAVLC or CABAC, one slice group); scripts/uvd-testvec.sh makes
 * one with ffmpeg. reference.yuv: the same picture decoded by ffmpeg as
 * yuv420p. H.264 decodes bit-exactly, so the pictures must be identical.
 *
 * The parameters are parsed here only as far as a test needs; the real
 * path (a parser that fills pipe_h264_picture_desc from ffmpeg's) is not
 * part of this program.
 *
 * Copyright (c) 2026 kouta-kun and Claude
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/time.h>

#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "pipe/p_video_codec.h"
#include "pipe/p_video_state.h"
#include "util/box.h"
#include "util/u_inlines.h"
#include "util/format/u_format.h"
#include "r600/r600_pipe_common.h"

extern struct pipe_screen *osmesa_create_screen(void);

/*
 * Parsing
 */

struct bits {
	const uint8_t *p;
	size_t bits, pos;
};

static unsigned get_bits(struct bits *b, unsigned n)
{
	unsigned v = 0;

	while (n--) {
		unsigned bit = b->pos < b->bits ? (b->p[b->pos >> 3] >> (7 - (b->pos & 7))) & 1 : 0;

		v = v << 1 | bit;
		b->pos++;
	}
	return v;
}

static unsigned get_ue(struct bits *b)
{
	unsigned zeros = 0;

	while (!get_bits(b, 1) && zeros < 32 && b->pos < b->bits)
		zeros++;
	return ((1u << zeros) - 1) + (zeros ? get_bits(b, zeros) : 0);
}

static int get_se(struct bits *b)
{
	unsigned v = get_ue(b);

	return (v & 1) ? (int)((v + 1) / 2) : -(int)(v / 2);
}

/* The payload of a NAL unit (after its header byte) without the emulation prevention bytes. */
static size_t unescape(const uint8_t *nal, size_t size, uint8_t *out)
{
	size_t i, n = 0;
	unsigned zeros = 0;

	for (i = 1; i < size; i++) {
		if (zeros >= 2 && nal[i] == 3) {
			zeros = 0;
			continue;
		}
		zeros = nal[i] == 0 ? zeros + 1 : 0;
		out[n++] = nal[i];
	}
	return n;
}

/* The bits of the payload before its trailing 1 and zeros. */
static size_t rbsp_bits(const uint8_t *p, size_t n)
{
	size_t last_bit;

	while (n && !p[n - 1])
		n--;
	if (!n)
		return 0;
	for (last_bit = 0; !((p[n - 1] >> last_bit) & 1); last_bit++)
		;
	return n * 8 - last_bit - 1;
}

static int parse_sps(const uint8_t *nal, size_t size, struct pipe_h264_sps *sps, unsigned *profile_idc,
		     unsigned crop[4])
{
	static uint8_t rbsp[4096];
	struct bits b = { rbsp, 0, 0 };
	unsigned i, n, constraints;

	n = unescape(nal, size > sizeof(rbsp) ? sizeof(rbsp) : size, rbsp);
	b.bits = rbsp_bits(rbsp, n);
	*profile_idc = get_bits(&b, 8);
	constraints = get_bits(&b, 8);
	(void)constraints;
	sps->level_idc = get_bits(&b, 8);
	get_ue(&b);			/* seq_parameter_set_id */
	sps->chroma_format_idc = 1;
	if (*profile_idc == 100 || *profile_idc == 110 || *profile_idc == 122 || *profile_idc == 244 ||
	    *profile_idc == 44 || *profile_idc == 83 || *profile_idc == 86 || *profile_idc == 118 ||
	    *profile_idc == 128) {
		sps->chroma_format_idc = get_ue(&b);
		if (sps->chroma_format_idc == 3)
			sps->separate_colour_plane_flag = get_bits(&b, 1);
		sps->bit_depth_luma_minus8 = get_ue(&b);
		sps->bit_depth_chroma_minus8 = get_ue(&b);
		get_bits(&b, 1);	/* qpprime_y_zero_transform_bypass_flag */
		sps->seq_scaling_matrix_present_flag = get_bits(&b, 1);
		if (sps->seq_scaling_matrix_present_flag) {
			fprintf(stderr, "uvdtest: scaling matrices in the SPS are not parsed here\n");
			return -1;
		}
	}
	if (sps->chroma_format_idc != 1 || sps->bit_depth_luma_minus8 || sps->bit_depth_chroma_minus8) {
		fprintf(stderr, "uvdtest: only 8-bit 4:2:0\n");
		return -1;
	}
	sps->log2_max_frame_num_minus4 = get_ue(&b);
	sps->pic_order_cnt_type = get_ue(&b);
	if (sps->pic_order_cnt_type == 0) {
		sps->log2_max_pic_order_cnt_lsb_minus4 = get_ue(&b);
	} else if (sps->pic_order_cnt_type == 1) {
		sps->delta_pic_order_always_zero_flag = get_bits(&b, 1);
		sps->offset_for_non_ref_pic = get_se(&b);
		sps->offset_for_top_to_bottom_field = get_se(&b);
		sps->num_ref_frames_in_pic_order_cnt_cycle = get_ue(&b);
		for (i = 0; i < sps->num_ref_frames_in_pic_order_cnt_cycle && i < 256; i++)
			sps->offset_for_ref_frame[i] = get_se(&b);
	}
	sps->max_num_ref_frames = get_ue(&b);
	sps->gaps_in_frame_num_value_allowed_flag = get_bits(&b, 1);
	sps->pic_width_in_mbs_minus1 = get_ue(&b);
	sps->pic_height_in_mbs_minus1 = get_ue(&b);
	sps->frame_mbs_only_flag = get_bits(&b, 1);
	if (!sps->frame_mbs_only_flag) {
		fprintf(stderr, "uvdtest: interlaced streams are not handled here\n");
		return -1;
	}
	sps->direct_8x8_inference_flag = get_bits(&b, 1);
	memset(crop, 0, 4 * sizeof(unsigned));
	if (get_bits(&b, 1))
		for (i = 0; i < 4; i++)
			crop[i] = get_ue(&b);	/* left, right, top, bottom, in units of 2 */
	memset(sps->ScalingList4x4, 16, sizeof(sps->ScalingList4x4));
	memset(sps->ScalingList8x8, 16, sizeof(sps->ScalingList8x8));
	return 0;
}

static int parse_pps(const uint8_t *nal, size_t size, struct pipe_h264_pps *pps)
{
	static uint8_t rbsp[4096];
	struct bits b = { rbsp, 0, 0 };
	unsigned n;

	n = unescape(nal, size > sizeof(rbsp) ? sizeof(rbsp) : size, rbsp);
	b.bits = rbsp_bits(rbsp, n);
	get_ue(&b);			/* pic_parameter_set_id */
	get_ue(&b);			/* seq_parameter_set_id */
	pps->entropy_coding_mode_flag = get_bits(&b, 1);
	pps->bottom_field_pic_order_in_frame_present_flag = get_bits(&b, 1);
	pps->num_slice_groups_minus1 = get_ue(&b);
	if (pps->num_slice_groups_minus1) {
		fprintf(stderr, "uvdtest: slice groups are not parsed here\n");
		return -1;
	}
	pps->num_ref_idx_l0_default_active_minus1 = get_ue(&b);
	pps->num_ref_idx_l1_default_active_minus1 = get_ue(&b);
	pps->weighted_pred_flag = get_bits(&b, 1);
	pps->weighted_bipred_idc = get_bits(&b, 2);
	pps->pic_init_qp_minus26 = get_se(&b);
	pps->pic_init_qs_minus26 = get_se(&b);
	pps->chroma_qp_index_offset = get_se(&b);
	pps->deblocking_filter_control_present_flag = get_bits(&b, 1);
	pps->constrained_intra_pred_flag = get_bits(&b, 1);
	pps->redundant_pic_cnt_present_flag = get_bits(&b, 1);
	pps->second_chroma_qp_index_offset = pps->chroma_qp_index_offset;
	if (b.pos < b.bits) {
		pps->transform_8x8_mode_flag = get_bits(&b, 1);
		if (get_bits(&b, 1)) {
			fprintf(stderr, "uvdtest: scaling matrices in the PPS are not parsed here\n");
			return -1;
		}
		pps->second_chroma_qp_index_offset = get_se(&b);
	}
	memset(pps->ScalingList4x4, 16, sizeof(pps->ScalingList4x4));
	memset(pps->ScalingList8x8, 16, sizeof(pps->ScalingList8x8));
	return 0;
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

/*
 * A plane of the decoded picture as it is in memory: from the buffer
 * itself, not through a transfer. r600 always copies a texture in video
 * memory with the 3D engine to read it, and on a big-endian host that
 * swaps the two bytes of every 16-bit texel the GPU did not write (the
 * decoder wrote these), turning the chroma plane's U and V around.
 */
static const uint8_t *raw_plane(struct pipe_screen *screen, struct pipe_resource *res, unsigned *stride)
{
	struct r600_common_screen *rscreen = (struct r600_common_screen *)screen;
	struct r600_texture *rtex = r600_as_texture(res);
	const struct legacy_surf_level *level = &rtex->surface.u.legacy.level[0];
	uint8_t *base = rscreen->ws->buffer_map(rscreen->ws, rtex->resource.buf, NULL, PIPE_MAP_READ);

	*stride = level->nblk_x * util_format_get_blocksize(res->format);
	return base ? base + (uint64_t)level->offset_256B * 256 : NULL;
}

static double now_ms(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

int main(int argc, char **argv)
{
	const char *stream_path = NULL, *ref_path = NULL, *out_path = NULL;
	unsigned level = 0;
	size_t stream_size, ref_size, i;
	uint8_t *stream, *ref;
	const uint8_t *sps_nal = NULL, *pps_nal = NULL, *slice_nal = NULL;
	size_t sps_size = 0, pps_size = 0, slice_size = 0;
	struct pipe_h264_sps sps;
	struct pipe_h264_pps pps;
	struct pipe_h264_picture_desc desc;
	unsigned profile_idc, crop[4], width, height, out_w, out_h;
	struct pipe_screen *screen;
	struct pipe_context *ctx;
	struct pipe_video_codec templ, *codec;
	struct pipe_video_buffer vtempl, *target;
	struct pipe_resource *res[3] = { NULL, NULL, NULL };
	struct pipe_fence_handle *fence = NULL;
	static const uint8_t start_code[3] = { 0, 0, 1 };
	uint8_t *picture;
	const void *buffers[2];
	unsigned sizes[2];
	double t0;
	int a;

	for (a = 1; a < argc; a++) {
		if (!strcmp(argv[a], "-l") && a + 1 < argc)
			level = atoi(argv[++a]);
		else if (!strcmp(argv[a], "-o") && a + 1 < argc)
			out_path = argv[++a];
		else if (!stream_path)
			stream_path = argv[a];
		else if (!ref_path)
			ref_path = argv[a];
	}
	if (!stream_path || !ref_path) {
		fprintf(stderr, "usage: rdn_uvdtest stream.h264 reference.yuv [-l level] [-o out.nv12]\n");
		return 2;
	}
	stream = read_file(stream_path, &stream_size);
	ref = read_file(ref_path, &ref_size);
	if (!stream || !ref)
		return 1;

	/* The NAL units: SPS, PPS and the IDR slice. */
	for (i = 0; i + 3 < stream_size;) {
		size_t start, end;

		if (memcmp(stream + i, start_code, 3)) {
			i++;
			continue;
		}
		start = i + 3;
		for (end = start; end + 2 < stream_size && memcmp(stream + end, start_code, 3); end++)
			;
		if (end + 2 >= stream_size)
			end = stream_size;
		while (end > start && !stream[end - 1])
			end--;
		switch (stream[start] & 0x1f) {
		case 7:
			if (!sps_nal) {
				sps_nal = stream + start;
				sps_size = end - start;
			}
			break;
		case 8:
			if (!pps_nal) {
				pps_nal = stream + start;
				pps_size = end - start;
			}
			break;
		case 5:
			if (!slice_nal) {
				slice_nal = stream + start;
				slice_size = end - start;
			}
			break;
		}
		i = end;
	}
	if (!sps_nal || !pps_nal || !slice_nal) {
		fprintf(stderr, "uvdtest: the stream needs an SPS, a PPS and an IDR slice\n");
		return 1;
	}

	memset(&sps, 0, sizeof(sps));
	memset(&pps, 0, sizeof(pps));
	if (parse_sps(sps_nal, sps_size, &sps, &profile_idc, crop) || parse_pps(pps_nal, pps_size, &pps))
		return 1;
	pps.sps = &sps;
	width = (sps.pic_width_in_mbs_minus1 + 1) * 16;
	height = (sps.pic_height_in_mbs_minus1 + 1) * 16;
	out_w = width - 2 * (crop[0] + crop[1]);
	out_h = height - 2 * (crop[2] + crop[3]);
	printf("stream: profile_idc %u, level_idc %u, %ux%u (shown %ux%u), %s, %u reference frames\n",
	       profile_idc, (unsigned)sps.level_idc, width, height, out_w, out_h,
	       pps.entropy_coding_mode_flag ? "CABAC" : "CAVLC", (unsigned)sps.max_num_ref_frames);
	if (ref_size != (size_t)out_w * out_h * 3 / 2) {
		fprintf(stderr, "uvdtest: the reference has %lu bytes, a %ux%u yuv420p picture has %u\n",
			(unsigned long)ref_size, out_w, out_h, out_w * out_h * 3 / 2);
		return 1;
	}

	screen = osmesa_create_screen();
	if (!screen)
		return 1;
	ctx = screen->context_create(screen, NULL, 0);
	if (!ctx || !ctx->create_video_codec || !ctx->create_video_buffer) {
		fprintf(stderr, "uvdtest: the driver offers no video decoder (kext without the UVD firmware, "
			"or RDN_NO_UVD set)\n");
		return 1;
	}

	memset(&templ, 0, sizeof(templ));
	templ.profile = profile_idc == 66 ? PIPE_VIDEO_PROFILE_MPEG4_AVC_BASELINE :
			profile_idc == 77 ? PIPE_VIDEO_PROFILE_MPEG4_AVC_MAIN :
					    PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH;
	templ.level = level ? level : sps.level_idc;
	templ.entrypoint = PIPE_VIDEO_ENTRYPOINT_BITSTREAM;
	templ.width = width;
	templ.height = height;
	templ.max_references = sps.max_num_ref_frames ? sps.max_num_ref_frames : 1;
	t0 = now_ms();
	codec = ctx->create_video_codec(ctx, &templ);
	if (!codec) {
		fprintf(stderr, "uvdtest: create_video_codec failed\n");
		return 1;
	}
	printf("decoder created in %.1f ms\n", now_ms() - t0);

	memset(&vtempl, 0, sizeof(vtempl));
	vtempl.buffer_format = PIPE_FORMAT_NV12;
	vtempl.width = width;
	vtempl.height = height;
	target = ctx->create_video_buffer(ctx, &vtempl);
	if (!target) {
		fprintf(stderr, "uvdtest: create_video_buffer failed\n");
		return 1;
	}

	memset(&desc, 0, sizeof(desc));
	desc.base.profile = templ.profile;
	desc.base.entry_point = PIPE_VIDEO_ENTRYPOINT_BITSTREAM;
	desc.base.out_fence = &fence;
	desc.pps = &pps;
	desc.frame_num = 0;
	desc.num_ref_idx_l0_active_minus1 = pps.num_ref_idx_l0_default_active_minus1;
	desc.num_ref_idx_l1_active_minus1 = pps.num_ref_idx_l1_default_active_minus1;
	desc.slice_count = 1;
	desc.is_reference = true;
	desc.num_ref_frames = sps.max_num_ref_frames;

	picture = malloc(slice_size);
	memcpy(picture, slice_nal, slice_size);
	buffers[0] = start_code;
	sizes[0] = sizeof(start_code);
	buffers[1] = picture;
	sizes[1] = slice_size;

	t0 = now_ms();
	codec->begin_frame(codec, target, &desc.base);
	codec->decode_bitstream(codec, target, &desc.base, 2, buffers, sizes);
	if (codec->end_frame(codec, target, &desc.base)) {
		fprintf(stderr, "uvdtest: end_frame failed\n");
		return 1;
	}
	if (!fence || !codec->fence_wait(codec, fence, 5000ull * 1000 * 1000)) {
		fprintf(stderr, "uvdtest: the decoder did not finish in 5 s\n");
		return 1;
	}
	printf("decoded in %.1f ms\n", now_ms() - t0);

	target->get_resources(target, res);
	{
		unsigned stride_y, stride_uv;
		const uint8_t *y, *uv;
		unsigned long bad = 0, worst = 0, first = (unsigned long)-1;
		unsigned x, row;
		const uint8_t *ref_y = ref, *ref_u = ref + out_w * out_h, *ref_v = ref_u + out_w * out_h / 4;
		FILE *out = out_path ? fopen(out_path, "wb") : NULL;

		y = raw_plane(screen, res[0], &stride_y);
		uv = res[1] ? raw_plane(screen, res[1], &stride_uv) : NULL;
		if (!y || !uv) {
			fprintf(stderr, "uvdtest: cannot map the decoded picture\n");
			return 1;
		}
		for (row = 0; row < out_h; row++)
			for (x = 0; x < out_w; x++) {
				unsigned sy = row + 2 * crop[2], sx = x + 2 * crop[0];
				int d = y[sy * stride_y + sx] - ref_y[row * out_w + x];

				if (d) {
					if (first == (unsigned long)-1)
						first = row * out_w + x;
					bad++;
					if ((unsigned long)abs(d) > worst)
						worst = abs(d);
				}
			}
		printf("luma: %lu of %u samples differ (largest difference %lu%s)\n", bad, out_w * out_h,
		       worst, bad ? "" : ", identical");
		if (bad)
			printf("first difference at luma sample %lu (x %lu, y %lu)\n", first, first % out_w,
			       first / out_w);
		{
			unsigned long bad_u = 0, bad_v = 0;

			for (row = 0; row < out_h / 2; row++)
				for (x = 0; x < out_w / 2; x++) {
					unsigned sy = row + crop[2], sx = x + crop[0];
					const uint8_t *p = uv + sy * stride_uv + sx * 2;

					bad_u += p[0] != ref_u[row * (out_w / 2) + x];
					bad_v += p[1] != ref_v[row * (out_w / 2) + x];
				}
			printf("chroma: %lu U and %lu V of %u samples each differ\n", bad_u, bad_v,
			       out_w * out_h / 4);
			bad += bad_u + bad_v;
		}
		if (out) {
			for (row = 0; row < height; row++)
				fwrite(y + row * stride_y, 1, width, out);
			for (row = 0; row < height / 2; row++)
				fwrite(uv + row * stride_uv, 1, width, out);
			fclose(out);
		}
		printf("%s\n", bad ? "FAIL: the card's picture differs from ffmpeg's" : "PASS: identical to ffmpeg's");
		codec->destroy(codec);
		return bad != 0;
	}
}
