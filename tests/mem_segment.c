#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../hw/rdn_mem.h"

static unsigned calls, fail_at, live;
static void *allocate(void *cookie, size_t bytes)
{
 void *p;
 (void)cookie;
 if (++calls == fail_at) return NULL;
 p = calloc(1, bytes);
 if (p) ++live;
 return p;
}
static void release(void *cookie, void *p)
{
 (void)cookie;
 assert(p && live); --live; free(p);
}
static struct rdn_os os;
static void setup(struct rdn_mem *mem, uint64_t base, uint64_t size)
{
 calls = fail_at = 0;
 assert(!live);
 assert(!rdn_mem_init(mem, &os, base, size));
}
int main(void)
{
 struct rdn_mem mem;
 uint64_t out, a, b, saved;
 unsigned i;
 memset(&os, 0, sizeof(os)); os.alloc = allocate; os.free = release;
 for (i = 0; i < 2; ++i) {
  setup(&mem, 0x1000, 0x10000);
  fail_at = calls + 1 + i; out = 0xfeed;
  assert(rdn_mem_alloc_in_range(&mem, 0x1000, 0x1000, 0x3000, 0xf000, &out));
  assert(out == 0xfeed && live == 1);
  assert(rdn_mem_available(&mem) == 0x10000 && rdn_mem_largest(&mem) == 0x10000);
  fail_at = 0;
  assert(!rdn_mem_alloc_in_range(&mem, 0x10000, 1, 0x1000, 0x11000, &out) && out == 0x1000);
  rdn_mem_free(&mem, out); rdn_mem_fini(&mem); assert(!live);
 }
 setup(&mem, 0x1003, 0x2ffd);
 out = 0xfeed;
 assert(!rdn_mem_alloc_in_range(&mem, 0x1000, 0x1000, 0x1004, 0x3000, &out) && out == 0x2000);
 assert(rdn_mem_available(&mem) == 0x1ffd && rdn_mem_largest(&mem) == 0x1000);
 rdn_mem_free(&mem, out); assert(rdn_mem_largest(&mem) == 0x2ffd);
 saved = rdn_mem_available(&mem);
 assert(rdn_mem_alloc_in_range(&mem, 0x1001, 0x1000, 0x2000, 0x3000, &out));
 assert(rdn_mem_alloc_in_range(&mem, 1, 3, 0, UINT64_MAX, &out));
 assert(rdn_mem_alloc_in_range(&mem, 0, 1, 0, UINT64_MAX, &out));
 assert(rdn_mem_alloc_in_range(&mem, 1, 1, 0x3000, 0x3000, &out));
 assert(rdn_mem_available(&mem) == saved); rdn_mem_fini(&mem); assert(!live);
 setup(&mem, UINT64_MAX - 4095, 4095);
 out = 0xfeed;
 assert(rdn_mem_alloc_in_range(&mem, 4095, 8192, UINT64_MAX - 4095, UINT64_MAX, &out));
 assert(out == 0xfeed && rdn_mem_available(&mem) == 4095);
 assert(!rdn_mem_alloc_in_range(&mem, 4095, 1, UINT64_MAX - 4095, UINT64_MAX, &out));
 rdn_mem_free(&mem, out); rdn_mem_fini(&mem); assert(!live);
 setup(&mem, 0x18000000, 0x18000000);
 assert(!rdn_mem_alloc_in_range(&mem, 0x6000000, 4096, 0x19000000, 0x20000000, &a));
 out = 0xfeed;
 assert(rdn_mem_alloc_in_range(&mem, 0x5000000, 4096, 0x18000000, 0x20000000, &out));
 assert(out == 0xfeed);
 assert(!rdn_mem_alloc_in_range(&mem, 0x5000000, 4096, 0x20000000, 0x30000000, &b));
 assert(b == 0x20000000 && a + 0x6000000 <= b);
 rdn_mem_free(&mem, a); rdn_mem_free(&mem, b);
 assert(rdn_mem_available(&mem) == 0x18000000 && rdn_mem_largest(&mem) == 0x18000000);
 rdn_mem_fini(&mem); assert(!live);
 puts("PASS constrained allocator: OOM atomicity, bounds, alignment, overflow, fragmentation and later segment");
 return 0;
}
