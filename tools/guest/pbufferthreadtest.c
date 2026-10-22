/* Importing a surface must not execute another thread's queued GL commands. */
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern CGLError CGLGetSurface(CGLContextObj, int *, int *, int *);

static CGLContextObj producer, consumer;
static CGLPBufferObj pbuffer;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static int ready, proceed, iterations = 300, worker_failed;
static unsigned imports, uploads;

static void *render(void *unused)
{
    unsigned char data[256 * 256 * 4];
    GLuint texture;
    (void)unused;
    if (CGLSetCurrentContext(producer)) _exit(2);
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    memset(data, 0x7f, sizeof(data));
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 256, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, data);
    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    glClearColor(0, 1, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    pthread_mutex_lock(&lock);
    ready = 1;
    pthread_cond_broadcast(&cond);
    while (!proceed) pthread_cond_wait(&cond, &lock);
    pthread_mutex_unlock(&lock);

    for (int i = 0; i < iterations; ++i) {
        for (int j = 0; j < 4; ++j) {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 256,
                            GL_RGBA, GL_UNSIGNED_BYTE, data);
            ++uploads;
        }
        glClearColor(i & 1, !(i & 1), 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glFlushRenderAPPLE();
    }
    glFinish();
    worker_failed = glGetError() != GL_NO_ERROR;
    glDeleteTextures(1, &texture);
    CGLSetCurrentContext(NULL);
    return NULL;
}

static int import_and_read(int sid, int route, unsigned char *pixels)
{
    GLint values[] = {sid, GL_TEXTURE_RECTANGLE_ARB, GL_RGBA, 64, 64,
                      GL_UNSIGNED_INT_8_8_8_8_REV};
    CGLError error = route ? CGLSetParameter(consumer, (CGLContextParameter)999, values) :
                            CGLTexImagePBuffer(consumer, pbuffer, GL_FRONT);
    if (error) {
        fprintf(stderr, "import route %d: CGL error %d\n", route, error);
        return 1;
    }
    glGetTexImage(GL_TEXTURE_RECTANGLE_ARB, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    if (glGetError() != GL_NO_ERROR) return 1;
    ++imports;
    return 0;
}

int main(int argc, char **argv)
{
    CGLPixelFormatAttribute attrs[] = {kCGLPFAAccelerated, kCGLPFAPBuffer,
                                      kCGLPFARendererID, 0x21a00, 0};
    CGLPixelFormatObj format;
    GLint count;
    GLuint texture;
    pthread_t thread;
    unsigned char pixels[64 * 64 * 4];
    int cid, wid, sid, failures = 0;
    setbuf(stdout, NULL);
    alarm(120);
    if (argc > 1) iterations = atoi(argv[1]);
    if (iterations < 0 || iterations > 3000) return 2;
    if (CGLChoosePixelFormat(attrs, &format, &count) ||
        CGLCreateContext(format, NULL, &producer) ||
        CGLCreateContext(format, NULL, &consumer) ||
        CGLCreatePBuffer(64, 64, GL_TEXTURE_RECTANGLE_ARB, GL_RGBA, 0, &pbuffer) ||
        CGLSetCurrentContext(producer) || CGLSetPBuffer(producer, pbuffer, 0, 0, 0) ||
        CGLGetSurface(producer, &cid, &wid, &sid)) return 2;
    printf("%u-bit renderer: %s\n", (unsigned)sizeof(void *) * 8, glGetString(GL_RENDERER));
    if (CGLSetCurrentContext(NULL) || CGLSetCurrentContext(consumer)) return 2;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_RECTANGLE_ARB, texture);
    if (pthread_create(&thread, NULL, render, NULL)) return 2;
    pthread_mutex_lock(&lock);
    while (!ready) pthread_cond_wait(&cond, &lock);
    pthread_mutex_unlock(&lock);
    for (int route = 0; route < 2; ++route) {
        failures += import_and_read(sid, route, pixels);
        for (unsigned i = 0; i < sizeof(pixels); i += 4)
            failures += pixels[i] != 255 || pixels[i + 1] != 0 ||
                        pixels[i + 2] != 0 || pixels[i + 3] != 255;
    }
    printf("producer ownership: %s (%d mismatches/errors)\n", failures ? "FAIL" : "PASS", failures);
    pthread_mutex_lock(&lock);
    if (failures) iterations = 0;
    proceed = 1;
    pthread_cond_broadcast(&cond);
    pthread_mutex_unlock(&lock);
    for (int frame = 0; frame < iterations; ++frame) {
        failures += import_and_read(sid, frame & 1, pixels);
        for (unsigned i = 0; i < sizeof(pixels); i += 4)
            failures += memcmp(pixels + i, pixels, 4) != 0 || pixels[i + 3] != 255 ||
                        pixels[i + 2] != 0 || pixels[i] + pixels[i + 1] != 255;
    }
    pthread_join(thread, NULL);
    failures += worker_failed;
    printf("%u imports, %u uploads: %s (%d mismatches/errors)\n",
           imports, uploads, failures ? "FAIL" : "PASS", failures);
    glDeleteTextures(1, &texture);
    CGLSetCurrentContext(NULL);
    CGLDestroyContext(consumer);
    CGLDestroyContext(producer);
    CGLDestroyPBuffer(pbuffer);
    CGLDestroyPixelFormat(format);
    return failures != 0;
}
