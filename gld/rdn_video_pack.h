/* GPU video packing ABI shared with Aurora. */
#ifndef RDN_VIDEO_PACK_H
#define RDN_VIDEO_PACK_H

#include <stdint.h>

#define RDN_GL_VIDEO_PACK_VERSION_1 1u
#define RDN_GL_VIDEO_PACK_CAP_NV12_COMPLETED 1u
#define RDN_GL_VIDEO_PACK_MAX_TIMEOUT_MS 2000u

enum RDNGLVideoPackResult {
 RDN_GL_VIDEO_PACK_OK = 0,
 RDN_GL_VIDEO_PACK_UNSUPPORTED = 1,
 RDN_GL_VIDEO_PACK_FAILED_RETIRED = 2,
 RDN_GL_VIDEO_PACK_FAILED_PENDING = 3
};

struct RDNGLVideoPackRequestV1 {
 uint32_t version, bytes, flags, video_connection;
 uint32_t source_token[8];
 uint32_t coded_width, coded_height, destination_y, destination_uv;
 uint32_t timeout_ms, reserved[7];
};

int RDNGLQueryVideoPackingV1(uint32_t version, uint32_t *capabilities);
int RDNGLPackVideoFrameV1(const struct RDNGLVideoPackRequestV1 *request);

#endif
