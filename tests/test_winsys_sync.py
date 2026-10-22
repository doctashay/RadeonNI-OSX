"""Exercise actual winsys submission, map and retirement paths with fence failures."""
import os
import pathlib
import re
import shlex
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


def extract(source, name):
    matches = re.finditer(r"(?m)^static [\w *]+\b" + name + r"\s*\(", source)
    match = next((m for m in matches if ";" not in source[m.end():source.index("{", m.end())]), None)
    if not match:
        raise AssertionError(name)
    start = source.index("{", match.end())
    depth = 0
    tokens = re.finditer(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]',
                         source[start:], re.S)
    for token in tokens:
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if not depth:
                return source[match.start():start + token.end()] + "\n"
    raise AssertionError(name + " has no closing brace")


HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <syslog.h>
#define CALLOC_STRUCT(t) calloc(1,sizeof(struct t))
#define MALLOC malloc
#define FREE free
#define MAX2(a,b) ((a)>(b)?(a):(b))
#define OS_TIMEOUT_INFINITE UINT64_MAX
#define RADEON_USAGE_READ 1
#define RADEON_USAGE_WRITE 2
#define RADEON_USAGE_READWRITE 3
#define RADEON_FLAG_NO_CPU_ACCESS 1
#define RADEON_FLUSH_START_NEXT_GFX_IB_NOW 1
#define RADEON_FLUSH_ASYNC_START_NEXT_GFX_IB_NOW 2
#define CHECK(x) do { assert(x);checks++; } while(0)
enum pipe_map_flags { PIPE_MAP_READ=1, PIPE_MAP_WRITE=2, PIPE_MAP_UNSYNCHRONIZED=4,
                      PIPE_MAP_DONTBLOCK=8 };
enum rdn_place { RDN_VISIBLE,RDN_HIDDEN,RDN_GART,RDN_USER,RDN_SEGMENT };
struct list_head { int dummy; };
struct rdn_busy { uint32_t number; bool used,uvd,unknown; };
struct pb_buffer_lean { uint64_t size; };
struct rdn_bo { struct pb_buffer_lean base; uint64_t offset; unsigned flags;
   struct rdn_busy last_use,last_write,last_uvd; void *external_store,*user;
   bool foreign; enum rdn_place region; uint8_t *shadow; };
struct radeon_winsys { int unused; };
struct rdn_device {
   void *aperture; struct { uint64_t vram_gpu_base; } info;
   int (*submit)(struct rdn_device *,uint64_t,uint32_t,uint32_t *);
   int (*uvd_submit)(struct rdn_device *,uint64_t,uint32_t,uint32_t *);
   void (*free)(struct rdn_device *,uint64_t);
   void (*sync_for_cpu)(struct rdn_device *);
   void *(*gart_cpu)(struct rdn_device *,uint64_t);
   void (*release_external)(struct rdn_device *,void *);
};
struct radeon_drm_winsys { struct radeon_winsys base; int lock; struct rdn_device *dev;
   bool uvd_seg,uvd_split,uvd_used,uvd_check; uint64_t uvd_msg_at,uvd_fb_at;
   uint32_t uvd_msg_type,uvd_msg_num; uint64_t num_flushes,allocated_bytes;
   struct list_head pending_ibs; };
struct rdn_fence { unsigned refs; uint32_t number; bool uvd,reached,submitted,failed; };
struct pipe_fence_handle { int unused; };
struct rdn_cs_buffer { struct rdn_bo *bo; unsigned usage,slot; };
struct rdn_cs { struct radeon_drm_winsys *ws; bool uvd; unsigned num_buffers;
   struct rdn_cs_buffer buffers[2]; int hash[2];
   void (*flush_cs)(void *,unsigned,struct pipe_fence_handle **); void *flush_data;
   struct rdn_fence *next_fence,*error_fence; };
struct radeon_cmdbuf { void *priv; struct { unsigned cdw; uint32_t *buf; } current;
   unsigned used_vram_kb,used_gart_kb; };
