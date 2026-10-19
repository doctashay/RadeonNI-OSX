/*
 * H.264 parameter sets, slice headers, picture order count, reference marking and the decoded
 * picture buffer. See rdnvd_h264.h. Written from ITU-T H.264 (clause numbers in the comments).
 *
 * Copyright (c) 2026 kouta-kun and Claude
 * SPDX-License-Identifier: MIT
 */

#include <stdlib.h>
#include <string.h>

#include "rdnvd_h264.h"

/* ---- bit reader -------------------------------------------------------- */

struct bits {
	const uint8_t *p;
	size_t bits, pos;
	int err;
};

static uint32_t get_bits(struct bits *b, unsigned n)
{
	uint32_t v = 0;

	while (n--) {
		unsigned bit = 0;

		if (b->pos < b->bits)
			bit = (b->p[b->pos >> 3] >> (7 - (b->pos & 7))) & 1;
		else
			b->err = 1;
		v = v << 1 | bit;
		b->pos++;
	}
	return v;
}

static uint32_t get_ue(struct bits *b)
{
	unsigned zeros = 0;

	while (!get_bits(b, 1)) {
		if (++zeros > 31 || b->err)
			return 0;
	}
	return ((1u << zeros) - 1) + (zeros ? get_bits(b, zeros) : 0);
}

static int32_t get_se(struct bits *b)
{
	uint32_t v = get_ue(b);

	return (v & 1) ? (int32_t)((v + 1) / 2) : -(int32_t)(v / 2);
}

/* 7.2: whether there is more data before the trailing bits. */
static int more_rbsp_data(const struct bits *b)
{
	size_t end = b->bits;

	/* `bits` is the length without trailing zero bytes; the last 1 bit is the stop bit */
	if (b->pos >= end)
		return 0;
	{
		size_t last = end - 1;

		for (;; last--) {
			if ((b->p[last >> 3] >> (7 - (last & 7))) & 1)
				break;
			if (!last)
				return 0;
		}
		return b->pos < last;
	}
}

/* The payload of a NAL unit (after its header byte) without the emulation prevention bytes (7.4.1). */
static size_t unescape(const uint8_t *nal, size_t size, uint8_t *out, size_t max)
{
	size_t i, n = 0;
	unsigned zeros = 0;

	for (i = 1; i < size && n < max; i++) {
		if (zeros >= 2 && nal[i] == 3) {
			zeros = 0;
			continue;
		}
		zeros = nal[i] == 0 ? zeros + 1 : 0;
		out[n++] = nal[i];
	}
	return n;
}

static void bits_init(struct bits *b, const uint8_t *p, size_t n)
{
	b->p = p;
	b->bits = n * 8;
	b->pos = 0;
	b->err = 0;
}

/* The length in bits without the trailing 1 and zeros (for more_rbsp_data). */
static void bits_trim(struct bits *b, size_t n)
{
	while (n && !b->p[n - 1])
		n--;
	b->bits = n * 8;
}

/* ---- scaling lists (7.3.2.1.1.1, 7.4.2.1.1, Tables 7-3 and 7-4) --------- */

static const uint8_t default_4x4_intra[16] = {
	6, 13, 13, 20, 20, 20, 28, 28, 28, 28, 32, 32, 32, 37, 37, 42
};
static const uint8_t default_4x4_inter[16] = {
	10, 14, 14, 20, 20, 20, 24, 24, 24, 24, 27, 27, 27, 30, 30, 34
};
static const uint8_t default_8x8_intra[64] = {
	6, 10, 10, 13, 11, 13, 16, 16, 16, 16, 18, 18, 18, 18, 18, 23,
	23, 23, 23, 23, 23, 25, 25, 25, 25, 25, 25, 25, 27, 27, 27, 27,
	27, 27, 27, 27, 29, 29, 29, 29, 29, 29, 29, 31, 31, 31, 31, 31,
	31, 33, 33, 33, 33, 33, 36, 36, 36, 36, 38, 38, 38, 40, 40, 42
};
static const uint8_t default_8x8_inter[64] = {
	9, 13, 13, 15, 13, 15, 17, 17, 17, 17, 19, 19, 19, 19, 19, 21,
	21, 21, 21, 21, 21, 22, 22, 22, 22, 22, 22, 22, 24, 24, 24, 24,
	24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 25, 27, 27, 27, 27, 27,
	27, 28, 28, 28, 28, 28, 30, 30, 30, 30, 32, 32, 32, 33, 33, 35
};

/* Parse one list; returns 1 if the "use the default list" flag came out. */
static int scaling_list(struct bits *b, uint8_t *list, unsigned size)
{
	int last = 8, next = 8, use_default = 0;
	unsigned j;

	for (j = 0; j < size; j++) {
		if (next != 0) {
			int delta = get_se(b);

			next = (last + delta + 256) % 256;
			use_default = j == 0 && next == 0;
		}
		list[j] = (uint8_t)(next == 0 ? last : next);
		last = list[j];
	}
	return use_default;
}

static const uint8_t *default_list(unsigned i)
{
	if (i < 6)
		return i < 3 ? default_4x4_intra : default_4x4_inter;
	return (i & 1) == 0 ? default_8x8_intra : default_8x8_inter;
}

/*
 * The lists of rule A (no list sent for i: the default for 0, 3, 6, 7, otherwise the list
 * before it of the same kind) or rule B (the same, but 0, 3, 6 and 7 come from `fallback`, the
 * sequence's lists).
 */
static void fall_back(unsigned i, uint8_t l4[6][16], uint8_t l8[6][64], const uint8_t (*f4)[16],
		      const uint8_t (*f8)[64])
{
	unsigned size = i < 6 ? 16 : 64;
	uint8_t *dst = i < 6 ? l4[i] : l8[i - 6];
	const uint8_t *src;

	if (i == 0 || i == 3)
		src = f4 ? f4[i] : default_list(i);
	else if (i == 6 || i == 7)
		src = f8 ? f8[i - 6] : default_list(i);
	else if (i < 6)
		src = l4[i - 1];
	else
		src = l8[i - 8];
	memcpy(dst, src, size);
}

/* ---- parameter sets ---------------------------------------------------- */

static void skip_hrd(struct bits *b)
{
	uint32_t cpb_cnt = get_ue(b) + 1, i;

	get_bits(b, 8);
	for (i = 0; i < cpb_cnt && i < 32 && !b->err; i++) {
		get_ue(b);
		get_ue(b);
		get_bits(b, 1);
	}
	get_bits(b, 20);
}

static void parse_vui(struct bits *b, struct rdnvd_h264_sps *sps)
{
	int nal_hrd, vcl_hrd;

	if (get_bits(b, 1) && get_bits(b, 8) == 255)	/* aspect_ratio_info_present_flag, aspect_ratio_idc */
		get_bits(b, 32);
	if (get_bits(b, 1))				/* overscan_info_present_flag */
		get_bits(b, 1);
	if (get_bits(b, 1)) {				/* video_signal_type_present_flag */
		get_bits(b, 4);
		if (get_bits(b, 1))
			get_bits(b, 24);
	}
	if (get_bits(b, 1)) {				/* chroma_loc_info_present_flag */
		get_ue(b);
		get_ue(b);
	}
	if (get_bits(b, 1)) {				/* timing_info_present_flag */
		get_bits(b, 32);
		get_bits(b, 32);
		get_bits(b, 1);
	}
	nal_hrd = get_bits(b, 1);
	if (nal_hrd)
		skip_hrd(b);
	vcl_hrd = get_bits(b, 1);
	if (vcl_hrd)
		skip_hrd(b);
	if (nal_hrd || vcl_hrd)
		get_bits(b, 1);				/* low_delay_hrd_flag */
	get_bits(b, 1);					/* pic_struct_present_flag */
	if (get_bits(b, 1)) {				/* bitstream_restriction_flag */
		get_bits(b, 1);
		get_ue(b);
		get_ue(b);
		get_ue(b);
		get_ue(b);
		sps->max_num_reorder_frames = (uint8_t)get_ue(b);
		sps->max_dec_frame_buffering = (uint8_t)get_ue(b);
		if (!b->err)
			sps->vui_bitstream_restriction = 1;
	}
}

