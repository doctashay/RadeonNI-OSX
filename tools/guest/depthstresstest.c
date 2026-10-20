/* Exercise depth/HTILE clears, MSAA resolves and depth rejection. */
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <IOKit/IOKitLib.h>
#include "rdn_user.h"

#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

int main(int argc, char **argv)
{
    unsigned samples = argc > 1 ? atoi(argv[1]) : 4;
    unsigned loops = argc > 2 ? atoi(argv[2]) : 1000;
    unsigned w = argc > 3 ? atoi(argv[3]) : 1600;
    unsigned h = argc > 4 ? atoi(argv[4]) : 880;
    CGLPixelFormatAttribute attrs[] = {kCGLPFAAccelerated, kCGLPFANoRecovery,
        kCGLPFARendererID, 0x21a00, 0};
    CGLPixelFormatObj pf;
    CGLContextObj ctx;
    GLint n;
    GLuint fbo[2], rb[3];
    unsigned i;
    unsigned char pixel[4];
    io_connect_t pressure = 0;
    CGLError (*current)(CGLContextObj);
    void (*storage_ms)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
    void (*blit)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
    setbuf(stdout, NULL);
    CHECK(samples >= 1 && samples <= 8 && loops && w && h && w <= 2048 && h <= 2048);
    CHECK(!CGLChoosePixelFormat(attrs, &pf, &n) && pf);
    CHECK(!CGLCreateContext(pf, NULL, &ctx));
    current = dlsym(RTLD_DEFAULT, "CGLSetCurrentContext");
    CHECK(current && !current(ctx));
    storage_ms = dlsym(RTLD_DEFAULT, "glRenderbufferStorageMultisampleEXT");
    blit = dlsym(RTLD_DEFAULT, "glBlitFramebufferEXT");
    CHECK(storage_ms && blit);
    printf("%s, %s, %ux%u samples=%u\n", glGetString(GL_RENDERER), glGetString(GL_VERSION), w, h, samples);
    glGenFramebuffersEXT(2, fbo);
    glGenRenderbuffersEXT(3, rb);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo[0]);
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, rb[0]);
    storage_ms(GL_RENDERBUFFER_EXT, samples, GL_RGBA8, w, h);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, rb[0]);
    if (argc > 5 && atoi(argv[5])) {
        io_service_t service = IOServiceGetMatchingService(kIOMasterPortDefault,
            IOServiceMatching(RDN_UC_SERVICE_CLASS));
        unsigned offset, reserve, held = 0;
        glFinish();
        CHECK(glGetError() == GL_NO_ERROR);
        CHECK(service && !IOServiceOpen(service, mach_task_self(), RDN_UC_TYPE, &pressure));
        IOObjectRelease(service);
        CHECK(!IOConnectMethodScalarIScalarO(pressure, RDN_UC_ALLOC, 2, 1,
                                             8 << 20, 4096, &reserve));
        while (!IOConnectMethodScalarIScalarO(pressure, RDN_UC_ALLOC_HIDDEN, 2, 1,
                                               1 << 20, 4096, &offset)) ++held;
        while (!IOConnectMethodScalarIScalarO(pressure, RDN_UC_ALLOC, 2, 1,
                                               1 << 20, 4096, &offset)) ++held;
        CHECK(!IOConnectMethodScalarIScalarO(pressure, RDN_UC_FREE, 1, 0, reserve));
        printf("pressure holds %u MiB; 8 MiB reserved for submissions\n", held);
    }
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, rb[1]);
    storage_ms(GL_RENDERBUFFER_EXT, samples, GL_DEPTH24_STENCIL8_EXT, w, h);
    if (pressure) {
        GLenum error = glGetError();
        printf("depth allocation error: 0x%x\n", error);
        CHECK(error == GL_OUT_OF_MEMORY);
        CHECK(!IOServiceClose(pressure));
        pressure = 0;
        puts("VRAM exhaustion reported without submitting an invalid depth target");
        storage_ms(GL_RENDERBUFFER_EXT, samples, GL_DEPTH24_STENCIL8_EXT, w, h);
        CHECK(glGetError() == GL_NO_ERROR);
    }
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, rb[1]);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_STENCIL_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, rb[1]);
    CHECK(glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) == GL_FRAMEBUFFER_COMPLETE_EXT);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo[1]);
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, rb[2]);
    glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, w, h);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, rb[2]);
    CHECK(glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) == GL_FRAMEBUFFER_COMPLETE_EXT);
    glViewport(0, 0, w, h);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    for (i = 0; i < loops; ++i) {
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo[0]);
        glClearColor(0, 0, 0, 1);
        glClearDepth(1);
        glClearStencil(i & 255);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        glColor4f(0, 1, 0, 1);
        glBegin(GL_TRIANGLES);
        glVertex3f(-1, -1, 0); glVertex3f(3, -1, 0); glVertex3f(-1, 3, 0);
        glEnd();
        glColor4f(1, 0, 0, 1);
        glBegin(GL_TRIANGLES);
        glVertex3f(-1, -1, 0.5); glVertex3f(3, -1, 0.5); glVertex3f(-1, 3, 0.5);
        glEnd();
        glBindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, fbo[0]);
        glBindFramebufferEXT(GL_DRAW_FRAMEBUFFER_EXT, fbo[1]);
        blit(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo[1]);
        glReadPixels(w / 2, h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        CHECK(glGetError() == GL_NO_ERROR);
        CHECK(pixel[0] == 0 && pixel[1] == 255 && pixel[2] == 0 && pixel[3] == 255);
        if (!(i % 100)) printf("completed %u\n", i + 1);
    }
    glDeleteFramebuffersEXT(2, fbo);
    glDeleteRenderbuffersEXT(3, rb);
    current(NULL);
    CGLDestroyContext(ctx);
    CGLDestroyPixelFormat(pf);
    if (pressure) IOServiceClose(pressure);
    printf("PASS %u depth/resolve checks\n", loops);
    return 0;
}
