"""Exercise the GLD lease bridge with both Leopard IOKit entry-point shapes."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
IOKIT = r'''
#include <stddef.h>
#include <stdint.h>
typedef uint32_t io_connect_t;
typedef int kern_return_t;
typedef unsigned long IOByteCount;
#define kIOReturnUnderrun 17
int IOConnectCallStructMethod(io_connect_t, uint32_t, const void *, size_t,
                              void *, size_t *);
int IOConnectMethodStructureIStructureO(io_connect_t, uint32_t, unsigned,
                                       IOByteCount *, void *, void *);
int IOConnectMethodScalarIStructureI(io_connect_t, uint32_t, unsigned,
                                     IOByteCount, ...);
'''
OSMESA = r'''
#include <stdint.h>
typedef void *OSMesaContext;
typedef unsigned GLuint;
struct OSMesaNV12Source {
 uint32_t handle, offset, bytes, pitch, width, height;
 uint32_t y_top, y_bottom, uv_top, uv_bottom;
};
enum { OSMESA_NV12_OK, OSMESA_NV12_UNSUPPORTED,
       OSMESA_NV12_RETIRED, OSMESA_NV12_PENDING };
OSMesaContext OSMesaGetCurrentContext(void);
int OSMesaCanPackNV12(OSMesaContext);
int OSMesaPackNV12Store(OSMesaContext, const struct OSMesaNV12Source *,
                       GLuint, GLuint, uint32_t);
'''
HARNESS = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include "gld/rdn_video.c"
static unsigned checks, acquisitions, retirements, packs;
static int capable, acquire_error, retire_error, pack_result, short_reply;
static uint32_t last_outcome;
static struct rdn_video_gpu_source reply;
static struct RDNGLVideoPackRequestV1 request;
#define CHECK(c) do { ++checks; assert(c); } while (0)

OSMesaContext OSMesaGetCurrentContext(void) { return (void *)1; }
int OSMesaCanPackNV12(OSMesaContext c) { CHECK(c == (void *)1); return capable; }
int OSMesaPackNV12Store(OSMesaContext c, const struct OSMesaNV12Source *s,
                       GLuint y, GLuint uv, uint32_t timeout)
{
 ++packs;
 CHECK(c == (void *)1 && y == 4 && uv == 5 && timeout == 100);
 CHECK(s->handle == RDN_TARGET_VRAM_HANDLE && s->offset == reply.vram_offset);
 CHECK(s->bytes == reply.target_bytes && s->pitch == reply.pitch);
 CHECK(s->width == reply.coded_width && s->height == reply.coded_height);
 CHECK(s->y_top == reply.y_top && s->y_bottom == reply.y_bottom);
 CHECK(s->uv_top == reply.uv_top && s->uv_bottom == reply.uv_bottom);
 return pack_result;
}
static int invoke(io_connect_t connection, uint32_t method, const void *in,
                   size_t input_bytes, void *out, size_t *output_bytes)
{
 CHECK(connection == 9);
 if (method == RDN_VIDEO_ACQUIRE_GPU) {
  const struct rdn_video_gpu_acquire *a = in;
  ++acquisitions;
  CHECK(input_bytes == sizeof(*a) && a->version == 1 && a->bytes == sizeof(*a));
  CHECK(!memcmp(a->source_token, request.source_token, sizeof(a->source_token)));
  CHECK(words_zero(a->reserved, 6));
  CHECK(out && output_bytes && *output_bytes == sizeof(reply));
  if (acquire_error) return acquire_error;
  memcpy(out, &reply, sizeof(reply));
  if (short_reply) --*output_bytes;
  return 0;
 }
 CHECK(method == RDN_VIDEO_RETIRE_GPU);
 const struct rdn_video_gpu_retire *r = in;
 ++retirements;
 CHECK(input_bytes == sizeof(*r) && r->version == 1 && r->bytes == sizeof(*r));
 CHECK(!memcmp(r->source_token, request.source_token, sizeof(r->source_token)));
 CHECK(r->role == RDN_VIDEO_GPU_IMPORTER && words_zero(r->reserved, 4));
 CHECK(!out && !output_bytes);
 last_outcome = r->outcome;
 return retire_error;
}
int IOConnectCallStructMethod(io_connect_t c, uint32_t m, const void *in,
                              size_t n, void *out, size_t *size)
{ return invoke(c, m, in, n, out, size); }
int IOConnectMethodStructureIStructureO(io_connect_t c, uint32_t m, unsigned n,
                                       IOByteCount *size, void *in, void *out)
{ size_t bytes = *size; int r = invoke(c, m, in, n, out, &bytes); *size = bytes; return r; }
int IOConnectMethodScalarIStructureI(io_connect_t c, uint32_t m, unsigned scalars,
                                     IOByteCount n, ...)
{
 va_list args; const void *in;
 CHECK(!scalars); va_start(args, n); in = va_arg(args, const void *); va_end(args);
 return invoke(c, m, in, n, NULL, NULL);
}
static void reset(void)
{
 acquisitions = retirements = packs = 0;
 capable = 1; acquire_error = retire_error = short_reply = 0;
 pack_result = OSMESA_NV12_OK; last_outcome = 99;
 memset(&request, 0, sizeof(request));
 request.version = 1; request.bytes = sizeof(request); request.video_connection = 9;
 request.source_token[0] = 77; request.source_token[7] = 88;
 request.coded_width = 1920; request.coded_height = 1088;
 request.destination_y = 4; request.destination_uv = 5; request.timeout_ms = 100;
 memset(&reply, 0, sizeof(reply));
 reply.version = 1; reply.bytes = sizeof(reply);
 memcpy(reply.source_token, request.source_token, sizeof(reply.source_token));
 reply.vram_offset = 0x10000000; reply.target_bytes = 3342336; reply.pitch = 2048;
 reply.coded_width = 1920; reply.coded_height = 1088;
 reply.y_bottom = 1114112; reply.uv_top = 2228224; reply.uv_bottom = 2785280;
 reply.layout = RDN_VIDEO_LAYOUT_FIELD_NV12;
}
static void invalid(void)
{
 CHECK(RDNGLPackVideoFrameV1(&request) == RDN_GL_VIDEO_PACK_FAILED_RETIRED);
 CHECK(!acquisitions && !packs && !retirements);
}
int main(void)
{
 uint32_t caps;
 CHECK(sizeof(request) == 96 && sizeof(reply) == 96);
 CHECK(sizeof(struct rdn_video_gpu_acquire) == 64);
 CHECK(sizeof(struct rdn_video_gpu_retire) == 64);
 CHECK(RDN_VIDEO_ACQUIRE_GPU == 6 && RDN_VIDEO_RETIRE_GPU == 7);
 reset(); CHECK(!RDNGLQueryVideoPackingV1(1, &caps) && caps == 1);
 CHECK(RDNGLQueryVideoPackingV1(2, &caps) == 1 && caps == 0);
 capable = 0; CHECK(RDNGLQueryVideoPackingV1(1, &caps) == 1 && caps == 0);
 CHECK(RDNGLPackVideoFrameV1(&request) == 1 && !acquisitions);
 CHECK(RDNGLPackVideoFrameV1(NULL) == 2);
 reset(); request.version = 2; invalid();
 reset(); --request.bytes; invalid();
 reset(); request.flags = 1; invalid();
 reset(); request.video_connection = 0; invalid();
 reset(); request.coded_width = 0; invalid();
 reset(); ++request.coded_height; invalid();
 reset(); request.destination_y = 0; invalid();
 reset(); request.destination_uv = request.destination_y; invalid();
 reset(); request.timeout_ms = 0; invalid();
 reset(); request.timeout_ms = 2001; invalid();
 reset(); request.reserved[6] = 1; invalid();
 reset(); memset(request.source_token, 0, sizeof(request.source_token)); invalid();
 for (int result = 0; result < 5; ++result) {
  reset(); pack_result = result;
  CHECK(RDNGLPackVideoFrameV1(&request) == (result < 3 ? result : 3));
  CHECK(acquisitions == 1 && packs == 1 && retirements == 1);
  CHECK(last_outcome == (result < 3 ? RDN_VIDEO_GPU_RETIRED : RDN_VIDEO_GPU_PENDING));
 }
 for (int field = 0; field < 7; ++field) {
  reset();
  switch (field) {
   case 0: ++reply.version; break;
   case 1: --reply.bytes; break;
   case 2: ++reply.source_token[7]; break;
   case 3: ++reply.coded_width; break;
   case 4: ++reply.coded_height; break;
   case 5: ++reply.layout; break;
   case 6: ++reply.reserved[3]; break;
  }
  CHECK(RDNGLPackVideoFrameV1(&request) == 2);
  CHECK(!packs && retirements == 1 && last_outcome == RDN_VIDEO_GPU_RETIRED);
 }
 reset(); acquire_error = 5;
 CHECK(RDNGLPackVideoFrameV1(&request) == 2 && !packs && !retirements);
 reset(); short_reply = 1;
 CHECK(RDNGLPackVideoFrameV1(&request) == 2 && !packs && retirements == 1);
 reset(); short_reply = 1; retire_error = 5;
 CHECK(RDNGLPackVideoFrameV1(&request) == 3 && !packs && retirements == 1);
 reset(); retire_error = 5;
 CHECK(RDNGLPackVideoFrameV1(&request) == 3 && packs == 1 && retirements == 1);
 printf("PASS video pack bridge: %u checks\n", checks);
 return 0;
}
'''


class VideoPackBridge(unittest.TestCase):
    def test_iokit_entry_points(self):
        compiler = shlex.split(os.environ.get("CC", "cc"))
        flags = shlex.split(os.environ.get("CFLAGS", ""))
        runner = shlex.split(os.environ.get("TEST_RUNNER", ""))
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary)
            (path / "IOKit").mkdir()
            (path / "GL").mkdir()
            (path / "IOKit/IOKitLib.h").write_text(IOKIT)
            (path / "GL/rdn_nv12.h").write_text(OSMESA)
            (path / "test.c").write_text(HARNESS)
            for abi in ("-D__LP64__=1", "-U__LP64__"):
                with self.subTest(abi=abi):
                    subprocess.run(compiler + flags + [abi, "-std=c99", "-Wall", "-Wextra",
                        "-Werror", "-I" + str(path), "-I" + str(ROOT),
                        "-I" + str(ROOT / "mesa/target"), str(path / "test.c"),
                        "-o", str(path / "test")], check=True)
                    subprocess.run(runner + [str(path / "test")], check=True)


if __name__ == "__main__":
    unittest.main()