/* A.3.1, Table A-1: MaxDpbMbs. */
static uint32_t max_dpb_mbs(const struct rdnvd_h264_sps *sps)
{
	switch (sps->level_idc) {
	case 9:
	case 10:
		return 396;
	case 11:
		return (sps->constraint_flags & 0x10) && (sps->profile_idc == 66 || sps->profile_idc == 77 ||
							  sps->profile_idc == 88) ? 396 : 900;
	case 12:
	case 13:
	case 20:
		return 2376;
	case 21:
		return 4752;
	case 22:
	case 30:
		return 8100;
	case 31:
		return 18000;
	case 32:
		return 20480;
	case 40:
	case 41:
		return 32768;
	case 42:
		return 34816;
	case 50:
		return 110400;
	case 51:
	case 52:
		return 184320;
	default:
		return 696320;
	}
}

static int parse_sps(struct rdnvd_h264 *h, const uint8_t *nal, size_t size);

struct rdnvd_h264_dpb_fs {
	int used;
	int is_short, is_long, non_existing, needed_for_output;
	uint32_t frame_num, long_term_idx;
	int32_t poc[2], poc_frame;
	unsigned seq;			/* decoding order, for "newest first" */
	void *surface;
	uint64_t pts;
	struct rdnvd_h264_sps sps;	/* what the picture needs to be shown */
};

struct pending_slice {
	size_t offset, size;
};

struct slice_hdr {
	uint8_t nal_ref_idc, nal_unit_type, idr;
	uint32_t first_mb, slice_type, pps_id, frame_num;
	uint8_t field_pic, bottom_field;
	uint32_t idr_pic_id, poc_lsb, redundant_pic_cnt;
	int32_t delta_poc_bottom, delta_poc[2];
	uint8_t num_ref_idx_active_minus1[2];
	uint8_t no_output_of_prior_pics, long_term_reference_flag, adaptive_marking;
	unsigned num_mmco;
	struct { uint8_t op; uint32_t a, b; } mmco[68];
};

struct rdnvd_h264 {
	struct rdnvd_h264_callbacks cb;
	void *user;

	struct rdnvd_h264_sps sps[RDNVD_H264_MAX_SPS];
	struct rdnvd_h264_pps pps[RDNVD_H264_MAX_PPS];

	/* The sequence in force. */
	int have_seq, started;
	struct rdnvd_h264_sps seq;

	/* The picture being collected. */
	int pending;
	struct slice_hdr first;
	struct rdnvd_h264_sps pend_sps;
	struct rdnvd_h264_pps pend_pps;
	uint64_t pend_pts;
	uint8_t *data;
	size_t data_size, data_cap;
	struct pending_slice slices[RDNVD_H264_MAX_SLICES];
	unsigned num_slices;

	/* Picture order count and frame numbers of the previous pictures (8.2.1, 8.2.5.2). */
	int32_t prev_poc_msb, prev_poc_lsb;
	uint32_t prev_frame_num_offset, prev_frame_num, prev_ref_frame_num;
	unsigned decode_seq;

	/* The decoded picture buffer (C.4). */
	struct rdnvd_h264_dpb_fs dpb[RDNVD_H264_MAX_REFS + 1];
	int max_long_term_idx;		/* -1: none */
};

/* ---- SPS ---------------------------------------------------------------- */

static int parse_sps(struct rdnvd_h264 *h, const uint8_t *nal, size_t size)
{
	static const uint8_t high[] = { 100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135 };
	uint8_t rbsp[4096];
	struct bits b;
	struct rdnvd_h264_sps s;
	size_t n = unescape(nal, size, rbsp, sizeof(rbsp));
	unsigned i, lists;

	bits_init(&b, rbsp, n);
	bits_trim(&b, n);
	memset(&s, 0, sizeof(s));
	s.profile_idc = (uint8_t)get_bits(&b, 8);
	s.constraint_flags = (uint8_t)get_bits(&b, 8);
	s.level_idc = (uint8_t)get_bits(&b, 8);
	{
		uint32_t id = get_ue(&b);

		if (id >= RDNVD_H264_MAX_SPS)
			return RDNVD_H264_BAD_STREAM;
		s.id = (uint8_t)id;
	}
	s.chroma_format_idc = 1;
	for (i = 0; i < sizeof(high); i++)
		if (s.profile_idc == high[i])
			break;
	if (i < sizeof(high)) {
		s.chroma_format_idc = (uint8_t)get_ue(&b);
		if (s.chroma_format_idc == 3)
			s.separate_colour_plane_flag = (uint8_t)get_bits(&b, 1);
		s.bit_depth_luma_minus8 = (uint8_t)get_ue(&b);
		s.bit_depth_chroma_minus8 = (uint8_t)get_ue(&b);
		get_bits(&b, 1);	/* qpprime_y_zero_transform_bypass_flag */
		s.scaling_matrix_present = (uint8_t)get_bits(&b, 1);
	}
	if (b.err || s.chroma_format_idc > 3)
		return RDNVD_H264_BAD_STREAM;
	if (s.chroma_format_idc != 1 || s.bit_depth_luma_minus8 || s.bit_depth_chroma_minus8) {
		/* Not 4:2:0 at 8 bits: the card's decoder takes nothing else. Remember it, refuse at use. */
		s.valid = 2;
	}
	/* The lists: flat unless sent (7.4.2.1.1). */
	memset(s.scaling_list_4x4, 16, sizeof(s.scaling_list_4x4));
	memset(s.scaling_list_8x8, 16, sizeof(s.scaling_list_8x8));
	if (s.scaling_matrix_present) {
		lists = s.chroma_format_idc != 3 ? 8 : 12;
		for (i = 0; i < lists; i++) {
			if (get_bits(&b, 1)) {
				uint8_t *dst = i < 6 ? s.scaling_list_4x4[i] : s.scaling_list_8x8[i - 6];

				if (scaling_list(&b, dst, i < 6 ? 16 : 64))
					memcpy(dst, default_list(i), i < 6 ? 16 : 64);
			} else {
				fall_back(i, s.scaling_list_4x4, s.scaling_list_8x8, NULL, NULL);
			}
		}
	}
	s.log2_max_frame_num_minus4 = (uint8_t)get_ue(&b);
	s.pic_order_cnt_type = (uint8_t)get_ue(&b);
	if (s.pic_order_cnt_type == 0) {
		s.log2_max_pic_order_cnt_lsb_minus4 = (uint8_t)get_ue(&b);
	} else if (s.pic_order_cnt_type == 1) {
		uint32_t cycle;

		s.delta_pic_order_always_zero_flag = (uint8_t)get_bits(&b, 1);
		s.offset_for_non_ref_pic = get_se(&b);
		s.offset_for_top_to_bottom_field = get_se(&b);
		cycle = get_ue(&b);
		if (cycle > 255)
			return RDNVD_H264_BAD_STREAM;
		s.num_ref_frames_in_pic_order_cnt_cycle = (uint8_t)cycle;
		for (i = 0; i < cycle; i++)
			s.offset_for_ref_frame[i] = get_se(&b);
	} else if (s.pic_order_cnt_type != 2) {
		return RDNVD_H264_BAD_STREAM;
	}
	{
		uint32_t refs = get_ue(&b);

		s.max_num_ref_frames = (uint8_t)(refs > 16 ? 16 : refs);
	}
	s.gaps_in_frame_num_allowed_flag = (uint8_t)get_bits(&b, 1);
	s.pic_width_in_mbs = get_ue(&b) + 1;
	s.pic_height_in_map_units = get_ue(&b) + 1;
	s.frame_mbs_only_flag = (uint8_t)get_bits(&b, 1);
	if (!s.frame_mbs_only_flag)
		s.mb_adaptive_frame_field_flag = (uint8_t)get_bits(&b, 1);
	s.direct_8x8_inference_flag = (uint8_t)get_bits(&b, 1);
	if (s.pic_width_in_mbs > 1024 || s.pic_height_in_map_units > 1024)
		return RDNVD_H264_BAD_STREAM;
	s.width_mbs = s.pic_width_in_mbs;
	s.height_mbs = s.pic_height_in_map_units * (2 - s.frame_mbs_only_flag);
	if (get_bits(&b, 1)) {		/* frame_cropping_flag: units of 7.4.2.1.1 */
		uint32_t cx = s.chroma_format_idc == 0 || s.chroma_format_idc == 3 ? 1 : 2;
		uint32_t cy = (s.chroma_format_idc == 1 ? 2 : 1) * (2 - s.frame_mbs_only_flag);

		if (s.chroma_format_idc == 0)
			cy = 2 - s.frame_mbs_only_flag;
		s.crop_left = get_ue(&b) * cx;
		s.crop_right = get_ue(&b) * cx;
		s.crop_top = get_ue(&b) * cy;
		s.crop_bottom = get_ue(&b) * cy;
		if (s.crop_left + s.crop_right >= s.width_mbs * 16 ||
		    s.crop_top + s.crop_bottom >= s.height_mbs * 16)
			return RDNVD_H264_BAD_STREAM;
	}
	if (get_bits(&b, 1))		/* vui_parameters_present_flag */
		parse_vui(&b, &s);
	if (b.err && !s.vui_bitstream_restriction) {
		/* A VUI that runs off the end is ignored; the rest was read before it. */
		b.err = 0;
	}
	{
		uint32_t mbs = s.width_mbs * s.height_mbs, frames = max_dpb_mbs(&s) / (mbs ? mbs : 1);

		if (frames > 16)
			frames = 16;
		if (s.vui_bitstream_restriction && s.max_dec_frame_buffering <= 16)
			frames = s.max_dec_frame_buffering;
		if (frames < s.max_num_ref_frames)
			frames = s.max_num_ref_frames;
		if (frames < 1)
			frames = 1;
		s.dpb_frames = (uint8_t)frames;
	}
	if (!s.valid)
		s.valid = 1;
	h->sps[s.id] = s;
	return RDNVD_H264_OK;
}

