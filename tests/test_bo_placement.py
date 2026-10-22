"""Inject allocation pressure into the actual winsys buffer allocator."""
import pathlib
import os
import shlex
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
#include <string.h>
#define CALLOC_STRUCT(t) calloc(1, sizeof(struct t))
#define FREE free
#define MAX2(a,b) ((a) > (b) ? (a) : (b))
enum radeon_bo_domain { RADEON_DOMAIN_GTT = 2, RADEON_DOMAIN_VRAM = 4 };
enum radeon_bo_flag { RADEON_FLAG_NO_CPU_ACCESS = 1, RADEON_FLAG_NO_INTERPROCESS_SHARING = 2 };
enum rdn_place { RDN_VISIBLE, RDN_HIDDEN, RDN_GART, RDN_SEGMENT };
struct pb_buffer_lean { int reference, alignment_log2, usage; uint64_t size; };
struct rdn_bo { struct pb_buffer_lean base; unsigned diagnostic_id; uint64_t offset;
               enum rdn_place region; unsigned domain, flags; };
struct rdn_device { int (*alloc)(struct rdn_device *,uint64_t,unsigned,uint64_t *);
                    int (*alloc_hidden)(struct rdn_device *,uint64_t,unsigned,uint64_t *);
                    int (*gart_alloc)(struct rdn_device *,uint64_t,unsigned,uint64_t *);
                    int (*uvd_alloc)(struct rdn_device *,uint64_t,unsigned,uint64_t *);
                    bool uvd_start;
                    void (*free)(struct rdn_device *,uint64_t); };
struct radeon_winsys { int unused; };
struct radeon_drm_winsys { bool video,uvd_seg,uvd_split; int lock; struct rdn_device *dev; uint64_t allocated_bytes;
                         struct { unsigned creates, cache_hits, overflowed; } mem_stats; };
static struct radeon_drm_winsys ws;
static unsigned calls[4], trims, scenario, frees, filler_calls, event_count;
static char events[128];
static bool cache_hit;
static void event(char c) { assert(event_count<sizeof(events)-1);events[event_count++]=c;events[event_count]=0; }
static bool rdn_uvd_prepare(struct radeon_drm_winsys *w) { assert(w->lock);return true; }
static int rdn_next_diagnostic_id;
static unsigned p_atomic_inc_return(int *p) { return ++*p; }
static uint64_t align64(uint64_t n,unsigned a) { return (n+a-1)&~(uint64_t)(a-1); }
static unsigned util_next_power_of_two(unsigned n) { unsigned v=1;while(v<n)v*=2;return v; }
static unsigned util_logbase2(unsigned n) { unsigned v=0;while(n>1){n/=2;v++;}return v; }
static struct radeon_drm_winsys *rdn_winsys(struct radeon_winsys *p) { return &ws; }
static void simple_mtx_lock(int *p) { assert(!*p);*p=1; }
static void simple_mtx_unlock(int *p) { assert(*p);*p=0; }
static bool rdn_cache_get(struct radeon_drm_winsys *w,uint64_t s,unsigned a,
                          enum rdn_place p,uint64_t *o) { assert(w->lock);*o=4096;return cache_hit; }
