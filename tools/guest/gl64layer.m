/* Exercise CAOpenGLLayer's drawable handoff on Leopard. */
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <stdio.h>
extern CGLError CGLGetSurface(CGLContextObj, int *, int *, int *);
@interface TestLayer : CAOpenGLLayer
@end
@implementation TestLayer
- (void)drawInCGLContext:(CGLContextObj)c pixelFormat:(CGLPixelFormatObj)p
    forLayerTime:(CFTimeInterval)t displayTime:(const CVTimeStamp *)stamp
{
    CGLSetCurrentContext(c);
    CGLPBufferObj pb = NULL;
    GLenum face = 0; GLint level = 0, screen = 0, fbo = 0, vp[4] = {0};
    CGLError err = CGLGetPBuffer(c, &pb, &face, &level, &screen);
    int cid = 0, wid = 0, sid = 0;
    CGLGetSurface(c, &cid, &wid, &sid);
    fprintf(stderr, "surface cid=%x wid=%x sid=%x\n", cid, wid, sid);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &fbo);
    glGetIntegerv(GL_VIEWPORT, vp);
    fprintf(stderr, "ctx=%p pb=%p error=%d fbo=%d viewport=%d,%d,%d,%d renderer=%s\n",
        c,pb,err,fbo,vp[0],vp[1],vp[2],vp[3],glGetString(GL_RENDERER));
    unsigned int texels[32 * 32];
    for (unsigned i = 0; i < 32 * 32; ++i) texels[i] = 0xff00ff00;
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_RECTANGLE_ARB, texture);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB, 0, GL_RGBA, 32, 32, 0,
                 GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, texels);
    glEnable(GL_TEXTURE_RECTANGLE_ARB);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0,320,240,0,-1,1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity(); glColor4f(1,1,1,1);
    glBegin(GL_QUADS);
    glTexCoord2i(0,0); glVertex2i(0,0);
    glTexCoord2i(32,0); glVertex2i(320,0);
    glTexCoord2i(32,32); glVertex2i(320,240);
    glTexCoord2i(0,32); glVertex2i(0,240);
    glEnd();
    glDisable(GL_TEXTURE_RECTANGLE_ARB);
    glDeleteTextures(1, &texture);
    unsigned char pixel[4] = {0};
    glReadPixels(160,120,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
    fprintf(stderr,"pixel=%u,%u,%u,%u error=%x: %s\n",pixel[0],pixel[1],pixel[2],pixel[3],glGetError(),
        pb && vp[2]==320 && vp[3]==240 && pixel[0]==0 && pixel[1]==255 && pixel[2]==0 && pixel[3]==255 ? "PASS" : "FAIL");
    [super drawInCGLContext:c pixelFormat:p forLayerTime:t displayTime:stamp];
}
@end
int main(void)
{
    freopen("/tmp/gl64layer.log", "w", stderr); setbuf(stderr,NULL);
    NSAutoreleasePool *pool=[[NSAutoreleasePool alloc] init];
    [NSApplication sharedApplication];
    NSRect r=NSMakeRect(150,150,320,240);
    NSWindow *w=[[NSWindow alloc] initWithContentRect:r styleMask:NSTitledWindowMask|NSClosableWindowMask backing:NSBackingStoreBuffered defer:NO];
    [w setTitle:@"PPC64 CAOpenGLLayer: green expected"];
    NSView *view=[w contentView];
    CALayer *root=[CALayer layer];
    [view setLayer:root]; [view setWantsLayer:YES];
    TestLayer *layer=[TestLayer layer]; [layer setFrame:CGRectMake(0,0,320,240)];
    [root addSublayer:layer]; [layer setNeedsDisplay];
    [w makeKeyAndOrderFront:nil]; [NSApp activateIgnoringOtherApps:YES];
    [NSApp run]; [pool release]; return 0;
}
