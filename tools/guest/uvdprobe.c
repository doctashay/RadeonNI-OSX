/*
 * UVD bring-up diagnostics; querying does not start or reset the engine.
 * gcc -Ihw tools/guest/uvdprobe.c -framework IOKit -framework CoreFoundation -o uvdprobe
 *
 * Copyright (c) 2026 kouta-kun and Claude
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <mach/mach.h>
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include "rdn_user.h"

int main(int argc, char **argv)
{
	io_service_t service;
	io_connect_t connection = IO_OBJECT_NULL;
	CFTypeRef property;
	struct rdn_user_uvd_diagnostic info;
	IOByteCount size = sizeof(info);
	uint32_t version = 0;
	int status = 0, result = 1;
	kern_return_t kr;
	int initialize = argc == 2 && !strcmp(argv[1], "init");

	if (argc > 2 || (argc == 2 && !initialize && strcmp(argv[1], "info"))) {
		fprintf(stderr, "usage: %s [info|init]\n", argv[0]);
		return 2;
	}
	service = IOServiceGetMatchingService(kIOMasterPortDefault,
		IOServiceMatching(RDN_UC_SERVICE_CLASS));
	if (!service) {
		fprintf(stderr, "RadeonNI accelerator is not available\n");
		return 1;
	}
	property = IORegistryEntryCreateCFProperty(service,
		CFSTR("RadeonNIUVDDiagnosticVersion"), kCFAllocatorDefault, 0);
	if (property && CFGetTypeID(property) == CFNumberGetTypeID())
		CFNumberGetValue((CFNumberRef)property, kCFNumberSInt32Type, &version);
	if (property)
		CFRelease(property);
	if (version != RDN_UVD_DIAGNOSTIC_VERSION) {
		printf("UVD diagnostic ABI unavailable; no engine operation attempted\n");
		IOObjectRelease(service);
		return 3;
	}
	kr = IOServiceOpen(service, mach_task_self(), RDN_UC_TYPE, &connection);
	IOObjectRelease(service);
	if (kr) {
		fprintf(stderr, "IOServiceOpen: 0x%x\n", kr);
		return 1;
	}
	memset(&info, 0, sizeof(info));
	kr = IOConnectMethodScalarIStructureO(connection, RDN_UC_UVD_DIAGNOSTIC_INFO,
		0, &size, &info);
	if (kr || size != sizeof(info) || info.version != version) {
		fprintf(stderr, "UVD diagnostic query: 0x%x, size %lu\n",
			kr, (unsigned long)size);
		goto out;
	}
	printf("UVD diagnostic: state=%u flags=0x%x status=0x%08x error=%d reserved=%u\n",
		info.state, info.flags, info.last_status, info.last_error, info.reserved_bytes);
	if (initialize) {
		const uint32_t required = RDN_UVD_DIAGNOSTIC_ALLOWED |
			RDN_UVD_DIAGNOSTIC_FIRMWARE | RDN_UVD_DIAGNOSTIC_RLC_FIRMWARE;
		if (geteuid() != 0 ||
		    (info.flags & required) != required) {
			fprintf(stderr, "init requires root, EnableUVDDiagnostics, FW_UVD and FW_RLC\n");
			goto out;
		}
		kr = IOConnectMethodScalarIScalarO(connection, RDN_UC_UVD_DIAGNOSTIC_INIT,
			1, 1, 0, &status);
		printf("UVD init: result=0x%x status=0x%08x\n", kr, (unsigned)status);
		if (kr)
			goto out;
		printf("Firmware/ring test completed; H.264 decoding is not yet verified\n");
	}
	result = 0;
out:
	IOServiceClose(connection);
	return result;
}
