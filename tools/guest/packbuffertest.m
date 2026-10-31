/* Exercise small PBO read maps and GPU pixel readback on one renderer. */
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
 const GLubyte *(*get)(GLenum)=lookup("glGetString");
 void (*gen)(GLsizei,GLuint *)=lookup("glGenBuffers");
 void (*bind)(GLenum,GLuint)=lookup("glBindBuffer");
 void (*data)(GLenum,ptrdiff_t,const void *,GLenum)=lookup("glBufferData");
 void *(*map)(GLenum,ptrdiff_t,ptrdiff_t,GLbitfield)=lookup("glMapBufferRange");
 GLboolean (*unmap)(GLenum)=lookup("glUnmapBuffer");
 void (*destroy)(GLsizei,const GLuint *)=lookup("glDeleteBuffers");
 CHECK(error&&get&&gen&&bind&&data&&map&&unmap&&destroy);
 GLint selected=0;CHECK(!CGLGetParameter([ctx CGLContextObj],kCGLCPCurrentRendererID,&selected));
 printf("gpu=%u selected=%x renderer=%s version=%s\n",gpu,selected,get(GL_RENDERER),get(GL_VERSION));
 CHECK((selected&0xffff)==0x1a00+gpu*0x100);
 const unsigned sizes[]={24,16,32,256,4096};
 const GLenum usages[]={0x88e5,0x88e1,0x88e9,GL_STATIC_DRAW};
 unsigned char input[4096];for(unsigned i=0;i<sizeof(input);i++)input[i]=(i*17+3)&255;
 for(unsigned u=0;u<4;u++)for(unsigned s=0;s<5;s++) {
  GLuint bo;gen(1,&bo);bind(0x88eb,bo);data(0x88eb,sizes[s],input,usages[u]);CHECK(!error());
  void *p=map(0x88eb,0,sizes[s],1);GLenum e=error();
  printf("PBO usage=%x size=%u map=%p error=%x\n",usages[u],sizes[s],p,e);
  CHECK(p&&!e&&!memcmp(p,input,sizes[s]));CHECK(unmap(0x88eb));destroy(1,&bo);
 }
 for(unsigned round=0;round<32;round++) {
  GLuint src,dst;gen(1,&src);gen(1,&dst);
  bind(GL_ARRAY_BUFFER,src);data(GL_ARRAY_BUFFER,24,input,GL_DYNAMIC_DRAW);
  bind(0x88eb,src);bind(GL_ARRAY_BUFFER,dst);data(GL_ARRAY_BUFFER,24,NULL,GL_STREAM_DRAW);CHECK(!error());
  void *p=map(0x88eb,0,24,1);GLenum e=error();printf("conversion pattern round=%u map=%p error=%x\n",round,p,e);
  CHECK(p&&!e&&!memcmp(p,input,24));CHECK(unmap(0x88eb));destroy(1,&src);destroy(1,&dst);
 }
 void (*copy)(GLenum,GLenum,ptrdiff_t,ptrdiff_t,ptrdiff_t)=lookup("glCopyBufferSubData");
 CHECK(copy);
 GLuint src,dst;gen(1,&src);gen(1,&dst);
 bind(0x8f36,src);data(0x8f36,24,input,GL_STATIC_DRAW);
 bind(0x8f37,dst);data(0x8f37,24,NULL,GL_STATIC_DRAW);CHECK(!error());
 copy(0x8f36,0x8f37,0,0,24);CHECK(!error());
 bind(0x88eb,dst);void *copied=map(0x88eb,0,24,1);GLenum copyError=error();
 printf("GPU copy PBO map=%p error=%x\n",copied,copyError);CHECK(copied&&!copyError&&!memcmp(copied,input,24));
 CHECK(unmap(0x88eb));destroy(1,&src);destroy(1,&dst);
 void (*clearColor)(GLfloat,GLfloat,GLfloat,GLfloat)=lookup("glClearColor");
 void (*clear)(GLbitfield)=lookup("glClear");
 void (*read)(GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,void *)=lookup("glReadPixels");
 CHECK(clearColor&&clear&&read);
 for(unsigned u=0;u<4;u++) {
  GLuint bo;gen(1,&bo);bind(0x88eb,bo);data(0x88eb,24,NULL,usages[u]);CHECK(!error());
  clearColor(0.25,0.5,0.75,1);clear(GL_COLOR_BUFFER_BIT);CHECK(!error());
  read(0,0,2,3,GL_RGBA,GL_UNSIGNED_BYTE,0);GLenum e=error();printf("readpixels usage=%x error=%x\n",usages[u],e);CHECK(!e);
  unsigned char *p=map(0x88eb,0,24,1);e=error();printf("GPU PBO size=24 map=%p error=%x\n",p,e);CHECK(p&&!e);
  for(unsigned i=0;i<6;i++)CHECK(abs(p[i*4]-64)<=1&&abs(p[i*4+1]-128)<=1&&abs(p[i*4+2]-191)<=1&&p[i*4+3]==255);
  CHECK(unmap(0x88eb));destroy(1,&bo);
 }

 GLuint (*createShader)(GLenum)=lookup("glCreateShader");
 void (*shaderSource)(GLuint,GLsizei,const char **,const GLint *)=lookup("glShaderSource");
 void (*compileShader)(GLuint)=lookup("glCompileShader");
 void (*shaderStatus)(GLuint,GLenum,GLint *)=lookup("glGetShaderiv");
 GLuint (*createProgram)(void)=lookup("glCreateProgram");
 void (*attachShader)(GLuint,GLuint)=lookup("glAttachShader");
 void (*linkProgram)(GLuint)=lookup("glLinkProgram");
 void (*programStatus)(GLuint,GLenum,GLint *)=lookup("glGetProgramiv");
 void (*useProgram)(GLuint)=lookup("glUseProgram");
 void (*viewport)(GLint,GLint,GLsizei,GLsizei)=lookup("glViewport");
 void (*drawIndirect)(GLenum,const void *)=lookup("glDrawArraysIndirect");
 void (*drawIndexed)(GLenum,GLenum,const void *)=lookup("glDrawElementsIndirect");
 CHECK(createShader&&shaderSource&&compileShader&&shaderStatus&&createProgram&&attachShader&&linkProgram&&programStatus&&useProgram&&viewport&&drawIndirect&&drawIndexed);
 const char *sources[]={"#version 460\nvoid main(){vec2 p[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));gl_Position=vec4(p[gl_VertexID],0,1);}","#version 460\nout vec4 color;void main(){color=vec4(1,0,0,1);}"};
 GLuint program=createProgram();
 for(unsigned i=0;i<2;i++){GLuint shader=createShader(i?GL_FRAGMENT_SHADER:GL_VERTEX_SHADER);shaderSource(shader,1,&sources[i],NULL);compileShader(shader);GLint ok=0;shaderStatus(shader,GL_COMPILE_STATUS,&ok);CHECK(ok);attachShader(program,shader);}
 linkProgram(program);GLint ok=0;programStatus(program,GL_LINK_STATUS,&ok);CHECK(ok);useProgram(program);viewport(0,0,64,64);
 void (*drawArrays)(GLenum,GLint,GLsizei)=lookup("glDrawArrays");CHECK(drawArrays);
 bind(0x88eb,0);clearColor(0,0,1,1);clear(GL_COLOR_BUFFER_BIT);drawArrays(GL_TRIANGLES,0,3);
 unsigned char direct[4]={0};read(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,direct);
 printf("direct draw control pixel=%u,%u,%u,%u error=%x\n",direct[0],direct[1],direct[2],direct[3],error());
 CHECK(direct[0]==255&&direct[1]==0&&direct[2]==0&&direct[3]==255);
 for(unsigned indexed=0;indexed<2;indexed++) {
  unsigned arguments[16]={0};unsigned argumentWord=getenv("RDN_TEST_SMALL_INDIRECT")?0:4;
  arguments[argumentWord]=3;arguments[argumentWord+1]=1;
  if(indexed)arguments[argumentWord+3]=0xffffffffu;
  GLuint indirect,index,readback;gen(1,&indirect);gen(1,&index);gen(1,&readback);
  bind(0x8f3f,indirect);data(0x8f3f,getenv("RDN_TEST_SMALL_INDIRECT")?(indexed?20:16):sizeof(arguments),arguments,getenv("RDN_TEST_STREAM_INDIRECT")?GL_STREAM_DRAW:GL_STATIC_DRAW);
  unsigned indices[3]={1,2,3};bind(GL_ELEMENT_ARRAY_BUFFER,index);data(GL_ELEMENT_ARRAY_BUFFER,sizeof(indices),indices,GL_STATIC_DRAW);
  bind(0x88eb,0);clearColor(0,0,1,1);clear(GL_COLOR_BUFFER_BIT);
  printf("begin indirect indexed=%u\n",indexed);
  if(indexed)drawIndexed(GL_TRIANGLES,GL_UNSIGNED_INT,(void *)(uintptr_t)(argumentWord*4));else drawIndirect(GL_TRIANGLES,(void *)(uintptr_t)(argumentWord*4));
  CHECK(!error());bind(0x88eb,readback);data(0x88eb,24,NULL,GL_STREAM_READ);read(0,0,2,3,GL_RGBA,GL_UNSIGNED_BYTE,0);CHECK(!error());
  unsigned char *p=map(0x88eb,0,24,1);GLenum e=error();printf("indirect indexed=%u PBO map=%p error=%x\n",indexed,p,e);CHECK(p&&!e);
  for(unsigned i=0;i<6;i++)CHECK(p[i*4]==255&&p[i*4+1]==0&&p[i*4+2]==0&&p[i*4+3]==255);
  CHECK(unmap(0x88eb));
  void (*instanced)(GLenum,GLint,GLsizei,GLsizei)=lookup("glDrawArraysInstanced");CHECK(instanced);
  bind(0x88eb,0);clear(GL_COLOR_BUFFER_BIT);instanced(GL_TRIANGLES,0,3,4096);
  bind(0x88eb,readback);read(0,0,2,3,GL_RGBA,GL_UNSIGNED_BYTE,0);
  p=map(0x88eb,0,24,1);CHECK(p&&!error());
  for(unsigned i=0;i<6;i++)CHECK(p[i*4]==255&&p[i*4+1]==0&&p[i*4+2]==0&&p[i*4+3]==255);
  CHECK(unmap(0x88eb));printf("PASS indirect-to-instanced indexed=%u\n",indexed);
  destroy(1,&indirect);destroy(1,&index);destroy(1,&readback);
 }
 CGLSetCurrentContext(NULL);current(NULL);[ctx release];[pf release];[pool drain];
 puts("PASS: small PBO read maps");return 0;
}
