/* Observe fence progress without mapping or flushing GPU memory. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <mach/mach_time.h>
#include <IOKit/IOKitLib.h>
#include "rdn_user.h"

static double milliseconds(void)
{
    static mach_timebase_info_data_t tb;
    if (!tb.denom)
        mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1000000.0;
}

static int read_reg(io_connect_t conn, unsigned reg, unsigned *value)
{
    return IOConnectMethodScalarIScalarO(conn, RDN_UC_REG_READ, 1, 1,
                                        (int)reg, value);
}

int main(int argc, char **argv)
{
    io_service_t service;
    io_connect_t conn = 0;
    unsigned fence = 0, last = 0, status = 0, rptr = 0;
    double start, changed, logged = 0, now;
    char *end;
    long seconds;
    int result = 0;

    if (argc != 2) {
        fprintf(stderr, "usage: hangwatch seconds (1..3600)\n");
        return 1;
    }
    seconds = strtol(argv[1], &end, 10);
    if (*end || seconds < 1 || seconds > 3600)
        return 1;
    service = IOServiceGetMatchingService(kIOMasterPortDefault,
                 IOServiceMatching(RDN_UC_SERVICE_CLASS));
    if (!service)
        return 1;
    result = IOServiceOpen(service, mach_task_self(), RDN_UC_TYPE, &conn);
    IOObjectRelease(service);
    if (result)
        return 1;
    setvbuf(stdout, NULL, _IOLBF, 0);
    start = changed = milliseconds();
    while ((now = milliseconds()) - start < seconds * 1000.0) {
        int stalled;
        if (read_reg(conn, 0x8504, &fence) || read_reg(conn, 0x8508, &rptr) ||
            read_reg(conn, 0x8010, &status)) {
            result = 1;
            break;
        }
        if (fence != last || !(status & 0x80000000u))
            changed = now;
        last = fence;
        stalled = now - changed >= 5000;
        if (now - logged >= 1000 || stalled) {
            printf("{\"ms\":%.0f,\"fence\":%u,\"rptr\":%u,\"status\":%u,"
                   "\"stalled\":%s}\n", now - start, fence, rptr, status,
                   stalled ? "true" : "false");
            logged = now;
        }
        if (stalled) {
            result = 2;
            break;
        }
        usleep(100000);
    }
    IOServiceClose(conn);
    return result;
}
