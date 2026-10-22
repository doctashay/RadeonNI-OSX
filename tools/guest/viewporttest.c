/* Reproduce drawable-coordinate state restoration in a private GL context. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <mach-o/dyld.h>
#include <mach-o/nlist.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>

int main(void)
{
    CGLPixelFormatAttribute attrs[] = { kCGLPFARendererID, 0x21a00,
        kCGLPFAOffScreen, 0 };
    CGLPixelFormatObj pf;
    CGLContextObj ctx;
    GLint count, viewport[4], scissor[4], again[4];
    uint32_t *pixels = calloc(128 * 128, 4);
    struct nlist symbols[3];
    int *ox = NULL, *oy = NULL;
    unsigned i;
    int failures = 0;
    setbuf(stdout, NULL);
    if (CGLChoosePixelFormat(attrs, &pf, &count) ||
        CGLCreateContext(pf, NULL, &ctx) || CGLSetCurrentContext(ctx) ||
        CGLSetOffScreen(ctx, 128, 128, 512, pixels)) return 2;
    glClearColor(0, 1, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    for (i = 0; i < _dyld_image_count(); ++i) {
        const char *path = _dyld_get_image_name(i);
        if (!strstr(path, "RadeonNIGLDriver")) continue;
        memset(symbols, 0, sizeof(symbols));
        symbols[0].n_un.n_name = "_rdn_origin_x";
        symbols[1].n_un.n_name = "_rdn_origin_y";
        if (nlist(path, symbols)) return 3;
        ox = (int *)(symbols[0].n_value + _dyld_get_image_vmaddr_slide(i));
        oy = (int *)(symbols[1].n_value + _dyld_get_image_vmaddr_slide(i));
        break;
    }
    if (!ox || !oy) return 4;
    /* Emulate the origin assigned when WindowServer binds a screen region. */
    *ox = 16; *oy = 24;
    glViewport(0, 0, 32, 32);
    glScissor(0, 0, 32, 32);
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_SCISSOR_BOX, scissor);
    printf("viewport query: %d %d %d %d; expected 0 0 32 32\n",
        viewport[0], viewport[1], viewport[2], viewport[3]);
    printf("scissor query: %d %d %d %d; expected 0 0 32 32\n",
        scissor[0], scissor[1], scissor[2], scissor[3]);
    failures += viewport[0] != 0 || viewport[1] != 0;
    failures += scissor[0] != 0 || scissor[1] != 0;
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    glGetIntegerv(GL_VIEWPORT, again);
    printf("viewport after save/restore: %d %d %d %d\n",
        again[0], again[1], again[2], again[3]);
    failures += memcmp(viewport, again, sizeof(viewport)) != 0;
    {
        GLuint fbo, texture;
        unsigned char rgb[32 * 32 * 4];
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 32, 32, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glGenFramebuffersEXT(1, &fbo);
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
        glGetIntegerv(GL_VIEWPORT, again);
        printf("viewport inherited by FBO: %d %d; expected 0 0\n", again[0], again[1]);
        failures += again[0] != 0 || again[1] != 0;
        glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
                                  GL_TEXTURE_2D, texture, 0);
        if (glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) != GL_FRAMEBUFFER_COMPLETE_EXT)
            return 5;
        glViewport(0, 0, 32, 32);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0, 1, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glMatrixMode(GL_PROJECTION); glLoadIdentity();
        glMatrixMode(GL_MODELVIEW); glLoadIdentity();
        glColor3f(0, 0, 1);
        glBegin(GL_QUADS);
        glVertex2f(-1, -1); glVertex2f(1, -1);
        glVertex2f(1, 1); glVertex2f(-1, 1);
        glEnd();
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgb);
        printf("FBO center: %u %u %u; expected 0 0 255\n",
            rgb[(16 * 32 + 16) * 4], rgb[(16 * 32 + 16) * 4 + 1],
            rgb[(16 * 32 + 16) * 4 + 2]);
        failures += rgb[(16 * 32 + 16) * 4 + 2] != 255;
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
        glGetIntegerv(GL_VIEWPORT, again);
        printf("viewport after FBO unbind: %d %d; expected 0 0\n", again[0], again[1]);
        failures += again[0] != 0 || again[1] != 0;
        glDeleteFramebuffersEXT(1, &fbo);
        glDeleteTextures(1, &texture);
    }
    *ox = 0; *oy = 0;
    CGLSetCurrentContext(NULL);
    CGLDestroyContext(ctx);
    CGLDestroyPixelFormat(pf);
    free(pixels);
    printf("%d failed checks\n", failures);
    return failures ? 1 : 0;
}
