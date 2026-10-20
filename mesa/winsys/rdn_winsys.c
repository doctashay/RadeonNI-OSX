/*
 * Mesa winsys for the Radeon HD 7570 without the Linux DRM.
 *
 * It gives r600 what the radeon DRM winsys gives it, over rdn_device.h:
 *  - buffers are ranges of video memory, mapped through the aperture, each
 *    with a fixed GPU address. The driver is told it has GPU virtual
 *    addressing (r600_has_virtual_memory), so it writes those addresses
 *    into the command stream itself and nothing has to be patched;
 *  - a command stream is copied into video memory and run as one indirect
 *    buffer, synchronously on the calling thread;
 *  - a fence is the device's fence number.
 * Surface layouts come from Mesa's own radeon_drm_surface.c and
 * radeon_surface.c, built unchanged (see shim/).
 *
 * The video decoder (UVD) is a second command stream type, with fences of
 * its own. r600's radeon_uvd.c names the buffers it uses the way the radeon
 * kernel driver takes them (the offset in the buffer and four times the
 * buffer's place in the stream's list); the addresses are filled in when the
 * stream is flushed, as the kernel does. The two rings do not know of each
 * other, so a stream waits (on the CPU) for the other ring's work on the
 * buffers it names before it is submitted.
 *
 * Not here: buffer sharing between processes, a second (DMA) ring, any
 * checking of the command stream.
 *
 * Copyright (c) 2026 kouta-kun and Claude
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "radeon_drm_winsys.h"
#include "rdn_winsys.h"
#include "rdn_trace.h"
#include "radeon_surface.h"

#include "pipe/p_defines.h"
#include "pipe/p_screen.h"
#include "util/os_time.h"
#include "util/u_cpu_detect.h"
#include "util/u_math.h"
#include "util/u_memory.h"

uint32_t rdn_shim_device_id;
uint32_t rdn_shim_tiling_config;

/* Command words per stream; r600 flushes itself well below this. */
#define RDN_CS_MAX_DW (64 * 1024)
/* A video decoder stream is a few dozen words per picture. */
#define RDN_UVD_CS_MAX_DW 4096

/*
 * Where a buffer's memory is: video memory the CPU maps through the
 * aperture, video memory beyond it, or the program's own memory, which
 * the GPU reaches through the GART.
 */
/* RDN_USER: the program's own memory behind the GART, only ever given back. */
/* RDN_SEGMENT: the video decoder's 256 MB of video memory beyond the aperture (docs/UVD.md); the CPU sees a copy. */
enum rdn_place { RDN_VISIBLE, RDN_HIDDEN, RDN_GART, RDN_USER, RDN_SEGMENT, RDN_REGIONS };

/* What r600 holds on to: a command buffer it may ask about or wait for. */
struct rdn_fence {
   struct pipe_reference reference;
   uint32_t number;
   bool submitted;
   bool reached;
   /* The video decoder's numbering. */
   bool uvd;
};

/*
 * Until when the GPU uses a piece of memory: the device's number of the
 * last command buffer that does, if there has been one. A number and not
 * a reference to a fence, because every buffer of every command buffer
 * gets one at every flush.
 */
struct rdn_busy {
   uint32_t number;
   bool used;
   /* Of the video decoder's numbering. */
   bool uvd;
};

struct rdn_bo {
   struct pb_buffer_lean base;
   uint64_t offset;
   enum radeon_bo_domain domain;
   enum radeon_bo_flag flags;
   /* The last submissions that use it at all, and that write to it. */
   struct rdn_busy last_use;
   struct rdn_busy last_write;
   /* The last video decoder submission that uses it (read or write). */
   struct rdn_busy last_uvd;
   /* Memory that is not from the allocator (the screen). */
   bool foreign;
   void *external_store;
   uint32_t diagnostic_id;
   enum rdn_place region;
   /* The program's own memory behind the GART (rdn_buffer_from_ptr). */
   void *user;
   /*
    * In the decoder's segment (RDN_SEGMENT): what the CPU maps is this copy in system memory; `dirty` when it has
    * been mapped for writing and not yet sent to video memory (before the next decoder stream that names the buffer),
    * `shadow_valid` when it holds what video memory holds.
    */
   uint8_t *shadow;
   bool dirty, shadow_valid;
};

static int rdn_next_diagnostic_id;

struct rdn_cs_buffer {
   struct rdn_bo *bo;
   unsigned usage;
   /* Its place in the command stream's table. */
   unsigned slot;
};

/* Slots of a command stream's table to begin with; a power of two. */
#define RDN_CS_HASH 1024

struct rdn_cs {
   struct radeon_drm_winsys *ws;
   struct rdn_cs_buffer *buffers;
   unsigned num_buffers, max_buffers;
   /*
    * Where in `buffers` each buffer is, found by its address: a table
    * with open addressing that is never more than half full. -1 is an
    * empty slot.
    */
   int32_t *hash;
   unsigned hash_size, hash_shift;
   void (*flush_cs)(void *ctx, unsigned flags, struct pipe_fence_handle **fence);
   void *flush_data;
   struct rdn_fence *next_fence;
   /* A stream of video decoder commands. */
   bool uvd;
};

struct rdn_cached {
   struct list_head all, bucket;
   uint64_t offset, size;
   enum rdn_place region;
   /* The last command buffer that used the memory. */
   struct rdn_busy busy;
   /* ws->num_flushes when it was let go of. */
   uint64_t stamp;
};

struct rdn_pending_ib {
   struct list_head list;
   uint64_t offset, size;
   struct rdn_busy busy;
};

static inline struct radeon_drm_winsys *rdn_winsys(struct radeon_winsys *rws)
{
   return (struct radeon_drm_winsys *)rws;
}

static inline struct rdn_bo *rdn_bo(struct pb_buffer_lean *buf)
{
   return (struct rdn_bo *)buf;
}

/*
 * Fences
 */

static void rdn_fence_set(struct rdn_fence **dst, struct rdn_fence *src)
{
   struct rdn_fence *old = *dst;

   if (pipe_reference(old ? &old->reference : NULL, src ? &src->reference : NULL))
      FREE(old);
   *dst = src;
}

static struct rdn_fence *rdn_fence_new(void)
{
   struct rdn_fence *fence = CALLOC_STRUCT(rdn_fence);

   if (fence)
      pipe_reference_init(&fence->reference, 1);
   return fence;
}

/*
 * True once the GPU has finished command buffer `number` (of the video
 * decoder's, with `uvd`).
 */
static bool rdn_number_wait(struct radeon_drm_winsys *ws, uint32_t number, bool uvd,
                            uint64_t timeout_ns)
{
   uint32_t *last_done = uvd ? &ws->uvd_last_done : &ws->last_done;
   bool *have_last_done = uvd ? &ws->have_uvd_last_done : &ws->have_last_done;

   /* They finish in the order of their numbers: no need to ask. */
   if (*have_last_done && (int32_t)(number - *last_done) <= 0)
      return true;
   if (uvd) {
      if (ws->dev->uvd_fence_wait(ws->dev, number, timeout_ns))
         return false;
   } else if (timeout_ns == 0) {
      if (!ws->dev->fence_done(ws->dev, number))
         return false;
   } else if (ws->dev->fence_wait(ws->dev, number, timeout_ns)) {
      return false;
   }
   if (!*have_last_done || (int32_t)(number - *last_done) > 0) {
      *last_done = number;
      *have_last_done = true;
   }
   return true;
}

static bool rdn_busy_wait(struct radeon_drm_winsys *ws, const struct rdn_busy *busy,
                          uint64_t timeout_ns)
{
   return !busy->used || rdn_number_wait(ws, busy->number, busy->uvd, timeout_ns);
}

static bool rdn_fence_wait(struct radeon_drm_winsys *ws, struct rdn_fence *fence,
                           uint64_t timeout_ns)
{
   if (!fence || fence->reached)
      return true;
   if (!fence->submitted)
      return false;
   if (!rdn_number_wait(ws, fence->number, fence->uvd, timeout_ns))
      return false;
   fence->reached = true;
   return true;
}

static bool rdn_ws_fence_wait(struct radeon_winsys *rws,
                              struct pipe_fence_handle *fence, uint64_t timeout)
{
   return rdn_fence_wait(rdn_winsys(rws), (struct rdn_fence *)fence, timeout);
}

static void rdn_ws_fence_reference(struct radeon_winsys *rws,
                                   struct pipe_fence_handle **dst,
                                   struct pipe_fence_handle *src)
{
   rdn_fence_set((struct rdn_fence **)dst, (struct rdn_fence *)src);
}

/*
 * Buffers
 */

/*
 * The cache.
 *
 * Every piece of video memory is the kext's to give, and asking it is a
 * kernel call. A program that draws replaces its vertex buffers every
 * frame, and r600's upload path keeps asking for fresh ones, so memory a
 * buffer or a command buffer lets go of is kept here and handed to the
 * next request of the same size, once the GPU has finished with it. What
 * nobody asked for again within RDN_CACHE_FLUSHES command buffers goes
 * back to the kext, as does everything above RDN_CACHE_BYTES. All of it
 * is called with the lock held.
 */
#define RDN_CACHE_FLUSHES 128
static const uint64_t RDN_CACHE_BYTES[RDN_REGIONS] = { 32ull << 20, 128ull << 20, 64ull << 20, 0, 0 };

static struct list_head *rdn_cache_bucket(struct radeon_drm_winsys *ws,
                                          uint64_t size, enum rdn_place region)
{
   unsigned h = (unsigned)(size >> 8) ^ (unsigned)(size >> 15) ^ (region * 0x55);

   return &ws->cache_buckets[h % ARRAY_SIZE(ws->cache_buckets)];
}

/* Give memory back to where it came from. */
static void rdn_region_free(struct radeon_drm_winsys *ws, enum rdn_place region,
                            uint64_t offset, uint64_t size)
{
   if (region == RDN_USER)
      ws->dev->gart_unbind_user(ws->dev, offset, size);
   else if (region == RDN_GART)
      ws->dev->gart_free(ws->dev, offset);
   else
      ws->dev->free(ws->dev, offset);
}

static void rdn_cache_drop(struct radeon_drm_winsys *ws, struct rdn_cached *c)
{
   ws->cached_bytes[c->region] -= c->size;
   list_del(&c->all);
   list_del(&c->bucket);
   FREE(c);
}

/* Keep memory for later, or give it back at once if that cannot be noted. */
static void rdn_cache_put(struct radeon_drm_winsys *ws, uint64_t offset,
                          uint64_t size, enum rdn_place region,
                          const struct rdn_busy *busy)
{
   struct rdn_cached *c = CALLOC_STRUCT(rdn_cached);

   if (!c) {
      rdn_busy_wait(ws, busy, OS_TIMEOUT_INFINITE);
      rdn_region_free(ws, region, offset, size);
      return;
   }
   c->offset = offset;
   c->size = size;
   c->region = region;
   c->stamp = ws->num_flushes;
   c->busy = *busy;
   list_addtail(&c->all, &ws->cache_all);
   list_addtail(&c->bucket, rdn_cache_bucket(ws, size, region));
   ws->cached_bytes[region] += size;
}

