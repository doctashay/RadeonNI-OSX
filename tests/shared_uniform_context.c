/* Native PPC64: gcc-4.2 -arch ppc64 shared_uniform_context.c -framework OpenGL -o shared_uniform_context */
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define API(ret, name, args) static ret (*name) args
API(GLuint, CreateShader, (GLenum));
API(void, ShaderSource, (GLuint, GLsizei, const char **, const GLint *));
API(void, CompileShader, (GLuint));
API(void, GetShaderiv, (GLuint, GLenum, GLint *));
API(GLuint, CreateProgram, (void));
API(void, AttachShader, (GLuint, GLuint));
API(void, LinkProgram, (GLuint));
API(void, GetProgramiv, (GLuint, GLenum, GLint *));
API(void, UseProgram, (GLuint));
API(GLint, GetUniformLocation, (GLuint, const char *));
API(void, Uniform1f, (GLint, GLfloat));
API(void, GetUniformfv, (GLuint, GLint, GLfloat *));
API(void, GenTextures, (GLsizei, GLuint *));
API(void, BindTexture, (GLenum, GLuint));
API(void, TexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *));
API(void, GenFramebuffers, (GLsizei, GLuint *));
API(void, BindFramebuffer, (GLenum, GLuint));
API(void, FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint));
API(GLenum, CheckFramebufferStatus, (GLenum));
API(void, Viewport, (GLint, GLint, GLsizei, GLsizei));
API(void, DrawArrays, (GLenum, GLint, GLsizei));
API(void, ReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *));
API(GLenum, GetError, (void));
API(void, Flush, (void));
static void *(*lookup)(const char *);
static int (*bind_context)(void *);
#define LOAD(name) do { name = lookup("gl" #name); if (!name) return 2; } while (0)
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL: %s\n", #x); return 2; } } while (0)

static GLuint compile(GLenum type, const char *text)
{
    GLuint shader = CreateShader(type);
    GLint ok = 0;
    ShaderSource(shader, 1, &text, NULL);
    CompileShader(shader);
    GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    return ok ? shader : 0;
}

static int current(CGLContextObj ctx)
{
    return CGLSetCurrentContext(ctx) == kCGLNoError && bind_context(ctx);
}

int main(int argc, char **argv)
{
    CGLPixelFormatAttribute attrs[] = {
        kCGLPFAOffScreen, kCGLPFAColorSize, 32,
        kCGLPFARendererID, 0x21a00, 0
    };
    CGLPixelFormatObj format = NULL;
    CGLContextObj producer = NULL, consumer = NULL;
    GLint count = 0, ok = 0, opacity;
    GLuint vertex, fragment, program, texture, fbo;
    unsigned char backing[2][64 * 64 * 4], pixel[4];
    int i, failures = 0, force = argc == 2 && !strcmp(argv[1], "--force-update");
    void *driver = dlopen("/System/Library/Extensions/RadeonNIGLDriver.bundle/Contents/MacOS/RadeonNIGLDriver", RTLD_NOW | RTLD_LOCAL);
    int (*request)(int, int) = driver ? dlsym(driver, "RDNGLRequestCompatibilityContext") : NULL;
    lookup = driver ? dlsym(driver, "RDNGLGetProcAddress") : NULL;
    bind_context = driver ? dlsym(driver, "RDNGLMakeCurrent") : NULL;
    CHECK(request && lookup && bind_context && request(4, 6));
    CHECK(!CGLChoosePixelFormat(attrs, &format, &count));
    CHECK(!CGLCreateContext(format, NULL, &producer));
    CHECK(!CGLSetOffScreen(producer, 64, 64, 256, backing[0]));
    CHECK(current(producer));
    LOAD(CreateShader); LOAD(ShaderSource); LOAD(CompileShader); LOAD(GetShaderiv);
    LOAD(CreateProgram); LOAD(AttachShader); LOAD(LinkProgram); LOAD(GetProgramiv);
    LOAD(UseProgram); LOAD(GetUniformfv); LOAD(GetUniformLocation); LOAD(Uniform1f); LOAD(GenTextures);
    LOAD(BindTexture); LOAD(TexImage2D); LOAD(GenFramebuffers); LOAD(BindFramebuffer);
    LOAD(FramebufferTexture2D); LOAD(CheckFramebufferStatus); LOAD(Viewport);
    LOAD(DrawArrays); LOAD(ReadPixels); LOAD(GetError); LOAD(Flush);
    vertex = compile(GL_VERTEX_SHADER,
        "#version 330\nvoid main(){vec2 p[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));"
        "gl_Position=vec4(p[gl_VertexID],0,1);}");
    fragment = compile(GL_FRAGMENT_SHADER,
        "#version 330\nuniform float opacity;out vec4 color;void main(){color=vec4(opacity);}");
    CHECK(vertex && fragment);
    program = CreateProgram();
    AttachShader(program, vertex); AttachShader(program, fragment); LinkProgram(program);
    GetProgramiv(program, GL_LINK_STATUS, &ok); CHECK(ok);
    UseProgram(program);
    opacity = GetUniformLocation(program, "opacity"); CHECK(opacity >= 0);
    CHECK(!CGLCreateContext(format, producer, &consumer));
    CHECK(!CGLSetOffScreen(consumer, 64, 64, 256, backing[1]));
    CHECK(current(consumer));
    UseProgram(program);
    GenTextures(1, &texture); BindTexture(GL_TEXTURE_2D, texture);
    TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    GenFramebuffers(1, &fbo); BindFramebuffer(0x8d40, fbo);
    FramebufferTexture2D(0x8d40, 0x8ce0, GL_TEXTURE_2D, texture, 0);
    CHECK(CheckFramebufferStatus(0x8d40) == 0x8cd5);
    Viewport(0, 0, 64, 64);
    Uniform1f(opacity, 0); DrawArrays(GL_TRIANGLES, 0, 3);
    ReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    for (i = 1; i <= 63; ++i) {
        GLfloat value = i / 63.0f;
        int expected = (int)(value * 255 + 0.5f);
        GLfloat reported = -1;
        CHECK(current(producer)); UseProgram(program); Uniform1f(opacity, value); Flush();
        CHECK(current(consumer)); UseProgram(program);
        if (force) Uniform1f(opacity, -1);
        Uniform1f(opacity, value);
        GetUniformfv(program, opacity, &reported); CHECK(reported == value);
        DrawArrays(GL_TRIANGLES, 0, 3);
        ReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        CHECK(GetError() == GL_NO_ERROR);
        if (abs(pixel[3] - expected) > 1) {
            if (failures < 4) printf("opacity %.3f: expected %d, got %u\n", value, expected, pixel[3]);
            ++failures;
        }
    }
    printf("%s: %d/63 stale opacity results%s\n", failures ? "FAIL" : "PASS", failures,
           force ? " (forced value change control)" : "");
    CHECK(current(NULL));
    CGLDestroyContext(consumer); CGLDestroyContext(producer); CGLDestroyPixelFormat(format);
    return failures ? 1 : 0;
}