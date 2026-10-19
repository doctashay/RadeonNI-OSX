/*
 * rdnvd: the card's video decoder for programs (see rdnvd.h). The H.264 layer (video/rdnvd_h264.c)
 * finds the pictures and decides the references; Mesa's r600 UVD code (radeon_uvd.c) builds the
 * decoder's messages from a pipe_h264_picture_desc and runs them on the card; this file fills that
 * description in and keeps the surfaces.
 *
 * Copyright (c) 2026 kouta-kun and Claude
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "pipe/p_video_codec.h"
#include "pipe/p_video_state.h"
#include "util/box.h"
#include "util/u_endian.h"
#include "util/u_inlines.h"
#include "r600/r600_pipe_common.h"
#include "r600/r600_pipe.h"

#include "rdnvd.h"
#include "video/rdnvd_h264.h"

#define RDNVD_PUBLIC __attribute__((visibility("default")))

extern struct pipe_screen *osmesa_create_screen(void);

struct rdnvd {
	struct pipe_screen *screen;
	struct pipe_context *ctx;
	/* System memory (the GART) the card copies a picture into for the CPU to read. */
	struct pipe_resource *staging;
};

/* A decoded picture: a video buffer, and who holds it (the parser while it is a reference or waits for output, the caller). */
struct frame_buf {
	struct rdnvd_frame frame;		/* first: a frame pointer is a frame_buf pointer */
	struct pipe_video_buffer *buf;
	struct rdnvd *d;
	struct rdnvd_session *session;		/* NULL once the session is closed */
	unsigned gen;
	int refs;
	struct frame_buf *next;			/* the session's list of all its buffers */
	struct frame_buf *next_free;
};

struct rdnvd_session {
	struct rdnvd *d;
	struct rdnvd_h264 *parser;
	struct pipe_video_codec *codec;
	rdnvd_output_fn output;
	void *user;
	unsigned gen;				/* of the codec and its buffers: a new sequence is a new one */
	unsigned width, height;			/* the codec's, in samples */
	struct frame_buf *all, *free;
	struct pipe_fence_handle *fence;
	int error;				/* the first failure of a callback, for the feed that caused it */
};

/* ---- surfaces ---------------------------------------------------------------- */

static void frame_destroy(struct frame_buf *f)
{
	if (f->buf)
		f->buf->destroy(f->buf);
	free(f);
}

static struct frame_buf *frame_get(struct rdnvd_session *s)
{
	struct frame_buf *f = s->free;
	struct pipe_video_buffer tmpl;

	if (f) {
		s->free = f->next_free;
		f->refs = 1;
		return f;
	}
	f = calloc(1, sizeof(*f));
	if (!f)
		return NULL;
	memset(&tmpl, 0, sizeof(tmpl));
	tmpl.buffer_format = PIPE_FORMAT_NV12;
	tmpl.width = s->width;
	tmpl.height = s->height;
	f->buf = s->d->ctx->create_video_buffer(s->d->ctx, &tmpl);
	if (!f->buf) {
		free(f);
		return NULL;
	}
	f->d = s->d;
	f->session = s;
	f->gen = s->gen;
	f->refs = 1;
	f->next = s->all;
	s->all = f;
	return f;
}

/* Take `f` out of the session's list of its buffers. */
static void frame_unlink(struct rdnvd_session *s, struct frame_buf *f)
{
	struct frame_buf **p;

	for (p = &s->all; *p; p = &(*p)->next)
		if (*p == f) {
			*p = f->next;
			return;
		}
}

/* One reference less; at none the buffer goes back to the session's pool, or is destroyed. */
static void frame_unref(struct frame_buf *f)
{
	struct rdnvd_session *s = f->session;

	if (--f->refs > 0)
		return;
	if (s && f->gen == s->gen) {
		f->next_free = s->free;
		s->free = f;
		return;
	}
	if (s)
		frame_unlink(s, f);
	frame_destroy(f);
}

/* ---- the decoder ---------------------------------------------------------------- */

