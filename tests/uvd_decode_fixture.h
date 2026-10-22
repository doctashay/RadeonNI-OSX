/* Legacy UVD H.264 test data; this is not a decode implementation. */
#ifndef RDN_UVD_DECODE_FIXTURE_H
#define RDN_UVD_DECODE_FIXTURE_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    RDN_UVD_FIXTURE_MESSAGE_BYTES = 3568,
    RDN_UVD_FIXTURE_MESSAGE_SIZE = 3556,
    RDN_UVD_FIXTURE_BITSTREAM_BYTES = 1408,
    RDN_UVD_FIXTURE_WIDTH = 864,
    RDN_UVD_FIXTURE_HEIGHT = 480,
    RDN_UVD_FIXTURE_PITCH = 1024,
    RDN_UVD_FIXTURE_DPB_BYTES = 15923584,
    RDN_UVD_FIXTURE_TARGET_BYTES = 737280,
    RDN_UVD_FIXTURE_NV12_BYTES = 622080
};

enum rdn_uvd_fixture_message {
    RDN_UVD_FIXTURE_CREATE = 0,
    RDN_UVD_FIXTURE_DECODE = 1,
    RDN_UVD_FIXTURE_DESTROY = 2
};

extern const uint8_t rdn_uvd_fixture_create[RDN_UVD_FIXTURE_MESSAGE_BYTES];
extern const uint8_t rdn_uvd_fixture_decode[RDN_UVD_FIXTURE_MESSAGE_BYTES];
extern const uint8_t rdn_uvd_fixture_destroy[RDN_UVD_FIXTURE_MESSAGE_BYTES];
extern const uint8_t rdn_uvd_fixture_bitstream[RDN_UVD_FIXTURE_BITSTREAM_BYTES];
extern const char rdn_uvd_fixture_upstream_sha256[];
extern const char rdn_uvd_fixture_reference_sha256[];

/* Writes a byte-exact LE message, changing only its nonzero session handle. */
int rdn_uvd_fixture_message_copy(enum rdn_uvd_fixture_message type,
                               uint32_t handle, void *dst, size_t capacity);

/* Weaves the fixture's linear, field-separated target into packed NV12. */
int rdn_uvd_fixture_unpack_nv12(const void *target, size_t target_bytes,
                               void *nv12, size_t nv12_bytes);
#ifdef __cplusplus
}
#endif
#endif
