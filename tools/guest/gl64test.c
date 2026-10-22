/* Exercise accelerated context creation, shader execution and texture readback. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>

int main(int argc, char **argv)
{
    CGLError (*make_current)(CGLContextObj) = argc > 1 ?
        (CGLError (*)(CGLContextObj))dlsym(RTLD_DEFAULT, "CGLSetCurrentContext") :
        CGLSetCurrentContext;
    (void)argv;
    CGLPixelFormatAttribute attrs[] = {
        kCGLPFAAccelerated, kCGLPFANoRecovery, kCGLPFAOffScreen,
        kCGLPFARendererID, 0x21a00, 0
    };
    CGLPixelFormatObj pf = NULL;
    CGLContextObj ctx = NULL;
    GLint count = 0, status = 0;
    GLuint fbo, texture, shader, program;
    unsigned char pixel[4] = {0}, storage[32 * 32 * 4];
    const char *source = "void main(){ gl_FragColor=vec4(0.0,1.0,0.0,1.0); }";
    setbuf(stdout, NULL);
    printf("pointer bits: %u\n", (unsigned)(8 * sizeof(void *)));
    CGLError error = CGLChoosePixelFormat(attrs, &pf, &count);
    printf("choose: %d, formats %d\n", error, count);
    if (error || !pf) return 1;
    error = CGLCreateContext(pf, NULL, &ctx);
    printf("create: %d\n", error);
    if (error || !ctx) return 1;
    if (!make_current || make_current(ctx)) return 1;
    printf("renderer before drawable: %s\n", glGetString(GL_RENDERER));
    if (argc == 1 && CGLSetOffScreen(ctx, 32, 32, 128, storage)) return 1;
    printf("renderer: %s\nversion: %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 32, 32, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffersEXT(1, &fbo);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
                              GL_TEXTURE_2D, texture, 0);
    if (glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) != GL_FRAMEBUFFER_COMPLETE_EXT) return 1;
    glViewport(0, 0, 32, 32);
    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    shader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status) return 1;
    program = glCreateProgram();
    glAttachShader(program, shader);
    glLinkProgram(program);
    glGetProgramiv(program, GL_LINK_STATUS, &status);
    if (!status) return 1;
    glUseProgram(program);
    glBegin(GL_QUADS);
    glVertex2f(-1,-1); glVertex2f(1,-1); glVertex2f(1,1); glVertex2f(-1,1);
    glEnd();
    glReadPixels(16,16,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
    GLenum gl_error = glGetError();
    printf("shader pixel: %u %u %u %u, GL error %x\n", pixel[0],pixel[1],pixel[2],pixel[3],gl_error);
    if (gl_error || pixel[0] || pixel[1] != 255 || pixel[2] || pixel[3] != 255) return 1;
    glUseProgram(0);
    glDeleteProgram(program); glDeleteShader(shader);
    glDeleteFramebuffersEXT(1,&fbo); glDeleteTextures(1,&texture);
    CGLSetCurrentContext(NULL); CGLDestroyContext(ctx); CGLDestroyPixelFormat(pf);
    puts("PASS");
    return 0;
}
