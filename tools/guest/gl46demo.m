/* Standalone Mesa core-profile demo; Cocoa only presents the rendered pixels.
 */
#import <Cocoa/Cocoa.h>
#include <OpenGL/gl.h>
#include <dlfcn.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#define W 640
#define H 360
#define PARTICLES 8192
static void *context, *library;
static unsigned char *pixels;
static void *(*getproc)(const char *);
static GLuint program, vao, computeProgram, clearProgram, particleBuffer,
    imageBuffer;
static GLint computeTime, computeDelta, computeInit;
static GLint timeUniform;
#define PROC(ret, name, args) static ret(*p##name) args
PROC(GLuint, CreateShader, (GLenum));
PROC(void, ShaderSource, (GLuint, GLsizei, const char **, const GLint *));
PROC(void, CompileShader, (GLuint));
PROC(void, GetShaderiv, (GLuint, GLenum, GLint *));
PROC(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei *, char *));
PROC(GLuint, CreateProgram, (void));
PROC(void, AttachShader, (GLuint, GLuint));
PROC(void, LinkProgram, (GLuint));
PROC(void, GetProgramiv, (GLuint, GLenum, GLint *));
PROC(void, GetProgramInfoLog, (GLuint, GLsizei, GLsizei *, char *));
PROC(void, UseProgram, (GLuint));
PROC(void, GenVertexArrays, (GLsizei, GLuint *));
PROC(void, BindVertexArray, (GLuint));
PROC(GLint, GetUniformLocation, (GLuint, const char *));
PROC(void, Uniform1f, (GLint, GLfloat));
PROC(void, Viewport, (GLint, GLint, GLsizei, GLsizei));
PROC(void, DrawArrays, (GLenum, GLint, GLsizei));
PROC(void, Finish, (void));
PROC(GLenum, GetError, (void));
PROC(const GLubyte *, GetString, (GLenum));
PROC(void, GenBuffers, (GLsizei, GLuint *));
PROC(void, BindBuffer, (GLenum, GLuint));
PROC(void, BufferData, (GLenum, ptrdiff_t, const void *, GLenum));
PROC(void, BindBufferBase, (GLenum, GLuint, GLuint));

PROC(void, DispatchCompute, (GLuint, GLuint, GLuint));
PROC(void, MemoryBarrier, (GLbitfield));
PROC(void, Uniform1i, (GLint, GLint));

static const char *computeSource =
    "#version 460 core\n"
    "layout(local_size_x=128) in; struct Particle{vec4 position;vec4 "
    "velocity;};"
    "layout(std430,binding=0) buffer State{Particle particles[];};"
    "layout(std430,binding=1) buffer Image{uvec4 glow[];};uniform float "
    "time,dt;uniform int initialize;"
    "float hash(float x){return fract(sin(x*127.1)*43758.5453);}"
    "void main(){uint id=gl_GlobalInvocationID.x;if(id>=8192u)return;"
    "vec3 p=particles[id].position.xyz,v=particles[id].velocity.xyz;"
    "if(initialize!=0){float "
    "a=float(id)*2.39996323,z=1.-2.*(float(id)+.5)/8192.;"
    "p=vec3(cos(a)*sqrt(1.-z*z),z,sin(a)*sqrt(1.-z*z))*(1.8+hash(float(id))*.8)"
    ";"
    "v=cross(vec3(0,.7,0),p);}"
    "else{float r=max(length(p),.1);vec3 "
    "curl=vec3(sin(p.y*2.+time*.4),sin(p.z*2.+time*.3),sin(p.x*2.+time*.2));"
    "vec3 acceleration=-p*.32+curl*.24+normalize(p)*(2.15-r)*1.2;"
    "v=(v+acceleration*dt)*exp(-.12*dt);p+=v*dt;}"
    "particles[id].position=vec4(p,1);particles[id].velocity=vec4(v,0);"
    "vec3 ro=vec3(3.5*sin(time*.13),1.2,3.5*cos(time*.13));vec3 "
    "fw=normalize(-ro),rt=normalize(cross(fw,vec3(0,1,0))),up=cross(rt,fw);"
    "vec3 rel=p-ro;float depth=dot(rel,fw);if(depth<.1)return;"
    "ivec2 "
    "xy=ivec2(vec2(dot(rel,rt),dot(rel,up))*1.45/depth*360.+vec2(320,180));"
    "vec3 tint=mix(vec3(.03,.4,.8),vec3(.6,.08,.4),clamp(length(v),0.,1.));"
    "for(int y=-1;y<=1;y++)for(int x=-1;x<=1;x++){ivec2 "
    "q=xy+ivec2(x,y);if(any(lessThan(q,ivec2(0)))||any(greaterThanEqual(q,"
    "ivec2(640,360))))continue;"
    "uint at=uint(q.y*640+q.x);uvec3 rgb=uvec3(tint*(120./float(1+x*x+y*y)));"
    "atomicAdd(glow[at].x,rgb.x);atomicAdd(glow[at].y,rgb.y);atomicAdd(glow[at]"
    ".z,rgb.z);}}\n";
