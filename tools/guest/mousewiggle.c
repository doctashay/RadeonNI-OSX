/*
 * mousewiggle: move the pointer back and forth for a number of seconds (nothing is clicked). Makes the window server call
 * the framebuffer driver's cursor entry points.
 *
 *   mousewiggle [seconds]
 *
 * Builds on the G5:
 *   gcc-4.0 -arch ppc -isysroot /Developer/SDKs/MacOSX10.4u.sdk -mmacosx-version-min=10.4 -o mousewiggle mousewiggle.c \
 *       -framework ApplicationServices
 *
 * Copyright (c) 2026 kouta-kun and Claude
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <ApplicationServices/ApplicationServices.h>

int main(int argc, char **argv)
{
	double seconds = argc > 1 ? atof(argv[1]) : 3;
	int i, n = (int)(seconds * 50);
	CGEventRef e = CGEventCreate(NULL);
	CGPoint home = CGEventGetLocation(e);

	CFRelease(e);
	for (i = 0; i < n; i++) {
		CGPoint p = home;

		p.x += (i % 40) * 4;
		p.y += (i % 40) * 2;
		CGWarpMouseCursorPosition(p);
		usleep(20000);
	}
	CGWarpMouseCursorPosition(home);
	printf("moved the pointer %d times\n", n);
	return 0;
}
