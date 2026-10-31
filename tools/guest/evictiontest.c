/* Exercise tiled live textures across eviction, GPU writes, and promotion. */
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s GL=%x\n",__LINE__,#x,glGetError()); return 1; } } while(0)
#define COUNT 16
#define SIDE 1024
int main(int argc,char **argv)
{
 setbuf(stdout,0);
 unsigned renderer=argc>1?strtoul(argv[1],0,16):0x1b00;
 CGLPixelFormatAttribute attrs[]={kCGLPFAAccelerated,kCGLPFARendererID,renderer,kCGLPFADisplayMask,argc>2?strtoul(argv[2],0,16):1,0};
 CGLPixelFormatObj pf; CGLContextObj ctx; GLint count,selected=0;
 CHECK(!CGLChoosePixelFormat(attrs,&pf,&count)&&pf);
 CHECK(!CGLCreateContext(pf,0,&ctx)&&!CGLSetCurrentContext(ctx));
 CHECK(!CGLGetParameter(ctx,kCGLCPCurrentRendererID,&selected));
 CHECK((selected&0xffff)==renderer);printf("renderer=%x %s\n",selected,glGetString(GL_RENDERER));
 unsigned char *pixels=malloc(SIDE*SIDE*4);CHECK(pixels);
 GLuint fbo;glGenFramebuffersEXT(1,&fbo);glBindFramebufferEXT(GL_FRAMEBUFFER_EXT,fbo);
 glDisable(GL_DITHER);
 for(unsigned round=0;round<3;round++) {
  GLuint textures[COUNT];glGenTextures(COUNT,textures);
  for(unsigned i=0;i<COUNT;i++) {
   glBindTexture(GL_TEXTURE_2D,textures[i]);
   glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
   glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,SIDE,SIDE,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
   glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT,GL_COLOR_ATTACHMENT0_EXT,GL_TEXTURE_2D,textures[i],0);
   CHECK(glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT)==GL_FRAMEBUFFER_COMPLETE_EXT);
   glClearColor((17+i+round)/255.0f,(53+i)/255.0f,(121+i)/255.0f,1);glClear(GL_COLOR_BUFFER_BIT);glFinish();CHECK(!glGetError());
  }
  GLuint sampled;glGenTextures(1,&sampled);glBindTexture(GL_TEXTURE_2D,sampled);
  glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,SIDE,SIDE,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
  glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT,GL_COLOR_ATTACHMENT0_EXT,GL_TEXTURE_2D,sampled,0);
  CHECK(glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT)==GL_FRAMEBUFFER_COMPLETE_EXT);
  glViewport(0,0,SIDE,SIDE);glEnable(GL_TEXTURE_2D);glTexEnvi(GL_TEXTURE_ENV,GL_TEXTURE_ENV_MODE,GL_REPLACE);
  for(unsigned i=0;i<COUNT;i++) {
   glBindTexture(GL_TEXTURE_2D,textures[i]);glBegin(GL_QUADS);
   glTexCoord2f(0,0);glVertex2f(-1,-1);glTexCoord2f(1,0);glVertex2f(1,-1);
   glTexCoord2f(1,1);glVertex2f(1,1);glTexCoord2f(0,1);glVertex2f(-1,1);glEnd();
   glReadPixels(SIDE/2,SIDE/2,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
   CHECK(!glGetError());CHECK(pixels[0]==17+i+round&&pixels[1]==53+i&&pixels[2]==121+i&&pixels[3]==255);
  }
  glDisable(GL_TEXTURE_2D);glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT,GL_COLOR_ATTACHMENT0_EXT,GL_TEXTURE_2D,textures[0],0);
  glDeleteTextures(1,&sampled);printf("PASS round=%u shader sampling all live textures\n",round);
  for(unsigned pass=0;pass<2;pass++) {
   for(unsigned i=0;i<COUNT;i++) {
    glBindTexture(GL_TEXTURE_2D,textures[i]);
    glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels);CHECK(!glGetError());
    for(unsigned j=0;j<SIDE*SIDE;j++) {
     CHECK(pixels[j*4]==17+i+round+pass && pixels[j*4+1]==53+i && pixels[j*4+2]==121+i && pixels[j*4+3]==255);
    }
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT,GL_COLOR_ATTACHMENT0_EXT,GL_TEXTURE_2D,textures[i],0);
    glClearColor((18+i+round+pass)/255.0f,(53+i)/255.0f,(121+i)/255.0f,1);glClear(GL_COLOR_BUFFER_BIT);glFinish();CHECK(!glGetError());
   }
   printf("PASS round=%u pass=%u all 64 MiB verified, including GPU writes\n",round,pass);
  }
  glDeleteTextures(COUNT-4,textures+4);
  for(unsigned i=0;i<4;i++) {
   glBindTexture(GL_TEXTURE_2D,textures[i]);glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
   CHECK(!glGetError());for(unsigned j=0;j<SIDE*SIDE;j++)CHECK(pixels[j*4]==19+i+round);
  }
  glDeleteTextures(4,textures);glFinish();printf("PASS round=%u release and reuse\n",round);
 }
 glDeleteFramebuffersEXT(1,&fbo);free(pixels);CGLSetCurrentContext(0);CGLDestroyContext(ctx);CGLDestroyPixelFormat(pf);return 0;
}
