/* Leopard exposes renderer memory in signed byte-valued CGL properties. */
#include <OpenGL/OpenGL.h>
#include <stdio.h>
int main(void)
{
 CGLRendererInfoObj info=0;GLint count=0;unsigned found=0;
 if(CGLQueryRendererInfo(~0u,&info,&count))return 1;
 for(GLint i=0;i<count;i++) {
  GLint renderer=0,video=0,texture=0,mask=0;
  if(CGLDescribeRenderer(info,i,kCGLRPRendererID,&renderer))return 1;
  if((renderer&0xffff)<0x1a00||(renderer&0xffff)>0x1d00)continue;
  if(CGLDescribeRenderer(info,i,kCGLRPVideoMemory,&video)||
     CGLDescribeRenderer(info,i,kCGLRPTextureMemory,&texture)||
     CGLDescribeRenderer(info,i,kCGLRPDisplayMask,&mask))return 1;
  printf("renderer=%x mask=%x video_bytes=%d texture_bytes=%d\n",renderer,mask,video,texture);
  if(video<=0||texture<=0)return 1;
  found++;
 }
 CGLDestroyRendererInfo(info);
 printf("%s renderer memory: %u GPUs\n",found>=2?"PASS":"FAIL",found);
 return found<2;
}
