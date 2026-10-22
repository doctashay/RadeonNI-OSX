/* Build with ANGLE's headers and libEGL/libGLESv2; run against the CGL backend. */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL %s (EGL 0x%x GL 0x%x)\n", #x, eglGetError(), glGetError()); \
    return 1; \
} } while (0)

static GLuint compile(GLenum type, const char *source)
{
    GLuint shader = glCreateShader(type);
    GLint ok = 0;
    char log[2048];
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        fprintf(stderr, "%s\n", log);
        return 0;
    }
    return shader;
}

int main(void)
{
    PFNEGLGETPLATFORMDISPLAYEXTPROC platform;
    EGLDisplay display;
    EGLContext context;
    EGLSurface surface;
    EGLConfig config;
    EGLint major, minor, count;
    EGLint attrs[] = { EGL_PLATFORM_ANGLE_TYPE_ANGLE,
                      EGL_PLATFORM_ANGLE_TYPE_OPENGL_ANGLE, EGL_NONE };
    EGLint options[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
    EGLint version[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLint size[] = { EGL_WIDTH, 32, EGL_HEIGHT, 32, EGL_NONE };
    GLuint program, vertex, fragment, texture, buffers[2], vao;
    GLint ok;
    unsigned char green[4] = {0, 255, 0, 255}, pixel[4] = {0};
    unsigned char source[16384], destination[16384], *mapped;
    unsigned i;
    const char *vs = "#version 300 es\n"
        "void main(){vec2 p[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));"
        "gl_Position=vec4(p[gl_VertexID],0,1);}";
    const char *fs = "#version 300 es\nprecision highp float;"
        "uniform highp usampler2D image;out vec4 color;"
        "void main(){color=vec4(texelFetch(image,ivec2(0),0))/255.0;}";

    setbuf(stdout, NULL);
    platform = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    CHECK(platform);
    display = platform(EGL_PLATFORM_ANGLE_ANGLE, NULL, attrs);
    CHECK(display != EGL_NO_DISPLAY && eglInitialize(display, &major, &minor));
    CHECK(eglChooseConfig(display, options, &config, 1, &count) && count == 1);
    CHECK(eglBindAPI(EGL_OPENGL_ES_API));
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, version);
    CHECK(context != EGL_NO_CONTEXT);
    surface = eglCreatePbufferSurface(display, config, size);
    CHECK(surface != EGL_NO_SURFACE && eglMakeCurrent(display, surface, surface, context));
    printf("EGL %d.%d\nRenderer: %s\nVersion: %s\nGLSL: %s\n", major, minor,
           glGetString(GL_RENDERER), glGetString(GL_VERSION), glGetString(GL_SHADING_LANGUAGE_VERSION));
    CHECK(strstr((const char *)glGetString(GL_VERSION), "OpenGL ES 3."));
    CHECK(strstr((const char *)glGetString(GL_RENDERER), "TURKS") ||
          strstr((const char *)glGetString(GL_RENDERER), "Radeon"));
    vertex = compile(GL_VERTEX_SHADER, vs); fragment = compile(GL_FRAGMENT_SHADER, fs);
    CHECK(vertex && fragment);
    program = glCreateProgram(); glAttachShader(program, vertex); glAttachShader(program, fragment);
    glLinkProgram(program); glGetProgramiv(program, GL_LINK_STATUS, &ok); CHECK(ok);
    glUseProgram(program); glUniform1i(glGetUniformLocation(program, "image"), 0);
    glGenTextures(1, &texture); glBindTexture(GL_TEXTURE_2D, texture);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8UI, 1, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, green);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    glViewport(0, 0, 32, 32); glClearColor(1, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glReadPixels(16, 16, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    printf("GLSL 300 integer texture pixel: %u %u %u %u\n", pixel[0], pixel[1], pixel[2], pixel[3]);
    CHECK(!memcmp(pixel, green, 4) && glGetError() == GL_NO_ERROR);
    for (i = 0; i < sizeof(source); ++i) source[i] = (i * 17) ^ ((i >> 8) * 29);
    memset(destination, 0xa5, sizeof(destination));
    glGenBuffers(2, buffers);
    glBindBuffer(GL_COPY_READ_BUFFER, buffers[0]);
    glBufferData(GL_COPY_READ_BUFFER, sizeof(source), source, GL_STREAM_DRAW);
    glBindBuffer(GL_COPY_WRITE_BUFFER, buffers[1]);
    glBufferData(GL_COPY_WRITE_BUFFER, sizeof(destination), destination, GL_STREAM_DRAW);
    glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0xff0, 0x430, 0x1a10);
    memcpy(destination + 0x430, source + 0xff0, 0x1a10);
    mapped = glMapBufferRange(GL_COPY_WRITE_BUFFER, 0, sizeof(destination), GL_MAP_READ_BIT);
    CHECK(mapped && !memcmp(mapped, destination, sizeof(destination)));
    CHECK(glUnmapBuffer(GL_COPY_WRITE_BUFFER) && glGetError() == GL_NO_ERROR);
    glDeleteBuffers(2, buffers); glDeleteTextures(1, &texture); glDeleteVertexArrays(1, &vao);
    glDeleteProgram(program); glDeleteShader(vertex); glDeleteShader(fragment);
    CHECK(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
    CHECK(eglDestroySurface(display, surface) && eglDestroyContext(display, context));
    CHECK(eglTerminate(display));
    puts("PASS: ES3 context, GLSL 300 integer texture rendering and GPU buffer copy/readback");
    return 0;
}
