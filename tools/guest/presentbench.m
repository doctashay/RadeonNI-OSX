/* Window presentation throughput and front-buffer correctness. */
#import <Cocoa/Cocoa.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <mach/mach_time.h>
#include <sys/resource.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
static mach_timebase_info_data_t tb;
static double now(void){return mach_absolute_time()*(double)tb.numer/tb.denom/1e9;}
static double cpu(void){struct rusage r;getrusage(RUSAGE_SELF,&r);return r.ru_utime.tv_sec+r.ru_stime.tv_sec+(r.ru_utime.tv_usec+r.ru_stime.tv_usec)/1e6;}
static int cmp(const void *a,const void *b){double x=*(double*)a,y=*(double*)b;return (x>y)-(x<y);}
int main(int argc,char **argv)
{
 int width=argc>3?atoi(argv[3]):639,height=argc>4?atoi(argv[4]):479;
 if(width<1||height<1||width>4096||height>4096)return 2;
 NSAutoreleasePool *pool=[[NSAutoreleasePool alloc] init];[NSApplication sharedApplication];
 if(argc>1)freopen(argv[1],"w",stdout);setbuf(stdout,0);mach_timebase_info(&tb);
 NSOpenGLPixelFormatAttribute attrs[]={NSOpenGLPFAAccelerated,NSOpenGLPFADoubleBuffer,0};
 NSOpenGLPixelFormat *pf=[[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];if(!pf)return 2;
 NSOpenGLView *view=[[NSOpenGLView alloc] initWithFrame:NSMakeRect(0,0,width,height) pixelFormat:pf];
 NSWindow *window=[[NSWindow alloc] initWithContentRect:NSMakeRect(40,160,width,height) styleMask:NSTitledWindowMask backing:NSBackingStoreBuffered defer:NO];
 [window setReleasedWhenClosed:NO];[window setContentView:view];[window setTitle:@"Radeon presentation benchmark"];[window orderFront:nil];
 [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.25]];
 NSOpenGLContext *ctx=[view openGLContext];[ctx makeCurrentContext];GLint zero=getenv("RDN_BENCH_SWAP_INTERVAL")?atoi(getenv("RDN_BENCH_SWAP_INTERVAL")):0;[ctx setValues:&zero forParameter:NSOpenGLCPSwapInterval];
 if(argc>6) {
  [window setFrameOrigin:NSMakePoint(atoi(argv[5]),atoi(argv[6]))];[ctx update];
  [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.5]];
 }
 printf("renderer=%s queue=%s\n",glGetString(GL_RENDERER),getenv("RDN_GLD_SURFACE_QUEUE")?getenv("RDN_GLD_SURFACE_QUEUE"):"default");
 GLint renderer=0,doubled=0;
 CGLGetParameter(CGLGetCurrentContext(),kCGLCPCurrentRendererID,&renderer);
 glGetIntegerv(GL_DOUBLEBUFFER,&doubled);
 printf("renderer_id=%x double_buffer=%d size=%dx%d\n",renderer,doubled,width,height);
 unsigned char initial[4]={255,255,255,255};
 glReadBuffer(GL_BACK);glReadPixels(width/2,height/2,1,1,GL_RGBA,GL_UNSIGNED_BYTE,initial);
 printf("initial_back=[%u,%u,%u,%u]\n",initial[0],initial[1],initial[2],initial[3]);
 if(initial[0]||initial[1]||initial[2]||glGetError())return 4;
 double *times=malloc(250000*sizeof(double));if(!times)return 3;
 glViewport(0,0,width,height);glDisable(GL_DITHER);glClearColor(.25,.5,.75,1);
 for(int i=0;i<20;i++){glClear(GL_COLOR_BUFFER_BIT);[ctx flushBuffer];}glFinish();
 int stress=argc>2 && atoi(argv[2])!=0;
 double start=now(),cs=cpu();unsigned frames=0;
 while(now()-start<(argc>7?atoi(argv[7]):(stress?8:5)) && frames<250000) {
  if(stress && !(frames%37)) {
   [window setContentSize:NSMakeSize(639+(frames%2)*16,479+(frames%3)*8)];
   [ctx update];[[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.005]];
  }
  double t=now();glClear(GL_COLOR_BUFFER_BIT);[ctx flushBuffer];times[frames++]=(now()-t)*1000;
  if(!(frames&31))[[NSRunLoop currentRunLoop] runUntilDate:[NSDate date]];
 }
 double elapsed=now()-start,used=cpu()-cs;[ctx makeCurrentContext];glFinish();unsigned char pixel[4]={0};glReadBuffer(GL_FRONT);glReadPixels(width/2,height/2,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
 GLenum error=glGetError();int correct=doubled&&abs((int)pixel[0]-64)<=2&&abs((int)pixel[1]-128)<=2&&abs((int)pixel[2]-191)<=2;
 qsort(times,frames,sizeof(double),cmp);
 printf("{\"frames\":%u,\"fps\":%.3f,\"wall_ms\":%.3f,\"cpu_ms\":%.3f,\"p50_ms\":%.3f,\"p95_ms\":%.3f,\"p99_ms\":%.3f,\"gl_error\":%u,\"pixel\":[%u,%u,%u,%u],\"correct\":%s}\n",frames,frames/elapsed,elapsed*1000,used*1000,times[frames/2],times[frames*95/100],times[frames*99/100],error,pixel[0],pixel[1],pixel[2],pixel[3],correct?"true":"false");
 free(times);[ctx clearDrawable];[window close];[view release];[window release];[pf release];[pool release];return !correct||error;
}
