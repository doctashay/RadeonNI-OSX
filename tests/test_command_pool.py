"""Reserved command allocations must never enter or consume the drawable cache."""
import pathlib,subprocess,tempfile,unittest
from test_winsys_sync import extract
ROOT=pathlib.Path(__file__).resolve().parents[1]
HARNESS=r"""
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
#define RDN_VISIBLE 0
struct dev {int (*alloc_command)(struct dev*,uint64_t,uint64_t,uint64_t*);int (*alloc)(struct dev*,uint64_t,uint64_t,uint64_t*);};
struct radeon_drm_winsys {struct dev *dev;};
static unsigned reserved,normal,cached,waited;static bool full,throttled;
static void rdn_reap_ibs(struct radeon_drm_winsys*w,bool wait){waited+=wait;}
static void rdn_cache_trim(struct radeon_drm_winsys*w,bool all){}
static int rdn_throttle(struct radeon_drm_winsys*w){return throttled?-1:0;}
static bool rdn_cache_get(struct radeon_drm_winsys*w,uint64_t s,unsigned a,unsigned r,bool allow_small,uint64_t*o){cached++;return false;}
static int alloc_command(struct dev*d,uint64_t s,uint64_t a,uint64_t*o){reserved++;*o=0x2200000;return full?-1:0;}
static int alloc_normal(struct dev*d,uint64_t s,uint64_t a,uint64_t*o){normal++;return -1;}
"""
CASES=r"""
int main(void){struct dev d={alloc_command,alloc_normal};struct radeon_drm_winsys ws={&d};uint64_t offset;
assert(!rdn_ib_alloc(&ws,65536,&offset)&&reserved==1&&!normal&&!cached);
full=true;assert(rdn_ib_alloc(&ws,65536,&offset)&&reserved==3&&waited==1&&!normal&&!cached);
throttled=true;assert(rdn_ib_alloc(&ws,65536,&offset)&&reserved==3);
throttled=false;d.alloc_command=0;assert(rdn_ib_alloc(&ws,65536,&offset)&&normal==2&&cached==1);
puts("PASS command reserve is isolated, retries retirement, and preserves old-kernel fallback");return 0;}
"""
class Commands(unittest.TestCase):
 def test_allocator(self):
  body=HARNESS+extract((ROOT/'mesa/winsys/rdn_winsys.c').read_text(),'rdn_ib_alloc')+CASES
  with tempfile.TemporaryDirectory() as d:
   p=pathlib.Path(d);(p/'t.c').write_text(body)
   subprocess.run(['cc','-std=gnu99','-Wall','-Werror',str(p/'t.c'),'-o',str(p/'t')],check=True)
   subprocess.run([str(p/'t')],check=True)
if __name__=='__main__':unittest.main()