struct rdn_pending_ib { struct list_head list; uint64_t offset,size; struct rdn_busy busy; };
struct rdn_uvd_dbg { unsigned n,cmd[8],offset[8]; struct rdn_bo *bo[8]; };
static struct { uint64_t flushes,bytes; unsigned most_buffers; } rdn_cs_stats;
static struct radeon_drm_winsys ws;
static struct rdn_cs cs;
static struct rdn_bo bo[2];
static struct radeon_cmdbuf stream;
static struct rdn_device dev;
static struct rdn_fence error_fence={.refs=1,.failed=true,.reached=true};
static uint8_t aperture[65536];
static uint32_t words[64];
static unsigned checks,waits,submits,allocs,releases,cached,synced,external_releases;
static unsigned fail_number,mutate_number,callback_calls,quarantined_ibs,device_frees;
static struct rdn_busy observed[16];
static bool publish_pending,submit_error,keep_callback_refs,relocate_ok;
static struct radeon_drm_winsys *rdn_winsys(struct radeon_winsys *w) { return &ws; }
static struct rdn_bo *rdn_bo(struct pb_buffer_lean *b) { return (struct rdn_bo *)b; }
static void simple_mtx_lock(int *p) { CHECK(!*p);*p=1; }
static void simple_mtx_unlock(int *p) {
   CHECK(*p);
   if(publish_pending) {
      for(unsigned i=0;i<cs.num_buffers;i++) {
         struct rdn_busy b=cs.uvd?cs.buffers[i].bo->last_uvd:cs.buffers[i].bo->last_use;
         CHECK(b.used && b.number==77 && b.uvd==cs.uvd);
      }
      publish_pending=false;
   }
   *p=0;
}
static bool rdn_number_wait(struct radeon_drm_winsys *w,uint32_t number,bool uvd,uint64_t timeout) {
   CHECK(!w->lock && waits<16);observed[waits++]=(struct rdn_busy){number,true,uvd,false};
   if(mutate_number) { bo[0].last_uvd.number=mutate_number;mutate_number=0; }
   return number!=fail_number;
}
static uint64_t align64(uint64_t n,uint64_t a) { return (n+a-1)&~(a-1); }
static int rdn_ib_alloc(struct radeon_drm_winsys *w,uint64_t size,uint64_t *offset) {
   CHECK(w->lock);allocs++;*offset=0;return 0;
}
static int submit(struct rdn_device *d,uint64_t offset,uint32_t count,uint32_t *fence) {
   CHECK(ws.lock);submits++;if(submit_error)return -EIO;
   *fence=77;publish_pending=true;return 0;
}
static void dev_free(struct rdn_device *d,uint64_t offset) { CHECK(ws.lock);device_frees++; }
static void sync_cpu(struct rdn_device *d) { CHECK(!ws.lock);synced++; }
static void *gart_cpu(struct rdn_device *d,uint64_t offset) { return aperture+offset; }
static void release_external(struct rdn_device *d,void *p) { CHECK(!ws.lock);external_releases++; }
static void rdn_cache_put(struct radeon_drm_winsys *w,uint64_t offset,uint64_t size,
                          enum rdn_place region,const struct rdn_busy *busy) { CHECK(w->lock);cached++; }
