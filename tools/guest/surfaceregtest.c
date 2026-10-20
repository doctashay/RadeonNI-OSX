/* Exercise reservation cleanup without rendering or allocating VRAM. */
#include <stdio.h>
#include <string.h>
#include <mach/mach.h>
#include <IOKit/IOKitLib.h>
#include "rdn_user.h"

int main(int argc, char **argv)
{
	io_service_t service = IOServiceGetMatchingService(kIOMasterPortDefault,
		IOServiceMatching(RDN_UC_SERVICE_CLASS));
	io_connect_t conn;
	kern_return_t kr;
	unsigned i, verified = 0;
	int buffered = argc > 1 && !strcmp(argv[1], "buffered");
	int hold = buffered || (argc > 1 && !strcmp(argv[1], "hold"));
	int close_each = argc > 1 && !hold;

	if (!service)
		return 1;
	kr = IOServiceOpen(service, mach_task_self(), RDN_UC_TYPE, &conn);
	if (kr)
		return 1;
	{
		struct rdn_user_surface_info info;
		IOByteCount size = sizeof(info);
		kr = IOConnectMethodScalarIStructureO(conn, RDN_UC_SURFACE_QUERY,
			1, &size, 0x7f000000U, &info);
		printf("surface query status: 0x%x\n", kr);
		if (buffered && kr) { IOServiceClose(conn); IOObjectRelease(service); return 1; }
	}
	for (i = 0; i < 256; ++i) {
		unsigned id = 0x7f000000U + i;
		int offset = 0;
		if (buffered) {
			kr = IOConnectMethodScalarIScalarO(conn, RDN_UC_ALLOC,
				2, 1, 4096, 4096, &offset);
			if (kr) { printf("allocation %u failed: 0x%x\n", i, kr); break; }
		}
		kr = IOConnectMethodScalarIScalarO(conn, RDN_UC_SURFACE_BUFFER,
			5, 0, id, offset, buffered ? 64 : 0, buffered ? 16 : 0, buffered ? 16 : 0);
		if (kr) {
			printf("reserve %u failed: 0x%x\n", i, kr);
			break;
		}
		if (buffered) {
			struct rdn_user_surface_info info;
			IOByteCount size = sizeof(info);
			kr = IOConnectMethodScalarIStructureO(conn, RDN_UC_SURFACE_QUERY,
				1, &size, id, &info);
			if (kr || size != sizeof(info) || info.id != id || info.offset != (unsigned)offset ||
			    info.row_bytes != 64 || info.width != 16 || info.height != 16) {
				printf("query %u failed or returned wrong metadata: 0x%x\n", i, kr); break;
			}
			++verified;
		}
		if (hold)
			continue;
		if (close_each) {
			IOServiceClose(conn);
			kr = IOServiceOpen(service, mach_task_self(), RDN_UC_TYPE, &conn);
		} else {
			kr = IOConnectMethodScalarIScalarO(conn, RDN_UC_SURFACE_BUFFER,
				5, 0, id, 0xffffffffU, 0, 0, 0);
		}
		if (kr) {
			printf("release %u failed: 0x%x\n", i, kr);
			break;
		}
	}
	/* Clean up test reservations even on kernels with broken close cleanup. */
	if (close_each || hold) {
		unsigned j;
		for (j = 0; j < 256; ++j)
			IOConnectMethodScalarIScalarO(conn, RDN_UC_SURFACE_BUFFER,
				5, 0, 0x7f000000U + j, 0xffffffffU, 0, 0, 0);
	}
	IOServiceClose(conn);
	IOObjectRelease(service);
	if (buffered) printf("%u exact surface queries verified\n", verified);
	printf("%u %s completed\n", i, hold ? "simultaneous reservations" : "reservation/release cycles");
	return i != 256;
}
