/* Read quarantined UVD firmware through CP DMA into client-owned VRAM.
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FW_BYTES 205080u
#define GUARD_BYTES 4096u
#define DEST_BYTES ((FW_BYTES + 2 * GUARD_BYTES + 4095) & ~4095u)
#define IB_BYTES 4096u
#define COPY_WORDS 13u
#define SENTINEL_BYTES 16u
#define SENTINEL_ALLOC_BYTES (3u * GUARD_BYTES)

static int within(uint32_t offset, uint32_t bytes, uint32_t limit)
{
    return offset <= limit && bytes <= limit - offset;
}

static int overlap(uint32_t a, uint32_t an, uint32_t b, uint32_t bn)
{
    return (uint64_t)a < (uint64_t)b + bn &&
           (uint64_t)b < (uint64_t)a + an;
}

static int valid_storage(uint32_t source, uint32_t reserved,
                         uint32_t destination, uint32_t ib, uint32_t limit)
{
    return !(source & 4095) && !(destination & 4095) && !(ib & 4095) &&
           reserved >= FW_BYTES && within(source, reserved, limit) &&
           within(destination, DEST_BYTES, limit) && within(ib, IB_BYTES, limit) &&
           !overlap(source, reserved, destination, DEST_BYTES) &&
           !overlap(source, reserved, ib, IB_BYTES) &&
           !overlap(destination, DEST_BYTES, ib, IB_BYTES);
}

static int make_copy(uint32_t *words, uint64_t source, uint64_t destination)
{
    const uint64_t limit = (uint64_t)1 << 40;
    if (!words || source >= limit || destination >= limit ||
        FW_BYTES > limit - source || FW_BYTES > limit - destination ||
        (source < destination + FW_BYTES && destination < source + FW_BYTES))
        return 0;
    /* Match cpdmatest: surface sync, byte-copy CP_DMA, then PFP_SYNC_ME. */
    words[0] = 0xc0034300; words[1] = 0x1980003c;
    words[2] = 0xffffffff; words[3] = 0; words[4] = 10;
    words[5] = 0xc0044100; words[6] = (uint32_t)source;
    words[7] = 0x80000000u | (uint32_t)(source >> 32);
    words[8] = (uint32_t)destination;
    words[9] = (uint32_t)(destination >> 32);
    words[10] = FW_BYTES;
    words[11] = 0xc0004200; words[12] = 0;
    return 1;
}

static int valid_sentinel_storage(uint32_t destination, uint32_t ib, uint32_t limit)
{
    return !(destination & 4095) && !(ib & 4095) &&
           within(destination, SENTINEL_ALLOC_BYTES, limit) && within(ib, IB_BYTES, limit) &&
           !overlap(destination, SENTINEL_ALLOC_BYTES, ib, IB_BYTES);
}

static int make_sentinel(uint32_t *words, uint64_t destination)
{
    const uint64_t limit = (uint64_t)1 << 40;
    if (!words || (destination & 3) || destination >= limit ||
        SENTINEL_BYTES > limit - destination)
        return 0;
    /* Mesa evergreen_cp_dma_clear_buffer: immediate DATA, no byte swapping. */
    words[0] = 0xc0034300; words[1] = 0x1980003c;
    words[2] = 0xffffffff; words[3] = 0; words[4] = 10;
    words[5] = 0xc0044100; words[6] = 0x01234567;
    words[7] = 0xc0000000u;
    words[8] = (uint32_t)destination;
    words[9] = (uint32_t)(destination >> 32);
    words[10] = SENTINEL_BYTES;
    words[11] = 0xc0004200; words[12] = 0;
    return 1;
}

static int parse_offset(const char *text, uint32_t *offset)
{
    char *end;
    unsigned long long value;
    if (!text[0] || text[0] == '-' || text[0] == '+' || isspace((unsigned char)text[0]))
        return 0;
    errno = 0;
    value = strtoull(text, &end, 0);
    if (errno || *end || value > UINT32_MAX || (value & 4095))
        return 0;
    *offset = (uint32_t)value;
    return 1;
}

#ifndef UVDFETCHPROBE_MODEL
#include <signal.h>
#include <unistd.h>
#include <mach/mach.h>
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include "rdn_user.h"

#define CALL(expression) do { kr = (expression); if (kr) { \
    fprintf(stderr, "line %u: IOKit error 0x%x\n", (unsigned)__LINE__, kr); \
    goto out; } } while (0)

