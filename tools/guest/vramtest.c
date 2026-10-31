/* GPU VRAM integrity test. Build with -Ihw -framework IOKit -framework CoreFoundation. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <mach/mach.h>
#include <IOKit/IOKitLib.h>
#include "rdn_user.h"

#define MIB (1u << 20)
#define MAX_BLOCKS 16384
struct block { uint32_t offset, size; };
static struct block blocks[MAX_BLOCKS];
static io_connect_t conn;
static uint32_t ib_offset;
static volatile uint32_t *ib;

static int allocate(unsigned method, uint32_t bytes, uint32_t *offset)
{
	int value;
	if (IOConnectMethodScalarIScalarO(conn, method, 2, 1, bytes, 4096, &value))
		return -1;
	*offset = (uint32_t)value;
	return 0;
}

static int copy_gpu(uint64_t dst, uint64_t src, uint32_t bytes)
{
	int fence = 0, reached = 0;
	unsigned i;
	ib[0] = 0xc0044100; /* CP_DMA, CP_SYNC waits for the copy. */
	ib[1] = (uint32_t)src;
	ib[2] = (uint32_t)(src >> 32) | 0x80000000u;
	ib[3] = (uint32_t)dst;
	ib[4] = (uint32_t)(dst >> 32);
	ib[5] = bytes;
	for (i = 6; i < 16; ++i) ib[i] = 0x80000000;
	__sync_synchronize();
	if (IOConnectMethodScalarIScalarO(conn, RDN_UC_SUBMIT, 2, 1,
		ib_offset, 16, &fence) ||
	    IOConnectMethodScalarIScalarO(conn, RDN_UC_FENCE_WAIT, 2, 1,
		fence, 5000, &reached) || !reached) {
		fprintf(stderr, "GPU copy failed: fence=%u reached=%d\n", (unsigned)fence, reached);
		return -1;
	}
	__sync_synchronize();
	return 0;
}

static int temperature_ok(void)
{
	int core, memory, temperature;
	if (IOConnectMethodScalarIScalarO(conn, RDN_UC_POWER, 2, 3, 0, 0,
		&core, &memory, &temperature)) return 0;
	printf("temperature=%.1f C\n", (temperature - RDN_UC_TEMPERATURE_BIAS) / 1000.0);
	return temperature - RDN_UC_TEMPERATURE_BIAS < 85000;
}

static uint32_t pattern(uint32_t address, uint32_t seed)
{
	return (address / 4) * 0x9e3779b9u ^ seed;
}

