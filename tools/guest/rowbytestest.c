/* Quartz byte-stride texture uploads, including non-pixel-aligned rows. */
#include <stdio.h>
#include <string.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#define ROW_BYTES 0x8A16
int main(void)
{
    CGLPixelFormatAttribute attrs[]={kCGLPFARendererID,0x21a00,0};
    CGLPixelFormatObj pf; CGLContextObj ctx; GLint n, value;
    GLuint texture, pbo; unsigned char input[176], output[308];
    unsigned failures=0, x,y;
    if(CGLChoosePixelFormat(attrs,&pf,&n)||CGLCreateContext(pf,0,&ctx)||CGLSetCurrentContext(ctx)) return 2;
    glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);
    memset(input,0xee,sizeof(input));
    for(y=0;y<11;++y)for(x=0;x<7;++x)input[y*16+x]=y*7+x;
    glPixelStorei(GL_UNPACK_ROW_LENGTH,19);glPixelStorei(GL_UNPACK_ALIGNMENT,8);
    glPixelStorei(ROW_BYTES,16);
    glGetIntegerv(ROW_BYTES,&value);failures+=value!=16;
    glTexImage2D(GL_TEXTURE_2D,0,GL_ALPHA8,7,11,0,GL_ALPHA,GL_UNSIGNED_BYTE,input);
    glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,output);
    for(y=0;y<11;++y)for(x=0;x<7;++x)failures+=output[(y*7+x)*4+3]!=y*7+x;
    glGetIntegerv(GL_UNPACK_ROW_LENGTH,&value);failures+=value!=19;
    glGetIntegerv(GL_UNPACK_ALIGNMENT,&value);failures+=value!=8;
    for(y=0;y<11;++y)for(x=0;x<7;++x)input[y*16+x]=255-y*7-x;
    glTexSubImage2D(GL_TEXTURE_2D,0,0,0,7,11,GL_ALPHA,GL_UNSIGNED_BYTE,input);
    glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,output);
    for(y=0;y<11;++y)for(x=0;x<7;++x)failures+=output[(y*7+x)*4+3]!=255-y*7-x;
    memset(input,0xee,sizeof(input));
    for(y=0;y<3;++y){input[y*5]=y*80;input[y*5+1]=90;input[y*5+2]=160;}
    glPixelStorei(ROW_BYTES,5);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGB8,1,3,0,GL_RGB,GL_UNSIGNED_BYTE,input);
    glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,output);
    for(y=0;y<3;++y)failures+=output[y*4]!=y*80||output[y*4+1]!=90||output[y*4+2]!=160;
    glGenBuffers(1,&pbo);glBindBuffer(GL_PIXEL_UNPACK_BUFFER,pbo);
    glBufferData(GL_PIXEL_UNPACK_BUFFER,sizeof(input),input,GL_STATIC_DRAW);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGB8,1,3,0,GL_RGB,GL_UNSIGNED_BYTE,0);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER,0);
    glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,output);
    for(y=0;y<3;++y)failures+=output[y*4]!=y*80||output[y*4+1]!=90||output[y*4+2]!=160;
    glPixelStorei(ROW_BYTES,0);glGetIntegerv(ROW_BYTES,&value);failures+=value!=0;
    glPixelStorei(GL_UNPACK_ROW_LENGTH,0);glPixelStorei(GL_UNPACK_ALIGNMENT,1);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGB8,1,1,0,GL_RGB,GL_UNSIGNED_BYTE,input);
    GLenum error=glGetError();failures+=error!=0;
    printf("byte-stride uploads: %u failures, GL error 0x%x\n",failures,error);
    glDeleteBuffers(1,&pbo);glDeleteTextures(1,&texture);
    CGLSetCurrentContext(0);CGLDestroyContext(ctx);CGLDestroyPixelFormat(pf);
    return failures!=0;
}
