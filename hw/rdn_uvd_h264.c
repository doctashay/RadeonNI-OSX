/*
 * Message layout and legacy DPB sizing: Mesa 26.2.4 radeon_uvd.{c,h}.
 * Copyright 2011 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: MIT
 */
#include "rdn_uvd_h264.h"
#ifdef KERNEL
#include <libkern/libkern.h>
#else
#include <string.h>
#endif

/* Firmware offsets are explicit: compiler padding and CPU endian are irrelevant. */
enum {
	MSG_CODEC = 224,
	MSG_EXTENSION = 3296,
	H264_SCALING4 = 36,
	H264_SCALING8 = 132,
	H264_FRAME_NUM = 260,
	H264_FRAME_NUM_LIST = 264,
	H264_CURR_POC = 328,
	H264_REF_POC = 336,
	H264_DECODED_PIC = 464
};

static void put_le32(uint8_t *out, uint32_t value)
{
	out[0] = (uint8_t)value;
	out[1] = (uint8_t)(value >> 8);
	out[2] = (uint8_t)(value >> 16);
	out[3] = (uint8_t)(value >> 24);
}

static uint64_t align_up(uint64_t value, uint32_t alignment)
{
	return (value + alignment - 1) & ~((uint64_t)alignment - 1);
}

static int zero_words(const uint32_t *p, unsigned count)
{
	unsigned i;
	for (i = 0; i < count; i++)
		if (p[i])
			return 0;
	return 1;
}

static int ranges_overlap(const void *a, size_t an, const void *b, size_t bn)
{
	uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
	if (x > UINTPTR_MAX - an || y > UINTPTR_MAX - bn)
		return 1;
	return x < y + bn && y < x + an;
}

static int valid_config(const struct rdn_uvd_h264_config *c)
{
	uint32_t fs, dpb_mbs;
	uint64_t width_mbs, height_mbs, mbs;
	if (!c || c->version != RDN_UVD_H264_VERSION ||
	    (c->profile_idc != 66 && c->profile_idc != 77 && c->profile_idc != 100) ||
	    !c->coded_width || c->coded_width > RDN_UVD_H264_MAX_WIDTH ||
	    !c->coded_height || c->coded_height > RDN_UVD_H264_MAX_HEIGHT ||
	    (c->coded_width & 15) || (c->coded_height & 15) ||
	    c->max_num_ref_frames > RDN_UVD_H264_MAX_REFS ||
	    c->chroma_format_idc != 1 || c->bit_depth_luma_minus8 ||
	    c->bit_depth_chroma_minus8 || c->frame_mbs_only_flag != 1 ||
	    c->mb_adaptive_frame_field_flag || !zero_words(c->reserved, 3))
		return 0;
	/* H.264 table A-1: MaxFS and MaxDpbMbs. Frame rate/bitrate need stream timing. */
	switch (c->level_idc) {
	case 10: fs = 99; dpb_mbs = 396; break;
	case 11: fs = 396; dpb_mbs = 900; break;
	case 12: case 13: case 20: fs = 396; dpb_mbs = 2376; break;
	case 21: fs = 792; dpb_mbs = 4752; break;
	case 22: case 30: fs = 1620; dpb_mbs = 8100; break;
	case 31: fs = 3600; dpb_mbs = 18000; break;
	case 32: fs = 5120; dpb_mbs = 20480; break;
	case 40: case 41: fs = 8192; dpb_mbs = 32768; break;
	case 42: fs = 8704; dpb_mbs = 34816; break;
	default: return 0;
	}
	width_mbs = c->coded_width / 16;
	height_mbs = c->coded_height / 16;
	mbs = width_mbs * height_mbs;
	return mbs <= fs && width_mbs * width_mbs <= 8u * fs &&
	       height_mbs * height_mbs <= 8u * fs &&
	       mbs * c->max_num_ref_frames <= dpb_mbs;
}