static void rdn_cache_trim(struct radeon_drm_winsys *w,bool all) { assert(w->lock && all);trims++;event('T'); }
static void rdn_mem_stats_account(struct radeon_drm_winsys *w,uint64_t s,enum rdn_place p,int n) {}
static bool rdn_mem_stats_on(void) { return false; }
static void rdn_mem_stats_print(const char *s) {}
static void pipe_reference_init(int *p,int n) { *p=n; }
static int allocate(unsigned region,uint64_t *offset) {
   assert(ws.lock);calls[region]++;*offset=4096;event("VHGS"[region]);
   if (scenario==1 && trims) return 0;
   if (scenario==2 && region==RDN_VISIBLE) return 0;
   if (scenario==3 && region==RDN_GART) return 0;
   if (scenario==4) return 0;
   return -1;
}
static int visible(struct rdn_device *d,uint64_t s,unsigned a,uint64_t *o) { return allocate(RDN_VISIBLE,o); }
static int hidden(struct rdn_device *d,uint64_t s,unsigned a,uint64_t *o) {
   if(s==(2u<<20)) { assert(ws.lock);filler_calls++;*o=8192;event('F');return 0; }
   return allocate(RDN_HIDDEN,o);
}
static int gart(struct rdn_device *d,uint64_t s,unsigned a,uint64_t *o) { return allocate(RDN_GART,o); }
static int segment(struct rdn_device *d,uint64_t s,unsigned a,uint64_t *o) { return allocate(RDN_SEGMENT,o); }
static void release(struct rdn_device *d,uint64_t o) { assert(ws.lock && o==8192);frees++;event('f'); }
static void reset(unsigned mode) {
   assert(!ws.lock);scenario=mode;memset(calls,0,sizeof(calls));trims=frees=filler_calls=event_count=0;events[0]=0;
   ws.video=ws.uvd_seg=ws.uvd_split=cache_hit=false;
}
'''
CASES = r'''
int main(void) {
   struct rdn_device dev={visible,hidden,gart,segment,true,release};
   struct pb_buffer_lean *b;
   ws.dev=&dev;
   unsetenv("RDN_HIDDEN_FILL_MB");
   for(unsigned mode=0;mode<4;mode++) {
      reset(mode);
      b=rdn_buffer_create(NULL,65536,4096,RADEON_DOMAIN_VRAM,RADEON_FLAG_NO_CPU_ACCESS);
      assert(!calls[RDN_GART] && trims==1 && calls[RDN_HIDDEN]==2);
      if(mode==1) { assert(b && ((struct rdn_bo *)b)->region==RDN_HIDDEN);assert(!strcmp(events,"HTH")); }
      else if(mode==2) { assert(b && ((struct rdn_bo *)b)->region==RDN_VISIBLE);assert(!strcmp(events,"HTHV")); }
      else assert(!b);
      free(b);
   }
   /* Only private, CPU-accessible storage may spill into the GART. */
   for(unsigned flags=0;flags<4;flags++) {
      reset(3);
      b=rdn_buffer_create(NULL,65536,4096,RADEON_DOMAIN_VRAM,flags);
      bool spill=flags==RADEON_FLAG_NO_INTERPROCESS_SHARING;
      assert((b!=NULL)==spill && (calls[RDN_GART]!=0)==spill);
      if(b) assert(((struct rdn_bo *)b)->region==RDN_GART);
      free(b);
   }
   reset(3);
   b=rdn_buffer_create(NULL,65536,4096,RADEON_DOMAIN_GTT,RADEON_FLAG_NO_INTERPROCESS_SHARING);
   assert(b && !strcmp(events,"G") && ((struct rdn_bo *)b)->domain==RADEON_DOMAIN_GTT);free(b);
   reset(3);
   b=rdn_buffer_create(NULL,65536,4096,RADEON_DOMAIN_GTT,0);
   assert(!b && !calls[RDN_GART] && !strcmp(events,"VTV"));
   reset(3);dev.alloc_hidden=NULL;
   b=rdn_buffer_create(NULL,65536,4096,RADEON_DOMAIN_GTT,
                       RADEON_FLAG_NO_CPU_ACCESS|RADEON_FLAG_NO_INTERPROCESS_SHARING);
   assert(!b && !calls[RDN_GART] && !strcmp(events,"VTV"));
   dev.alloc_hidden=hidden;
   /* Decoder-segment failure must not allocate in any other heap. */
   for(unsigned mode=0;mode<5;mode++) {
      reset(mode);ws.video=ws.uvd_seg=true;
      b=rdn_buffer_create(NULL,65536,4096,RADEON_DOMAIN_GTT,0);
      assert(!calls[RDN_VISIBLE] && !calls[RDN_HIDDEN] && !calls[RDN_GART]);
      assert((b!=NULL)==(mode==1 || mode==4));
      assert(!strcmp(events,mode==4?"S":"STS"));
      if(b)assert(((struct rdn_bo *)b)->region==RDN_SEGMENT);
      free(b);
   }
   reset(4);ws.video=ws.uvd_split=true;
   b=rdn_buffer_create(NULL,65536,4096,RADEON_DOMAIN_GTT,0);
   assert(b && ((struct rdn_bo *)b)->region==RDN_VISIBLE);free(b);
   reset(4);ws.video=ws.uvd_split=true;
   b=rdn_buffer_create(NULL,65536,4096,RADEON_DOMAIN_VRAM,0);
   assert(b && ((struct rdn_bo *)b)->region==RDN_SEGMENT);free(b);
   reset(0);ws.video=ws.uvd_seg=cache_hit=true;
   b=rdn_buffer_create(NULL,65536,4096,RADEON_DOMAIN_VRAM,0);
   assert(b && !event_count && ((struct rdn_bo *)b)->region==RDN_SEGMENT);free(b);
   reset(1);setenv("RDN_HIDDEN_FILL_MB","2",1);
   b=rdn_buffer_create(NULL,256<<10,4096,RADEON_DOMAIN_VRAM,RADEON_FLAG_NO_CPU_ACCESS);
   assert(b && !strcmp(events,"FHfTFHf") && frees==2 && filler_calls==2);free(b);
   unsetenv("RDN_HIDDEN_FILL_MB");
   puts("PASS retry-before-fallback, both GART guards, segment/split placement, cache and hidden-fill lifetime");
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
static int fail_busy = -1;
static unsigned freed;
#define list_for_each_entry_safe(type,var,head,member) \
   for(type *var=nodes;var<nodes+3;var++) if(!var->removed)
static bool rdn_busy_wait(struct radeon_drm_winsys *w,int *b,uint64_t t) { return *b != fail_busy; }
static void rdn_region_free(struct radeon_drm_winsys *w,unsigned r,uint64_t o,uint64_t s) { freed++; }
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
   assert(freed==3);
   ws.cached_bytes[0]=4;ws.cached_bytes[1]=200;
   nodes[0]=(struct rdn_cached){.region=0,.size=4,.stamp=90,.busy=77};
   nodes[1]=(struct rdn_cached){.region=1,.size=100,.stamp=91};
   nodes[2]=(struct rdn_cached){.region=1,.size=100,.stamp=92};
   fail_busy=77;
   rdn_cache_trim(&ws,true);
   assert(!nodes[0].removed && nodes[1].removed && nodes[2].removed && freed==5);
   assert(ws.cached_bytes[0]==4 && !ws.cached_bytes[1]);
   fail_busy=-1;
   rdn_cache_trim(&ws,true);
   assert(nodes[0].removed && freed==6 && !ws.cached_bytes[0]);
   puts("PASS per-pool cache budget and failed graphics fence retains cached allocation");
   return 0;
}
'''

CACHE_PUT_HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#define OS_TIMEOUT_INFINITE UINT64_MAX
enum rdn_place { RDN_VISIBLE };
struct list_head { int unused; };
struct rdn_busy { bool unknown; };
struct rdn_cached { struct list_head all,bucket; uint64_t offset,size,stamp;
                    enum rdn_place region; struct rdn_busy busy; };
struct radeon_drm_winsys { uint64_t num_flushes,cached_bytes[1]; struct list_head cache_all; };
static bool oom;
static unsigned freed;
static struct rdn_cached *allocated;
static void *allocate(size_t bytes) { return oom?NULL:(allocated=calloc(1,bytes)); }
#define CALLOC_STRUCT(t) allocate(sizeof(struct t))
static bool rdn_busy_wait(struct radeon_drm_winsys *w,const struct rdn_busy *b,uint64_t t) { return !b->unknown; }
static void rdn_region_free(struct radeon_drm_winsys *w,enum rdn_place r,uint64_t o,uint64_t s) { freed++; }
static struct list_head *rdn_cache_bucket(struct radeon_drm_winsys *w,uint64_t s,enum rdn_place p) { return &w->cache_all; }
static void list_addtail(struct list_head *a,struct list_head *b) {}
'''
CACHE_PUT_CASES = r'''
int main(void) {
   struct radeon_drm_winsys ws={0};struct rdn_busy busy={true};
   oom=true;rdn_cache_put(&ws,4096,4096,RDN_VISIBLE,&busy);
   assert(!freed && !allocated && !ws.cached_bytes[0]);
   busy.unknown=false;rdn_cache_put(&ws,4096,4096,RDN_VISIBLE,&busy);assert(freed==1);
   oom=false;busy.unknown=true;rdn_cache_put(&ws,8192,4096,RDN_VISIBLE,&busy);
   assert(allocated && allocated->busy.unknown && ws.cached_bytes[0]==4096 && freed==1);
   free(allocated);
   puts("PASS cache-node OOM never frees unknown-busy storage");return 0;
}
'''


