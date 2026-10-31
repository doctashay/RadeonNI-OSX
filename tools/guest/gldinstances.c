/* Load four renderer modules and check their shared CGL hook dispatch. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <OpenGL/OpenGL.h>
struct hook_api {
 int (*lookup)(void *(*)(const char *), void *(*)(const char *, void *));
 void (*current)(void (*)(void *));
};
static unsigned calls_a, calls_b;
static void after_a(void *ctx) { ++calls_a; }
static void after_b(void *ctx) { ++calls_b; }
int main(int argc, char **argv) {
 unsigned i; void *handles[4]; const struct hook_api *apis[4];
 if (argc != 5) return 2;
 setbuf(stdout,NULL);
 /* Leopard initializes CGL thread state during pixel-format setup. */
 CGLPixelFormatAttribute attrs[]={kCGLPFARendererID,0x20200,0};
 CGLPixelFormatObj format;GLint formats;
 if(CGLChoosePixelFormat(attrs,&format,&formats)||!format)return 9;
 CGLDestroyPixelFormat(format);
 for (i=0;i<4;++i) {
  handles[i]=dlopen(argv[i+1],RTLD_NOW|RTLD_LOCAL);
  if(!handles[i]) { fprintf(stderr,"%s\n",dlerror());return 3; }
  long (*version)(void*,void*,void*,void*)=dlsym(handles[i],"gldGetVersion");
  const struct hook_api *(*api)(void)=dlsym(handles[i],"RDNGLHookAPI");
  uint32_t a=0,b=0,c=0,id=0;
  if(!version||!api||!version(&a,&b,&c,&id)) return 4;
  apis[i]=api();
  printf("instance=%u version=%u,%u,%u renderer=%x api=%p\n",i,a,b,c,id,apis[i]);
  if(id!=0x1a00+i*0x100) return 5;
  if(i && apis[i]==apis[0])return 6;
  long (*info)(void*,long,long,long,long,long,long,long)=dlsym(handles[i],"gldGetRendererInfo");
  unsigned mask;
  for(mask=1;mask<=32;mask<<=1) {
   union { uint64_t align; unsigned char bytes[256]; } record;
   memset(&record,0,sizeof(record));
   if(!info || info(record.bytes,mask,0,0,0,0,0,0))return 10;
   uint32_t *words=(uint32_t *)(record.bytes+sizeof(void*)-4);
   if(words[1]!=id || words[12]<(16u<<20))return 11;
  }
  printf("instance=%u capability queries passed for six display bits\n",i);
 }
 apis[0]->current(after_a);apis[1]->current(after_b);
 if(CGLSetCurrentContext(NULL))return 7;
 printf("shared hooks=%u,%u\n",calls_a,calls_b);
 if(calls_a!=1||calls_b!=1)return 8;
 puts("PASS: four isolated renderers, shared CGL hook dispatch");return 0;
}
