/* Standalone serializer/layout tests; no device access.
 * SPDX-License-Identifier: MIT
 */
#include "../hw/rdn_uvd_h264.h"
#include "uvd_decode_fixture.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(expr) do { \
	checks++; \
	if (!(expr)) { failures++; printf("FAIL line %u: %s\n", (unsigned)__LINE__, #expr); } \
} while (0)

typedef char config_has_fixed_size[sizeof(struct rdn_uvd_h264_config) == 56 ? 1 : -1];
typedef char picture_has_fixed_size[sizeof(struct rdn_uvd_h264_picture) == 516 ? 1 : -1];
typedef char layout_has_fixed_size[sizeof(struct rdn_uvd_h264_layout) == 56 ? 1 : -1];

static uint32_t le32(const uint8_t *p)
{
	return p[0] + (uint32_t)p[1] * 256 + (uint32_t)p[2] * 65536 +
	       (uint32_t)p[3] * 16777216;
}

static void set32(void *ptr, size_t offset, uint32_t value)
{
	memcpy((uint8_t *)ptr + offset, &value, sizeof(value));
}

static int filled(const void *ptr, size_t size, uint8_t value)
{
	const uint8_t *p = (const uint8_t *)ptr;
	size_t i;
	for (i = 0; i < size; i++)
		if (p[i] != value)
			return 0;
	return 1;
}

static void defaults(struct rdn_uvd_h264_config *c, struct rdn_uvd_h264_picture *p)
{
	memset(c, 0, sizeof(*c));
	c->version = 1;
	c->profile_idc = 100;
	c->level_idc = 30;
	c->coded_width = 864;
	c->coded_height = 480;
	c->max_num_ref_frames = 2;
	c->chroma_format_idc = 1;
	c->frame_mbs_only_flag = 1;
	memset(p, 0, sizeof(*p));
	p->version = 1;
	p->sps_info_flags = 5;
	p->pps_info_flags = 0x88;
	p->log2_max_frame_num_minus4 = 1;
	p->log2_max_pic_order_cnt_lsb_minus4 = 3;
	p->pic_init_qp_minus26 = 2;
	memset(p->scaling_list_4x4, 16, sizeof(p->scaling_list_4x4));
	memset(p->scaling_list_8x8, 16, sizeof(p->scaling_list_8x8));
}

static void fixture_tests(void)
{
	struct rdn_uvd_h264_config c;
	struct rdn_uvd_h264_picture p;
	struct rdn_uvd_h264_layout l;
	uint8_t storage[RDN_UVD_H264_MESSAGE_BYTES + 2], expected[RDN_UVD_H264_MESSAGE_BYTES];
	uint8_t *out = storage + 1;
	defaults(&c, &p);
	CHECK(!rdn_uvd_h264_layout_init(&c, RDN_UVD_H264_TARGET_FIELDS, &l));
	CHECK(l.dpb_bytes == 15923584 && l.dpb_allocation_bytes == 15925248);
	CHECK(l.target_pitch == 1024 && l.target_rows == 480);
	CHECK(l.luma_top_offset == 0 && l.luma_bottom_offset == 0x3c000);
	CHECK(l.chroma_top_offset == 0x78000 && l.chroma_bottom_offset == 0x96000);
	CHECK(l.target_bytes == 737280 && l.target_allocation_bytes == 737280);
	CHECK(l.packed_nv12_bytes == 622080 && l.message_feedback_bytes == 6144);
	memset(storage, 0xa5, sizeof(storage));
	CHECK(!rdn_uvd_fixture_message_copy(RDN_UVD_FIXTURE_CREATE, 0x12345678, expected, sizeof(expected)));
	CHECK(!rdn_uvd_h264_pack_create(&c, &l, 0x12345678, out, RDN_UVD_H264_MESSAGE_BYTES));
	CHECK(!memcmp(out, expected, sizeof(expected)));
	CHECK(storage[0] == 0xa5 && storage[sizeof(storage) - 1] == 0xa5);
	CHECK(!rdn_uvd_fixture_message_copy(RDN_UVD_FIXTURE_DECODE, 0x12345678, expected, sizeof(expected)));
	/* Mesa programs db_pitch; the AMD sample leaves this one field zero. */
	CHECK(le32(expected + 52) == 0);
	expected[52] = 0x60;
	expected[53] = 0x03;
	CHECK(!rdn_uvd_h264_pack_decode(&c, &p, &l, 0x12345678, 1, 1408, out, RDN_UVD_H264_MESSAGE_BYTES));
	CHECK(!memcmp(out, expected, sizeof(expected)));
	CHECK(storage[0] == 0xa5 && storage[sizeof(storage) - 1] == 0xa5);
	CHECK(!rdn_uvd_fixture_message_copy(RDN_UVD_FIXTURE_DESTROY, 0x12345678, expected, sizeof(expected)));
	CHECK(!rdn_uvd_h264_pack_destroy(0x12345678, out, RDN_UVD_H264_MESSAGE_BYTES));
	CHECK(!memcmp(out, expected, sizeof(expected)));
	CHECK(storage[0] == 0xa5 && storage[sizeof(storage) - 1] == 0xa5);
}

static void encoding_tests(void)
{
	struct rdn_uvd_h264_config c;
	struct rdn_uvd_h264_picture p;
	struct rdn_uvd_h264_layout l;
	uint8_t storage[RDN_UVD_H264_MESSAGE_BYTES + 2], *out = storage + 1;
	unsigned i, j;
	defaults(&c, &p);
	CHECK(!rdn_uvd_h264_layout_init(&c, 0, &l));
	p.log2_max_frame_num_minus4 = 12;
	p.pic_order_cnt_type = 1;
	p.sps_info_flags = 13;
	p.pps_info_flags = 0x1ed;
	p.pic_init_qp_minus26 = -26;
	p.pic_init_qs_minus26 = 25;
	p.chroma_qp_index_offset = -12;
	p.second_chroma_qp_index_offset = 12;
	p.num_ref_idx_l0_active_minus1 = 15;
	p.num_ref_idx_l1_active_minus1 = 14;
	p.frame_num = 65535;
	p.reference_count = 2;
	p.frame_num_list[0] = 0x1234;
	p.frame_num_list[1] = 0xabcd;
	p.field_order_cnt[0] = INT32_MIN;
	p.field_order_cnt[1] = INT32_MAX;
	p.field_order_cnt_list[0][0] = -1;
	p.field_order_cnt_list[0][1] = -65537;
	p.field_order_cnt_list[1][0] = 0x12345678;
	p.field_order_cnt_list[1][1] = -0x1234567;
	for (i = 0; i < 6; i++)
		for (j = 0; j < 16; j++) p.scaling_list_4x4[i][j] = (uint8_t)(1 + i * 16 + j);
	for (i = 0; i < 2; i++)
		for (j = 0; j < 64; j++) p.scaling_list_8x8[i][j] = (uint8_t)(255 - i * 64 - j);
	memset(storage, 0xcc, sizeof(storage));
	CHECK(!rdn_uvd_h264_pack_decode(&c, &p, &l, 0xfedcba98, 0x89abcdef, 129,
		out, RDN_UVD_H264_MESSAGE_BYTES));
	CHECK(out[8] == 0x98 && out[9] == 0xba && out[10] == 0xdc && out[11] == 0xfe);
	CHECK(out[12] == 0xef && out[13] == 0xcd && out[14] == 0xab && out[15] == 0x89);
	CHECK(le32(out + 88) == 256 && le32(out + 124) == 0);
	CHECK(le32(out + 132) == 0 && le32(out + 136) == 491520 && le32(out + 140) == 491520);
	CHECK(le32(out + 232) == 13 && le32(out + 236) == 0x1ed);
	CHECK(out[240] == 1 && out[241] == 0 && out[242] == 0 && out[243] == 12);
	CHECK(out[244] == 1 && out[245] == 3 && out[246] == 2 && out[247] == 0);
	CHECK(out[248] == 0xe6 && out[249] == 25 && out[250] == 0xf4 && out[251] == 12);
	CHECK(out[252] == 0 && out[253] == 0 && out[254] == 15 && out[255] == 14);
	CHECK(filled(out + 256, 4, 0));
	CHECK(!memcmp(out + 260, p.scaling_list_4x4, 96));
	CHECK(!memcmp(out + 356, p.scaling_list_8x8, 128));
	CHECK(le32(out + 484) == 65535 && le32(out + 688) == 65535);
	CHECK(le32(out + 488) == 0x1234 && le32(out + 492) == 0xabcd);
	CHECK(out[552] == 0 && out[553] == 0 && out[554] == 0 && out[555] == 0x80);
	CHECK(le32(out + 556) == 0x7fffffff && le32(out + 560) == 0xffffffff);
	CHECK(le32(out + 564) == 0xfffeffff && le32(out + 568) == 0x12345678);
	CHECK(le32(out + 572) == 0xfedcba99);
	CHECK(filled(out + 576, 112, 0));
	CHECK(filled(out + 692, 3296 - 692, 0));
	CHECK(out[3296] == 1 && filled(out + 3297, RDN_UVD_H264_MESSAGE_BYTES - 3297, 0));
	CHECK(storage[0] == 0xcc && storage[sizeof(storage) - 1] == 0xcc);
	for (i = 0; i < 3; i++) {
		c.profile_idc = i == 0 ? 66 : i == 1 ? 77 : 100;
		p.pps_info_flags = 0x88;
		CHECK(!rdn_uvd_h264_pack_decode(&c, &p, &l, 1, 1, 1, out, RDN_UVD_H264_MESSAGE_BYTES));
		CHECK(le32(out + 224) == i);
	}
}

struct mutation { size_t offset; uint32_t value; };
#define BAD(type, field, value) { offsetof(struct type, field), (uint32_t)(value) }

static void invalid_config_tests(void)
{
	static const struct mutation mutations[] = {
		BAD(rdn_uvd_h264_config, version, 0), BAD(rdn_uvd_h264_config, version, 2),
		BAD(rdn_uvd_h264_config, profile_idc, 0), BAD(rdn_uvd_h264_config, profile_idc, 110),
		BAD(rdn_uvd_h264_config, level_idc, 9), BAD(rdn_uvd_h264_config, level_idc, 13),
		BAD(rdn_uvd_h264_config, level_idc, 50), BAD(rdn_uvd_h264_config, coded_width, 0),
		BAD(rdn_uvd_h264_config, coded_width, 865), BAD(rdn_uvd_h264_config, coded_width, 1936),
		BAD(rdn_uvd_h264_config, coded_width, UINT32_MAX), BAD(rdn_uvd_h264_config, coded_height, 0),
		BAD(rdn_uvd_h264_config, coded_height, 481), BAD(rdn_uvd_h264_config, coded_height, 1104),
		BAD(rdn_uvd_h264_config, coded_height, UINT32_MAX), BAD(rdn_uvd_h264_config, max_num_ref_frames, 6),
		BAD(rdn_uvd_h264_config, max_num_ref_frames, 17), BAD(rdn_uvd_h264_config, chroma_format_idc, 0),
		BAD(rdn_uvd_h264_config, chroma_format_idc, 2), BAD(rdn_uvd_h264_config, bit_depth_luma_minus8, 2),
		BAD(rdn_uvd_h264_config, bit_depth_chroma_minus8, 1), BAD(rdn_uvd_h264_config, frame_mbs_only_flag, 0),
		BAD(rdn_uvd_h264_config, frame_mbs_only_flag, 2), BAD(rdn_uvd_h264_config, mb_adaptive_frame_field_flag, 1),
		BAD(rdn_uvd_h264_config, reserved[0], 1), BAD(rdn_uvd_h264_config, reserved[2], 1)
	};
	struct rdn_uvd_h264_config c;
	struct rdn_uvd_h264_picture p;
	struct rdn_uvd_h264_layout l, output;
	uint8_t msg[RDN_UVD_H264_MESSAGE_BYTES];
	unsigned i;
	defaults(&c, &p);
	CHECK(!rdn_uvd_h264_layout_init(&c, 0, &l));
	for (i = 0; i < sizeof(mutations) / sizeof(mutations[0]); i++) {
		defaults(&c, &p);
		set32(&c, mutations[i].offset, mutations[i].value);
		memset(&output, 0xa5, sizeof(output));
		memset(msg, 0xa5, sizeof(msg));
		CHECK(rdn_uvd_h264_layout_init(&c, 0, &output) < 0);
		CHECK(filled(&output, sizeof(output), 0xa5));
		CHECK(rdn_uvd_h264_pack_create(&c, &l, 1, msg, sizeof(msg)) < 0);
		CHECK(rdn_uvd_h264_pack_decode(&c, &p, &l, 1, 1, 1, msg, sizeof(msg)) < 0);
		CHECK(filled(msg, sizeof(msg), 0xa5));
	}
}

static void invalid_picture_tests(void)
{
	static const struct mutation mutations[] = {
		BAD(rdn_uvd_h264_picture, version, 0), BAD(rdn_uvd_h264_picture, version, 2),
		BAD(rdn_uvd_h264_picture, sps_info_flags, 1), BAD(rdn_uvd_h264_picture, sps_info_flags, 7),
		BAD(rdn_uvd_h264_picture, sps_info_flags, 0x15), BAD(rdn_uvd_h264_picture, pps_info_flags, 0x200),
		BAD(rdn_uvd_h264_picture, pps_info_flags, 0x30), BAD(rdn_uvd_h264_picture, log2_max_frame_num_minus4, 13),
		BAD(rdn_uvd_h264_picture, pic_order_cnt_type, 3), BAD(rdn_uvd_h264_picture, log2_max_pic_order_cnt_lsb_minus4, 13),
		BAD(rdn_uvd_h264_picture, pic_init_qp_minus26, -27), BAD(rdn_uvd_h264_picture, pic_init_qp_minus26, 26),
		BAD(rdn_uvd_h264_picture, pic_init_qs_minus26, -27), BAD(rdn_uvd_h264_picture, pic_init_qs_minus26, 26),
		BAD(rdn_uvd_h264_picture, chroma_qp_index_offset, -13), BAD(rdn_uvd_h264_picture, chroma_qp_index_offset, 13),
		BAD(rdn_uvd_h264_picture, second_chroma_qp_index_offset, -13), BAD(rdn_uvd_h264_picture, second_chroma_qp_index_offset, 13),
		BAD(rdn_uvd_h264_picture, num_slice_groups_minus1, 1), BAD(rdn_uvd_h264_picture, slice_group_map_type, 1),
		BAD(rdn_uvd_h264_picture, slice_group_change_rate_minus1, 65536), BAD(rdn_uvd_h264_picture, num_ref_idx_l0_active_minus1, 16),
		BAD(rdn_uvd_h264_picture, num_ref_idx_l1_active_minus1, 16), BAD(rdn_uvd_h264_picture, field_pic_flag, 1),
		BAD(rdn_uvd_h264_picture, bottom_field_flag, 1), BAD(rdn_uvd_h264_picture, frame_num, 32),
		BAD(rdn_uvd_h264_picture, reference_count, 3), BAD(rdn_uvd_h264_picture, frame_num_list[0], 1),
		BAD(rdn_uvd_h264_picture, field_order_cnt_list[15][1], -1), BAD(rdn_uvd_h264_picture, reserved[0], 1),
		BAD(rdn_uvd_h264_picture, reserved[3], UINT32_MAX)
	};
	struct rdn_uvd_h264_config c;
	struct rdn_uvd_h264_picture p;
	struct rdn_uvd_h264_layout l;
	uint8_t msg[RDN_UVD_H264_MESSAGE_BYTES];
	unsigned i;
	defaults(&c, &p);
	CHECK(!rdn_uvd_h264_layout_init(&c, 0, &l));
	for (i = 0; i < sizeof(mutations) / sizeof(mutations[0]); i++) {
		defaults(&c, &p);
		set32(&p, mutations[i].offset, mutations[i].value);
		memset(msg, 0xa5, sizeof(msg));
		CHECK(rdn_uvd_h264_pack_decode(&c, &p, &l, 1, 1, 1, msg, sizeof(msg)) < 0);
		CHECK(filled(msg, sizeof(msg), 0xa5));
	}
	for (i = 0; i < 4; i++) {
		defaults(&c, &p);
		if (i == 0) p.scaling_list_4x4[5][15] = 0;
		if (i == 1) p.scaling_list_8x8[1][63] = 0;
		if (i == 2) { c.profile_idc = 66; p.pps_info_flags |= 256; }
		if (i == 3) { c.profile_idc = 77; p.pps_info_flags |= 1; }
		memset(msg, 0xa5, sizeof(msg));
		CHECK(rdn_uvd_h264_pack_decode(&c, &p, &l, 1, 1, 1, msg, sizeof(msg)) < 0);
		CHECK(filled(msg, sizeof(msg), 0xa5));
	}
}

static void boundary_tests(void)
{
	static const uint32_t levels[] = {10, 11, 12, 13, 20, 21, 22, 30, 31, 32, 40, 41, 42};
	struct rdn_uvd_h264_config c;
	struct rdn_uvd_h264_picture p;
	struct rdn_uvd_h264_layout l, bad;
	uint8_t msg[RDN_UVD_H264_MESSAGE_BYTES];
	uint32_t padded;
	unsigned i, mode;
	defaults(&c, &p);
	c.coded_width = c.coded_height = 16;
	c.max_num_ref_frames = 16;
	for (i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
		c.level_idc = levels[i];
		for (mode = 0; mode < 2; mode++) {
			CHECK(!rdn_uvd_h264_layout_init(&c, mode, &l));
			CHECK(l.dpb_bytes == 24000 && l.dpb_allocation_bytes == 24576);
			CHECK(l.target_pitch == 256 && l.target_rows == (mode ? 32u : 16u));
			CHECK(l.target_bytes == (mode ? 12288u : 6144u));
			CHECK(l.target_allocation_bytes == (mode ? 12288u : 8192u));
			CHECK(l.packed_nv12_bytes == 384);
		}
	}
	c.coded_width = 1920;
	c.coded_height = 1088;
	c.max_num_ref_frames = 4;
	c.level_idc = 40;
	CHECK(!rdn_uvd_h264_layout_init(&c, 0, &l));
	CHECK(l.dpb_bytes == 80163840 && l.dpb_allocation_bytes == 80166912);
	CHECK(l.target_pitch == 2048 && l.target_bytes == 3342336 && l.packed_nv12_bytes == 3133440);
	c.max_num_ref_frames = 5;
	CHECK(rdn_uvd_h264_layout_init(&c, 0, &l) < 0);
	c.max_num_ref_frames = 4;
	c.level_idc = 31;
	CHECK(rdn_uvd_h264_layout_init(&c, 0, &l) < 0);
	c.coded_width = 1280;
	c.coded_height = 720;
	CHECK(!rdn_uvd_h264_layout_init(&c, 0, &l));
	CHECK(l.dpb_bytes == 35630080);
	c.level_idc = 10;
	c.coded_width = 464;
	c.coded_height = 16;
	c.max_num_ref_frames = 0;
	CHECK(rdn_uvd_h264_layout_init(&c, 0, &l) < 0); /* Width exceeds sqrt(8*MaxFS). */
	defaults(&c, &p);
	CHECK(!rdn_uvd_h264_layout_init(&c, 0, &l));
	memset(msg, 0xa5, sizeof(msg));
	for (i = 0; i < sizeof(l) / 4; i++) {
		bad = l;
		set32(&bad, i * 4, UINT32_MAX);
		CHECK(rdn_uvd_h264_pack_create(&c, &bad, 1, msg, sizeof(msg)) < 0);
		CHECK(rdn_uvd_h264_pack_decode(&c, &p, &bad, 1, 1, 1, msg, sizeof(msg)) < 0);
		CHECK(filled(msg, sizeof(msg), 0xa5));
	}
	padded = 0xa5a5a5a5;
	CHECK(rdn_uvd_h264_bitstream_size(0, &padded) < 0 && padded == 0xa5a5a5a5);
	CHECK(rdn_uvd_h264_bitstream_size(UINT32_MAX, &padded) < 0 && padded == 0xa5a5a5a5);
	CHECK(rdn_uvd_h264_bitstream_size(RDN_UVD_H264_MAX_BITSTREAM_BYTES + 1, &padded) < 0);
	CHECK(!rdn_uvd_h264_bitstream_size(1, &padded) && padded == 128);
	CHECK(!rdn_uvd_h264_bitstream_size(128, &padded) && padded == 128);
	CHECK(!rdn_uvd_h264_bitstream_size(129, &padded) && padded == 256);
	CHECK(!rdn_uvd_h264_bitstream_size(RDN_UVD_H264_MAX_BITSTREAM_BYTES, &padded) &&
		padded == RDN_UVD_H264_MAX_BITSTREAM_BYTES);
	CHECK(rdn_uvd_h264_bitstream_size(1, NULL) < 0);
}

static void bad_call_tests(void)
{
	struct rdn_uvd_h264_config c;
	struct rdn_uvd_h264_picture p;
	struct rdn_uvd_h264_layout l;
	union { uint32_t align; uint8_t bytes[RDN_UVD_H264_MESSAGE_BYTES + 16]; } alias;
	uint8_t msg[RDN_UVD_H264_MESSAGE_BYTES];
	defaults(&c, &p);
	CHECK(!rdn_uvd_h264_layout_init(&c, 0, &l));
	memset(msg, 0xa5, sizeof(msg));
	CHECK(rdn_uvd_h264_layout_init(NULL, 0, &l) < 0);
	CHECK(rdn_uvd_h264_layout_init(&c, 0, NULL) < 0);
	CHECK(rdn_uvd_h264_layout_init(&c, 2, &l) < 0);
	CHECK(rdn_uvd_h264_pack_create(NULL, &l, 1, msg, sizeof(msg)) < 0);
	CHECK(rdn_uvd_h264_pack_create(&c, NULL, 1, msg, sizeof(msg)) < 0);
	CHECK(rdn_uvd_h264_pack_create(&c, &l, 0, msg, sizeof(msg)) < 0);
	CHECK(rdn_uvd_h264_pack_create(&c, &l, 1, NULL, sizeof(msg)) < 0);
	CHECK(rdn_uvd_h264_pack_create(&c, &l, 1, msg, sizeof(msg) - 1) < 0);
	CHECK(rdn_uvd_h264_pack_decode(NULL, &p, &l, 1, 1, 1, msg, sizeof(msg)) < 0);
	CHECK(rdn_uvd_h264_pack_decode(&c, NULL, &l, 1, 1, 1, msg, sizeof(msg)) < 0);
	CHECK(rdn_uvd_h264_pack_decode(&c, &p, NULL, 1, 1, 1, msg, sizeof(msg)) < 0);
	CHECK(rdn_uvd_h264_pack_decode(&c, &p, &l, 0, 1, 1, msg, sizeof(msg)) < 0);
	CHECK(rdn_uvd_h264_pack_decode(&c, &p, &l, 1, 0, 1, msg, sizeof(msg)) < 0);
	CHECK(rdn_uvd_h264_pack_decode(&c, &p, &l, 1, 1, 0, msg, sizeof(msg)) < 0);
	CHECK(rdn_uvd_h264_pack_decode(&c, &p, &l, 1, 1, UINT32_MAX, msg, sizeof(msg)) < 0);
	CHECK(rdn_uvd_h264_pack_decode(&c, &p, &l, 1, 1, 1, NULL, sizeof(msg)) < 0);
	CHECK(rdn_uvd_h264_pack_decode(&c, &p, &l, 1, 1, 1, msg, sizeof(msg) - 1) < 0);
	CHECK(rdn_uvd_h264_pack_destroy(0, msg, sizeof(msg)) < 0);
	CHECK(rdn_uvd_h264_pack_destroy(1, NULL, sizeof(msg)) < 0);
	CHECK(rdn_uvd_h264_pack_destroy(1, msg, sizeof(msg) - 1) < 0);
	CHECK(rdn_uvd_h264_pack_destroy(1, (void *)(UINTPTR_MAX - 10), sizeof(msg)) < 0);
	CHECK(filled(msg, sizeof(msg), 0xa5));
	memset(&alias, 0xa5, sizeof(alias));
	memcpy(alias.bytes, &c, sizeof(c));
	CHECK(rdn_uvd_h264_pack_create((const struct rdn_uvd_h264_config *)alias.bytes,
		&l, 1, alias.bytes, sizeof(alias.bytes)) < 0);
	CHECK(!memcmp(alias.bytes, &c, sizeof(c)) && filled(alias.bytes + sizeof(c),
		sizeof(alias.bytes) - sizeof(c), 0xa5));
	CHECK(rdn_uvd_h264_layout_init((const struct rdn_uvd_h264_config *)alias.bytes,
		0, (struct rdn_uvd_h264_layout *)alias.bytes) < 0);
	memcpy(alias.bytes, &p, sizeof(p));
	CHECK(rdn_uvd_h264_pack_decode(&c, (const struct rdn_uvd_h264_picture *)alias.bytes,
		&l, 1, 1, 1, alias.bytes, sizeof(alias.bytes)) < 0);
	CHECK(!memcmp(alias.bytes, &p, sizeof(p)));
	memcpy(alias.bytes, &l, sizeof(l));
	CHECK(rdn_uvd_h264_pack_create(&c, (const struct rdn_uvd_h264_layout *)alias.bytes,
		1, alias.bytes, sizeof(alias.bytes)) < 0);
	CHECK(!memcmp(alias.bytes, &l, sizeof(l)));
}

static void level_1b_tests(void)
{
	struct rdn_uvd_h264_config c;
	struct rdn_uvd_h264_picture p;
	struct rdn_uvd_h264_layout l;
	unsigned profile;
	for (profile = 66; profile <= 77; profile += 11) {
		defaults(&c, &p);
		c.profile_idc = profile;
		c.coded_width = 176;
		c.coded_height = 144;
		c.level_idc = 11;
		CHECK(!rdn_uvd_h264_layout_init(&c, 0, &l));
		/* The SPS caller normalizes constraint_set3 + level 11 to level 1b. */
		c.level_idc = 9;
		memset(&l, 0xa5, sizeof(l));
		CHECK(rdn_uvd_h264_layout_init(&c, 0, &l) < 0);
		CHECK(filled(&l, sizeof(l), 0xa5));
	}
}

static void ib_defaults(struct rdn_uvd_h264_ib_buffers *b,
	const struct rdn_uvd_h264_layout *l)
{
	memset(b, 0, sizeof(*b));
	b->vcpu.address = UINT64_C(0x0f02000000); b->vcpu.bytes = 0x200000;
	b->ring.address = UINT64_C(0x0f04000000); b->ring.bytes = 0x4000;
	b->ib.address = UINT64_C(0x0f04004000); b->ib.bytes = 4096;
	b->message.address = UINT64_C(0x0f04005000); b->message.bytes = 4096;
	b->feedback.address = UINT64_C(0x0f04006000); b->feedback.bytes = 2048;
	b->dpb.address = UINT64_C(0x0f11000000); b->dpb.bytes = l->dpb_allocation_bytes;
	b->bitstream.address = UINT64_C(0x0f13000000); b->bitstream.bytes = 4096;
	b->target.address = UINT64_C(0x0f14000000); b->target.bytes = l->target_allocation_bytes;
}

static struct rdn_uvd_h264_gpu_range *ib_range(struct rdn_uvd_h264_ib_buffers *b,
	unsigned index)
{
	switch (index) {
	case 0: return &b->vcpu;
	case 1: return &b->ring;
	case 2: return &b->ib;
	case 3: return &b->message;
	case 4: return &b->dpb;
	case 5: return &b->bitstream;
	case 6: return &b->target;
	default: return &b->feedback;
	}
}

static void ib_packet_tests(void)
{
	static const uint32_t expected[32] = {
		0x3bc4, 0x04005000, 0x3bc5, 0xf, 0x3bc3, 0,
		0x3bc4, 0x11000000, 0x3bc5, 0xf, 0x3bc3, 2,
		0x3bc4, 0x13000000, 0x3bc5, 0xf, 0x3bc3, 0x200,
		0x3bc4, 0x14000000, 0x3bc5, 0xf, 0x3bc3, 4,
		0x3bc4, 0x04006000, 0x3bc5, 0xf, 0x3bc3, 6,
		0x3bc6, 1
	};
	struct rdn_uvd_h264_config c;
	struct rdn_uvd_h264_picture p;
	struct rdn_uvd_h264_layout l;
	struct rdn_uvd_h264_ib_buffers b, before;
	uint8_t storage[130], *out = storage + 1;
	uint32_t words, v;
	unsigned type, order, i;
	defaults(&c, &p);
	CHECK(!rdn_uvd_h264_layout_init(&c, 0, &l));
	CHECK(sizeof(struct rdn_uvd_h264_gpu_range) == 16 && sizeof(b) == 128);
	for (type = 0; type < 3; type++) {
		ib_defaults(&b, &l);
		if (type != 1)
			for (i = 4; i < 8; i++) memset(ib_range(&b, i), 0, sizeof(b.dpb));
		before = b;
		for (order = 0; order < 2; order++) {
			memset(storage, 0xa5, sizeof(storage));
			words = 123;
			CHECK(!rdn_uvd_h264_build_ib(type == 1 ? &c : NULL, type == 1 ? &l : NULL,
				type, order, type == 1 ? 1408 : 0, &b, out, 128, &words));
			CHECK(words == (type == 1 ? 32u : 16u) && !(words & 15));
			CHECK(storage[0] == 0xa5 && filled(out + words * 4, 129 - words * 4, 0xa5));
			CHECK(!memcmp(&b, &before, sizeof(b)));
			for (i = 0; i < words; i++) {
				v = type == 1 || i < 6 ? expected[i] : (i & 1 ? 0 : 0x3bff);
				if (!order) {
					CHECK(le32(out + i * 4) == v);
				} else {
					CHECK(out[i*4] == (uint8_t)(v >> 24) &&
					      out[i*4+1] == (uint8_t)(v >> 16) &&
					      out[i*4+2] == (uint8_t)(v >> 8) && out[i*4+3] == (uint8_t)v);
				}
			}
		}
	}
}

static void ib_reject(const struct rdn_uvd_h264_config *c,
	const struct rdn_uvd_h264_layout *l, const struct rdn_uvd_h264_ib_buffers *b,
	uint32_t type, uint32_t order, uint32_t bitstream)
{
	uint8_t out[130];
	uint32_t count = 0xa5a5a5a5;
	memset(out, 0xa5, sizeof(out));
	CHECK(rdn_uvd_h264_build_ib(c, l, type, order, bitstream, b, out + 1, 128, &count) < 0);
	CHECK(count == 0xa5a5a5a5 && filled(out, sizeof(out), 0xa5));
}

static void ib_invalid_tests(void)
{
	struct rdn_uvd_h264_config c;
	struct rdn_uvd_h264_picture p;
	struct rdn_uvd_h264_layout l, bad;
	struct rdn_uvd_h264_ib_buffers b;
	struct rdn_uvd_h264_gpu_range *range;
	uint8_t out[128];
	uint32_t count = 0xa5a5a5a5;
	unsigned i, j;
	defaults(&c, &p);
	CHECK(!rdn_uvd_h264_layout_init(&c, 0, &l));
	for (i = 0; i < 8; i++) {
		for (j = 0; j < 7; j++) {
			ib_defaults(&b, &l);
			range = ib_range(&b, i);
			if (j == 0) range->bytes = 0;
			if (j == 1) range->address++;
			if (j == 2) range->address = UINT64_C(1) << 40;
			if (j == 3) range->address = (UINT64_C(1) << 40) - 64;
			if (j == 4) range->address = UINT64_MAX;
			if (j == 5) range->address = (UINT64_C(0x0f1fffffff) - range->bytes / 2) & ~UINT64_C(63);
			if (j == 6) range->reserved = 1;
			ib_reject(&c, &l, &b, 1, 0, 1408);
		}
	}
	for (i = 0; i < 8; i++) {
		for (j = i + 1; j < 8; j++) {
			ib_defaults(&b, &l);
			ib_range(&b, j)->address = ib_range(&b, i)->address;
			ib_reject(&c, &l, &b, 1, 0, 1408);
		}
	}
	for (i = 2; i < 8; i++) {
		ib_defaults(&b, &l);
		range = ib_range(&b, i);
		range->bytes = i == 2 ? 127 : i == 3 ? RDN_UVD_H264_MESSAGE_BYTES - 1 :
			i == 4 ? l.dpb_bytes - 1 : i == 5 ? 1407 : i == 6 ? l.target_bytes - 1 : 2047;
		ib_reject(&c, &l, &b, 1, 0, 1408);
	}
	for (i = 0; i < 5; i++) {
		ib_defaults(&b, &l);
		if (i == 0) b.ib.address += 4;
		if (i == 1) b.message.address += 4;
		if (i == 2) b.message.address += 0x10000000;
		if (i == 3) b.feedback.address += 0x10000000;
		if (i == 4) b.ib.address += UINT64_C(0x100000000);
		ib_reject(&c, &l, &b, 1, 0, 1408);
	}
	ib_defaults(&b, &l);
	ib_reject(&c, &l, &b, 3, 0, 1408);
	ib_reject(&c, &l, &b, 1, 2, 1408);
	ib_reject(&c, &l, &b, 1, 0, 0);
	ib_reject(&c, &l, &b, 1, 0, UINT32_MAX);
	ib_reject(NULL, &l, &b, 1, 0, 1408);
	ib_reject(&c, NULL, &b, 1, 0, 1408);
	ib_reject(&c, &l, NULL, 1, 0, 1408);
	bad = l; bad.dpb_bytes--;
	ib_reject(&c, &bad, &b, 1, 0, 1408);
	for (i = 0; i < 3; i += 2) {
		ib_reject(NULL, NULL, &b, i, 0, 0); /* Unused decode ranges must be zero. */
	}
	for (i = 4; i < 8; i++) memset(ib_range(&b, i), 0, sizeof(b.dpb));
	ib_reject(NULL, NULL, &b, 0, 0, 1);
	ib_reject(NULL, NULL, &b, 2, 0, 1);
	ib_defaults(&b, &l);
	memset(out, 0xa5, sizeof(out));
	CHECK(rdn_uvd_h264_build_ib(&c, &l, 1, 0, 1408, &b, out, 127, &count) < 0);
	CHECK(rdn_uvd_h264_build_ib(&c, &l, 1, 0, 1408, &b, NULL, 128, &count) < 0);
	CHECK(rdn_uvd_h264_build_ib(&c, &l, 1, 0, 1408, &b, out, 128, NULL) < 0);
	CHECK(rdn_uvd_h264_build_ib(&c, &l, 1, 0, 1408, &b,
		(void *)(UINTPTR_MAX - 63), 128, &count) < 0);
	CHECK(rdn_uvd_h264_build_ib(&c, &l, 1, 0, 1408, &b, out, 128,
		(uint32_t *)(UINTPTR_MAX - 1)) < 0);
	CHECK(count == 0xa5a5a5a5 && filled(out, sizeof(out), 0xa5));
	CHECK(rdn_uvd_h264_build_ib(&c, &l, 1, 0, 1408, &b, &b, sizeof(b), &count) < 0);
	CHECK(rdn_uvd_h264_build_ib(&c, &l, 1, 0, 1408, &b, out, 128, &b.ib.bytes) < 0);
	CHECK(rdn_uvd_h264_build_ib(&c, &l, 1, 0, 1408, &b, out, 128, &c.version) < 0);
	CHECK(rdn_uvd_h264_build_ib(&c, &l, 1, 0, 1408, &b, out, 128, &l.dpb_bytes) < 0);
	CHECK(rdn_uvd_h264_build_ib(&c, &l, 1, 0, 1408, &b, &count, 128, &count) < 0);
	CHECK(count == 0xa5a5a5a5 && filled(out, sizeof(out), 0xa5));
}

static void ib_boundary_tests(void)
{
	struct rdn_uvd_h264_config c;
	struct rdn_uvd_h264_picture p;
	struct rdn_uvd_h264_layout l;
	struct rdn_uvd_h264_ib_buffers b;
	uint8_t out[128];
	uint32_t count = 0;
	unsigned i;
	defaults(&c, &p);
	CHECK(!rdn_uvd_h264_layout_init(&c, 0, &l));
	ib_defaults(&b, &l);
	for (i = 0; i < 8; i++) ib_range(&b, i)->address += UINT64_C(0xf000000000);
	CHECK(!rdn_uvd_h264_build_ib(&c, &l, 1, 0, 1408, &b, out, 128, &count));
	CHECK(count == 32 && le32(out + 12) == 0xff && le32(out + 108) == 0xff);
	ib_defaults(&b, &l);
	b.bitstream.bytes = 256;
	CHECK(!rdn_uvd_h264_build_ib(&c, &l, 1, 0, 129, &b, out, 128, &count));
	b.bitstream.bytes--;
	ib_reject(&c, &l, &b, 1, 0, 129);
	ib_defaults(&b, &l);
	b.target.address = UINT64_C(0x0f20000000) - b.target.bytes;
	CHECK(!rdn_uvd_h264_build_ib(&c, &l, 1, 0, 1408, &b, out, 128, &count));
	b.target.bytes++;
	ib_reject(&c, &l, &b, 1, 0, 1408);
	memset(&b, 0, sizeof(b));
	b.vcpu.address = UINT64_C(0xffff000000); b.vcpu.bytes = 0x200000;
	b.ring.address = UINT64_C(0xffff200000); b.ring.bytes = 4096;
	b.ib.address = UINT64_C(0xffffffd000); b.ib.bytes = 64;
	b.message.address = UINT64_C(0xfffffff000); b.message.bytes = 4096;
	CHECK(!rdn_uvd_h264_build_ib(NULL, NULL, 0, 0, 0, &b, out, 64, &count));
	CHECK(count == 16 && le32(out + 4) == 0xfffff000 && le32(out + 12) == 0xff);
	b.message.bytes++;
	ib_reject(NULL, NULL, &b, 0, 0, 0);
	b.message.bytes--;
	b.ib.bytes--;
	ib_reject(NULL, NULL, &b, 0, 0, 0);
	ib_defaults(&b, &l);
	CHECK(rdn_uvd_h264_build_ib(&c, &l, 1, 0, 1408, &b, &c, 128, &count) < 0);
	CHECK(rdn_uvd_h264_build_ib(&c, &l, 1, 0, 1408, &b, &l, 128, &count) < 0);
}

int main(void)
{
	fixture_tests();
	encoding_tests();
	invalid_config_tests();
	invalid_picture_tests();
	boundary_tests();
	bad_call_tests();
	level_1b_tests();
	ib_packet_tests();
	ib_invalid_tests();
	ib_boundary_tests();
	printf("{\"checks\":%u,\"failures\":%u,\"passed\":%s}\n",
		checks, failures, failures ? "false" : "true");
	return failures ? 1 : 0;
}
