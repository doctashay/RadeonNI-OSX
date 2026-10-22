/* Copyright (c) 2026 kouta-kun and Claude
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/osmesa.h>
#include <GL/rdn_nv12.h>
#include "rdn_target.h"

#define GL_FUNCS(X) \
 X(const GLubyte *, GetString, (GLenum)) \
 X(GLenum, GetError, (void)) \
 X(void, GetIntegerv, (GLenum, GLint *)) \
 X(GLboolean, IsEnabled, (GLenum)) \
 X(void, Enable, (GLenum)) \
 X(void, ActiveTexture, (GLenum)) \
 X(void, Viewport, (GLint, GLint, GLsizei, GLsizei)) \
 X(void, Scissor, (GLint, GLint, GLsizei, GLsizei)) \
 X(void, GenTextures, (GLsizei, GLuint *)) \
 X(void, DeleteTextures, (GLsizei, const GLuint *)) \
 X(void, BindTexture, (GLenum, GLuint)) \
 X(void, TexParameteri, (GLenum, GLenum, GLint)) \
 X(void, TexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const GLvoid *)) \
 X(void, GetTexImage, (GLenum, GLint, GLenum, GLenum, GLvoid *)) \
 X(void, PixelStorei, (GLenum, GLint))
#define X(ret, name, args) static ret (*rgl##name) args;
GL_FUNCS(X)
#undef X

static int load_gl(void)
{
#define X(ret, name, args) \
 rgl##name = (ret (*) args)OSMesaGetProcAddress("gl" #name); \
 if (!rgl##name) { fprintf(stderr, "missing gl%s\n", #name); return 0; }
 GL_FUNCS(X)
#undef X
 return 1;
}

static unsigned char value(unsigned plane, unsigned x, unsigned y)
{
 return (plane * 71 + y * 17 + x * 29 + (x >> 3) * 11) & 255;
}

static uint32_t align_to(uint32_t n, uint32_t alignment)
{
 return (n + alignment - 1) & ~(alignment - 1);
}

static int run_case(OSMesaContext context, unsigned width, unsigned height,
                    unsigned long *checked)
{
 struct OSMesaNV12Source source = {0};
 uint32_t at = 0, base = 0, offsets[4], rows[4];
 GLuint textures[2] = {0};
 GLint viewport[4], scissor[4], active = 0, binding = 0;
 const GLint expected_viewport[4] = {3, 4, 17, 19};
 const GLint expected_scissor[4] = {5, 6, 7, 9};
 unsigned char *expected = NULL, *pixels = NULL;
 volatile unsigned char *mapped = NULL;
 int result = 1, allocated = 0;
 source.handle = RDN_TARGET_VRAM_HANDLE;
 source.width = width; source.height = height;
 source.pitch = align_to(width + 24, 256);
 rows[0] = rows[1] = height / 2;
 rows[2] = (height / 2 + 1) / 2; rows[3] = height / 4;
 for (unsigned i = 0; i < 4; i++) {
  offsets[i] = at;
  at = align_to(at + source.pitch * align_to(rows[i], 8) + 256, 256);
 }
 source.bytes = at;
 source.y_top = offsets[0]; source.y_bottom = offsets[1];
 source.uv_top = offsets[2]; source.uv_bottom = offsets[3];
 uint32_t allocation_bytes = align_to(at + 8192, 4096);
 expected = malloc(allocation_bytes);
 pixels = malloc((size_t)width * height);
 if (!expected || !pixels || !rdn_target_vram_alloc(allocation_bytes, &base))
  goto done;
 allocated = 1;
 source.offset = base + 4096;
 mapped = rdn_target_vram_map(base);
 if (!mapped) goto done;
 memset(expected, 0xa5, allocation_bytes);
 for (unsigned i = 0; i < 4; i++)
  for (unsigned y = 0; y < rows[i]; y++)
   for (unsigned x = 0; x < width; x++)
    expected[4096 + offsets[i] + y * source.pitch + x] = value(i / 2, x, 2 * y + i % 2);
 for (uint32_t i = 0; i < allocation_bytes; i++) mapped[i] = expected[i];
 __sync_synchronize();
 rglPixelStorei(GL_PACK_ALIGNMENT, 1);
 rglPixelStorei(GL_UNPACK_ALIGNMENT, 1);
 rglGenTextures(2, textures);
 for (unsigned plane = 0; plane < 2; plane++) {
  rglBindTexture(GL_TEXTURE_2D, textures[plane]);
  rglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  rglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  rglTexImage2D(GL_TEXTURE_2D, 0, plane ? GL_RG8 : GL_R8,
                width / (plane + 1), height / (plane + 1), 0,
                plane ? GL_RG : GL_RED, GL_UNSIGNED_BYTE, NULL);
 }
 if (rglGetError() != GL_NO_ERROR) goto done;
 rglViewport(3, 4, 17, 19); rglScissor(5, 6, 7, 9);
 rglEnable(GL_SCISSOR_TEST); rglEnable(GL_BLEND);
 rglActiveTexture(GL_TEXTURE3); rglBindTexture(GL_TEXTURE_2D, textures[1]);
 int packed = OSMesaPackNV12Store(context, &source, textures[0], textures[1], 2000);
 if (packed == OSMESA_NV12_PENDING) {
  fprintf(stderr, "packing completion unknown; retaining source and context until process exit\n");
  printf("{\"type\":\"case\",\"width\":%u,\"height\":%u,\"result\":3}\n", width, height);
  fflush(stdout); _exit(3);
 }
 if (packed != OSMESA_NV12_OK) {
  fprintf(stderr, "packing %ux%u returned %d\n", width, height, packed);
  goto done;
 }
 rglGetIntegerv(GL_VIEWPORT, viewport); rglGetIntegerv(GL_SCISSOR_BOX, scissor);
 rglGetIntegerv(GL_ACTIVE_TEXTURE, &active); rglGetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
 if (memcmp(viewport, expected_viewport, sizeof(viewport)) ||
     memcmp(scissor, expected_scissor, sizeof(scissor)) || active != GL_TEXTURE3 ||
     binding != (GLint)textures[1] || !rglIsEnabled(GL_SCISSOR_TEST) || !rglIsEnabled(GL_BLEND)) {
  fprintf(stderr, "application GL state changed\n"); goto done;
 }
 for (unsigned plane = 0; plane < 2; plane++) {
  rglBindTexture(GL_TEXTURE_2D, textures[plane]);
  memset(pixels, 0xde, (size_t)width * height);
  rglGetTexImage(GL_TEXTURE_2D, 0, plane ? GL_RG : GL_RED, GL_UNSIGNED_BYTE, pixels);
  if (rglGetError() != GL_NO_ERROR) goto done;
  for (unsigned y = 0; y < height / (plane + 1); y++)
   for (unsigned x = 0; x < width; x++) {
    if (pixels[y * width + x] != value(plane, x, y)) {
     fprintf(stderr, "pixel mismatch %ux%u plane%u x%u y%u: %u != %u\n",
             width, height, plane, x, y, pixels[y * width + x], value(plane, x, y));
     goto done;
    }
    (*checked)++;
   }
 }
 /* Check the source and padding only after confirmed completion. */
 mapped = rdn_target_vram_map(base);
 if (!mapped) goto done;
 for (uint32_t i = 0; i < allocation_bytes; i++)
  if (mapped[i] != expected[i]) {
   fprintf(stderr, "source or guard changed at byte%u\n", i); goto done;
  }
 result = 0;
 printf("{\"type\":\"case\",\"width\":%u,\"height\":%u,\"pitch\":%u,\"result\":0,\"source_unchanged\":true,\"gl_state_unchanged\":true}\n",
        width, height, source.pitch);