/* Memory of exactly this size that the GPU has finished with, if any. */
static bool rdn_cache_get(struct radeon_drm_winsys *ws, uint64_t size,
                          uint64_t alignment, enum rdn_place region, uint64_t *offset)
{
   struct list_head *bucket = rdn_cache_bucket(ws, size, region);
   unsigned looked = 0;

   list_for_each_entry_safe(struct rdn_cached, c, bucket, bucket) {
      if (++looked > 32)
         break;
      if (c->size != size || c->region != region || (c->offset & (alignment - 1)))
         continue;
      /* The oldest first: if the GPU still has that one, it has the rest. */
      if (!rdn_busy_wait(ws, &c->busy, 0))
         break;
      *offset = c->offset;
      rdn_cache_drop(ws, c);
      return true;
   }
   return false;
}

/*
 * Give back to the kext what has been kept too long or is too much; with
 * `all`, everything, waiting for the GPU where it has to.
 */
static void rdn_cache_trim(struct radeon_drm_winsys *ws, bool all)
{
   list_for_each_entry_safe(struct rdn_cached, c, &ws->cache_all, all) {
      bool old = ws->num_flushes - c->stamp > RDN_CACHE_FLUSHES;
      bool much = ws->cached_bytes[c->region] > RDN_CACHE_BYTES[c->region];

      if (!all && !old && !much)
         continue;
      if (!rdn_busy_wait(ws, &c->busy, all ? OS_TIMEOUT_INFINITE : 0)) {
         if (all)
            continue;
         break;
      }
      rdn_region_free(ws, c->region, c->offset, c->size);
      rdn_cache_drop(ws, c);
   }
}

/*
 * RDN_STATS=1: account for every buffer; at exit and when memory runs out,
 * say what was allocated at the moment the most was.
 */
static uint64_t rdn_stats_most;
static struct rdn_stats_picture {
   __typeof__(((struct radeon_drm_winsys *)0)->mem_stats) mem_stats;
   uint64_t cached_bytes[RDN_REGIONS];
} rdn_stats_at_most;

static bool rdn_mem_stats_on(void)
{
   static int on = -1;

   if (on < 0)
      on = getenv("RDN_STATS") != NULL;
   return on;
}

static unsigned rdn_size_class(uint64_t size)
{
   unsigned c = 0;

   while (c < 23 && size >= (4096ull << c))
      c++;
   return c;
}

static void rdn_mem_stats_account(struct radeon_drm_winsys *ws, uint64_t size,
                                  enum rdn_place region, int sign)
{
   unsigned c = rdn_size_class(size);

   if (!rdn_mem_stats_on())
      return;
   /* The kext gives memory out at multiples of 4 KB. */
   ws->mem_stats.bytes[region] += sign * (int64_t)size;
   ws->mem_stats.count[region] += sign;
   ws->mem_stats.padding[region] += sign * (int64_t)(align64(size, 4096) - size);
   ws->mem_stats.class_bytes[region][c] += sign * (int64_t)size;
   ws->mem_stats.class_count[region][c] += sign;
   if (ws->mem_stats.bytes[region] > ws->mem_stats.peak_bytes[region])
      ws->mem_stats.peak_bytes[region] = ws->mem_stats.bytes[region];
   /* Keep the picture of the moment the most was allocated. */
   if (ws->mem_stats.bytes[0] + ws->mem_stats.bytes[1] + ws->mem_stats.bytes[2] > rdn_stats_most) {
      rdn_stats_most = ws->mem_stats.bytes[0] + ws->mem_stats.bytes[1] + ws->mem_stats.bytes[2];
      rdn_stats_at_most.mem_stats = ws->mem_stats;
      rdn_stats_at_most.cached_bytes[0] = ws->cached_bytes[0];
      rdn_stats_at_most.cached_bytes[1] = ws->cached_bytes[1];
      rdn_stats_at_most.cached_bytes[2] = ws->cached_bytes[2];
      rdn_stats_at_most.cached_bytes[3] = ws->cached_bytes[3];
      rdn_stats_at_most.cached_bytes[4] = ws->cached_bytes[4];
   }
}

static void rdn_mem_stats_print(const char *when)
{
   struct rdn_stats_picture *ws = &rdn_stats_at_most;
   unsigned h, c;

   if (!rdn_stats_most)
      return;
   static const char *const where[RDN_REGIONS] = {
      "in the aperture", "beyond the aperture", "in system memory (GART)",
      "the program's own memory (GART)", "in the decoder's segment"
   };

   for (h = 0; h < RDN_REGIONS; h++) {
      fprintf(stderr, "rdn memory (%s), %s: %llu MB in %llu buffers, most %llu MB, "
              "%llu MB lost to 4 KB rounding, %llu MB kept in the cache\n",
              when, where[h],
              (unsigned long long)(ws->mem_stats.bytes[h] >> 20),
              (unsigned long long)ws->mem_stats.count[h],
              (unsigned long long)(ws->mem_stats.peak_bytes[h] >> 20),
              (unsigned long long)(ws->mem_stats.padding[h] >> 20),
              (unsigned long long)(ws->cached_bytes[h] >> 20));
      for (c = 0; c < 24; c++)
         if (ws->mem_stats.class_count[h][c])
            fprintf(stderr, "rdn memory   under %llu KB: %llu buffers, %llu MB\n",
                    (unsigned long long)(4ull << c),
                    (unsigned long long)ws->mem_stats.class_count[h][c],
                    (unsigned long long)(ws->mem_stats.class_bytes[h][c] >> 20));
   }
   fprintf(stderr, "rdn memory: %llu buffers created in all, %llu of them from the cache, "
           "%llu put in system memory because video memory was full\n",
           (unsigned long long)ws->mem_stats.creates,
           (unsigned long long)ws->mem_stats.cache_hits,
           (unsigned long long)ws->mem_stats.overflowed);
}

static void rdn_cs_stats_print(void);

static void rdn_mem_stats_exit(void)
{
   rdn_mem_stats_print("when the most was allocated");
   rdn_cs_stats_print();
}

/*
 * The decoder and the video memory beyond the aperture (docs/UVD.md). Split mode is the default: the firmware, ring and messages stay in the
 * aperture, the decoder's reference and output buffers go to a hidden 256 MB segment. RDN_UVD_SEGMENT in the program's environment, or the first character of the
 * file below, overrides it: 0 for the aperture only, 1 for all of the decoder in a segment (it does not survive a restart of the block), 2 for split mode.
 * Whoever starts the decoder first decides; after that the decoder's place is the kext's, whatever a later program asks. A kext without the
 * segment methods, or without room in any segment, leaves the decoder in the aperture.
 */
#define RDN_UVD_SEGMENT_FILE "/Library/Application Support/RadeonNI/uvd-segment"

static int rdn_segment_wanted(void)
{
   static int mode = -1;

   if (mode < 0) {
      const char *e = getenv("RDN_UVD_SEGMENT");

      mode = 2;
      if (e) {
         mode = atoi(e);
      } else {
         FILE *f = fopen(RDN_UVD_SEGMENT_FILE, "r");
         int c = f ? fgetc(f) : EOF;

         if (f)
            fclose(f);
         if (c == '0' || c == '1' || c == '2')
            mode = c - '0';
      }
      if (mode < 0 || mode > 2)
         mode = 2;
   }
   return mode;
}

/* With the lock held: the decoder is started (once per process) in the place asked for, and where it runs is noted. */
static bool rdn_uvd_prepare(struct radeon_drm_winsys *ws)
{
   if (ws->uvd_ready)
      return true;
   if (ws->dev->uvd_start) {
      uint64_t first = 0, size = 0;
      bool split = false;

      if (ws->dev->uvd_start(ws->dev, rdn_segment_wanted(), &first, &size, &split))
         return false;
      ws->uvd_seg = size != 0 && !split && ws->dev->uvd_alloc && ws->dev->vram_put && ws->dev->vram_get &&
                    ws->dev->uvd_submit_words;
      ws->uvd_split = size != 0 && split && ws->dev->uvd_alloc && ws->dev->vram_put && ws->dev->vram_get;
      ws->uvd_ready = true;
   } else {
      ws->uvd_ready = !ws->dev->uvd_init(ws->dev);
   }
   return ws->uvd_ready;
}

/* The CPU's view of a buffer in the decoder's segment: a copy in system memory (made when first wanted). */
static void *rdn_segment_shadow(struct radeon_drm_winsys *ws, struct rdn_bo *bo, enum pipe_map_flags usage)
{
   if (!bo->shadow) {
      bo->shadow = CALLOC(1, bo->base.size);
      if (!bo->shadow)
         return NULL;
      bo->shadow_valid = false;
   }
   /* A reader wants what video memory holds (slow: through the register window). */
   if ((usage & PIPE_MAP_READ) && !(usage & PIPE_MAP_DISCARD_WHOLE_RESOURCE) && !bo->shadow_valid) {
      if (ws->dev->vram_get(ws->dev, bo->offset, bo->shadow, bo->base.size))
         return NULL;
      bo->shadow_valid = true;
   }
   if (usage & PIPE_MAP_WRITE)
      bo->dirty = true;
   return bo->shadow;
}

static struct pb_buffer_lean *
rdn_buffer_create(struct radeon_winsys *rws, uint64_t size, unsigned alignment,
                  enum radeon_bo_domain domain, enum radeon_bo_flag flags)
{
   struct radeon_drm_winsys *ws = rdn_winsys(rws);
   struct rdn_bo *bo = CALLOC_STRUCT(rdn_bo);
   enum rdn_place want;
   int r;

   if (!bo)
      return NULL;
   bo->diagnostic_id = p_atomic_inc_return(&rdn_next_diagnostic_id);
   /* The GPU addresses surfaces in units of 256 bytes. */
   alignment = MAX2(alignment, 256);
   alignment = util_next_power_of_two(alignment);
   size = align64(MAX2(size, 1), 256);