/* ---- PPS ---------------------------------------------------------------- */

static int parse_pps(struct rdnvd_h264 *h, const uint8_t *nal, size_t size)
{
	uint8_t rbsp[4096];
	struct bits b;
	struct rdnvd_h264_pps p;
	size_t n = unescape(nal, size, rbsp, sizeof(rbsp));
	uint32_t id, sps_id;
	unsigned i;

	bits_init(&b, rbsp, n);
	bits_trim(&b, n);
	memset(&p, 0, sizeof(p));
	id = get_ue(&b);
	sps_id = get_ue(&b);
	if (id >= RDNVD_H264_MAX_PPS || sps_id >= RDNVD_H264_MAX_SPS)
		return RDNVD_H264_BAD_STREAM;
	p.id = (uint8_t)id;
	p.sps_id = (uint8_t)sps_id;
	p.entropy_coding_mode_flag = (uint8_t)get_bits(&b, 1);
	p.bottom_field_pic_order_in_frame_present_flag = (uint8_t)get_bits(&b, 1);
	p.num_slice_groups_minus1 = (uint8_t)get_ue(&b);
	if (p.num_slice_groups_minus1 > 7)
		return RDNVD_H264_BAD_STREAM;
	if (p.num_slice_groups_minus1) {
		unsigned g;

		p.slice_group_map_type = (uint8_t)get_ue(&b);
		if (p.slice_group_map_type == 0) {
			for (g = 0; g <= p.num_slice_groups_minus1; g++)
				get_ue(&b);
		} else if (p.slice_group_map_type == 2) {
			for (g = 0; g < p.num_slice_groups_minus1; g++) {
				get_ue(&b);
				get_ue(&b);
			}
		} else if (p.slice_group_map_type >= 3 && p.slice_group_map_type <= 5) {
			get_bits(&b, 1);
			p.slice_group_change_rate_minus1 = (uint16_t)get_ue(&b);
		} else if (p.slice_group_map_type == 6) {
			uint32_t units = get_ue(&b) + 1, bitsn = 0;

			while ((1u << bitsn) < (unsigned)p.num_slice_groups_minus1 + 1)
				bitsn++;
			for (i = 0; i < units && !b.err; i++)
				get_bits(&b, bitsn);
		}
	}
	p.num_ref_idx_default_active_minus1[0] = (uint8_t)get_ue(&b);
	p.num_ref_idx_default_active_minus1[1] = (uint8_t)get_ue(&b);
	p.weighted_pred_flag = (uint8_t)get_bits(&b, 1);
	p.weighted_bipred_idc = (uint8_t)get_bits(&b, 2);
	p.pic_init_qp_minus26 = (int8_t)get_se(&b);
	p.pic_init_qs_minus26 = (int8_t)get_se(&b);
	p.chroma_qp_index_offset = (int8_t)get_se(&b);
	p.deblocking_filter_control_present_flag = (uint8_t)get_bits(&b, 1);
	p.constrained_intra_pred_flag = (uint8_t)get_bits(&b, 1);
	p.redundant_pic_cnt_present_flag = (uint8_t)get_bits(&b, 1);
	p.second_chroma_qp_index_offset = p.chroma_qp_index_offset;
	if (b.err)
		return RDNVD_H264_BAD_STREAM;
	if (more_rbsp_data(&b)) {
		unsigned lists;
		unsigned chroma = h->sps[sps_id].valid ? h->sps[sps_id].chroma_format_idc : 1;

		p.transform_8x8_mode_flag = (uint8_t)get_bits(&b, 1);
		p.pic_scaling_matrix_present = (uint8_t)get_bits(&b, 1);
		if (p.pic_scaling_matrix_present) {
			lists = 6 + (chroma != 3 ? 2 : 6) * p.transform_8x8_mode_flag;
			for (i = 0; i < lists; i++) {
				p.list_present[i] = (uint8_t)get_bits(&b, 1);
				if (p.list_present[i]) {
					uint8_t *dst = i < 6 ? p.scaling_list_4x4[i] : p.scaling_list_8x8[i - 6];

					p.list_use_default[i] = (uint8_t)scaling_list(&b, dst, i < 6 ? 16 : 64);
				}
			}
		}
		p.second_chroma_qp_index_offset = (int8_t)get_se(&b);
	}
	if (b.err)
		return RDNVD_H264_BAD_STREAM;
	p.valid = 1;
	h->pps[p.id] = p;
	return RDNVD_H264_OK;
}

