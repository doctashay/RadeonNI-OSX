/*
 * uvdreset: ask the kext to start the video decoder's firmware again (RDN_UC_UVD_RESET), n times, with a pause between, and say how long each took.
 * Nothing is decoded. The decoder must have been started once (any decode does it).
 *
 *   uvdreset n [pause_ms]
 *
 * Builds on the G5 (IOKit only), with hw/rdn_user.h next to it:
 *   gcc-4.0 -arch ppc -isysroot /Developer/SDKs/MacOSX10.4u.sdk -mmacosx-version-min=10.4 -I. -o uvdreset uvdreset.c -framework IOKit -framework CoreFoundation
 *
 * Copyright (c) 2026 kouta-kun and Claude
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>
#include <mach/mach.h>
#include <IOKit/IOKitLib.h>

#include "rdn_user.h"

int main(int argc, char **argv)
{
	io_service_t service = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching(RDN_UC_SERVICE_CLASS));
	io_connect_t conn;
	int n = argc > 1 ? atoi(argv[1]) : 1, pause_ms = argc > 2 ? atoi(argv[2]) : 200, i;

	if (!service || IOServiceOpen(service, mach_task_self(), RDN_UC_TYPE, &conn)) {
		fprintf(stderr, "cannot reach the accelerator\n");
		return 1;
	}
	for (i = 0; i < n; i++) {
		struct timeval t0, t1;
		int epoch = 0;
		kern_return_t kr;

		gettimeofday(&t0, NULL);
		kr = IOConnectMethodScalarIScalarO(conn, RDN_UC_UVD_RESET, 0, 1, &epoch);
		gettimeofday(&t1, NULL);
		printf("reset %d: 0x%x, epoch %d, %ld ms\n", i + 1, kr, epoch, (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_usec - t0.tv_usec) / 1000);
		fflush(stdout);
		usleep(pause_ms * 1000);
	}
	return 0;
}