   simple_mtx_lock(&ws->lock);
   /*
    * What r600 will never map (tiled textures and render targets, which
    * it fills through a staging copy) goes to the video memory beyond the
    * aperture, if the device has any. What it asks for in the GTT domain
    * alone (buffers the CPU keeps writing: vertices, constants, staging
    * copies) goes to the program's own memory behind the GART, if that is
    * on: ordinary cached memory instead of the aperture. The rest, and
    * whatever does not fit where it should, goes in the aperture. So does
    * what r600 marks as shareable (it leaves out RADEON_FLAG_NO_INTERPROCESS_SHARING):
    * the video decoder's message and bitstream buffers are such, and the
    * decoder reads its messages from the 256 MB of video memory it lives in.
    */
   if (ws->video && !(flags & RADEON_FLAG_NO_INTERPROCESS_SHARING) && ws->dev->uvd_start &&
       rdn_uvd_prepare(ws) && (ws->uvd_seg || (ws->uvd_split && !(domain == RADEON_DOMAIN_GTT))))
      want = RDN_SEGMENT;
   else if ((flags & RADEON_FLAG_NO_CPU_ACCESS) && ws->dev->alloc_hidden)
      want = RDN_HIDDEN;
   else if (domain == RADEON_DOMAIN_GTT && ws->dev->gart_alloc &&
            (flags & RADEON_FLAG_NO_INTERPROCESS_SHARING))
      want = RDN_GART;
   else
      want = RDN_VISIBLE;
   r = -1;
   if (rdn_cache_get(ws, size, alignment, want, &bo->offset)) {
      bo->region = want;
      r = 0;
      ws->mem_stats.cache_hits++;
   }
   ws->mem_stats.creates++;
   if (want == RDN_SEGMENT) {
      /* Nowhere else will do: the decoder reaches only its segment. What the cache holds may make room. */
      if (r) {
         r = ws->dev->uvd_alloc(ws->dev, size, alignment, &bo->offset);
         if (r) {
            rdn_cache_trim(ws, true);
            r = ws->dev->uvd_alloc(ws->dev, size, alignment, &bo->offset);
         }
         if (!r)
            bo->region = RDN_SEGMENT;
      }
      goto placed;
   }
   for (unsigned attempt = 0; r && attempt < 2; ++attempt) {
      if (attempt)
         rdn_cache_trim(ws, true);
      if (want == RDN_HIDDEN) {
         /*
          * Debugging (docs/UVD.md, the decoder's second segment): RDN_HIDDEN_FILL_MB=n puts the next hidden buffer of 256 KB or
          * more n MB further into the hidden memory, by allocating that much first and giving it back after.
          */
         const char *fill = getenv("RDN_HIDDEN_FILL_MB");
         uint64_t filler = 0;
         bool filled = false;

         if (fill && size >= (256 << 10) &&
             !ws->dev->alloc_hidden(ws->dev, (uint64_t)atoi(fill) << 20, 4096, &filler))
            filled = true;
         r = ws->dev->alloc_hidden(ws->dev, size, alignment, &bo->offset);
         if (filled)
            ws->dev->free(ws->dev, filler);
      } else if (want == RDN_GART) {
         r = ws->dev->gart_alloc(ws->dev, size, alignment, &bo->offset);
      } else {
         r = ws->dev->alloc(ws->dev, size, alignment, &bo->offset);
      }
      if (!r)
         bo->region = want;
   }
   if (r && want != RDN_VISIBLE) {
      r = ws->dev->alloc(ws->dev, size, alignment, &bo->offset);
      if (!r)
         bo->region = RDN_VISIBLE;
   }
   if (r && want != RDN_GART && ws->dev->gart_alloc &&
       (flags & RADEON_FLAG_NO_INTERPROCESS_SHARING) && !(flags & RADEON_FLAG_NO_CPU_ACCESS)) {
      /*
       * Video memory is full. Nothing here can move a buffer out of it to
       * make room, but a new one can live in the program's own memory
       * behind the GART: slower for the GPU to read, and the program goes
       * on instead of stopping. Not what r600 marks as shareable (the video
       * decoder's buffers, the display's): the engines that use those
       * cannot reach the GART, so the allocation fails, and whoever asked
       * can do without (a second decoder: the first one has the memory).
       * Nor tiled GPU-only storage, which cannot spill into system RAM on
       * this backend.
       */
      r = ws->dev->gart_alloc(ws->dev, size, alignment, &bo->offset);
      if (!r) {
         bo->region = RDN_GART;
         ws->mem_stats.overflowed++;
      }
   }
placed:
   if (!r) {
      ws->allocated_bytes += size;
      rdn_mem_stats_account(ws, size, bo->region, 1);
   }
   simple_mtx_unlock(&ws->lock);
   if (r) {
      static bool said;

      fprintf(stderr, "rdn: out of video memory (%llu bytes asked, %llu in use)\n",
              (unsigned long long)size, (unsigned long long)ws->allocated_bytes);
      /* Again whenever 128 MB more are in use than the last time. */
      static uint64_t said_at;

      if (rdn_mem_stats_on() && said && ws->allocated_bytes > said_at + (128ull << 20))
         said = false;
      if (rdn_mem_stats_on() && !said) {
         said = true;
         said_at = ws->allocated_bytes;
         rdn_mem_stats_print("when it ran out");
      }
      FREE(bo);
      return NULL;
   }

   pipe_reference_init(&bo->base.reference, 1);
   bo->base.alignment_log2 = util_logbase2(alignment);
   bo->base.usage = 0;
   bo->base.size = size;
   bo->domain = bo->region == RDN_GART ? RADEON_DOMAIN_GTT : RADEON_DOMAIN_VRAM;
   bo->flags = flags;
   return &bo->base;
}

static void rdn_buffer_destroy(struct radeon_winsys *rws, struct pb_buffer_lean *buf)
{
   struct radeon_drm_winsys *ws = rdn_winsys(rws);
   struct rdn_bo *bo = rdn_bo(buf);

   if (bo->external_store) {
      if (bo->last_use.used &&
          !rdn_number_wait(ws, bo->last_use.number, false, OS_TIMEOUT_INFINITE))
         return; /* Keep the allocation pinned if completion is unknown. */
      ws->dev->release_external(ws->dev, bo->external_store);
   }
   if (bo->user) {
      /*
       * The program's memory. The pages must stay behind the GART until
       * the GPU has read what it was told to; the cache keeps them until
       * then (nothing is ever handed out from RDN_USER) and unbinds.
       */
      simple_mtx_lock(&ws->lock);
      rdn_cache_put(ws, bo->offset, bo->base.size, RDN_USER, &bo->last_use);
      rdn_cache_trim(ws, false);
      simple_mtx_unlock(&ws->lock);
   } else if (!bo->foreign) {
      /*
       * The GPU may still be using the memory. Waiting for it here would
       * stop the program at every buffer it drops while drawing, so the
       * memory is kept, with the fence after which it is free. (The cache
       * notes one fence: the video decoder's is waited for.)
       */
      rdn_busy_wait(ws, &bo->last_uvd, OS_TIMEOUT_INFINITE);
      simple_mtx_lock(&ws->lock);
      rdn_cache_put(ws, bo->offset, bo->base.size, bo->region, &bo->last_use);
      ws->allocated_bytes -= bo->base.size;
      rdn_mem_stats_account(ws, bo->base.size, bo->region, -1);
      simple_mtx_unlock(&ws->lock);
   }
   FREE(bo->shadow);
   FREE(bo);
}

/* Where the search for a buffer starts in the table. */
static unsigned rdn_cs_hash(const struct rdn_cs *cs, const struct rdn_bo *bo)
{
   /* The allocator's addresses differ from bit 4 up. */
   return ((uint32_t)((uintptr_t)bo >> 4) * 2654435761u) >> cs->hash_shift;
}

/* The buffer's index in the command stream's list, or -1. */
static int rdn_cs_lookup(struct rdn_cs *cs, struct rdn_bo *bo)
{
   unsigned h = rdn_cs_hash(cs, bo);

   for (;;) {
      int i = cs->hash[h];

      if (i < 0)
         return -1;
      if (cs->buffers[i].bo == bo)
         return i;
      h = (h + 1) & (cs->hash_size - 1);
   }
}

/* Note where buffer `i` of the list is. There is always a free slot. */
static void rdn_cs_hash_insert(struct rdn_cs *cs, unsigned i)
{
   unsigned h = rdn_cs_hash(cs, cs->buffers[i].bo);

   while (cs->hash[h] >= 0)
      h = (h + 1) & (cs->hash_size - 1);
   cs->hash[h] = (int32_t)i;
   cs->buffers[i].slot = h;
}

/* A table of `size` slots (a power of two) with the list's buffers in it. */
static bool rdn_cs_hash_resize(struct rdn_cs *cs, unsigned size)
{
   int32_t *hash = MALLOC(size * sizeof(*hash));
   unsigned i;

   if (!hash)
      return false;
   memset(hash, 0xff, size * sizeof(*hash));
   FREE(cs->hash);
   cs->hash = hash;
   cs->hash_size = size;
   cs->hash_shift = 32 - util_logbase2(size);
   for (i = 0; i < cs->num_buffers; i++)
      rdn_cs_hash_insert(cs, i);
   return true;
}

static bool rdn_cs_is_buffer_referenced(struct radeon_cmdbuf *rcs,
                                        struct pb_buffer_lean *buf, unsigned usage)
{
   struct rdn_cs *cs = (struct rdn_cs *)rcs->priv;
   int i;

   if (!cs)
      return false;
   i = rdn_cs_lookup(cs, rdn_bo(buf));
   if (i < 0)
      return false;
   return (cs->buffers[i].usage & usage) != 0;
}

static bool rdn_buffer_wait(struct radeon_winsys *rws, struct pb_buffer_lean *buf,
                            uint64_t timeout, unsigned usage)
{
   struct rdn_bo *bo = rdn_bo(buf);
   struct radeon_drm_winsys *ws = rdn_winsys(rws);
   struct rdn_busy busy, uvd;

   /*
    * Snapshot the fences under the submission lock; wait without it. A reader only has to wait for writers
    * (the video decoder's work counts as both).
    */
   simple_mtx_lock(&ws->lock);
   busy = (usage & RADEON_USAGE_WRITE) ? bo->last_use : bo->last_write;
   uvd = bo->last_uvd;
   simple_mtx_unlock(&ws->lock);
   return rdn_busy_wait(ws, &busy, timeout) && rdn_busy_wait(ws, &uvd, timeout);
}

static int rdn_cs_flush(struct radeon_cmdbuf *rcs, unsigned flags,
                        struct pipe_fence_handle **pfence);

/* What a stream does before the CPU maps a buffer that it refers to. */
static void rdn_cs_flush_for_map(struct radeon_cmdbuf *rcs, struct rdn_cs *cs, unsigned flags)
{
   if (cs->flush_cs)
      cs->flush_cs(cs->flush_data, flags, NULL);
   else
      rdn_cs_flush(rcs, flags, NULL);
}

static void *rdn_buffer_map(struct radeon_winsys *rws, struct pb_buffer_lean *buf,
                            struct radeon_cmdbuf *rcs, enum pipe_map_flags usage)
{
   struct radeon_drm_winsys *ws = rdn_winsys(rws);
   struct rdn_bo *bo = rdn_bo(buf);
   struct rdn_cs *cs = rcs ? (struct rdn_cs *)rcs->priv : NULL;

