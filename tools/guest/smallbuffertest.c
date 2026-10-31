/* Keep small static buffers live together, then verify reuse and isolation. */
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mach/mach_time.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line=%d GL=%x\n",__LINE__,glGetError()); return 1; } } while (0)
#define COUNT 1024
int main(int argc, char **argv)
{
   unsigned renderer=argc>1?strtoul(argv[1],0,16):0x1b00;
   CGLPixelFormatAttribute attrs[]={kCGLPFAAccelerated,kCGLPFARendererID,renderer,
      kCGLPFADisplayMask,argc>2?strtoul(argv[2],0,16):1,0};
   CGLPixelFormatObj pf; CGLContextObj ctx; GLint count,selected;
   CHECK(!CGLChoosePixelFormat(attrs,&pf,&count)&&pf);
   CHECK(!CGLCreateContext(pf,0,&ctx)&&!CGLSetCurrentContext(ctx));
   CHECK(!CGLGetParameter(ctx,kCGLCPCurrentRendererID,&selected));
   CHECK((selected&0xffff)==renderer);
   GLuint buffers[COUNT]; unsigned char data[256], readback[256];
   mach_timebase_info_data_t tb; mach_timebase_info(&tb);
   uint64_t start=mach_absolute_time();
   for(unsigned round=0;round<8;round++) {
      glGenBuffers(COUNT,buffers);
      for(unsigned i=0;i<COUNT;i++) {
         memset(data,(i+round)%251,sizeof(data));
         glBindBuffer(GL_ARRAY_BUFFER,buffers[i]);
         glBufferData(GL_ARRAY_BUFFER,sizeof(data),data,GL_STATIC_DRAW);
      }
      for(unsigned i=0;i<COUNT;i++) {
         glBindBuffer(GL_ARRAY_BUFFER,buffers[i]);
         glGetBufferSubData(GL_ARRAY_BUFFER,0,sizeof(readback),readback);
         memset(data,(i+round)%251,sizeof(data));
         CHECK(!memcmp(data,readback,sizeof(data))&&!glGetError());
      }
      glDeleteBuffers(COUNT,buffers);
   }
   printf("PASS renderer=%x buffers=%u rounds=8 bytes=256 elapsed_ms=%.3f\n",selected,COUNT,
          (mach_absolute_time()-start)*(double)tb.numer/tb.denom/1e6);
   CGLSetCurrentContext(NULL); CGLDestroyContext(ctx); CGLDestroyPixelFormat(pf);
   return 0;
}
