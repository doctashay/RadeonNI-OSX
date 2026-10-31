"""Shared/command requests must not reuse cached private slab entries."""
from pathlib import Path
import subprocess
import tempfile
from test_winsys_sync import extract

root = Path(__file__).resolve().parents[1]
source = (root/'mesa/winsys/rdn_winsys.c').read_text()
harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
enum rdn_place { RDN_VISIBLE, RDN_GART };
struct list_head { int unused; };
struct rdn_cached { uint64_t size,offset; enum rdn_place region; bool busy; };
struct device { bool (*is_small)(struct device *,uint64_t); };
struct radeon_drm_winsys { struct device *dev; };
static struct rdn_cached entries[2];
static unsigned count, dropped;
static struct list_head bucket;
#define list_for_each_entry_safe(type,c,head,link) \
 for (type *c=entries; c<entries+count; c++)
static struct list_head *rdn_cache_bucket(struct radeon_drm_winsys *ws,uint64_t size,enum rdn_place region) { return &bucket; }
static bool rdn_busy_wait(struct radeon_drm_winsys *ws,bool *busy,unsigned timeout) { return !*busy; }
static void rdn_cache_drop(struct radeon_drm_winsys *ws,struct rdn_cached *c) { dropped++; }
static bool is_small(struct device *d,uint64_t offset) { return offset==4096; }
'''
cases = r'''
int main(void) {
 struct device dev={is_small}; struct radeon_drm_winsys ws={&dev}; uint64_t at=0;
 entries[0]=(struct rdn_cached){256,4096,RDN_VISIBLE,false}; count=1;
 assert(!rdn_cache_get(&ws,256,256,RDN_VISIBLE,false,&at) && !dropped);
 assert(rdn_cache_get(&ws,256,256,RDN_VISIBLE,true,&at) && at==4096 && dropped==1);
 entries[0].busy=true;
 assert(!rdn_cache_get(&ws,256,256,RDN_VISIBLE,true,&at) && dropped==1);
 entries[1]=(struct rdn_cached){256,8192,RDN_VISIBLE,false}; count=2;
 assert(rdn_cache_get(&ws,256,256,RDN_VISIBLE,false,&at) && at==8192 && dropped==2);
 puts("PASS private/shared cache separation and pending-entry retention");
}
'''
with tempfile.TemporaryDirectory(prefix='rdn-small-cache-') as directory:
    path=Path(directory)
    (path/'test.c').write_text(harness+extract(source,'rdn_cache_get')+cases)
    subprocess.run(['cc','-std=gnu99','-Wall','-Werror','-Wno-unused-variable',
        str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
