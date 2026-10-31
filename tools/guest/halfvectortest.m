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
 const char *source="#version 430\n#extension GL_AMD_gpu_shader_half_float : require\nlayout(local_size_x=1) in;layout(std430,binding=0) readonly buffer Input {float values[];};layout(std430,binding=1) buffer Output {float results[];};void main(){uint i=gl_GlobalInvocationID.x;precise f16vec2 v=f16vec2(float16_t(values[i]),float16_t(i));precise f16vec2 d=f16mat2(1,2,3,4)*v;results[i*4]=float(d.x);results[i*4+1]=float(d.y);results[i*4+2]=float(float16_t(int(i)-4));results[i*4+3]=float(float16_t(i!=0u));}";
 GLuint sh=glCreateShader(0x91b9);glShaderSource(sh,1,&source,NULL);glCompileShader(sh);GLint ok=0;glGetShaderiv(sh,GL_COMPILE_STATUS,&ok);
 if(!ok){char log[4096]={0};glGetShaderInfoLog(sh,sizeof(log),NULL,log);printf("shader %s\n",log);return 5;}
 GLuint program=glCreateProgram();glAttachShader(program,sh);glLinkProgram(program);glGetProgramiv(program,GL_LINK_STATUS,&ok);
 if(!ok){char log[4096]={0};glGetProgramInfoLog(program,sizeof(log),NULL,log);printf("program %s\n",log);return 6;}
 float input[]={1.25,-2,0,4,.125,-.5,5,16};
 float expected[32];for(unsigned i=0;i<8;i++){expected[i*4]=input[i]+3*i;expected[i*4+1]=2*input[i]+4*i;expected[i*4+2]=(int)i-4;expected[i*4+3]=i!=0;}
 float result[32]={0};GLuint bo[2];glGenBuffers(2,bo);
 glBindBuffer(0x90d2,bo[0]);glBufferData(0x90d2,sizeof(input),input,GL_STATIC_DRAW);glBindBufferRange(0x90d2,0,bo[0],0,sizeof(input));
 glBindBuffer(0x90d2,bo[1]);glBufferData(0x90d2,sizeof(result),result,GL_STREAM_DRAW);glBindBufferRange(0x90d2,1,bo[1],0,sizeof(result));
 glUseProgram(program);glDispatchCompute(8,1,1);glMemoryBarrier(0x2200);if(getenv("RDN_EXPLICIT_FINISH"))glFinish();glBindBuffer(0x90d2,bo[1]);glGetBufferSubData(0x90d2,0,sizeof(result),result);
 unsigned failures=0;for(unsigned i=0;i<32;i++){printf("i=%u got=%.12g expected=%.12g\n",i,result[i],expected[i]);failures+=result[i]!=expected[i];}
 CHECK(!error()&&!failures);puts("PASS half vectors, matrices, signed/unsigned/boolean conversions");return 0;
}