static const char *clearSource =
    "#version 460 core\nlayout(local_size_x=128) in;layout(std430,binding=1) "
    "buffer Image{uvec4 glow[];};"
    "void main(){uint "
    "i=gl_GlobalInvocationID.x;if(i<230400u)glow[i]=uvec4(0);}";
static const char *vertex = "#version 460 core\n"
                            "void main(){vec2 "
                            "p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_"
                            "Position=vec4(p*2.-1.,0.,1.);}";
static const char *fragment =
    "#version 460 core\n"
    "layout(std430,binding=1) readonly buffer Image{uvec4 "
    "glow[];};layout(location=0) out vec4 color; uniform float time;\n"
    "mat2 rot(float a){return mat2(cos(a),-sin(a),sin(a),cos(a));}\n"
    "float shape(vec3 p){p.xz=rot(time*.25)*p.xz;p.xy=rot(.45+time*.19)*p.xy;"
    "float tor=length(vec2(length(p.xz)-1.12,p.y))-.29;"
    "float ball=length(p)-.64;return min(tor,ball);}\n"
    "vec2 scene(vec3 p){float a=shape(p);float b=p.y+1.55;return "
    "a<b?vec2(a,1):vec2(b,2);}\n"
    "void main(){vec2 uv=(gl_FragCoord.xy-vec2(320,180))/360.;"
    "vec3 ro=vec3(3.5*sin(time*.13),1.2,3.5*cos(time*.13));"
    "vec3 "
    "fw=normalize(-ro),rt=normalize(cross(fw,vec3(0,1,0))),up=cross(rt,fw);"
    "vec3 rd=normalize(fw*1.45+rt*uv.x+up*uv.y);float t=0.;vec2 hit;"
    "for(int "
    "i=0;i<80;i++){hit=scene(ro+rd*t);if(hit.x<.002||t>16.)break;t+=hit.x*.85;}"
    "vec3 c=mix(vec3(.018,.025,.07),vec3(.12,.22,.35),max(rd.y,0.));"
    "if(t<16.){vec3 p=ro+rd*t;vec2 e=vec2(.002,0);"
    "vec3 "
    "n=normalize(vec3(scene(p+e.xyy).x-scene(p-e.xyy).x,scene(p+e.yxy).x-scene("
    "p-e.yxy).x,scene(p+e.yyx).x-scene(p-e.yyx).x));"
    "vec3 l=normalize(vec3(-3,5,4));float dif=max(dot(n,l),0.);"
    "vec3 base=.5+.5*cos(vec3(0,2,4)+p.y*2.+time*.3);"
    "if(hit.y>1.5)base=mix(vec3(.055,.075,.12),vec3(.14,.18,.24),mod(floor(p.x)"
    "+floor(p.z),2.));"
    "float shadow=1.,s=.03;for(int j=0;j<20;j++){float "
    "d=scene(p+l*s).x;shadow=min(shadow,12.*d/s);s+=clamp(d,.03,.4);}"
    "float spec=pow(max(dot(reflect(-l,n),-rd),0.),48.);"
    "c=base*(.15+.85*dif*clamp(shadow,0.,1.))+spec*.7;"
    "c+=vec3(.15,.35,.7)*pow(1.-max(dot(n,-rd),0.),3.)*.6;"
    "c=mix(c,vec3(.018,.025,.07),1.-exp(-t*t*.015));}"
    "uint at=uint(gl_FragCoord.y)*640u+uint(gl_FragCoord.x);vec3 "
    "light=vec3(glow[at].xyz)/255.;c+=light/"
    "(1.+light*.35);color=vec4(pow(max(c,vec3(0)),vec3(1./2.2)),1.);}";
