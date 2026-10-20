/*
 * winprobe: a program with nothing of QuickTime in it that does what QuickTime Player does to the screen, step by step, while another
 * program watches the video decoder (see docs/UVD.md, "The decoder stalls in QuickTime Player").
 *
 *   winprobe none seconds       connects to the window server (NSApplication), no window
 *   winprobe window seconds     a plain Cocoa window, redrawn five times a second
 *   winprobe gl seconds         a Cocoa window with an OpenGL view, cleared and flushed five times a second
 *
 * Inside a bundle (winprobe.app, started by `open`), with no arguments: a plain window for 300 s, as an application LaunchServices knows (Dock
 * icon, menu bar). Info.plist with LSUIElement set makes it one without a Dock icon and menu bar.
 *
 * Builds on the G5 (its Xcode):
 *   gcc-4.0 -arch ppc -isysroot /Developer/SDKs/MacOSX10.4u.sdk -mmacosx-version-min=10.4 -o winprobe winprobe.m \
 *       -framework Cocoa -framework OpenGL
 *
 * Copyright (c) 2026 kouta-kun and Claude
 * SPDX-License-Identifier: MIT
 */

#import <Cocoa/Cocoa.h>
#import <OpenGL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

@interface ColorView : NSView
{
	float phase;
}
- (void)step;
@end

@implementation ColorView
- (void)step
{
	phase += 0.1f;
	if (phase > 1.0f)
		phase = 0.0f;
	[self setNeedsDisplay:YES];
}
- (void)drawRect:(NSRect)r
{
	[[NSColor colorWithCalibratedRed:phase green:0.4f blue:1.0f - phase alpha:1.0f] set];
	NSRectFill(r);
}
@end

int main(int argc, char **argv)
{
	NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
	const char *mode;
	double seconds;
	int a = 1;

	/* LaunchServices starts a bundle with -psn_... first; no arguments at all means a plain window for 300 s */
	if (a < argc && !strncmp(argv[a], "-psn", 4))
		a++;
	mode = a < argc ? argv[a] : "window";
	seconds = a + 1 < argc ? atof(argv[a + 1]) : 300;
	NSWindow *window = nil;
	ColorView *view = nil;
	NSOpenGLView *glview = nil;
	time_t end;

	[NSApplication sharedApplication];
	printf("winprobe %s %.0f s: connected to the window server\n", mode, seconds);
	fflush(stdout);
	if (!strcmp(mode, "window") || !strcmp(mode, "gl")) {
		window = [[NSWindow alloc] initWithContentRect:NSMakeRect(120, 120, 640, 360)
						     styleMask:NSTitledWindowMask | NSClosableWindowMask
						       backing:NSBackingStoreBuffered
							 defer:NO];
		[window setTitle:[NSString stringWithUTF8String:mode]];
		if (!strcmp(mode, "gl")) {
			NSOpenGLPixelFormatAttribute attrs[] = { NSOpenGLPFAAccelerated, NSOpenGLPFADoubleBuffer, NSOpenGLPFAColorSize,
								 (NSOpenGLPixelFormatAttribute)24, (NSOpenGLPixelFormatAttribute)0 };
			NSOpenGLPixelFormat *pf = [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];

			glview = [[NSOpenGLView alloc] initWithFrame:NSMakeRect(0, 0, 640, 360) pixelFormat:pf];
			[window setContentView:glview];
		} else {
			view = [[ColorView alloc] initWithFrame:NSMakeRect(0, 0, 640, 360)];
			[window setContentView:view];
		}
		[window makeKeyAndOrderFront:nil];
		printf("window is up\n");
		fflush(stdout);
	}
	end = time(NULL) + (time_t)seconds;
	while (time(NULL) < end) {
		NSEvent *e = [NSApp nextEventMatchingMask:NSAnyEventMask
						untilDate:[NSDate dateWithTimeIntervalSinceNow:0.2]
						   inMode:NSDefaultRunLoopMode
						  dequeue:YES];

		if (e)
			[NSApp sendEvent:e];
		if (view)
			[view step];
		if (glview) {
			static float c;

			c += 0.1f;
			if (c > 1.0f)
				c = 0.0f;
			[[glview openGLContext] makeCurrentContext];
			glClearColor(c, 0.4f, 1.0f - c, 1.0f);
			glClear(GL_COLOR_BUFFER_BIT);
			[[glview openGLContext] flushBuffer];
		}
	}
	if (window)
		[window close];
	[pool release];
	printf("winprobe done\n");
	return 0;
}