   if (bo->region == RDN_HIDDEN)
      return NULL;
   if (!(usage & PIPE_MAP_UNSYNCHRONIZED)) {
      /* Reading needs pending writes done; writing needs every use done. */
      unsigned conflicts = (usage & PIPE_MAP_WRITE) ? RADEON_USAGE_READWRITE
                                                    : RADEON_USAGE_WRITE;
      unsigned wait_usage = (usage & PIPE_MAP_WRITE) ? RADEON_USAGE_READWRITE
                                                     : RADEON_USAGE_READ;

      if (usage & PIPE_MAP_DONTBLOCK) {
         if (cs && rdn_cs_is_buffer_referenced(rcs, buf, conflicts)) {
            rdn_cs_flush_for_map(rcs, cs, RADEON_FLUSH_ASYNC_START_NEXT_GFX_IB_NOW);
            return NULL;
         }
         if (!rdn_buffer_wait(rws, buf, 0, wait_usage))
            return NULL;
      } else {
         if (cs && rdn_cs_is_buffer_referenced(rcs, buf, conflicts))
            rdn_cs_flush_for_map(rcs, cs, RADEON_FLUSH_START_NEXT_GFX_IB_NOW);
         if (!rdn_buffer_wait(rws, buf, OS_TIMEOUT_INFINITE, wait_usage))
            return NULL;
      }
      /* The aperture's cache of host accesses; system memory has none. */
      if (bo->region != RDN_GART && bo->region != RDN_SEGMENT)
         ws->dev->sync_for_cpu(ws->dev);
   }
   if (bo->user)
      return bo->user;
   if (bo->region == RDN_SEGMENT)
      return (bo->flags & RADEON_FLAG_NO_CPU_ACCESS) ? NULL : rdn_segment_shadow(ws, bo, usage);
   if (bo->region == RDN_GART)
      return ws->dev->gart_cpu(ws->dev, bo->offset);
   return (uint8_t *)ws->dev->aperture + bo->offset;
}

static void rdn_buffer_unmap(struct radeon_winsys *rws, struct pb_buffer_lean *buf)
{
}

static void rdn_buffer_get_metadata(struct radeon_winsys *rws, struct pb_buffer_lean *buf,
                                    struct radeon_bo_metadata *md, struct radeon_surf *surf)
{
}

static void rdn_buffer_set_metadata(struct radeon_winsys *rws, struct pb_buffer_lean *buf,
                                    struct radeon_bo_metadata *md,
                                    const struct radeon_surf *surf)
{
}

static struct pb_buffer_lean *
rdn_buffer_from_handle(struct radeon_winsys *rws, struct winsys_handle *whandle,
                       unsigned vm_alignment, bool is_prime_linear_buffer)
{
   struct radeon_drm_winsys *ws = rdn_winsys(rws);
   struct rdn_bo *bo;

   bool screen = whandle->handle == RDN_WINSYS_HANDLE_SCREEN;

   /* The screen, or video memory the caller allocated itself. */
   if (whandle->type != WINSYS_HANDLE_TYPE_KMS ||
       (screen && !ws->dev->screen.width) ||
       (!screen && whandle->handle != RDN_WINSYS_HANDLE_VRAM))
      return NULL;
   bo = CALLOC_STRUCT(rdn_bo);
   if (!bo)
      return NULL;
   bo->diagnostic_id = p_atomic_inc_return(&rdn_next_diagnostic_id);
   pipe_reference_init(&bo->base.reference, 1);
   bo->base.alignment_log2 = 8;
   bo->base.usage = 0;
   if (screen) {
      bo->base.size = (uint64_t)ws->dev->screen.pitch_pixels * 4 *
                      ws->dev->screen.height;
      bo->offset = ws->dev->screen.offset;
   } else {
      /* The surface's place in it is the handle's offset (r600 adds it). */
      bo->base.size = ws->dev->aperture_size;
      bo->offset = 0;
   }
   bo->domain = RADEON_DOMAIN_VRAM;
   bo->flags = 0;
   bo->foreign = true;
   if (!screen && ws->dev->retain_external)
      bo->external_store = ws->dev->retain_external(ws->dev, whandle->offset);
   return &bo->base;
}

static struct pb_buffer_lean *
rdn_buffer_from_ptr(struct radeon_winsys *rws, void *pointer, uint64_t size,
                    enum radeon_bo_flag flags)
{
   struct radeon_drm_winsys *ws = rdn_winsys(rws);
   struct rdn_bo *bo;
   int r;

   if (!ws->dev->gart_bind_user)
      return NULL;
   bo = CALLOC_STRUCT(rdn_bo);
   if (!bo)
      return NULL;
   bo->diagnostic_id = p_atomic_inc_return(&rdn_next_diagnostic_id);
   simple_mtx_lock(&ws->lock);
   r = ws->dev->gart_bind_user(ws->dev, pointer, size, &bo->offset);
   simple_mtx_unlock(&ws->lock);
   if (r) {
      FREE(bo);
      return NULL;
   }
   pipe_reference_init(&bo->base.reference, 1);
   bo->base.alignment_log2 = 12;
   bo->base.usage = 0;
   bo->base.size = size;
   bo->domain = RADEON_DOMAIN_GTT;
   bo->flags = flags;
   bo->region = RDN_GART;
   bo->user = pointer;
   return &bo->base;
}

static bool rdn_buffer_false(struct pb_buffer_lean *buf)
{
   return false;
}

static bool rdn_buffer_is_user_ptr(struct pb_buffer_lean *buf)
{
   return rdn_bo(buf)->user != NULL;
}

static bool rdn_buffer_get_handle(struct radeon_winsys *rws, struct pb_buffer_lean *buf,
                                  struct winsys_handle *whandle)
{
   return false;
}

static bool rdn_buffer_commit(struct radeon_winsys *rws, struct pb_buffer_lean *buf,
                              uint64_t offset, uint64_t size, bool commit)
{
   return false;
}

static uint64_t rdn_buffer_get_virtual_address(struct pb_buffer_lean *buf)
{
   /* Filled in at creation of the winsys: see rdn_winsys_create(). */
   extern uint64_t rdn_vram_gpu_base;
   extern uint64_t rdn_gart_gpu_base;

   if (rdn_bo(buf)->region == RDN_GART)
      return rdn_gart_gpu_base + rdn_bo(buf)->offset;
   return rdn_vram_gpu_base + rdn_bo(buf)->offset;
}

uint64_t rdn_vram_gpu_base;
uint64_t rdn_gart_gpu_base;

static unsigned rdn_buffer_get_reloc_offset(struct pb_buffer_lean *buf)
{
   return 0;
}

static enum radeon_bo_domain rdn_buffer_get_initial_domain(struct pb_buffer_lean *buf)
{
   return rdn_bo(buf)->domain;
}

static enum radeon_bo_flag rdn_buffer_get_flags(struct pb_buffer_lean *buf)
{
   return rdn_bo(buf)->flags;
}

/*
 * Command streams
 */

static struct radeon_winsys_ctx *rdn_ctx_create(struct radeon_winsys *rws, unsigned flags)
{
   /* Nothing per context; r600 only wants something that is not NULL. */
   return (struct radeon_winsys_ctx *)rws;
}

static void rdn_ctx_destroy(struct radeon_winsys_ctx *ctx)
{
}

static enum pipe_reset_status
rdn_ctx_query_reset_status(struct radeon_winsys_ctx *ctx, bool full_reset_only,
                           bool *needs_reset, bool *reset_completed)
{
   if (needs_reset)
      *needs_reset = false;
   if (reset_completed)
      *reset_completed = false;
   return PIPE_NO_RESET;
}

static bool rdn_cs_create(struct radeon_cmdbuf *rcs, struct radeon_winsys_ctx *ctx,
                          enum amd_ip_type ip_type,
                          void (*flush)(void *ctx, unsigned flags,
                                        struct pipe_fence_handle **fence),
                          void *flush_ctx)
{
   struct radeon_drm_winsys *ws = (struct radeon_drm_winsys *)ctx;
   unsigned max_dw = RDN_CS_MAX_DW;
   struct rdn_cs *cs;

   /* The 3D command processor, and the video decoder if there is one. */
   if (ip_type == AMD_IP_UVD) {
      bool ready;

      if (!ws->dev->uvd_submit)
         return false;
      simple_mtx_lock(&ws->lock);
      /* Started by the first stream that wants it. */
      ready = rdn_uvd_prepare(ws);
      simple_mtx_unlock(&ws->lock);
      if (!ready)
         return false;
      max_dw = RDN_UVD_CS_MAX_DW;
   } else if (ip_type != AMD_IP_GFX) {
      return false;
   }
   cs = CALLOC_STRUCT(rdn_cs);
   if (!cs)
      return false;
   memset(rcs, 0, sizeof(*rcs));
   cs->ws = ws;
   cs->uvd = ip_type == AMD_IP_UVD;
   rcs->current.buf = MALLOC(max_dw * 4);
   if (!rcs->current.buf || !rdn_cs_hash_resize(cs, RDN_CS_HASH)) {
      FREE(rcs->current.buf);
      FREE(cs);
      return false;
   }
   rcs->current.max_dw = max_dw;
   cs->flush_cs = flush;
   cs->flush_data = flush_ctx;
   rcs->priv = cs;
   return true;
}

/* Publish buffer ownership before another context can submit. ws->lock held. */
static void rdn_cs_mark_busy(struct rdn_cs *cs, const struct rdn_busy *busy)
{
   unsigned i;

   for (i = 0; i < cs->num_buffers; i++) {
      struct rdn_cs_buffer *b = &cs->buffers[i];

      if (busy->uvd) {
         b->bo->last_uvd = *busy;
      } else {
         b->bo->last_use = *busy;
         if (b->usage & RADEON_USAGE_WRITE)
            b->bo->last_write = *busy;
      }
   }
}

/* Release outside ws->lock: the final reference can enter the buffer cache. */
static void rdn_cs_release_buffers(struct rdn_cs *cs)
{
   unsigned i;

   for (i = 0; i < cs->num_buffers; i++) {
      struct rdn_cs_buffer *b = &cs->buffers[i];

      cs->hash[b->slot] = -1;
      radeon_bo_reference(&cs->ws->base, (struct pb_buffer_lean **)&b->bo, NULL);
   }
   cs->num_buffers = 0;
}

static void rdn_cs_destroy(struct radeon_cmdbuf *rcs)
{
   struct rdn_cs *cs = (struct rdn_cs *)rcs->priv;

   if (!cs)
      return;
   rdn_cs_release_buffers(cs);
   rdn_fence_set(&cs->next_fence, NULL);
   FREE(cs->buffers);
   FREE(cs->hash);
   FREE(rcs->current.buf);
   FREE(cs);
   rcs->priv = NULL;
}

