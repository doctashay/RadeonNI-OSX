/*
 * glclear: make an accelerated OpenGL context (CGL, no window) and clear its buffer and wait for the card, over and over
 * for a number of seconds. Something for the card's 3D engine to do while another program uses its video decoder.
 *
 *   glclear [seconds]
 *
 * Builds on the G5 (its Xcode):
 *   gcc-4.0 -arch ppc -isysroot /Developer/SDKs/MacOSX10.4u.sdk -mmacosx-version-min=10.4 -o glclear glclear.c -framework OpenGL
 *
 * Copyright (c) 2026 kouta-kun and Claude
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>

int main(int argc, char **argv)
{
	double seconds = argc > 1 ? atof(argv[1]) : 3;
	CGLPixelFormatAttribute attrs[] = { kCGLPFAAccelerated, kCGLPFAColorSize, (CGLPixelFormatAttribute)24, (CGLPixelFormatAttribute)0 };
	CGLPixelFormatObj pix = NULL;
	CGLContextObj ctx = NULL;
	GLint npix = 0;
	struct timeval t0, t;
	unsigned n = 0;

	if (CGLChoosePixelFormat(attrs, &pix, &npix) || !pix || CGLCreateContext(pix, NULL, &ctx) || !ctx) {
		fprintf(stderr, "no accelerated context\n");
		return 1;
	}
	CGLSetCurrentContext(ctx);
	gettimeofday(&t0, NULL);
	do {
		glClearColor((n % 7) / 7.0f, 0.4f, 0.6f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		glFinish();
		n++;
		gettimeofday(&t, NULL);
	} while ((t.tv_sec - t0.tv_sec) + (t.tv_usec - t0.tv_usec) / 1e6 < seconds);
	printf("%u clears on %s\n", n, (const char *)glGetString(GL_RENDERER));
	return 0;
}
