/* Bounded offscreen shader load with per-second thermal telemetry. */
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <dlfcn.h>
#include "rdn_user.h"
static double now(void){struct timeval t;gettimeofday(&t,0);return t.tv_sec+t.tv_usec/1000000.0;}
static unsigned readreg(io_connect_t c,unsigned r){unsigned v=0;IOConnectMethodScalarIScalarO(c,RDN_UC_REG_READ,1,1,r,&v);return v;}
#define REQUIRE(x) do{if(!(x)){printf("FAIL line %d\n",__LINE__);return 1;}}while(0)
int main(int argc,char **argv)
{
 unsigned seconds=argc>1?atoi(argv[1]):5;REQUIRE(seconds>=1&&seconds<=15);setbuf(stdout,0);
 CGLPixelFormatAttribute attrs[]={kCGLPFAAccelerated,kCGLPFANoRecovery,kCGLPFARendererID,0x21a00,0};
 CGLPixelFormatObj pf;CGLContextObj context;GLint n,ok=0;GLuint fbo,texture;
 REQUIRE(!CGLChoosePixelFormat(attrs,&pf,&n)&&pf);REQUIRE(!CGLCreateContext(pf,0,&context));
 CGLError (*current)(CGLContextObj)=dlsym(RTLD_DEFAULT,"CGLSetCurrentContext");REQUIRE(current&&!current(context));
 printf("renderer=%s version=%s\n",glGetString(GL_RENDERER),glGetString(GL_VERSION));
 glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,2048,2048,0,GL_RGBA,GL_UNSIGNED_BYTE,0);
 glGenFramebuffersEXT(1,&fbo);glBindFramebufferEXT(GL_FRAMEBUFFER_EXT,fbo);glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT,GL_COLOR_ATTACHMENT0_EXT,GL_TEXTURE_2D,texture,0);
 REQUIRE(glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT)==GL_FRAMEBUFFER_COMPLETE_EXT);
 const char *source="#version 120\nuniform float phase;void main(){vec4 v=vec4(gl_FragCoord.xy/2048.0,phase,0.9);for(int i=0;i<32;i++)v=fract(sin(v*1.01+float(i)*0.1)*1.001+0.1);gl_FragColor=v;}";
 GLhandleARB shader=glCreateShaderObjectARB(GL_FRAGMENT_SHADER_ARB);glShaderSourceARB(shader,1,&source,0);glCompileShaderARB(shader);glGetObjectParameterivARB(shader,GL_OBJECT_COMPILE_STATUS_ARB,&ok);REQUIRE(ok);
 GLhandleARB program=glCreateProgramObjectARB();glAttachObjectARB(program,shader);glLinkProgramARB(program);glGetObjectParameterivARB(program,GL_OBJECT_LINK_STATUS_ARB,&ok);REQUIRE(ok);
 glUseProgramObjectARB(program);GLint phase=glGetUniformLocationARB(program,"phase");glViewport(0,0,2048,2048);glDisable(GL_DEPTH_TEST);
 io_connect_t conn;io_service_t service=IOServiceGetMatchingService(0,IOServiceMatching(RDN_UC_SERVICE_CLASS));REQUIRE(service&&!IOServiceOpen(service,mach_task_self(),RDN_UC_TYPE,&conn));
 double start=now(),last=start;unsigned frames=0,total=0;int stopped=0;
 while(now()-start<seconds) {
  glUniform1fARB(phase,(total%1000)/1000.0);glBegin(GL_QUADS);glVertex2f(-1,-1);glVertex2f(1,-1);glVertex2f(1,1);glVertex2f(-1,1);glEnd();glFinish();REQUIRE(glGetError()==GL_NO_ERROR);frames++;total++;
  double t=now();if(t-last>=1.0) {
   unsigned s=0,m=0,temp=0;REQUIRE(!IOConnectMethodScalarIScalarO(conn,RDN_UC_POWER,2,3,0,0,&s,&m,&temp));
   printf("t=%.2f fps=%.2f core=%.2f memory=%.2f temp=%.1f profile=%08x power=%08x thermal=%08x\n",t-start,frames/(t-last),s/100.0,m/100.0,((int)temp-RDN_UC_TEMPERATURE_BIAS)/1000.0,readreg(conn,0x66c),readreg(conn,0x63c),readreg(conn,0x72c));
   if(temp>=RDN_UC_TEMPERATURE_BIAS+90000){stopped=1;break;}frames=0;last=t;
  }
 }
 unsigned char pixel[4];glReadPixels(1024,1024,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);REQUIRE(glGetError()==GL_NO_ERROR);
 printf("%s frames=%u elapsed=%.2f pixel=%u,%u,%u,%u\n",stopped?"STOP temperature limit":"PASS",total,now()-start,pixel[0],pixel[1],pixel[2],pixel[3]);
 IOServiceClose(conn);IOObjectRelease(service);glDeleteFramebuffersEXT(1,&fbo);glDeleteTextures(1,&texture);glDeleteObjectARB(program);glDeleteObjectARB(shader);
 current(0);CGLDestroyContext(context);CGLDestroyPixelFormat(pf);return stopped?2:0;
}