static int sentinel_main(void)
{
    static const uint8_t expected[4] = { 0x67, 0x45, 0x23, 0x01 };
    io_service_t service = IO_OBJECT_NULL;
    io_connect_t connection = IO_OBJECT_NULL;
    vm_address_t aperture = 0;
    vm_size_t mapped_bytes = 0;
    struct rdn_user_info info;
    IOByteCount size;
    uint32_t destination = 0, ib = 0, fence = 0, done = 0;
    uint32_t words[COPY_WORDS], i, mismatches = 0, guard_errors = 0;
    uint64_t base, destination_gpu;
    volatile uint8_t *mapped, *bytes;
    volatile uint32_t *commands;
    char raw[2 * SENTINEL_BYTES + 1];
    kern_return_t kr;
    int result = 1;

    service = IOServiceGetMatchingService(kIOMasterPortDefault,
        IOServiceMatching(RDN_UC_SERVICE_CLASS));
    if (!service) goto out;
    CALL(IOServiceOpen(service, mach_task_self(), RDN_UC_TYPE_V2, &connection));
    memset(&info, 0, sizeof(info)); size = sizeof(info);
    CALL(IOConnectMethodScalarIStructureO(connection, RDN_UC_GET_INFO, 0, &size, &info));
    if (size != sizeof(info) || info.version != RDN_USER_VERSION) goto out;
    CALL(IOConnectMapMemory(connection, RDN_UC_MEMORY_APERTURE, mach_task_self(),
        &aperture, &mapped_bytes, kIOMapAnywhere));
    if (!aperture || !info.aperture_size || mapped_bytes < info.aperture_size ||
        (uint64_t)info.aperture_size - 1 > (uint64_t)UINTPTR_MAX - aperture) goto out;
    mapped = (volatile uint8_t *)aperture;
    CALL(IOConnectMethodScalarIScalarO(connection, RDN_UC_ALLOC, 2, 1,
        SENTINEL_ALLOC_BYTES, 4096, &destination));
    CALL(IOConnectMethodScalarIScalarO(connection, RDN_UC_ALLOC, 2, 1,
        IB_BYTES, 4096, &ib));
    if (!valid_sentinel_storage(destination, ib, info.aperture_size)) goto out;
    base = ((uint64_t)info.vram_gpu_base_hi << 32) | info.vram_gpu_base_lo;
    if (base >= ((uint64_t)1 << 40) ||
        info.aperture_size > ((uint64_t)1 << 40) - base) goto out;
    destination_gpu = base + destination + GUARD_BYTES;
    if (!make_sentinel(words, destination_gpu)) goto out;
    bytes = mapped + destination;
    for (i = 0; i < SENTINEL_ALLOC_BYTES; i++) bytes[i] = 0xa5;
    commands = (volatile uint32_t *)(mapped + ib);
    for (i = 0; i < COPY_WORDS; i++) commands[i] = words[i];
    __asm__ volatile("sync" ::: "memory");
    kr = IOConnectMethodScalarIScalarO(connection, RDN_UC_SUBMIT,
        2, 1, ib, COPY_WORDS, &fence);
    if (kr) {
        printf("{\"status\":\"submission_uncertain_allocations_retained\","
            "\"mode\":\"sentinel\",\"pid\":%u,\"error\":%u}\n",
            (unsigned)getpid(), (unsigned)kr);
        signal(SIGINT, SIG_IGN); signal(SIGTERM, SIG_IGN);
        for (;;) pause();
    }
    CALL(IOConnectMethodScalarIScalarO(connection, RDN_UC_FENCE_WAIT,
        2, 1, fence, 3000, &done));
    if (!done) {
        fprintf(stderr, "Fresh sentinel fence %u timed out; no destination read\n", fence);
        goto out;
    }
    CALL(IOConnectMethodScalarIScalarO(connection, RDN_UC_SYNC_FOR_CPU, 0, 0));
    for (i = 0; i < SENTINEL_ALLOC_BYTES; i++) {
        if (i >= GUARD_BYTES && i < GUARD_BYTES + SENTINEL_BYTES) {
            if (bytes[i] != expected[(i - GUARD_BYTES) & 3]) mismatches++;
        } else if (bytes[i] != 0xa5) guard_errors++;
    }
    for (i = 0; i < SENTINEL_BYTES; i++)
        snprintf(raw + 2 * i, 3, "%02x", (unsigned)bytes[GUARD_BYTES + i]);
    result = mismatches || guard_errors ? 1 : 0;
    printf("{\"status\":\"%s\",\"mode\":\"sentinel\",\"bytes\":%u,"
        "\"immediate\":\"0x01234567\",\"expected_raw\":\"67452301674523016745230167452301\","
        "\"actual_raw\":\"%s\",\"destination_gpu\":\"0x%010llx\","
        "\"destination_offset\":%u,\"ib_offset\":%u,\"fence\":%u,"
        "\"mismatches\":%u,\"guard_errors\":%u}\n", result ? "fail" : "pass",
        SENTINEL_BYTES, raw, (unsigned long long)destination_gpu,
        destination, ib, fence, mismatches, guard_errors);
out:
    /* Client teardown waits for its graphics fence; timeout retains storage. */
    if (connection) IOServiceClose(connection);
    if (service) IOObjectRelease(service);
    return result;
}