static enum pipe_video_profile profile_for(const struct rdnvd_h264_sps *sps)
{
	if (sps->profile_idc == 66)
		return (sps->constraint_flags & 0x40) ? PIPE_VIDEO_PROFILE_MPEG4_AVC_CONSTRAINED_BASELINE :
							PIPE_VIDEO_PROFILE_MPEG4_AVC_BASELINE;
	if (sps->profile_idc == 77)
		return PIPE_VIDEO_PROFILE_MPEG4_AVC_MAIN;
	if (sps->profile_idc >= 100)
		return PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH;
	return PIPE_VIDEO_PROFILE_UNKNOWN;		/* Extended: data partitioning, SP/SI */
}

static void codec_close(struct rdnvd_session *s)
{
	if (s->codec) {
		s->codec->destroy(s->codec);
		s->codec = NULL;
	}
	/* Buffers of the old sequence that nobody holds go now; held ones go when released. */
	while (s->free) {
		struct frame_buf *f = s->free;

		s->free = f->next_free;
		frame_unlink(s, f);
		frame_destroy(f);
	}
	s->gen++;
}

static int on_sequence(void *user, const struct rdnvd_h264_sps *sps)
{
	struct rdnvd_session *s = user;
	struct pipe_video_codec templ;
	enum pipe_video_profile profile = profile_for(sps);

	if (profile == PIPE_VIDEO_PROFILE_UNKNOWN || sps->chroma_format_idc != 1)
		return RDNVD_UNSUPPORTED;
	codec_close(s);
	memset(&templ, 0, sizeof(templ));
	templ.profile = profile;
	templ.level = sps->level_idc;
	templ.entrypoint = PIPE_VIDEO_ENTRYPOINT_BITSTREAM;
	templ.width = s->width = sps->width_mbs * 16;
	templ.height = s->height = sps->height_mbs * 16;
	templ.max_references = sps->max_num_ref_frames ? sps->max_num_ref_frames : 1;
	s->codec = s->d->ctx->create_video_codec(s->d->ctx, &templ);
	return s->codec ? 0 : RDNVD_FAILED;
}

static void fence_release(struct rdnvd_session *s)
{
	if (s->fence) {
		struct r600_common_screen *rs = (struct r600_common_screen *)s->d->screen;

		rs->ws->fence_reference(rs->ws, &s->fence, NULL);
	}
}

/* The raster position of each place of the zig-zag scan (8.5.6, 8.5.7). */
static const uint8_t zigzag4[16] = { 0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15 };
static const uint8_t zigzag8[64] = {
	0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14,
	21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53,
	60, 61, 54, 47, 55, 62, 63
};

/*
 * The scaling lists the way the decoder takes them from Mesa: how VA-API gives them (RDNVD_SCALING picks
 * another order for experiments: 0 as sent (zig-zag), 1 raster, 2 raster transposed).
 */
static void scaling_to_decoder(uint8_t dst4[6][16], uint8_t dst8[6][64], const uint8_t src4[6][16],
			       const uint8_t src8[6][64])
{
	static int mode = -1;
	unsigned i, j;

	if (mode < 0) {
		const char *e = getenv("RDNVD_SCALING");

		mode = e ? atoi(e) : 1;
	}
	for (i = 0; i < 6; i++) {
		for (j = 0; j < 16; j++) {
			unsigned pos = mode == 0 ? j : zigzag4[j];

			if (mode == 2)
				pos = (pos % 4) * 4 + pos / 4;
			dst4[i][pos] = src4[i][j];
		}
		for (j = 0; j < 64; j++) {
			unsigned pos = mode == 0 ? j : zigzag8[j];

			if (mode == 2)
				pos = (pos % 8) * 8 + pos / 8;
			dst8[i][pos] = src8[i][j];
		}
	}
}

