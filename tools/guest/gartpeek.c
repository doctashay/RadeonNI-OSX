/* Read GART-backed process memory without submitting work or stopping the task. */
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mesa/winsys/rdn_device.h"

#if !defined(__ppc__) || defined(__ppc64__)
#error Build this inspector for the matching 32-bit PowerPC driver.
#endif

/* Verified in the 2026-10-29 retire driver: rdn_cs_flush and dev_gart_cpu. */
enum { WS_DEVICE = 0xb10, DEVICE_GART = 0x1d8, GART_COUNT = 256 };
struct remote_chunk {
    uint32_t cpu, offset, size;
    uint32_t allocator[6];
};
typedef char chunk_stride_check[sizeof(struct remote_chunk) == 0x24 ? 1 : -1];

static int read_remote(task_t task, uint64_t address, void *data, size_t bytes)
{
    mach_vm_size_t got = 0;
    kern_return_t result;
    if (!address || address > UINT32_MAX || bytes > UINT32_MAX - address)
        return 0;
    result = mach_vm_read_overwrite(task, address, bytes,
                                   (mach_vm_address_t)(uintptr_t)data, &got);
    if (result != KERN_SUCCESS || got != bytes) {
        fprintf(stderr, "read 0x%08" PRIx64 " + %lu: %s, got %llu\n",
                address, (unsigned long)bytes, mach_error_string(result),
                (unsigned long long)got);
        return 0;
    }
    return 1;
}

static int number(const char *text, uint64_t *value)
{
    char *end;
    if (!*text || *text == '-') return 0;
    errno = 0;
    *value = strtoull(text, &end, 0);
    return !errno && !*end;
}

int main(int argc, char **argv)
{
    struct remote_chunk chunks[GART_COUNT], after;
    struct rdn_device_info info;
    task_t task = MACH_PORT_NULL;
    uint64_t pid, cs, gpu, length;
    uint32_t ws, device;
    kern_return_t result;
    int status = 1, arg;

    if (argc < 6 || argc > 12 || (argc - 3) % 3 ||
        !number(argv[1], &pid) || !pid || pid > INT_MAX ||
        !number(argv[2], &cs) || !cs || cs > UINT32_MAX || (cs & 3)) {
        fprintf(stderr, "usage: gartpeek PID CS GPU_ADDRESS BYTES OUTFILE [GPU_ADDRESS BYTES OUTFILE ...]\n");
        return 2;
    }
    result = task_for_pid(mach_task_self(), (int)pid, &task);
    if (result != KERN_SUCCESS) {
        fprintf(stderr, "task_for_pid: %s\n", mach_error_string(result));
        return 1;
    }
    if (!read_remote(task, cs, &ws, sizeof(ws)) || (ws & 3) ||
        !read_remote(task, (uint64_t)ws + WS_DEVICE, &device, sizeof(device)) ||
        (device & 3) || !read_remote(task, device, &info, sizeof(info)) ||
        !read_remote(task, (uint64_t)device + DEVICE_GART, chunks, sizeof(chunks)))
        goto done;
    if (info.pci_device_id != 0x6704 || !info.gart_gpu_base || !info.gart_size) {
        fprintf(stderr, "device/layout mismatch: pci=%04x gart=0x%llx size=%llu\n",
                info.pci_device_id, (unsigned long long)info.gart_gpu_base,
                (unsigned long long)info.gart_size);
        goto done;
    }
    printf("pid=%llu cs=0x%08llx ws=0x%08x device=0x%08x gart=0x%llx layout=b10/1d8/24\n",
           (unsigned long long)pid, (unsigned long long)cs, ws, device,
           (unsigned long long)info.gart_gpu_base);
    for (arg = 3; arg < argc; arg += 3) {
        uint64_t offset, cpu = 0;
        unsigned index, matches = 0, selected = 0;
        unsigned char *data;
        FILE *file;
        if (!number(argv[arg], &gpu) || !number(argv[arg + 1], &length) ||
            !length || length > 65536 || gpu < info.gart_gpu_base ||
            (offset = gpu - info.gart_gpu_base) > UINT32_MAX ||
            length > UINT32_MAX - offset) {
            fprintf(stderr, "invalid GART read range\n");
            goto done;
        }
        for (index = 0; index < GART_COUNT; ++index) {
            const struct remote_chunk *chunk = &chunks[index];
            if (!chunk->cpu || (chunk->cpu & 4095) || !chunk->size ||
                (chunk->offset & 4095) || (chunk->size & 4095) ||
                offset < chunk->offset || offset - chunk->offset > chunk->size ||
                length > chunk->size - (offset - chunk->offset))
                continue;
            selected = index;
            cpu = chunk->cpu + offset - chunk->offset;
            ++matches;
        }
        if (matches != 1 || cpu > UINT32_MAX || length > UINT32_MAX - cpu) {
            fprintf(stderr, "GART range has %u matching chunks\n", matches);
            goto done;
        }
        data = malloc((size_t)length);
        if (!data) goto done;
        if (!read_remote(task, cpu, data, (size_t)length) ||
            !read_remote(task, (uint64_t)device + DEVICE_GART + selected * sizeof(after),
                         &after, sizeof(after)) ||
            memcmp(&after, &chunks[selected], 3 * sizeof(uint32_t))) {
            fprintf(stderr, "read failed or GART mapping changed\n");
            free(data);
            goto done;
        }
        file = fopen(argv[arg + 2], "wb");
        if (!file) { perror(argv[arg + 2]); free(data); goto done; }
        if (fwrite(data, 1, (size_t)length, file) != length) {
            fprintf(stderr, "short output write\n");
            fclose(file); free(data); goto done;
        }
        if (fclose(file)) { perror("close output"); free(data); goto done; }
        printf("gpu=0x%llx cpu=0x%08llx bytes=%llu chunk=%u saved=%s\n",
               (unsigned long long)gpu, (unsigned long long)cpu,
               (unsigned long long)length, selected, argv[arg + 2]);
        free(data);
    }
    status = 0;
done:
    mach_port_deallocate(mach_task_self(), task);
    return status;
}
