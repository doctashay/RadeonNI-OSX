/* Exercise GART allocation, reuse and GPU reads from the end of large PBOs. */
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s error=%x\n",__LINE__,#x,glGetError()); return 1; } } while(0)
int main(int argc,char **argv)
{
 unsigned renderer=argc>1?strtoul(argv[1],0,16):0x1b00;
 CGLPixelFormatAttribute attrs[]={kCGLPFAAccelerated,kCGLPFARendererID,renderer,kCGLPFADisplayMask,argc>2?strtoul(argv[2],0,16):1,0};
 CGLPixelFormatObj pf; CGLContextObj ctx; GLint count; GLuint buffers[3],tex;
 REQUIRE(!CGLChoosePixelFormat(attrs,&pf,&count)&&pf);
 REQUIRE(!CGLCreateContext(pf,0,&ctx)&&!CGLSetCurrentContext(ctx));
 GLint selected=0;REQUIRE(!CGLGetParameter(ctx,kCGLCPCurrentRendererID,&selected));
 REQUIRE((selected&0xffff)==(attrs[2]&0xffff));
 printf("renderer_id=%x\n",selected);
 unsigned char *readback=malloc(1024*1024); REQUIRE(readback);setbuf(stdout,0);
 glGenTextures(1,&tex);glBindTexture(GL_TEXTURE_2D,tex);
 for(unsigned round=0;round<4;round++) {
  glGenBuffers(3,buffers);
  for(unsigned i=0;i<3;i++) {
   size_t size=(120+round*4)*1024u*1024u;
   unsigned char pattern=37+round*13+i;
   glBindBuffer(GL_PIXEL_UNPACK_BUFFER,buffers[i]);
   glBufferData(GL_PIXEL_UNPACK_BUFFER,size,NULL,GL_STREAM_DRAW); REQUIRE(!glGetError());
   unsigned char *p=glMapBuffer(GL_PIXEL_UNPACK_BUFFER,GL_WRITE_ONLY);REQUIRE(p);
   memset(p+size-1024*1024,pattern,1024*1024);REQUIRE(glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER));
   glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,512,512,0,GL_RGBA,GL_UNSIGNED_BYTE,(void *)(size-1024*1024));
   glBindBuffer(GL_PIXEL_UNPACK_BUFFER,0);glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,readback);REQUIRE(!glGetError());
   for(unsigned j=0;j<1024*1024;j++)REQUIRE(readback[j]==pattern);
  }
  glDeleteBuffers(3,buffers);glFinish();printf("PASS round=%u live_PBO_MiB=%u\n",round,3*(120+round*4));
 }
 glDeleteTextures(1,&tex);free(readback);CGLSetCurrentContext(0);CGLDestroyContext(ctx);CGLDestroyPixelFormat(pf);return 0;
}

