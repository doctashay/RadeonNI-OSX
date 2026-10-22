/*
 * glctx_scan: where, inside the OpenGL engine's context (CGLContextObj->rend),
 * are the two things docs/GLD-INTERFACE.md says our bundle must find: the
 * pointer back to the application's dispatch table, and the driver's own
 * function table? And how long is the application's dispatch table?
 *
 *   gcc -isysroot /Developer/SDKs/MacOSX10.4u.sdk -arch ppc -o glctx_scan \
 *       glctx_scan.c -framework OpenGL
 *   ./glctx_scan [renderer-id]      (default 0x20400, the software renderer)
 *
 * Creates an ordinary CGL context (software renderer if no accelerator is
 * announced), then reads the engine's context with vm_read_overwrite so that
 * nothing faults, and prints:
 *   - the offsets in rend that hold &ctx->disp and ctx itself;
 *   - runs of words that point into one image's text, with the image's
 *     name (the driver table is a run of pointers into the renderer);
 *   - how many entries of ctx->disp point into the engine's text.
 *
 * On 10.4.11 the answers are known (0x4680, 0x4698, 684 entries); on 10.5.9
 * they are what this finds. Copyright (c) 2026 kouta-kun and Claude, MIT.
 */
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/CGLContext.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc/malloc.h>
#include <string.h>

#define SCAN_BYTES 0x30000

static int readable(const void *p, unsigned long n, void *buf)
{
	vm_size_t got = 0;

	return vm_read_overwrite(mach_task_self(), (vm_address_t)p, n,
				 (vm_address_t)buf, &got) == KERN_SUCCESS && got == n;
}

static const char *image_of(unsigned long p, unsigned long *off)
{
	Dl_info di;

	if (p < 0x1000 || !dladdr((void *)p, &di) || !di.dli_fname)
		return NULL;
	if (off)
		*off = p - (unsigned long)di.dli_fbase;
	return di.dli_fname;
}

static const char *base(const char *s)
{
	const char *b = strrchr(s, '/');

	return b ? b + 1 : s;
}

int main(int argc, char **argv)
{
	long renderer = argc > 1 ? strtol(argv[1], NULL, 0) : 0x20400;
	CGLPixelFormatAttribute attrs[] = {
		kCGLPFAOffScreen, kCGLPFAColorSize, 32, kCGLPFARendererID,
		(CGLPixelFormatAttribute)renderer, 0
	};
	CGLPixelFormatObj pf;
	CGLContextObj ctx;
	GLint n;
	unsigned long *mem;
	unsigned long disp, rend, i, j;
	char *buf;
	int nread;

	if (CGLChoosePixelFormat(attrs, &pf, &n) || !pf) {
		printf("no pixel format\n");
		return 1;
	}
	if (CGLCreateContext(pf, NULL, &ctx) || !ctx) {
		printf("no context\n");
		return 1;
	}
	CGLSetCurrentContext(ctx);
	rend = (unsigned long)ctx->rend;
	disp = (unsigned long)&ctx->disp;
	printf("ctx %p  rend %#lx  &ctx->disp %#lx\n", (void *)ctx, rend, disp);

	buf = malloc(SCAN_BYTES);
	/* read as much of rend as is mapped, in 4 KiB steps */
	for (nread = 0; nread < SCAN_BYTES; nread += 4096)
		if (!readable((void *)(rend + nread), 4096, buf + nread))
			break;
	printf("engine context: %#x bytes readable from rend\n", nread);
	mem = (unsigned long *)buf;

	puts("-- words equal to &ctx->disp or ctx:");
	for (i = 0; i < nread / sizeof(*mem); i++)
		if (mem[i] == disp || mem[i] == (unsigned long)ctx)
			printf("  rend+%#05lx = %s\n", i * sizeof(*mem), mem[i] == disp ? "&ctx->disp" : "ctx");

	puts("-- runs (>= 6) of words that point into one image:");
	for (i = 0; i < nread / sizeof(*mem);) {
		const char *im = image_of(mem[i], NULL);

		if (!im) {
			i++;
			continue;
		}
		for (j = i + 1; j < nread / sizeof(*mem); j++) {
			const char *jm = image_of(mem[j], NULL);

			if (!jm || strcmp(jm, im))
				break;
		}
		if (j - i >= 6)
			printf("  rend+%#05lx .. +%#05lx  %3lu words  %s\n", i * sizeof(*mem), (j - 1) * sizeof(*mem), j - i, base(im));
		i = j;
	}

	puts("-- ctx->disp (the application's table):");
	{
		unsigned long *t = (unsigned long *)disp, w;
		const char *first = NULL;
		unsigned long k, in_engine = 0;

		for (k = 0; k < 900; k++) {
			unsigned long v;

			if (!readable(&t[k], sizeof(*t), &v))
				break;
			w = v;
			if (k == 0)
				first = image_of(w, NULL);
			if (first && image_of(w, NULL) && !strcmp(image_of(w, NULL), first))
				in_engine++;
			else if (w)
				break;
		}
		printf("  %lu consecutive entries point into %s; entry %lu = %#lx (%s)\n",
		       in_engine, first ? base(first) : "?", k, k < 900 ? t[k] : 0,
		       k < 900 && image_of(t[k], NULL) ? base(image_of(t[k], NULL)) : "not code");
	}
	puts("-- ctx words after &ctx->disp as runs of one kind (E engine, R other image, 0 zero, ? other):");
	{
		unsigned long *t = (unsigned long *)disp, v, k, start = 0;
		char kind, cur = 0;
		const char *im, *curim = NULL;

		for (k = 0; k <= 800; k++) {
			if (k == 800 || !readable(&t[k], sizeof(*t), &v)) {
				kind = 'X';
				v = 0;
			} else {
				im = image_of(v, NULL);
				kind = v == 0 ? '0' : im ? (strstr(im, "GLEngine") ? 'E' : 'R') : '?';
			}
			if (kind != cur || (kind == 'R' && 0)) {
				if (cur)
					printf("  entries %3lu..%3lu  %c  (%lu)%s%s\n", start, k - 1, cur, k - start,
					       curim ? "  " : "", curim ? base(curim) : "");
				cur = kind;
				start = k;
				curim = kind == 'R' && k < 800 ? image_of(v, NULL) : NULL;
			}
			if (kind == 'X')
				break;
		}
		if (readable(&t[0], sizeof(*t), &v)) {
			unsigned long here;

			for (k = 600; k < 800; k++)
				if (readable(&t[k], sizeof(*t), &here) && here && !image_of(here, NULL) && here > 0x1000)
					{ printf("  first non-code, non-zero word >= entry 600: entry %lu = %#lx\n", k, here); break; }
		}
		printf("  malloc size of ctx: %lu bytes\n", (unsigned long)malloc_size(ctx));
	}
	CGLSetCurrentContext(NULL);
	CGLDestroyContext(ctx);
	return 0;
}