static unsigned rdn_cs_add_buffer(struct radeon_cmdbuf *rcs, struct pb_buffer_lean *buf,
                                  unsigned usage, enum radeon_bo_domain domain)
{
   struct rdn_cs *cs = (struct rdn_cs *)rcs->priv;
   int i = rdn_cs_lookup(cs, rdn_bo(buf));

   if (i >= 0) {
      cs->buffers[i].usage |= usage;
      return i;
   }
   if (cs->num_buffers == cs->max_buffers) {
      cs->max_buffers = cs->max_buffers ? cs->max_buffers * 2 : 256;
      cs->buffers = realloc(cs->buffers, cs->max_buffers * sizeof(*cs->buffers));
   }
   /* Half full with this one: twice the table, or the searches get long. */
   if ((cs->num_buffers + 1) * 2 > cs->hash_size &&
       !rdn_cs_hash_resize(cs, cs->hash_size * 2) &&
       cs->num_buffers + 1 >= cs->hash_size) {
      fprintf(stderr, "rdn: out of memory for a command buffer's list\n");
      abort();
   }
   i = cs->num_buffers++;
   cs->buffers[i].bo = NULL;
   radeon_bo_reference(&cs->ws->base, (struct pb_buffer_lean **)&cs->buffers[i].bo, buf);
   cs->buffers[i].usage = usage;
   rdn_cs_hash_insert(cs, i);
   /*
    * What the radeon DRM winsys does at every first add: r600 asks
    * radeon_cs_memory_below_limit() how much the command stream already
    * references and flushes when it is too much. Without it the stream
    * kept every buffer it had touched until it was full of commands; with
    * glthread's 1 MB upload buffers that was some 700 MB (UT2004,
    * 2026-10-10). The screen's surface is not counted: it is not ours.
    */
   if (!rdn_bo(buf)->foreign) {
      if (rdn_bo(buf)->domain & RADEON_DOMAIN_VRAM)
         rcs->used_vram_kb += buf->size / 1024;
      else
         rcs->used_gart_kb += buf->size / 1024;
   }
   return i;
}

static int rdn_cs_lookup_buffer(struct radeon_cmdbuf *rcs, struct pb_buffer_lean *buf)
{
   return rdn_cs_lookup((struct rdn_cs *)rcs->priv, rdn_bo(buf));
}

static bool rdn_cs_validate(struct radeon_cmdbuf *rcs)
{
   return true;
}

/*
 * RDN_SYNC in the environment, for finding races between the CPU and the
 * GPU: bit 0 waits for every command buffer to finish when it is
 * submitted, bit 1 gives every draw a command buffer of its own.
 */
static unsigned rdn_sync_mode(void)
{
   static int mode = -1;

   if (mode < 0) {
      const char *s = getenv("RDN_SYNC");

      mode = s ? atoi(s) : 0;
   }
   return mode;
}

static bool rdn_cs_check_space(struct radeon_cmdbuf *rcs, unsigned dw)
{
   if (rdn_sync_mode() & 2)
      return false;
   return rcs->current.max_dw - rcs->current.cdw >= dw;
}

static unsigned rdn_cs_get_buffer_list(struct radeon_cmdbuf *rcs,
                                       struct radeon_bo_list_item *list)
{
   struct rdn_cs *cs = (struct rdn_cs *)rcs->priv;
   unsigned i;

   if (list)
      for (i = 0; i < cs->num_buffers; i++) {
         list[i].bo_size = cs->buffers[i].bo->base.size;
         list[i].vm_address = rdn_buffer_get_virtual_address(&cs->buffers[i].bo->base);
         list[i].priority_usage = cs->buffers[i].usage;
      }
   return cs->num_buffers;
}

/* Give back the command buffers the GPU has finished with. */
static void rdn_reap_ibs(struct radeon_drm_winsys *ws, bool wait)
{
   list_for_each_entry_safe(struct rdn_pending_ib, ib, &ws->pending_ibs, list) {
      if (!rdn_busy_wait(ws, &ib->busy, wait ? OS_TIMEOUT_INFINITE : 0))
         break;
      rdn_cache_put(ws, ib->offset, ib->size, RDN_VISIBLE, &ib->busy);
      list_del(&ib->list);
      FREE(ib);
   }
}

/*
 * A program that never waits for the GPU (it only flushes when it swaps)
 * must not get further ahead of it than this many command buffers.
 */
#define RDN_MAX_PENDING_IBS 4

static void rdn_throttle(struct radeon_drm_winsys *ws)
{
   for (;;) {
      struct rdn_pending_ib *oldest = NULL;
      unsigned pending = 0;

      list_for_each_entry(struct rdn_pending_ib, ib, &ws->pending_ibs, list) {
         if (!oldest)
            oldest = ib;
         pending++;
      }
      if (pending < RDN_MAX_PENDING_IBS)
         return;
      /* A wait that timed out must not keep us here. */
      if (!rdn_busy_wait(ws, &oldest->busy, OS_TIMEOUT_INFINITE))
         return;
      rdn_reap_ibs(ws, false);
   }
}

/*
 * With the lock held: video memory for a command buffer of `size` bytes,
 * once the earlier ones have made room.
 */
static int rdn_ib_alloc(struct radeon_drm_winsys *ws, uint64_t size, uint64_t *offset)
{
   int r;

   rdn_reap_ibs(ws, false);
   rdn_cache_trim(ws, false);
   rdn_throttle(ws);
   r = rdn_cache_get(ws, size, 4096, RDN_VISIBLE, offset) ? 0 :
       ws->dev->alloc(ws->dev, size, 4096, offset);
   if (r) {
      /* Wait for the earlier ones to finish and try once more. */
      rdn_reap_ibs(ws, true);
      rdn_cache_trim(ws, true);
      r = ws->dev->alloc(ws->dev, size, 4096, offset);
   }
   return r;
}

/* RDN_STATS: what went to the GPU. */
static struct {
   uint64_t flushes, bytes;
   unsigned most_buffers;
} rdn_cs_stats;

/*
 * Video decoder streams
 */

/*
 * The registers r600's radeon_uvd.c writes a command's buffer address and
 * kind to (RUVD_GPCOM_VCPU_DATA0, _DATA1).
 */
#define RDN_UVD_GPCOM_VCPU_DATA0 0xEF10
#define RDN_UVD_GPCOM_VCPU_DATA1 0xEF14
#define RDN_UVD_GPCOM_VCPU_CMD 0xEF0C

/*
 * Looking at what the decoder's firmware makes of the commands: with RDN_UVD_DEBUG, or the file /tmp/rdnavc.debug, each
 * stream is waited for and the start of its message and of the firmware's feedback is logged (syslog).
 */
struct rdn_uvd_dbg {
   struct rdn_bo *bo[8];
   unsigned offset[8], cmd[8], n;
};

static bool rdn_uvd_debug(void)
{
   static int on = -1;

   if (on < 0)
      on = getenv("RDN_UVD_DEBUG") != NULL || access("/tmp/rdnavc.debug", F_OK) == 0;
   return on;
}

/*
 * Replace the buffer references of a stream by addresses, as the radeon
 * kernel driver does (radeon_uvd_cs_reloc): DATA0 holds the offset in the
 * buffer and DATA1 four times its index in the stream's list; they become
 * the low and high words of the 40-bit address. False if the stream is not
 * made of register writes or names a buffer the decoder cannot reach.
 */
static bool rdn_uvd_relocate(struct rdn_cs *cs, struct radeon_cmdbuf *rcs, struct rdn_uvd_dbg *dbg)
{
   uint32_t *w = rcs->current.buf;
   unsigned i = 0, data0 = ~0u;
   struct rdn_bo *last_bo = NULL;
   unsigned last_off = 0;

   while (i < rcs->current.cdw) {
      unsigned type = w[i] >> 30;
      unsigned count, reg, j;

      if (type == 2) {
         i++;
         continue;
      }
      if (type != 0)
         return false;
      count = ((w[i] >> 16) & 0x3fff) + 1;
      reg = (w[i] & 0xffff) << 2;
      if (i + 1 + count > rcs->current.cdw)
         return false;
      for (j = 0; j < count; j++, reg += 4) {
         unsigned at = i + 1 + j;

         if (reg == RDN_UVD_GPCOM_VCPU_DATA0) {
            data0 = at;
         } else if (reg == RDN_UVD_GPCOM_VCPU_DATA1) {
            unsigned index = w[at] / 4;
            uint64_t address;
            struct rdn_bo *bo;

            if (data0 == ~0u || index >= cs->num_buffers)
               return false;
            bo = cs->buffers[index].bo;
            /* Only video memory: the program's own memory is out of the decoder's reach. */
            if (bo->region == RDN_GART || bo->region == RDN_USER || w[data0] >= bo->base.size ||
                (cs->ws->uvd_seg && bo->region != RDN_SEGMENT))
               return false;
            address = rdn_buffer_get_virtual_address(&bo->base) + w[data0];
            last_bo = bo;
            last_off = w[data0];
            w[data0] = (uint32_t)address;
            w[at] = (uint32_t)(address >> 32);
            data0 = ~0u;
         } else if (reg == RDN_UVD_GPCOM_VCPU_CMD && dbg && last_bo && dbg->n < 8) {
            dbg->bo[dbg->n] = last_bo;
            dbg->offset[dbg->n] = last_off;
            dbg->cmd[dbg->n++] = w[at] >> 1;
            last_bo = NULL;
         }
      }
      i += 1 + count;
   }
   return true;
}

