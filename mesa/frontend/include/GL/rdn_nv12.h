/* Copyright (c) 2026 kouta-kun and Claude
 * SPDX-License-Identifier: MIT
 */
#ifndef RDN_NV12_H
#define RDN_NV12_H

#include <stdint.h>
#include "GL/osmesa.h"

#ifdef __cplusplus
extern "C" {
#endif

struct OSMesaNV12Source {
   uint32_t handle, offset, bytes, pitch;
   uint32_t width, height;
   uint32_t y_top, y_bottom, uv_top, uv_bottom;
};

enum OSMesaNV12Result {
   OSMESA_NV12_OK = 0,
   OSMESA_NV12_UNSUPPORTED = 1,
   OSMESA_NV12_RETIRED = 2,
   OSMESA_NV12_PENDING = 3
};

/* Plane offsets are relative to the kernel-validated source span.
 * Destination names must already have matching R8/RG8 level-zero storage.
 * PENDING retains all submitted resources; the caller must retain the source lease.
 */
GLAPI int GLAPIENTRY OSMesaCanPackNV12(OSMesaContext context);
GLAPI int GLAPIENTRY OSMesaPackNV12Store(OSMesaContext context,
                                       const struct OSMesaNV12Source *source,
                                       GLuint destination_y, GLuint destination_uv,
                                       uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
#endif
