/*
 * Shared range-allocator API tests and an independent byte occupancy model.
 * Copyright (c) 2026 kouta-kun and Claude
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hw/rdn_mem.h"

#define BASE 0x1003u
#define BYTES 4096u
#define SLOTS 32u
#define CHECK(x) do { ++checks; if (!(x)) { \
	printf("FAIL line %d, check %u\n", __LINE__, checks); exit(1); } } while (0)

static unsigned checks, calls, fail_at, live;
static struct rdn_os os;

static void *allocate(void *cookie, size_t bytes)
{
	void *p;
	(void)cookie;
	if (++calls == fail_at)
		return NULL;
	p = malloc(bytes);
	if (p) {
		memset(p, 0xa5, bytes);
		++live;
	}
	return p;
}

static void release(void *cookie, void *p)
{
	(void)cookie;
	CHECK(p && live);
	--live;
	free(p);
}

static int take(unsigned api, struct rdn_mem *mem, uint64_t size,
		uint64_t align, uint64_t lo, uint64_t hi, uint64_t *offset)
{
	if (api == 0)
		return rdn_mem_alloc(mem, size, align, offset);
	if (api == 1)
		return rdn_mem_alloc_range(mem, size, align, lo, hi, offset);
	return rdn_mem_alloc_in_range(mem, size, align, lo, hi, offset);
}

static void setup(struct rdn_mem *mem, uint64_t base, uint64_t size)
{
	CHECK(!live);
	calls = fail_at = 0;
	CHECK(!rdn_mem_init(mem, &os, base, size));
}

static void failures(void)
{
	struct rdn_mem mem;
	uint64_t out, big;
	unsigned api, nth;
	for (api = 0; api != 3; ++api) {
		for (nth = 1; nth != 3; ++nth) {
			setup(&mem, BASE, BYTES);
			fail_at = calls + nth;
			out = 0xfeed;
			CHECK(take(api, &mem, 64, 64, BASE + 1, BASE + BYTES, &out));
			CHECK(out == 0xfeed && live == 1);
			CHECK(rdn_mem_available_range(&mem, 0, UINT64_MAX, &big) == BYTES);
			CHECK(big == BYTES && rdn_mem_largest(&mem) == BYTES);
			fail_at = 0;
			CHECK(!take(api, &mem, BYTES, 1, BASE, BASE + BYTES, &out));
			CHECK(out == BASE);
			rdn_mem_free(&mem, out);
			rdn_mem_fini(&mem);
			CHECK(!live);
		}
	}
	setup(&mem, BASE, BYTES);
	out = 0xfeed;
	CHECK(rdn_mem_alloc_in_range(&mem, 0, 1, 0, UINT64_MAX, &out));
	CHECK(rdn_mem_alloc_in_range(&mem, 1, 0, 0, UINT64_MAX, &out));
	CHECK(out == 0xfeed && rdn_mem_available(&mem) == BYTES);
	CHECK(!rdn_mem_alloc(&mem, 0, 0, &out) && out == BASE);
	rdn_mem_free(&mem, out);
	CHECK(!rdn_mem_alloc_range(&mem, 0, 0, BASE + 1, BASE + 2, &out));
	CHECK(out == BASE + 1);
	rdn_mem_free(&mem, out);
	for (api = 0; api != 3; ++api) {
		out = 0xfeed;
		CHECK(take(api, &mem, 1, 3, 0, UINT64_MAX, &out));
		CHECK(out == 0xfeed && rdn_mem_available(&mem) == BYTES);
		CHECK(take(api, &mem, 1, 1, 0, UINT64_MAX, NULL));
		CHECK(take(api, NULL, 1, 1, 0, UINT64_MAX, &out));
	}
	big = 0xfeed;
	CHECK(!rdn_mem_available_range(&mem, BASE + 2, BASE + 1, &big) && !big);
	CHECK(!rdn_mem_available_range(NULL, 0, UINT64_MAX, &big) && !big);
	rdn_mem_fini(&mem);
	CHECK(!live);
	CHECK(rdn_mem_init(&mem, &os, UINT64_MAX, 1));
	CHECK(!mem.head && !live);
	rdn_mem_fini(&mem);
	setup(&mem, UINT64_MAX - 4095, 4095);
	for (api = 0; api != 3; ++api) {
		out = 0xfeed;
		CHECK(take(api, &mem, 1, 8192, 0, UINT64_MAX, &out));
		CHECK(out == 0xfeed);
		CHECK(!take(api, &mem, 4095, 1, 0, UINT64_MAX, &out));
		CHECK(out == UINT64_MAX - 4095);
		rdn_mem_free(&mem, out);
	}
	rdn_mem_fini(&mem);
	setup(&mem, BASE, 0);
	CHECK(rdn_mem_alloc(&mem, 0, 0, &out));
	CHECK(!rdn_mem_available(&mem) && !rdn_mem_largest(&mem));
	rdn_mem_fini(&mem);
	CHECK(!live);
}

static uint32_t random_next(uint32_t *seed)
{
	*seed = *seed * 1664525u + 1013904223u;
	return *seed;
}

static void free_space(const unsigned char *used, uint64_t lo, uint64_t hi,
		       uint64_t *total, uint64_t *largest)
{
	unsigned i, run = 0;
	*total = *largest = 0;
	for (i = 0; i != BYTES; ++i) {
		if (!used[i] && BASE + i >= lo && BASE + i < hi) {
			++*total;
			if (++run > *largest)
				*largest = run;
		} else {
			run = 0;
		}
	}
}

static void model(void)
{
	struct rdn_mem mem;
	struct { uint64_t offset, size; int active; } slots[SLOTS];
	unsigned char used[BYTES];
	uint32_t seed = 0x12345678;
	unsigned step, slot, i;
	memset(used, 0, sizeof(used));
	memset(slots, 0, sizeof(slots));
	setup(&mem, BASE, BYTES);
	for (step = 0; step != 12000; ++step) {
		uint64_t size, align, lo, hi, expected, out, total, largest, big;
		unsigned api = random_next(&seed) % 3;
		int result;
		slot = random_next(&seed) % SLOTS;
		if (slots[slot].active) {
			rdn_mem_free(&mem, slots[slot].offset);
			memset(used + slots[slot].offset - BASE, 0, (size_t)slots[slot].size);
			slots[slot].active = 0;
		}
		size = random_next(&seed) % 301;
		align = random_next(&seed) % 11;
		align = align == 10 ? 3 : align == 0 ? 0 : 1u << (align - 1);
		lo = BASE - 128 + random_next(&seed) % (BYTES + 256);
		hi = BASE - 128 + random_next(&seed) % (BYTES + 256);
		if (api == 0) { lo = BASE; hi = BASE + BYTES; }
		expected = UINT64_MAX;
		if ((api != 2 || (size && align)) && !(align && (align & (align - 1)))) {
			uint64_t n = size ? size : 1, alignment = align ? align : 1;
			for (i = 0; i < BYTES; ++i) {
				unsigned j;
				uint64_t address = BASE + i;
				if (address < lo || address >= hi || address % alignment ||
				    n > hi - address || n > BYTES - i)
					continue;
				for (j = 0; j < n && !used[i + j]; ++j) {}
				if (j == n) { expected = address; break; }
			}
		}
		out = 0xfeed;
		result = take(api, &mem, size, align, lo, hi, &out);
		CHECK((result == 0) == (expected != UINT64_MAX));
		CHECK(out == (expected != UINT64_MAX ? expected : 0xfeed));
		if (!result) {
			slots[slot].active = 1;
			slots[slot].offset = out;
			slots[slot].size = size ? size : 1;
			memset(used + out - BASE, 1, (size_t)slots[slot].size);
		}
		free_space(used, 0, UINT64_MAX, &total, &largest);
		CHECK(rdn_mem_available(&mem) == total && rdn_mem_largest(&mem) == largest);
		free_space(used, lo, hi, &total, &largest);
		CHECK(rdn_mem_available_range(&mem, lo, hi, &big) == total && big == largest);
		CHECK(rdn_mem_available_range(&mem, lo, hi, NULL) == total);
	}
	for (slot = 0; slot != SLOTS; ++slot)
		if (slots[slot].active)
			rdn_mem_free(&mem, slots[slot].offset);
	CHECK(rdn_mem_available(&mem) == BYTES && rdn_mem_largest(&mem) == BYTES);
	rdn_mem_fini(&mem);
	CHECK(!live);
}

int main(void)
{
	memset(&os, 0, sizeof(os));
	os.alloc = allocate;
	os.free = release;
	failures();
	model();
	printf("PASS allocator compatibility: %u checks, 12000 mixed-API operations\n", checks);
	return 0;
}
