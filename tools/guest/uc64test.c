/* Check scalar conversion and GART wiring above the 32-bit address range. */
#include <stdio.h>
#include <stdint.h>
#include <mach/mach.h>
#include "../../mesa/target/rdn_iokit.h"
#include "../../hw/rdn_user.h"

int main(int argc, char **argv)
{
    io_service_t service;
    io_connect_t conn = 0;
    struct rdn_user_info info;
    IOByteCount size = sizeof(info);
    uint32_t offset = 0;
    vm_address_t address = 0x100000000ULL;
    kern_return_t kr;
    setbuf(stdout, NULL);
    service = IOServiceGetMatchingService(kIOMasterPortDefault,
                                         IOServiceMatching(RDN_UC_SERVICE_CLASS));
    if (!service) return 1;
    kr = IOServiceOpen(service, mach_task_self(), RDN_UC_TYPE, &conn);
    IOObjectRelease(service);
    printf("open: %x\n", kr);
    if (kr) return 1;
    kr = IOConnectMethodScalarIStructureO(conn, RDN_UC_GET_INFO, 0, &size, &info);
    printf("info: %x, size %lu, version %u, aperture %u\n", kr,
           (unsigned long)size, info.version, info.aperture_size);
    if (kr || size != sizeof(info) || info.version != RDN_USER_VERSION) return 1;
    kr = IOConnectMethodScalarIScalarO(conn, RDN_UC_ALLOC, 2, 1, 4096, 4096, &offset);
    printf("allocate: %x, offset %x\n", kr, offset);
    if (kr) return 1;
    if (IOConnectMethodScalarIScalarO(conn, RDN_UC_FREE, 1, 0, offset)) return 1;
    if (argc > 1) {
        kr = vm_allocate(mach_task_self(), &address, 4096, VM_FLAGS_ANYWHERE);
        printf("VM allocation: %x, address %llx\n", kr, (unsigned long long)address);
        if (kr || address <= UINT32_MAX) return 1;
        *(volatile uint32_t *)address = 0x12345678;
        kr = IOConnectMethodScalarIScalarO(conn, RDN_UC_GART_BIND64, 3, 1,
             (uint32_t)address, (uint32_t)(address >> 32), 4096, &offset);
        printf("64-bit GART bind: %x, offset %x\n", kr, offset);
        if (kr) return 1;
        if (IOConnectMethodScalarIScalarO(conn, RDN_UC_GART_UNBIND, 1, 0, offset)) return 1;
        vm_deallocate(mach_task_self(), address, 4096);
    }
    IOServiceClose(conn);
    puts("PASS");
    return 0;
}
