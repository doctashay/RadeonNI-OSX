"""Inject mapping failures into Mesa's actual vertex translation function."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
MESA = ROOT / "third_party/mesa-26.2.4/src/gallium/auxiliary/util/u_vbuf.c"

HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define PIPE_MAX_ATTRIBS 16
#define PIPE_MAP_READ 1
#define MAX2(a,b) ((a) > (b) ? (a) : (b))
enum pipe_error { PIPE_OK, PIPE_ERROR_OUT_OF_MEMORY };
struct pipe_resource { unsigned width0; };
struct pipe_transfer { unsigned id; };
struct pipe_vertex_buffer {
   unsigned buffer_offset; bool is_user_buffer;
   union { void *user; struct pipe_resource *resource; } buffer;
};
struct pipe_context { void *stream_uploader; };
struct elements { unsigned strides[PIPE_MAX_ATTRIBS]; };
struct u_vbuf {
   void *translate_cache; struct pipe_context *pipe; struct elements *ve;
   struct pipe_vertex_buffer vertex_buffer[PIPE_MAX_ATTRIBS];
   struct pipe_vertex_buffer real_vertex_buffer[PIPE_MAX_ATTRIBS];
   bool has_signed_vb_offset;
};
struct translate_key { unsigned output_stride; };
struct pipe_draw_info {
   unsigned max_index, index_size; bool has_user_indices;
   union { void *user; struct pipe_resource *resource; } index;
};
struct pipe_draw_start_count_bias { unsigned start, count; };
struct translate {
   void (*set_buffer)(struct translate *,unsigned,const void *,unsigned,unsigned);
   void (*run)(struct translate *,unsigned,unsigned,unsigned,unsigned,void *);
   void (*run_elts)(struct translate *,const unsigned *,unsigned,unsigned,unsigned,void *);
   void (*run_elts16)(struct translate *,const uint16_t *,unsigned,unsigned,unsigned,void *);
   void (*run_elts8)(struct translate *,const uint8_t *,unsigned,unsigned,unsigned,void *);
};
static unsigned maps, unmapped, fail_map, runs;
static bool fail_upload;
static unsigned char data[4096];
static struct pipe_transfer transfers[4];
static struct pipe_resource output = {4096}, released = {4096};
static unsigned u_bit_scan(unsigned *mask) {
   unsigned bit = __builtin_ctz(*mask); *mask &= *mask - 1; return bit;
}
static void *pipe_buffer_map_range(struct pipe_context *pipe, struct pipe_resource *r,
                                  unsigned off, unsigned size, unsigned flags,
                                  struct pipe_transfer **transfer) {
   unsigned id = ++maps;
   assert(id < 4 && off + size <= r->width0);
   if (id == fail_map) { *transfer = NULL; return NULL; }
   transfers[id].id = id; *transfer = &transfers[id]; return data + off;
}
static void pipe_buffer_unmap(struct pipe_context *pipe, struct pipe_transfer *t) {
   assert(!(unmapped & (1u << t->id))); unmapped |= 1u << t->id;
}
static void set_buffer(struct translate *tr, unsigned i, const void *p,
                       unsigned stride, unsigned max) { assert(p); }
static void run(struct translate *tr, unsigned first, unsigned n, unsigned a,
                unsigned b, void *p) { assert(p); runs++; }
static void run_elts(struct translate *tr, const unsigned *indices, unsigned n,
                     unsigned a, unsigned b, void *p) { assert(indices && p); runs++; }
static struct translate translator = {.set_buffer = set_buffer, .run = run,
                                      .run_elts = run_elts};
static struct translate *translate_cache_find(void *cache, struct translate_key *key) {
   return &translator;
}
static void u_upload_alloc(void *u, unsigned min, unsigned size, unsigned align,
                           unsigned *off, struct pipe_resource **out,
                           struct pipe_resource **release, void **map) {
   *off = 0; *out = fail_upload ? NULL : &output;
   *map = fail_upload ? NULL : data; *release = &released;
}
'''

CASES = r'''
int main(void) {
   struct pipe_context pipe = {0};
   struct elements ve = {{4,4}};
   struct pipe_resource input = {4096};
   struct u_vbuf mgr = {.pipe = &pipe, .ve = &ve};
   struct translate_key key = {4};
   struct pipe_draw_info info = {.max_index = 3, .index_size = 4};
   struct pipe_draw_start_count_bias draw = {0,4};
   unsigned scenario;
   mgr.vertex_buffer[0].buffer.resource = &input;
   mgr.vertex_buffer[1].buffer.resource = &input;
   info.index.resource = &input;
   for (scenario = 0; scenario < 6; scenario++) {
      bool indexed = scenario >= 4;
      struct pipe_resource *release = NULL;
      maps = unmapped = runs = 0;
      fail_map = scenario == 1 ? 1 : scenario == 2 ? 2 : scenario == 4 ? 3 : 0;
      fail_upload = scenario == 3;
      mgr.real_vertex_buffer[2].buffer.resource = NULL;
      enum pipe_error result = u_vbuf_translate_buffers(&mgr, &key, &info, &draw,
                                                        3, 2, 0, 4, 0, indexed, &release);
      unsigned expected = ((1u << (maps + 1)) - 2) & ~(fail_map ? 1u << fail_map : 0);
      assert(unmapped == expected);
      if (fail_map || fail_upload) {
         assert(result == PIPE_ERROR_OUT_OF_MEMORY && !runs);
         assert(!mgr.real_vertex_buffer[2].buffer.resource);
      } else {
         assert(result == PIPE_OK && runs == 1);
         assert(mgr.real_vertex_buffer[2].buffer.resource == &output);
      }
   }
   puts("PASS vertex/index map failures, upload failure, cleanup and successful retry");
   return 0;
}
'''


class MapFailureTest(unittest.TestCase):
    def test_translation_failure_cleanup(self):
        source = MESA.read_text()
        begin = source.index("static enum pipe_error\nu_vbuf_translate_buffers(")
        end = source.index("\nstatic bool\nu_vbuf_translate_find_free_vb_slots", begin)
        with tempfile.TemporaryDirectory(prefix="rdn-vbuf-") as directory:
            path = pathlib.Path(directory)
            (path / "test.c").write_text(HARNESS + source[begin:end] + CASES)
            subprocess.run(["cc", "-std=gnu99", "-Wall", "-Werror", "-o",
                            str(path / "test"), str(path / "test.c")], check=True)
            subprocess.run([str(path / "test")], check=True)


if __name__ == "__main__":
    unittest.main()