static int on_decode(void *user, const struct rdnvd_h264_picture *pic, void **surface)
{
	struct rdnvd_session *s = user;
	const struct rdnvd_h264_sps *src_sps = pic->sps;
	const struct rdnvd_h264_pps *src_pps = pic->pps;
	struct pipe_h264_sps sps;
	struct pipe_h264_pps pps;
	struct pipe_h264_picture_desc desc;
	static const uint8_t start_code[3] = { 0, 0, 1 };
	const void *buffers[2 * RDNVD_H264_MAX_SLICES];
	unsigned sizes[2 * RDNVD_H264_MAX_SLICES];
	struct frame_buf *f;
	unsigned i;

	if (!s->codec)
		return RDNVD_FAILED;
	f = frame_get(s);
	if (!f)
		return RDNVD_NO_MEMORY;

	memset(&sps, 0, sizeof(sps));
	sps.level_idc = src_sps->level_idc;
	sps.chroma_format_idc = src_sps->chroma_format_idc;
	sps.separate_colour_plane_flag = src_sps->separate_colour_plane_flag;
	sps.bit_depth_luma_minus8 = src_sps->bit_depth_luma_minus8;
	sps.bit_depth_chroma_minus8 = src_sps->bit_depth_chroma_minus8;
	sps.seq_scaling_matrix_present_flag = src_sps->scaling_matrix_present;
	memcpy(sps.ScalingList4x4, src_sps->scaling_list_4x4, sizeof(sps.ScalingList4x4));
	memcpy(sps.ScalingList8x8, src_sps->scaling_list_8x8, sizeof(sps.ScalingList8x8));
	sps.log2_max_frame_num_minus4 = src_sps->log2_max_frame_num_minus4;
	sps.pic_order_cnt_type = src_sps->pic_order_cnt_type;
	sps.log2_max_pic_order_cnt_lsb_minus4 = src_sps->log2_max_pic_order_cnt_lsb_minus4;
	sps.delta_pic_order_always_zero_flag = src_sps->delta_pic_order_always_zero_flag;
	sps.offset_for_non_ref_pic = src_sps->offset_for_non_ref_pic;
	sps.offset_for_top_to_bottom_field = src_sps->offset_for_top_to_bottom_field;
	sps.num_ref_frames_in_pic_order_cnt_cycle = src_sps->num_ref_frames_in_pic_order_cnt_cycle;
	memcpy(sps.offset_for_ref_frame, src_sps->offset_for_ref_frame, sizeof(src_sps->offset_for_ref_frame));
	sps.max_num_ref_frames = src_sps->max_num_ref_frames;
	sps.frame_mbs_only_flag = src_sps->frame_mbs_only_flag;
	sps.mb_adaptive_frame_field_flag = src_sps->mb_adaptive_frame_field_flag;
	sps.direct_8x8_inference_flag = src_sps->direct_8x8_inference_flag;
	sps.gaps_in_frame_num_value_allowed_flag = src_sps->gaps_in_frame_num_allowed_flag;
	sps.pic_width_in_mbs_minus1 = src_sps->width_mbs - 1;
	sps.pic_height_in_mbs_minus1 = src_sps->height_mbs - 1;

	memset(&pps, 0, sizeof(pps));
	pps.sps = &sps;
	pps.entropy_coding_mode_flag = src_pps->entropy_coding_mode_flag;
	pps.bottom_field_pic_order_in_frame_present_flag = src_pps->bottom_field_pic_order_in_frame_present_flag;
	pps.num_slice_groups_minus1 = src_pps->num_slice_groups_minus1;
	pps.slice_group_map_type = src_pps->slice_group_map_type;
	pps.slice_group_change_rate_minus1 = (uint8_t)src_pps->slice_group_change_rate_minus1;
	pps.num_ref_idx_l0_default_active_minus1 = src_pps->num_ref_idx_default_active_minus1[0];
	pps.num_ref_idx_l1_default_active_minus1 = src_pps->num_ref_idx_default_active_minus1[1];
	pps.weighted_pred_flag = src_pps->weighted_pred_flag;
	pps.weighted_bipred_idc = src_pps->weighted_bipred_idc;
	pps.pic_init_qp_minus26 = src_pps->pic_init_qp_minus26;
	pps.pic_init_qs_minus26 = src_pps->pic_init_qs_minus26;
	pps.chroma_qp_index_offset = src_pps->chroma_qp_index_offset;
	pps.deblocking_filter_control_present_flag = src_pps->deblocking_filter_control_present_flag;
	pps.constrained_intra_pred_flag = src_pps->constrained_intra_pred_flag;
	pps.redundant_pic_cnt_present_flag = src_pps->redundant_pic_cnt_present_flag;
	pps.transform_8x8_mode_flag = src_pps->transform_8x8_mode_flag;
	pps.second_chroma_qp_index_offset = src_pps->second_chroma_qp_index_offset;
	/* The lists in force for the picture (PPS over SPS). */
	scaling_to_decoder(pps.ScalingList4x4, pps.ScalingList8x8, pic->scaling_list_4x4, pic->scaling_list_8x8);

	memset(&desc, 0, sizeof(desc));
	desc.base.profile = s->codec->profile;
	desc.base.entry_point = PIPE_VIDEO_ENTRYPOINT_BITSTREAM;
	desc.base.out_fence = &s->fence;
	desc.pps = &pps;
	desc.frame_num = pic->frame_num;
	desc.num_ref_idx_l0_active_minus1 = pic->num_ref_idx_active_minus1[0];
	desc.num_ref_idx_l1_active_minus1 = pic->num_ref_idx_active_minus1[1];
	desc.slice_count = pic->num_slices;
	desc.field_order_cnt[0] = pic->poc[0];
	desc.field_order_cnt[1] = pic->poc[1];
	desc.is_reference = pic->is_reference;
	desc.num_ref_frames = src_sps->max_num_ref_frames;
	for (i = 0; i < pic->num_refs && i < 16; i++) {
		const struct rdnvd_h264_ref *ref = &pic->refs[i];

		desc.frame_num_list[i] = ref->frame_num;
		desc.field_order_cnt_list[i][0] = ref->poc[0];
		desc.field_order_cnt_list[i][1] = ref->poc[1];
		desc.is_long_term[i] = ref->is_long_term;
		desc.is_non_existing[i] = ref->is_non_existing;
		desc.top_is_reference[i] = desc.bottom_is_reference[i] = true;
		if (ref->surface)
			desc.ref[i] = ((struct frame_buf *)ref->surface)->buf;
	}
	for (i = 0; i < pic->num_slices; i++) {
		buffers[2 * i] = start_code;
		sizes[2 * i] = sizeof(start_code);
		buffers[2 * i + 1] = pic->slices[i].nal;
		sizes[2 * i + 1] = (unsigned)pic->slices[i].size;
	}

	s->codec->begin_frame(s->codec, f->buf, &desc.base);
	s->codec->decode_bitstream(s->codec, f->buf, &desc.base, 2 * pic->num_slices, buffers, sizes);
	if (s->codec->end_frame(s->codec, f->buf, &desc.base) ||
	    !s->fence || !s->codec->fence_wait(s->codec, s->fence, 10ull * 1000 * 1000 * 1000)) {
		fence_release(s);
		frame_unref(f);
		return RDNVD_FAILED;
	}
	fence_release(s);
	*surface = f;
	return 0;
}

