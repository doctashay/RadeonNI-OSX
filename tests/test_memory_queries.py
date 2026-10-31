"""Separate heap queries and account for live eviction and pending cache entries."""
from pathlib import Path
import subprocess
import tempfile
from test_winsys_sync import extract
source=(Path(__file__).resolve().parents[1]/'mesa/winsys/rdn_winsys.c').read_text()
harness=r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
enum rdn_place { RDN_VISIBLE,RDN_HIDDEN,RDN_GART,RDN_USER,RDN_SEGMENT };
enum radeon_value_id { RADEON_NUM_GFX_IBS,RADEON_TIMESTAMP,RADEON_REQUESTED_VRAM_MEMORY,
 RADEON_REQUESTED_GTT_MEMORY,RADEON_VRAM_USAGE,RADEON_GTT_USAGE,RADEON_VRAM_VIS_USAGE,
 RADEON_MAPPED_VRAM,RADEON_MAPPED_GTT,RADEON_NUM_EVICTIONS };
struct radeon_winsys { int unused; };
struct rdn_bo { bool evicted_storage; uint64_t managed_size; };
struct radeon_drm_winsys { int lock; uint64_t cached_bytes[5],num_flushes,evictions;
 struct { uint64_t bytes[5]; } mem_stats; };
static struct radeon_drm_winsys ws;
static struct rdn_bo buffers[2];
#define list_for_each_entry(type,bo,head,link) for(type *bo=buffers;bo<buffers+2;bo++)
static struct radeon_drm_winsys *rdn_winsys(struct radeon_winsys *p) { return &ws; }
static void simple_mtx_lock(int *p) { assert(!*p);*p=1; }
static void simple_mtx_unlock(int *p) { assert(*p);*p=0; }
static uint64_t os_time_get_nano(void) { return 123; }
'''
cases=r'''
int main(void) {
 ws.mem_stats.bytes[RDN_VISIBLE]=100;ws.cached_bytes[RDN_VISIBLE]=20;
 ws.mem_stats.bytes[RDN_HIDDEN]=400;ws.cached_bytes[RDN_HIDDEN]=40;
 ws.mem_stats.bytes[RDN_SEGMENT]=80;
 ws.mem_stats.bytes[RDN_GART]=200;ws.cached_bytes[RDN_GART]=30;
 ws.mem_stats.bytes[RDN_USER]=50;ws.cached_bytes[RDN_USER]=10;
 buffers[0]=(struct rdn_bo){true,300};buffers[1]=(struct rdn_bo){false,100};
 ws.evictions=2;
 assert(rdn_query_value(0,RADEON_REQUESTED_VRAM_MEMORY)==640);
 assert(rdn_query_value(0,RADEON_REQUESTED_GTT_MEMORY)==290);
 assert(rdn_query_value(0,RADEON_VRAM_USAGE)==340);
 assert(rdn_query_value(0,RADEON_GTT_USAGE)==590);
 assert(rdn_query_value(0,RADEON_MAPPED_VRAM)==120);
 assert(rdn_query_value(0,RADEON_MAPPED_GTT)==590);
 assert(rdn_query_value(0,RADEON_NUM_EVICTIONS)==2);
 buffers[0].evicted_storage=false;
 assert(rdn_query_value(0,RADEON_VRAM_USAGE)==640);
 assert(rdn_query_value(0,RADEON_GTT_USAGE)==290);
 assert(!ws.lock);
 puts("PASS separate VRAM/GTT budgets, cached allocations, host imports and live residency");
}
'''
with tempfile.TemporaryDirectory(prefix='rdn-memory-') as d:
 p=Path(d);(p/'test.c').write_text(harness+extract(source,'rdn_query_value')+cases)
 subprocess.run(['cc','-std=gnu99','-Wall','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
