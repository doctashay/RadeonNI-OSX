/* Legacy UVD H.264 message packing; no hardware access.
 * SPDX-License-Identifier: MIT
 */
#ifndef RDN_UVD_H264_H
#define RDN_UVD_H264_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RDN_UVD_H264_VERSION 1u
#define RDN_UVD_H264_MESSAGE_SIZE 3556u
#define RDN_UVD_H264_MESSAGE_BYTES 3568u
#define RDN_UVD_H264_MAX_WIDTH 1920u
#define RDN_UVD_H264_MAX_HEIGHT 1088u
#define RDN_UVD_H264_MAX_REFS 16u
#define RDN_UVD_H264_MAX_BITSTREAM_BYTES (16u * 1024u * 1024u)
#define RDN_UVD_H264_TARGET_LINEAR 0u
#define RDN_UVD_H264_TARGET_FIELDS 1u

/* Host-order metadata, not firmware structs or capability claims.
 * Map Baseline/Main level_idc 11 with constraint_set3_flag to level 9 before validation.
 */
struct rdn_uvd_h264_config {
	uint32_t version;
	uint32_t profile_idc; /* 66, 77 or 100. */
	uint32_t level_idc;   /* Normalize SPS level 1b to 9; it is unsupported. */
	uint32_t coded_width;
	uint32_t coded_height;
	uint32_t max_num_ref_frames;
	uint32_t chroma_format_idc;
	uint32_t bit_depth_luma_minus8;
	uint32_t bit_depth_chroma_minus8;
	uint32_t frame_mbs_only_flag;
	uint32_t mb_adaptive_frame_field_flag;
	uint32_t reserved[3];
};

struct rdn_uvd_h264_picture {
	uint32_t version;
	uint32_t sps_info_flags; /* UVD bits: direct8x8, MBAFF, frame-only, delta-POC-zero. */
	uint32_t pps_info_flags; /* UVD bits 0..8; weighted_bipred_idc occupies 4..5. */
	uint32_t log2_max_frame_num_minus4;
	uint32_t pic_order_cnt_type;
	uint32_t log2_max_pic_order_cnt_lsb_minus4;
	int32_t pic_init_qp_minus26;
	int32_t pic_init_qs_minus26;
	int32_t chroma_qp_index_offset;
	int32_t second_chroma_qp_index_offset;
	uint32_t num_slice_groups_minus1;
	uint32_t slice_group_map_type;
	uint32_t slice_group_change_rate_minus1;
	uint32_t num_ref_idx_l0_active_minus1;
	uint32_t num_ref_idx_l1_active_minus1;
	uint32_t field_pic_flag;
	uint32_t bottom_field_flag;
	uint32_t frame_num;
	uint32_t reference_count;
	uint32_t frame_num_list[16];
	int32_t field_order_cnt[2];
	int32_t field_order_cnt_list[16][2];
	/* Resolved, nonzero scaling weights in raster order, not H.264 scan order. */
	uint8_t scaling_list_4x4[6][16];
	uint8_t scaling_list_8x8[2][64];
	uint32_t reserved[4];
};

/* Kernel-owned layout, derived by layout_init; never accept a client layout. */
struct rdn_uvd_h264_layout {
	uint32_t target_field_mode;
	uint32_t db_pitch;
	uint32_t dpb_bytes;
	uint32_t dpb_allocation_bytes;
	uint32_t target_pitch;
	uint32_t target_rows;
	uint32_t luma_top_offset;
	uint32_t luma_bottom_offset;
	uint32_t chroma_top_offset;
	uint32_t chroma_bottom_offset;
	uint32_t target_bytes;
	uint32_t target_allocation_bytes;
	uint32_t packed_nv12_bytes;
	uint32_t message_feedback_bytes;
};

#define RDN_UVD_H264_MSG_CREATE 0u
#define RDN_UVD_H264_MSG_DECODE 1u
#define RDN_UVD_H264_MSG_DESTROY 2u
#define RDN_UVD_H264_IB_LE 0u
#define RDN_UVD_H264_IB_BE 1u
#define RDN_UVD_H264_IB_MAX_BYTES 128u
#define RDN_UVD_H264_FEEDBACK_BYTES 2048u

/* Dedicated kernel-owned usable spans; not a user-client ABI. */
struct rdn_uvd_h264_gpu_range {
	uint64_t address;
	uint32_t bytes;
	uint32_t reserved;
};

struct rdn_uvd_h264_ib_buffers {
	struct rdn_uvd_h264_gpu_range vcpu;
	struct rdn_uvd_h264_gpu_range ring;
	struct rdn_uvd_h264_gpu_range ib;
	struct rdn_uvd_h264_gpu_range message;
	struct rdn_uvd_h264_gpu_range dpb;
	struct rdn_uvd_h264_gpu_range bitstream;
	struct rdn_uvd_h264_gpu_range target;
	struct rdn_uvd_h264_gpu_range feedback;
};

/* Coded dimensions must be multiples of 16. Field storage also accepts progressive input. */
int rdn_uvd_h264_layout_init(const struct rdn_uvd_h264_config *config,
	uint32_t target_field_mode, struct rdn_uvd_h264_layout *layout);

/* Returns the 128-byte aligned size; the caller must zero the trailing bytes. */
int rdn_uvd_h264_bitstream_size(uint32_t bytes, uint32_t *padded_bytes);

/* On failure, outputs are unchanged. Output may be unaligned, but cannot alias inputs.
 * Handles and feedback serials are nonzero kernel-owned values. These functions
 * do not validate bitstream syntax, allocate buffers, submit work or prove completion.
 */
int rdn_uvd_h264_pack_create(const struct rdn_uvd_h264_config *config,
	const struct rdn_uvd_h264_layout *layout, uint32_t handle,
	void *output, size_t capacity);
int rdn_uvd_h264_pack_decode(const struct rdn_uvd_h264_config *config,
	const struct rdn_uvd_h264_picture *picture,
	const struct rdn_uvd_h264_layout *layout, uint32_t handle,
	uint32_t feedback, uint32_t bitstream_bytes, void *output, size_t capacity);
int rdn_uvd_h264_pack_destroy(uint32_t handle, void *output, size_t capacity);

/* All GPU spans must be disjoint, including VCPU/ring; unused decode spans are zero.
 * Message contents must come from the matching pack_* call. Decode also needs its
 * validated config/layout, zero-padded bitstream and LE feedback[0] = 2048.
 * IB byte order must match LMI's IB swap mode, independently of the LE messages.
 * word_count receives 16 or 32; output may be unaligned and cannot alias inputs/count.
 * On failure output/count are unchanged. No allocation, submission or fence is done.
 */
int rdn_uvd_h264_build_ib(const struct rdn_uvd_h264_config *config,
	const struct rdn_uvd_h264_layout *layout, uint32_t message_type,
	uint32_t word_order, uint32_t bitstream_bytes,
	const struct rdn_uvd_h264_ib_buffers *buffers,
	void *output, size_t capacity, uint32_t *word_count);

#ifdef __cplusplus
}
#endif
#endif
