/* Alternate shader workloads over immutable geometry without per-draw waits. */
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <dlfcn.h>

#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)
#define CHECK_GL() do { GLenum e = glGetError(); if (e) { printf("GL error 0x%x line %d\n", e, __LINE__); return 1; } } while (0)
#ifndef GRID
#define GRID 64
#endif
#ifndef TARGET_WIDTH
#define TARGET_WIDTH 256
#define TARGET_HEIGHT 256
#endif
struct vertex { GLfloat p[3], n[3], uv[2]; };

int main(int argc, char **argv)
{
    unsigned draws = argc > 1 ? atoi(argv[1]) : 10000;
    int depth_test = argc > 2 && atoi(argv[2]);
    int variants = argc > 3 && atoi(argv[3]);
    GLenum usage = argc > 4 && atoi(argv[4]) ? GL_STREAM_DRAW : GL_STATIC_DRAW;
    int depth_bits = argc > 5 ? atoi(argv[5]) : 24;
    CGLPixelFormatAttribute attrs[] = {kCGLPFAAccelerated, kCGLPFANoRecovery,
        kCGLPFARendererID, 0x21a00, 0};
    CGLPixelFormatObj pf;
    CGLContextObj ctx;
    CGLError (*current)(CGLContextObj) = dlsym(RTLD_DEFAULT, "CGLSetCurrentContext");
    GLint n;
    GLuint fbo, rb, depth = 0, buffers[2], textures[4];
    GLfloat white[4] = {1, 1, 1, 1}, ambient[4] = {.125, .125, .125, 1};
    struct vertex *vertices = calloc((GRID + 1) * (GRID + 1), sizeof(*vertices));
    GLushort *indices = calloc(GRID * GRID * 6, sizeof(*indices));
    unsigned char texels[16 * 16 * 4], pixel[4];
    unsigned x, y, i, at = 0;
    setbuf(stdout, NULL);
    CHECK(draws && draws <= 100000 && vertices && indices && current);
    CHECK(depth_bits == 16 || depth_bits == 24);
    CHECK(!CGLChoosePixelFormat(attrs, &pf, &n) && pf);
    CHECK(!CGLCreateContext(pf, NULL, &ctx) && !current(ctx));
    for (y = 0; y <= GRID; y++) for (x = 0; x <= GRID; x++) {
        struct vertex *v = &vertices[y * (GRID + 1) + x];
        v->p[0] = 2.0f * x / GRID - 1; v->p[1] = 2.0f * y / GRID - 1;
        v->p[2] = ((x ^ y) & 1) ? .25f : -.25f;
        v->n[2] = 1; v->uv[0] = (float)x / GRID; v->uv[1] = (float)y / GRID;
    }
    for (y = 0; y < GRID; y++) for (x = 0; x < GRID; x++) {
        unsigned a = y * (GRID + 1) + x, b = a + GRID + 1;
        indices[at++] = a; indices[at++] = a + 1; indices[at++] = b;
        indices[at++] = b; indices[at++] = a + 1; indices[at++] = b + 1;
    }
    glGenBuffers(2, buffers);
    printf("buffers %u %u, renderer %s\n", buffers[0], buffers[1], glGetString(GL_RENDERER));
    CHECK_GL();
    glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
    glBufferData(GL_ARRAY_BUFFER, (GRID + 1) * (GRID + 1) * sizeof(*vertices), vertices, usage);
    CHECK_GL();
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers[1]);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, at * sizeof(*indices), indices, usage);
    CHECK_GL();
    free(vertices); free(indices);
    glVertexPointer(3, GL_FLOAT, sizeof(struct vertex), 0);
    CHECK_GL();
    glNormalPointer(GL_FLOAT, sizeof(struct vertex), (void *)(3 * sizeof(GLfloat)));
    glEnableClientState(GL_VERTEX_ARRAY); glEnableClientState(GL_NORMAL_ARRAY);
    CHECK_GL();
    for (i = 0; i < sizeof(texels); i++) texels[i] = 255;
    glGenTextures(4, textures);
    for (i = 0; i < 4; i++) {
        glActiveTexture(GL_TEXTURE0 + i); glClientActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, textures[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexCoordPointer(2, GL_FLOAT, sizeof(struct vertex), (void *)(6 * sizeof(GLfloat)));
        glEnableClientState(GL_TEXTURE_COORD_ARRAY); glEnable(GL_TEXTURE_2D);
    }
    CHECK_GL();
    glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE, white);
    for (i = 0; i < 8; i++) {
        GLfloat pos[4] = {(GLfloat)(i & 1), (GLfloat)((i >> 1) & 1), 1, 0};
        glEnable(GL_LIGHT0 + i);
        glLightfv(GL_LIGHT0 + i, GL_POSITION, pos);
        glLightfv(GL_LIGHT0 + i, GL_DIFFUSE, ambient);
    }
    CHECK_GL();
    glGenFramebuffersEXT(1, &fbo); glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
    glGenRenderbuffersEXT(1, &rb); glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, rb);
    glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, TARGET_WIDTH, TARGET_HEIGHT);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, rb);
    if (depth_test) {
        glGenRenderbuffersEXT(1, &depth); glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, depth);
        glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT,
            depth_bits == 16 ? GL_DEPTH_COMPONENT16 : GL_DEPTH24_STENCIL8_EXT,
            TARGET_WIDTH, TARGET_HEIGHT);
        glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, depth);
        if (depth_bits == 24)
            glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_STENCIL_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, depth);
        glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LEQUAL);
        glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    }
    CHECK(glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) == GL_FRAMEBUFFER_COMPLETE_EXT);
    glViewport(0, 0, TARGET_WIDTH, TARGET_HEIGHT); glColor4f(0, 1, 0, 1);
    glFinish(); CHECK_GL();
    printf("begin %u draws, %u immutable indices per draw, depth=%d/%d variants=%d usage=0x%x target=%dx%d\n",
           draws, at, depth_test, depth_bits, variants, usage, TARGET_WIDTH, TARGET_HEIGHT);
    for (i = 0; i < draws; i++) {
        if (variants) {
            unsigned j, lights = i % 9, units = (i / 9) % 5;
            if (lights) glEnable(GL_LIGHTING); else glDisable(GL_LIGHTING);
            for (j = 0; j < 8; j++) {
                if (j < lights) glEnable(GL_LIGHT0 + j); else glDisable(GL_LIGHT0 + j);
            }
            for (j = 0; j < 4; j++) {
                glActiveTexture(GL_TEXTURE0 + j);
                if (j < units) glEnable(GL_TEXTURE_2D); else glDisable(GL_TEXTURE_2D);
            }
        } else if (i & 1) glEnable(GL_LIGHTING); else glDisable(GL_LIGHTING);
        if (depth_test) glDepthRange((i & 1) ? .1 : 0, (i & 1) ? .9 : 1);
        glDrawElements(GL_TRIANGLES, at, GL_UNSIGNED_SHORT, 0);
        if (!(i % 1000)) { glFlush(); printf("queued %u\n", i + 1); }
    }
    glDisable(GL_LIGHTING); glDisable(GL_DEPTH_TEST);
    glDrawElements(GL_TRIANGLES, at, GL_UNSIGNED_SHORT, 0);
    glReadPixels(TARGET_WIDTH / 2, TARGET_HEIGHT / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    CHECK(glGetError() == GL_NO_ERROR);
    printf("pixel %u %u %u %u\n", pixel[0], pixel[1], pixel[2], pixel[3]);
    CHECK(pixel[0] == 0 && pixel[1] == 255 && pixel[2] == 0 && pixel[3] == 255);
    glDeleteBuffers(2, buffers); glDeleteTextures(4, textures);
    glDeleteFramebuffersEXT(1, &fbo); glDeleteRenderbuffersEXT(1, &rb);
    if (depth) glDeleteRenderbuffersEXT(1, &depth);
    current(NULL); CGLDestroyContext(ctx); CGLDestroyPixelFormat(pf);
    puts("PASS overlapping draws");
    return 0;
}