static uint32_t rdn_le32_at(const uint8_t *p)
{
   return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

/*
 * Split mode: what the program wrote into the copies of the segment's buffers that the stream names goes to video memory (all of each; a
 * stream names few of them and the CPU maps only what it fills). With the lock not held.
 */
static int rdn_uvd_upload_dirty(struct radeon_drm_winsys *ws, struct rdn_cs *cs)
{
   unsigned i;
   int r = 0;

   for (i = 0; i < cs->num_buffers && !r; i++) {
      struct rdn_bo *bo = cs->buffers[i].bo;

      if (bo->region != RDN_SEGMENT || !bo->dirty || !bo->shadow)
         continue;
      r = ws->dev->vram_put(ws->dev, bo->offset, bo->shadow, bo->base.size);
      if (!r) {
         bo->dirty = false;
         bo->shadow_valid = false;
      }
   }
   return r;
}

/*
 * Where in the decode message (little-endian words) the bitstream's length is, ruvd_msg.body.decode.bsd_size (radeon_uvd.h):
 * 4 words of header, stream_type, decode_flags, the two sizes, the reference buffer (4 words), the display buffer's
 * description (8 words), use_addr_macro, the bitstream's address and then its size.
 */
#define RDN_UVD_MSG_BSD_SIZE 88

/*
 * A stream for a decoder in a segment: what the program wrote into the copies of the buffers it names goes to video memory
 * (the message whole, the bitstream as far as the message says it goes), the feedback's counter is cleared there, and the
 * stream's words are handed over by value. With the lock held.
 */
static int rdn_uvd_segment_submit(struct radeon_drm_winsys *ws, struct rdn_cs *cs, struct radeon_cmdbuf *rcs,
                                  struct rdn_uvd_dbg *dbg, struct rdn_fence *fence)
{
   static const uint8_t zero[4];
   struct rdn_bo *msg = NULL, *fb = NULL;
   unsigned msg_off = 0, fb_off = 0, k, i;
   uint64_t bs_bytes = 0;
   int r;

   for (k = 0; k < dbg->n; k++) {
      if (dbg->cmd[k] == 0) {
         msg = dbg->bo[k];
         msg_off = dbg->offset[k];
      } else if (dbg->cmd[k] == 3) {
         fb = dbg->bo[k];
         fb_off = dbg->offset[k];
      }
   }
   if (msg && msg->shadow && msg_off + RDN_UVD_MSG_BSD_SIZE + 4 <= msg->base.size &&
       rdn_le32_at(msg->shadow + msg_off + 4) == 1)
      bs_bytes = align64(rdn_le32_at(msg->shadow + msg_off + RDN_UVD_MSG_BSD_SIZE), 128);
   for (i = 0; i < cs->num_buffers; i++) {
      struct rdn_bo *bo = cs->buffers[i].bo;
      uint64_t bytes = bo->base.size;

      if (bo->region != RDN_SEGMENT || !bo->dirty || !bo->shadow)
         continue;
      for (k = 0; k < dbg->n; k++)
         if (dbg->cmd[k] == 0x100 && dbg->bo[k] == bo && bs_bytes && bs_bytes < bytes)
            bytes = bs_bytes;
      r = ws->dev->vram_put(ws->dev, bo->offset, bo->shadow, bytes);
      if (r)
         return r;
      if (rdn_uvd_debug()) {
         /* did the bytes arrive? */
         uint8_t back[32];
         unsigned n = bytes < sizeof(back) ? (unsigned)bytes : (unsigned)sizeof(back);

         if (ws->dev->vram_get(ws->dev, bo->offset, back, n))
            syslog(LOG_NOTICE, "RadeonNIAVC: uvd segment: read back of 0x%llx failed", (unsigned long long)bo->offset);
         else
            syslog(LOG_NOTICE, "RadeonNIAVC: uvd segment: put %llu bytes at 0x%llx (stream %s), %s",
                   (unsigned long long)bytes, (unsigned long long)bo->offset,
                   bo == msg ? "message" : "buffer", memcmp(back, bo->shadow, n) ? "READ BACK DIFFERENT" : "read back equal");
      }
      bo->dirty = false;
      bo->shadow_valid = false;
   }
   if (fb) {
      r = ws->dev->vram_put(ws->dev, fb->offset + fb_off + 4, zero, 4);
      if (r)
         return r;
   }
   if (rdn_uvd_debug()) {
      char line[300];
      int len = 0;

      for (k = 0; k < dbg->n && len < (int)sizeof(line) - 40; k++)
         len += snprintf(line + len, sizeof(line) - len, " %x@%llx+%x/%llx", dbg->cmd[k],
                         (unsigned long long)dbg->bo[k]->offset, dbg->offset[k], (unsigned long long)dbg->bo[k]->base.size);
      syslog(LOG_NOTICE, "RadeonNIAVC: uvd segment stream, %u words:%s; first words %08x %08x %08x %08x", rcs->current.cdw, line,
             rcs->current.cdw > 0 ? rcs->current.buf[0] : 0, rcs->current.cdw > 1 ? rcs->current.buf[1] : 0,
             rcs->current.cdw > 2 ? rcs->current.buf[2] : 0, rcs->current.cdw > 3 ? rcs->current.buf[3] : 0);
   }
   return ws->dev->uvd_submit_words(ws->dev, rcs->current.buf, rcs->current.cdw, &fence->number);
}

/*
 * Give the kext all the memory the cache keeps. A closed video decoder's buffers (some 65 MB at 1080p) would otherwise sit here until
 * the program exits, out of reach of every other program and of the decoder room check.
 */
void rdn_winsys_trim(struct radeon_winsys *rws)
{
   struct radeon_drm_winsys *ws = rdn_winsys(rws);

   simple_mtx_lock(&ws->lock);
   rdn_cache_trim(ws, true);
   simple_mtx_unlock(&ws->lock);
}

void rdn_winsys_set_video(struct radeon_winsys *rws)
{
   rdn_winsys(rws)->video = true;
}

int rdn_winsys_uvd_answered(struct radeon_winsys *rws)
{
   struct radeon_drm_winsys *ws = rdn_winsys(rws);
   const uint8_t *msg, *fb;

   /* asked again while the answer is late: the flag stays until the next stream is submitted */
   if (!ws->uvd_check)
      return 1;
   if (ws->uvd_seg) {
      uint8_t counter[4];

      /* only a decode message (type 1) is answered; the word is read from video memory the CPU cannot map */
      if (ws->uvd_msg_type != 1 || ws->dev->vram_get(ws->dev, ws->uvd_fb_at + 4, counter, 4))
         return 1;
      if (rdn_uvd_debug()) {
         static unsigned said;

         if (said++ < 40)
            syslog(LOG_NOTICE, "RadeonNIAVC: uvd segment: feedback counter %u, expected %u (at 0x%llx)",
                   (unsigned)rdn_le32_at(counter), (unsigned)ws->uvd_msg_num, (unsigned long long)(ws->uvd_fb_at + 4));
      }
      return rdn_le32_at(counter) == ws->uvd_msg_num;
   }
   ws->dev->sync_for_cpu(ws->dev);
   msg = (const uint8_t *)ws->dev->aperture + ws->uvd_msg_at;
   fb = (const uint8_t *)ws->dev->aperture + ws->uvd_fb_at;
   /* only a decode message (type 1) is answered */
   if (rdn_le32_at(msg + 4) != 1)
      return 1;
   return rdn_le32_at(fb + 4) == rdn_le32_at(msg + 12);
}

static int rdn_uvd_cs_flush(struct radeon_cmdbuf *rcs, unsigned flags,
                            struct pipe_fence_handle **pfence)
{
   struct rdn_cs *cs = (struct rdn_cs *)rcs->priv;
   struct radeon_drm_winsys *ws = cs->ws;
   struct rdn_pending_ib *ib;
   struct rdn_fence *fence;
   struct rdn_busy busy = { 0, false, true };
   struct rdn_uvd_dbg dbg;
   uint64_t offset, ib_size;
   int r = 0;

   memset(&dbg, 0, sizeof(dbg));
   /* Pad to 16 words, as radeon_drm_cs.c does for the decoder. */
   while (rcs->current.cdw & 15)
      rcs->current.buf[rcs->current.cdw++] = 0x80000000; /* type2 nop packet */

   fence = rdn_fence_new();
   ib = CALLOC_STRUCT(rdn_pending_ib);
   if (!fence || !ib) {
      FREE(ib);
      FREE(fence);
      return -ENOMEM;
   }
   fence->uvd = true;

   if (!rcs->current.cdw) {
      /* Nothing to run. */
      FREE(ib);
      fence->reached = true;
   } else if (!rdn_uvd_relocate(cs, rcs, &dbg)) {
      fprintf(stderr, "rdn: video decoder commands refused: they name memory it cannot reach\n");
      FREE(ib);
      fence->reached = true;
      r = -EINVAL;
   } else {
      /* What the 3D engine did to these buffers (or still reads of them) first. */
      for (unsigned i = 0; i < cs->num_buffers; i++) {
         struct rdn_bo *bo = cs->buffers[i].bo;

         rdn_busy_wait(ws, (cs->buffers[i].usage & RADEON_USAGE_WRITE) ? &bo->last_use :
                                                                         &bo->last_write,
                       OS_TIMEOUT_INFINITE);
      }
      ib_size = align64(rcs->current.cdw * 4, 4096);
      simple_mtx_lock(&ws->lock);
      /* Split mode: the segment's buffers the program filled go there first. */
      r = ws->uvd_split ? rdn_uvd_upload_dirty(ws, cs) : 0;
      /* A decoder in a segment takes the stream by value (the kext keeps it in video memory itself). */
      if (!r)
         r = ws->uvd_seg ? 0 : rdn_ib_alloc(ws, ib_size, &offset);
      if (!r && ws->uvd_seg) {
         r = rdn_uvd_segment_submit(ws, cs, rcs, &dbg, fence);
      } else if (!r) {
         {
            /*
             * The feedback's counter is cleared first: what the buffer held from an earlier decode (or an earlier program,
             * the same addresses come again) must not pass for the firmware's answer to this one.
             */
            unsigned k3;

            for (k3 = 0; k3 < dbg.n; k3++)
               if (dbg.cmd[k3] == 3 && dbg.bo[k3]->region != RDN_GART && dbg.bo[k3]->region != RDN_USER &&
                   dbg.bo[k3]->region != RDN_SEGMENT)
                  memset((uint8_t *)ws->dev->aperture + dbg.bo[k3]->offset + dbg.offset[k3] + 4, 0, 4);
         }
         memcpy((uint8_t *)ws->dev->aperture + offset, rcs->current.buf,
                rcs->current.cdw * 4);
         /*
          * The video memory window is write-combined (uncached, not guarded): the kext's eieio does not order its
          * stores against the doorbell, only sync does. The decoder reads its commands, the message and the
          * bitstream as soon as the doorbell rings; a stale read of any of them can wedge it.
          */
         __sync_synchronize();
         r = ws->dev->uvd_submit(ws->dev, offset, rcs->current.cdw, &fence->number);
         if (r)
            ws->dev->free(ws->dev, offset);
      }
      if (r) {
         simple_mtx_unlock(&ws->lock);
         fprintf(stderr, "rdn: video decoder command submission failed (%d)\n", r);
         FREE(ib);
         fence->reached = true;
      } else {
         fence->submitted = true;
         ws->uvd_used = true;
         {
            /* where the firmware will find the message and write its feedback, for rdn_winsys_uvd_answered() */
            struct rdn_bo *msg = NULL, *fb = NULL;
            unsigned msg_off = 0, fb_off = 0, k2;

            for (k2 = 0; k2 < dbg.n; k2++) {
               if (dbg.bo[k2]->region == RDN_GART || dbg.bo[k2]->region == RDN_USER ||
                   (dbg.bo[k2]->region == RDN_SEGMENT && !ws->uvd_seg))
                  continue;
               if (dbg.cmd[k2] == 0) {
                  msg = dbg.bo[k2];
                  msg_off = dbg.offset[k2];
               } else if (dbg.cmd[k2] == 3) {
                  fb = dbg.bo[k2];
                  fb_off = dbg.offset[k2];
               }
            }
            ws->uvd_check = msg && fb;
            if (ws->uvd_check) {
               ws->uvd_msg_at = msg->offset + msg_off;
               ws->uvd_fb_at = fb->offset + fb_off;
               if (ws->uvd_seg && msg->shadow) {
                  /* the message is not read back through the aperture: its copy has what the feedback must echo */
                  ws->uvd_msg_type = rdn_le32_at(msg->shadow + msg_off + 4);
                  ws->uvd_msg_num = rdn_le32_at(msg->shadow + msg_off + 12);
               } else {
                  ws->uvd_check = !ws->uvd_seg;
               }
            }
         }
         busy.number = fence->number;
         busy.used = true;
         rdn_cs_mark_busy(cs, &busy);
         if (ws->uvd_seg) {
            /* no command buffer of ours in video memory to give back later */
            FREE(ib);
         } else {
            ib->offset = offset;
            ib->size = ib_size;
            ib->busy = busy;
            list_addtail(&ib->list, &ws->pending_ibs);
         }
         simple_mtx_unlock(&ws->lock);
         if ((rdn_sync_mode() & 1) || rdn_uvd_debug())
            rdn_fence_wait(ws, fence, OS_TIMEOUT_INFINITE);
         if (rdn_uvd_debug() && !ws->uvd_seg) {
            unsigned k;

            ws->dev->sync_for_cpu(ws->dev);
            {
               /* every buffer the stream names: which command, where in video memory, how big, and the raw message */
               char line[400];
               int len = 0;

               for (k = 0; k < dbg.n && len < (int)sizeof(line) - 60; k++)
                  len += snprintf(line + len, sizeof(line) - len, " %x:%s@%llx+%x/%llx", dbg.cmd[k],
                                  dbg.bo[k]->region == RDN_GART ? "gart" : dbg.bo[k]->region == RDN_USER ? "user" : "vram",
                                  (unsigned long long)dbg.bo[k]->offset, dbg.offset[k],
                                  (unsigned long long)dbg.bo[k]->base.size);
               syslog(LOG_NOTICE, "RadeonNIAVC: uvd stream%s", line);
               for (k = 0; k < dbg.n; k++) {
                  if (dbg.cmd[k] == 0 && dbg.bo[k]->region != RDN_GART && dbg.bo[k]->region != RDN_USER) {
                     FILE *f = fopen("/tmp/rdn-uvd-msgs.bin", "ab");

                     if (f) {
                        fwrite((const uint8_t *)ws->dev->aperture + dbg.bo[k]->offset + dbg.offset[k], 1, 0xde4, f);
                        fclose(f);
                     }
                  }
               }
            }
            for (k = 0; k < dbg.n; k++) {
               struct rdn_bo *bo = dbg.bo[k];
               const uint8_t *p;

               if (dbg.cmd[k] == 0x100 && bo->region != RDN_GART && bo->region != RDN_USER) {
                  /* the slice data the decoder reads: its first bytes, to compare with another run's */
                  p = (const uint8_t *)ws->dev->aperture + bo->offset + dbg.offset[k];
                  syslog(LOG_NOTICE, "RadeonNIAVC: uvd bitstream: %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x",
                         p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11], p[12], p[13], p[14],
                         p[15], p[16], p[17], p[18], p[19], p[20], p[21], p[22], p[23]);
                  continue;
               }
               if (bo->region == RDN_GART || bo->region == RDN_USER || dbg.cmd[k] > 3)
                  continue;
               if (dbg.cmd[k] == 1 || dbg.cmd[k] == 2)
                  continue;
               p = (const uint8_t *)ws->dev->aperture + bo->offset + dbg.offset[k];
               syslog(LOG_NOTICE, "RadeonNIAVC: uvd %s at 0x%llx: %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x",
                      dbg.cmd[k] == 0 ? "message" : "feedback",
                      (unsigned long long)(bo->offset + dbg.offset[k]), p[0], p[1], p[2], p[3], p[4], p[5], p[6],
                      p[7], p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15], p[16], p[17], p[18], p[19], p[20],
                      p[21], p[22], p[23]);
            }
         }
      }
   }

   rdn_cs_release_buffers(cs);
   rcs->current.cdw = 0;
   rcs->used_vram_kb = 0;
   rcs->used_gart_kb = 0;

   if (pfence)
      rdn_fence_set((struct rdn_fence **)pfence, fence);
   rdn_fence_set(&fence, NULL);
   return r;
}

