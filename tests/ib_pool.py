#!/usr/bin/env python3
"""Exercise the winsys IB allocator with controllable device fences."""
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
mesa = Path(sys.argv[1])
source = (root / 'mesa/winsys/rdn_winsys.c').read_text()
def section(start, end):
    return source[source.index(start):source.index(end, source.index(start))]

prefix = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util/list.h"
#define FREE free
#define OS_TIMEOUT_INFINITE UINT64_MAX
#define RDN_VISIBLE 0
struct rdn_device {
   int (*alloc_command)(struct rdn_device *, uint64_t, uint64_t, uint64_t *);
   int (*alloc)(struct rdn_device *, uint64_t, uint64_t, uint64_t *);
   void (*free)(struct rdn_device *, uint64_t);
};
struct rdn_busy { uint32_t number; bool used, uvd, unknown; };
struct radeon_drm_winsys {
   struct rdn_device *dev;
   struct list_head pending_ibs, idle_ibs;
   unsigned idle_ib_count;
   uint64_t idle_ib_bytes, ib_pool_hits, ib_pool_allocs;
};
static unsigned next_offset, allocations, frees, done[2], fail_alloc, cache_puts;
static bool live[256], allow_wait = true;
static int alloc(struct rdn_device *dev, uint64_t size, uint64_t align, uint64_t *out)
{
   (void)dev; (void)size; assert(align == 4096);
   if (fail_alloc) { --fail_alloc; return -ENOMEM; }
   *out = ++next_offset; assert(next_offset < 256);
   live[*out] = true; ++allocations; return 0;
}
static void release(struct rdn_device *dev, uint64_t offset)
{ (void)dev; assert(live[offset]); live[offset] = false; ++frees; }
static bool rdn_number_wait(struct radeon_drm_winsys *ws, uint32_t number,
                            bool uvd, uint64_t timeout)
{
   (void)ws;
   if (timeout && allow_wait) done[uvd] = number;
   return number <= done[uvd];
}
static void rdn_cache_trim(struct radeon_drm_winsys *ws, bool force)
{ (void)ws; (void)force; }
static bool rdn_cache_get(struct radeon_drm_winsys *ws, uint64_t size,
                         uint64_t align, int region, bool wait, uint64_t *offset)
{ (void)ws; (void)size; (void)align; (void)region; (void)wait; (void)offset; return false; }
static void rdn_cache_put(struct radeon_drm_winsys *ws, uint64_t offset,
                         uint64_t size, int region, const struct rdn_busy *busy)
{ (void)size; (void)region; (void)busy; ++cache_puts; release(ws->dev, offset); }
'''
body = r'''
static void pending(struct radeon_drm_winsys *ws, uint64_t offset, uint64_t size,
                    unsigned fence, bool uvd, bool unknown)
{
   struct rdn_pending_ib *ib = calloc(1, sizeof(*ib)); assert(ib);
   ib->offset = offset; ib->size = size;
   ib->busy = (struct rdn_busy){fence, true, uvd, unknown};
   list_addtail(&ib->list, &ws->pending_ibs);
}
int main(void)
{
   struct rdn_device dev = {alloc, alloc, release};
   struct radeon_drm_winsys ws = {.dev = &dev};
   uint64_t first, second, offset;
   list_inithead(&ws.pending_ibs); list_inithead(&ws.idle_ibs);
   assert(!rdn_ib_alloc(&ws, 65536, &first));
   pending(&ws, first, 65536, 10, false, false);
   assert(!rdn_ib_alloc(&ws, 65536, &second) && second != first);
   release(&dev, second);
   done[0] = 10;
   assert(!rdn_ib_alloc(&ws, 65536, &second) && second == first);
   assert(ws.ib_pool_hits == 1);
   pending(&ws, second, 65536, 11, true, false);
   done[0] = 100; rdn_reap_ibs(&ws, false);
   assert(ws.idle_ib_count == 0);
   done[1] = 11; rdn_reap_ibs(&ws, false);
   assert(ws.idle_ib_count == 1);
   assert(!rdn_ib_alloc(&ws, 131072, &offset) && offset != first);
   release(&dev, offset);
   fail_alloc = 1;
   assert(!rdn_ib_alloc(&ws, 262144, &offset));
   assert(!ws.idle_ib_count && !live[first]); release(&dev, offset);
   for (unsigned i = 0; i < 12; ++i) {
      assert(!alloc(&dev, 65536, 4096, &offset));
      pending(&ws, offset, 65536, 1, false, false);
   }
   rdn_reap_ibs(&ws, false);
   assert(ws.idle_ib_count == 8 && ws.idle_ib_bytes == 8 * 65536);
   rdn_idle_ibs_clear(&ws);
   for (unsigned i = 0; i < 4; ++i) {
      assert(!alloc(&dev, 1048576, 4096, &offset));
      pending(&ws, offset, 1048576, 1, false, false);
   }
   rdn_reap_ibs(&ws, false);
   assert(ws.idle_ib_bytes == 2097152 && ws.idle_ib_count == 2);
   rdn_idle_ibs_clear(&ws);
   done[0] = 0; allow_wait = false;
   for (unsigned i = 0; i < 4; ++i) {
      assert(!rdn_ib_alloc(&ws, 65536, &offset));
      pending(&ws, offset, 65536, i + 1, false, false);
   }
   unsigned before = allocations;
   assert(rdn_ib_alloc(&ws, 65536, &offset) == -EIO && allocations == before);
   allow_wait = true;
   assert(!rdn_ib_alloc(&ws, 65536, &offset)); release(&dev, offset);
   rdn_reap_ibs(&ws, true); rdn_idle_ibs_clear(&ws);
   assert(!rdn_ib_alloc(&ws, 65536, &offset));
   pending(&ws, offset, 65536, 0, false, true);
   rdn_reap_ibs(&ws, true);
   assert(!ws.idle_ib_count && !list_is_empty(&ws.pending_ibs) && live[offset]);
   struct rdn_pending_ib *ib = list_first_entry(&ws.pending_ibs, struct rdn_pending_ib, list);
   list_del(&ib->list); release(&dev, ib->offset); free(ib);
   dev.alloc_command = NULL;
   assert(!rdn_ib_alloc(&ws, 65536, &offset));
   pending(&ws, offset, 65536, 1, false, false);
   rdn_reap_ibs(&ws, true);
   assert(cache_puts == 1 && !ws.idle_ib_count);
   assert(allocations == frees);
   puts("PASS: fence isolation, reuse, size matching, OOM, pool bounds, throttle, unknown submission, fallback, teardown");
}
'''
program = prefix + section('struct rdn_pending_ib {', 'static inline struct radeon_drm_winsys')
program += section('static bool rdn_busy_wait(', 'static bool rdn_fence_wait(')
program += section('/* Dedicated command allocations', '/* RDN_STATS: what went') + body
with tempfile.TemporaryDirectory(prefix='rdn-ib-test-') as d:
    path = Path(d)
    (path / 'test.c').write_text(program)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-g', '-I' + str(mesa / 'src'), str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
