/* Verify that clientClose drains submitted work before freeing its buffers. */
#include <stdio.h>
#include <mach/mach.h>
#include <IOKit/IOKitLib.h>
#include "rdn_user.h"

int main(void)
{
    io_service_t service = IOServiceGetMatchingService(kIOMasterPortDefault,
        IOServiceMatching(RDN_UC_SERVICE_CLASS));
    io_connect_t monitor = 0, client = 0;
    unsigned cycle;
    if (!service || IOServiceOpen(service, mach_task_self(), RDN_UC_TYPE, &monitor))
        return 1;
    for (cycle = 0; cycle < 32; cycle++) {
        vm_address_t address = 0;
        vm_size_t length = 0;
        int offset = 0, fence = 0, reached = 0;
        unsigned i;
        volatile unsigned *words;
        if (IOServiceOpen(service, mach_task_self(), RDN_UC_TYPE, &client) ||
            IOConnectMapMemory(client, RDN_UC_MEMORY_APERTURE, mach_task_self(),
                &address, &length, kIOMapAnywhere) ||
            IOConnectMethodScalarIScalarO(client, RDN_UC_ALLOC, 2, 1,
                262144, 4096, &offset))
            return 2;
        words = (volatile unsigned *)(address + (unsigned)offset);
        for (i = 0; i < 65536; i++)
            words[i] = 0x80000000;
        if (IOConnectMethodScalarIScalarO(client, RDN_UC_SUBMIT, 2, 1,
                offset, 65536, &fence))
            return 3;
        IOConnectUnmapMemory(client, RDN_UC_MEMORY_APERTURE,
            mach_task_self(), address);
        if (IOServiceClose(client) ||
            IOConnectMethodScalarIScalarO(monitor, RDN_UC_FENCE_WAIT, 2, 1,
                fence, 0, &reached) || !reached) {
            fprintf(stderr, "client close returned before fence %u (cycle %u)\n",
                (unsigned)fence, cycle);
            return 4;
        }
    }
    IOServiceClose(monitor);
    IOObjectRelease(service);
    puts("PASS: 32 client closes completed their submitted fences");
    return 0;
}
