#import <Cocoa/Cocoa.h>
#include <OpenGL/gl.h>
#include <IOKit/IOKitLib.h>
#include "rdn_user.h"

static UInt32 registered_size(io_connect_t connection, UInt32 width, UInt32 height)
{
    struct rdn_user_surfaces list;
    size_t size = sizeof(list);
    unsigned i;
#ifdef __LP64__
    if (IOConnectCallStructMethod(connection, RDN_UC_SURFACE_LIST, NULL, 0, &list, &size))
#else
    if (IOConnectMethodScalarIStructureO(connection, RDN_UC_SURFACE_LIST, 0, &size, &list))
#endif
        return 0;
    for (i = 0; i < list.count && i < RDN_USER_SURFACES; ++i)
        if (list.surface[i].width == width && list.surface[i].height == height)
            return list.surface[i].offset;
    return 0;
}

int main(void)
{
    freopen("/tmp/rdn-surfacerebind.log", "w", stdout);
    setbuf(stdout, NULL);
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    NSOpenGLPixelFormatAttribute attributes[] = {
        NSOpenGLPFARendererID, 0x21a00, NSOpenGLPFADoubleBuffer, 0
    };
    io_service_t service;
    io_connect_t connection;
    int failures = 0, i;
    [NSApplication sharedApplication];
    NSOpenGLPixelFormat *format = [[NSOpenGLPixelFormat alloc] initWithAttributes:attributes];
    NSOpenGLView *view = [[NSOpenGLView alloc] initWithFrame:NSMakeRect(0, 0, 128, 96)
                                               pixelFormat:format];
    NSWindow *window = [[NSWindow alloc] initWithContentRect:NSMakeRect(1090, 450, 128, 96)
        styleMask:NSTitledWindowMask backing:NSBackingStoreBuffered defer:NO];
    [window setReleasedWhenClosed:NO];
    [window setTitle:@"Surface rebind test"];
    [window setContentView:view];
    [window orderFront:nil];
    service = IOServiceGetMatchingService(kIOMasterPortDefault,
        IOServiceMatching(RDN_UC_SERVICE_CLASS));
    if (!format || !service || IOServiceOpen(service, mach_task_self(), RDN_UC_TYPE, &connection))
        return 2;
    IOObjectRelease(service);
    for (i = 0; i < 4; ++i) {
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
        [[view openGLContext] makeCurrentContext];
        glClearColor(i & 1, 0, !(i & 1), 1);
        glClear(GL_COLOR_BUFFER_BIT);
        [[view openGLContext] flushBuffer];
        glFinish();
        UInt32 before = registered_size(connection, 128, 96);
        failures += !before;
        [[view openGLContext] clearDrawable];
        [[view openGLContext] setView:view];
        [[view openGLContext] makeCurrentContext];
        glClear(GL_COLOR_BUFFER_BIT);
        [[view openGLContext] flushBuffer];
        glFinish();
        UInt32 after = registered_size(connection, 128, 96);
        printf("rebind %d: 0x%x -> 0x%x\n", i, before, after);
        failures += !after || before != after;
        [window setFrameOrigin:NSMakePoint(1090 + i, 450)];
        [[view openGLContext] update];
        failures += !registered_size(connection, 128, 96);
    }
    NSView *otherView = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 160, 100)];
    NSWindow *otherWindow = [[NSWindow alloc] initWithContentRect:NSMakeRect(900, 450, 160, 100)
        styleMask:NSTitledWindowMask backing:NSBackingStoreBuffered defer:NO];
    [otherWindow setReleasedWhenClosed:NO];
    [otherWindow setContentView:otherView];
    [otherWindow orderFront:nil];
    NSOpenGLContext *context = [view openGLContext];
    UInt32 original = registered_size(connection, 128, 96);
    [context clearDrawable];
    [context setView:otherView];
    [context makeCurrentContext];
    glClearColor(0, 1, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    [context flushBuffer];
    glFinish();
    UInt32 retained = registered_size(connection, 128, 96);
    printf("same context, different window: 0x%x -> 0x%x\n", original, retained);
    failures += !original || original != retained;
    [context clearDrawable];
    [context setView:view];
    [context makeCurrentContext];
    glFinish();
    failures += registered_size(connection, 128, 96) != original;
    [otherWindow close];
    [otherView release];
    [otherWindow release];
    for (i = 0; i < 40; ++i) {
        NSView *transientView = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 160, 100)];
        NSWindow *transientWindow = [[NSWindow alloc] initWithContentRect:NSMakeRect(900, 450, 160, 100)
            styleMask:NSTitledWindowMask backing:NSBackingStoreBuffered defer:NO];
        [transientWindow setReleasedWhenClosed:NO];
        [transientWindow setContentView:transientView];
        [transientWindow orderBack:nil];
        [context clearDrawable];
        [context setView:transientView];
        [context makeCurrentContext];
        glClear(GL_COLOR_BUFFER_BIT);
        [context flushBuffer];
        glFinish();
        failures += !registered_size(connection, 160, 100);
        [context clearDrawable];
        [transientWindow close];
        [transientView release];
        [transientWindow release];
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
    }
    [context setView:view];
    [context makeCurrentContext];
    glFinish();
    failures += registered_size(connection, 128, 96) != original;
    printf("40 transient surfaces closed; original surface retained\n");
    printf("surface registration across rebinds: %d failed checks\n", failures);
    [NSOpenGLContext clearCurrentContext];
    [window close];
    [view release];
    [window release];
    [format release];
    IOServiceClose(connection);
    [pool drain];
    return failures != 0;
}
