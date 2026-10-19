/*
 * H.264 for the card's video decoder, the part that has nothing to do with the card: parameter
 * sets, slice headers, picture order count, reference picture marking, the decoded picture
 * buffer and the order pictures leave it in (ITU-T H.264 7.3, 8.2 and C.4).
 *
 * The caller feeds NAL units (Annex B or length prefixed); the parser finds the pictures, calls
 * `decode` for each one with everything the decoder needs (parameters, reference list, slices)
 * and then calls `output` for pictures in display order. It never touches the card and
 * includes nothing but the C library, so PowerVLC and QuickTime modules can use it without
 * Mesa, and tests run on any host.
 *
 * Frame pictures only (progressive and MBAFF). A stream with field pictures is refused
 * (RDNVD_H264_UNSUPPORTED), as are data partitioning, 4:2:2 and more than 8 bits.
 *
 * The public structures use fixed-size types only (no bool: it has four bytes on Tiger).
 *
 * Copyright (c) 2026 kouta-kun and Claude
 * SPDX-License-Identifier: MIT
 */

#ifndef RDNVD_H264_H
#define RDNVD_H264_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RDNVD_H264_MAX_SPS	32
#define RDNVD_H264_MAX_PPS	256
#define RDNVD_H264_MAX_REFS	16
#define RDNVD_H264_MAX_SLICES	1024

/* Results. */
#define RDNVD_H264_OK		0
#define RDNVD_H264_UNSUPPORTED	(-1)	/* a feature the decoder does not take: the caller should use software */
#define RDNVD_H264_BAD_STREAM	(-2)	/* the data does not parse */
#define RDNVD_H264_NO_MEMORY	(-3)
#define RDNVD_H264_DECODE_FAILED (-4)	/* the decode callback failed */

struct rdnvd_h264_sps {
	uint8_t valid;
	uint8_t profile_idc, constraint_flags, level_idc, id;
	uint8_t chroma_format_idc, separate_colour_plane_flag;
	uint8_t bit_depth_luma_minus8, bit_depth_chroma_minus8;
	uint8_t scaling_matrix_present;
	/* In the order of the bitstream (zig-zag); the lists that are not sent are filled by the rules of 7.4.2.1.1. */
	uint8_t scaling_list_4x4[6][16];
	uint8_t scaling_list_8x8[6][64];
	uint8_t log2_max_frame_num_minus4, pic_order_cnt_type;
	uint8_t log2_max_pic_order_cnt_lsb_minus4, delta_pic_order_always_zero_flag;
	int32_t offset_for_non_ref_pic, offset_for_top_to_bottom_field;
	uint8_t num_ref_frames_in_pic_order_cnt_cycle;
	int32_t offset_for_ref_frame[255];
	uint8_t max_num_ref_frames, gaps_in_frame_num_allowed_flag;
	uint8_t frame_mbs_only_flag, mb_adaptive_frame_field_flag, direct_8x8_inference_flag;
	uint32_t pic_width_in_mbs, pic_height_in_map_units;
	/* The frame's size in macroblocks and the part that is shown, in samples. */
	uint32_t width_mbs, height_mbs;
	uint32_t crop_left, crop_right, crop_top, crop_bottom;
	uint8_t vui_bitstream_restriction;
	uint8_t max_num_reorder_frames, max_dec_frame_buffering;
	/* From the level and the size, or the VUI's max_dec_frame_buffering. */
	uint8_t dpb_frames;
};

struct rdnvd_h264_pps {
	uint8_t valid;
	uint8_t id, sps_id;
	uint8_t entropy_coding_mode_flag, bottom_field_pic_order_in_frame_present_flag;
	uint8_t num_slice_groups_minus1, slice_group_map_type;
	uint16_t slice_group_change_rate_minus1;
	uint8_t num_ref_idx_default_active_minus1[2];
	uint8_t weighted_pred_flag, weighted_bipred_idc;
	int8_t pic_init_qp_minus26, pic_init_qs_minus26, chroma_qp_index_offset, second_chroma_qp_index_offset;
	uint8_t deblocking_filter_control_present_flag, constrained_intra_pred_flag;
	uint8_t redundant_pic_cnt_present_flag, transform_8x8_mode_flag;
	uint8_t pic_scaling_matrix_present;
	/* What was sent, resolved against the sequence's lists when the picture is decoded (see struct rdnvd_h264_picture). */
	uint8_t list_present[12], list_use_default[12];
	uint8_t scaling_list_4x4[6][16];
	uint8_t scaling_list_8x8[6][64];
};