static void on_output(void *user, void *surface, uint64_t pts, const struct rdnvd_h264_sps *sps)
{
	struct rdnvd_session *s = user;
	struct frame_buf *f = surface;

	f->refs++;				/* the caller's */
	f->frame.pts = pts;
	f->frame.coded_width = sps->width_mbs * 16;
	f->frame.coded_height = sps->height_mbs * 16;
	f->frame.crop_left = sps->crop_left;
	f->frame.crop_top = sps->crop_top;
	f->frame.width = f->frame.coded_width - sps->crop_left - sps->crop_right;
	f->frame.height = f->frame.coded_height - sps->crop_top - sps->crop_bottom;
	s->output(s->user, &f->frame);
}

static void on_release(void *user, void *surface)
{
	frame_unref(surface);
}

/* ---- the interface ------------------------------------------------------------------- */

RDNVD_PUBLIC struct rdnvd *rdnvd_open(void)
{
	struct rdnvd *d = calloc(1, sizeof(*d));

	if (!d)
		return NULL;
	d->screen = osmesa_create_screen();
	if (d->screen)
		d->ctx = d->screen->context_create(d->screen, NULL, 0);
	if (!d->ctx || !d->ctx->create_video_codec || !d->ctx->create_video_buffer) {
		if (d->ctx)
			d->ctx->destroy(d->ctx);
		free(d);
		return NULL;
	}
	return d;
}

RDNVD_PUBLIC void rdnvd_close(struct rdnvd *d)
{
	if (!d)
		return;
	pipe_resource_reference(&d->staging, NULL);
	d->ctx->destroy(d->ctx);
	free(d);
}

