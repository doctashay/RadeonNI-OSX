#include <assert.h>
#include <stdio.h>
#include "../mesa/winsys/rdn_trace.h"

int main(void)
{
   uint32_t src[] = { 0xc0032b00, 0x123400, 0xf, 3, 0,
                      0xc0012d00, 3, 2, 0x80000000 };
   uint32_t dst[40];
   uint32_t truncated[] = { 0xc0032b00, 0 };
   size_t count = sizeof(src) / sizeof(src[0]);

   assert(rdn_trace_words(src, count, false) == 24);
   assert(rdn_trace_encode(src, count, dst, 24, 0xf025c1000ull, false) == 24);
   assert(!memcmp(src, dst, 5 * 4));
   assert(dst[5] == 0xc0033d00 && dst[6] == 0x025c1000);
   assert(dst[7] == 0x6000f && dst[8] == 1 && dst[9] == 0);
   assert(!memcmp(src + 5, dst + 10, 3 * 4));
   assert(dst[13] == 0xc0033d00 && dst[16] == 2);
   assert(dst[23] == 0x80000000);
   assert(!rdn_trace_encode(src, count, dst, 23, 0xf025c1000ull, false));
   assert(!rdn_trace_encode(src, count, dst, 24, 0xf025c1001ull, false));
   assert(!rdn_trace_encode(src, count, dst, 24, 1ull << 40, false));
   assert(!rdn_trace_words(truncated, 2, false));
   assert(rdn_trace_words(src, count, true) == 40);
   assert(rdn_trace_encode(src, count, dst, 40, 0xf025c1000ull, true) == 40);
   assert(dst[5] == 0xc0044700 && dst[6] == 0x514);
   assert(dst[7] == 0x025c1000 && dst[8] == 0x2000000f && dst[9] == 1);
   assert(dst[11] == 0xc0053c00 && dst[12] == 0x13);
   assert(dst[13] == dst[7] && dst[14] == 0xf && dst[15] == 1);
   assert(dst[16] == 0xffffffff && dst[17] == 10);
   assert(!memcmp(src + 5, dst + 18, 3 * 4));
   assert(dst[21] == 0xc0044700 && dst[25] == 2);
   assert(dst[27] == 0xc0053c00 && dst[31] == 2);
   assert(!rdn_trace_encode(src, count, dst, 39, 0xf025c1000ull, true));
   puts("PASS trace packets");
   return 0;
}
