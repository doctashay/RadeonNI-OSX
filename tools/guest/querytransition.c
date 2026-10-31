#include <Carbon/Carbon.h>
#include <AGL/agl.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <mach-o/dyld.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s GL=%x\n", __LINE__, #x, glGetError()); return 1; } } while (0)

static void *proc(const char *name)
{
    void *address = dlsym(RTLD_DEFAULT, name);
    if (address) return address;
    for (unsigned i = 0; i < _dyld_image_count(); ++i) {
        const char *path = _dyld_get_image_name(i);
        if (!strstr(path, "RadeonNIGLDriver")) continue;
        void *handle = dlopen(path, RTLD_LAZY | RTLD_NOLOAD);
        if (!handle) continue;
        void *(*lookup)(const char *) = dlsym(handle, "RDNGLGetProcAddress");
        if (lookup) {
            const GLubyte *(*get_string)(GLenum) = lookup("glGetString");
            if (get_string && get_string(GL_VERSION)) address = lookup(name);
        }
        dlclose(handle);
        if (address) return address;
    }
    return NULL;
}

static GLuint shader(GLenum type, const char *source)
{
    GLuint object = glCreateShader(type);
    glShaderSource(object, 1, &source, NULL);
    glCompileShader(object);
    GLint ok = 0;
    glGetShaderiv(object, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(object, sizeof(log), NULL, log);
        fprintf(stderr, "%s\n", log);
        exit(2);
    }
    return object;
}

static GLuint program(const char *fragment)
{
    const char *vertex = "#version 120\nvarying vec2 uv; void main(){gl_Position=gl_Vertex;uv=gl_Vertex.xy*0.5+0.5;}";
    GLuint vs = shader(GL_VERTEX_SHADER, vertex), fs = shader(GL_FRAGMENT_SHADER, fragment);
    GLuint object = glCreateProgram();
    glAttachShader(object, vs); glAttachShader(object, fs); glLinkProgram(object);
    GLint ok = 0; glGetProgramiv(object, GL_LINK_STATUS, &ok);
    if (!ok) exit(3);
    glDeleteShader(vs); glDeleteShader(fs);
    return object;
}

static void quads(unsigned count, float z)
{
    glBegin(GL_QUADS);
    for (unsigned i = 0; i < count; ++i) {
        glVertex3f(-1,-1,z); glVertex3f(1,-1,z);
        glVertex3f(1,1,z); glVertex3f(-1,1,z);
    }
    glEnd();
}

