#include <assert.h>
#include <stdio.h>
#include "../mesa/winsys/rdn_trace.h"

static void check_packets(const uint32_t *words, size_t count, enum rdn_trace_mode mode)
{
   size_t pos = 0;
   unsigned draws = 0, writes = 0, waits = 0;
   while (pos < count) {
      uint32_t h = words[pos], kind = h >> 30;
      size_t size = kind == 2 ? 1 : ((h >> 16) & 0x3fff) + 2;
      assert(kind != 1 && size <= count - pos);
      if (kind == 3) {
         unsigned op = (h >> 8) & 255;
         if (op == 0x2b || op == 0x2d)
            ++draws;
         else if (op == 0x47) {
            assert(mode != RDN_TRACE_CP && size == 6);
            assert(words[pos + 1] == (mode == RDN_TRACE_RETIRE ? 0x528 : 0x514));
            assert(words[pos + 4] == ++writes);
         } else if (op == 0x3d) {
            assert(mode == RDN_TRACE_CP && size == 5);
            assert(words[pos + 3] == ++writes);
         } else if (op == 0x3c) {
            assert(mode == RDN_TRACE_COMPLETE && size == 7);
            assert(words[pos + 4] == ++waits);
         } else {
            assert(!"unexpected packet");
         }
      }
      pos += size;
   }
   assert(pos == count && draws == 2 && writes == 2);
   assert(waits == (mode == RDN_TRACE_COMPLETE ? 2 : 0));
}

int main(void)
{
   uint32_t src[] = { 0xc0032b00, 0x123400, 0xf, 3, 0,
                      0xc0012d00, 3, 2, 0x80000000 };
   uint32_t dst[40];
   uint32_t truncated[] = { 0xc0032b00, 0 };
   size_t count = sizeof(src) / sizeof(src[0]);

   assert(rdn_trace_words(src, count, RDN_TRACE_CP) == 24);
   assert(rdn_trace_encode(src, count, dst, 24, 0xf025c1000ull, RDN_TRACE_CP) == 24);
   check_packets(dst, 24, RDN_TRACE_CP);
   assert(!memcmp(src, dst, 5 * 4));
   assert(dst[5] == 0xc0033d00 && dst[6] == 0x025c1000);
   assert(dst[7] == 0x6000f && dst[8] == 1 && dst[9] == 0);
   assert(!memcmp(src + 5, dst + 10, 3 * 4));
   assert(dst[13] == 0xc0033d00 && dst[16] == 2);
   assert(dst[23] == 0x80000000);
   assert(!rdn_trace_encode(src, count, dst, 23, 0xf025c1000ull, RDN_TRACE_CP));
   assert(!rdn_trace_encode(src, count, dst, 24, 0xf025c1001ull, RDN_TRACE_CP));
   assert(!rdn_trace_encode(src, count, dst, 24, 1ull << 40, RDN_TRACE_CP));
   assert(!rdn_trace_words(truncated, 2, RDN_TRACE_CP));
   assert(rdn_trace_words(src, count, RDN_TRACE_COMPLETE) == 40);
   assert(rdn_trace_encode(src, count, dst, 40, 0xf025c1000ull, RDN_TRACE_COMPLETE) == 40);
   check_packets(dst, 40, RDN_TRACE_COMPLETE);
   assert(dst[5] == 0xc0044700 && dst[6] == 0x514);
   assert(dst[7] == 0x025c1000 && dst[8] == 0x2000000f && dst[9] == 1);
   assert(dst[11] == 0xc0053c00 && dst[12] == 0x13);
   assert(dst[13] == dst[7] && dst[14] == 0xf && dst[15] == 1);
   assert(dst[16] == 0xffffffff && dst[17] == 10);
   assert(!memcmp(src + 5, dst + 18, 3 * 4));
   assert(dst[21] == 0xc0044700 && dst[25] == 2);
   assert(dst[27] == 0xc0053c00 && dst[31] == 2);
   assert(!rdn_trace_encode(src, count, dst, 39, 0xf025c1000ull, RDN_TRACE_COMPLETE));
   assert(rdn_trace_words(src, count, RDN_TRACE_RETIRE) == 24);
   assert(rdn_trace_encode(src, count, dst, 24, 0xf025c1000ull, RDN_TRACE_RETIRE) == 24);
   check_packets(dst, 24, RDN_TRACE_RETIRE);
   assert(!memcmp(src, dst, 5 * 4));
   assert(dst[5] == 0xc0044700 && dst[6] == 0x528);
   assert(dst[7] == 0x025c1000 && dst[8] == 0x2000000f && dst[9] == 1 && dst[10] == 0);
   assert(!memcmp(src + 5, dst + 11, 3 * 4));
   assert(dst[14] == 0xc0044700 && dst[18] == 2 && dst[19] == 0);
   assert(dst[20] == 0x80000000 && dst[23] == 0x80000000);
   assert(!rdn_trace_encode(src, count, dst, 23, 0xf025c1000ull, RDN_TRACE_RETIRE));
   assert(!rdn_trace_encode(src, count, dst, 24, 0xf025c1001ull, RDN_TRACE_RETIRE));
   assert(!rdn_trace_encode(src, count, dst, 24, 1ull << 40, RDN_TRACE_RETIRE));
   assert(!rdn_trace_words(truncated, 2, RDN_TRACE_RETIRE));
   puts("PASS trace packets");
   return 0;
}
