"""Exercise private VRAM slab packing and parent lifetime with the real allocator."""
from pathlib import Path
import subprocess
import tempfile
from test_winsys_sync import extract

root = Path(__file__).resolve().parents[1]
source = (root / 'mesa/target/rdn_device_darwin.c').read_text()
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include "rdn_mem.h"
#define RDN_UC_FREE 1
struct rdn_device { int unused; };
struct small_slab { struct small_slab *next; struct rdn_mem mem; };
struct darwin_device { struct rdn_device base; struct small_slab *small_slabs; struct rdn_os mem_os; int conn; };
static unsigned allocs, frees;
static bool fail;
static void *allocate(void *cookie,size_t bytes) { return calloc(1,bytes); }
static void release(void *cookie,void *p) { free(p); }
static int dev_alloc(struct rdn_device *dev,uint64_t size,uint64_t align,uint64_t *out) {
 assert(size==65536 && align==4096);
 if (fail) return -1;
 *out=0x100000+(uint64_t)allocs++*65536; return 0;
}
static int IOConnectMethodScalarIScalarO(int c,int method,int in,int out,int offset) {
 assert(method==RDN_UC_FREE && !(offset&65535)); frees++; return 0;
}
'''
cases = r'''
int main(void) {
 struct darwin_device d={0}; uint64_t offsets[257], at;
 d.mem_os.alloc=allocate; d.mem_os.free=release;
 fail=true; assert(dev_alloc_small(&d.base,256,256,&at)); assert(!d.small_slabs);
 fail=false;
 assert(dev_alloc_small(&d.base,4097,256,&at));
 assert(dev_alloc_small(&d.base,256,8192,&at));
 for(unsigned i=0;i<257;i++) {
  assert(!dev_alloc_small(&d.base,256,256,&offsets[i]));
  assert(!(offsets[i]&255) && dev_is_small(&d.base,offsets[i]));
  for(unsigned j=0;j<i;j++) assert(offsets[i]!=offsets[j]);
 }
 assert(allocs==2 && !frees);
 for(unsigned i=1;i<256;i++) dev_free(&d.base,offsets[i]);
 assert(!frees); /* One live entry pins the whole first parent. */
 assert(!dev_alloc_small(&d.base,4096,4096,&at) && !(at&4095));
 assert(allocs==2); dev_free(&d.base,at);
 dev_free(&d.base,offsets[0]); assert(frees==1);
 dev_free(&d.base,offsets[256]); assert(frees==2 && !d.small_slabs);
 puts("PASS 257 distinct entries, 2 kernel allocations, alignment, reuse, failure and parent lifetime");
}
'''
with tempfile.TemporaryDirectory(prefix='rdn-small-slabs-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(prefix + ''.join(extract(source, name) for name in
        ['dev_alloc_small', 'dev_is_small', 'dev_free']) + cases)
    subprocess.run(['cc', '-std=gnu99', '-Wall', '-Werror', '-I'+str(root/'hw'),
        str(path/'test.c'), str(root/'hw/rdn_mem.c'), '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True)
