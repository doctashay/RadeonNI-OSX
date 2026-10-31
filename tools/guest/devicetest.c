/* Check explicit accelerator selection without submitting GPU work. */
#include <stdio.h>
#include <IOKit/IOKitLib.h>
#include "rdn_device.h"

int main(void)
{
    io_iterator_t iterator;
    io_service_t service, root;
    unsigned count = 0;
    int failed = 0;
    struct rdn_device *bad = rdn_device_open_service(0);
    if (bad) {
        bad->destroy(bad);
        return 1;
    }
    root = IORegistryGetRootEntry(kIOMasterPortDefault);
    if (!root)
        return 1;
    bad = rdn_device_open_service(root);
    IOObjectRelease(root);
    if (bad) {
        bad->destroy(bad);
        return 1;
    }
    if (IOServiceGetMatchingServices(kIOMasterPortDefault,
            IOServiceMatching("RadeonNIAccel"), &iterator))
        return 1;
    while ((service = IOIteratorNext(iterator))) {
        struct rdn_device *a = rdn_device_open_service(service);
        struct rdn_device *b = rdn_device_open_service(service);
        unsigned width = a ? a->screen.width : 0;
        ++count;
        if (!a || !b || !width) {
            failed = 1;
        } else {
            a->screen.width = b->screen.width = 0;
            a->screen_refresh(a);
            b->screen_refresh(b);
            if (a->info.pci_device_id != b->info.pci_device_id ||
                a->screen.width != width || b->screen.width != width)
                failed = 1;
            printf("accelerator %u: PCI %04x, refresh %u/%u, expected %u\n",
                count, a->info.pci_device_id, a->screen.width,
                b->screen.width, width);
        }
        if (b) b->destroy(b);
        if (a) a->destroy(a);
        IOObjectRelease(service);
    }
    IOObjectRelease(iterator);
    printf("%s: %u accelerator(s); invalid services rejected\n",
        count && !failed ? "PASS" : "FAIL", count);
    return !count || failed;
}