int rdn_uvd_h264_layout_init(const struct rdn_uvd_h264_config *c,
	uint32_t field_mode, struct rdn_uvd_h264_layout *output)
{
	struct rdn_uvd_h264_layout l;
	uint64_t image, mbs, dpb, pitch, rows, target, packed;
	if (!output || !valid_config(c) || field_mode > RDN_UVD_H264_TARGET_FIELDS ||
	    ranges_overlap(output, sizeof(*output), c, sizeof(*c)))
		return -1;
	memset(&l, 0, sizeof(l));
	/* Legacy UVD always reserves 17 reference images, regardless of SPS count. */
	image = align_up((uint64_t)c->coded_width * c->coded_height * 3 / 2, 1024);
	mbs = (c->coded_width / 16) * align_up(c->coded_height / 16, 2);
	dpb = image * 17 + mbs * 17 * 192 + mbs * 32;
	pitch = align_up(c->coded_width, 256);
	rows = field_mode ? 2 * align_up(c->coded_height / 2, 16) : c->coded_height;
	target = pitch * rows * 3 / 2;
	packed = (uint64_t)c->coded_width * c->coded_height * 3 / 2;
	if (align_up(dpb, 4096) > UINT32_MAX ||
	    align_up(target, 4096) > UINT32_MAX || packed > UINT32_MAX)
		return -1;
	l.target_field_mode = field_mode;
	l.db_pitch = c->coded_width;
	l.dpb_bytes = (uint32_t)dpb;
	l.dpb_allocation_bytes = (uint32_t)align_up(dpb, 4096);
	l.target_pitch = (uint32_t)pitch;
	l.target_rows = (uint32_t)rows;
	l.chroma_top_offset = (uint32_t)(pitch * rows);
	l.luma_bottom_offset = field_mode ? (uint32_t)(pitch * rows / 2) : 0;
	l.chroma_bottom_offset = l.chroma_top_offset +
		(field_mode ? (uint32_t)(pitch * rows / 4) : 0);
	l.target_bytes = (uint32_t)target;
	l.target_allocation_bytes = (uint32_t)align_up(target, 4096);
	l.packed_nv12_bytes = (uint32_t)packed;
	l.message_feedback_bytes = 4096 + 2048;
	*output = l;
	return 0;
}

int rdn_uvd_h264_bitstream_size(uint32_t bytes, uint32_t *padded)
{
	if (!padded || !bytes || bytes > RDN_UVD_H264_MAX_BITSTREAM_BYTES)
		return -1;
	*padded = (uint32_t)align_up(bytes, 128);
	return 0;
}

static int valid_layout(const struct rdn_uvd_h264_config *c,
	const struct rdn_uvd_h264_layout *l)
{
	struct rdn_uvd_h264_layout expected;
	return l && !rdn_uvd_h264_layout_init(c, l->target_field_mode, &expected) &&
	       !memcmp(l, &expected, sizeof(expected));
}

static int valid_picture(const struct rdn_uvd_h264_config *c,
	const struct rdn_uvd_h264_picture *p)
{
	uint32_t max_frame_num;
	unsigned i, j;
	if (!p || p->version != RDN_UVD_H264_VERSION ||
	    (p->sps_info_flags & ~13u) || !(p->sps_info_flags & 4) ||
	    (p->pps_info_flags & ~511u) || ((p->pps_info_flags >> 4) & 3) == 3 ||
	    (c->profile_idc != 100 && (p->pps_info_flags & 1)) ||
	    (c->profile_idc == 66 && (p->pps_info_flags & 256)) ||
	    p->log2_max_frame_num_minus4 > 12 || p->pic_order_cnt_type > 2 ||
	    p->log2_max_pic_order_cnt_lsb_minus4 > 12 ||
	    p->pic_init_qp_minus26 < -26 || p->pic_init_qp_minus26 > 25 ||
	    p->pic_init_qs_minus26 < -26 || p->pic_init_qs_minus26 > 25 ||
	    p->chroma_qp_index_offset < -12 || p->chroma_qp_index_offset > 12 ||
	    p->second_chroma_qp_index_offset < -12 || p->second_chroma_qp_index_offset > 12 ||
	    p->num_slice_groups_minus1 || p->slice_group_map_type ||
	    p->slice_group_change_rate_minus1 || p->num_ref_idx_l0_active_minus1 > 15 ||
	    p->num_ref_idx_l1_active_minus1 > 15 || p->field_pic_flag ||
	    p->bottom_field_flag || p->reference_count > c->max_num_ref_frames ||
	    !zero_words(p->reserved, 4))
		return 0;
	max_frame_num = 1u << (p->log2_max_frame_num_minus4 + 4);
	if (p->frame_num >= max_frame_num)
		return 0;
	for (i = 0; i < RDN_UVD_H264_MAX_REFS; i++) {
		if (p->frame_num_list[i] >= max_frame_num)
			return 0;
		if (i >= p->reference_count &&
		    (p->frame_num_list[i] || p->field_order_cnt_list[i][0] ||
		     p->field_order_cnt_list[i][1]))
			return 0;
	}
	for (i = 0; i < 6; i++)
		for (j = 0; j < 16; j++)
			if (!p->scaling_list_4x4[i][j])
				return 0;
	for (i = 0; i < 2; i++)
		for (j = 0; j < 64; j++)
			if (!p->scaling_list_8x8[i][j])
				return 0;
	return 1;
}