static GLuint shader(GLenum type, const char *text) {
  GLuint s = pCreateShader(type);
  GLint ok;
  char log[4096];
  pShaderSource(s, 1, &text, 0);
  pCompileShader(s);
  pGetShaderiv(s, 0x8B81, &ok);
  if (!ok) {
    pGetShaderInfoLog(s, sizeof(log), 0, log);
    fprintf(stderr, "shader: %s\n", log);
    exit(3);
  }
  return s;
}
@interface DemoView : NSView {
  NSBitmapImageRep *bitmap;
  NSTimer *timer;
  struct timeval start;
  unsigned frames;
  float previous;
  NSString *status;
}
- (void)tick:(NSTimer *)sender;
@end
@implementation DemoView
- (id)initWithFrame:(NSRect)frame {
  self = [super initWithFrame:frame];
  if (!self)
    return nil;
  NSString *path = [[[NSBundle mainBundle] bundlePath]
      stringByAppendingPathComponent:@"Contents/Frameworks/librdngl.dylib"];
  library = dlopen([path fileSystemRepresentation], RTLD_NOW | RTLD_LOCAL);
  if (!library) {
    fprintf(stderr, "%s\n", dlerror());
    exit(1);
  }
  void *(*create)(const int *, void *) =
      dlsym(library, "OSMesaCreateContextAttribs");
  unsigned char (*current)(void *, void *, unsigned, int, int) =
      dlsym(library, "OSMesaMakeCurrent");
  getproc = dlsym(library, "OSMesaGetProcAddress");
  if (!create || !current || !getproc)
    exit(2);
  int attrs[] = {0x22, 1, 0x33, 0x34, 0x36, 4, 0x37, 6, 0};
  context = create(attrs, 0);
  pixels = calloc(W * H, 4);
  if (!context || !pixels ||
      !current(context, pixels, GL_UNSIGNED_BYTE, W, H)) {
    fprintf(stderr, "OpenGL 4.6 context unavailable\n");
    exit(2);
  }
#define LOAD(name)                                                             \
  do {                                                                         \
    *(void **)(&p##name) = getproc("gl" #name);                                \
    if (!p##name)                                                              \
      exit(2);                                                                 \
  } while (0)

  LOAD(GenBuffers);
  LOAD(BindBuffer);
  LOAD(BufferData);
  LOAD(BindBufferBase);
  LOAD(DispatchCompute);
  LOAD(MemoryBarrier);
  LOAD(Uniform1i);
  LOAD(CreateShader);
  LOAD(ShaderSource);
  LOAD(CompileShader);
  LOAD(GetShaderiv);
  LOAD(GetShaderInfoLog);
  LOAD(CreateProgram);
  LOAD(AttachShader);
  LOAD(LinkProgram);
  LOAD(GetProgramiv);
  LOAD(GetProgramInfoLog);
  LOAD(UseProgram);
  LOAD(GenVertexArrays);
  LOAD(BindVertexArray);
  LOAD(GetUniformLocation);
  LOAD(Uniform1f);
  LOAD(Viewport);
  LOAD(DrawArrays);
  LOAD(Finish);
  LOAD(GetError);
  LOAD(GetString);
  const char *renderer = (const char *)pGetString(GL_RENDERER),
             *version = (const char *)pGetString(GL_VERSION);
  fprintf(stderr, "renderer=%s\nversion=%s\nGLSL=%s\n", renderer, version,
          pGetString(0x8B8C));
  if (!renderer || !version || !strstr(renderer, "AMD") ||
      strncmp(version, "4.6", 3)) {
    fprintf(stderr, "Hardware OpenGL 4.6 required\n");
    exit(2);
  }
  program = pCreateProgram();
  pAttachShader(program, shader(0x8B31, vertex));
  pAttachShader(program, shader(0x8B30, fragment));
  pLinkProgram(program);
  GLint ok;
  pGetProgramiv(program, 0x8B82, &ok);
  if (!ok) {
    char log[4096];
    pGetProgramInfoLog(program, sizeof(log), 0, log);
    fprintf(stderr, "link: %s\n", log);
    exit(3);
  }
  computeProgram = pCreateProgram();
  pAttachShader(computeProgram, shader(0x91B9, computeSource));
  pLinkProgram(computeProgram);
  pGetProgramiv(computeProgram, 0x8B82, &ok);
  if (!ok) {
    char log[4096];
    pGetProgramInfoLog(computeProgram, sizeof(log), 0, log);
    fprintf(stderr, "compute link: %s\n", log);
    exit(3);
  }
  clearProgram = pCreateProgram();
  pAttachShader(clearProgram, shader(0x91B9, clearSource));
  pLinkProgram(clearProgram);
  pGetProgramiv(clearProgram, 0x8B82, &ok);
  if (!ok)
    exit(3);
  computeTime = pGetUniformLocation(computeProgram, "time");
  computeDelta = pGetUniformLocation(computeProgram, "dt");
  computeInit = pGetUniformLocation(computeProgram, "initialize");
  pGenBuffers(1, &particleBuffer);
  pBindBuffer(0x90D2, particleBuffer);
  pBufferData(0x90D2, PARTICLES * 8 * sizeof(float), NULL, GL_DYNAMIC_DRAW);
  pBindBufferBase(0x90D2, 0, particleBuffer);
  pGenBuffers(1, &imageBuffer);
  pBindBuffer(0x90D2, imageBuffer);
  pBufferData(0x90D2, W * H * 16, NULL, GL_DYNAMIC_DRAW);
  pBindBufferBase(0x90D2, 1, imageBuffer);
  fprintf(stderr,
          "compute shader + SSBO + GPU atomic accumulation: %d GPU particles\n",
          PARTICLES);
  pUseProgram(program);
  timeUniform = pGetUniformLocation(program, "time");
  pGenVertexArrays(1, &vao);
  pBindVertexArray(vao);
  pViewport(0, 0, W, H);
  bitmap =
      [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL
                                              pixelsWide:W
                                              pixelsHigh:H
                                           bitsPerSample:8
                                         samplesPerPixel:4
                                                hasAlpha:YES
                                                isPlanar:NO
                                          colorSpaceName:NSDeviceRGBColorSpace
                                             bytesPerRow:W * 4
                                            bitsPerPixel:32];
  status = [[NSString
      stringWithFormat:@"%s  |  OpenGL 4.6 Core / GLSL 4.60", renderer] retain];
  gettimeofday(&start, 0);
  timer = [NSTimer scheduledTimerWithTimeInterval:1. / 30.
                                           target:self
                                         selector:@selector(tick:)
                                         userInfo:nil
                                          repeats:YES];
  return self;
}
- (void)tick:(NSTimer *)sender {
  struct timeval now;
  gettimeofday(&now, 0);
  float t = now.tv_sec - start.tv_sec + (now.tv_usec - start.tv_usec) * 1e-6;
  pUseProgram(clearProgram);
  pDispatchCompute(W * H / 128, 1, 1);
  pMemoryBarrier(0x2000);
  pUseProgram(computeProgram);
  pUniform1f(computeTime, t);
  pUniform1f(computeDelta, frames ? fminf(t - previous, .035f) : 0);
  pUniform1i(computeInit, frames == 0);
  pDispatchCompute(PARTICLES / 128, 1, 1);
  pMemoryBarrier(0x2000);
  previous = t;
  pUseProgram(program);
  pUniform1f(timeUniform, t);
  pDrawArrays(GL_TRIANGLES, 0, 3);
  pFinish();
  GLenum error = pGetError();
  if (error) {
    fprintf(stderr, "frame error: %x\n", error);
    [timer invalidate];
    return;
  }
  unsigned y;
  unsigned char *dst = [bitmap bitmapData];
  for (y = 0; y < H; y++) {
    unsigned x;
    const unsigned char *src = pixels + (H - 1 - y) * W * 4;
    for (x = 0; x < W; x++) {
      unsigned char *d = dst + (y * W + x) * 4;
      d[0] = src[x * 4 + 1];
      d[1] = src[x * 4 + 2];
      d[2] = src[x * 4 + 3];
      d[3] = src[x * 4];
    }
  }
  if (!(frames++ % 120))
    fprintf(stderr, "frame=%u seconds=%.2f GL_NO_ERROR\n", frames, t);
  [self setNeedsDisplay:YES];
}
- (void)drawRect:(NSRect)rect {
  [[NSColor colorWithCalibratedRed:.025 green:.035 blue:.065 alpha:1] set];
  NSRectFill([self bounds]);
  [bitmap drawInRect:NSMakeRect(0, 36, W, H)];
  NSDictionary *a =
      [NSDictionary dictionaryWithObjectsAndKeys:[NSColor whiteColor],
                                                 NSForegroundColorAttributeName,
                                                 [NSFont systemFontOfSize:11],
                                                 NSFontAttributeName, nil];
  [status drawAtPoint:NSMakePoint(12, 18) withAttributes:a];
  [@"8,192 GPU-simulated particles | Compute shaders + SSBOs + GPU atomics"
         drawAtPoint:NSMakePoint(12, 3)
      withAttributes:a];
}
@end
@interface AppDelegate : NSObject
@end
@implementation AppDelegate
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app {
  return YES;
}
@end
int main(void) {
  NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
  freopen("/Users/shay/RadeonNI-diagnostics/gl46demo.log", "w", stderr);
  setbuf(stderr, NULL);
  [NSApplication sharedApplication];
  AppDelegate *delegate = [[AppDelegate alloc] init];
  [NSApp setDelegate:delegate];
  NSWindow *w = [[NSWindow alloc]
      initWithContentRect:NSMakeRect(260, 260, W, H + 36)
                styleMask:NSTitledWindowMask | NSClosableWindowMask |
                          NSMiniaturizableWindowMask
                  backing:NSBackingStoreBuffered
                    defer:NO];
  [w setTitle:@"Radeon • OpenGL 4.6 • Hardware shader demo"];
  DemoView *v = [[DemoView alloc] initWithFrame:NSMakeRect(0, 0, W, H + 36)];
  [w setContentView:v];
  [w makeKeyAndOrderFront:nil];
  [NSApp activateIgnoringOtherApps:YES];
  [NSApp run];
  [pool drain];
  return 0;
}