int main(int argc, char **argv)
{
    io_service_t service = IO_OBJECT_NULL;
    io_connect_t connection = IO_OBJECT_NULL;
    vm_address_t aperture = 0;
    vm_size_t mapped_bytes = 0;
    struct rdn_user_info info;
    struct rdn_user_uvd_diagnostic diagnostic;
    IOByteCount size;
    CFTypeRef property;
    uint32_t version = 0, source = 0, destination = 0, ib = 0;
    uint32_t fence = 0, done = 0, words[COPY_WORDS];
    uint32_t i, mismatches = 0, guard_errors = 0, source_errors = 0;
    uint32_t first_mismatch = UINT32_MAX;
    uint64_t base, source_gpu, destination_gpu;
    volatile uint8_t *mapped, *copied;
    volatile uint32_t *commands;
    uint8_t *firmware = NULL;
    FILE *file = NULL;
    kern_return_t kr;
    int result = 1;

    setbuf(stdout, NULL);
    if (argc == 2 && !strcmp(argv[1], "--sentinel"))
        return sentinel_main();
    if (argc != 3 || !parse_offset(argv[1], &source)) {
        fprintf(stderr, "usage: %s <page-aligned VCPU aperture offset> <205080-byte firmware> | --sentinel\n", argv[0]);
        return 2;
    }
    firmware = malloc(FW_BYTES);
    file = fopen(argv[2], "rb");
    if (!firmware || !file || fread(firmware, 1, FW_BYTES, file) != FW_BYTES ||
        fgetc(file) != EOF || ferror(file)) {
        fprintf(stderr, "Expected exactly %u firmware bytes\n", FW_BYTES);
        goto out;
    }
    fclose(file); file = NULL;
    service = IOServiceGetMatchingService(kIOMasterPortDefault,
        IOServiceMatching(RDN_UC_SERVICE_CLASS));
    if (!service) goto out;
    property = IORegistryEntryCreateCFProperty(service,
        CFSTR("RadeonNIUVDDiagnosticVersion"), kCFAllocatorDefault, 0);
    if (property && CFGetTypeID(property) == CFNumberGetTypeID())
        CFNumberGetValue((CFNumberRef)property, kCFNumberSInt32Type, &version);
    if (property) CFRelease(property);
    if (version != RDN_UVD_DIAGNOSTIC_VERSION) {
        fprintf(stderr, "UVD diagnostic ABI unavailable\n"); goto out;
    }
    CALL(IOServiceOpen(service, mach_task_self(), RDN_UC_TYPE_V2, &connection));
    memset(&diagnostic, 0, sizeof(diagnostic)); size = sizeof(diagnostic);
    CALL(IOConnectMethodScalarIStructureO(connection, RDN_UC_UVD_DIAGNOSTIC_INFO,
        0, &size, &diagnostic));
    if (size != sizeof(diagnostic) || diagnostic.version != version ||
        diagnostic.state != RDN_UVD_DIAGNOSTIC_FAULTED ||
        diagnostic.reserved_bytes < FW_BYTES) {
        fprintf(stderr, "Expected a faulted UVD engine with quarantined storage\n"); goto out;
    }
    memset(&info, 0, sizeof(info)); size = sizeof(info);
    CALL(IOConnectMethodScalarIStructureO(connection, RDN_UC_GET_INFO, 0, &size, &info));
    if (size != sizeof(info) || info.version != RDN_USER_VERSION ||
        !within(source, diagnostic.reserved_bytes, info.aperture_size)) goto out;
    CALL(IOConnectMapMemory(connection, RDN_UC_MEMORY_APERTURE, mach_task_self(),
        &aperture, &mapped_bytes, kIOMapAnywhere));
    if (!aperture || mapped_bytes < info.aperture_size ||
        (uint64_t)info.aperture_size - 1 > (uint64_t)UINTPTR_MAX - aperture) goto out;
    mapped = (volatile uint8_t *)aperture;
    CALL(IOConnectMethodScalarIScalarO(connection, RDN_UC_SYNC_FOR_CPU, 0, 0));
    for (i = 0; i < FW_BYTES; i++)
        if (mapped[source + i] != firmware[i]) source_errors++;
    if (source_errors) {
        fprintf(stderr, "Source CPU readback differs in %u bytes; no copy submitted\n", source_errors);
        goto out;
    }
    CALL(IOConnectMethodScalarIScalarO(connection, RDN_UC_ALLOC, 2, 1,
        DEST_BYTES, 4096, &destination));
    CALL(IOConnectMethodScalarIScalarO(connection, RDN_UC_ALLOC, 2, 1,
        IB_BYTES, 4096, &ib));
    if (!valid_storage(source, diagnostic.reserved_bytes, destination, ib,
                       info.aperture_size)) {
        fprintf(stderr, "Invalid or overlapping allocation ranges\n"); goto out;
    }
    base = ((uint64_t)info.vram_gpu_base_hi << 32) | info.vram_gpu_base_lo;
    if (base >= ((uint64_t)1 << 40) ||
        info.aperture_size > ((uint64_t)1 << 40) - base) goto out;
    source_gpu = base + source;
    destination_gpu = base + destination + GUARD_BYTES;
    if (!make_copy(words, source_gpu, destination_gpu)) goto out;
    copied = mapped + destination;
    for (i = 0; i < DEST_BYTES; i++) copied[i] = 0xa5;
    commands = (volatile uint32_t *)(mapped + ib);
    for (i = 0; i < COPY_WORDS; i++) commands[i] = words[i];
    __asm__ volatile("sync" ::: "memory");
    kr = IOConnectMethodScalarIScalarO(connection, RDN_UC_SUBMIT,
        2, 1, ib, COPY_WORDS, &fence);
    if (kr) {
        /* SUBMIT may have queued the IB before its fence allocation failed. */
        printf("{\"status\":\"submission_uncertain_allocations_retained\",\"pid\":%u,\"error\":%u}\n",
            (unsigned)getpid(), (unsigned)kr);
        signal(SIGINT, SIG_IGN); signal(SIGTERM, SIG_IGN);
        for (;;) pause();
    }
    CALL(IOConnectMethodScalarIScalarO(connection, RDN_UC_FENCE_WAIT,
        2, 1, fence, 3000, &done));
    if (!done) {
        fprintf(stderr, "Fresh fence %u timed out; no destination read\n", fence);
        goto out;
    }
    CALL(IOConnectMethodScalarIScalarO(connection, RDN_UC_SYNC_FOR_CPU, 0, 0));
    for (i = 0; i < DEST_BYTES; i++) {
        if (i >= GUARD_BYTES && i < GUARD_BYTES + FW_BYTES) {
            if (copied[i] != firmware[i - GUARD_BYTES]) {
                if (!mismatches) first_mismatch = i - GUARD_BYTES;
                mismatches++;
            }
        } else if (copied[i] != 0xa5) guard_errors++;
    }
    for (i = 0; i < FW_BYTES; i++)
        if (mapped[source + i] != firmware[i]) source_errors++;
    result = (mismatches || guard_errors || source_errors) ? 1 : 0;
    printf("{\"status\":\"%s\",\"bytes\":%u,\"source_offset\":%u,"
        "\"source_gpu\":\"0x%010llx\",\"destination_gpu\":\"0x%010llx\","
        "\"destination_offset\":%u,\"ib_offset\":%u,\"fence\":%u,"
        "\"mismatches\":%u,\"first_mismatch\":%u,\"guard_errors\":%u,\"source_errors\":%u}\n",
        result ? "fail" : "pass", FW_BYTES, source,
        (unsigned long long)source_gpu, (unsigned long long)destination_gpu,
        destination, ib, fence, mismatches, first_mismatch, guard_errors, source_errors);
out:
    /* Client teardown waits for the latest graphics fence, retaining storage on timeout. */
    if (connection) IOServiceClose(connection);
    if (service) IOObjectRelease(service);
    if (file) fclose(file);
    free(firmware);
    return result;
}
#endif