static int valid_output(uint32_t handle, void *out, size_t capacity)
{
	return handle && out && capacity >= RDN_UVD_H264_MESSAGE_BYTES &&
	       (uintptr_t)out <= UINTPTR_MAX - RDN_UVD_H264_MESSAGE_BYTES;
}

static void message_init(uint8_t *out, uint32_t type, uint32_t handle)
{
	memset(out, 0, RDN_UVD_H264_MESSAGE_BYTES);
	put_le32(out, RDN_UVD_H264_MESSAGE_SIZE);
	put_le32(out + 4, type);
	put_le32(out + 8, handle);
}

int rdn_uvd_h264_pack_create(const struct rdn_uvd_h264_config *c,
	const struct rdn_uvd_h264_layout *l, uint32_t handle, void *output, size_t capacity)
{
	uint8_t *out = (uint8_t *)output;
	if (!valid_output(handle, out, capacity) || !valid_layout(c, l) ||
	    ranges_overlap(out, RDN_UVD_H264_MESSAGE_BYTES, c, sizeof(*c)) ||
	    ranges_overlap(out, RDN_UVD_H264_MESSAGE_BYTES, l, sizeof(*l)))
		return -1;
	message_init(out, 0, handle);
	put_le32(out + 28, c->coded_width);
	put_le32(out + 32, c->coded_height);
	put_le32(out + 40, l->dpb_bytes);
	return 0;
}