/* The lists in force for a picture: the PPS's over the sequence's (7.4.2.2). */
static void resolve_scaling(const struct rdnvd_h264_sps *sps, const struct rdnvd_h264_pps *pps,
			    uint8_t l4[6][16], uint8_t l8[6][64])
{
	unsigned i, lists;

	memcpy(l4, sps->scaling_list_4x4, 6 * 16);
	memcpy(l8, sps->scaling_list_8x8, 6 * 64);
	if (!pps->pic_scaling_matrix_present)
		return;
	lists = 6 + (sps->chroma_format_idc != 3 ? 2 : 6) * pps->transform_8x8_mode_flag;
	for (i = 0; i < lists; i++) {
		if (pps->list_present[i]) {
			uint8_t *dst = i < 6 ? l4[i] : l8[i - 6];
			const uint8_t *src = pps->list_use_default[i] ? default_list(i) :
					     i < 6 ? pps->scaling_list_4x4[i] : pps->scaling_list_8x8[i - 6];

			memcpy(dst, src, i < 6 ? 16 : 64);
		} else if (sps->scaling_matrix_present) {
			/* rule B: 0, 3, 6, 7 from the sequence's lists, the rest from the list before */
			fall_back(i, l4, l8, sps->scaling_list_4x4, sps->scaling_list_8x8);
		} else {
			/* rule A */
			fall_back(i, l4, l8, NULL, NULL);
		}
	}
}

/* ---- slice header (7.3.3) ------------------------------------------------ */

static int parse_slice_header(struct rdnvd_h264 *h, const uint8_t *nal, size_t size, struct slice_hdr *s,
			      int *sps_ok)
{
	uint8_t rbsp[2048];
	struct bits b;
	const struct rdnvd_h264_pps *pps;
	const struct rdnvd_h264_sps *sps;
	size_t n = unescape(nal, size > 1536 ? 1536 : size, rbsp, sizeof(rbsp));
	unsigned list, i, chroma_array_type;

	memset(s, 0, sizeof(*s));
	*sps_ok = 0;
	bits_init(&b, rbsp, n);
	s->nal_ref_idc = (nal[0] >> 5) & 3;
	s->nal_unit_type = nal[0] & 0x1f;
	s->idr = s->nal_unit_type == 5;
	s->first_mb = get_ue(&b);
	s->slice_type = get_ue(&b);
	if (s->slice_type > 9)
		return RDNVD_H264_BAD_STREAM;
	s->slice_type %= 5;
	s->pps_id = get_ue(&b);
	if (s->pps_id >= RDNVD_H264_MAX_PPS || !h->pps[s->pps_id].valid)
		return RDNVD_H264_BAD_STREAM;
	pps = &h->pps[s->pps_id];
	if (!h->sps[pps->sps_id].valid)
		return RDNVD_H264_BAD_STREAM;
	sps = &h->sps[pps->sps_id];
	if (sps->valid != 1)
		return RDNVD_H264_UNSUPPORTED;
	*sps_ok = 1;
	if (sps->separate_colour_plane_flag)
		get_bits(&b, 2);
	s->frame_num = get_bits(&b, sps->log2_max_frame_num_minus4 + 4);
	if (!sps->frame_mbs_only_flag) {
		s->field_pic = (uint8_t)get_bits(&b, 1);
		if (s->field_pic)
			s->bottom_field = (uint8_t)get_bits(&b, 1);
	}
	if (s->idr)
		s->idr_pic_id = get_ue(&b);
	if (sps->pic_order_cnt_type == 0) {
		s->poc_lsb = get_bits(&b, sps->log2_max_pic_order_cnt_lsb_minus4 + 4);
		if (pps->bottom_field_pic_order_in_frame_present_flag && !s->field_pic)
			s->delta_poc_bottom = get_se(&b);
	}
	if (sps->pic_order_cnt_type == 1 && !sps->delta_pic_order_always_zero_flag) {
		s->delta_poc[0] = get_se(&b);
		if (pps->bottom_field_pic_order_in_frame_present_flag && !s->field_pic)
			s->delta_poc[1] = get_se(&b);
	}
	if (pps->redundant_pic_cnt_present_flag)
		s->redundant_pic_cnt = get_ue(&b);
	if (s->slice_type == 1)
		get_bits(&b, 1);	/* direct_spatial_mv_pred_flag */
	s->num_ref_idx_active_minus1[0] = pps->num_ref_idx_default_active_minus1[0];
	s->num_ref_idx_active_minus1[1] = pps->num_ref_idx_default_active_minus1[1];
	if (s->slice_type == 0 || s->slice_type == 1 || s->slice_type == 3) {
		if (get_bits(&b, 1)) {
			uint32_t v = get_ue(&b);

			if (v > 31)
				return RDNVD_H264_BAD_STREAM;
			s->num_ref_idx_active_minus1[0] = (uint8_t)v;
			if (s->slice_type == 1) {
				v = get_ue(&b);
				if (v > 31)
					return RDNVD_H264_BAD_STREAM;
				s->num_ref_idx_active_minus1[1] = (uint8_t)v;
			}
		}
	}
	/* ref_pic_list_modification (7.3.3.1) */
	for (list = 0; list < (s->slice_type == 1 ? 2u : 1u); list++) {
		if (s->slice_type == 2 || s->slice_type == 4)
			break;
		if (get_bits(&b, 1)) {
			unsigned guard = 0;
			uint32_t idc;

			do {
				idc = get_ue(&b);
				if (idc == 0 || idc == 1 || idc == 2)
					get_ue(&b);
			} while (idc != 3 && !b.err && ++guard < 100);
		}
	}
	/* pred_weight_table (7.3.3.2) */
	if ((pps->weighted_pred_flag && (s->slice_type == 0 || s->slice_type == 3)) ||
	    (pps->weighted_bipred_idc == 1 && s->slice_type == 1)) {
		chroma_array_type = sps->separate_colour_plane_flag ? 0 : sps->chroma_format_idc;
		get_ue(&b);
		if (chroma_array_type)
			get_ue(&b);
		for (list = 0; list < (s->slice_type == 1 ? 2u : 1u); list++)
			for (i = 0; i <= s->num_ref_idx_active_minus1[list] && !b.err; i++) {
				if (get_bits(&b, 1)) {
					get_se(&b);
					get_se(&b);
				}
				if (chroma_array_type && get_bits(&b, 1)) {
					get_se(&b);
					get_se(&b);
					get_se(&b);
					get_se(&b);
				}
			}
	}
	/* dec_ref_pic_marking (7.3.3.3) */
	if (s->nal_ref_idc) {
		if (s->idr) {
			s->no_output_of_prior_pics = (uint8_t)get_bits(&b, 1);
			s->long_term_reference_flag = (uint8_t)get_bits(&b, 1);
		} else {
			s->adaptive_marking = (uint8_t)get_bits(&b, 1);
			if (s->adaptive_marking) {
				for (;;) {
					uint32_t op = get_ue(&b);

					if (op == 0 || b.err)
						break;
					if (op > 6 || s->num_mmco >= 66)
						return RDNVD_H264_BAD_STREAM;
					s->mmco[s->num_mmco].op = (uint8_t)op;
					s->mmco[s->num_mmco].a = 0;
					s->mmco[s->num_mmco].b = 0;
					if (op == 1 || op == 3)
						s->mmco[s->num_mmco].a = get_ue(&b);
					if (op == 2)
						s->mmco[s->num_mmco].a = get_ue(&b);
					if (op == 3 || op == 6)
						s->mmco[s->num_mmco].b = get_ue(&b);
					if (op == 4)
						s->mmco[s->num_mmco].a = get_ue(&b);
					s->num_mmco++;
				}
			}
		}
	}
	return b.err ? RDNVD_H264_BAD_STREAM : RDNVD_H264_OK;
}

