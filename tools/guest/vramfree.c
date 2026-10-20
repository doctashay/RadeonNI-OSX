/*
 * vramfree: how much of the video memory heap is free right now, and in what pieces. It only asks the kext's allocator for ranges and gives each
 * back at once (the largest block by bisection), then, for the total, takes the blocks one after another and gives them all back; nothing is
 * written to the card. Run it with a video decoder open (a paused QuickTime Player) to see what a second decoder would find. A decoder that
 * allocates in the same few milliseconds may see its allocation fail, so do not run it while a clip is playing.
 *
 *   vramfree                 largest block, total free, and the pieces
 *   vramfree -h              the same for the video memory beyond the aperture (RDN_UC_ALLOC_HIDDEN), which only the GPU reaches
 *   vramfree dpb image bitstream   also the decoder's room check as rdnvd's room_for() makes it: `dpb` bytes, then four buffers of
 *                            `bitstream` bytes, eight of `image` bytes and one of 1 MB, all held at once (1080p: 23827456 3133440 4177920)
 *
 * Builds on the G5 (IOKit only), with hw/rdn_user.h next to it:
 *   gcc-4.0 -arch ppc -isysroot /Developer/SDKs/MacOSX10.4u.sdk -mmacosx-version-min=10.4 -I. -o vramfree vramfree.c -framework IOKit -framework CoreFoundation
 *
 * Copyright (c) 2026 kouta-kun and Claude
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <mach/mach.h>
#include <IOKit/IOKitLib.h>

#include "rdn_user.h"

static io_connect_t conn;
static int hidden;

static int vram_alloc(uint32_t bytes, uint32_t *offset)
{
	int off = 0;

	if (IOConnectMethodScalarIScalarO(conn, hidden ? RDN_UC_ALLOC_HIDDEN : RDN_UC_ALLOC, 2, 1, (int)bytes, 4096, &off))
		return -1;
	*offset = (uint32_t)off;
	return 0;
}

static void vram_free(uint32_t offset)
{
	IOConnectMethodScalarIScalarO(conn, RDN_UC_FREE, 1, 0, (int)offset);
}

/* The largest size (a multiple of 4 KB) the allocator gives now, between `lo` (known good) and `hi` (a size that may fail) */
static uint32_t largest(uint32_t hi)
{
	uint32_t lo = 0, off;

	while (hi - lo > 4096) {
		uint32_t mid = (lo + (hi - lo) / 2) & ~4095u;

		if (!mid)
			break;
		if (!vram_alloc(mid, &off)) {
			vram_free(off);
			lo = mid;
		} else {
			hi = mid;
		}
	}
	return lo;
}

int main(int argc, char **argv)
{
	io_service_t service = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching(RDN_UC_SERVICE_CLASS));
	struct rdn_user_info info;
	IOByteCount infosize = sizeof(info);
	uint32_t held[256], nheld = 0, got, total = 0, off, i;

	if (!service || IOServiceOpen(service, mach_task_self(), RDN_UC_TYPE, &conn)) {
		fprintf(stderr, "cannot reach the accelerator\n");
		return 1;
	}
	if (IOConnectMethodScalarIStructureO(conn, RDN_UC_GET_INFO, 0, &infosize, &info)) {
		fprintf(stderr, "no info\n");
		return 1;
	}
	if (argc >= 2 && !strcmp(argv[1], "-h")) {
		int hoff = 0, hsize = 0;

		if (IOConnectMethodScalarIScalarO(conn, RDN_UC_HIDDEN_INFO, 0, 2, &hoff, &hsize) || !hsize) {
			fprintf(stderr, "no hidden memory\n");
			return 1;
		}
		hidden = 1;
		info.heap_size = (uint32_t)hsize;
		info.heap_offset = (uint32_t)hoff;
		argc = 1;
	}
	printf("%s heap %u KB (from %u KB)\n", hidden ? "hidden" : "visible", info.heap_size / 1024, info.heap_offset / 1024);
	printf("largest block: %u KB\n", largest(info.heap_size) / 1024);
	/* the pieces: take the largest block, again and again, then give them all back */
	for (;;) {
		got = largest(info.heap_size);
		if (got < 64 * 1024 || nheld >= 256 || vram_alloc(got, &off))
			break;
		held[nheld++] = off;
		total += got;
		printf("  piece %u KB\n", got / 1024);
	}
	for (i = 0; i < nheld; i++)
		vram_free(held[i]);
	printf("free in pieces of 64 KB or more: %u KB\n", total / 1024);
	if (argc >= 4) {
		uint32_t dpb = (uint32_t)strtoul(argv[1], NULL, 0), image = (uint32_t)strtoul(argv[2], NULL, 0);
		uint32_t bs = (uint32_t)strtoul(argv[3], NULL, 0), a, pieces[16], n = 0, want = 13;
		int ok = 0;

		if (!vram_alloc(dpb, &a)) {
			while (n < want && !vram_alloc(n < 4 ? bs : n < 12 ? image : (1u << 20), &pieces[n]))
				n++;
			ok = n == want;
			while (n)
				vram_free(pieces[--n]);
			vram_free(a);
		}
		printf("room check (%u KB, then 4 x %u KB, 8 x %u KB, 1 MB): %s\n", dpb / 1024, bs / 1024, image / 1024, ok ? "room" : "NO ROOM");
	}
	IOServiceClose(conn);
	return 0;
}