int rdn_uvd_h264_pack_decode(const struct rdn_uvd_h264_config *c,
	const struct rdn_uvd_h264_picture *p, const struct rdn_uvd_h264_layout *l,
	uint32_t handle, uint32_t feedback, uint32_t bitstream_bytes,
	void *output, size_t capacity)
{
	uint8_t *out = (uint8_t *)output, *h;
	uint32_t padded;
	unsigned i;
	if (!valid_output(handle, out, capacity) || !feedback || !valid_layout(c, l) ||
	    !valid_picture(c, p) || rdn_uvd_h264_bitstream_size(bitstream_bytes, &padded) ||
	    ranges_overlap(out, RDN_UVD_H264_MESSAGE_BYTES, c, sizeof(*c)) ||
	    ranges_overlap(out, RDN_UVD_H264_MESSAGE_BYTES, p, sizeof(*p)) ||
	    ranges_overlap(out, RDN_UVD_H264_MESSAGE_BYTES, l, sizeof(*l)))
		return -1;
	message_init(out, 1, handle);
	put_le32(out + 12, feedback);
	put_le32(out + 20, 1);
	put_le32(out + 24, c->coded_width);
	put_le32(out + 28, c->coded_height);
	put_le32(out + 36, l->dpb_bytes);
	put_le32(out + 52, l->db_pitch);
	put_le32(out + 88, padded);
	put_le32(out + 112, l->target_pitch);
	put_le32(out + 124, l->target_field_mode);
	put_le32(out + 128, l->luma_top_offset);
	put_le32(out + 132, l->luma_bottom_offset);
	put_le32(out + 136, l->chroma_top_offset);
	put_le32(out + 140, l->chroma_bottom_offset);
	h = out + MSG_CODEC;
	put_le32(h, c->profile_idc == 66 ? 0 : c->profile_idc == 77 ? 1 : 2);
	put_le32(h + 4, c->level_idc);
	put_le32(h + 8, p->sps_info_flags);
	put_le32(h + 12, p->pps_info_flags);
	h[16] = 1;
	h[19] = (uint8_t)p->log2_max_frame_num_minus4;
	h[20] = (uint8_t)p->pic_order_cnt_type;
	h[21] = (uint8_t)p->log2_max_pic_order_cnt_lsb_minus4;
	h[22] = (uint8_t)c->max_num_ref_frames;
	h[24] = (uint8_t)p->pic_init_qp_minus26;
	h[25] = (uint8_t)p->pic_init_qs_minus26;
	h[26] = (uint8_t)p->chroma_qp_index_offset;
	h[27] = (uint8_t)p->second_chroma_qp_index_offset;
	h[30] = (uint8_t)p->num_ref_idx_l0_active_minus1;
	h[31] = (uint8_t)p->num_ref_idx_l1_active_minus1;
	memcpy(h + H264_SCALING4, p->scaling_list_4x4, sizeof(p->scaling_list_4x4));
	memcpy(h + H264_SCALING8, p->scaling_list_8x8, sizeof(p->scaling_list_8x8));
	put_le32(h + H264_FRAME_NUM, p->frame_num);
	put_le32(h + H264_DECODED_PIC, p->frame_num);
	for (i = 0; i < 16; i++) {
		put_le32(h + H264_FRAME_NUM_LIST + i * 4, p->frame_num_list[i]);
		put_le32(h + H264_REF_POC + i * 8, (uint32_t)p->field_order_cnt_list[i][0]);
		put_le32(h + H264_REF_POC + i * 8 + 4, (uint32_t)p->field_order_cnt_list[i][1]);
	}
	put_le32(h + H264_CURR_POC, (uint32_t)p->field_order_cnt[0]);
	put_le32(h + H264_CURR_POC + 4, (uint32_t)p->field_order_cnt[1]);
	out[MSG_EXTENSION] = 1;
	return 0;
}

int rdn_uvd_h264_pack_destroy(uint32_t handle, void *output, size_t capacity)
{
	if (!valid_output(handle, output, capacity))
		return -1;
	message_init((uint8_t *)output, 2, handle);
	return 0;
}

/* Legacy UVD packets and placement checks: Linux v6.12 radeon_uvd.c. */
static int valid_gpu_range(const struct rdn_uvd_h264_gpu_range *r, uint32_t minimum)
{
	const uint64_t limit = UINT64_C(1) << 40;
	return !r->reserved && r->bytes >= minimum && r->bytes &&
	       !(r->address & 3) && r->address < limit &&
	       r->bytes <= limit - r->address &&
	       (r->address >> 28) == ((r->address + r->bytes - 1) >> 28);
}

static void ib_reg(uint32_t *words, unsigned *count, uint32_t reg, uint32_t value)
{
	words[(*count)++] = reg >> 2;
	words[(*count)++] = value;
}

static void ib_command(uint32_t *words, unsigned *count, uint32_t command,
	uint64_t address)
{
	ib_reg(words, count, 0xef10, (uint32_t)address);
	ib_reg(words, count, 0xef14, (uint32_t)(address >> 32));
	ib_reg(words, count, 0xef0c, command << 1);
}

