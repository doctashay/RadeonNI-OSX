/* Native window presentation smoke test; build with -arch ppc64 and Cocoa/OpenGL. */
#import <Cocoa/Cocoa.h>
#include <OpenGL/gl.h>

@interface RenderView : NSOpenGLView
@end
@implementation RenderView
- (void)drawRect:(NSRect)rect
{
    [[self openGLContext] makeCurrentContext];
    glViewport(0, 0, 320, 240);
    glClearColor(0, 1, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    [[self openGLContext] flushBuffer];
}
@end

int main(void)
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    [NSApplication sharedApplication];
    NSOpenGLPixelFormatAttribute attrs[] = {
        NSOpenGLPFAAccelerated, NSOpenGLPFANoRecovery,
        NSOpenGLPFADoubleBuffer, NSOpenGLPFAColorSize, 24, 0
    };
    NSOpenGLPixelFormat *format = [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];
    if (!format) return 1;
    NSRect frame = NSMakeRect(100, 100, 320, 240);
    NSWindow *window = [[NSWindow alloc] initWithContentRect:frame
        styleMask:NSTitledWindowMask | NSClosableWindowMask
        backing:NSBackingStoreBuffered defer:NO];
    RenderView *view = [[RenderView alloc] initWithFrame:frame pixelFormat:format];
    [window setTitle:@"PPC64 OpenGL: green window expected"];
    [window setContentView:view];
    [window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    [NSApp run];
    [pool release];
    return 0;
}