done:
 if (textures[0] || textures[1]) rglDeleteTextures(2, textures);
 if (allocated) rdn_target_vram_free(base);
 free(expected); free(pixels);
 return result;
}

int main(void)
{
 const unsigned sizes[][2] = {{64,64}, {130,70}, {1280,720}, {1920,1088}};
 unsigned char drawable[16 * 16 * 4] = {0};
 unsigned long checked = 0;
 OSMesaContext context = OSMesaCreateContextExt(OSMESA_BGRA, 0, 0, 0, NULL);
 if (!context || !OSMesaMakeCurrent(context, drawable, GL_UNSIGNED_BYTE, 16, 16) ||
     !load_gl() || !OSMesaCanPackNV12(context)) {
  fprintf(stderr, "NV12 packing unavailable\n"); return 2;
 }
 fprintf(stderr, "renderer: %s; GL: %s\n", rglGetString(GL_RENDERER), rglGetString(GL_VERSION));
 for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
  if (run_case(context, sizes[i][0], sizes[i][1], &checked)) {
   OSMesaDestroyContext(context); return 1;
  }
 OSMesaDestroyContext(context);
 printf("{\"type\":\"complete\",\"passed\":true,\"cases\":4,\"checked_bytes\":%lu}\n", checked);
 return 0;
}
