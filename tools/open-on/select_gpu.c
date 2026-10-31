/* Per-launch CGL selection; explicit software contexts remain software. */
#include <OpenGL/OpenGL.h>
#include <OpenGL/CGLRenderers.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
static pthread_once_t once=PTHREAD_ONCE_INIT;
static pthread_key_t choosing;
static CGLError (*original_choose)(const CGLPixelFormatAttribute *,CGLPixelFormatObj *,GLint *);
static uint32_t display_mask;
static int number(const char *name,uint32_t *value)
{
 const char *s=getenv(name);char *end;unsigned long v;
 if(!s||!*s||*s=='-')return 0;
 v=strtoul(s,&end,0);if(*end||!v||v>UINT32_MAX)return 0;
 *value=(uint32_t)v;return 1;
}
static void initialize(void)
{
 original_choose=CGLChoosePixelFormat;pthread_key_create(&choosing,NULL);
 if(!number("RDN_FORCE_DISPLAY_MASK",&display_mask))display_mask=0;
}
static int valued(int attribute)
{
 switch(attribute){case 7:case 8:case 11:case 12:case 13:case 14:case 20:case 21:case 22:case 55:case 56:case 70:case 84:case 99:return 1;default:return 0;}
}
static CGLError choose_gpu(const CGLPixelFormatAttribute *input,CGLPixelFormatObj *format,GLint *count)
{
 pthread_once(&once,initialize);
 if(!original_choose || original_choose==choose_gpu)return kCGLBadState;
 if(!display_mask || pthread_getspecific(choosing))return original_choose(input,format,count);
 CGLPixelFormatAttribute attributes[128];unsigned n=0;
 for(unsigned i=0;input && input[i];) {
  if(i>=120)return kCGLBadAttribute;
  int key=input[i++],has_value=valued(key),value=has_value?input[i++]:0;
  if(key==kCGLPFARendererID && ((value&0xffff)==(kCGLRendererGenericID&0xffff)||(value&0xffff)==(kCGLRendererGenericFloatID&0xffff)))return original_choose(input,format,count);
  if(key==kCGLPFARendererID || key==kCGLPFADisplayMask || key==kCGLPFAAllRenderers || key==kCGLPFASingleRenderer || key==kCGLPFAAccelerated || key==kCGLPFANoRecovery)continue;
  if(n+has_value+9>=128)return kCGLBadAttribute;
  attributes[n++]=key;if(has_value)attributes[n++]=value;
 }
 attributes[n++]=kCGLPFADisplayMask;attributes[n++]=display_mask;
 attributes[n++]=kCGLPFAAccelerated;
 attributes[n++]=kCGLPFANoRecovery;attributes[n]=0;
 pthread_setspecific(choosing,(void *)1);
 CGLError error=original_choose(attributes,format,count);
 pthread_setspecific(choosing,NULL);return error;
}
__attribute__((used,section("__DATA,__interpose")))
static const struct {const void *replacement,*original;} interpose[]={
 {(const void *)choose_gpu,(const void *)CGLChoosePixelFormat}
};
