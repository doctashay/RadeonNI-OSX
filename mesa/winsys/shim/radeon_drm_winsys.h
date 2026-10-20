/*
 * Stand-in for winsys/radeon/drm/radeon_drm_winsys.h, so that Mesa's
 * radeon_drm_surface.c builds unchanged on top of this winsys: it only
 * needs a winsys structure with these names in it.
 *
 * Copyright (c) 2026 kouta-kun and Claude
 * SPDX-License-Identifier: MIT
 */

#ifndef RDN_RADEON_DRM_WINSYS_SHIM_H
#define RDN_RADEON_DRM_WINSYS_SHIM_H

#include "winsys/radeon_winsys.h"
#include "util/list.h"
#include "util/simple_mtx.h"
#include "util/u_inlines.h"
#include "rdn_device.h"

struct radeon_surface_manager;

enum radeon_generation {
   DRV_R300,
   DRV_R600,
   DRV_SI
};

struct radeon_drm_winsys {
   struct radeon_winsys base;
   struct pipe_reference reference;

   /* What radeon_drm_surface.c uses. */
   struct radeon_surface_manager *surf_man;
   struct radeon_info info;
   enum radeon_generation gen;

   /* This winsys. */
   struct rdn_device *dev;
   simple_mtx_t lock;
   /* Command buffers in video memory whose fence has not been reached. */
   struct list_head pending_ibs;
   /*
    * Video memory this process has let go of and keeps for its next
    * request of the same size (rdn_winsys.c, "The cache"): in the order it
    * was let go of, and the same entries by size.
    */
   struct list_head cache_all;
   struct list_head cache_buckets[128];
   uint64_t cached_bytes[5];   /* by region: enum rdn_place (RDN_REGIONS in rdn_winsys.c) */
   /* The newest fence known to have been reached. */
   uint32_t last_done;
   bool have_last_done;
   /* The same for the video decoder's fences (numbered apart), and whether it is up and ever used. */
   uint32_t uvd_last_done;
   bool have_uvd_last_done;
   bool uvd_ready;
   bool uvd_used;
   /*
    * A process that decodes (rdn_winsys_set_video): its shareable buffers are the decoder's. When the decoder runs in a
    * segment of the video memory beyond the aperture (docs/UVD.md) they go there (uvd_seg), and the type and number of the
    * newest decode message are what the feedback must carry (rdn_winsys_uvd_answered()).
    */
   bool video, uvd_seg;
   uint32_t uvd_msg_type, uvd_msg_num;
   /* The newest decoder stream's message and feedback buffers as aperture offsets (rdn_winsys_uvd_answered()). */
   bool uvd_check;
   uint64_t uvd_msg_at, uvd_fb_at;
   uint64_t allocated_bytes;
   /*
    * RDN_STATS: what is allocated now, by where it is (enum rdn_place)
    * and by size class (class c: under 4 KB << c), and the most there was.
    */
   struct {
      uint64_t bytes[5], count[5], padding[5];
      uint64_t class_bytes[5][24], class_count[5][24];
      uint64_t peak_bytes[5], creates, cache_hits, overflowed;
   } mem_stats;
   uint64_t num_flushes;
};

void radeon_surface_init_functions(struct radeon_drm_winsys *ws);

#endif /* RDN_RADEON_DRM_WINSYS_SHIM_H */