/* 7.4.1.2.4: does this slice start another picture than `a`'s? */
static int first_slice_of_new_picture(const struct slice_hdr *a, const struct slice_hdr *b,
				      const struct rdnvd_h264_sps *sps, const struct rdnvd_h264_pps *pps_a,
				      const struct rdnvd_h264_pps *pps_b)
{
	if (a->frame_num != b->frame_num || a->pps_id != b->pps_id || a->field_pic != b->field_pic ||
	    a->bottom_field != b->bottom_field)
		return 1;
	if ((a->nal_ref_idc == 0) != (b->nal_ref_idc == 0))
		return 1;
	if (sps->pic_order_cnt_type == 0 &&
	    (a->poc_lsb != b->poc_lsb || a->delta_poc_bottom != b->delta_poc_bottom))
		return 1;
	if (sps->pic_order_cnt_type == 1 &&
	    (a->delta_poc[0] != b->delta_poc[0] || a->delta_poc[1] != b->delta_poc[1]))
		return 1;
	if (a->idr != b->idr)
		return 1;
	if (a->idr && b->idr && a->idr_pic_id != b->idr_pic_id)
		return 1;
	(void)pps_a;
	(void)pps_b;
	return 0;
}

/* ---- the decoded picture buffer (C.4, 8.2.5) ---------------------------- */

static void fs_release(struct rdnvd_h264 *h, struct rdnvd_h264_dpb_fs *fs)
{
	if (fs->surface && h->cb.release)
		h->cb.release(h->user, fs->surface);
	memset(fs, 0, sizeof(*fs));
}

/* Free frame stores that are neither waiting for output nor a reference. */
static void dpb_clean(struct rdnvd_h264 *h)
{
	unsigned i;

	for (i = 0; i < RDNVD_H264_MAX_REFS + 1; i++) {
		struct rdnvd_h264_dpb_fs *fs = &h->dpb[i];

		if (fs->used && !fs->needed_for_output && !fs->is_short && !fs->is_long)
			fs_release(h, fs);
	}
}

static unsigned dpb_count(const struct rdnvd_h264 *h)
{
	unsigned i, n = 0;

	for (i = 0; i < RDNVD_H264_MAX_REFS + 1; i++)
		n += h->dpb[i].used;
	return n;
}

static unsigned dpb_waiting(const struct rdnvd_h264 *h)
{
	unsigned i, n = 0;

	for (i = 0; i < RDNVD_H264_MAX_REFS + 1; i++)
		n += h->dpb[i].used && h->dpb[i].needed_for_output;
	return n;
}

/* C.4.5.3: output the picture with the smallest order count. False if nothing waits. */
static int dpb_bump(struct rdnvd_h264 *h)
{
	struct rdnvd_h264_dpb_fs *best = NULL;
	unsigned i;

	for (i = 0; i < RDNVD_H264_MAX_REFS + 1; i++) {
		struct rdnvd_h264_dpb_fs *fs = &h->dpb[i];

		if (fs->used && fs->needed_for_output && (!best || fs->poc_frame < best->poc_frame))
			best = fs;
	}
	if (!best)
		return 0;
	best->needed_for_output = 0;
	if (best->surface && h->cb.output)
		h->cb.output(h->user, best->surface, best->pts, &best->sps);
	if (!best->is_short && !best->is_long)
		fs_release(h, best);
	return 1;
}

/* Output everything that waits, then empty the buffer. */
static void dpb_flush(struct rdnvd_h264 *h, int drop)
{
	unsigned i;

	if (!drop)
		while (dpb_bump(h))
			;
	for (i = 0; i < RDNVD_H264_MAX_REFS + 1; i++)
		if (h->dpb[i].used)
			fs_release(h, &h->dpb[i]);
	h->max_long_term_idx = -1;
}

static struct rdnvd_h264_dpb_fs *dpb_free_slot(struct rdnvd_h264 *h)
{
	unsigned i;

	for (i = 0; i < RDNVD_H264_MAX_REFS + 1; i++)
		if (!h->dpb[i].used)
			return &h->dpb[i];
	return NULL;
}

static int32_t frame_num_wrap(const struct rdnvd_h264 *h, uint32_t frame_num, uint32_t cur)
{
	int32_t max = 1 << (h->seq.log2_max_frame_num_minus4 + 4);

	return frame_num > cur ? (int32_t)frame_num - max : (int32_t)frame_num;
}

/* 8.2.5.3: the sliding window. */
static void sliding_window(struct rdnvd_h264 *h, uint32_t cur_frame_num)
{
	unsigned i, shorts = 0, longs = 0, limit = h->seq.max_num_ref_frames ? h->seq.max_num_ref_frames : 1;
	struct rdnvd_h264_dpb_fs *oldest = NULL;

	for (i = 0; i < RDNVD_H264_MAX_REFS + 1; i++) {
		struct rdnvd_h264_dpb_fs *fs = &h->dpb[i];

		if (!fs->used)
			continue;
		if (fs->is_short) {
			shorts++;
			if (!oldest || frame_num_wrap(h, fs->frame_num, cur_frame_num) <
				       frame_num_wrap(h, oldest->frame_num, cur_frame_num))
				oldest = fs;
		}
		longs += fs->is_long;
	}
	if (shorts + longs >= limit && shorts && oldest)
		oldest->is_short = 0;
}

static struct rdnvd_h264_dpb_fs *find_short(struct rdnvd_h264 *h, int32_t pic_num, uint32_t cur_frame_num)
{
	unsigned i;

	for (i = 0; i < RDNVD_H264_MAX_REFS + 1; i++) {
		struct rdnvd_h264_dpb_fs *fs = &h->dpb[i];

		if (fs->used && fs->is_short && frame_num_wrap(h, fs->frame_num, cur_frame_num) == pic_num)
			return fs;
	}
	return NULL;
}

static void free_long_idx(struct rdnvd_h264 *h, uint32_t idx, struct rdnvd_h264_dpb_fs *except)
{
	unsigned i;

	for (i = 0; i < RDNVD_H264_MAX_REFS + 1; i++) {
		struct rdnvd_h264_dpb_fs *fs = &h->dpb[i];

		if (fs->used && fs->is_long && fs->long_term_idx == idx && fs != except)
			fs->is_long = 0;
	}
}

/*
 * 8.2.5: mark the references after the current picture (not yet in the buffer) is decoded.
 * Returns whether the current picture became a long-term one (with *long_idx); sets *mmco5.
 */
