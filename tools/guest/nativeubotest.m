/* Validate native-endian UBO reads without an unbounded shader loop. */
#import <Cocoa/Cocoa.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if(!(x)){printf("FAIL line=%d error=%x\n",__LINE__,error());return 1;} } while(0)
int main(int argc,char **argv)
{
 setbuf(stdout,NULL);
 unsigned gpu=argc>1?atoi(argv[1]):0;
 char path[256],suffix[8]="";if(gpu)snprintf(suffix,sizeof(suffix),"%u",gpu);
 snprintf(path,sizeof(path),"/System/Library/Extensions/RadeonNIGLDriver%s.bundle/Contents/MacOS/RadeonNIGLDriver%s",suffix,suffix);
 void *driver=dlopen(path,RTLD_NOW|RTLD_LOCAL);
 int (*request)(int,int)=driver?dlsym(driver,"RDNGLRequestCompatibilityContext"):NULL;
 int (*current)(void *)=driver?dlsym(driver,"RDNGLMakeCurrent"):NULL;
 void *(*lookup)(const char *)=driver?dlsym(driver,"RDNGLGetProcAddress"):NULL;
 if(!request||!current||!lookup||!request(4,6))return 2;
 Dl_info image;if(dladdr(request,&image))printf("driver=%s\n",image.dli_fname);
 NSAutoreleasePool *pool=[[NSAutoreleasePool alloc] init];[NSApplication sharedApplication];
 NSOpenGLPixelFormatAttribute a[]={NSOpenGLPFAAccelerated,NSOpenGLPFANoRecovery,NSOpenGLPFARendererID,0x21a00+gpu*0x100,NSOpenGLPFAScreenMask,argc>2?strtoul(argv[2],0,16):CGDisplayIDToOpenGLDisplayMask(CGMainDisplayID()),0};
 NSOpenGLPixelFormat *pf=[[NSOpenGLPixelFormat alloc] initWithAttributes:a];
 NSOpenGLContext *ctx=[[NSOpenGLContext alloc] initWithFormat:pf shareContext:nil];
 if(!ctx)return 3;
 unsigned char pixels[64*64*4]={0};
 if(CGLSetOffScreen([ctx CGLContextObj],64,64,64*4,pixels)||CGLSetCurrentContext([ctx CGLContextObj])||!current([ctx CGLContextObj]))return 4;
 GLenum (*error)(void)=lookup("glGetError");
#define FN(ret,name,args) ret (*name)args=lookup(#name);CHECK(name)
 FN(void,glGetIntegerv,(GLenum,GLint *));
 FN(GLuint,glCreateShader,(GLenum));
 FN(void,glShaderSource,(GLuint,GLsizei,const char **,const GLint *));
 FN(void,glCompileShader,(GLuint));
 FN(void,glGetShaderiv,(GLuint,GLenum,GLint *));
 FN(void,glGetShaderInfoLog,(GLuint,GLsizei,GLsizei *,char *));
 FN(GLuint,glCreateProgram,(void));
 FN(void,glAttachShader,(GLuint,GLuint));
 FN(void,glLinkProgram,(GLuint));
 FN(void,glGetProgramiv,(GLuint,GLenum,GLint *));
 FN(void,glGetProgramInfoLog,(GLuint,GLsizei,GLsizei *,char *));
 FN(void,glUseProgram,(GLuint));
 FN(GLint,glGetUniformLocation,(GLuint,const char *));
 FN(void,glUniform1f,(GLint,GLfloat));
 FN(void,glGenBuffers,(GLsizei,GLuint *));
 FN(void,glBindBuffer,(GLenum,GLuint));
 FN(void,glBufferData,(GLenum,ptrdiff_t,const void *,GLenum));
 FN(void,glBufferSubData,(GLenum,ptrdiff_t,ptrdiff_t,const void *));
 FN(void,glBindBufferRange,(GLenum,GLuint,GLuint,ptrdiff_t,ptrdiff_t));
 FN(void,glViewport,(GLint,GLint,GLsizei,GLsizei));
 FN(void,glClearColor,(GLfloat,GLfloat,GLfloat,GLfloat));
 FN(void,glClear,(GLbitfield));
 FN(void,glDrawArrays,(GLenum,GLint,GLsizei));
 FN(void,glReadPixels,(GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,void *));
 FN(void,glDispatchCompute,(GLuint,GLuint,GLuint));
 FN(void,glMemoryBarrier,(GLbitfield));
 GLint selected=0;CGLGetParameter([ctx CGLContextObj],kCGLCPCurrentRendererID,&selected);
 printf("gpu=%u selected=%x\n",gpu,selected);CHECK((selected&0xffff)==0x1a00+gpu*0x100);
 FN(void,glFinish,(void));
 FN(void,glGetBufferSubData,(GLenum,ptrdiff_t,ptrdiff_t,void *));
 const char *source="#version 430\nlayout(local_size_x=1) in;uniform float bias;layout(std140,binding=0) uniform Input {ivec4 params;vec4 values[2];};layout(std430,binding=1) buffer Output {float results[];};void main(){uint i=gl_GlobalInvocationID.x;if(i<4u)results[i]=values[params.y & 1][i]+bias;else if(i==4u)results[i]=float(params.x);else if(i==5u)results[i]=float(params.z);else if(i==6u)results[i]=float(params.w);else{float n=0.0;for(float f=0.5;f<min(float(params.x),16.0);f+=2.0)n+=1.0;results[i]=n;}}";
 GLuint sh=glCreateShader(0x91b9);glShaderSource(sh,1,&source,NULL);glCompileShader(sh);GLint ok=0;glGetShaderiv(sh,GL_COMPILE_STATUS,&ok);
 if(!ok){char log[4096]={0};glGetShaderInfoLog(sh,sizeof(log),NULL,log);printf("shader %s\n",log);return 5;}
 GLuint program=glCreateProgram();glAttachShader(program,sh);glLinkProgram(program);glGetProgramiv(program,GL_LINK_STATUS,&ok);
 if(!ok){char log[4096]={0};glGetProgramInfoLog(program,sizeof(log),NULL,log);printf("program %s\n",log);return 6;}
 struct {int params[4];float values[8];} input={{5,1,-7,0x123456},{1,2,3,4,1.25f,-2.5f,3.75f,4.5f}};
 float expected[]={1.5f,-2.25f,4.0f,4.75f,5,-7,0x123456,3};
 float result[8]={0};GLuint bo[2];glGenBuffers(2,bo);
 unsigned char storage[512]={0};memcpy(storage+256,&input,sizeof(input));
 glBindBuffer(0x8a11,bo[0]);glBufferData(0x8a11,sizeof(storage),storage,GL_DYNAMIC_DRAW);glBindBufferRange(0x8a11,0,bo[0],256,sizeof(input));
 glBindBuffer(0x90d2,bo[1]);glBufferData(0x90d2,sizeof(result),result,GL_STREAM_DRAW);glBindBufferRange(0x90d2,1,bo[1],0,sizeof(result));
 glUseProgram(program);glUniform1f(glGetUniformLocation(program,"bias"),0.25f);
 unsigned failures=0;
 for(unsigned pass=0;pass<3;pass++) {
  if(pass==1){input.params[0]=3;input.params[1]=0;expected[0]=1.25f;expected[1]=2.25f;expected[2]=3.25f;expected[3]=4.25f;expected[4]=3;expected[7]=2;glBindBuffer(0x8a11,bo[0]);glBufferSubData(0x8a11,256,sizeof(input),&input);}
  if(pass==2){input.params[0]=7;expected[4]=7;expected[7]=4;memcpy(storage+256,&input,sizeof(input));glBindBuffer(0x8a11,bo[0]);glBufferData(0x8a11,sizeof(storage),storage,GL_DYNAMIC_DRAW);}
  glDispatchCompute(8,1,1);glMemoryBarrier(0x2200);glFinish();glBindBuffer(0x90d2,bo[1]);glGetBufferSubData(0x90d2,0,sizeof(result),result);
  for(unsigned i=0;i<8;i++){printf("pass=%u i=%u got=%.12g expected=%.12g\n",pass,i,result[i],expected[i]);failures+=result[i]!=expected[i];}
  CHECK(!error());
 }
 CHECK(!failures);puts("PASS native UBO constants, dynamic indexing, range offset, updates and replacement");return 0;
}