class PlacementTest(unittest.TestCase):
    def compile_run(self, source):
        with tempfile.TemporaryDirectory(prefix="rdn-placement-") as directory:
            path = pathlib.Path(directory)
            (path / "test.c").write_text(source)
            command = shlex.split(os.environ.get("CC", "cc"))
            command += ["-std=gnu99", "-Wall", "-Werror"]
            command += shlex.split(os.environ.get("CFLAGS", ""))
            subprocess.run(command + ["-o", str(path / "test"), str(path / "test.c")], check=True)
            runner = shlex.split(os.environ.get("TEST_RUNNER", ""))
            subprocess.run(runner + [str(path / "test")], check=True)

    def test_allocation_pressure(self):
        source = (ROOT / "mesa/winsys/rdn_winsys.c").read_text()
        begin = source.index("static struct pb_buffer_lean *\nrdn_buffer_create(")
        end = source.index("\nstatic void rdn_buffer_destroy", begin)
        self.compile_run(HARNESS + source[begin:end] + CASES)

    def test_cache_metadata_allocation_failure(self):
        source = (ROOT / "mesa/winsys/rdn_winsys.c").read_text()
        begin = source.index("static void rdn_cache_put(")
        end = source.index("\n/* Memory of exactly", begin)
        self.compile_run(CACHE_PUT_HARNESS + source[begin:end] + CACHE_PUT_CASES)

    def test_per_pool_cache_budget(self):
        source = (ROOT / "mesa/winsys/rdn_winsys.c").read_text()
        begin = source.index("static void rdn_cache_trim(")
        end = source.index("\n/*", begin)
        self.compile_run(CACHE_HARNESS + source[begin:end] + CACHE_CASES)


if __name__ == "__main__":
    unittest.main()
