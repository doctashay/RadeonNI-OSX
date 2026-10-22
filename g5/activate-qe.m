#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>

int main(void)
{
	NSAutoreleasePool *pool = [NSAutoreleasePool new];
	NSWindow *window;
	CALayer *layer;
	int i;

	[NSApplication sharedApplication];
	window = [[NSWindow alloc]
		initWithContentRect:NSMakeRect(0, 0, 32, 32)
		styleMask:NSBorderlessWindowMask
		backing:NSBackingStoreBuffered defer:NO];
	[window setReleasedWhenClosed:NO];
	[[window contentView] setWantsLayer:YES];
	layer = [[window contentView] layer];
	[window orderFront:nil];
	for (i = 0; i < 20; i++) {
		NSEvent *event;

		[layer setOpacity:(i & 1) ? 0.9f : 1.0f];
		event = [NSApp nextEventMatchingMask:NSAnyEventMask
			untilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]
			inMode:NSDefaultRunLoopMode dequeue:YES];
		if (event)
			[NSApp sendEvent:event];
	}
	[window close];
	[window release];
	(void)pool;
	return 0;
}
