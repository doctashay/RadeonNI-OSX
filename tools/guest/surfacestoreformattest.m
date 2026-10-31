/* Validate native-endian UBO reads without an unbounded shader loop. */
#import <Cocoa/Cocoa.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <IOKit/IOKitLib.h>
#include "rdn_user.h"
#include "rdn_probe_device.h"
#define CHECK(x) do { if(!(x)){printf("FAIL line=%d error=%x\n",__LINE__,error());return 1;} } while(0)
#include <mach-o/dyld.h>
#include <mach-o/nlist.h>
static const char *image_path;
static void *local_symbol(const char *name) {
 for(unsigned i=0;i<_dyld_image_count();i++)if(!strcmp(_dyld_get_image_name(i),image_path)) {
  struct nlist n[2];memset(n,0,sizeof(n));n[0].n_un.n_name=(char *)name;
  if(!nlist(image_path,n)&&n[0].n_value)return (void *)(n[0].n_value+_dyld_get_image_vmaddr_slide(i));
 }
 return NULL;
}
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
 Dl_info image;if(dladdr(request,&image)){image_path=image.dli_fname;printf("driver=%s\n",image.dli_fname);}
 NSAutoreleasePool *pool=[[NSAutoreleasePool alloc] init];[NSApplication sharedApplication];
 NSOpenGLPixelFormatAttribute a[]={NSOpenGLPFAAccelerated,NSOpenGLPFANoRecovery,NSOpenGLPFARendererID,0x21a00+gpu*0x100,NSOpenGLPFAScreenMask,argc>2?strtoul(argv[2],0,16):CGDisplayIDToOpenGLDisplayMask(CGMainDisplayID()),0};
 NSOpenGLPixelFormat *pf=[[NSOpenGLPixelFormat alloc] initWithAttributes:a];
 NSOpenGLContext *ctx=[[NSOpenGLContext alloc] initWithFormat:pf shareContext:nil];
 if(!ctx)return 3;
 unsigned char pixels[64*64*4]={0};
 if(CGLSetOffScreen([ctx CGLContextObj],64,64,64*4,pixels)||CGLSetCurrentContext([ctx CGLContextObj])||!current([ctx CGLContextObj]))return 4;

 GLenum (*error)(void)=lookup("glGetError");
 void *(*mesa_current)(void)=local_symbol("_OSMesaGetCurrentContext");
 GLboolean (*make_current)(void *,void *,GLenum,GLsizei,GLsizei)=local_symbol("_OSMesaMakeCurrent");
 GLboolean (*import_store)(void *,GLenum,GLuint,GLsizei,GLuint,GLsizei,GLsizei)=local_symbol("_OSMesaTexStore");
 void (*gen)(GLsizei,GLuint *)=lookup("glGenTextures");
 void (*bind)(GLenum,GLuint)=lookup("glBindTexture");
 void (*getlevel)(GLenum,GLint,GLenum,GLint *)=lookup("glGetTexLevelParameteriv");
 void (*getimage)(GLenum,GLint,GLenum,GLenum,void *)=lookup("glGetTexImage");
 void (*destroy)(GLsizei,const GLuint *)=lookup("glDeleteTextures");
 CHECK(mesa_current&&make_current&&import_store&&gen&&bind&&getlevel&&getimage&&destroy);
 void *mesa=mesa_current();CHECK(mesa);
 io_service_t svc=rdn_probe_device(gpu?"6704":"675d");io_connect_t conn;CHECK(svc);
 CHECK(IOServiceOpen(svc,mach_task_self(),RDN_UC_TYPE_V2,&conn)==0);
 unsigned offset=0;CHECK(IOConnectMethodScalarIScalarO(conn,RDN_UC_ALLOC,2,1,65536,4096,&offset)==0);
 vm_address_t base=0;vm_size_t length=0;
 CHECK(IOConnectMapMemory(conn,RDN_UC_MEMORY_APERTURE,mach_task_self(),&base,&length,kIOMapAnywhere)==0);
 CHECK(offset<=length&&65536<=length-offset);
 unsigned char *store=(unsigned char *)(base+offset);memset(store,0,65536);
 for(unsigned y=0;y<16;y++)for(unsigned x=0;x<16;x++){
  unsigned char *p=store+y*256+x*4;p[0]=255;p[1]=64;p[2]=128;p[3]=192;
 }
 __asm__ volatile("sync":::"memory");
 const GLenum types[]={GL_UNSIGNED_BYTE,GL_UNSIGNED_SHORT,GL_FLOAT};
 unsigned char scratch[16*16*16]={0};unsigned failures=0;
 for(unsigned pass=0;pass<3;pass++){
  CHECK(make_current(mesa,scratch,types[pass],16,16));
  GLuint texture;gen(1,&texture);bind(GL_TEXTURE_RECTANGLE_ARB,texture);
  CHECK(import_store(mesa,GL_TEXTURE_RECTANGLE_ARB,0x7571,256,offset,16,16));
  GLint bits=0,format=0;getlevel(GL_TEXTURE_RECTANGLE_ARB,0,GL_TEXTURE_RED_SIZE,&bits);getlevel(GL_TEXTURE_RECTANGLE_ARB,0,GL_TEXTURE_INTERNAL_FORMAT,&format);
  printf("consumer_type=%x surface_red_bits=%d internal=%x\n",types[pass],bits,format);
  if(bits!=8){failures++;destroy(1,&texture);continue;}
  unsigned char rgba[16*16*4]={0};getimage(GL_TEXTURE_RECTANGLE_ARB,0,GL_RGBA,GL_UNSIGNED_BYTE,rgba);
  for(unsigned i=0;i<16*16;i++)if(rgba[i*4]!=64||rgba[i*4+1]!=128||rgba[i*4+2]!=192||rgba[i*4+3]!=255)failures++;
  CHECK(!error());destroy(1,&texture);
 }
 IOConnectUnmapMemory(conn,RDN_UC_MEMORY_APERTURE,mach_task_self(),base);
 IOConnectMethodScalarIScalarO(conn,RDN_UC_FREE,1,0,offset);IOServiceClose(conn);IOObjectRelease(svc);
 printf("%s packed ARGB surface imports in byte, ushort and float contexts: %u failures\n",failures?"FAIL":"PASS",failures);return failures?1:0;
}
