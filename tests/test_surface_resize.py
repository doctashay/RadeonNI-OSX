"""Exercise backing retirement while the GL worker still owns queued draws."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]

def function(source, signature):
    start = source.rindex(signature)
    return source[start:source.index("\n}", start) + 2]

class SurfaceResizeTest(unittest.TestCase):
    def test_queued_draws_retire_before_storage(self):
        source = (ROOT / "gld/rdn_mesa.c").read_text()
        start = source.index("struct surface_store {")
        end = source.index("static pthread_mutex_t surface_store_lock", start)
        harness = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
typedef void *OSMesaContext;
#define GL_TRUE 1
#define GL_FALSE 0
#define rdn_log(...) ((void)0)
#define RDN_TARGET_VRAM_HANDLE 1
struct context {void *mesa; uint32_t surface,stored,store_new,store_offset,store_row_bytes,store_width,store_height;};
static pthread_mutex_t surface_store_lock=PTHREAD_MUTEX_INITIALIZER;
static bool pending;
static unsigned allocated,freed,drains,copies;
static uint32_t published[5];
static bool query_failed;
static void OSMesaFlushRender(void *mesa,int wait) {assert(mesa);if(wait){pending=false;drains++;}}
static int OSMesaClearStoreImage(void *m,unsigned h,unsigned s,unsigned o,unsigned w,unsigned n) {(void)m;(void)h;(void)s;(void)o;(void)w;(void)n;return 1;}
static bool rdn_target_surface_query(uint32_t id,uint32_t info[5],bool *found) {*found=true;memcpy(info,published,sizeof(published));info[0]=id;return !query_failed;}
static bool rdn_target_surface_buffer(uint32_t id,uint32_t off,uint32_t stride,uint32_t w,uint32_t h) {if(!off)assert(!pending);published[0]=id;published[1]=off;published[2]=stride;published[3]=w;published[4]=h;return true;}
static bool rdn_target_vram_alloc(uint32_t bytes,uint32_t *off) {assert(bytes);*off=++allocated*4096;return true;}
static void rdn_target_vram_free(uint32_t off) {assert(off && !pending);freed++;}
static OSMesaContext OSMesaGetCurrentContext(void) {return NULL;}
static int rdn_target_surface_gpu_copy(uint32_t id,uint32_t off,uint32_t stride,uint32_t w,uint32_t h) {(void)id;(void)off;(void)stride;(void)w;(void)h;copies++;return 0;}
static void *rdn_target_vram_map(uint32_t off) {(void)off;static char data[65536];return data;}
"""
        harness += source[start:end]
        harness += function(source, "static void clear_surface_store(") + "\n"
        harness += function(source, "static void surface_store(") + "\n"
        harness += function(source, "static int backing_store(struct context *c, void *backing, uint32_t stride,\n                         uint32_t width") + "\n"
        harness += r"""
int main(void) {
 struct context c={.mesa=(void *)1,.surface=1};
 surface_store(&c,640,400,1);assert(c.stored && !drains);
 pending=true;surface_store(&c,640,400,1);assert(pending && !drains && !freed);
 surface_store(&c,1920,971,1);assert(c.stored && !pending && drains==1 && freed==1);
 pending=true;surface_store(&c,800,450,1);assert(c.stored && !pending && drains==2 && freed==2);
 pending=true;surface_store(&c,640,400,1);assert(c.stored && !pending && drains==3 && freed==3);
 assert(backing_store(&c,NULL,2560,640,400,1,1));assert(!copies);
 struct context fresh={.mesa=(void *)1,.surface=2};
 assert(backing_store(&fresh,NULL,128,32,16,1,1));assert(copies==1);
 assert(backing_store(&fresh,NULL,128,32,16,1,1));assert(copies==1);
 published[1]+=4096;
 assert(backing_store(&fresh,NULL,128,32,16,1,1));assert(copies==2);
 query_failed=true;
 assert(backing_store(&fresh,NULL,128,32,16,1,1));assert(copies==3);
}
"""
        with tempfile.TemporaryDirectory() as directory:
            path=pathlib.Path(directory)
            (path/"test.c").write_text(harness)
            subprocess.run(["cc","-std=c99","-Wall","-Wextra","-pthread",str(path/"test.c"),"-o",str(path/"test")],check=True)
            subprocess.run([str(path/"test")],check=True)

if __name__ == "__main__":
    unittest.main()
