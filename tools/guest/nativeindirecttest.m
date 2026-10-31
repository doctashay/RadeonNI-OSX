/* Validate native-endian indirect commands on a selected GPU. */
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
 void *driver=dlopen(argc>3?argv[3]:path,RTLD_NOW|RTLD_LOCAL);
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

 FN(void,glUniform1i,(GLint,GLint));
 FN(void,glDrawArraysInstancedBaseInstance,(GLenum,GLint,GLsizei,GLsizei,GLuint));
 FN(void,glDrawArraysIndirect,(GLenum,const void *));
 FN(void,glDrawElementsIndirect,(GLenum,GLenum,const void *));
 FN(void,glMultiDrawArraysIndirect,(GLenum,const void *,GLsizei,GLsizei));
 FN(void,glMultiDrawElementsIndirect,(GLenum,GLenum,const void *,GLsizei,GLsizei));
 FN(void,glGenQueries,(GLsizei,GLuint *));
 FN(void,glBeginQuery,(GLenum,GLuint));
 FN(void,glEndQuery,(GLenum));
 FN(void,glGetQueryObjectuiv,(GLuint,GLenum,GLuint *));
 const char *vs_parameters="#version 430\n#extension GL_ARB_shader_draw_parameters : require\nuniform int indexed;void main(){vec2 p[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));bool valid=gl_BaseInstanceARB==7 && gl_BaseVertexARB==((indexed!=0 && gl_DrawIDARB==1)?-3:0);gl_Position=valid?vec4(p[gl_VertexID%3],0,1):vec4(2,2,0,1);}";
 const char *vs="#version 430\nvoid main(){uint id=uint(gl_VertexID);vec2 p=vec2((id<<1)&2u,id&2u);gl_Position=vec4(p*2.0-1.0,0,1);}";
 if(argc>4 && atoi(argv[4]))vs=vs_parameters;
 const char *fs="#version 430\nout vec4 color;void main(){color=vec4(0.25,0.5,0.75,1);}";
 GLuint program=glCreateProgram();
 for(unsigned i=0;i<2;i++){
  GLuint shader=glCreateShader(i?GL_FRAGMENT_SHADER:GL_VERTEX_SHADER);
  const char *text=i?fs:vs;glShaderSource(shader,1,&text,NULL);glCompileShader(shader);
  GLint ok=0;glGetShaderiv(shader,GL_COMPILE_STATUS,&ok);
  if(!ok){char log[2048];glGetShaderInfoLog(shader,sizeof(log),NULL,log);puts(log);return 5;}
  glAttachShader(program,shader);
 }
 glLinkProgram(program);GLint ok=0;glGetProgramiv(program,GL_LINK_STATUS,&ok);CHECK(ok);glUseProgram(program);
 puts("direct draw control");glViewport(0,0,64,64);glDrawArraysInstancedBaseInstance(GL_TRIANGLES,0,3,2,7);glFinish();puts("direct draw finished");
 GLuint bo[2],query;glGenBuffers(2,bo);glGenQueries(1,&query);
 unsigned indices[]={0,1,2,3,4,5};glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,bo[1]);glBufferData(GL_ELEMENT_ARRAY_BUFFER,sizeof(indices),indices,GL_STATIC_DRAW);
 /* Native words, nonzero offset, padded stride and signed base vertex. */
 unsigned arrays[]={3,2,0,7,0,0,0,0,3,1,0,7};
 unsigned elements[]={3,2,0,0,7,0,0,0,3,1,3,-3,7};
 for(unsigned indexed=0;indexed<2;indexed++){
  glUniform1i(glGetUniformLocation(program,"indexed"),indexed);
  unsigned storage[32]={0},readback[32]={0};
  memcpy(storage+4,indexed?elements:arrays,indexed?sizeof(elements):sizeof(arrays));
  glBindBuffer(0x8f3f,bo[0]);glBufferData(0x8f3f,sizeof(storage),storage,GL_DYNAMIC_DRAW);
  for(unsigned multi=0;multi<2;multi++){
   glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);glViewport(0,0,64,64);
   glBeginQuery(0x8c87,query);
   if(indexed){if(multi)glMultiDrawElementsIndirect(GL_TRIANGLES,GL_UNSIGNED_INT,(void *)16,2,32);else glDrawElementsIndirect(GL_TRIANGLES,GL_UNSIGNED_INT,(void *)16);}
   else{if(multi)glMultiDrawArraysIndirect(GL_TRIANGLES,(void *)16,2,32);else glDrawArraysIndirect(GL_TRIANGLES,(void *)16);}
   puts("waiting for primitive query");glEndQuery(0x8c87);GLuint primitives=0;glGetQueryObjectuiv(query,GL_QUERY_RESULT,&primitives);
   printf("indexed=%u multi=%u primitives=%u expected=%u\n",indexed,multi,primitives,multi?3:2);CHECK(primitives==(multi?3:2));
   puts("reading pixel");unsigned char pixel[4]={0};glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);CHECK(pixel[0]>=63&&pixel[0]<=65&&pixel[1]>=127&&pixel[1]<=129&&pixel[2]>=190&&pixel[2]<=192);
   puts("reading original commands");glGetBufferSubData(0x8f3f,0,sizeof(readback),readback);CHECK(!memcmp(storage,readback,sizeof(storage)));CHECK(!error());
  }
 }
 puts("PASS native indirect arrays/elements, instances, offsets, padded multi-draw, negative base vertex, pixel readback and source preservation");
 return 0;
}
