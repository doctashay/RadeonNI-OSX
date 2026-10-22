/* Native test: gcc modern_context.c -framework OpenGL -lpthread -o modern_context */
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int (*bind_mesa)(void *);
static void *(*lookup)(const char *);
static CGLContextObj context;
static int failed;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL: %s\n", #x); return 1; } } while (0)
static int framebuffer_roundtrip(void)
{
 GLuint fbo;
 void (*gen)(GLsizei,GLuint *)=lookup("glGenFramebuffers");
 void (*bind)(GLenum,GLuint)=lookup("glBindFramebuffer");
 void (*destroy)(GLsizei,const GLuint *)=lookup("glDeleteFramebuffers");
 GLenum (*error)(void)=lookup("glGetError");
 CHECK(gen && bind && destroy && error);
 gen(1,&fbo); bind(0x8d40,fbo); bind(0x8d40,0); destroy(1,&fbo);
 CHECK(error()==GL_NO_ERROR);
 return 0;
}
static void *handoff(void *unused)
{
 const GLubyte *(*get)(GLenum);
 if (CGLSetCurrentContext(context) || !bind_mesa(context)) { failed = 1; return NULL; }
 get = lookup("glGetString");
 if (!get || !strstr((const char *)get(GL_VERSION), "4.6")) failed = 1;
 if (framebuffer_roundtrip()) failed = 1;
 CGLSetCurrentContext(NULL);
 bind_mesa(NULL);
 return NULL;
}
int main(int argc, char **argv)
{
 void *driver, *sdl, *window;
 int (*request)(int,int), (*init)(unsigned);
 void *(*create_window)(const char *,int,int,int,int,unsigned);
 void *(*create_context)(void *);
 const GLubyte *(*get)(GLenum);
 void (*gen)(GLsizei,GLuint *), (*bind)(GLenum,GLuint);
 void (*storage)(GLenum,ptrdiff_t,const void *,GLbitfield);
 void *(*map)(GLenum,ptrdiff_t,ptrdiff_t,GLbitfield);
 GLboolean (*unmap)(GLenum);
 void (*destroy)(GLsizei,const GLuint *);
 GLuint buffer = 0, values[4] = { 1, 0x12345678, 3, 0xffffffff }, *mapped;
 GLuint shader, program;
 GLint ok;
 int compute_failed;
 GLuint (*create_shader)(GLenum), (*create_program)(void);
 void (*shader_source)(GLuint,GLsizei,const char **,const GLint *);
 void (*compile_shader)(GLuint), (*attach_shader)(GLuint,GLuint), (*link_program)(GLuint), (*use_program)(GLuint);
 void (*get_shader)(GLuint,GLenum,GLint *), (*get_program)(GLuint,GLenum,GLint *);
 void (*bind_base)(GLenum,GLuint,GLuint), (*dispatch)(GLuint,GLuint,GLuint), (*barrier)(GLbitfield);
 const char *source="#version 460\nlayout(local_size_x=4) in; layout(std430,binding=0) buffer B { uint v[]; }; void main(){v[gl_LocalInvocationID.x]+=7u;}";
 pthread_t thread;
 CHECK(argc == 2 || argc == 3);
 driver=dlopen("/System/Library/Extensions/RadeonNIGLDriver.bundle/Contents/MacOS/RadeonNIGLDriver",RTLD_NOW|RTLD_LOCAL);
 CHECK(driver);
 request=dlsym(driver,"RDNGLRequestCompatibilityContext");
 lookup=dlsym(driver,"RDNGLGetProcAddress");
 bind_mesa=dlsym(driver,"RDNGLMakeCurrent");
 CHECK(request && lookup && bind_mesa);
 CHECK(!request(9,9));
 if (argc == 2) CHECK(request(4,6));
 CHECK(!lookup("glBufferStorage"));
 sdl=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL); CHECK(sdl);
 init=dlsym(sdl,"SDL_Init"); create_window=dlsym(sdl,"SDL_CreateWindow"); create_context=dlsym(sdl,"SDL_GL_CreateContext");
 CHECK(init && create_window && create_context && !init(32));
 window=create_window("",0,0,64,64,10); CHECK(window);
 CHECK(create_context(window));
 context=CGLGetCurrentContext(); CHECK(context);
 if (argc == 3) {
  const char *version=(const char *)glGetString(GL_VERSION);
  CHECK(version && !strncmp(version,"2.1",3));
  CHECK(!lookup("glBufferStorage"));
  printf("PASS: unchanged legacy context %s\n",version);
  return 0;
 }
 CHECK(bind_mesa(context));
 CHECK(!request(4,6));
 get=lookup("glGetString"); CHECK(get);
 printf("Renderer: %s\nVersion: %s\nGLSL: %s\n",get(GL_RENDERER),get(GL_VERSION),get(GL_SHADING_LANGUAGE_VERSION));
 CHECK(strstr((const char *)get(GL_VERSION),"4.6"));
 CHECK(strstr((const char *)get(GL_RENDERER),"Radeon") || strstr((const char *)get(GL_RENDERER),"TURKS"));
 CHECK(lookup("glDispatchCompute") && lookup("glMemoryBarrier") && lookup("glFenceSync"));
 gen=lookup("glGenBuffers"); bind=lookup("glBindBuffer"); storage=lookup("glBufferStorage");
 map=lookup("glMapBufferRange"); unmap=lookup("glUnmapBuffer"); destroy=lookup("glDeleteBuffers");
 CHECK(gen && bind && storage && map && unmap && destroy);
 gen(1,&buffer);bind(GL_ARRAY_BUFFER,buffer);storage(GL_ARRAY_BUFFER,sizeof(values),values,1);
 mapped=map(GL_ARRAY_BUFFER,0,sizeof(values),1);CHECK(mapped && !memcmp(mapped,values,sizeof(values)));
 CHECK(unmap(GL_ARRAY_BUFFER));
 create_shader=lookup("glCreateShader");create_program=lookup("glCreateProgram");
 shader_source=lookup("glShaderSource");compile_shader=lookup("glCompileShader");
 attach_shader=lookup("glAttachShader");link_program=lookup("glLinkProgram");use_program=lookup("glUseProgram");
 get_shader=lookup("glGetShaderiv");get_program=lookup("glGetProgramiv");
 bind_base=lookup("glBindBufferBase");dispatch=lookup("glDispatchCompute");barrier=lookup("glMemoryBarrier");
 CHECK(create_shader && create_program && shader_source && compile_shader && attach_shader &&
       link_program && use_program && get_shader && get_program && bind_base && dispatch && barrier);
 shader=create_shader(0x91b9);shader_source(shader,1,&source,NULL);compile_shader(shader);
 get_shader(shader,0x8b81,&ok);CHECK(ok);
 program=create_program();attach_shader(program,shader);link_program(program);
 get_program(program,0x8b82,&ok);CHECK(ok);use_program(program);
 bind_base(0x90d2,0,buffer);dispatch(1,1,1);barrier(0x200);
 mapped=map(GL_ARRAY_BUFFER,0,sizeof(values),1);
 if (mapped) printf("Compute readback: %08x %08x %08x %08x\n",mapped[0],mapped[1],mapped[2],mapped[3]);
 CHECK(mapped);
 compute_failed=!(mapped[0]==8 && mapped[1]==0x1234567f && mapped[2]==10 && mapped[3]==6);
 CHECK(unmap(GL_ARRAY_BUFFER));destroy(1,&buffer);
 CHECK(!CGLSetCurrentContext(NULL) && bind_mesa(NULL));
 CHECK(!lookup("glBufferStorage"));
 CHECK(!pthread_create(&thread,NULL,handoff,NULL));CHECK(!pthread_join(thread,NULL));CHECK(!failed);
 CHECK(!CGLSetCurrentContext(context) && bind_mesa(context));
 CHECK(!framebuffer_roundtrip());
 puts("PASS: 4.6 context, modern lookup, immutable buffer readback, thread handoff");
 CHECK(!compute_failed);
 puts("PASS: GLSL 4.60 compute readback");
 return 0;
}
