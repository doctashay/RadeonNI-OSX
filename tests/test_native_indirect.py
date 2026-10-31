"""Exercise the native indirect decoder before emitting direct GPU draws."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class NativeIndirectTest(unittest.TestCase):
    def test_native_draws(self):
        patch = (ROOT / "mesa/patches/0105-r600-expand-native-indirect-draws.patch").read_text()
        source = "\n".join(line[1:] for line in patch.splitlines()
                           if line.startswith("+") and not line.startswith("+++"))
        start = source.index("static void r600_draw_native_indirect(")
        decoder = source[start:source.index("\n}", start) + 2]
        harness = r"""
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define MIN2(a,b) ((a)<(b)?(a):(b))
#define PIPE_MAP_READ 1
struct pipe_resource {unsigned width0;uint32_t data[128];};
struct pipe_transfer {int unused;};
struct pipe_draw_info {unsigned index_size,instance_count,start_instance;};
struct pipe_draw_start_count_bias {unsigned count,start;int index_bias;};
struct pipe_draw_indirect_info {struct pipe_resource *buffer,*indirect_draw_count;unsigned offset,stride,draw_count,indirect_draw_count_offset;};
struct pipe_context {void (*draw_vbo)(struct pipe_context *,const struct pipe_draw_info *,unsigned,const void *,const struct pipe_draw_start_count_bias *,unsigned);};
static unsigned maps,calls,fail_map;
static struct pipe_draw_start_count_bias result[8];
static struct pipe_draw_info infos[8];
static unsigned ids[8];
struct r600_common_context {int unused;};
struct r600_context {struct r600_common_context b;};
#define r600_as_resource(r) (r)
static void *r600_buffer_map_sync_with_rings(struct r600_common_context *c,struct pipe_resource *r,unsigned flags) {
 (void)c;assert(flags==PIPE_MAP_READ);maps++;
 if(fail_map==maps)return NULL;
 return r->data;
}
static void draw(struct pipe_context *c,const struct pipe_draw_info *i,unsigned id,const void *ind,const struct pipe_draw_start_count_bias *d,unsigned n) {
 (void)c;assert(!ind && n==1 && calls<8);result[calls]=*d;infos[calls]=*i;ids[calls++]=id;
}
""" + decoder + r"""
int main(void) {
 struct pipe_context ctx={.draw_vbo=draw};struct pipe_draw_info info={.index_size=4};
 struct pipe_resource src={.width0=72},counter={.width0=16};
 uint32_t commands[13]={3492,1,0,-3,7,0,0,0,3540,2,3,5,9};
 memcpy(src.data+4,commands,sizeof(commands));counter.data[3]=9;
 uint32_t before[128];memcpy(before,src.data,sizeof(before));
 struct pipe_draw_indirect_info ind={.buffer=&src,.offset=16,.stride=32,.draw_count=2,.indirect_draw_count=&counter,.indirect_draw_count_offset=12};
 r600_draw_native_indirect(&ctx,&info,5,&ind);
 assert(calls==2 && result[0].count==3492 && result[0].index_bias==-3 && result[1].count==3540 && result[1].start==3 && result[1].index_bias==5);
 assert(infos[0].instance_count==1 && infos[1].instance_count==2 && infos[0].start_instance==7 && infos[1].start_instance==9 && ids[0]==5 && ids[1]==6);
 assert(!memcmp(before,src.data,sizeof(before)));
 calls=0;counter.data[3]=1;r600_draw_native_indirect(&ctx,&info,0,&ind);assert(calls==1);
 calls=0;counter.data[3]=0;r600_draw_native_indirect(&ctx,&info,0,&ind);assert(!calls);
 ind.indirect_draw_count=NULL;ind.stride=0;r600_draw_native_indirect(&ctx,&info,0,&ind);assert(calls==2 && result[1].count==3492);
 calls=0;info.index_size=0;src.data[7]=11;r600_draw_native_indirect(&ctx,&info,8,&ind);assert(calls==2 && !result[0].index_bias && infos[0].start_instance==11 && ids[1]==9);
 calls=0;ind.draw_count=0;r600_draw_native_indirect(&ctx,&info,0,&ind);
 ind.draw_count=2;ind.offset=3;r600_draw_native_indirect(&ctx,&info,0,&ind);
 ind.offset=16;ind.stride=UINT32_MAX-3;r600_draw_native_indirect(&ctx,&info,0,&ind);
 ind.stride=32;ind.offset=64;r600_draw_native_indirect(&ctx,&info,0,&ind);
 ind.offset=16;ind.indirect_draw_count=&counter;ind.indirect_draw_count_offset=14;r600_draw_native_indirect(&ctx,&info,0,&ind);
 ind.indirect_draw_count_offset=12;fail_map=maps+1;r600_draw_native_indirect(&ctx,&info,0,&ind);
 ind.indirect_draw_count=NULL;fail_map=maps+1;r600_draw_native_indirect(&ctx,&info,0,&ind);
 assert(!calls);
 return 0;
}
"""
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory)
            (path / "test.c").write_text(harness)
            subprocess.run(["cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                            str(path / "test.c"), "-o", str(path / "test")], check=True)
            subprocess.run([str(path / "test")], check=True)


if __name__ == "__main__":
    unittest.main()
