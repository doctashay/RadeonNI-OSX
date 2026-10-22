/* Resolve a typed decoder lease and pack it in the current Mesa context. */
#include <string.h>
#include <IOKit/IOKitLib.h>
#include <GL/rdn_nv12.h>

#include "hw/rdn_video.h"
#include "rdn_target.h"
#include "rdn_video_pack.h"

typedef char rdn_video_pack_request_size[
 sizeof(struct RDNGLVideoPackRequestV1) == 96 ? 1 : -1];

static int words_zero(const uint32_t *words, unsigned count)
{
 unsigned i;
 for (i = 0; i < count; ++i)
  if (words[i]) return 0;
 return 1;
}

static kern_return_t acquire_source(io_connect_t connection,
 const struct rdn_video_gpu_acquire *request,
 struct rdn_video_gpu_source *source)
{
#ifdef __LP64__
 size_t bytes = sizeof(*source);
 kern_return_t result = IOConnectCallStructMethod(connection,
  RDN_VIDEO_ACQUIRE_GPU, request, sizeof(*request), source, &bytes);
#else
 IOByteCount bytes = sizeof(*source);
 kern_return_t result = IOConnectMethodStructureIStructureO(connection,
  RDN_VIDEO_ACQUIRE_GPU, sizeof(*request), &bytes, (void *)request, source);
#endif
 if (!result && bytes != sizeof(*source)) return kIOReturnUnderrun;
 return result;
}

static kern_return_t retire_source(io_connect_t connection,
 const uint32_t token[8], uint32_t outcome)
{
 struct rdn_video_gpu_retire request;
 memset(&request, 0, sizeof(request));
 request.version = RDN_VIDEO_API_VERSION;
 request.bytes = sizeof(request);
 memcpy(request.source_token, token, sizeof(request.source_token));
 request.role = RDN_VIDEO_GPU_IMPORTER;
 request.outcome = outcome;
#ifdef __LP64__
 return IOConnectCallStructMethod(connection, RDN_VIDEO_RETIRE_GPU,
  &request, sizeof(request), NULL, NULL);
#else
 return IOConnectMethodScalarIStructureI(connection, RDN_VIDEO_RETIRE_GPU,
  0, sizeof(request), &request);
#endif
}

__attribute__((visibility("default")))
int RDNGLQueryVideoPackingV1(uint32_t version, uint32_t *capabilities)
{
 if (!capabilities) return RDN_GL_VIDEO_PACK_FAILED_RETIRED;
 *capabilities = 0;
 if (version != RDN_GL_VIDEO_PACK_VERSION_1 ||
     !OSMesaCanPackNV12(OSMesaGetCurrentContext()))
  return RDN_GL_VIDEO_PACK_UNSUPPORTED;
 *capabilities = RDN_GL_VIDEO_PACK_CAP_NV12_COMPLETED;
 return RDN_GL_VIDEO_PACK_OK;
}

__attribute__((visibility("default")))
int RDNGLPackVideoFrameV1(const struct RDNGLVideoPackRequestV1 *request)
{
 struct rdn_video_gpu_acquire acquire;
 struct rdn_video_gpu_source source;
 struct OSMesaNV12Source image;
 OSMesaContext context;
 kern_return_t result;
 int packed = OSMESA_NV12_RETIRED;

 if (!request || request->version != RDN_GL_VIDEO_PACK_VERSION_1 ||
     request->bytes != sizeof(*request) || request->flags ||
     !request->video_connection || !request->coded_width ||
     !request->coded_height || (request->coded_width & 1) ||
     (request->coded_height & 1) || !request->destination_y ||
     !request->destination_uv || request->destination_y == request->destination_uv ||
     !request->timeout_ms || request->timeout_ms > RDN_GL_VIDEO_PACK_MAX_TIMEOUT_MS ||
     !words_zero(request->reserved, 7) || words_zero(request->source_token, 8))
  return RDN_GL_VIDEO_PACK_FAILED_RETIRED;
 context = OSMesaGetCurrentContext();
 if (!OSMesaCanPackNV12(context)) return RDN_GL_VIDEO_PACK_UNSUPPORTED;

 memset(&acquire, 0, sizeof(acquire));
 memset(&source, 0, sizeof(source));
 acquire.version = RDN_VIDEO_API_VERSION;
 acquire.bytes = sizeof(acquire);
 memcpy(acquire.source_token, request->source_token, sizeof(acquire.source_token));
 result = acquire_source(request->video_connection, &acquire, &source);
 if (result) {
  /* A malformed successful reply may already own an import. */
  if (result == kIOReturnUnderrun) {
   if (retire_source(request->video_connection, request->source_token,
                     RDN_VIDEO_GPU_RETIRED))
    return RDN_GL_VIDEO_PACK_FAILED_PENDING;
  }
  return RDN_GL_VIDEO_PACK_FAILED_RETIRED;
 }
 if (source.version == RDN_VIDEO_API_VERSION && source.bytes == sizeof(source) &&
     !memcmp(source.source_token, request->source_token, sizeof(source.source_token)) &&
     source.coded_width == request->coded_width &&
     source.coded_height == request->coded_height &&
     source.layout == RDN_VIDEO_LAYOUT_FIELD_NV12 && words_zero(source.reserved, 4)) {
  image.handle = RDN_TARGET_VRAM_HANDLE;
  image.offset = source.vram_offset;
  image.bytes = source.target_bytes;
  image.pitch = source.pitch;
  image.width = source.coded_width;
  image.height = source.coded_height;
  image.y_top = source.y_top;
  image.y_bottom = source.y_bottom;
  image.uv_top = source.uv_top;
  image.uv_bottom = source.uv_bottom;
  packed = OSMesaPackNV12Store(context, &image, request->destination_y,
                             request->destination_uv, request->timeout_ms);
 }
 if (packed != OSMESA_NV12_OK && packed != OSMESA_NV12_UNSUPPORTED &&
     packed != OSMESA_NV12_RETIRED)
  packed = OSMESA_NV12_PENDING;
 result = retire_source(request->video_connection, request->source_token,
  packed == OSMESA_NV12_PENDING ? RDN_VIDEO_GPU_PENDING : RDN_VIDEO_GPU_RETIRED);
 if (result || packed == OSMESA_NV12_PENDING)
  return RDN_GL_VIDEO_PACK_FAILED_PENDING;
 return packed;
}