int rdn_uvd_h264_build_ib(const struct rdn_uvd_h264_config *c,
	const struct rdn_uvd_h264_layout *l, uint32_t type, uint32_t order,
	uint32_t bitstream_bytes, const struct rdn_uvd_h264_ib_buffers *b,
	void *output, size_t capacity, uint32_t *word_count)
{
	const struct rdn_uvd_h264_gpu_range *ranges[8];
	uint32_t minimum[8] = {4, 4, 64, RDN_UVD_H264_MESSAGE_BYTES, 0, 0, 0, 0};
	uint32_t words[32], padded;
	unsigned i, j, count = 0, used = 4, bytes = 64;
	uint8_t *out = (uint8_t *)output;
	if (!b || !out || !word_count || type > RDN_UVD_H264_MSG_DESTROY ||
	    order > RDN_UVD_H264_IB_BE)
		return -1;
	if (type == RDN_UVD_H264_MSG_DECODE) {
		if (!valid_layout(c, l) || rdn_uvd_h264_bitstream_size(bitstream_bytes, &padded))
			return -1;
		used = 8;
		bytes = minimum[2] = 128;
		minimum[4] = l->dpb_bytes;
		minimum[5] = padded;
		minimum[6] = l->target_bytes;
		minimum[7] = RDN_UVD_H264_FEEDBACK_BYTES;
	} else if (bitstream_bytes) {
		return -1;
	}
	if (capacity < bytes || (uintptr_t)out > UINTPTR_MAX - bytes ||
	    (uintptr_t)word_count > UINTPTR_MAX - sizeof(*word_count) ||
	    ranges_overlap(out, bytes, b, sizeof(*b)) ||
	    ranges_overlap(out, bytes, word_count, sizeof(*word_count)) ||
	    ranges_overlap(word_count, sizeof(*word_count), b, sizeof(*b)) ||
	    (c && (ranges_overlap(out, bytes, c, sizeof(*c)) ||
	           ranges_overlap(word_count, sizeof(*word_count), c, sizeof(*c)))) ||
	    (l && (ranges_overlap(out, bytes, l, sizeof(*l)) ||
	           ranges_overlap(word_count, sizeof(*word_count), l, sizeof(*l)))))
		return -1;
	ranges[0] = &b->vcpu;
	ranges[1] = &b->ring;
	ranges[2] = &b->ib;
	ranges[3] = &b->message;
	ranges[4] = &b->dpb;
	ranges[5] = &b->bitstream;
	ranges[6] = &b->target;
	ranges[7] = &b->feedback;
	for (i = 0; i < 8; i++) {
		if (i >= used) {
			if (ranges[i]->address || ranges[i]->bytes || ranges[i]->reserved)
				return -1;
		} else if (!valid_gpu_range(ranges[i], minimum[i])) {
			return -1;
		}
	}
	if ((b->ib.address & 63) || (b->message.address & 63) ||
	    (b->ib.address >> 32) != (b->ring.address >> 32) ||
	    (b->message.address >> 28) != (b->vcpu.address >> 28) ||
	    (used == 8 && (b->feedback.address >> 28) != (b->vcpu.address >> 28)))
		return -1;
	for (i = 0; i < used; i++) {
		for (j = i + 1; j < used; j++) {
			if (ranges[i]->address < ranges[j]->address + ranges[j]->bytes &&
			    ranges[j]->address < ranges[i]->address + ranges[i]->bytes)
				return -1;
		}
	}
	ib_command(words, &count, 0, b->message.address);
	if (type == RDN_UVD_H264_MSG_DECODE) {
		ib_command(words, &count, 1, b->dpb.address);
		ib_command(words, &count, 0x100, b->bitstream.address);
		ib_command(words, &count, 2, b->target.address);
		ib_command(words, &count, 3, b->feedback.address);
		ib_reg(words, &count, 0xef18, 1);
	}
	while (count & 15)
		ib_reg(words, &count, 0xeffc, 0);
	for (i = 0; i < count; i++) {
		if (order == RDN_UVD_H264_IB_LE) {
			put_le32(out + 4 * i, words[i]);
		} else {
			out[4 * i] = (uint8_t)(words[i] >> 24);
			out[4 * i + 1] = (uint8_t)(words[i] >> 16);
			out[4 * i + 2] = (uint8_t)(words[i] >> 8);
			out[4 * i + 3] = (uint8_t)words[i];
		}
	}
	*word_count = count;
	return 0;
}
