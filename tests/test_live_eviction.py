"""Inject allocation and completion failures into the live residency helpers."""
import os,pathlib,shlex,subprocess,tempfile,unittest
from test_winsys_sync import extract
ROOT=pathlib.Path(__file__).resolve().parents[1]
HARNESS=r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#define RADEON_DOMAIN_GTT 2
#define RADEON_DOMAIN_VRAM 4
struct busy { bool unknown; unsigned number; };
struct rdn_bo { struct {unsigned alignment_log2;} base; struct busy last_use,last_uvd;
 uint64_t managed_alias,managed_size,offset; void *evicted_storage; unsigned domain,diagnostic_id; };
struct rdn_device { int (*managed_evict)(struct rdn_device*,uint64_t,uint64_t,void**);
 int (*managed_promote)(struct rdn_device*,uint64_t,uint64_t,uint64_t,void*);
 int (*alloc_hidden)(struct rdn_device*,uint64_t,unsigned,uint64_t*);
 void (*free)(struct rdn_device*,uint64_t); };
struct radeon_drm_winsys { struct rdn_device *dev; uint64_t managed_budget,managed_resident; unsigned evictions,promotions; };
static struct rdn_bo bos[2];static unsigned freed,copies,allocs;static int eviction_error,promotion_error,allocation_error;
#define list_for_each_entry(type,bo,head,link) for(type *bo=bos;bo<bos+2;bo++)
static bool rdn_busy_wait(struct radeon_drm_winsys *w,struct busy *b,unsigned timeout){return !b->unknown&&!b->number;}
static bool rdn_mem_stats_on(void){return false;}
static int evict(struct rdn_device*d,uint64_t a,uint64_t s,void**p){copies++;*p=(void*)0x1000;return eviction_error;}
static int promote(struct rdn_device*d,uint64_t a,uint64_t p,uint64_t s,void*b){copies++;return promotion_error;}
static int alloc(struct rdn_device*d,uint64_t s,unsigned a,uint64_t*p){allocs++;*p=0x2000;return allocation_error;}
static void release(struct rdn_device*d,uint64_t p){freed++;}
"""
CASES=r"""
int main(void){
 struct rdn_device dev={evict,promote,alloc,release};
 struct radeon_drm_winsys ws={.dev=&dev,.managed_resident=8192};
 bos[0]=(struct rdn_bo){.managed_alias=0x40000000,.managed_size=4096,.offset=8192,.base.alignment_log2=12};
 bos[1]=bos[0];bos[1].last_use.unknown=true;
 bos[0].last_use.number=1;assert(!rdn_evict_one(&ws)&&!copies&&!freed);
 bos[0].last_use.number=0;bos[0].last_uvd.unknown=true;assert(!rdn_evict_one(&ws)&&!copies);
 bos[0].last_uvd.unknown=false;eviction_error=-ENOMEM;
 assert(!rdn_evict_one(&ws)&&!freed&&!bos[0].last_use.unknown&&ws.managed_resident==8192);
 eviction_error=-EIO;assert(!rdn_evict_one(&ws)&&!freed&&bos[0].last_use.unknown);
 bos[0].last_use.unknown=false;eviction_error=0;
 assert(rdn_evict_one(&ws)&&freed==1&&bos[0].evicted_storage&&ws.managed_resident==4096&&ws.evictions==1);
 assert(bos[0].managed_alias==0x40000000&&bos[0].domain==RADEON_DOMAIN_GTT);
 ws.managed_budget=4096;assert(rdn_promote_buffer(&ws,&bos[0])&&!allocs);
 ws.managed_budget=8192;allocation_error=-ENOMEM;assert(rdn_promote_buffer(&ws,&bos[0])&&bos[0].evicted_storage);
 allocation_error=0;promotion_error=-EIO;assert(!rdn_promote_buffer(&ws,&bos[0])&&bos[0].last_use.unknown&&freed==1&&bos[0].evicted_storage);
 bos[0].last_use.unknown=false;promotion_error=0;
 assert(rdn_promote_buffer(&ws,&bos[0])&&!bos[0].evicted_storage&&bos[0].offset==0x2000);
 assert(ws.managed_resident==8192&&ws.promotions==1&&bos[0].managed_alias==0x40000000);
 puts("PASS live eviction skips busy BOs, retains unknown copies, bounds residency and preserves VA");return 0;
}
"""
class LiveEviction(unittest.TestCase):
 def test_residency_failures(self):
  source=(ROOT/'mesa/winsys/rdn_winsys.c').read_text()
  body=HARNESS+extract(source,'rdn_evict_one')+extract(source,'rdn_promote_buffer')+CASES
  with tempfile.TemporaryDirectory() as d:
   p=pathlib.Path(d);(p/'test.c').write_text(body)
   subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu99','-Wall','-Werror']+shlex.split(os.environ.get('CFLAGS',''))+[str(p/'test.c'),'-o',str(p/'test')],check=True)
   subprocess.run(shlex.split(os.environ.get('TEST_RUNNER',''))+[str(p/'test')],check=True)
if __name__=='__main__':unittest.main()