RDNVD_PUBLIC struct rdnvd_session *rdnvd_h264_open(struct rdnvd *d, const uint8_t *avcc, size_t avcc_size,
						   rdnvd_output_fn output, void *user, unsigned *nal_length_size)
{
	static const struct rdnvd_h264_callbacks cb = { on_sequence, on_decode, on_output, on_release };
	struct rdnvd_session *s = calloc(1, sizeof(*s));

	if (!s)
		return NULL;
	s->d = d;
	s->output = output;
	s->user = user;
	s->parser = rdnvd_h264_parser_new(&cb, s);
	if (!s->parser) {
		free(s);
		return NULL;
	}
	if (nal_length_size)
		*nal_length_size = 0;
	if (avcc) {
		int r = rdnvd_h264_parser_avcc(s->parser, avcc, avcc_size);

		if (r < 0) {
			rdnvd_h264_parser_free(s->parser);
			free(s);
			return NULL;
		}
		if (nal_length_size)
			*nal_length_size = (unsigned)r;
	}
	return s;
}

RDNVD_PUBLIC int rdnvd_h264_feed(struct rdnvd_session *s, const uint8_t *data, size_t size, uint64_t pts,
				 unsigned nal_length_size, int end_of_au)
{
	int r, r2 = 0;

	r = nal_length_size ? rdnvd_h264_parser_feed_avcc(s->parser, data, size, nal_length_size, pts) :
			      rdnvd_h264_parser_feed_annexb(s->parser, data, size, pts);
	if (end_of_au)
		r2 = rdnvd_h264_parser_end_au(s->parser);
	return r ? r : r2;
}

RDNVD_PUBLIC int rdnvd_h264_flush(struct rdnvd_session *s, int drop)
{
	return rdnvd_h264_parser_flush(s->parser, drop);
}

RDNVD_PUBLIC void rdnvd_h264_close(struct rdnvd_session *s)
{
	struct frame_buf *f, *next;

	if (!s)
		return;
	rdnvd_h264_parser_free(s->parser);	/* releases what the parser holds */
	codec_close(s);
	/* What the caller still holds stays until it is released; the rest is gone. */
	for (f = s->all; f; f = next) {
		next = f->next;
		f->session = NULL;
	}
	free(s);
}

