/* Preserve user FBO state and restore framebuffer zero across thread migration. */
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static CGLContextObj context;
static GLuint framebuffer;
static int modern, surfaceless, worker_result;
static unsigned phase;
static int (*bind_mesa)(void *);
static void *(*lookup)(const char *);
static CGLError (*set_current)(CGLContextObj);
static void (*bind_framebuffer)(GLenum, GLuint);
static GLenum (*framebuffer_status)(GLenum);
static void (*get_integer)(GLenum, GLint *);
static void (*clear_color)(GLfloat, GLfloat, GLfloat, GLfloat);
static void (*clear)(GLbitfield);
static void (*read_pixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
static GLenum (*get_error)(void);

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL phase %u: %s\n", phase, #x); return 1; \
} } while (0)

static int make_current(CGLContextObj ctx)
{
    CHECK(!set_current(ctx));
    if (modern) CHECK(bind_mesa(ctx));
    return 0;
}

static int pixel(unsigned red, unsigned green)
{
    unsigned char p[4] = {0};
    read_pixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
    if (p[0] != red || p[1] != green || p[2] != 0 || p[3] != 255) {
        fprintf(stderr, "pixel %u,%u,%u,%u expected %u,%u,0,255\n",
                p[0], p[1], p[2], p[3], red, green);
        return 1;
    }
    return 0;
}

static int roundtrip(void)
{
    GLint bound = 0;
    get_integer(GL_FRAMEBUFFER_BINDING_EXT, &bound);
    CHECK((GLuint)bound == framebuffer);
    CHECK(!pixel((phase & 1) ? 0 : 255, (phase & 1) ? 255 : 0));
    bind_framebuffer(GL_FRAMEBUFFER_EXT, 0);
    CHECK(framebuffer_status(GL_FRAMEBUFFER_EXT) == GL_FRAMEBUFFER_COMPLETE_EXT);
    clear_color(0, 1, 0, 1);
    clear(GL_COLOR_BUFFER_BIT);
    CHECK(!pixel(0, 255));
    if (!surfaceless) CHECK(!CGLFlushDrawable(context));
    CHECK(get_error() == GL_NO_ERROR);
    bind_framebuffer(GL_FRAMEBUFFER_EXT, framebuffer);
    ++phase;
    clear_color((phase & 1) ? 0 : 1, (phase & 1) ? 1 : 0, 0, 1);
    clear(GL_COLOR_BUFFER_BIT);
    CHECK(get_error() == GL_NO_ERROR);
    return 0;
}

static void *worker(void *unused)
{
    (void)unused;
    worker_result = make_current(context);
    if (!worker_result) worker_result = roundtrip();
    if (make_current(NULL)) worker_result = 1;
    return NULL;
}

int main(int argc, char **argv)
{
    CGLPixelFormatAttribute attrs[] = { kCGLPFAAccelerated, kCGLPFANoRecovery,
        kCGLPFAOffScreen, kCGLPFARendererID, 0x21a00, 0 };
    CGLPixelFormatObj format = NULL;
    GLint count;
    GLuint texture;
    unsigned char storage[32 * 32 * 4] = {0};
    unsigned i, iterations = 100;
    pthread_t thread;
    void *driver, *library;
    int (*request)(int, int);
    void (*gen_framebuffers)(GLsizei, GLuint *);
    void (*attach_texture)(GLenum, GLenum, GLenum, GLuint, GLint);

    setbuf(stdout, NULL);
    modern = argc > 1 && !strcmp(argv[1], "modern");
    if (argc > 2) iterations = strtoul(argv[2], NULL, 0);
    surfaceless = argc > 3 && !strcmp(argv[3], "surfaceless");
    CHECK(iterations > 0 && iterations <= 10000);
    driver = dlopen("/System/Library/Extensions/RadeonNIGLDriver.bundle/Contents/MacOS/RadeonNIGLDriver", RTLD_NOW | RTLD_LOCAL);
    if (!driver) fprintf(stderr, "%s\n", dlerror());
    CHECK(driver);
    request = dlsym(driver, "RDNGLRequestCompatibilityContext");
    bind_mesa = dlsym(driver, "RDNGLMakeCurrent");
    if (modern) {
        CHECK(request && bind_mesa && request(4, 6));
        lookup = dlsym(driver, "RDNGLGetProcAddress");
        CHECK(lookup);
    }
    CHECK(!CGLChoosePixelFormat(attrs, &format, &count));
    CHECK(!CGLCreateContext(format, NULL, &context));
    library = dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_NOW);
    CHECK(library);
    set_current = dlsym(library, "CGLSetCurrentContext");
    CHECK(set_current && !make_current(context));
    if (!surfaceless) CHECK(!CGLSetOffScreen(context, 32, 32, 128, storage));
    printf("%u-bit %s: %s / %s\n", (unsigned)sizeof(void *) * 8,
           modern ? "modern" : "legacy", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    if (modern) CHECK(bind_mesa(context));
#define PROC(name) (modern ? lookup(name) : dlsym(library, name))
    bind_framebuffer = PROC(modern ? "glBindFramebuffer" : "glBindFramebufferEXT");
    gen_framebuffers = PROC(modern ? "glGenFramebuffers" : "glGenFramebuffersEXT");
    framebuffer_status = PROC(modern ? "glCheckFramebufferStatus" : "glCheckFramebufferStatusEXT");
    attach_texture = PROC(modern ? "glFramebufferTexture2D" : "glFramebufferTexture2DEXT");
    get_integer = PROC("glGetIntegerv");
    clear_color = PROC("glClearColor"); clear = PROC("glClear");
    read_pixels = PROC("glReadPixels"); get_error = PROC("glGetError");
    CHECK(bind_framebuffer && gen_framebuffers && framebuffer_status && attach_texture &&
          get_integer && clear_color && clear && read_pixels && get_error);
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 32, 32, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    gen_framebuffers(1, &framebuffer);
    bind_framebuffer(GL_FRAMEBUFFER_EXT, framebuffer);
    attach_texture(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, texture, 0);
    CHECK(framebuffer_status(GL_FRAMEBUFFER_EXT) == GL_FRAMEBUFFER_COMPLETE_EXT);
    clear_color(1, 0, 0, 1); clear(GL_COLOR_BUFFER_BIT);
    CHECK(!make_current(NULL));
    for (i = 0; i < iterations; ++i) {
        CHECK(!pthread_create(&thread, NULL, worker, NULL));
        CHECK(!pthread_join(thread, NULL) && !worker_result);
        CHECK(!make_current(context) && !roundtrip() && !make_current(NULL));
    }
    CHECK(!CGLDestroyContext(context));
    CHECK(!CGLDestroyPixelFormat(format));
    printf("PASS: %u thread handoffs, preserved FBO pixels, framebuffer-zero rendering%s\n",
           phase, surfaceless ? " (surfaceless)" : "/presentation");
    return 0;
}
