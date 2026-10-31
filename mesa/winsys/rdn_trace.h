#ifndef RDN_TRACE_H
#define RDN_TRACE_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdbool.h>

enum rdn_trace_mode {
   RDN_TRACE_CP,
   RDN_TRACE_COMPLETE,
   RDN_TRACE_RETIRE
};

/* Retire markers track pipeline completion without flushing caches or waiting. */
static inline size_t rdn_trace_words(const uint32_t *src, size_t count, enum rdn_trace_mode mode)
{
   size_t pos = 0, extra = 0;

   while (pos < count) {
      uint32_t h = src[pos], kind = h >> 30;
      size_t size = kind == 2 ? 1 : ((h >> 16) & 0x3fff) + 2;

      if (kind == 1 || size > count - pos)
         return 0;
      if (kind == 3 && (((h >> 8) & 255) == 0x2b || ((h >> 8) & 255) == 0x2d))
         extra += mode == RDN_TRACE_COMPLETE ? 13 : mode == RDN_TRACE_RETIRE ? 6 : 5;
      pos += size;
   }
   if (extra > SIZE_MAX - count - 7)
      return 0;
   return (count + extra + 7) & ~(size_t)7;
}

static inline size_t rdn_trace_encode(const uint32_t *src, size_t count,
                                    uint32_t *dst, size_t capacity, uint64_t address,
                                    enum rdn_trace_mode mode)
{
   size_t need = rdn_trace_words(src, count, mode), pos = 0, out = 0;
   uint32_t marker = 0;

   if (!need || need > capacity || (address & 3) || address >> 40)
      return 0;
   while (pos < count) {
      uint32_t h = src[pos], kind = h >> 30;
      size_t size = kind == 2 ? 1 : ((h >> 16) & 0x3fff) + 2;

      memcpy(dst + out, src + pos, size * 4);
      out += size;
      pos += size;
      if (kind == 3 && (((h >> 8) & 255) == 0x2b || ((h >> 8) & 255) == 0x2d)) {
         if (mode != RDN_TRACE_CP) {
            dst[out++] = 0xc0044700;
            /* BOTTOM_OF_PIPE_TS or CACHE_FLUSH_AND_INV_TS_EVENT, index 5. */
            dst[out++] = mode == RDN_TRACE_RETIRE ? 0x528 : 0x514;
            dst[out++] = (uint32_t)address;
            dst[out++] = (uint32_t)(address >> 32) | (1u << 29);
            dst[out++] = ++marker;
            dst[out++] = 0;
            if (mode == RDN_TRACE_RETIRE)
               continue;
            /* Complete mode waits before the CP can issue another draw. */
            dst[out++] = 0xc0053c00;
            dst[out++] = 0x13; /* WAIT_REG_MEM: memory, equal */
            dst[out++] = (uint32_t)address;
            dst[out++] = (uint32_t)(address >> 32);
            dst[out++] = marker;
            dst[out++] = 0xffffffff;
            dst[out++] = 10;
            continue;
         }
         dst[out++] = 0xc0033d00;
         dst[out++] = (uint32_t)address;
         dst[out++] = (uint32_t)(address >> 32) | (1u << 17) | (1u << 18);
         dst[out++] = ++marker;
         dst[out++] = 0;
      }
   }
   while (out < need)
      dst[out++] = 0x80000000;
   return out;
}

#endif