/* Optional bounded captures. Words use host byte order, including the header. */
static FILE *rdn_capture_submission(struct radeon_cmdbuf *rcs, uint64_t offset,
                                   const uint32_t *words, unsigned count,
                                   uint64_t marker_offset)
{
   const char *dir = getenv("RDN_CAPTURE_DIR");
   struct rdn_cs *cs = rcs->priv;
   struct radeon_drm_winsys *ws = cs->ws;
   uint32_t header[16] = { 0x52444e31, 1, 16, 12 };
   char path[1024];
   FILE *file;
   int length;

   if (!dir || !*dir)
      return NULL;
   length = snprintf(path, sizeof(path), "%s/rdn-%u-%p-%02u.bin", dir,
                     (unsigned)getpid(), (void *)ws, (unsigned)(ws->num_flushes % 16));
   if (length < 0 || (size_t)length >= sizeof(path))
      return NULL;
   file = fopen(path, "rb");
   if (file) {
      uint32_t old[16];
      bool pending = fread(old, sizeof(old), 1, file) == 1 &&
         old[0] == header[0] && old[4] == (uint32_t)getpid() &&
         old[5] < ws->num_flushes && old[13] && !old[11] &&
         !ws->dev->fence_done(ws->dev, old[12]);

      fclose(file);
      if (pending)
         return NULL;
   }
   file = fopen(path, "wb");
   if (!file)
      return NULL;
   header[4] = getpid();
   header[5] = (uint32_t)ws->num_flushes;
   header[6] = (uint32_t)(uintptr_t)cs;
   header[7] = cs->num_buffers;
   header[8] = count;
   header[9] = offset >> 32;
   header[10] = offset;
   header[14] = marker_offset >> 32;
   header[15] = marker_offset;
   /* Words 11/12 are filled after submission; 13 marks that update. */
   if (fwrite(header, sizeof(header), 1, file) != 1)
      goto fail;
   for (unsigned i = 0; i < cs->num_buffers; ++i) {
      const struct rdn_bo *bo = cs->buffers[i].bo;
      uint64_t address = rdn_buffer_get_virtual_address((struct pb_buffer_lean *)&bo->base);
      uint32_t row[12] = { bo->diagnostic_id, bo->region, bo->foreign,
         cs->buffers[i].usage, address >> 32, address,
         (uint64_t)bo->base.size >> 32, bo->base.size, bo->last_use.number,
         bo->last_write.number, bo->last_use.used, bo->last_write.used };

      if (fwrite(row, sizeof(row), 1, file) != 1)
         goto fail;
   }
   if (fwrite(words, 4, count, file) != count ||
       fflush(file))
      goto fail;
   return file;
fail:
   fclose(file);
   return NULL;
}

static void rdn_capture_result(FILE *file, int result, uint32_t fence)
{
   uint32_t words[3] = { (uint32_t)result, fence, 1 };

   if (!file)
      return;
   if (!fseek(file, 11 * 4, SEEK_SET))
      (void)fwrite(words, sizeof(words), 1, file);
   fclose(file);
}

static unsigned rdn_complete_packet_words(const uint32_t *words, unsigned count)
{
   unsigned pos = 0;

   while (pos < count) {
      unsigned type = words[pos] >> 30;
      unsigned length = type == 2 ? 1 : type == 1 ? 2 :
                        ((words[pos] >> 16) & 0x3fff) + 2;

      if (length > count - pos)
         break;
      pos += length;
   }
   return pos;
}

static int rdn_cs_flush(struct radeon_cmdbuf *rcs, unsigned flags,
                        struct pipe_fence_handle **pfence)
{
   struct rdn_cs *cs = (struct rdn_cs *)rcs->priv;
   struct radeon_drm_winsys *ws = cs->ws;
   struct rdn_pending_ib *ib;
   struct rdn_fence *fence;
   struct rdn_busy busy = { 0, false, false };
   uint64_t offset, ib_size;
   uint32_t *traced = NULL;
   unsigned submit_words;
   const char *trace_mode = getenv("RDN_CAPTURE_MARKERS");
   bool trace_complete = trace_mode && !strcmp(trace_mode, "complete");
   int r;

   if (cs->uvd)
      return rdn_uvd_cs_flush(rcs, flags, pfence);

   /* Pad to 8 words, as the command processor fetches (radeon_drm_cs.c). */
   while (rcs->current.cdw & 7)
      rcs->current.buf[rcs->current.cdw++] = 0x80000000; /* type2 nop packet */

   unsigned complete = rdn_complete_packet_words(rcs->current.buf, rcs->current.cdw);
   if (complete != rcs->current.cdw) {
      fprintf(stderr, "rdn: incomplete command packet at %u of %u words\n",
              complete, rcs->current.cdw);
      for (unsigned i = complete; i < rcs->current.cdw; ++i)
         rcs->current.buf[i] = 0x80000000;
   }

   fence = cs->next_fence;
   cs->next_fence = NULL;
   if (!fence)
      fence = rdn_fence_new();
   ib = CALLOC_STRUCT(rdn_pending_ib);
   if (!fence || !ib) {
      FREE(ib);
      return -ENOMEM;
   }

   /* In steps of 64 KB, so that the cache has one of the size again. */
   submit_words = rcs->current.cdw;
   if (getenv("RDN_CAPTURE_DIR") && trace_mode) {
      size_t count = rdn_trace_words(rcs->current.buf, rcs->current.cdw, trace_complete);

      if (count) {
         traced = MALLOC(count * 4);
         if (traced)
            submit_words = count;
      }
   }
   ib_size = align64(submit_words * 4 + (traced ? 256 : 0), 64 * 1024);

   /* What the video decoder is doing to these buffers comes first. */
   if (ws->uvd_used)
      for (unsigned i = 0; i < cs->num_buffers; i++)
         rdn_busy_wait(ws, &cs->buffers[i].bo->last_uvd, OS_TIMEOUT_INFINITE);

   simple_mtx_lock(&ws->lock);
   r = rdn_ib_alloc(ws, ib_size, &offset);
   if (!r) {
      const uint32_t *words = rcs->current.buf;
      uint64_t marker_offset = 0;

      if (traced) {
         marker_offset = offset + ib_size - 256;
         if (rdn_trace_encode(rcs->current.buf, rcs->current.cdw, traced,
                              submit_words, ws->dev->info.vram_gpu_base + marker_offset,
                              trace_complete)) {
            *(uint32_t *)((uint8_t *)ws->dev->aperture + marker_offset) = 0;
            words = traced;
         } else {
            marker_offset = 0;
            submit_words = rcs->current.cdw;
         }
      }
      /*
       * The words are in the CPU's byte order, which the device expects.
       * Having r600 write them here in the first place was tried and is
       * no faster: the aperture takes 720 MB a second whether the words
       * come one by one or in a copy (journal, 2026-10-06).
       */
      memcpy((uint8_t *)ws->dev->aperture + offset, words, submit_words * 4);
      FILE *capture = rdn_capture_submission(rcs, offset, words, submit_words, marker_offset);
      r = ws->dev->submit(ws->dev, offset, submit_words, &fence->number);
      rdn_capture_result(capture, r, r ? 0 : fence->number);
      if (r)
         ws->dev->free(ws->dev, offset);
   }
   FREE(traced);
   if (r) {
      simple_mtx_unlock(&ws->lock);
      fprintf(stderr, "rdn: command submission failed (%d)\n", r);
      FREE(ib);
      /* Nothing ran: do not leave anyone waiting on this fence. */
      fence->reached = true;
   } else {
      fence->submitted = true;
      busy.number = fence->number;
      busy.used = true;
      rdn_cs_mark_busy(cs, &busy);
      ib->offset = offset;
      ib->size = ib_size;
      ib->busy = busy;
      list_addtail(&ib->list, &ws->pending_ibs);
      ws->num_flushes++;
      rdn_cs_stats.flushes++;
      rdn_cs_stats.bytes += rcs->current.cdw * 4;
      rdn_cs_stats.most_buffers = MAX2(rdn_cs_stats.most_buffers, cs->num_buffers);
      simple_mtx_unlock(&ws->lock);
      if (rdn_sync_mode() & 1)
         rdn_fence_wait(ws, fence, OS_TIMEOUT_INFINITE);
   }

