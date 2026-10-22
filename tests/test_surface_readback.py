"""Exercise the production readback with padded rows and clipped damage."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class ReadbackTest(unittest.TestCase):
    def test_pixels_and_failures(self):
        source = (ROOT / "mesa/frontend/osmesa.c").read_text()
        start = source.index("static bool\nosmesa_read_buffer(")
        end = source.index("\n\n/*", start)
        harness = r'''
#include <cassert>
#include <cstdint>
#include <cstddef>
#include <cstring>
#define MAX2(a,b) ((a) > (b) ? (a) : (b))
#define MIN2(a,b) ((a) < (b) ? (a) : (b))
#define PIPE_MAP_READ 1
using GLint = int;
struct pipe_box { int x,y,width,height; };
struct pipe_resource { unsigned width0,height0; int format; };
struct pipe_transfer { unsigned stride; } transfer = {48};
static uint32_t pixels[6][12];
static pipe_box mapped;
static bool fail;
static int maps, unmaps;
struct pipe_context {
    uint8_t *(*texture_map)(pipe_context *,pipe_resource *,int,int,pipe_box *,pipe_transfer **);
    void (*texture_unmap)(pipe_context *,pipe_transfer *);
};
struct State { pipe_context *pipe; };
struct Context { State *st; int num_rects; GLint *rects; };
using OSMesaContext = Context *;
void u_box_2d(int x,int y,int w,int h,pipe_box *b) { *b={x,y,w,h}; }
unsigned util_format_get_blocksize(int) { return 4; }
uint8_t *map(pipe_context *,pipe_resource *,int,int,pipe_box *b,pipe_transfer **t) {
    ++maps; mapped=*b; *t=&transfer;
    return fail ? nullptr : (uint8_t *)&pixels[b->y][b->x];
}
void unmap(pipe_context *,pipe_transfer *) { ++unmaps; }
'''
        harness += source[start:end]
        harness += r'''
int main() {
    pipe_context pipe={map,unmap}; State st={&pipe};
    int rects[]={-2,1,5,2, 6,4,20,20};
    Context ctx={&st,2,rects}; pipe_resource res={8,6,0};
    uint32_t dst[6][10];
    for(int y=0;y<6;++y) for(int x=0;x<12;++x) pixels[y][x]=100*y+x;
    for(int clipped=0;clipped<2;++clipped) for(int flip=0;flip<2;++flip) {
        memset(dst,0xff,sizeof(dst));
        assert(osmesa_read_buffer(&ctx,&res,dst,40,flip,clipped));
        for(int y=0;y<6;++y) for(int x=0;x<10;++x) {
            bool hit=x<8 && (!clipped || (x<3 && y>=1 && y<3) || (x>=6 && x<8 && y>=4));
            assert(dst[y][x]==(hit ? pixels[flip ? 5-y:y][x] : UINT32_MAX));
        }
        assert(mapped.height==(clipped ? 5:6));
    }
    int small[]={2,1,2,1}; ctx.rects=small; ctx.num_rects=1;
    assert(osmesa_read_buffer(&ctx,&res,dst,40,true,true));
    assert(mapped.x==2 && mapped.y==4 && mapped.width==2 && mapped.height==1);
    small[0]=100; int before=maps;
    assert(osmesa_read_buffer(&ctx,&res,dst,40,false,true)); assert(maps==before);
    fail=true; before=unmaps; memset(dst,0xff,sizeof(dst));
    assert(!osmesa_read_buffer(&ctx,&res,dst,40,false,false));
    assert(unmaps==before && dst[0][0]==UINT32_MAX);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            src = pathlib.Path(directory) / "test.cpp"
            exe = pathlib.Path(directory) / "test"
            src.write_text(harness)
            subprocess.run(["c++", "-std=c++11", "-Wall", "-Wextra", str(src), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    unittest.main()
