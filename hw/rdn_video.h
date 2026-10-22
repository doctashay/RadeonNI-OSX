/* Bounded H.264 video user-client interface. */
#ifndef RDN_VIDEO_API_H
#define RDN_VIDEO_API_H
#include <stdint.h>
#include "rdn_uvd_h264.h"
#define RDN_VIDEO_UC_TYPE 0x72646e76u /* rdnv */
#define RDN_VIDEO_API_VERSION 1u
#define RDN_VIDEO_MEMORY_UPLOAD 0x100u
#define RDN_VIDEO_MEMORY_OUTPUT0 0x101u
#define RDN_VIDEO_MEMORY_OUTPUT1 0x102u
#define RDN_VIDEO_MAX_UPLOAD_BYTES (4u * 1024u * 1024u)
#define RDN_VIDEO_OUTPUT_SLOTS 2u
#define RDN_VIDEO_CREATE_VALIDATION 1u
#define RDN_VIDEO_CAP_READY 1u
#define RDN_VIDEO_CAP_PRODUCTION 2u
#define RDN_VIDEO_CAP_VALIDATION 4u
#define RDN_VIDEO_CAP_BUSY 8u
#define RDN_VIDEO_FORMAT_NV12 1u
#define RDN_VIDEO_PROFILE_BASELINE 1u
#define RDN_VIDEO_PROFILE_MAIN 2u
#define RDN_VIDEO_PROFILE_HIGH 4u
enum { RDN_VIDEO_QUERY, RDN_VIDEO_CREATE, RDN_VIDEO_DECODE,
       RDN_VIDEO_RELEASE, RDN_VIDEO_DESTROY, RDN_VIDEO_METHOD_COUNT };
/* All fields use CPU byte order; no pointers, size_t or compiler bool. */
struct rdn_video_token { uint32_t high, low; };
struct rdn_video_query {
 uint32_t version, bytes, flags, unavailable_reason;
 uint32_t max_width, max_height, max_refs, profile_mask, level_mask;
 uint32_t max_upload_bytes, max_sessions, max_inflight, output_slots;
 uint32_t max_vram_bytes, max_cpu_bytes, reserved[17];
};
struct rdn_video_create {
 uint32_t version, bytes;
 struct rdn_uvd_h264_config config;
 uint32_t flags, reserved[7];
};
struct rdn_video_session {
 uint32_t version, bytes;
 struct rdn_video_token session;
 uint32_t coded_width, coded_height, format, y_stride, uv_stride;
 uint32_t y_offset, uv_offset, output_bytes, upload_bytes;
 uint32_t output_slots, upload_memory_type, output0_memory_type;
 uint32_t output1_memory_type, reserved[15];
};
struct rdn_video_decode {
 uint32_t version, bytes;
 struct rdn_video_token session;
 uint32_t request_sequence, bitstream_bytes, reserved[2];
 struct rdn_uvd_h264_picture picture;
};
struct rdn_video_output {
 uint32_t version, bytes;
 struct rdn_video_token session;
 uint32_t request_sequence, output_sequence, slot, flags;
 uint32_t coded_width, coded_height, format, y_stride, uv_stride;
 uint32_t y_offset, uv_offset, output_bytes, reserved[16];
};
struct rdn_video_release {
 uint32_t version, bytes;
 struct rdn_video_token session;
 uint32_t output_sequence, slot, reserved[2];
};
struct rdn_video_destroy {
 uint32_t version, bytes;
 struct rdn_video_token session;
 uint32_t reserved[4];
};
#endif