/* A reference picture, as the decoder is told about it. */
struct rdnvd_h264_ref {
	/* FrameNum, or LongTermFrameIdx for a long-term picture. */
	uint32_t frame_num;
	int32_t poc[2];
	uint8_t is_long_term, is_non_existing;
	void *surface;
};

struct rdnvd_h264_slice {
	/* The NAL unit with its header byte, no start code; in the parser's own buffer. */
	const uint8_t *nal;
	size_t size;
};

/* One coded picture. Valid only during the decode callback. */
struct rdnvd_h264_picture {
	const struct rdnvd_h264_sps *sps;
	const struct rdnvd_h264_pps *pps;
	uint8_t idr, nal_ref_idc, is_reference;
	uint32_t frame_num;
	int32_t poc[2];
	uint8_t slice_type;			/* of the first slice: 0 P, 1 B, 2 I (3 SP and 4 SI count as P and I) */
	uint8_t num_ref_idx_active_minus1[2];	/* of the first slice */
	/* The scaling lists that apply (PPS over SPS over flat), bitstream order. */
	uint8_t scaling_list_4x4[6][16];
	uint8_t scaling_list_8x8[6][64];
	unsigned num_refs;
	struct rdnvd_h264_ref refs[RDNVD_H264_MAX_REFS];	/* short-term, newest first, then long-term */
	unsigned num_slices;
	const struct rdnvd_h264_slice *slices;
	uint64_t pts;
};

struct rdnvd_h264_callbacks {
	/*
	 * The sequence changes (the first parameter set, or a new SPS with other dimensions or
	 * reference counts at an IDR picture). Pictures of the old sequence have all been output by
	 * now. Returns 0, or an error to stop.
	 */
	int (*sequence)(void *user, const struct rdnvd_h264_sps *sps);
	/*
	 * Decode `pic`. On success set *surface to what holds the decoded picture; the parser keeps
	 * it while the picture is a reference or waits for output and hands it to `output`.
	 */
	int (*decode)(void *user, const struct rdnvd_h264_picture *pic, void **surface);
	/* A picture in display order. The parser keeps no claim on `surface` once `release` has been called for it too. */
	void (*output)(void *user, void *surface, uint64_t pts, const struct rdnvd_h264_sps *sps);
	/* The parser no longer holds `surface` (not a reference, already output). */
	void (*release)(void *user, void *surface);
};

struct rdnvd_h264;

struct rdnvd_h264 *rdnvd_h264_parser_new(const struct rdnvd_h264_callbacks *cb, void *user);
void rdnvd_h264_parser_free(struct rdnvd_h264 *h);

/* The configuration record of an MP4/QuickTime avcC box: its SPS and PPS go in; returns the NAL length size (1-4) or an error. */
int rdnvd_h264_parser_avcc(struct rdnvd_h264 *h, const uint8_t *avcc, size_t size);
/* Tell the parser the decoder is set up for `sps` already: the first picture of that sequence does not ask for a new one. */
void rdnvd_h264_parser_set_sequence(struct rdnvd_h264 *h, const struct rdnvd_h264_sps *sps);
/* The first usable sequence parameter set the parser has seen (from an avcC record, say), or NULL. */
const struct rdnvd_h264_sps *rdnvd_h264_parser_first_sps(const struct rdnvd_h264 *h);
/* NAL units after start codes (00 00 01); or, with nal_length_size 1 to 4, length prefixed. `pts` goes with the pictures that complete in it. */
int rdnvd_h264_parser_feed_annexb(struct rdnvd_h264 *h, const uint8_t *data, size_t size, uint64_t pts);
int rdnvd_h264_parser_feed_avcc(struct rdnvd_h264 *h, const uint8_t *data, size_t size, unsigned nal_length_size,
			 uint64_t pts);
/* The data fed so far ends an access unit: decode the picture that is complete. */
int rdnvd_h264_parser_end_au(struct rdnvd_h264 *h);
/* End of the stream or a seek: decode what is pending, output everything (drop = 1: forget instead of output). */
int rdnvd_h264_parser_flush(struct rdnvd_h264 *h, int drop);

#ifdef __cplusplus
}
#endif

#endif /* RDNVD_H264_H */
