"""Fault-inject the production GA submission and synchronization routines."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class GASyncTest(unittest.TestCase):
    def test_failures_do_not_fall_back(self):
        source = (ROOT / "ga/RadeonNIGA.c").read_text()
        start = source.index("static IOReturn ga_gpu_wait(")
        end = source.index("/*\n * Call counters", start)
        harness = r'''
#include <cassert>
#include <cstdint>
#include <cstdarg>
#include <cstring>
using UInt32=uint32_t; using SInt32=int32_t; using IOReturn=int;
enum { kIOReturnSuccess=0, kIOReturnTimeout=-1, kIOReturnBadArgument=-2,
       RDN_UC_FENCE_WAIT=1, RDN_UC_SCREEN_FILL=2, RDN_UC_SCREEN_COPY=3 };
struct Info { UInt32 fb_bits_per_pixel,fb_offset,fb_pitch_pixels,fb_width,fb_height; };
struct ga { int conn; uint8_t *aperture; Info info; int gpu,gpu_pending; IOReturn gpu_error; UInt32 gpu_fence; };
static int error, calls;
static UInt32 reached;
IOReturn IOConnectMethodScalarIScalarO(int,int method,int inputs,int outputs,...) {
    ++calls; va_list ap; va_start(ap,outputs);
    for(int i=0;i<inputs;i++) (void)va_arg(ap,UInt32);
    UInt32 *out=va_arg(ap,UInt32 *); va_end(ap);
    *out=method==RDN_UC_FENCE_WAIT ? reached : 17;
    return error;
}
'''
        harness += source[start:end]
        harness += r'''
int main() {
    uint8_t pixels[64]; memset(pixels,0x5a,sizeof(pixels));
    ga g={0,pixels,{32,0,4,4,4},1,0,0,0};
    assert(ga_fill(&g,0,0,4,4,0)==0 && g.gpu_pending && g.gpu_fence==17);
    assert(ga_gpu_wait(&g)==kIOReturnTimeout && g.gpu_pending);
    reached=1; assert(ga_gpu_wait(&g)==0 && !g.gpu_pending);
    error=-42;
    assert(ga_copy(&g,0,0,1,1,2,2)==-42 && g.gpu_error==-42);
    int before=calls;
    assert(ga_fill(&g,0,0,4,4,0)==-42 && calls==before);
    assert(ga_gpu_wait(&g)==-42);
    g.gpu_pending=1;
    error=0;
    before=calls;
    assert(ga_gpu_wait(&g)==-42 && !g.gpu_pending && calls==before+1);
    for(auto p:pixels) assert(p==0x5a);
    g.gpu=0; g.gpu_error=0; g.gpu_pending=1; error=-42;
    assert(ga_fill(&g,0,0,4,4,0)==-42 && g.gpu_pending);
    for(auto p:pixels) assert(p==0x5a);
    error=0; assert(ga_fill(&g,0,0,4,4,0)==0);
    for(auto p:pixels) assert(p==0);
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