static int mark_references(struct rdnvd_h264 *h, const struct slice_hdr *s, int *mmco5, uint32_t *long_idx)
{
	unsigned i, k;
	int cur_long = 0;

	*mmco5 = 0;
	*long_idx = 0;
	if (!s->nal_ref_idc)
		return 0;
	if (s->idr) {
		for (i = 0; i < RDNVD_H264_MAX_REFS + 1; i++)
			h->dpb[i].is_short = h->dpb[i].is_long = 0;
		if (s->long_term_reference_flag) {
			cur_long = 1;
			h->max_long_term_idx = 0;
		} else {
			h->max_long_term_idx = -1;
		}
		return cur_long;
	}
	if (!s->adaptive_marking) {
		sliding_window(h, s->frame_num);
		return 0;
	}
	for (k = 0; k < s->num_mmco; k++) {
		uint32_t a = s->mmco[k].a, b = s->mmco[k].b;
		struct rdnvd_h264_dpb_fs *fs;

		switch (s->mmco[k].op) {
		case 1:
			fs = find_short(h, (int32_t)s->frame_num - (int32_t)(a + 1), s->frame_num);
			if (fs)
				fs->is_short = 0;
			break;
		case 2:
			for (i = 0; i < RDNVD_H264_MAX_REFS + 1; i++)
				if (h->dpb[i].used && h->dpb[i].is_long && h->dpb[i].long_term_idx == a)
					h->dpb[i].is_long = 0;
			break;
		case 3:
			fs = find_short(h, (int32_t)s->frame_num - (int32_t)(a + 1), s->frame_num);
			if (fs) {
				free_long_idx(h, b, fs);
				fs->is_short = 0;
				fs->is_long = 1;
				fs->long_term_idx = b;
			}
			break;
		case 4:
			h->max_long_term_idx = (int)a - 1;
			for (i = 0; i < RDNVD_H264_MAX_REFS + 1; i++)
				if (h->dpb[i].used && h->dpb[i].is_long && (int)h->dpb[i].long_term_idx > h->max_long_term_idx)
					h->dpb[i].is_long = 0;
			break;
		case 5:
			for (i = 0; i < RDNVD_H264_MAX_REFS + 1; i++)
				h->dpb[i].is_short = h->dpb[i].is_long = 0;
			h->max_long_term_idx = -1;
			*mmco5 = 1;
			break;
		case 6:
			free_long_idx(h, b, NULL);
			cur_long = 1;
			*long_idx = b;
			break;
		}
	}
	return cur_long;
}

/* ---- picture order count (8.2.1) and frame number gaps (8.2.5.2) ------- */

struct poc_info {
	int32_t poc[2];
	int32_t msb;
	uint32_t frame_num_offset;
};

static void compute_poc(const struct rdnvd_h264 *h, const struct rdnvd_h264_sps *sps, const struct slice_hdr *s,
			struct poc_info *out)
{
	uint32_t max_frame_num = 1u << (sps->log2_max_frame_num_minus4 + 4);

	memset(out, 0, sizeof(*out));
	if (sps->pic_order_cnt_type == 0) {
		int32_t prev_msb = s->idr ? 0 : h->prev_poc_msb, prev_lsb = s->idr ? 0 : h->prev_poc_lsb;
		int32_t max_lsb = 1 << (sps->log2_max_pic_order_cnt_lsb_minus4 + 4), lsb = (int32_t)s->poc_lsb;

		if (lsb < prev_lsb && prev_lsb - lsb >= max_lsb / 2)
			out->msb = prev_msb + max_lsb;
		else if (lsb > prev_lsb && lsb - prev_lsb > max_lsb / 2)
			out->msb = prev_msb - max_lsb;
		else
			out->msb = prev_msb;
		out->poc[0] = out->msb + lsb;
		out->poc[1] = out->poc[0] + s->delta_poc_bottom;
		return;
	}
	if (s->idr)
		out->frame_num_offset = 0;
	else if (h->prev_frame_num > s->frame_num)
		out->frame_num_offset = h->prev_frame_num_offset + max_frame_num;
	else
		out->frame_num_offset = h->prev_frame_num_offset;
	if (sps->pic_order_cnt_type == 1) {
		int32_t abs_frame_num = 0, expected = 0, delta_cycle = 0, i;

		if (sps->num_ref_frames_in_pic_order_cnt_cycle)
			abs_frame_num = (int32_t)(out->frame_num_offset + s->frame_num);
		if (!s->nal_ref_idc && abs_frame_num > 0)
			abs_frame_num--;
		for (i = 0; i < sps->num_ref_frames_in_pic_order_cnt_cycle; i++)
			delta_cycle += sps->offset_for_ref_frame[i];
		if (abs_frame_num > 0) {
			int32_t cycles = (abs_frame_num - 1) / sps->num_ref_frames_in_pic_order_cnt_cycle;
			int32_t in_cycle = (abs_frame_num - 1) % sps->num_ref_frames_in_pic_order_cnt_cycle;

			expected = cycles * delta_cycle;
			for (i = 0; i <= in_cycle; i++)
				expected += sps->offset_for_ref_frame[i];
		}
		if (!s->nal_ref_idc)
			expected += sps->offset_for_non_ref_pic;
		out->poc[0] = expected + s->delta_poc[0];
		out->poc[1] = out->poc[0] + sps->offset_for_top_to_bottom_field + s->delta_poc[1];
		return;
	}
	/* type 2 */
	if (s->idr)
		out->poc[0] = 0;
	else
		out->poc[0] = 2 * (int32_t)(out->frame_num_offset + s->frame_num) - (s->nal_ref_idc ? 0 : 1);
	out->poc[1] = out->poc[0];
}

/*
 * Make room in the buffer for one more frame: output pictures until there is a free store (C.4.5.3).
 * If nothing can be output and the buffer is full of references, the oldest short-term one goes.
 */
static struct rdnvd_h264_dpb_fs *dpb_make_room(struct rdnvd_h264 *h)
{
	for (;;) {
		dpb_clean(h);
		if (dpb_count(h) < h->seq.dpb_frames)
			return dpb_free_slot(h);
		if (!dpb_bump(h)) {
			struct rdnvd_h264_dpb_fs *oldest = NULL;
			unsigned i;

			for (i = 0; i < RDNVD_H264_MAX_REFS + 1; i++) {
				struct rdnvd_h264_dpb_fs *fs = &h->dpb[i];

				if (fs->used && fs->is_short && (!oldest || fs->seq < oldest->seq))
					oldest = fs;
			}
			if (!oldest)
				return dpb_free_slot(h);
			oldest->is_short = 0;
		}
	}
}

/* 8.2.5.2: frames the stream skipped, as "non-existing" short-term references. */
static void fill_frame_num_gap(struct rdnvd_h264 *h, const struct rdnvd_h264_sps *sps, uint32_t frame_num)
{
	uint32_t max = 1u << (sps->log2_max_frame_num_minus4 + 4);
	uint32_t unused = (h->prev_ref_frame_num + 1) % max;
	unsigned guard = 0;

	while (unused != frame_num && guard++ < max) {
		struct rdnvd_h264_dpb_fs *fs;

		sliding_window(h, unused);
		fs = dpb_make_room(h);
		if (!fs)
			return;
		memset(fs, 0, sizeof(*fs));
		fs->used = 1;
		fs->is_short = 1;
		fs->non_existing = 1;
		fs->frame_num = unused;
		fs->seq = h->decode_seq++;
		fs->sps = *sps;
		if (unused < h->prev_frame_num)
			h->prev_frame_num_offset += max;
		h->prev_frame_num = unused;
		h->prev_ref_frame_num = unused;
		unused = (unused + 1) % max;
	}
}

/* ---- finishing a picture ------------------------------------------------- */

static int same_sequence(const struct rdnvd_h264_sps *a, const struct rdnvd_h264_sps *b)
{
	return a->width_mbs == b->width_mbs && a->height_mbs == b->height_mbs &&
	       a->max_num_ref_frames == b->max_num_ref_frames && a->profile_idc == b->profile_idc &&
	       a->level_idc == b->level_idc && a->frame_mbs_only_flag == b->frame_mbs_only_flag &&
	       a->mb_adaptive_frame_field_flag == b->mb_adaptive_frame_field_flag && a->dpb_frames == b->dpb_frames &&
	       a->crop_left == b->crop_left && a->crop_right == b->crop_right && a->crop_top == b->crop_top &&
	       a->crop_bottom == b->crop_bottom;
}

