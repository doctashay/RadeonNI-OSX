/* Exercise CGL or a named librdngl, including copies spanning submissions. */
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    unsigned iterations = argc > 1 ? strtoul(argv[1], NULL, 0) : 100;
    unsigned capacity = argc > 2 ? strtoul(argv[2], NULL, 0) : 65536;
    CGLPixelFormatAttribute attrs[] = { kCGLPFAAccelerated, kCGLPFANoRecovery,
                                       kCGLPFARendererID, 0x21a00, 0 };
    CGLPixelFormatObj format = NULL;
    CGLContextObj context = NULL;
    GLint count;
    GLuint buffers[2];
    unsigned char *source, *destination, *actual;
    unsigned frame, i;
    void (*copy)(GLenum, GLenum, long, long, long);
    void *library;
    void *mesa_context = NULL;
    void *(*get_proc)(const char *) = NULL;
    void (*destroy)(void *) = NULL;
    unsigned char pixel[4];
    struct {
        __typeof__(&glGetString) GetString;
        __typeof__(&glGenBuffers) GenBuffers;
        __typeof__(&glBindBuffer) BindBuffer;
        __typeof__(&glBufferData) BufferData;
        __typeof__(&glGetBufferSubData) GetBufferSubData;
        __typeof__(&glGetError) GetError;
        __typeof__(&glDeleteBuffers) DeleteBuffers;
    } gl;

    if (!iterations || iterations > 10000 || capacity < 65536 || capacity > 4194304)
        return 2;
    setbuf(stdout, NULL);
    source = malloc(capacity); destination = malloc(capacity); actual = malloc(capacity);
    if (!source || !destination || !actual) return 2;
    if (argc > 3) {
        const int attributes[] = {0x22, 1, 0x33, 0x34, 0x36, 4, 0x37, 6, 0};
        void *(*create)(const int *, void *);
        int (*current)(void *, void *, GLenum, int, int);
        library = dlopen(argv[3], RTLD_NOW | RTLD_LOCAL);
        if (!library) { fprintf(stderr, "%s\n", dlerror()); return 2; }
        create = dlsym(library, "OSMesaCreateContextAttribs");
        current = dlsym(library, "OSMesaMakeCurrent");
        destroy = dlsym(library, "OSMesaDestroyContext");
        get_proc = dlsym(library, "OSMesaGetProcAddress");
        if (!create || !current || !destroy || !get_proc) return 2;
        mesa_context = create(attributes, NULL);
        if (!mesa_context || !current(mesa_context, pixel, GL_UNSIGNED_BYTE, 1, 1))
            return 2;
    } else {
        if (CGLChoosePixelFormat(attrs, &format, &count) ||
            CGLCreateContext(format, NULL, &context) || CGLSetCurrentContext(context))
            return 2;
        library = dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_LAZY);
        if (!library) return 2;
    }
#define RESOLVE(name) (get_proc ? get_proc(name) : dlsym(library, name))
#define LOAD(name) do { gl.name = RESOLVE("gl" #name); if (!gl.name) return 2; } while (0)
    LOAD(GetString); LOAD(GenBuffers); LOAD(BindBuffer); LOAD(BufferData);
    LOAD(GetBufferSubData); LOAD(GetError); LOAD(DeleteBuffers);
    copy = RESOLVE("glCopyBufferSubData");
#undef LOAD
#undef RESOLVE
    if (!copy) return 2;
    printf("%u-bit %s renderer: %s\n", (unsigned)sizeof(void *) * 8,
           mesa_context ? "librdngl" : "CGL", gl.GetString(GL_RENDERER));
    gl.GenBuffers(2, buffers);
    for (frame = 0; frame < iterations; frame++) {
        unsigned so = frame ? (frame * 37) & 8191 : 0xff0;
        unsigned to = frame ? (frame * 13) & 8191 : 0x430;
        unsigned bytes = frame ? 1 + ((frame * 65537u) % (capacity - 8192)) : 0x1a10;
        for (i = 0; i < capacity; i++) {
            source[i] = ((i * 17) ^ ((i >> 8) * 29) ^ (frame * 31)) & 255;
            destination[i] = 0xa5;
        }
        gl.BindBuffer(0x8f36, buffers[0]);
        gl.BufferData(0x8f36, capacity, source, GL_STREAM_DRAW);
        gl.BindBuffer(0x8f37, buffers[1]);
        gl.BufferData(0x8f37, capacity, destination, GL_STREAM_DRAW);
        copy(0x8f36, 0x8f37, so, to, bytes);
        gl.GetBufferSubData(0x8f37, 0, capacity, actual);
        GLenum error = gl.GetError();
        if (error) { printf("GL error 0x%x at frame %u\n", error, frame); return 1; }
        memcpy(destination + to, source + so, bytes);
        for (i = 0; i < capacity; i++) {
            if (actual[i] != destination[i]) {
                printf("MISMATCH frame %u byte %u: got %u expected %u\n",
                       frame, i, actual[i], destination[i]);
                return 1;
            }
        }
        if (!(frame % 100)) printf("checked %u copies\n", frame + 1);
    }
    gl.DeleteBuffers(2, buffers);
    if (mesa_context) destroy(mesa_context);
    else {
        CGLSetCurrentContext(NULL); CGLDestroyContext(context); CGLDestroyPixelFormat(format);
    }
    free(actual); free(destination); free(source);
    printf("PASS %u GPU buffer copies with byte and guard checks\n", iterations);
    return 0;
}
