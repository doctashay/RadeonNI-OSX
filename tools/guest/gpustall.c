/* Read-only GPU progress snapshot; does not map or flush video memory. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include "rdn_user.h"

static const struct { const char *name; unsigned address; } registers[] = {
    {"SRBM_STATUS", 0x0e50}, {"SRBM_STATUS2", 0x0ec4},
    {"GRBM_STATUS", 0x8010}, {"GRBM_STATUS2", 0x8014},
    {"CP_STALLED_STAT1", 0x8674}, {"CP_STALLED_STAT2", 0x8678},
    {"CP_BUSY_STAT", 0x867c}, {"CP_STAT", 0x8680},
    {"CP_RB_RPTR", 0x8700}, {"CP_RB_WPTR", 0xc114},
    {"CP_RB_BASE", 0xc100}, {"CP_RB_CNTL", 0xc104},
    {"CP_ME_CNTL", 0x86d8}, {"SCRATCH0", 0x8500},
    {"SCRATCH1", 0x8504}, {"SCRATCH2", 0x8508},
    {"VM_CONTEXT0_FAULT_ADDR", 0x14f8}, {"VM_CONTEXT0_FAULT_STATUS", 0x14d8},
    {"VM_CONTEXT0_PTE_BASE", 0x153c}, {"VM_CONTEXT0_START", 0x155c},
    {"VM_CONTEXT1_FAULT_ADDR", 0x14fc}, {"VM_CONTEXT1_FAULT_STATUS", 0x14dc}
};

int main(int argc, char **argv)
{
    io_iterator_t iterator = 0;
    io_service_t service;
    if (IOServiceGetMatchingServices(kIOMasterPortDefault,
            IOServiceMatching(RDN_UC_SERVICE_CLASS), &iterator)) return 1;
    while ((service = IOIteratorNext(iterator))) {
        io_connect_t connection = 0;
        struct rdn_user_info info;
        IOByteCount bytes = sizeof(info);
        CFTypeRef property;
        char bundle[128] = "unknown";
        unsigned flags = 0, events = 0, fallbacks = 0, pass, i;
        kern_return_t result = IOServiceOpen(service, mach_task_self(), RDN_UC_TYPE_V2, &connection);
        if (result) result = IOServiceOpen(service, mach_task_self(), RDN_UC_TYPE, &connection);
        property = IORegistryEntryCreateCFProperty(service, CFSTR("IOGLBundleName"), kCFAllocatorDefault, 0);
        if (property) {
            if (CFGetTypeID(property) == CFStringGetTypeID())
                CFStringGetCString(property, bundle, sizeof(bundle), kCFStringEncodingUTF8);
            CFRelease(property);
        }
        IOObjectRelease(service);
        if (result) { printf("%s open=%08x\n", bundle, result); continue; }
        memset(&info, 0, sizeof(info));
        result = IOConnectMethodScalarIStructureO(connection, RDN_UC_GET_INFO, 0, &bytes, &info);
        printf("device=%04x bundle=%s info_result=%08x version=%u bytes=%u\n",
               info.pci_device_id, bundle, result, info.version, (unsigned)bytes);
        result = IOConnectMethodScalarIScalarO(connection, RDN_UC_FENCE_CONTROL, 1, 3,
                                               0, &flags, &events, &fallbacks);
        printf("fence_query=%08x flags=%08x events=%u fallbacks=%u\n", result, flags, events, fallbacks);
        for (pass = 0; pass < 2; pass++) {
            printf("snapshot=%u\n", pass);
            for (i = 0; i < sizeof(registers)/sizeof(registers[0]); i++) {
                unsigned value = 0;
                result = IOConnectMethodScalarIScalarO(connection, RDN_UC_REG_READ, 1, 1,
                                                        registers[i].address, &value);
                printf("%s[%05x]=%08x result=%08x\n", registers[i].name, registers[i].address, value, result);
            }
            if (!pass) usleep(1000000);
        }
        if (argc == 4 && info.pci_device_id == 0x6704) {
            unsigned long offset = strtoul(argv[2], NULL, 0);
            unsigned long length = strtoul(argv[3], NULL, 0);
            vm_address_t address = 0;
            vm_size_t size = 0;
            if (offset > info.aperture_size || length > info.aperture_size - offset) {
                fprintf(stderr, "dump requires an aperture-bounded range\n");
                return 1;
            }
            result = IOConnectMapMemory(connection, RDN_UC_MEMORY_APERTURE, mach_task_self(),
                                         &address, &size, kIOMapAnywhere | kIOMapReadOnly);
            if (!result && offset <= size && length <= size - offset) {
                FILE *file = fopen(argv[1], "wb");
                if (!file || fwrite((const char *)address + offset, 1, length, file) != length) return 1;
                fclose(file);
                printf("saved=%s offset=%08lx length=%lu\n", argv[1], offset, length);
            } else {
                fprintf(stderr, "read-only aperture map failed: %08x\n", result);
                return 1;
            }
            IOConnectUnmapMemory(connection, RDN_UC_MEMORY_APERTURE, mach_task_self(), address);
        }
        IOServiceClose(connection);
    }
    IOObjectRelease(iterator);
    return 0;
}
