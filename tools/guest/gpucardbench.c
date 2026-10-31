/* Repeatable offscreen fill, texture and ALU throughput with pixel checks. */
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <sys/resource.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dlfcn.h>
#include "rdn_user.h"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static mach_timebase_info_data_t tb;
static double now(void){return mach_absolute_time()*(double)tb.numer/tb.denom/1e9;}
static double cpu(void){struct rusage r;getrusage(RUSAGE_SELF,&r);return r.ru_utime.tv_sec+r.ru_stime.tv_sec+(r.ru_utime.tv_usec+r.ru_stime.tv_usec)/1e6;}
struct sample {double core,memory,temp;};
static io_connect_t conn;
static int sample(struct sample *s)
{
 unsigned c=0,m=0,t=0;int e=IOConnectMethodScalarIScalarO(conn,RDN_UC_POWER,2,3,0,0,&c,&m,&t);
 s->core=c/100.0;s->memory=m/100.0;s->temp=((int)t-RDN_UC_TEMPERATURE_BIAS)/1000.0;return !e;
}
static unsigned quantize(float f){if(f<0)f=0;if(f>1)f=1;return (unsigned)(f*255+0.5f);}
int main(int argc,char **argv)
{
 double seconds=argc>1?atof(argv[1]):2;unsigned width=argc>2?atoi(argv[2]):1920,height=argc>3?atoi(argv[3]):1080;
 CHECK(seconds>=1&&seconds<=10&&width>=64&&height>=64&&width<=4096&&height<=4096);setbuf(stdout,0);mach_timebase_info(&tb);
 io_service_t service=IOServiceGetMatchingService(0,IOServiceMatching(RDN_UC_SERVICE_CLASS));CHECK(service&&!IOServiceOpen(service,mach_task_self(),RDN_UC_TYPE,&conn));IOObjectRelease(service);
 struct rdn_user_info info;IOByteCount size=sizeof(info);CHECK(!IOConnectMethodScalarIStructureO(conn,RDN_UC_GET_INFO,0,&size,&info));
 CGLPixelFormatAttribute attrs[]={kCGLPFAAccelerated,kCGLPFANoRecovery,kCGLPFARendererID,0x21a00,0};
 CGLPixelFormatObj pf;CGLContextObj context;GLint n,ok;GLuint fbo,target,texture,vbo;
 CHECK(!CGLChoosePixelFormat(attrs,&pf,&n)&&pf);CHECK(!CGLCreateContext(pf,0,&context));
 CGLError (*current)(CGLContextObj)=dlsym(RTLD_DEFAULT,"CGLSetCurrentContext");CHECK(current&&!current(context));
 CHECK(glGetString(GL_RENDERER));
 printf("{\"renderer\":\"%s\",\"version\":\"%s\",\"pci_id\":\"%04x\",\"width\":%u,\"height\":%u,\"seconds\":%.1f}\n",glGetString(GL_RENDERER),glGetString(GL_VERSION),info.pci_device_id,width,height,seconds);
 glGenTextures(1,&target);glBindTexture(GL_TEXTURE_2D,target);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,width,height,0,GL_RGBA,GL_UNSIGNED_BYTE,0);
 glGenFramebuffersEXT(1,&fbo);glBindFramebufferEXT(GL_FRAMEBUFFER_EXT,fbo);glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT,GL_COLOR_ATTACHMENT0_EXT,GL_TEXTURE_2D,target,0);
 CHECK(glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT)==GL_FRAMEBUFFER_COMPLETE_EXT);
 unsigned char *pixels=malloc(512*512*4);CHECK(pixels);
 for(unsigned y=0;y<512;y++)for(unsigned x=0;x<512;x++){unsigned i=4*(y*512+x);pixels[i]=32+(x&31)*4;pixels[i+1]=48+(y&31)*4;pixels[i+2]=64+((x^y)&31)*4;pixels[i+3]=255;}
 glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
 glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,512,512,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels);free(pixels);
 const GLfloat vertices[]={-1,-1,0,0,1,-1,1,0,-1,1,0,1,1,1,1,1};
 glGenBuffersARB(1,&vbo);glBindBufferARB(GL_ARRAY_BUFFER_ARB,vbo);glBufferDataARB(GL_ARRAY_BUFFER_ARB,sizeof(vertices),vertices,GL_STATIC_DRAW_ARB);
 glVertexPointer(2,GL_FLOAT,4*sizeof(GLfloat),(void *)0);glTexCoordPointer(2,GL_FLOAT,4*sizeof(GLfloat),(void *)(2*sizeof(GLfloat)));
 glEnableClientState(GL_VERTEX_ARRAY);glEnableClientState(GL_TEXTURE_COORD_ARRAY);
 glMatrixMode(GL_PROJECTION);glLoadIdentity();glMatrixMode(GL_MODELVIEW);glLoadIdentity();glViewport(0,0,width,height);glDisable(GL_DEPTH_TEST);
 const char *source="#version 120\nuniform vec2 invSize;void main(){vec3 v=vec3(gl_FragCoord.xy*invSize,0.25);for(int i=0;i<64;i++)v=sin(v*0.75+vec3(0.1,0.2,0.3));gl_FragColor=vec4(v,1.0);}";
 GLhandleARB shader=glCreateShaderObjectARB(GL_FRAGMENT_SHADER_ARB);glShaderSourceARB(shader,1,&source,0);glCompileShaderARB(shader);glGetObjectParameterivARB(shader,GL_OBJECT_COMPILE_STATUS_ARB,&ok);
 if(!ok){char log[2048];glGetInfoLogARB(shader,sizeof(log),0,log);fprintf(stderr,"%s\n",log);}CHECK(ok);
 GLhandleARB program=glCreateProgramObjectARB();glAttachObjectARB(program,shader);glLinkProgramARB(program);glGetObjectParameterivARB(program,GL_OBJECT_LINK_STATUS_ARB,&ok);CHECK(ok);
 glUseProgramObjectARB(program);glUniform2fARB(glGetUniformLocationARB(program,"invSize"),1.0f/width,1.0f/height);glUseProgramObjectARB(0);
 const char *names[]={"blend_fill","texture","alu_sine64"};int failed=0;
 for(unsigned scene=0;scene<3;scene++) {
  struct sample before,after;CHECK(sample(&before));CHECK(before.temp<85);
  glUseProgramObjectARB(scene==2?program:0);glDisable(GL_TEXTURE_2D);glDisable(GL_BLEND);glClearColor(0,0,0,0);glClear(GL_COLOR_BUFFER_BIT);
  if(scene==0){glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);glColor4f(.25,.5,.75,.5);}
  if(scene==1){glEnable(GL_TEXTURE_2D);glBindTexture(GL_TEXTURE_2D,texture);glTexEnvi(GL_TEXTURE_ENV,GL_TEXTURE_ENV_MODE,GL_REPLACE);glColor4f(1,1,1,1);}
  unsigned scale=argc>5?atoi(argv[5]):1;CHECK(scale>=1&&scale<=16);
  unsigned batch=(scene==2?1:32)*scale,frames=0;
  double warm=now();do{glDrawArrays(GL_TRIANGLE_STRIP,0,4);glFinish();}while(now()-warm<.3);
  CHECK(glGetError()==GL_NO_ERROR);double start=now(),cpuStart=cpu(),poll=start;
  do {
   for(unsigned j=0;j<batch;j++)glDrawArrays(GL_TRIANGLE_STRIP,0,4);
   glFinish();frames+=batch;
   if(now()-poll>=1){CHECK(sample(&after));CHECK(after.temp<85);poll=now();}
  }while(now()-start<seconds);
  double elapsed=now()-start,cpuTime=cpu()-cpuStart;CHECK(glGetError()==GL_NO_ERROR);CHECK(sample(&after));
  unsigned char pixel[4];glReadPixels(width/2,height/2,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);CHECK(glGetError()==GL_NO_ERROR);
  unsigned expected[4]={64,128,191,128};
  if(scene==1){unsigned x=(unsigned)((width/2+.5)*512/width),y=(unsigned)((height/2+.5)*512/height);expected[0]=32+(x&31)*4;expected[1]=48+(y&31)*4;expected[2]=64+((x^y)&31)*4;expected[3]=255;}
  if(scene==2){float v[3]={(width/2+.5f)/width,(height/2+.5f)/height,.25f};for(unsigned j=0;j<64;j++)for(unsigned k=0;k<3;k++)v[k]=sinf(v[k]*.75f+.1f*(k+1));for(unsigned k=0;k<3;k++)expected[k]=quantize(v[k]);expected[3]=255;}
  int correct=1;unsigned delta=0;for(unsigned k=0;k<4;k++){unsigned d=abs((int)pixel[k]-(int)expected[k]);if(d>delta)delta=d;if(d>3)correct=0;}
  printf("{\"scene\":\"%s\",\"width\":%u,\"height\":%u,\"frames\":%u,\"wall_ms\":%.3f,\"cpu_ms\":%.3f,\"fps\":%.3f,\"mpixels_s\":%.3f,\"core_before_mhz\":%.2f,\"core_after_mhz\":%.2f,\"memory_before_mhz\":%.2f,\"memory_after_mhz\":%.2f,\"temp_before_c\":%.1f,\"temp_after_c\":%.1f,\"pixel\":[%u,%u,%u,%u],\"max_pixel_delta\":%u,\"correct\":%s}\n",names[scene],width,height,frames,elapsed*1000,cpuTime*1000,frames/elapsed,(double)frames*width*height/elapsed/1e6,before.core,after.core,before.memory,after.memory,before.temp,after.temp,pixel[0],pixel[1],pixel[2],pixel[3],delta,correct?"true":"false");
  if(argc>4 && argv[4][0]!='-') {
   size_t bytes=(size_t)width*height*4;unsigned char *image=malloc(bytes);CHECK(image);
   glReadPixels(0,0,width,height,GL_RGBA,GL_UNSIGNED_BYTE,image);CHECK(glGetError()==GL_NO_ERROR);
   char path[1024];snprintf(path,sizeof(path),"%s-%s.rgba",argv[4],names[scene]);FILE *f=fopen(path,"wb");CHECK(f);
   CHECK(fwrite(image,1,bytes,f)==bytes);fclose(f);free(image);
  }
  if(!correct)failed=1;
 }
 glFinish();glUseProgramObjectARB(0);glDeleteObjectARB(program);glDeleteObjectARB(shader);glDeleteBuffersARB(1,&vbo);glDeleteFramebuffersEXT(1,&fbo);glDeleteTextures(1,&texture);glDeleteTextures(1,&target);
 current(0);CGLDestroyContext(context);CGLDestroyPixelFormat(pf);IOServiceClose(conn);return failed;
}