int main(int argc, char **argv)
{
	struct rdn_user_info info;
	IOByteCount info_size = sizeof(info);
	io_service_t service;
	vm_address_t aperture = 0, staging = 0;
	vm_size_t aperture_size = 0;
	uint32_t guard, offset, n = 0, i, j, pos, bytes, *data;
	uint64_t base, total = 0, highest = 0, limit = UINT64_MAX, done;
	int hidden_offset, hidden_size, gart_on, gart_base, gart_pages, gart_offset;
	unsigned heap, pass, verify;
	if (argc == 3 && !strcmp(argv[1], "--limit-mib")) {
		char *end;
		unsigned long value = strtoul(argv[2], &end, 10);
		if (*end || !value || value > 2048) return 2;
		limit = (uint64_t)value * MIB;
	} else if (argc != 1) {
		fprintf(stderr, "usage: vramtest [--limit-mib 1..2048]\n"); return 2;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	service = IOServiceGetMatchingService(0, IOServiceMatching(RDN_UC_SERVICE_CLASS));
	if (!service || IOServiceOpen(service, mach_task_self(), RDN_UC_TYPE_V2, &conn)) return 1;
	IOObjectRelease(service);
	if (IOConnectMethodScalarIStructureO(conn, RDN_UC_GET_INFO, 0, &info_size, &info) ||
	    IOConnectMethodScalarIScalarO(conn, RDN_UC_HIDDEN_INFO, 0, 2, &hidden_offset, &hidden_size) ||
	    IOConnectMethodScalarIScalarO(conn, RDN_UC_GART_INFO, 0, 3, &gart_on, &gart_base, &gart_pages) || !gart_on)
		goto fail;
	base = ((uint64_t)info.vram_gpu_base_hi << 32) | info.vram_gpu_base_lo;
	printf("PCI=%04x GPU_base=%llx aperture=%u visible_heap=%u hidden_offset=%u hidden_size=%u\n",
		info.pci_device_id, (unsigned long long)base, info.aperture_size, info.heap_size,
		(unsigned)hidden_offset, (unsigned)hidden_size);
	if (IOConnectMapMemory(conn, RDN_UC_MEMORY_APERTURE, mach_task_self(),
		&aperture, &aperture_size, kIOMapAnywhere | kIOMapWriteCombineCache) ||
	    allocate(RDN_UC_ALLOC, 4096, &ib_offset) ||
	    vm_allocate(mach_task_self(), &staging, MIB, VM_FLAGS_ANYWHERE) ||
	    IOConnectMethodScalarIScalarO(conn, RDN_UC_GART_BIND, 2, 1, staging, MIB, &gart_offset) ||
	    allocate(RDN_UC_ALLOC, 32 * MIB, &guard)) goto fail;
	ib = (volatile uint32_t *)(aperture + ib_offset);
	data = (uint32_t *)staging;
	/* Hold display headroom while discovering all remaining free ranges. */
	for (heap = 0; heap < 2; ++heap) {
		uint64_t low = heap ? info.heap_offset : (uint32_t)hidden_offset;
		uint64_t high = low + (heap ? info.heap_size : (uint32_t)hidden_size);
		unsigned method = heap ? RDN_UC_ALLOC : RDN_UC_ALLOC_HIDDEN;
		bytes = 16 * MIB;
		while (total < limit && n < MAX_BLOCKS) {
			if (limit - total < bytes) bytes = (uint32_t)(limit - total);
			if (allocate(method, bytes, &offset)) {
				if (bytes == 4096) break;
				bytes = bytes > MIB ? MIB : 4096;
				continue;
			}
			if (offset < low || (uint64_t)offset + bytes > high) goto fail;
			for (i = 0; i < n; ++i)
				if ((uint64_t)offset < (uint64_t)blocks[i].offset + blocks[i].size &&
				    (uint64_t)blocks[i].offset < (uint64_t)offset + bytes) goto fail;
			blocks[n].offset = offset; blocks[n++].size = bytes;
			total += bytes;
			if ((uint64_t)offset + bytes > highest) highest = (uint64_t)offset + bytes;
			printf("range=%08x bytes=%u heap=%s\n", offset, bytes, heap ? "visible" : "hidden");
		}
	}
	if (IOConnectMethodScalarIScalarO(conn, RDN_UC_FREE, 1, 0, guard) || !n || n == MAX_BLOCKS) goto fail;
	printf("allocated=%llu bytes blocks=%u highest_end=%llx reserved=%u headroom=%u unavailable_heap=%llu\n",
		(unsigned long long)total, n, (unsigned long long)highest, info.heap_offset, 32*MIB,
		(unsigned long long)((uint64_t)info.heap_size + (uint32_t)hidden_size - total - 32*MIB - 4096));
	/* Write every allocation before reading any, so aliases cannot hide. */
	for (pass = 0; pass < 2; ++pass) for (verify = 0; verify < 2; ++verify) {
		uint32_t seed = pass ? 0xa5a5a5a5u : 0x5a5a5a5au;
		done = 0;
		for (i = 0; i < n; ++i) for (pos = 0; pos < blocks[i].size; pos += bytes) {
			offset = blocks[i].offset + pos;
			bytes = blocks[i].size - pos;
			if (bytes > MIB) bytes = MIB;
			if (!(done % (128*MIB)) && !temperature_ok()) goto fail;
			for (j = 0; j < bytes/4; ++j)
				data[j] = verify ? ~pattern(offset + j*4, seed) : pattern(offset + j*4, seed);
			if (copy_gpu(verify ? (uint32_t)gart_base + (uint64_t)(uint32_t)gart_offset : base + offset,
				verify ? base + offset : (uint32_t)gart_base + (uint64_t)(uint32_t)gart_offset, bytes)) goto fail;
			if (verify) for (j = 0; j < bytes/4; ++j) {
				uint32_t expected = pattern(offset + j*4, seed);
				if (data[j] != expected) {
					fprintf(stderr, "MISMATCH offset=%08x expected=%08x actual=%08x\n", offset+j*4, expected, data[j]);
					goto fail;
				}
			}
			done += bytes;
		}
		printf("pass=%u operation=%s bytes=%llu OK\n", pass, verify ? "verify" : "write", (unsigned long long)done);
	}
	printf("PASS bytes=%llu patterns=2 highest_end=%llx\n", (unsigned long long)total, (unsigned long long)highest);
	IOServiceClose(conn);
	return 0;
fail:
	fprintf(stderr, "FAIL: VRAM validation incomplete\n");
	/* Client teardown retains GPU-visible memory if its fence cannot retire. */
	IOServiceClose(conn);
	return 1;
}
