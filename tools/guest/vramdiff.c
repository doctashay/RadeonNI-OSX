/*
 * vramdiff: find who else writes into the card's video memory. It maps the aperture, copies everything above the screen (16 MB up), starts an
 * application the way a user would (background launch, then activate, then wait for it to quit), and after each step says which 4 KB pages
 * of video memory changed and which of them lie in the video decoder's own memory (its firmware window, ring and scratch page). Nothing is written to the card.
 *
 *   vramdiff app.app          (the application must quit by itself, e.g. winprobe.app, whose window lasts 40 s)
 *
 * Builds on the G5 (IOKit only), with hw/rdn_user.h next to it:
 *   gcc-4.0 -arch ppc -isysroot /Developer/SDKs/MacOSX10.4u.sdk -mmacosx-version-min=10.4 -I. -o vramdiff vramdiff.c -framework IOKit -framework CoreFoundation
 *
 * Copyright (c) 2026 kouta-kun and Claude
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <mach/mach.h>
#include <IOKit/IOKitLib.h>

#include "rdn_user.h"

#define FROM		(16u << 20)
#define PAGE		4096u

static volatile uint32_t *aperture;
static size_t span;
static int up, vcpu, ring;

static void snapshot(uint32_t *dst)
{
	size_t i, words = span / 4;

	for (i = 0; i < words; i++)
		dst[i] = aperture[FROM / 4 + i];
}

/* In the decoder's own memory: its firmware window and ring (two ranges from RDN_UC_UVD_INFO) and the page after the ring */
static int in_uvd(uint32_t off)
{
	if (!up)
		return 0;
	return off >= (uint32_t)vcpu && off < (uint32_t)ring + 4096 + 0x20000;
}

static void diff(const uint32_t *a, const uint32_t *b, const char *what)
{
	size_t pages = span / PAGE, p, w, shown = 0, changed = 0, in = 0;
	size_t first = 0, last = 0;
	int open = 0;

	printf("-- %s --\n", what);
	for (p = 0; p <= pages; p++) {
		int differs = 0;

		if (p < pages)
			for (w = 0; w < PAGE / 4 && !differs; w++)
				differs = a[p * PAGE / 4 + w] != b[p * PAGE / 4 + w];
		if (differs) {
			changed++;
			if (in_uvd(FROM + (uint32_t)(p * PAGE)))
				in++;
			if (!open) {
				open = 1;
				first = p;
			}
			last = p;
		} else if (open) {
			if (shown++ < 40) {
				uint32_t off = FROM + (uint32_t)(first * PAGE);

				printf("   0x%08x-0x%08x (%lu KB)%s  now: %08x %08x %08x %08x\n", off, FROM + (uint32_t)((last + 1) * PAGE) - 1,
				       (unsigned long)((last - first + 1) * 4), in_uvd(off) ? "  IN THE DECODER'S MEMORY" : "",
				       (unsigned)b[first * PAGE / 4], (unsigned)b[first * PAGE / 4 + 1], (unsigned)b[first * PAGE / 4 + 2],
				       (unsigned)b[first * PAGE / 4 + 3]);
			}
			open = 0;
		}
	}
	printf("   %lu pages changed, %lu of them in the decoder's memory\n", (unsigned long)changed, (unsigned long)in);
	fflush(stdout);
}

int main(int argc, char **argv)
{
	io_service_t service = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching(RDN_UC_SERVICE_CLASS));
	io_connect_t conn;
	vm_address_t addr = 0;
	vm_size_t len = 0;
	uint32_t *a, *b, *tmp;
	char cmd[512];
	int n;

	if (argc < 2) {
		fprintf(stderr, "usage: vramdiff /path/to/app.app\n");
		return 1;
	}
	if (!service || IOServiceOpen(service, mach_task_self(), RDN_UC_TYPE, &conn)) {
		fprintf(stderr, "cannot reach the accelerator\n");
		return 1;
	}
	if (IOConnectMapMemory(conn, RDN_UC_MEMORY_APERTURE, mach_task_self(), &addr, &len, kIOMapAnywhere | kIOMapWriteCombineCache)) {
		fprintf(stderr, "cannot map video memory\n");
		return 1;
	}
	aperture = (volatile uint32_t *)(uintptr_t)addr;
	span = (size_t)len - FROM;
	if (IOConnectMethodScalarIScalarO(conn, RDN_UC_UVD_INFO, 0, 3, &up, &vcpu, &ring))
		up = 0;
	printf("aperture %lu MB, watching from 16 MB; decoder %s (firmware window at 0x%x, ring at 0x%x)\n", (unsigned long)(len >> 20),
	       up ? "up" : "not up", vcpu, ring);
	a = malloc(span);
	b = malloc(span);
	if (!a || !b) {
		fprintf(stderr, "no memory\n");
		return 1;
	}
	snapshot(a);
	printf("first snapshot done\n");
	fflush(stdout);
	snapshot(b);
	diff(a, b, "control: nothing started");
	tmp = a; a = b; b = tmp;

	snprintf(cmd, sizeof(cmd), "open -g -a %s", argv[1]);
	system(cmd);
	sleep(4);
	snapshot(b);
	diff(a, b, "after the background launch");
	tmp = a; a = b; b = tmp;

	snprintf(cmd, sizeof(cmd), "osascript -e 'tell application \"%s\" to activate' >/dev/null 2>&1", argv[1]);
	system(cmd);
	sleep(3);
	snapshot(b);
	diff(a, b, "after the activation");
	tmp = a; a = b; b = tmp;

	/* wait for the application to quit by itself */
	for (n = 0; n < 60; n++) {
		FILE *f = popen("ps -axww -o command | grep -c '[w]inprobe.app/Contents/MacOS'", "r");
		int running = 0;

		if (f) {
			if (fscanf(f, "%d", &running) != 1)
				running = 0;
			pclose(f);
		}
		if (!running)
			break;
		sleep(1);
	}
	sleep(2);
	snapshot(b);
	diff(a, b, "after the application quit");
	return 0;
}