int main(int argc, char **argv)
{
    const char *mode_text = getenv("RDN_QUERY_PROBE_MODE");
    const char *rounds_text = getenv("RDN_QUERY_PROBE_ROUNDS");
    const unsigned mode = argc > 1 && argv[1][0]!='-' ? strtoul(argv[1], NULL, 0) : mode_text ? strtoul(mode_text,NULL,0) : 1;
    const unsigned rounds = argc > 2 && argv[2][0]!='-' ? strtoul(argv[2], NULL, 0) : rounds_text ? strtoul(rounds_text,NULL,0) : 1024;
    const char *log = getenv("RDN_QUERY_PROBE_LOG");
    if (log) { if (!freopen(log,"w",stdout)) return 2; dup2(fileno(stdout),fileno(stderr)); }
    const unsigned size = 128;
    const int use_queries = mode && mode != 11;
    const int conditional = mode == 8 || mode == 9 || mode == 14 || mode == 15;
    setbuf(stdout, NULL);
    printf("pid=%d\n",getpid());
    ProcessSerialNumber process = {0, kCurrentProcess};
    CHECK(TransformProcessType(&process, kProcessTransformToForegroundApplication) == noErr);
    Rect bounds = {80,80,208,208}; WindowRef window;
    CHECK(CreateNewWindow(kDocumentWindowClass, kWindowStandardHandlerAttribute, &bounds, &window) == noErr);
    SetWindowTitleWithCFString(window, CFSTR("RadeonNI query transition test"));
    GLint attributes[] = {AGL_RGBA,AGL_DOUBLEBUFFER,AGL_ACCELERATED,AGL_DEPTH_SIZE,24,AGL_STENCIL_SIZE,8,AGL_RENDERER_ID,0x21b00,AGL_NONE};
    AGLPixelFormat format = aglChoosePixelFormat(NULL,0,attributes);
    CHECK(format);
    AGLContext context = aglCreateContext(format,NULL); aglDestroyPixelFormat(format);
    CHECK(context && aglSetDrawable(context,GetWindowPort(window)) && aglSetCurrentContext(context));
    ShowWindow(window);
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    printf("renderer=%s version=%s mode=%u rounds=%u\n",renderer,glGetString(GL_VERSION),mode,rounds);
    CHECK(renderer && strstr(renderer,"V7900"));
    void (*mask)(GLuint,GLboolean,GLboolean,GLboolean,GLboolean) = proc("glColorMaski");
    CHECK(mask);
    GLuint framebuffer, colors[4], depth, queries[4], empty_queries[4];
    void (*begin_conditional)(GLuint, GLenum) = proc("glBeginConditionalRender");
    void (*end_conditional)(void) = proc("glEndConditionalRender");
    if (conditional) CHECK(begin_conditional && end_conditional);
    const GLenum query_target = mode == 9 ? 0x8c2f : GL_SAMPLES_PASSED;
    glGenFramebuffersEXT(1,&framebuffer); glBindFramebufferEXT(GL_FRAMEBUFFER_EXT,framebuffer);
    glGenRenderbuffersEXT(4,colors);
    for (unsigned i=0;i<4;++i) {
        glBindRenderbufferEXT(GL_RENDERBUFFER_EXT,colors[i]);
        glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT,GL_RGBA8,size,size);
        glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT,GL_COLOR_ATTACHMENT0_EXT+i,GL_RENDERBUFFER_EXT,colors[i]);
    }
    glGenRenderbuffersEXT(1,&depth); glBindRenderbufferEXT(GL_RENDERBUFFER_EXT,depth);
    glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT,GL_DEPTH24_STENCIL8_EXT,size,size);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT,GL_DEPTH_ATTACHMENT_EXT,GL_RENDERBUFFER_EXT,depth);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT,GL_STENCIL_ATTACHMENT_EXT,GL_RENDERBUFFER_EXT,depth);
    GLenum draw_buffers[] = {GL_COLOR_ATTACHMENT0_EXT,GL_COLOR_ATTACHMENT1_EXT,GL_COLOR_ATTACHMENT2_EXT,GL_COLOR_ATTACHMENT3_EXT};
    glDrawBuffers(4,draw_buffers);
    CHECK(glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT)==GL_FRAMEBUFFER_COMPLETE_EXT);
    GLuint fill = program("#version 120\nvarying vec2 uv; void main(){gl_FragData[0]=vec4(uv,0,1);gl_FragData[1]=vec4(0.5,0.5,1,1);gl_FragData[2]=vec4(0.2);gl_FragData[3]=vec4(1);}");
    if (mode == 13) {
        void (*clip_control)(GLenum, GLenum) = proc("glClipControl");
        CHECK(clip_control);
        clip_control(0x8ca1, 0x935f);
        glViewport(0,0,size,size); glEnable(GL_DEPTH_TEST); glDepthFunc(GL_ALWAYS);
        glDepthMask(GL_TRUE); glClearDepth(1); glClear(GL_DEPTH_BUFFER_BIT);
        glUseProgram(fill); quads(1,0.25f); glFinish();
        float sampled_depth = -1;
        glReadPixels(64,64,1,1,GL_DEPTH_COMPONENT,GL_FLOAT,&sampled_depth);
        printf("zero-to-one depth=%g expected=0.25 error=%x\n",sampled_depth,glGetError());
        CHECK(sampled_depth > 0.24999f && sampled_depth < 0.25001f);
        printf("PASS clip control\n");
        return 0;
    }
    if (mode == 17) {
        GLuint buffers[64], dynamic_buffer;
        const unsigned bytes = 16 * 1024 * 1024;
        glGenBuffers(64,buffers);
        for (unsigned i=0;i<64;++i) {
            unsigned expected[4] = {i,0x12345678,0x89abcdef,~i}, actual[4];
            glBindBuffer(0x8f37,buffers[i]);
            glBufferData(0x8f37,bytes,NULL,GL_STATIC_DRAW);
            CHECK(glGetError()==GL_NO_ERROR);
            glBufferSubData(0x8f37,0,sizeof(expected),expected);
            glBufferSubData(0x8f37,bytes-sizeof(expected),sizeof(expected),expected);
            glGetBufferSubData(0x8f37,bytes-sizeof(expected),sizeof(actual),actual);
            CHECK(!memcmp(expected,actual,sizeof(actual)) && glGetError()==GL_NO_ERROR);
        }
        glGenBuffers(1,&dynamic_buffer);glBindBuffer(0x8f37,dynamic_buffer);
        glBufferData(0x8f37,64*1024*1024,NULL,GL_DYNAMIC_DRAW);
        CHECK(glGetError()==GL_NO_ERROR);
        glFinish();glDeleteBuffers(64,buffers);glDeleteBuffers(1,&dynamic_buffer);
        printf("PASS static=1024MB dynamic=64MB readback=64\n");
        return 0;
    }
    char source[512];
    snprintf(source,sizeof(source),"#version 120\nvarying vec2 uv; void main(){%sgl_FragData[%u]=vec4(uv,0.5,1);}",
             mode==2 ? "" : "if(uv.x<0.5)discard;",(mode==3 || mode==4 || mode==5 || mode>=10) ? 0 : 1);
    GLuint alpha = program(source);
    GLuint index_buffer = 0, vertex_buffer = 0;
    if (mode >= 10) {
        const GLfloat vertices[16] = {-1,-1,-0.1f,1, 1,-1,-0.1f,1, 1,1,-0.1f,1, -1,1,-0.1f,1};
        GLushort indices[888];
        const GLushort face[6] = {0,1,2,0,2,3};
        for (unsigned i = 0; i < 888; ++i) indices[i] = face[i % 6];
        glGenBuffers(1, &vertex_buffer); glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
        glVertexPointer(4, GL_FLOAT, 0, NULL); glEnableClientState(GL_VERTEX_ARRAY);
        glGenBuffers(1, &index_buffer); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, index_buffer);
        if (mode == 16) {
            GLubyte expected[sizeof(indices)], actual[sizeof(indices)];
            const GLubyte patch[5] = {0x12,0x34,0x56,0x78,0x9a};
            memcpy(expected,indices,sizeof(indices));
            glBindBuffer(0x8f37,index_buffer);
            glBufferData(0x8f37,sizeof(indices),indices,GL_STATIC_DRAW);
            glBufferSubData(0x8f37,3,sizeof(patch),patch);
            memcpy(expected+3,patch,sizeof(patch));
            glGetBufferSubData(0x8f37,0,sizeof(actual),actual);
            CHECK(!memcmp(expected,actual,sizeof(actual)));
            GLubyte *mapped = glMapBuffer(0x8f37,GL_READ_WRITE);
            CHECK(mapped && !memcmp(mapped,expected,sizeof(expected)));
            mapped[7] ^= 0x5a; expected[7] ^= 0x5a;
            CHECK(glUnmapBuffer(0x8f37));
            glGetBufferSubData(0x8f37,0,sizeof(actual),actual);
            CHECK(!memcmp(expected,actual,sizeof(actual)));
            glBufferSubData(0x8f37,0,sizeof(indices),indices);
            CHECK(glGetError()==GL_NO_ERROR);
        } else glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STREAM_DRAW);
    }
    glGenQueries(4,queries); glGenQueries(4,empty_queries); glViewport(0,0,size,size);
    glEnable(GL_DEPTH_TEST); glEnable(GL_STENCIL_TEST);
    if (mode==6 || mode==7) glDisable(GL_STENCIL_TEST);
    if (mode==7) glDisable(GL_DEPTH_TEST);
    glStencilFunc(GL_ALWAYS,1,255); glStencilOp(GL_KEEP,GL_KEEP,GL_KEEP);
    glClearDepth(1); glClearStencil(0);
    for (unsigned round=0;round<rounds+4;++round) {
        unsigned slot = round%4;
        if (round>=4 && use_queries) {
            GLuint result = 0;
            glGetQueryObjectuiv(queries[slot],GL_QUERY_RESULT,&result);
            unsigned expected = mode == 9 ? 1 : size*size*6/(mode==2 ? 1 : 2);
            if (result!=expected) { fprintf(stderr,"FAIL query round=%u result=%u expected=%u\n",round-4,result,expected); return 4; }
        }
        if (round>=rounds) continue;
        for (unsigned i=0;i<4;++i) mask(i,1,1,1,1);
        glDepthMask(GL_TRUE); glDepthFunc(GL_LEQUAL); glDisable(GL_BLEND); glClearStencil(0);
        glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);
        glUseProgram(fill); quads(1,0.2f);
        for (unsigned i=0;i<4;++i) {
            GLboolean enabled = mode!=5 && i==(mode==3 ? 0u : 1u);
            mask(i,enabled,enabled,enabled,enabled);
        }
        glDepthMask(GL_FALSE); glUseProgram(alpha);
        if (use_queries) glBeginQuery(query_target,queries[slot]);
        quads(6,0);
        if (use_queries) glEndQuery(query_target);
        glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_TRUE);
        if (conditional) begin_conditional(queries[slot], mode == 15 ? 0x8e17 : 0x8e13);
        if (mode >= 10) {
            mask(0,1,1,1,1); mask(1,0,0,0,0);
            glDrawElements(GL_TRIANGLES, 888, GL_UNSIGNED_SHORT, NULL);
        } else quads(8,-0.1f);
        if (conditional) {
            end_conditional();
            glBeginQuery(query_target, empty_queries[slot]);
            glEndQuery(query_target);
            begin_conditional(empty_queries[slot], mode == 15 ? 0x8e17 : 0x8e13);
            if (mode == 14 || mode == 15) {
                glClearColor(0,1,0,1); glClearStencil(7);
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
            } else { glUseProgram(fill); quads(1, -0.2f); }
            end_conditional();
        }
        CHECK(glGetError()==GL_NO_ERROR);
        if ((round%64)==0) { glFlush(); printf("submitted=%u\n",round+1); }
    }
    glFinish();
    glReadBuffer(GL_COLOR_ATTACHMENT0_EXT+(mode==3 || mode>=10 ? 0 : 1));
    for (unsigned side=0;side<2;++side) {
        unsigned char pixel[4];
        glReadPixels(side ? 96 : 32,64,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
        unsigned red, blue;
        if (mode==4 || mode==5) { red=128; blue=255; }
        else if (!side && mode!=2) { red=mode==3 || mode>=10 ? 65 : 128; blue=mode==3 || mode>=10 ? 0 : 255; }
        else { red=side ? 192 : 65; blue=128; }
        if (mode == 15) CHECK(pixel[0] == 0 && pixel[1] == 255 && pixel[2] == 0 && pixel[3] == 255);
        else CHECK(abs((int)pixel[0]-(int)red)<=3 && abs((int)pixel[1]-128)<=3 && abs((int)pixel[2]-(int)blue)<=3 && pixel[3]==255);
    }
    if (mode == 14 || mode == 15) {
        float depth_pixel;
        unsigned char stencil_pixel;
        glReadPixels(96,64,1,1,GL_DEPTH_COMPONENT,GL_FLOAT,&depth_pixel);
        glReadPixels(96,64,1,1,GL_STENCIL_INDEX,GL_UNSIGNED_BYTE,&stencil_pixel);
        CHECK(mode == 15 ? depth_pixel == 1.0f : depth_pixel > 0.4499f && depth_pixel < 0.4501f);
        CHECK(stencil_pixel == (mode == 15 ? 7 : 0));
    }
    printf("PASS mode=%u queries=%u pixels=2\n",mode,use_queries ? rounds : 0);
    glDeleteQueries(4,empty_queries); glDeleteQueries(4,queries); glDeleteProgram(fill); glDeleteProgram(alpha);
    glDeleteRenderbuffersEXT(4,colors); glDeleteRenderbuffersEXT(1,&depth); glDeleteFramebuffersEXT(1,&framebuffer);
    aglSetCurrentContext(NULL); aglSetDrawable(context,NULL); aglDestroyContext(context); DisposeWindow(window);
    return 0;
}