   rdn_cs_release_buffers(cs);
   rcs->current.cdw = 0;
   rcs->used_vram_kb = 0;
   rcs->used_gart_kb = 0;

   if (pfence)
      rdn_fence_set((struct rdn_fence **)pfence, fence);
   rdn_fence_set(&fence, NULL);
   return r;
}

static void rdn_cs_stats_print(void)
{
   fprintf(stderr, "rdn commands: %llu command buffers, %llu KB in all, "
           "at most %u buffers in one\n",
           (unsigned long long)rdn_cs_stats.flushes,
           (unsigned long long)(rdn_cs_stats.bytes >> 10),
           rdn_cs_stats.most_buffers);
}

static struct pipe_fence_handle *rdn_cs_get_next_fence(struct radeon_cmdbuf *rcs)
{
   struct rdn_cs *cs = (struct rdn_cs *)rcs->priv;
   struct rdn_fence *fence = NULL;

   if (!cs->next_fence)
      cs->next_fence = rdn_fence_new();
   rdn_fence_set(&fence, cs->next_fence);
   return (struct pipe_fence_handle *)fence;
}

static bool rdn_cs_request_feature(struct radeon_cmdbuf *rcs, enum radeon_feature_id fid,
                                   bool enable)
{
   return false;
}

static void rdn_cs_sync_flush(struct radeon_cmdbuf *rcs)
{
   /* Flushes are synchronous. */
}

/*
 * The winsys
 */

static void rdn_query_info(struct radeon_winsys *rws, struct radeon_info *info)
{
   *info = rdn_winsys(rws)->info;
}

static uint64_t rdn_query_value(struct radeon_winsys *rws, enum radeon_value_id value)
{
   struct radeon_drm_winsys *ws = rdn_winsys(rws);

   switch (value) {
   case RADEON_VRAM_USAGE:
   case RADEON_REQUESTED_VRAM_MEMORY:
   case RADEON_MAPPED_VRAM:
      return ws->allocated_bytes;
   case RADEON_NUM_GFX_IBS:
      return ws->num_flushes;
   case RADEON_TIMESTAMP:
      return os_time_get_nano();
   default:
      return 0;
   }
}

static bool rdn_read_registers(struct radeon_winsys *rws, unsigned reg_offset,
                               unsigned num_registers, uint32_t *out)
{
   return false;
}

static int rdn_get_fd(struct radeon_winsys *rws)
{
   return -1;
}

static bool rdn_winsys_unref(struct radeon_winsys *rws)
{
   return pipe_reference(&rdn_winsys(rws)->reference, NULL);
}

static void rdn_winsys_destroy(struct radeon_winsys *rws)
{
   struct radeon_drm_winsys *ws = rdn_winsys(rws);

   rdn_reap_ibs(ws, true);
   rdn_cache_trim(ws, true);
   if (ws->surf_man)
      radeon_surface_manager_free(ws->surf_man);
   ws->dev->destroy(ws->dev);
   simple_mtx_destroy(&ws->lock);
   FREE(ws);
}

/* What do_winsys_init() in radeon_drm_winsys.c derives for an Evergreen card. */
static void rdn_init_info(struct radeon_drm_winsys *ws)
{
   const struct rdn_device_info *dev = &ws->dev->info;
   struct radeon_info *info = &ws->info;
   uint32_t vram_kb = (uint32_t)(dev->vram_size / 1024);
   uint32_t hidden_kb = (uint32_t)(dev->hidden_size / 1024);

   /* The last radeon DRM: every feature r600 keys on a version is there. */
   info->drm_major = 2;
   info->drm_minor = 51;
   info->drm_patchlevel = 0;
   info->is_amdgpu = false;

   info->pci_id = dev->pci_device_id;
   info->family = CHIP_TURKS;
   info->gfx_level = EVERGREEN;
   ws->gen = DRV_R600;
   info->has_dedicated_vram = true;

   info->ip[AMD_IP_GFX].num_queues = 1;
   info->ip[AMD_IP_GFX].ver_major = 5;
   info->ip[AMD_IP_GFX].ib_alignment = 4096;
   info->ip[AMD_IP_SDMA].num_queues = 0;
   /* r600 offers the decoder when this is not 0. */
   info->ip[AMD_IP_UVD].num_queues = ws->dev->uvd_submit ? 1 : 0;

   info->has_userptr = ws->dev->gart_bind_user != NULL;
   /*
    * There is no GART, but r600 measures what one command buffer may
    * reference and what texture transfers may allocate against this size.
    * With zero it flushes on every draw (radeon_cs_memory_below_limit).
    * Buffers it asks for in the GTT domain come from video memory too.
    */
   info->gart_size_kb = vram_kb;
   info->vram_size_kb = vram_kb + hidden_kb;
   info->vram_vis_size_kb = vram_kb;
   info->max_heap_size_kb = vram_kb;
   info->gart_page_size = 4096;
   info->min_alloc_size = 4096;
   info->pte_fragment_size = 64 * 1024;
   info->max_alignment = 1024 * 1024;

   info->max_render_backends = dev->max_backends;
   info->clock_crystal_freq = 27000;
   info->r600_num_banks = 4 << ((dev->tile_config & 0xf0) >> 4);
   info->r600_pipe_interleave_bytes = 256 << ((dev->tile_config & 0xf00) >> 8);
   if (!info->r600_pipe_interleave_bytes)
      info->r600_pipe_interleave_bytes = 512;
   info->num_tile_pipes = dev->max_tile_pipes;
   info->r600_gb_backend_map = dev->backend_map;
   info->r600_gb_backend_map_valid = true;
   info->enabled_rb_mask = BITFIELD_MASK(info->max_render_backends);
   info->num_rb = util_bitcount64(info->enabled_rb_mask);

   /* Every buffer has a fixed GPU address: see the top of this file. */
   info->r600_has_virtual_memory = true;

   info->r600_max_quad_pipes = dev->max_pipes;
   info->num_cu = 1;
   info->max_se = dev->num_ses;
   info->num_se = dev->num_ses;
   info->max_sa_per_se = 1;

   info->gfx_ib_pad_with_type2 = true;
   info->has_cp_dma = true;
   info->cp_dma_gtt_copy_max_bytes = dev->cp_dma_gtt_copy_max_bytes;
   info->tcc_cache_line_size = 64;
   info->has_graphics = true;
   info->has_image_opcodes = true;
}

struct radeon_winsys *rdn_winsys_create(struct rdn_device *dev,
                                        const struct pipe_screen_config *config,
                                        rdn_screen_create_t screen_create)
{
   struct radeon_drm_winsys *ws = CALLOC_STRUCT(radeon_drm_winsys);

   if (!ws) {
      dev->destroy(dev);
      return NULL;
   }
   ws->dev = dev;
   pipe_reference_init(&ws->reference, 1);
   simple_mtx_init(&ws->lock, mtx_plain);
   list_inithead(&ws->pending_ibs);
   if (rdn_mem_stats_on()) {
      static bool registered;

      if (!registered)
         atexit(rdn_mem_stats_exit);
      registered = true;
   }
   list_inithead(&ws->cache_all);
   for (unsigned b = 0; b < ARRAY_SIZE(ws->cache_buckets); b++)
      list_inithead(&ws->cache_buckets[b]);
   rdn_vram_gpu_base = dev->info.vram_gpu_base;
   rdn_gart_gpu_base = dev->info.gart_gpu_base;
   rdn_init_info(ws);

   rdn_shim_device_id = dev->info.pci_device_id;
   rdn_shim_tiling_config = dev->info.tile_config;
   ws->surf_man = radeon_surface_manager_new(-1);
   if (!ws->surf_man) {
      fprintf(stderr, "rdn: no surface layout for device %04x\n",
              (unsigned)dev->info.pci_device_id);
      goto fail;
   }

   ws->base.unref = rdn_winsys_unref;
   ws->base.destroy = rdn_winsys_destroy;
   ws->base.get_fd = rdn_get_fd;
   ws->base.query_info = rdn_query_info;
   ws->base.query_value = rdn_query_value;
   ws->base.read_registers = rdn_read_registers;

   ws->base.buffer_create = rdn_buffer_create;
   ws->base.buffer_destroy = rdn_buffer_destroy;
   ws->base.buffer_map = rdn_buffer_map;
   ws->base.buffer_unmap = rdn_buffer_unmap;
   ws->base.buffer_wait = rdn_buffer_wait;
   ws->base.buffer_get_metadata = rdn_buffer_get_metadata;
   ws->base.buffer_set_metadata = rdn_buffer_set_metadata;
   ws->base.buffer_from_handle = rdn_buffer_from_handle;
   ws->base.buffer_from_ptr = rdn_buffer_from_ptr;
   ws->base.buffer_is_user_ptr = rdn_buffer_is_user_ptr;
   ws->base.buffer_is_suballocated = rdn_buffer_false;
   ws->base.buffer_get_handle = rdn_buffer_get_handle;
   ws->base.buffer_commit = rdn_buffer_commit;
   ws->base.buffer_get_virtual_address = rdn_buffer_get_virtual_address;
   ws->base.buffer_get_reloc_offset = rdn_buffer_get_reloc_offset;
   ws->base.buffer_get_initial_domain = rdn_buffer_get_initial_domain;
   ws->base.buffer_get_flags = rdn_buffer_get_flags;

   ws->base.ctx_create = rdn_ctx_create;
   ws->base.ctx_destroy = rdn_ctx_destroy;
   ws->base.ctx_query_reset_status = rdn_ctx_query_reset_status;
   ws->base.cs_create = rdn_cs_create;
   ws->base.cs_destroy = rdn_cs_destroy;
   ws->base.cs_add_buffer = rdn_cs_add_buffer;
   ws->base.cs_lookup_buffer = rdn_cs_lookup_buffer;
   ws->base.cs_validate = rdn_cs_validate;
   ws->base.cs_check_space = rdn_cs_check_space;
   ws->base.cs_get_buffer_list = rdn_cs_get_buffer_list;
   ws->base.cs_flush = rdn_cs_flush;
   ws->base.cs_get_next_fence = rdn_cs_get_next_fence;
   ws->base.cs_is_buffer_referenced = rdn_cs_is_buffer_referenced;
   ws->base.cs_request_feature = rdn_cs_request_feature;
   ws->base.cs_sync_flush = rdn_cs_sync_flush;
   ws->base.fence_wait = rdn_ws_fence_wait;
   ws->base.fence_reference = rdn_ws_fence_reference;

   /* surface_init, from Mesa's radeon_drm_surface.c. */
   radeon_surface_init_functions(ws);

   ws->base.screen = screen_create(&ws->base, config);
   if (!ws->base.screen)
      goto fail;
   return &ws->base;

fail:
   if (ws->surf_man)
      radeon_surface_manager_free(ws->surf_man);
   dev->destroy(dev);
   simple_mtx_destroy(&ws->lock);
   FREE(ws);
   return NULL;
}
