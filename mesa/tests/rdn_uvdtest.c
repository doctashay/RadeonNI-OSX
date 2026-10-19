/*
 * rdn_uvdtest: decode one H.264 picture on the card's video decoder (UVD)
 * through Mesa's pipe_video_codec and compare it with a reference.
 *
 *   rdn_uvdtest stream.h264 reference.yuv [-l level] [-n count] [-o last.nv12]
 *
 * stream.h264: Annex B with one SPS, one PPS and then one slice per
 * picture, the first an IDR (4:2:0, 8 bits, frame pictures, I and P
 * slices, picture order count type 0 or 2, one slice group);
 * scripts/uvd-testvec.sh makes one with ffmpeg. reference.yuv: the same
 * pictures decoded by ffmpeg as yuv420p. H.264 decodes bit-exactly, so
 * every picture must be identical. -n repeats the whole sequence.
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

/*
 * Slice headers: as far as the picture description needs them.
 */

struct slice {
	const uint8_t *nal;
	size_t size;
	unsigned nal_ref_idc;
	bool idr;
	unsigned slice_type, frame_num, poc_lsb;
	int delta_poc_bottom;
	unsigned num_ref_idx_l0_active_minus1, num_ref_idx_l1_active_minus1;
};

static int parse_slice(struct slice *sl, const struct pipe_h264_sps *sps, const struct pipe_h264_pps *pps)
{
	/* The header is in the first bytes. */
	static uint8_t rbsp[256];
	struct bits b = { rbsp, 0, 0 };
	unsigned n = unescape(sl->nal, sl->size > 64 ? 64 : sl->size, rbsp);

	b.bits = n * 8;
	sl->nal_ref_idc = (sl->nal[0] >> 5) & 3;
	sl->idr = (sl->nal[0] & 0x1f) == 5;
	get_ue(&b);			/* first_mb_in_slice */
	sl->slice_type = get_ue(&b) % 5;
	get_ue(&b);			/* pic_parameter_set_id */
	sl->frame_num = get_bits(&b, sps->log2_max_frame_num_minus4 + 4);
	if (sl->idr)
		get_ue(&b);		/* idr_pic_id */
	if (sps->pic_order_cnt_type == 0) {
		sl->poc_lsb = get_bits(&b, sps->log2_max_pic_order_cnt_lsb_minus4 + 4);
		if (pps->bottom_field_pic_order_in_frame_present_flag)
			sl->delta_poc_bottom = get_se(&b);
	} else if (sps->pic_order_cnt_type == 1) {
		fprintf(stderr, "uvdtest: picture order count type 1 is not handled here\n");
		return -1;
	}
	if (pps->redundant_pic_cnt_present_flag)
		get_ue(&b);
	if (sl->slice_type == 1)
		get_bits(&b, 1);	/* direct_spatial_mv_pred_flag */
	sl->num_ref_idx_l0_active_minus1 = pps->num_ref_idx_l0_default_active_minus1;
	sl->num_ref_idx_l1_active_minus1 = pps->num_ref_idx_l1_default_active_minus1;
	if (sl->slice_type == 0 || sl->slice_type == 1 || sl->slice_type == 3) {
		if (get_bits(&b, 1)) {
			sl->num_ref_idx_l0_active_minus1 = get_ue(&b);
			if (sl->slice_type == 1)
				sl->num_ref_idx_l1_active_minus1 = get_ue(&b);
		}
	}
	return 0;
}

/* A reference picture the decoder holds. */
struct ref {
	unsigned frame_num;
	int poc[2];
	struct pipe_video_buffer *target;
};

/*
 * The decoded picture in `target` against the reference's: the number of
 * samples that differ, and where the first luma one is.
 */
static unsigned long compare(struct pipe_screen *screen, struct pipe_video_buffer *target, const uint8_t *ref_frame,
			     unsigned width, unsigned height, const unsigned crop[4], unsigned out_w,
			     unsigned out_h, FILE *out, unsigned long *first_luma)
{
	struct pipe_resource *res[3] = { NULL, NULL, NULL };
	unsigned stride_y, stride_uv, x, row;
	const uint8_t *ref_y = ref_frame, *ref_u = ref_frame + out_w * out_h, *ref_v = ref_u + out_w * out_h / 4;
	const uint8_t *y, *uv;
	unsigned long bad = 0;

	target->get_resources(target, res);
	y = raw_plane(screen, res[0], &stride_y);
	uv = res[1] ? raw_plane(screen, res[1], &stride_uv) : NULL;
	if (!y || !uv) {
		fprintf(stderr, "uvdtest: cannot map the decoded picture\n");
		return (unsigned long)-1;
	}
	*first_luma = (unsigned long)-1;
	for (row = 0; row < out_h; row++)
		for (x = 0; x < out_w; x++)
			if (y[(row + 2 * crop[2]) * stride_y + x + 2 * crop[0]] != ref_y[row * out_w + x]) {
				if (*first_luma == (unsigned long)-1)
					*first_luma = row * out_w + x;
				bad++;
			}
	for (row = 0; row < out_h / 2; row++)
		for (x = 0; x < out_w / 2; x++) {
			const uint8_t *p = uv + (row + crop[2]) * stride_uv + (x + crop[0]) * 2;

			bad += p[0] != ref_u[row * (out_w / 2) + x];
			bad += p[1] != ref_v[row * (out_w / 2) + x];
		}
	if (out) {
		rewind(out);
		for (row = 0; row < height; row++)
			fwrite(y + row * stride_y, 1, width, out);
		for (row = 0; row < height / 2; row++)
			fwrite(uv + row * stride_uv, 1, width, out);
	}
	return bad;
}

