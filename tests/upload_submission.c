/* Shader upload and asynchronous submission regression. SPDX-License-Identifier: MIT */
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
API(void, Uniform4fv, (GLint, GLsizei, const GLfloat *));
API(void, UniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat *));
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


int main(void)
{
    CGLPixelFormatAttribute attrs[] = { kCGLPFAOffScreen, kCGLPFAColorSize, 32,
        kCGLPFARendererID, 0x21b00, 0 };
    CGLPixelFormatObj format = NULL;
    CGLContextObj ctx = NULL;
    GLint count = 0, ok = 0, values_location, matrix_location;
    GLuint vertex, fragment, program, texture, fbo;
    unsigned char backing[64 * 64 * 4], pixels[32 * 8 * 4];
    GLfloat values[32 * 4], matrix[16];
    unsigned batch, row, x, c, checks = 0;
    void *driver = dlopen("/System/Library/Extensions/RadeonNIGLDriver1.bundle/Contents/MacOS/RadeonNIGLDriver1", RTLD_NOW | RTLD_LOCAL);
    int (*request)(int, int) = driver ? dlsym(driver, "RDNGLRequestCompatibilityContext") : NULL;
    lookup = driver ? dlsym(driver, "RDNGLGetProcAddress") : NULL;
    bind_context = driver ? dlsym(driver, "RDNGLMakeCurrent") : NULL;
    CHECK(request && lookup && bind_context && request(4, 6));
    CHECK(!CGLChoosePixelFormat(attrs, &format, &count));
    CHECK(!CGLCreateContext(format, NULL, &ctx));
    CHECK(!CGLSetOffScreen(ctx, 64, 64, 256, backing)); CHECK(current(ctx));
    LOAD(CreateShader); LOAD(ShaderSource); LOAD(CompileShader); LOAD(GetShaderiv);
    LOAD(CreateProgram); LOAD(AttachShader); LOAD(LinkProgram); LOAD(GetProgramiv);
    LOAD(UseProgram); LOAD(GetUniformLocation); LOAD(Uniform4fv); LOAD(UniformMatrix4fv);
    LOAD(GenTextures); LOAD(BindTexture); LOAD(TexImage2D); LOAD(GenFramebuffers);
    LOAD(BindFramebuffer); LOAD(FramebufferTexture2D); LOAD(CheckFramebufferStatus);
    LOAD(Viewport); LOAD(DrawArrays); LOAD(ReadPixels); LOAD(GetError); LOAD(Flush);
    vertex = compile(GL_VERTEX_SHADER,
        "#version 330\nvoid main(){vec2 p[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));"
        "gl_Position=vec4(p[gl_VertexID],0,1);}");
    fragment = compile(GL_FRAGMENT_SHADER,
        "#version 330\nuniform vec4 values[32];uniform mat4 transform;out vec4 color;"
        "void main(){color=values[int(gl_FragCoord.x)]*transform[2][1];}");
    CHECK(vertex && fragment);
    program = CreateProgram(); AttachShader(program, vertex); AttachShader(program, fragment);
    LinkProgram(program); GetProgramiv(program, GL_LINK_STATUS, &ok); CHECK(ok);
    UseProgram(program);
    values_location = GetUniformLocation(program, "values[0]");
    matrix_location = GetUniformLocation(program, "transform");
    CHECK(values_location >= 0 && matrix_location >= 0);
    GenTextures(1, &texture); BindTexture(GL_TEXTURE_2D, texture);
    TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 32, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    GenFramebuffers(1, &fbo); BindFramebuffer(0x8d40, fbo);
    FramebufferTexture2D(0x8d40, 0x8ce0, GL_TEXTURE_2D, texture, 0);
    CHECK(CheckFramebufferStatus(0x8d40) == 0x8cd5);
    for (batch = 0; batch < 32; ++batch) {
        for (row = 0; row < 8; ++row) {
            for (x = 0; x < 32; ++x)
                for (c = 0; c < 4; ++c)
                    values[x * 4 + c] = ((batch * 29 + row * 17 + x * 7 + c * 43) % 256) / 255.0f;
            memset(matrix, 0, sizeof(matrix)); matrix[9] = row & 1 ? 0.5f : 1.0f;
            Uniform4fv(values_location, 32, values);
            UniformMatrix4fv(matrix_location, 1, GL_FALSE, matrix);
            Viewport(0, row, 32, 1); DrawArrays(GL_TRIANGLES, 0, 3); Flush();
        }
        ReadPixels(0, 0, 32, 8, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        CHECK(GetError() == GL_NO_ERROR);
        for (row = 0; row < 8; ++row)
            for (x = 0; x < 32; ++x)
                for (c = 0; c < 4; ++c) {
                    int value = (batch * 29 + row * 17 + x * 7 + c * 43) % 256;
                    int expected = row & 1 ? (value + 1) / 2 : value;
                    CHECK(abs(pixels[(row * 32 + x) * 4 + c] - expected) <= 1);
                    ++checks;
                }
    }
    CHECK(current(NULL)); CGLDestroyContext(ctx); CGLDestroyPixelFormat(format);
    printf("PASS: %u pixel-channel checks across 256 flushed draws\n", checks);
    return 0;
}
