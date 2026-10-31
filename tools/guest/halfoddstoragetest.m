/* Validate vertex SSBO fetches, range offsets and storage replacement. */
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
 FN(void,glGenBuffers,(GLsizei,GLuint *));
 FN(void,glBindBuffer,(GLenum,GLuint));
 FN(void,glBufferData,(GLenum,ptrdiff_t,const void *,GLenum));
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
 const char *source="#version 430\n#extension GL_AMD_gpu_shader_half_float : require\nlayout(local_size_x=3) in;layout(std430,binding=0) readonly buffer Input {float16_t values[];};layout(std430,binding=1) buffer Output {float16_t results[];};shared float16_t sharedValues[3];void main(){uint i=gl_LocalInvocationID.x;sharedValues[i]=values.length()==3?values[i]:float16_t(0);barrier();precise float16_t h=sharedValues[2u-i];precise float16_t sum=h+float16_t(0.5);results[i*2]=h;results[i*2+1]=sum;}";
 GLuint sh=glCreateShader(0x91b9);glShaderSource(sh,1,&source,NULL);glCompileShader(sh);GLint ok=0;glGetShaderiv(sh,GL_COMPILE_STATUS,&ok);
 if(!ok){char log[4096]={0};glGetShaderInfoLog(sh,sizeof(log),NULL,log);printf("shader %s\n",log);return 5;}
 GLuint program=glCreateProgram();glAttachShader(program,sh);glLinkProgram(program);glGetProgramiv(program,GL_LINK_STATUS,&ok);
 if(!ok){char log[4096]={0};glGetProgramInfoLog(program,sizeof(log),NULL,log);printf("program %s\n",log);return 6;}
 unsigned short input[]={0x3c00,0x4000,0xbc00};
 unsigned short expected[]={0xbc00,0xb800,0x4000,0x4100,0x3c00,0x3e00};
 unsigned short result[6];memset(result,0xa5,sizeof(result));GLuint bo[2];glGenBuffers(2,bo);
 glBindBuffer(0x90d2,bo[0]);glBufferData(0x90d2,sizeof(input),input,GL_STATIC_DRAW);glBindBufferRange(0x90d2,0,bo[0],0,sizeof(input));
 glBindBuffer(0x90d2,bo[1]);glBufferData(0x90d2,sizeof(result),result,GL_STREAM_DRAW);glBindBufferRange(0x90d2,1,bo[1],0,sizeof(result));
 glUseProgram(program);glDispatchCompute(1,1,1);glMemoryBarrier(0x2200);if(getenv("RDN_EXPLICIT_FINISH"))glFinish();glBindBuffer(0x90d2,bo[1]);glGetBufferSubData(0x90d2,0,sizeof(result),result);
 unsigned failures=0;for(unsigned i=0;i<6;i++){printf("i=%u got=%04x expected=%04x\n",i,result[i],expected[i]);failures+=result[i]!=expected[i];}
 CHECK(!error()&&!failures);puts("PASS packed half SSBO/shared loads, neighbor stores and arithmetic");return 0;
}