#define MAX_PICTURES 4096

int main(int argc, char **argv)
{
	const char *stream_path = NULL, *ref_path = NULL, *out_path = NULL;
	unsigned level = 0, count = 1, pass;
	size_t stream_size, ref_size, i;
	uint8_t *stream, *ref;
	const uint8_t *sps_nal = NULL, *pps_nal = NULL;
	size_t sps_size = 0, pps_size = 0;
	static struct slice slices[MAX_PICTURES];
	unsigned num_slices = 0;
	struct pipe_h264_sps sps;
	struct pipe_h264_pps pps;
	struct pipe_h264_picture_desc desc;
	unsigned profile_idc, crop[4], width, height, out_w, out_h, frame_bytes;
	struct pipe_screen *screen;
	struct pipe_context *ctx;
	struct pipe_video_codec templ, *codec;
	struct pipe_video_buffer vtempl, *pool[16];
	unsigned pool_size, pool_next = 0;
	struct pipe_fence_handle *fence = NULL;
	static const uint8_t start_code[3] = { 0, 0, 1 };
	const void *buffers[2];
	unsigned sizes[2];
	unsigned long total_bad = 0;
	double t0;
	FILE *out = NULL;
	int a;

	for (a = 1; a < argc; a++) {
		if (!strcmp(argv[a], "-l") && a + 1 < argc)
			level = atoi(argv[++a]);
		else if (!strcmp(argv[a], "-n") && a + 1 < argc)
			count = atoi(argv[++a]);
		else if (!strcmp(argv[a], "-o") && a + 1 < argc)
			out_path = argv[++a];
		else if (!stream_path)
			stream_path = argv[a];
		else if (!ref_path)
			ref_path = argv[a];
	}
	if (!stream_path || !ref_path) {
		fprintf(stderr, "usage: rdn_uvdtest stream.h264 reference.yuv [-l level] [-n count] [-o last.nv12]\n");
		return 2;
	}
	stream = read_file(stream_path, &stream_size);
	ref = read_file(ref_path, &ref_size);
	if (!stream || !ref)
		return 1;

	/* The NAL units: SPS, PPS and the slices. */
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
		case 1:
		case 5:
			if (num_slices < MAX_PICTURES) {
				slices[num_slices].nal = stream + start;
				slices[num_slices].size = end - start;
				num_slices++;
			}
			break;
		}
		i = end;
	}
	if (!sps_nal || !pps_nal || !num_slices || (slices[0].nal[0] & 0x1f) != 5) {
		fprintf(stderr, "uvdtest: the stream needs an SPS, a PPS and slices, the first an IDR\n");
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
	frame_bytes = out_w * out_h * 3 / 2;
	printf("stream: profile_idc %u, level_idc %u, %ux%u (shown %ux%u), %s, %u reference frames, %u pictures\n",
	       profile_idc, (unsigned)sps.level_idc, width, height, out_w, out_h,
	       pps.entropy_coding_mode_flag ? "CABAC" : "CAVLC", (unsigned)sps.max_num_ref_frames, num_slices);
	if (ref_size != (size_t)frame_bytes * num_slices) {
		fprintf(stderr, "uvdtest: the reference has %lu bytes, %u yuv420p pictures of %ux%u have %u\n",
			(unsigned long)ref_size, num_slices, out_w, out_h, frame_bytes * num_slices);
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

	/* Pictures go round a pool of targets, so that a reference is not overwritten while it is one. */
	pool_size = sps.max_num_ref_frames + 2;
	if (pool_size > 16)
		pool_size = 16;
	memset(&vtempl, 0, sizeof(vtempl));
	vtempl.buffer_format = PIPE_FORMAT_NV12;
	vtempl.width = width;
	vtempl.height = height;
	for (i = 0; i < pool_size; i++) {
		pool[i] = ctx->create_video_buffer(ctx, &vtempl);
		if (!pool[i]) {
			fprintf(stderr, "uvdtest: create_video_buffer failed\n");
			return 1;
		}
	}
	if (out_path)
		out = fopen(out_path, "wb");

	t0 = now_ms();
	for (pass = 0; pass < count; pass++) {
		struct ref refs[16];
		unsigned num_refs = 0, prev_frame_num = 0, frame_num_offset = 0, pic;
		int prev_poc_msb = 0, prev_poc_lsb = 0;

		for (pic = 0; pic < num_slices; pic++) {
			struct slice *sl = &slices[pic];
			struct pipe_video_buffer *target = pool[pool_next++ % pool_size];
			int poc_msb = 0, poc[2] = { 0, 0 };
			unsigned long bad, first;
			unsigned k;

			if (parse_slice(sl, &sps, &pps))
				return 1;
			if (sl->idr)
				num_refs = 0;

			/* Picture order count (8.2.1). */
			if (sps.pic_order_cnt_type == 2) {
				if (sl->idr)
					frame_num_offset = 0;
				else if (prev_frame_num > sl->frame_num)
					frame_num_offset += 1u << (sps.log2_max_frame_num_minus4 + 4);
				poc[0] = poc[1] = sl->idr ? 0 : 2 * (int)(frame_num_offset + sl->frame_num) -
							       (sl->nal_ref_idc ? 0 : 1);
				prev_frame_num = sl->frame_num;
			} else {
				int max_lsb = 1 << (sps.log2_max_pic_order_cnt_lsb_minus4 + 4);

				if (sl->idr)
					prev_poc_msb = prev_poc_lsb = 0;
				if ((int)sl->poc_lsb < prev_poc_lsb && prev_poc_lsb - (int)sl->poc_lsb >= max_lsb / 2)
					poc_msb = prev_poc_msb + max_lsb;
				else if ((int)sl->poc_lsb > prev_poc_lsb && (int)sl->poc_lsb - prev_poc_lsb > max_lsb / 2)
					poc_msb = prev_poc_msb - max_lsb;
				else
					poc_msb = prev_poc_msb;
				poc[0] = poc_msb + sl->poc_lsb;
				poc[1] = poc[0] + sl->delta_poc_bottom;
				if (sl->nal_ref_idc) {
					prev_poc_msb = poc_msb;
					prev_poc_lsb = sl->poc_lsb;
				}
			}

			memset(&desc, 0, sizeof(desc));
			desc.base.profile = templ.profile;
			desc.base.entry_point = PIPE_VIDEO_ENTRYPOINT_BITSTREAM;
			desc.base.out_fence = &fence;
			desc.pps = &pps;
			desc.frame_num = sl->frame_num;
			desc.num_ref_idx_l0_active_minus1 = sl->num_ref_idx_l0_active_minus1;
			desc.num_ref_idx_l1_active_minus1 = sl->num_ref_idx_l1_active_minus1;
			desc.slice_count = 1;
			desc.field_order_cnt[0] = poc[0];
			desc.field_order_cnt[1] = poc[1];
			desc.is_reference = sl->nal_ref_idc != 0;
			desc.num_ref_frames = sps.max_num_ref_frames;
			for (k = 0; k < num_refs; k++) {
				desc.frame_num_list[k] = refs[k].frame_num;
				desc.field_order_cnt_list[k][0] = refs[k].poc[0];
				desc.field_order_cnt_list[k][1] = refs[k].poc[1];
				desc.top_is_reference[k] = desc.bottom_is_reference[k] = true;
				desc.ref[k] = refs[k].target;
			}

			buffers[0] = start_code;
			sizes[0] = sizeof(start_code);
			buffers[1] = sl->nal;
			sizes[1] = sl->size;
			codec->begin_frame(codec, target, &desc.base);
			codec->decode_bitstream(codec, target, &desc.base, 2, buffers, sizes);
			if (codec->end_frame(codec, target, &desc.base)) {
				fprintf(stderr, "uvdtest: end_frame failed (picture %u)\n", pic);
				return 1;
			}
			if (!fence || !codec->fence_wait(codec, fence, 5000ull * 1000 * 1000)) {
				fprintf(stderr, "uvdtest: the decoder did not finish in 5 s (picture %u)\n", pic);
				return 1;
			}
			bad = compare(screen, target, ref + (size_t)pic * frame_bytes, width, height, crop, out_w, out_h,
				      pass == count - 1 ? out : NULL, &first);
			if (bad == (unsigned long)-1)
				return 1;
			if (bad && total_bad < 16)
				printf("picture %u (%s, frame_num %u, poc %d): %lu samples differ%s\n", pic,
				       sl->slice_type == 2 ? "I" : sl->slice_type == 0 ? "P" : "other", sl->frame_num,
				       poc[0], bad, first != (unsigned long)-1 ? ", luma too" : "");
			total_bad += bad;

			/* The sliding window of reference pictures, the newest first. */
			if (sl->nal_ref_idc) {
				unsigned keep = sps.max_num_ref_frames ? sps.max_num_ref_frames : 1;

				if (num_refs == keep)
					num_refs--;
				memmove(&refs[1], &refs[0], num_refs * sizeof(refs[0]));
				refs[0].frame_num = sl->frame_num;
				refs[0].poc[0] = poc[0];
				refs[0].poc[1] = poc[1];
				refs[0].target = target;
				num_refs++;
			}
		}
	}
	printf("decoded %u pictures in %.1f ms (%.2f ms each)\n", count * num_slices, now_ms() - t0,
	       (now_ms() - t0) / (count * num_slices));
	printf("%s\n", total_bad ? "FAIL: the card's pictures differ from ffmpeg's" : "PASS: every picture identical to ffmpeg's");
	if (out)
		fclose(out);
	codec->destroy(codec);
	return total_bad != 0;
}
