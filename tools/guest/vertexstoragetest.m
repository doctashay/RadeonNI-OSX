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
 GLint limit=0;glGetIntegerv(0x90d6,&limit);printf("vertex_ssbo_limit=%d\n",limit);CHECK(limit>=4);
 const char *sources[]={
 "#version 430\nlayout(std430,binding=3) readonly buffer Data {vec4 colors[];};flat out vec4 c;void main(){vec2 p[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));gl_Position=vec4(p[gl_VertexID],0,1);c=colors.length()==3?colors[gl_VertexID]:vec4(0,1,0,1);}",
 "#version 430\nflat in vec4 c;out vec4 outColor;void main(){outColor=c;}",
 "#version 430\nlayout(local_size_x=1) in;layout(std430,binding=3) buffer Data {vec4 colors[];};void main(){for(uint i=0u;i<3u;i++)colors[i]=vec4(1,0,0,1);}"
 };
 if(getenv("RDN_SKIP_LENGTH"))sources[0]="#version 430\nlayout(std430,binding=3) readonly buffer Data {vec4 colors[];};flat out vec4 c;void main(){vec2 p[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));gl_Position=vec4(p[gl_VertexID],0,1);c=colors[gl_VertexID];}";
 GLuint programs[2]={glCreateProgram(),glCreateProgram()};
 for(unsigned i=0;i<3;i++){
  GLuint sh=glCreateShader(i==0?GL_VERTEX_SHADER:i==1?GL_FRAGMENT_SHADER:0x91b9);
  glShaderSource(sh,1,&sources[i],NULL);glCompileShader(sh);GLint ok=0;glGetShaderiv(sh,GL_COMPILE_STATUS,&ok);
  if(!ok){char log[4096]={0};glGetShaderInfoLog(sh,sizeof(log),NULL,log);printf("shader=%u %s\n",i,log);return 5;}
  glAttachShader(programs[i==2],sh);
 }
 for(unsigned i=0;i<2;i++){
  glLinkProgram(programs[i]);GLint ok=0;glGetProgramiv(programs[i],GL_LINK_STATUS,&ok);
  if(!ok){char log[4096]={0};glGetProgramInfoLog(programs[i],sizeof(log),NULL,log);printf("program=%u %s\n",i,log);return 6;}
 }
 GLuint bo;glGenBuffers(1,&bo);glBindBuffer(0x90d2,bo);
 float data[128]={0};glBufferData(0x90d2,sizeof(data),data,GL_STREAM_DRAW);
 glBindBufferRange(0x90d2,3,bo,256,48);glViewport(0,0,64,64);
 for(unsigned pass=0;pass<64;pass++){
  unsigned blue=pass&1;
  for(unsigned i=0;i<3;i++){data[64+i*4]=!blue;data[65+i*4]=0;data[66+i*4]=blue;data[67+i*4]=1;}
  glBufferData(0x90d2,sizeof(data),data,GL_STREAM_DRAW);
  if(pass>=32){glUseProgram(programs[1]);glDispatchCompute(1,1,1);glMemoryBarrier(0x2000);blue=0;}
  glUseProgram(programs[0]);glClearColor(0,1,0,1);glClear(GL_COLOR_BUFFER_BIT);glDrawArrays(GL_TRIANGLES,0,3);
  unsigned char pixel[4]={0};glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
  printf("pass=%u pixel=%u,%u,%u,%u\n",pass,pixel[0],pixel[1],pixel[2],pixel[3]);
  CHECK(!error()&&pixel[0]==(!blue?255:0)&&pixel[1]==0&&pixel[2]==(blue?255:0)&&pixel[3]==255);
 }
 puts("PASS vertex SSBO range, length, replacement and compute visibility");
 return 0;
}
