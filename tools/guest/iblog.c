/*
 * iblog: switch the kext's command buffer recorder on and off, mark a moment, and put its lists into the registry (RDN_UC_IBLOG in hw/rdn_user.h).
 *
 *   iblog on | off | mark | dump
 *   iblog dbg n      (RDN_UC_DEBUG: 1 display reads off, 2 on, 3 HDP flush, 4 decoder sub-blocks reset, 5 decoder block reset; prints SRBM_STATUS)
 *
 * Each call prints the kext's clock in milliseconds, the same clock as the entries. `dump` fills the accelerator's registry property "IBLog":
 *   ioreg -c RadeonNIAccel -l -w0
 *
 * Builds on the G5 (its Xcode), with hw/rdn_user.h next to it:
 *   gcc-4.0 -arch ppc -isysroot /Developer/SDKs/MacOSX10.4u.sdk -mmacosx-version-min=10.4 -I. -o iblog iblog.c -framework IOKit -framework CoreFoundation
 *
 * Copyright (c) 2026 kouta-kun and Claude
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mach/mach.h>
#include <IOKit/IOKitLib.h>

#include "rdn_user.h"

int main(int argc, char **argv)
{
	io_service_t service;
	io_connect_t conn;
	int op, ms = 0;
	kern_return_t kr;

	if (argc == 3 && !strcmp(argv[1], "dbg")) {
		int st = 0;

		service = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching(RDN_UC_SERVICE_CLASS));
		if (!service || IOServiceOpen(service, mach_task_self(), RDN_UC_TYPE, &conn)) {
			fprintf(stderr, "cannot reach the accelerator\n");
			return 1;
		}
		kr = IOConnectMethodScalarIScalarO(conn, RDN_UC_DEBUG, 1, 1, atoi(argv[2]), &st);
		printf("dbg %s: 0x%x, SRBM_STATUS 0x%08x\n", argv[2], kr, (unsigned)st);
		return kr ? 1 : 0;
	}
	if (argc < 2) {
		fprintf(stderr, "usage: iblog on | off | mark | dump\n");
		return 1;
	}
	if (!strcmp(argv[1], "off"))
		op = 0;
	else if (!strcmp(argv[1], "on"))
		op = 1;
	else if (!strcmp(argv[1], "dump"))
		op = 2;
	else if (!strcmp(argv[1], "mark"))
		op = 3;
	else {
		fprintf(stderr, "usage: iblog on | off | mark | dump\n");
		return 1;
	}
	service = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching(RDN_UC_SERVICE_CLASS));
	if (!service) {
		fprintf(stderr, "no %s\n", RDN_UC_SERVICE_CLASS);
		return 1;
	}
	kr = IOServiceOpen(service, mach_task_self(), RDN_UC_TYPE, &conn);
	if (kr) {
		fprintf(stderr, "cannot open the accelerator (0x%x)\n", kr);
		return 1;
	}
	kr = IOConnectMethodScalarIScalarO(conn, RDN_UC_IBLOG, 1, 1, op, &ms);
	printf("iblog %s: 0x%x, kext time %d ms\n", argv[1], kr, ms);
	IOServiceClose(conn);
	return kr ? 1 : 0;
}
