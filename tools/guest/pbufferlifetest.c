/* An imported texture must retain storage after its pbuffer is destroyed. */
#include <stdio.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
extern CGLError CGLGetSurface(CGLContextObj, int *, int *, int *);
int main(void) {
 CGLPixelFormatAttribute a[]={kCGLPFAAccelerated,kCGLPFAPBuffer,0};
 CGLPixelFormatObj pf; GLint n; CGLContextObj producer,consumer; CGLPBufferObj pb;
 if(CGLChoosePixelFormat(a,&pf,&n)||CGLCreateContext(pf,0,&producer)||CGLCreateContext(pf,0,&consumer))return 1;
 CGLSetCurrentContext(producer);
 if(CGLCreatePBuffer(64,64,GL_TEXTURE_RECTANGLE_ARB,GL_RGBA,0,&pb)||CGLSetPBuffer(producer,pb,0,0,0))return 2;
 glClearColor(0,1,0,1);glClear(GL_COLOR_BUFFER_BIT);glFinish();
 int cid=0,wid=0,sid=0; CGLGetSurface(producer,&cid,&wid,&sid); CGLClearDrawable(producer);
 CGLSetCurrentContext(consumer); GLuint tex;glGenTextures(1,&tex);glBindTexture(GL_TEXTURE_RECTANGLE_ARB,tex);
 GLint v[]={sid,GL_TEXTURE_RECTANGLE_ARB,GL_RGBA,64,64,GL_UNSIGNED_INT_8_8_8_8_REV};
 CGLError e=CGLSetParameter(consumer,(CGLContextParameter)999,v);printf("import %d\n",e);if(e)return 3;
 CGLDestroyPBuffer(pb);
 for(int i=0;i<16;i++) {
  CGLSetCurrentContext(producer);
  if(CGLCreatePBuffer(64,64,GL_TEXTURE_RECTANGLE_ARB,GL_RGBA,0,&pb)||CGLSetPBuffer(producer,pb,0,0,0))return 4;
  glClearColor(1,0,0,1);glClear(GL_COLOR_BUFFER_BIT);glFinish();CGLClearDrawable(producer);CGLDestroyPBuffer(pb);
 }
 CGLSetCurrentContext(consumer);unsigned char pixels[64*64*4];
 glBindTexture(GL_TEXTURE_RECTANGLE_ARB,tex);glGetTexImage(GL_TEXTURE_RECTANGLE_ARB,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
 GLenum err=glGetError();int bad=0;for(int i=0;i<64*64;i++)if(pixels[i*4]||pixels[i*4+1]!=255||pixels[i*4+2]||pixels[i*4+3]!=255)bad++;
 printf("retained image: %u,%u,%u,%u error %x mismatched pixels %d: %s\n",pixels[0],pixels[1],pixels[2],pixels[3],err,bad,(!err&&!bad)?"PASS":"FAIL");
 glDeleteTextures(1,&tex);CGLSetCurrentContext(0);CGLDestroyContext(consumer);CGLDestroyContext(producer);CGLDestroyPixelFormat(pf);return err||bad;
}