static double now_ms(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

/*
 * The card's DMA engine copies the two planes' bytes as they are into system memory (3 MB a picture of
 * 1080p in about a millisecond), and the CPU reads that. The 3D engine's copy of an 8-bit texture into
 * system memory is some 20 ms per megabyte, and turns the two bytes of a chroma texel around.
 */
static int frame_read_dma(struct rdnvd_frame *frame, struct pipe_resource *res[3], uint8_t *y, size_t y_stride,
			  uint8_t *uv, size_t uv_stride)
{
	struct frame_buf *f = (struct frame_buf *)frame;
	struct rdnvd *d = f->d;
	struct r600_texture *ty = r600_as_texture(res[0]), *tc = r600_as_texture(res[1]);
	const struct legacy_surf_level *ly = &ty->surface.u.legacy.level[0], *lc = &tc->surface.u.legacy.level[0];
	unsigned y_pitch = ly->nblk_x, c_pitch = lc->nblk_x * 2;
	unsigned y_size = y_pitch * frame->coded_height, c_size = c_pitch * (frame->coded_height / 2);
	unsigned c_at = (y_size + 255) & ~255u, total = c_at + c_size;
	struct pipe_transfer *xfer;
	const uint8_t *src;
	unsigned row;
	unsigned cw = (frame->width + 1) / 2, ch = (frame->height + 1) / 2;

	if (!d->staging || d->staging->width0 < total) {
		pipe_resource_reference(&d->staging, NULL);
		d->staging = pipe_buffer_create(d->screen, 0, PIPE_USAGE_STAGING, total + (total >> 2));
		if (!d->staging)
			return RDNVD_NO_MEMORY;
	}
	r600_cp_dma_copy_buffer((struct r600_context *)d->ctx, d->staging, 0, res[0],
				(uint64_t)ly->offset_256B * 256, y_size);
	r600_cp_dma_copy_buffer((struct r600_context *)d->ctx, d->staging, c_at, res[1],
				(uint64_t)lc->offset_256B * 256, c_size);
	/* Mapping for reading flushes the commands and waits for them. */
	src = pipe_buffer_map_range(d->ctx, d->staging, 0, total, PIPE_MAP_READ, &xfer);
	if (!src)
		return RDNVD_FAILED;
	for (row = 0; row < frame->height; row++)
		memcpy(y + row * y_stride, src + (size_t)(row + frame->crop_top) * y_pitch + frame->crop_left,
		       frame->width);
	for (row = 0; row < ch; row++)
		memcpy(uv + row * uv_stride,
		       src + c_at + (size_t)(row + frame->crop_top / 2) * c_pitch + (frame->crop_left / 2) * 2, cw * 2);
	pipe_buffer_unmap(d->ctx, xfer);
	return 0;
}

RDNVD_PUBLIC int rdnvd_frame_read(struct rdnvd_frame *frame, uint8_t *y, size_t y_stride, uint8_t *uv,
				  size_t uv_stride)
{
	struct frame_buf *f = (struct frame_buf *)frame;
	struct pipe_context *ctx = f->d->ctx;
	struct pipe_resource *res[3] = { NULL, NULL, NULL };
	struct pipe_transfer *xfer;
	struct pipe_box box;
	const uint8_t *src;
	unsigned row, x;
	unsigned cw = (frame->width + 1) / 2, ch = (frame->height + 1) / 2;

	static int profile = -1;
	double t0 = 0, t1 = 0, t2 = 0, t3 = 0, t4 = 0;

	if (profile < 0)
		profile = getenv("RDNVD_PROFILE") != NULL;
	if (profile)
		t0 = now_ms();
	f->buf->get_resources(f->buf, res);
	if (!res[0] || !res[1])
		return RDNVD_FAILED;
	{
		static int dma = -1;

		if (dma < 0)
			dma = ((struct r600_common_screen *)f->d->screen)->has_cp_dma && !getenv("RDNVD_READ_BLIT");
		if (dma) {
			int r = frame_read_dma(frame, res, y, y_stride, uv, uv_stride);

			if (profile)
				fprintf(stderr, "rdnvd read (DMA): %.2f ms\n", now_ms() - t0);
			return r;
		}
	}

	u_box_2d(frame->crop_left, frame->crop_top, frame->width, frame->height, &box);
	src = ctx->texture_map(ctx, res[0], 0, PIPE_MAP_READ, &box, &xfer);
	if (!src)
		return RDNVD_FAILED;
	if (profile)
		t1 = now_ms();
	for (row = 0; row < frame->height; row++)
		memcpy(y + row * y_stride, src + (size_t)row * xfer->stride, frame->width);
	ctx->texture_unmap(ctx, xfer);
	if (profile)
		t2 = now_ms();

	u_box_2d(frame->crop_left / 2, frame->crop_top / 2, cw, ch, &box);
	src = ctx->texture_map(ctx, res[1], 0, PIPE_MAP_READ, &box, &xfer);
	if (!src)
		return RDNVD_FAILED;
	if (profile)
		t3 = now_ms();
	for (row = 0; row < ch; row++) {
		const uint8_t *in = src + (size_t)row * xfer->stride;
		uint8_t *out = uv + row * uv_stride;

#if UTIL_ARCH_BIG_ENDIAN
		/* The 3D engine's copy of a 16-bit texel the decoder wrote turns its two bytes around. */
		for (x = 0; x < cw; x++) {
			out[2 * x] = in[2 * x + 1];
			out[2 * x + 1] = in[2 * x];
		}
#else
		for (x = 0; x < cw; x++) {
			out[2 * x] = in[2 * x];
			out[2 * x + 1] = in[2 * x + 1];
		}
#endif
	}
	ctx->texture_unmap(ctx, xfer);
	if (profile) {
		t4 = now_ms();
		fprintf(stderr, "rdnvd read: luma map %.2f ms, copy %.2f ms, chroma map %.2f ms, copy %.2f ms\n",
			t1 - t0, t2 - t1, t3 - t2, t4 - t3);
	}
	return 0;
}

RDNVD_PUBLIC void rdnvd_frame_release(struct rdnvd_frame *frame)
{
	frame_unref((struct frame_buf *)frame);
}