static void rdn_cache_trim(struct radeon_drm_winsys *w,bool all) { CHECK(w->lock); }
static void rdn_mem_stats_account(struct radeon_drm_winsys *w,uint64_t s,enum rdn_place p,int d) {}
static struct rdn_fence *rdn_fence_new(void) {
   struct rdn_fence *f=calloc(1,sizeof(*f));assert(f);f->refs=1;return f;
}
static void rdn_fence_set(struct rdn_fence **p,struct rdn_fence *f) {
   if(f)f->refs++;
   if(*p && !--(*p)->refs)free(*p);
   *p=f;
}
static void radeon_bo_reference(struct radeon_winsys *w,struct pb_buffer_lean **p,void *n) {
   CHECK(!ws.lock && n==NULL);releases++;*p=NULL;
}
static void list_addtail(struct list_head *p,struct list_head *head) {
   CHECK(ws.lock);
   if(((struct rdn_pending_ib *)p)->busy.unknown)quarantined_ibs++;
   free(p);
}
static int rdn_sync_mode(void) { return 0; }
static bool rdn_uvd_debug(void) { return false; }
static bool rdn_uvd_relocate(struct rdn_cs *c,struct radeon_cmdbuf *r,struct rdn_uvd_dbg *d) { return relocate_ok; }
static int rdn_uvd_upload_dirty(struct radeon_drm_winsys *w,struct rdn_cs *c) { CHECK(w->lock);return 0; }
static int rdn_uvd_segment_submit(struct radeon_drm_winsys *w,struct rdn_cs *c,
                                struct radeon_cmdbuf *r,struct rdn_uvd_dbg *d,struct rdn_fence *f) {
   return submit(w->dev,0,r->current.cdw,&f->number);
}
static uint32_t rdn_le32_at(const void *p) { return 0; }
static void *rdn_segment_shadow(struct radeon_drm_winsys *w,struct rdn_bo *b,enum pipe_map_flags f) { return b->shadow; }
static bool rdn_cs_is_buffer_referenced(struct radeon_cmdbuf *r,struct pb_buffer_lean *b,unsigned usage) {
   struct rdn_cs *c=r->priv;
   for(unsigned i=0;i<c->num_buffers;i++)
      if(&c->buffers[i].bo->base==b && (c->buffers[i].usage & usage))return true;
   return false;
}
static size_t rdn_trace_words(const uint32_t *p,unsigned n,bool complete) { return 0; }
static bool rdn_trace_encode(const uint32_t *p,unsigned n,uint32_t *d,unsigned count,uint64_t addr,bool complete) { return false; }
static FILE *rdn_capture_submission(struct radeon_cmdbuf *r,uint64_t o,const uint32_t *p,unsigned n,uint64_t m) { return NULL; }
static void rdn_capture_result(FILE *f,int r,uint32_t n) {}
static unsigned rdn_complete_packet_words(const uint32_t *p,unsigned n) { return n; }
static int rdn_cs_flush(struct radeon_cmdbuf *,unsigned,struct pipe_fence_handle **);
'''

CASES = r'''
static void callback(void *data,unsigned flags,struct pipe_fence_handle **fence) {
   callback_calls++;
   if(!keep_callback_refs)rdn_cs_flush(data,flags,fence);
}
static void reset(bool uvd) {
   CHECK(!ws.lock);memset(&ws,0,sizeof(ws));memset(&cs,0,sizeof(cs));memset(bo,0,sizeof(bo));
   memset(&stream,0,sizeof(stream));memset(words,0,sizeof(words));
   waits=submits=allocs=releases=cached=synced=external_releases=0;
   fail_number=mutate_number=callback_calls=quarantined_ibs=device_frees=0;publish_pending=submit_error=keep_callback_refs=false;relocate_ok=true;
   dev=(struct rdn_device){.aperture=aperture,.submit=submit,.uvd_submit=submit,.free=dev_free,
                          .sync_for_cpu=sync_cpu,.gart_cpu=gart_cpu,.release_external=release_external};
   ws.dev=&dev;ws.allocated_bytes=4096;cs.ws=&ws;cs.uvd=uvd;cs.num_buffers=2;cs.error_fence=&error_fence;
   for(unsigned i=0;i<2;i++) {
      bo[i].base.size=2048;bo[i].region=RDN_VISIBLE;
      bo[i].last_use=(struct rdn_busy){10+i,true,false};
      bo[i].last_write=(struct rdn_busy){20+i,true,false};
      bo[i].last_uvd=(struct rdn_busy){30+i,true,true};
      cs.buffers[i]=(struct rdn_cs_buffer){&bo[i],i?RADEON_USAGE_WRITE:RADEON_USAGE_READ,i};cs.hash[i]=i;
   }
   stream.priv=&cs;stream.current.buf=words;stream.current.cdw=1;
}
int main(void) {
   unsetenv("RDN_CAPTURE_DIR");unsetenv("RDN_CAPTURE_MARKERS");
   /* Snapshot both fence domains before the first wait can yield. */
   reset(false);mutate_number=999;
   CHECK(rdn_buffer_wait(NULL,&bo[0].base,0,RADEON_USAGE_READ));
   CHECK(waits==2 && observed[0].number==20 && observed[1].number==30 && observed[1].uvd);
   reset(false);fail_number=30;
   CHECK(!rdn_buffer_wait(NULL,&bo[0].base,0,RADEON_USAGE_WRITE));
   CHECK(observed[0].number==10 && waits==2);
   /* Execute both full flush functions. Failure retains commands and all references. */
   for(unsigned engine=0;engine<2;engine++) {
      for(unsigned failure=0;failure<3;failure++) {
         reset(engine!=0);
         if(failure)fail_number=engine?(failure==1?20:11):(failure==1?30:31);
         struct rdn_fence *old=rdn_fence_new();old->reached=true;
         struct pipe_fence_handle *returned=(struct pipe_fence_handle *)old;
         struct rdn_fence *deferred=NULL;
         if(failure) {cs.next_fence=rdn_fence_new();rdn_fence_set(&deferred,cs.next_fence);}
         int result=rdn_cs_flush(&stream,0,&returned);
         if(failure) {
            CHECK(result==-EIO && !submits && !allocs && !releases);
            CHECK(cs.num_buffers==2 && stream.current.cdw==1 && cs.hash[0]==0 && cs.hash[1]==1);
            CHECK(bo[0].last_use.number==10 && bo[0].last_uvd.number==30);
            CHECK(returned && !rdn_fence_wait(&ws,(struct rdn_fence *)returned,0));
            CHECK(deferred && !rdn_fence_wait(&ws,deferred,0) && !cs.next_fence);
         } else {
            CHECK(!result && submits==1 && releases==2 && !cs.num_buffers && !stream.current.cdw);
            CHECK(!publish_pending && cs.hash[0]==-1 && cs.hash[1]==-1);
            if(engine) CHECK(bo[0].last_use.number==10 && bo[0].last_write.number==20 && bo[0].last_uvd.number==77);
            else CHECK(bo[0].last_use.number==77 && bo[0].last_write.number==20 && bo[1].last_write.number==77 && bo[0].last_uvd.number==30);
         }
         rdn_fence_set((struct rdn_fence **)&returned,NULL);
         rdn_fence_set(&deferred,NULL);
      }
   }
   reset(true);ws.uvd_seg=true;
   CHECK(!rdn_cs_flush(&stream,0,NULL) && !allocs && submits==1 && releases==2 && bo[0].last_uvd.number==77);
   reset(true);relocate_ok=false;
   CHECK(rdn_cs_flush(&stream,0,NULL)==-EINVAL && !submits && releases==2 && bo[0].last_uvd.number==30);
   for(unsigned engine=0;engine<2;engine++) {
      reset(engine!=0);submit_error=true;
      struct pipe_fence_handle *fence=NULL;
      CHECK(rdn_cs_flush(&stream,0,&fence)==-EIO && submits==1 && releases==2);
      CHECK(fence && !rdn_fence_wait(&ws,(struct rdn_fence *)fence,0));
      CHECK(((struct rdn_fence *)fence)->failed && quarantined_ibs==1 && !device_frees);
      CHECK(engine?bo[0].last_uvd.unknown:bo[0].last_use.unknown);
      CHECK(!rdn_busy_wait(&ws,engine?&bo[0].last_uvd:&bo[0].last_use,0));
      rdn_fence_set((struct rdn_fence **)&fence,NULL);
      /* Even a same-engine re-use must reject unknown prior completion. */
      cs.num_buffers=1;cs.buffers[0].bo=&bo[0];stream.current.cdw=1;
      unsigned previous=submits;
      CHECK(rdn_cs_flush(&stream,0,NULL)==-EIO && submits==previous);
   }
   /* A raw UVD stream can map without a driver callback; failures never expose bytes. */
   reset(true);
   CHECK(rdn_buffer_map(NULL,&bo[1].base,&stream,PIPE_MAP_READ)==aperture);
   CHECK(submits==1 && releases==2 && synced==1);
   reset(true);fail_number=20;
   CHECK(!rdn_buffer_map(NULL,&bo[1].base,&stream,PIPE_MAP_READ));
   CHECK(!submits && !synced && cs.num_buffers==2);
   reset(false);cs.flush_cs=callback;cs.flush_data=&stream;keep_callback_refs=true;
   CHECK(!rdn_buffer_map(NULL,&bo[1].base,&stream,PIPE_MAP_READ) && callback_calls==1 && !synced);
   reset(false);cs.flush_cs=callback;cs.flush_data=&stream;
   CHECK(rdn_buffer_map(NULL,&bo[1].base,&stream,PIPE_MAP_READ)==aperture && callback_calls==1 && synced==1);
   reset(false);fail_number=31;
   CHECK(!rdn_buffer_map(NULL,&bo[1].base,NULL,PIPE_MAP_READ) && !synced);
   reset(false);CHECK(!rdn_buffer_map(NULL,&bo[1].base,&stream,PIPE_MAP_DONTBLOCK|PIPE_MAP_READ));
   CHECK(submits==1 && !synced);
   reset(false);fail_number=31;
   CHECK(rdn_buffer_map(NULL,&bo[1].base,&stream,PIPE_MAP_UNSYNCHRONIZED|PIPE_MAP_READ)==aperture && !waits);
   /* Unknown UVD completion pins all storage classes, including imports and user pages. */
   for(unsigned kind=0;kind<4;kind++) {
      reset(false);struct rdn_bo *b=calloc(1,sizeof(*b));assert(b);*b=bo[0];
      b->shadow=malloc(8);assert(b->shadow);b->shadow[0]=42;
      if(kind==1)b->user=aperture;
      if(kind==2){b->foreign=true;b->external_store=aperture;}
      if(kind==3)b->region=RDN_SEGMENT;
      fail_number=30;rdn_buffer_destroy(NULL,&b->base);
      CHECK(!cached && !external_releases && ws.allocated_bytes==4096 && b->shadow[0]==42);
      fail_number=0;rdn_buffer_destroy(NULL,&b->base);
      CHECK(kind==2?external_releases==1:cached==1);
   }
   reset(false);struct rdn_bo *b=calloc(1,sizeof(*b));assert(b);*b=bo[0];
   b->foreign=true;b->external_store=aperture;fail_number=10;
   rdn_buffer_destroy(NULL,&b->base);CHECK(!external_releases && !cached);
   fail_number=0;rdn_buffer_destroy(NULL,&b->base);CHECK(external_releases==1);
   printf("PASS %u checks: actual GFX/UVD flush, map, fence publication and failed-completion retirement\n",checks);
   return 0;
}
'''


class WinsysSyncTest(unittest.TestCase):
    def test_actual_submission_and_retirement(self):
        source = (ROOT / "mesa/winsys/rdn_winsys.c").read_text()
        names = ["rdn_busy_wait", "rdn_fence_wait", "rdn_buffer_wait", "rdn_cs_flush_for_map", "rdn_buffer_map",
                 "rdn_cs_mark_busy", "rdn_cs_fail_fence", "rdn_cs_wait_dependencies", "rdn_cs_release_buffers",
                 "rdn_uvd_cs_flush", "rdn_cs_flush", "rdn_buffer_destroy"]
        body = HARNESS + "\n".join(extract(source, name) for name in names) + CASES
        with tempfile.TemporaryDirectory(prefix="rdn-winsys-sync-") as directory:
            path = pathlib.Path(directory)
            (path / "test.c").write_text(body)
            cc = shlex.split(os.environ.get("CC", "cc"))
            flags = ["-std=gnu99", "-Wall", "-Werror", "-Wno-unused-function"]
            flags += shlex.split(os.environ.get("CFLAGS", ""))
            subprocess.run(cc + flags + [str(path / "test.c"), "-o", str(path / "test")], check=True)
            subprocess.run(shlex.split(os.environ.get("TEST_RUNNER", "")) + [str(path / "test")], check=True)


if __name__ == "__main__":
    unittest.main()
