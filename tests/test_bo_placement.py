"""Inject allocation pressure into the actual winsys buffer allocator."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define CALLOC_STRUCT(t) calloc(1, sizeof(struct t))
#define FREE free
#define MAX2(a,b) ((a) > (b) ? (a) : (b))
enum radeon_bo_domain { RADEON_DOMAIN_GTT = 2, RADEON_DOMAIN_VRAM = 4 };
enum radeon_bo_flag { RADEON_FLAG_NO_CPU_ACCESS = 1 };
enum rdn_place { RDN_VISIBLE, RDN_HIDDEN, RDN_GART };
struct pb_buffer_lean { int reference, alignment_log2, usage; uint64_t size; };
struct rdn_bo { struct pb_buffer_lean base; unsigned diagnostic_id; uint64_t offset;
               enum rdn_place region; unsigned domain, flags; };
struct rdn_device { int (*alloc)(struct rdn_device *,uint64_t,unsigned,uint64_t *);
                    int (*alloc_hidden)(struct rdn_device *,uint64_t,unsigned,uint64_t *);
                    int (*gart_alloc)(struct rdn_device *,uint64_t,unsigned,uint64_t *); };
struct radeon_winsys { int unused; };
struct radeon_drm_winsys { int lock; struct rdn_device *dev; uint64_t allocated_bytes;
                         struct { unsigned creates, cache_hits, overflowed; } mem_stats; };
static struct radeon_drm_winsys ws;
static unsigned calls[3], trims, scenario;
static int rdn_next_diagnostic_id;
static unsigned p_atomic_inc_return(int *p) { return ++*p; }
static uint64_t align64(uint64_t n,unsigned a) { return (n+a-1)&~(uint64_t)(a-1); }
static unsigned util_next_power_of_two(unsigned n) { unsigned v=1;while(v<n)v*=2;return v; }
static unsigned util_logbase2(unsigned n) { unsigned v=0;while(n>1){n/=2;v++;}return v; }
static struct radeon_drm_winsys *rdn_winsys(struct radeon_winsys *p) { return &ws; }
static void simple_mtx_lock(int *p) { assert(!*p);*p=1; }
static void simple_mtx_unlock(int *p) { assert(*p);*p=0; }
static bool rdn_cache_get(struct radeon_drm_winsys *w,uint64_t s,unsigned a,
                          enum rdn_place p,uint64_t *o) { return false; }
static void rdn_cache_trim(struct radeon_drm_winsys *w,bool all) { assert(all);trims++; }
static void rdn_mem_stats_account(struct radeon_drm_winsys *w,uint64_t s,enum rdn_place p,int n) {}
static bool rdn_mem_stats_on(void) { return false; }
static void rdn_mem_stats_print(const char *s) {}
static void pipe_reference_init(int *p,int n) { *p=n; }
static int allocate(unsigned region,uint64_t *offset) {
   calls[region]++;*offset=4096;
   if (scenario==1 && region==RDN_HIDDEN && trims) return 0;
   if (scenario==2 && region==RDN_VISIBLE) return 0;
   if (scenario==3 && region==RDN_GART) return 0;
   return -1;
}
static int visible(struct rdn_device *d,uint64_t s,unsigned a,uint64_t *o) { return allocate(RDN_VISIBLE,o); }
static int hidden(struct rdn_device *d,uint64_t s,unsigned a,uint64_t *o) { return allocate(RDN_HIDDEN,o); }
static int gart(struct rdn_device *d,uint64_t s,unsigned a,uint64_t *o) { return allocate(RDN_GART,o); }
'''
CASES = r'''
int main(void) {
   struct rdn_device dev={visible,hidden,gart};
   ws.dev=&dev;
   for(scenario=0;scenario<4;scenario++) {
      calls[0]=calls[1]=calls[2]=trims=0;
      struct pb_buffer_lean *b=rdn_buffer_create(NULL,65536,4096,RADEON_DOMAIN_VRAM,RADEON_FLAG_NO_CPU_ACCESS);
      assert(calls[RDN_GART]==0);
      assert(trims==1 && calls[RDN_HIDDEN]==2);
      if(scenario==1) { assert(b && ((struct rdn_bo *)b)->region==RDN_HIDDEN);assert(!calls[RDN_VISIBLE]); }
      else if(scenario==2) { assert(b && ((struct rdn_bo *)b)->region==RDN_VISIBLE); }
      else assert(!b);
      free(b);
   }
   scenario=3;calls[0]=calls[1]=calls[2]=trims=0;
   struct pb_buffer_lean *b=rdn_buffer_create(NULL,65536,4096,RADEON_DOMAIN_GTT,0);
   assert(b && ((struct rdn_bo *)b)->region==RDN_GART && calls[RDN_GART]==1 && !trims);
   free(b);
   b=rdn_buffer_create(NULL,65536,4096,RADEON_DOMAIN_VRAM|RADEON_DOMAIN_GTT,0);
   assert(b && ((struct rdn_bo *)b)->region==RDN_GART);
   free(b);
   b=rdn_buffer_create(NULL,65536,4096,RADEON_DOMAIN_VRAM,0);
   assert(b && ((struct rdn_bo *)b)->region==RDN_GART);
   free(b);
   puts("PASS tiled placement, heap reclamation, visible fallback and CPU-mappable GTT fallback");
   return 0;
}
'''

CACHE_HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define OS_TIMEOUT_INFINITE UINT64_MAX
#define RDN_CACHE_FLUSHES 128
static const uint64_t RDN_CACHE_BYTES[3] = {32,128,64};
struct rdn_cached { unsigned region; uint64_t size,stamp,offset; bool removed; int busy; };
struct radeon_drm_winsys { uint64_t num_flushes,cached_bytes[3]; };
static struct rdn_cached nodes[3];
#define list_for_each_entry_safe(type,var,head,member) \
   for(type *var=nodes;var<nodes+3;var++) if(!var->removed)
static bool rdn_busy_wait(struct radeon_drm_winsys *w,int *b,uint64_t t) { return true; }
static void rdn_region_free(struct radeon_drm_winsys *w,unsigned r,uint64_t o,uint64_t s) {}
static void rdn_cache_drop(struct radeon_drm_winsys *w,struct rdn_cached *c) {
   assert(!c->removed);c->removed=true;w->cached_bytes[c->region]-=c->size;
}
'''
CACHE_CASES = r'''
int main(void) {
   struct radeon_drm_winsys ws={.num_flushes=100,.cached_bytes={4,200,0}};
   nodes[0]=(struct rdn_cached){.region=0,.size=4,.stamp=90};
   nodes[1]=(struct rdn_cached){.region=1,.size=100,.stamp=91};
   nodes[2]=(struct rdn_cached){.region=1,.size=100,.stamp=92};
   rdn_cache_trim(&ws,false);
   assert(!nodes[0].removed && nodes[1].removed && !nodes[2].removed);
   assert(ws.cached_bytes[1]==100);
   rdn_cache_trim(&ws,true);
   assert(nodes[0].removed && nodes[2].removed);
   assert(!ws.cached_bytes[0] && !ws.cached_bytes[1]);
   puts("PASS per-pool cache budget behind an in-budget older allocation");
   return 0;
}
'''


class PlacementTest(unittest.TestCase):
    def compile_run(self, source):
        with tempfile.TemporaryDirectory(prefix="rdn-placement-") as directory:
            path = pathlib.Path(directory)
            (path / "test.c").write_text(source)
            subprocess.run(["cc", "-std=gnu99", "-Wall", "-Werror", "-o",
                            str(path / "test"), str(path / "test.c")], check=True)
            subprocess.run([str(path / "test")], check=True)

    def test_allocation_pressure(self):
        source = (ROOT / "mesa/winsys/rdn_winsys.c").read_text()
        begin = source.index("static struct pb_buffer_lean *\nrdn_buffer_create(")
        end = source.index("\nstatic void rdn_buffer_destroy", begin)
        self.compile_run(HARNESS + source[begin:end] + CASES)

    def test_per_pool_cache_budget(self):
        source = (ROOT / "mesa/winsys/rdn_winsys.c").read_text()
        begin = source.index("static void rdn_cache_trim(")
        end = source.index("\n/*", begin)
        self.compile_run(CACHE_HARNESS + source[begin:end] + CACHE_CASES)


if __name__ == "__main__":
    unittest.main()
