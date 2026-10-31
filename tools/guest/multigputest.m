/* Render and read back independent windows on every attached display. */
#import <Cocoa/Cocoa.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <string.h>

int main(int argc, char **argv)
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    int modern = argc > 2;
    int (*make_current)(void *) = NULL;
    void *(*lookup)(const char *) = NULL;
    if (modern) {
        void *driver = dlopen("/System/Library/Extensions/RadeonNIGLDriver.bundle/Contents/MacOS/RadeonNIGLDriver", RTLD_NOW | RTLD_LOCAL);
        int (*request)(int, int) = driver ? dlsym(driver, "RDNGLRequestCompatibilityContext") : NULL;
        make_current = driver ? dlsym(driver, "RDNGLMakeCurrent") : NULL;
        lookup = driver ? dlsym(driver, "RDNGLGetProcAddress") : NULL;
        if (!request || !make_current || !lookup || !request(4, 6)) return 2;
    }
    [NSApplication sharedApplication];
    if (argc > 1) freopen(argv[1], "w", stdout);
    setbuf(stdout, NULL);
    NSArray *screens = [NSScreen screens];
    NSWindow *windows[8] = {0};
    NSOpenGLView *views[8] = {0};
    unsigned count = MIN([screens count], 8), i, frames = 0;
    int failed = count < 2;
    GLint renderers[8] = {0};
    double start;
    printf("displays=%u\n", count);
    for (i = 0; i < count; ++i) {
        NSScreen *screen = [screens objectAtIndex:i];
        CGDirectDisplayID display = [[[screen deviceDescription]
            objectForKey:@"NSScreenNumber"] unsignedIntValue];
        GLuint mask = CGDisplayIDToOpenGLDisplayMask(display);
        NSOpenGLPixelFormatAttribute attrs[] = {NSOpenGLPFAAccelerated,
            NSOpenGLPFANoRecovery, NSOpenGLPFADoubleBuffer,
            NSOpenGLPFAScreenMask, mask, 0};
        NSOpenGLPixelFormat *format = [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];
        if (!format) { failed = 1; break; }
        NSRect frame = [screen visibleFrame];
        views[i] = [[NSOpenGLView alloc] initWithFrame:NSMakeRect(0, 0, 640, 360)
            pixelFormat:format];
        [format release];
        windows[i] = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 640, 360)
            styleMask:NSTitledWindowMask backing:NSBackingStoreBuffered defer:NO screen:screen];
        [windows[i] setReleasedWhenClosed:NO];
        [windows[i] setContentView:views[i]];
        [windows[i] setFrameOrigin:NSMakePoint(frame.origin.x + 80, frame.origin.y + 180)];
        [windows[i] setTitle:[NSString stringWithFormat:@"GPU display %u: mask %x", i, mask]];
        [windows[i] orderFront:nil];
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.2]];
        NSOpenGLContext *ctx = [views[i] openGLContext];
        [ctx makeCurrentContext];
        if (modern) {
            if (!make_current([ctx CGLContextObj])) {
                printf("FAIL: display=%u modern make-current\n", i);
                failed = 1; break;
            }
            const GLubyte *(*get)(GLenum) = lookup("glGetString");
            const char *version = get ? (const char *)get(GL_VERSION) : NULL;
            printf("display=%u modern=%s\n", i, version ? version : "missing");
            if (!version || strncmp(version, "4.6", 3)) { failed = 1; break; }
        }
        GLint renderer = 0, swap = 0;
        CGLGetParameter([ctx CGLContextObj], kCGLCPCurrentRendererID, &renderer);
        [ctx setValues:&swap forParameter:NSOpenGLCPSwapInterval];
        printf("display=%u mask=%x renderer=%x GPU=%s origin=%.0f,%.0f\n",
            display, mask, renderer, glGetString(GL_RENDERER), frame.origin.x, frame.origin.y);
        printf("vendor=%s version=%s glsl=%s\n", glGetString(GL_VENDOR),
            glGetString(GL_VERSION), glGetString(GL_SHADING_LANGUAGE_VERSION));
        if ((renderer & 0xff00) < 0x1a00 || (renderer & 0xff00) > 0x1d00)
            failed = 1;
        renderers[i] = renderer;
        char path[256], suffix[8] = "";
        unsigned index = ((renderer & 0xff00) - 0x1a00) / 0x100;
        if (index) snprintf(suffix, sizeof(suffix), "%u", index);
        snprintf(path, sizeof(path), "/System/Library/Extensions/RadeonNIGLDriver%s.bundle/Contents/MacOS/RadeonNIGLDriver%s", suffix, suffix);
        void *handle = dlopen(path, RTLD_LAZY | RTLD_NOLOAD);
        unsigned (*device_id)(void) = handle ? dlsym(handle, "RDNGLGetDeviceID") : NULL;
        printf("display=%u device=%04x\n", i, device_id ? device_id() : 0);
        if (!device_id || !device_id()) failed = 1;
        if (modern) {
            int (*begin)(const void *) = handle ? dlsym(handle, "RDNGLBeginVideoFrameV1") : NULL;
            int (*poll)(const void *) = handle ? dlsym(handle, "RDNGLPollVideoFrameV1") : NULL;
            int (*cancel)(const void *) = handle ? dlsym(handle, "RDNGLCancelVideoFrameV1") : NULL;
            int valid = begin && poll && cancel && begin(NULL) == 2 &&
                        poll(NULL) == 3 && cancel(NULL) == 3;
            printf("display=%u async video exports=%s\n", i, valid ? "PASS" : "FAIL");
            if (!valid) failed = 1;
        }
        if (handle) dlclose(handle);
    }
    [NSApp activateIgnoringOtherApps:YES];
    if (count >= 2 && renderers[0] && renderers[0] == renderers[1]) {
        printf("FAIL: both test displays selected the same renderer\n");
        failed = 1;
    }
    start = [NSDate timeIntervalSinceReferenceDate];
    while (!failed && [NSDate timeIntervalSinceReferenceDate] - start < 20) {
        for (i = 0; i < count; ++i) {
            NSOpenGLContext *ctx = [views[i] openGLContext];
            [ctx makeCurrentContext];
            glViewport(0, 0, 640, 360);
            glDisable(GL_DITHER);
            glClearColor(i ? 0.8 : 0.2, 0.1, i ? 0.2 : 0.8, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            glMatrixMode(GL_PROJECTION); glLoadIdentity();
            glMatrixMode(GL_MODELVIEW); glLoadIdentity();
            glColor3f(0.1, 0.9, 0.3);
            glBegin(GL_TRIANGLES);
            glVertex2f(-0.8, -0.8); glVertex2f(0.8, -0.8); glVertex2f(0, 0.8);
            glEnd();
            [ctx flushBuffer];
        }
        ++frames;
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
    }
    for (i = 0; i < count; ++i) {
        if (!views[i]) continue;
        NSOpenGLContext *ctx = [views[i] openGLContext];
        [ctx makeCurrentContext];
        unsigned char center[4] = {0}, corner[4] = {0};
        glFinish(); glReadBuffer(GL_FRONT);
        glReadPixels(320, 180, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, center);
        glReadPixels(4, 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, corner);
        GLenum error = glGetError();
        int correct = !error && abs(center[0] - 26) <= 2 &&
            abs(center[1] - 230) <= 2 && abs(center[2] - 77) <= 2 &&
            abs(corner[0] - (i ? 204 : 51)) <= 2 &&
            abs(corner[2] - (i ? 51 : 204)) <= 2;
        failed |= !correct;
        printf("display=%u center=%u,%u,%u corner=%u,%u,%u error=%u correct=%d\n",
            i, center[0], center[1], center[2], corner[0], corner[1], corner[2], error, correct);
        [ctx clearDrawable]; [windows[i] close]; [views[i] release]; [windows[i] release];
    }
    printf("%s frames=%u elapsed=%.3f\n", failed ? "FAIL" : "PASS", frames,
        [NSDate timeIntervalSinceReferenceDate] - start);
    [pool release];
    return failed;
}