/* How many pictures may wait for output before the first must go (-1: as many as the buffer holds). */
static int reorder_limit(const struct rdnvd_h264_sps *sps)
{
	if (sps->vui_bitstream_restriction)
		return sps->max_num_reorder_frames;
	/* Without the VUI: no B pictures in Baseline, and order count type 2 cannot reorder. */
	if (sps->pic_order_cnt_type == 2 || sps->profile_idc == 66)
		return 0;
	return -1;
}

static int finish_picture(struct rdnvd_h264 *h)
{
	struct slice_hdr *s = &h->first;
	const struct rdnvd_h264_sps *sps = &h->pend_sps;
	struct rdnvd_h264_picture pic;
	struct rdnvd_h264_slice slices[RDNVD_H264_MAX_SLICES];
	struct poc_info poc;
	struct rdnvd_h264_dpb_fs *fs;
	void *surface = NULL;
	uint32_t long_idx = 0, max_frame_num = 1u << (sps->log2_max_frame_num_minus4 + 4);
	int err = RDNVD_H264_OK, r, mmco5 = 0, cur_long, limit;
	unsigned i, n;

	if (!h->pending)
		return RDNVD_H264_OK;
	h->pending = 0;

	/* A stream is joined at an IDR picture, or at an I picture. */
	if (!h->started && !(s->idr || s->slice_type == 2 || s->slice_type == 4))
		return RDNVD_H264_OK;
	if (!h->have_seq || ((s->idr || !h->started) && !same_sequence(&h->seq, sps))) {
		dpb_flush(h, 0);
		h->seq = *sps;
		h->have_seq = 1;
		if (h->cb.sequence && (r = h->cb.sequence(h->user, sps)) != 0) {
			h->have_seq = 0;
			return r;
		}
	} else if (s->idr || !h->started) {
		h->seq = *sps;
	}
	if (!h->started) {
		h->started = 1;
		h->prev_ref_frame_num = s->idr ? 0 : (s->frame_num + max_frame_num - 1) % max_frame_num;
		h->prev_frame_num = h->prev_ref_frame_num;
		h->prev_frame_num_offset = 0;
		h->prev_poc_msb = h->prev_poc_lsb = 0;
	}
	if (s->idr)
		dpb_flush(h, 0);
	else if (s->frame_num != h->prev_ref_frame_num && s->frame_num != (h->prev_ref_frame_num + 1) % max_frame_num)
		fill_frame_num_gap(h, sps, s->frame_num);

	compute_poc(h, sps, s, &poc);

	/* What the decoder is told. */
	memset(&pic, 0, sizeof(pic));
	pic.sps = sps;
	pic.pps = &h->pend_pps;
	pic.idr = s->idr;
	pic.nal_ref_idc = s->nal_ref_idc;
	pic.is_reference = s->nal_ref_idc != 0;
	pic.frame_num = s->frame_num;
	pic.poc[0] = poc.poc[0];
	pic.poc[1] = poc.poc[1];
	pic.slice_type = s->slice_type == 3 ? 0 : s->slice_type == 4 ? 2 : s->slice_type;
	pic.num_ref_idx_active_minus1[0] = s->num_ref_idx_active_minus1[0];
	pic.num_ref_idx_active_minus1[1] = s->num_ref_idx_active_minus1[1];
	resolve_scaling(sps, &h->pend_pps, pic.scaling_list_4x4, pic.scaling_list_8x8);
	pic.pts = h->pend_pts;
	/* The references: short-term, newest first; then long-term by index. */
	{
		const struct rdnvd_h264_dpb_fs *order[RDNVD_H264_MAX_REFS + 1];
		unsigned count = 0, a, b;

		for (i = 0; i < RDNVD_H264_MAX_REFS + 1; i++)
			if (h->dpb[i].used && (h->dpb[i].is_short || h->dpb[i].is_long))
				order[count++] = &h->dpb[i];
		for (a = 1; a < count; a++) {
			const struct rdnvd_h264_dpb_fs *x = order[a];

			for (b = a; b > 0; b--) {
				const struct rdnvd_h264_dpb_fs *y = order[b - 1];
				int before = x->is_short != y->is_short ? x->is_short :
					     x->is_short ? x->seq > y->seq : x->long_term_idx < y->long_term_idx;

				if (!before)
					break;
				order[b] = y;
			}
			order[b] = x;
		}
		for (a = 0; a < count && pic.num_refs < RDNVD_H264_MAX_REFS; a++) {
			struct rdnvd_h264_ref *ref = &pic.refs[pic.num_refs++];

			ref->frame_num = order[a]->is_long ? order[a]->long_term_idx : order[a]->frame_num;
			ref->poc[0] = order[a]->poc[0];
			ref->poc[1] = order[a]->poc[1];
			ref->is_long_term = (uint8_t)order[a]->is_long;
			ref->is_non_existing = (uint8_t)order[a]->non_existing;
			ref->surface = order[a]->surface;
		}
	}
	n = h->num_slices;
	for (i = 0; i < n; i++) {
		slices[i].nal = h->data + h->slices[i].offset;
		slices[i].size = h->slices[i].size;
	}
	pic.num_slices = n;
	pic.slices = slices;

	if (h->cb.decode) {
		r = h->cb.decode(h->user, &pic, &surface);
		if (r)
			err = RDNVD_H264_DECODE_FAILED;
	}

	/* 8.2.5: marking, then C.4.4/C.4.5: output and storage. */
	cur_long = mark_references(h, s, &mmco5, &long_idx);
	if (mmco5) {
		int32_t t = poc.poc[0] < poc.poc[1] ? poc.poc[0] : poc.poc[1];

		poc.poc[0] -= t;
		poc.poc[1] -= t;
		dpb_flush(h, 0);
	}
	if (cur_long)
		free_long_idx(h, long_idx, NULL);
	dpb_clean(h);

	limit = reorder_limit(sps);
	fs = NULL;
	if (!s->nal_ref_idc) {
		/* C.4.5.2: a non-reference picture that would be output first goes straight out. */
		int32_t cur = poc.poc[0] < poc.poc[1] ? poc.poc[0] : poc.poc[1];
		int lowest = 1;

		for (i = 0; i < RDNVD_H264_MAX_REFS + 1; i++)
			if (h->dpb[i].used && h->dpb[i].needed_for_output && h->dpb[i].poc_frame < cur)
				lowest = 0;
		if (lowest && (dpb_count(h) >= h->seq.dpb_frames || limit == 0)) {
			if (surface) {
				if (h->cb.output)
					h->cb.output(h->user, surface, h->pend_pts, sps);
				if (h->cb.release)
					h->cb.release(h->user, surface);
			}
			goto state;
		}
	}
	fs = dpb_make_room(h);
	if (fs) {
		memset(fs, 0, sizeof(*fs));
		fs->used = 1;
		fs->is_short = s->nal_ref_idc != 0 && !cur_long;
		fs->is_long = cur_long;
		fs->long_term_idx = long_idx;
		fs->needed_for_output = surface != NULL;
		fs->frame_num = s->frame_num;
		fs->poc[0] = poc.poc[0];
		fs->poc[1] = poc.poc[1];
		fs->poc_frame = poc.poc[0] < poc.poc[1] ? poc.poc[0] : poc.poc[1];
		fs->seq = h->decode_seq++;
		fs->surface = surface;
		fs->pts = h->pend_pts;
		fs->sps = *sps;
	} else if (surface && h->cb.release) {
		h->cb.release(h->user, surface);
	}
	if (limit >= 0)
		while ((int)dpb_waiting(h) > limit && dpb_bump(h))
			;

state:
	/* The state the next picture's order count and frame number are counted from. */
	if (sps->pic_order_cnt_type == 0) {
		if (s->nal_ref_idc) {
			if (mmco5) {
				h->prev_poc_msb = 0;
				h->prev_poc_lsb = poc.poc[0];
			} else {
				h->prev_poc_msb = poc.msb;
				h->prev_poc_lsb = (int32_t)s->poc_lsb;
			}
		}
	} else {
		h->prev_frame_num_offset = mmco5 ? 0 : poc.frame_num_offset;
	}
	h->prev_frame_num = mmco5 ? 0 : s->frame_num;
	if (s->nal_ref_idc)
		h->prev_ref_frame_num = mmco5 ? 0 : s->frame_num;
	return err;
}

