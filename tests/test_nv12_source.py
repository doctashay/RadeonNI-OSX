"""Check the production NV12 source-span validator without a GPU."""
import os
import pathlib
import re
import shlex
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
CASES = r'''
static unsigned checks;
#define CHECK(x) do { assert(x);checks++; } while(0)
static uint32_t round256(uint32_t n) { return (n+255)&~255u; }
static struct OSMesaNV12Source layout(unsigned w,unsigned h,unsigned pitch) {
   struct OSMesaNV12Source s={.handle=0x52444e31,.pitch=pitch,.width=w,.height=h};
   s.y_bottom=round256(pitch*(h/2));
   s.uv_top=round256(s.y_bottom+pitch*(h/2));
   s.uv_bottom=round256(s.uv_top+pitch*((h/2+1)/2));
   s.bytes=s.uv_bottom+pitch*(h/4-1)+w;
   return s;
}
int main(void) {
   CHECK(!rdn_nv12_source_valid(NULL));
   for(unsigned n=0;n<10000;n++) {
      unsigned w=2+2*(n%1024),h=4+2*((n*37)%1023),pitch=(w+31)&~7u;
      struct OSMesaNV12Source s=layout(w,h,pitch),bad=s;
      CHECK(rdn_nv12_source_valid(&s));
      bad.bytes--;CHECK(!rdn_nv12_source_valid(&bad));
      bad=s;bad.uv_bottom=bad.uv_top;CHECK(!rdn_nv12_source_valid(&bad));
      bad=s;bad.y_bottom++;CHECK(!rdn_nv12_source_valid(&bad));
      bad=s;bad.pitch++;CHECK(!rdn_nv12_source_valid(&bad));
      bad=s;bad.width++;CHECK(!rdn_nv12_source_valid(&bad));
      bad=s;bad.height++;CHECK(!rdn_nv12_source_valid(&bad));
      bad=s;bad.offset=0xffffff00;CHECK(!rdn_nv12_source_valid(&bad));
      bad=s;bad.handle=0;CHECK(!rdn_nv12_source_valid(&bad));
   }
   struct OSMesaNV12Source s=layout(64,64,64);
   s.bytes=round256(s.bytes);s.offset=UINT32_MAX-s.bytes+1;
   CHECK(rdn_nv12_source_valid(&s));
   s.bytes++;CHECK(!rdn_nv12_source_valid(&s));
   s=layout(64,64,64);s.pitch=0xfffffff8u;CHECK(!rdn_nv12_source_valid(&s));
   s=layout(64,64,64);s.height=2;CHECK(!rdn_nv12_source_valid(&s));
   s=layout(64,64,64);s.width=8194;CHECK(!rdn_nv12_source_valid(&s));
   printf("PASS %u NV12 span checks, including odd chroma height, final-byte bounds and 32-bit wrap\n",checks);
   return 0;
}
'''

class SourceBoundsTest(unittest.TestCase):
    def test_source_bounds(self):
        header = (ROOT / "mesa/frontend/include/GL/rdn_nv12.h").read_text()
        struct = re.search(r"struct OSMesaNV12Source \{.*?\};", header, re.S).group()
        source = (ROOT / "mesa/frontend/rdn_nv12.c").read_text()
        begin = source.index("bool\nrdn_nv12_source_valid(")
        end = source.index("\nstatic void *", begin)
        body = "#include <assert.h>\n#include <stdbool.h>\n#include <stdint.h>\n#include <stdio.h>\n"
        body += struct + "\n" + source[begin:end] + CASES
        with tempfile.TemporaryDirectory(prefix="rdn-nv12-bounds-") as directory:
            path = pathlib.Path(directory)
            (path / "test.c").write_text(body)
            cc = shlex.split(os.environ.get("CC", "cc"))
            flags = ["-std=c99", "-Wall", "-Wextra", "-Werror"]
            flags += shlex.split(os.environ.get("CFLAGS", ""))
            subprocess.run(cc + flags + [str(path / "test.c"), "-o", str(path / "test")], check=True)
            subprocess.run(shlex.split(os.environ.get("TEST_RUNNER", "")) + [str(path / "test")], check=True)

if __name__ == "__main__":
    unittest.main()