/* ---- NAL units ------------------------------------------------------------ */

static int add_slice(struct rdnvd_h264 *h, const uint8_t *nal, size_t size)
{
	if (h->num_slices >= RDNVD_H264_MAX_SLICES)
		return RDNVD_H264_BAD_STREAM;
	if (h->data_size + size > h->data_cap) {
		size_t cap = h->data_cap ? h->data_cap : 65536;
		uint8_t *p;

		while (cap < h->data_size + size)
			cap *= 2;
		p = realloc(h->data, cap);
		if (!p)
			return RDNVD_H264_NO_MEMORY;
		h->data = p;
		h->data_cap = cap;
	}
	memcpy(h->data + h->data_size, nal, size);
	h->slices[h->num_slices].offset = h->data_size;
	h->slices[h->num_slices].size = size;
	h->num_slices++;
	h->data_size += size;
	return RDNVD_H264_OK;
}

static int handle_slice(struct rdnvd_h264 *h, const uint8_t *nal, size_t size, uint64_t pts)
{
	struct slice_hdr s;
	int sps_ok, r, err = RDNVD_H264_OK;

	r = parse_slice_header(h, nal, size, &s, &sps_ok);
	if (r)
		return r;
	if (s.field_pic)
		return RDNVD_H264_UNSUPPORTED;
	if (s.redundant_pic_cnt)
		return RDNVD_H264_OK;
	if (h->pending &&
	    first_slice_of_new_picture(&h->first, &s, &h->pend_sps, &h->pend_pps, &h->pps[s.pps_id])) {
		err = finish_picture(h);
		if (err == RDNVD_H264_UNSUPPORTED || err == RDNVD_H264_BAD_STREAM)
			return err;
	}
	if (!h->pending) {
		const struct rdnvd_h264_pps *pps = &h->pps[s.pps_id];

		h->first = s;
		h->pend_pps = *pps;
		h->pend_sps = h->sps[pps->sps_id];
		h->pend_pts = pts;
		h->data_size = 0;
		h->num_slices = 0;
		h->pending = 1;
	}
	r = add_slice(h, nal, size);
	return r ? r : err;
}

static int handle_nal(struct rdnvd_h264 *h, const uint8_t *nal, size_t size, uint64_t pts)
{
	if (size < 1)
		return RDNVD_H264_OK;
	switch (nal[0] & 0x1f) {
	case 1:
	case 5:
		return handle_slice(h, nal, size, pts);
	case 7:
		return parse_sps(h, nal, size);
	case 8:
		return parse_pps(h, nal, size);
	case 2:
	case 3:
	case 4:
		return RDNVD_H264_UNSUPPORTED;
	case 6:
	case 9:
	case 10:
	case 11:
		return finish_picture(h);
	default:
		return RDNVD_H264_OK;
	}
}

/* ---- the interface --------------------------------------------------------- */

struct rdnvd_h264 *rdnvd_h264_parser_new(const struct rdnvd_h264_callbacks *cb, void *user)
{
	struct rdnvd_h264 *h = calloc(1, sizeof(*h));

	if (!h)
		return NULL;
	h->cb = *cb;
	h->user = user;
	h->max_long_term_idx = -1;
	return h;
}

void rdnvd_h264_parser_free(struct rdnvd_h264 *h)
{
	if (!h)
		return;
	rdnvd_h264_parser_flush(h, 1);
	free(h->data);
	free(h);
}

int rdnvd_h264_parser_avcc(struct rdnvd_h264 *h, const uint8_t *avcc, size_t size)
{
	size_t pos = 6;
	unsigned count, i, round;
	int r;

	if (size < 7 || avcc[0] != 1)
		return RDNVD_H264_BAD_STREAM;
	count = avcc[5] & 0x1f;
	for (round = 0; round < 2; round++) {
		for (i = 0; i < count; i++) {
			size_t len;

			if (pos + 2 > size)
				return RDNVD_H264_BAD_STREAM;
			len = (size_t)avcc[pos] << 8 | avcc[pos + 1];
			pos += 2;
			if (pos + len > size)
				return RDNVD_H264_BAD_STREAM;
			r = handle_nal(h, avcc + pos, len, 0);
			if (r && r != RDNVD_H264_UNSUPPORTED)
				return r;
			pos += len;
		}
		if (round == 0) {
			if (pos >= size)
				return RDNVD_H264_BAD_STREAM;
			count = avcc[pos++];
		}
	}
	return (avcc[4] & 3) + 1;
}

int rdnvd_h264_parser_feed_annexb(struct rdnvd_h264 *h, const uint8_t *data, size_t size, uint64_t pts)
{
	size_t i = 0;
	int err = RDNVD_H264_OK, r;

	while (i + 3 <= size) {
		size_t start, end;

		if (data[i] || data[i + 1] || data[i + 2] != 1) {
			i++;
			continue;
		}
		start = i + 3;
		for (end = start; end + 3 <= size && !(data[end] == 0 && data[end + 1] == 0 && data[end + 2] == 1);
		     end++)
			;
		if (end + 3 > size)
			end = size;
		/* trailing_zero_8bits and the zero that makes a four-byte start code */
		while (end > start && !data[end - 1])
			end--;
		r = handle_nal(h, data + start, end - start, pts);
		if (r && !err)
			err = r;
		i = end;
	}
	return err;
}

int rdnvd_h264_parser_feed_avcc(struct rdnvd_h264 *h, const uint8_t *data, size_t size, unsigned nal_length_size,
			 uint64_t pts)
{
	size_t pos = 0;
	int err = RDNVD_H264_OK, r;

	if (nal_length_size < 1 || nal_length_size > 4)
		return RDNVD_H264_BAD_STREAM;
	while (pos + nal_length_size <= size) {
		size_t len = 0, i;

		for (i = 0; i < nal_length_size; i++)
			len = len << 8 | data[pos + i];
		pos += nal_length_size;
		if (len > size - pos)
			return RDNVD_H264_BAD_STREAM;
		r = handle_nal(h, data + pos, len, pts);
		if (r && !err)
			err = r;
		pos += len;
	}
	return err;
}

int rdnvd_h264_parser_end_au(struct rdnvd_h264 *h)
{
	return finish_picture(h);
}

int rdnvd_h264_parser_flush(struct rdnvd_h264 *h, int drop)
{
	int err = RDNVD_H264_OK;

	if (drop)
		h->pending = 0;
	else
		err = finish_picture(h);
	dpb_flush(h, drop);
	/* The next picture must be one to start from. */
	h->started = 0;
	h->have_seq = h->have_seq && !drop;
	return err;
}
