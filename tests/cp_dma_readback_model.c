/* CP DMA byte-copy and checked GART flush model. SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "../hw/rdn_cp_dma_readback.h"
#include "../hw/rdn_gart.h"

#define CAPACITY (4u * 1024u * 1024u)
#define GPU_LIMIT (1ull << 40)
static unsigned checks, failures;
static uint32_t response, pending_reads, reads, writes, delays, logs;
static uint32_t write_offsets[2], write_values[2];

static void check(int passed, const char *name)
{
    checks++;
    if (!passed) { failures++; printf("FAIL %u: %s\n", checks, name); }
}

static uint32_t read_reg(void *cookie, uint32_t offset)
{
    (void)cookie;
    reads++;
    if (offset != 0x1470) return 0xffffffffu;
    return reads <= pending_reads ? 0 : response << 4;
}
static void write_reg(void *cookie, uint32_t offset, uint32_t value)
{
    (void)cookie;
    if (writes < 2) { write_offsets[writes] = offset; write_values[writes] = value; }
    writes++;
}
static void delay(void *cookie, uint32_t us) { (void)cookie; delays += us; }
static void log_message(void *cookie, enum rdn_log_level level, const char *fmt, va_list ap)
{ (void)cookie; (void)level; (void)fmt; (void)ap; logs++; }
static void reset_flush(uint32_t code, uint32_t pending)
{
    response = code; pending_reads = pending;
    reads = writes = delays = logs = 0;
    memset(write_offsets, 0, sizeof(write_offsets));
    memset(write_values, 0, sizeof(write_values));
}

static void exercise_copy(uint64_t source_base, uint32_t bytes)
{
    struct rdn_cp_dma_span src = { source_base, CAPACITY, 0 };
    struct rdn_cp_dma_span dst = { 0x41000000, CAPACITY, 0 };
    uint8_t output[132], *source = malloc(CAPACITY + 2), *target = malloc(CAPACITY + 2);
    uint32_t words[31], n = 0xfeed, i, cursor, copied = 0, packets = 0;
    int good = 1;
    if (!source || !target) exit(2);
    for (i = 0; i < CAPACITY + 2; i++) source[i] = (uint8_t)(i * 17 + (i >> 9));
    memset(target, 0xa5, CAPACITY + 2); memset(output, 0x5a, sizeof(output));
    check(!rdn_cp_dma_readback_build(&src, &dst, bytes, output + 1, 124, &n),
          "valid byte copy builds into unaligned output");
    check(n >= 13 && n <= 31 && (n - 7) % 6 == 0, "IB size is bounded");
    check(output[0] == 0x5a && output[1 + n * 4] == 0x5a && output[131] == 0x5a,
          "output canaries remain intact");
    memcpy(words, output + 1, n * 4);
    check(words[0] == 0xc0034300u && words[1] == 0x1980003cu &&
          words[2] == 0xffffffffu && words[3] == 0 && words[4] == 10,
          "literal surface synchronization prefix");
    cursor = 5;
    while (cursor + 6 <= n - 2) {
        uint64_t sa = ((uint64_t)(words[cursor + 2] & 255) << 32) | words[cursor + 1];
        uint64_t da = ((uint64_t)words[cursor + 4] << 32) | words[cursor + 3];
        uint32_t count = words[cursor + 5];
        int final = cursor + 6 == n - 2;
        if (words[cursor] != 0xc0044100u || !count || count > 1048576 ||
            sa != src.address + copied || da != dst.address + copied ||
            (words[cursor + 2] & ~255u) != (final ? 0x80000000u : 0) ||
            count > bytes - copied) { good = 0; break; }
        memcpy(target + 1 + copied, source + 1 + copied, count);
        copied += count; cursor += 6; packets++;
    }
    check(good && copied == bytes && packets >= 1 && packets <= 4,
          "packet interpreter copies exact addresses with final-only CP_SYNC and no swaps");
    check(words[n - 2] == 0xc0004200u && words[n - 1] == 0,
          "literal PFP_SYNC_ME suffix follows copies");
    check(!memcmp(source + 1, target + 1, bytes), "all payload bytes preserve endian-sensitive patterns");
    for (i = 1 + bytes; i < CAPACITY + 2; i++) if (target[i] != 0xa5) good = 0;
    check(good && target[0] == 0xa5, "destination bytes outside copy remain untouched");
    free(source); free(target);
}

static void exercise_list(void)
{
    struct rdn_cp_dma_copy copies[16], saved[16];
    uint32_t words[RDN_CP_DMA_READBACK_MAX_WORDS + 2], original[RDN_CP_DMA_READBACK_MAX_WORDS + 2];
    uint32_t count, i, cursor, range, total, syncs;
    memset(copies, 0, sizeof(copies));
    copies[0].source.address = 0x02200000;
    copies[0].destination.address = 0x41000000;
    copies[0].source.bytes = copies[0].destination.bytes = copies[0].bytes = 3342336;
    for (i = 1; i < 13; i++) {
        copies[i].source.address = 0x03000000 + (i - 1) * 4096;
        copies[i].destination.address = 0x41000000 + 3342336 + (i - 1) * 4096;
        copies[i].source.bytes = copies[i].destination.bytes = copies[i].bytes = 4096;
    }
    memset(words, 0x5a, sizeof(words)); count = 0xbeef;
    check(!rdn_cp_dma_readback_build_list(copies, 13, words + 1, 484, &count) && count == 103,
          "1080 target plus twelve guard pages builds one bounded IB");
    check(words[0] == 0x5a5a5a5a && words[104] == 0x5a5a5a5a,
          "multi-range output canaries remain intact");
    cursor = 6; total = syncs = 0;
    for (range = 0; range < 13; range++) {
        uint32_t consumed = 0;
        while (consumed < copies[range].bytes) {
            uint32_t part = words[cursor + 5];
            check(words[cursor] == 0xc0044100u && part && part <= 1048576 &&
                  words[cursor + 1] == copies[range].source.address + consumed &&
                  words[cursor + 3] == copies[range].destination.address + consumed,
                  "each target/guard packet uses its own checked subrange");
            syncs += !!(words[cursor + 2] & 0x80000000u);
            consumed += part; total += part; cursor += 6;
        }
        check(consumed == copies[range].bytes, "complete range copied without extending into neighbors");
    }
    check(syncs == 1 && (words[cursor - 4] & 0x80000000u) && total == 3391488 &&
          words[cursor] == 0xc0004200u && words[cursor + 1] == 0,
          "one final CP_SYNC and PFP_SYNC_ME cover target and all guards");
    /* Sixteen nonempty ranges totaling4MiB reach the nineteen-packet bound. */
    memset(copies, 0, sizeof(copies));
    total = 0;
    for (i = 0; i < 16; i++) {
        uint32_t bytes = i ? 1 : CAPACITY - 15;
        copies[i].source.address = 0x02200000 + total;
        copies[i].destination.address = 0x41000000 + total;
        copies[i].source.bytes = copies[i].destination.bytes = copies[i].bytes = bytes;
        total += bytes;
    }
    check(!rdn_cp_dma_readback_build_list(copies, 16, words + 1, 484, &count) && count == 121,
          "maximum range fragmentation fits exactly484 bytes");
    memcpy(saved, copies, sizeof(saved));
    memset(words, 0x5a, sizeof(words)); memcpy(original, words, sizeof(words)); count = 0xbeef;
#define REJECT_LIST(c,n,o,b,w) do { \
    check(rdn_cp_dma_readback_build_list((c),(n),(o),(b),(w)) == -EINVAL, "bad copy list rejected"); \
    check(!memcmp(words, original, sizeof(words)) && count == 0xbeef, "list failure preserves outputs"); \
} while (0)
    REJECT_LIST(copies, 0, words + 1, 484, &count);
    REJECT_LIST(copies, 17, words + 1, 484, &count);
    REJECT_LIST(NULL, 1, words + 1, 484, &count);
    REJECT_LIST((void *)(UINTPTR_MAX - 8), 16, words + 1, 484, &count);
    REJECT_LIST(copies, 16, words + 1, 483, &count);
    REJECT_LIST(copies, 16, copies, sizeof(copies), &count);
    REJECT_LIST(copies, 16, words + 1, 484, (uint32_t *)copies);
    copies[0].reserved = 1; REJECT_LIST(copies, 16, words + 1, 484, &count);
    memcpy(copies, saved, sizeof(saved)); copies[0].source.bytes++;
    copies[0].destination.bytes++; copies[0].bytes++;
    REJECT_LIST(copies, 16, words + 1, 484, &count);
    memcpy(copies, saved, sizeof(saved)); copies[1].destination.address = copies[0].destination.address;
    REJECT_LIST(copies, 16, words + 1, 484, &count);
    memcpy(copies, saved, sizeof(saved)); copies[1].source.address = copies[0].destination.address;
    REJECT_LIST(copies, 16, words + 1, 484, &count);
#undef REJECT_LIST
}

int main(void)
{
    const uint32_t sizes[] = { 1, 3, 4, 4095, 4096, 622080, 1048575, 1048576,
                              1048577, 1382400, 3133440, CAPACITY };
    const uint32_t literal[13] = { 0xc0034300u, 0x1980003cu, 0xffffffffu, 0, 10,
        0xc0044100u, 0x01234000u, 0x8000000fu, 0x41001000u, 0, 16, 0xc0004200u, 0 };
    struct rdn_cp_dma_span src = { 0xf01234000ull, CAPACITY, 0 };
    struct rdn_cp_dma_span dst = { 0x41001000, CAPACITY, 0 }, bad;
    struct rdn_os os = {0}; struct rdn_card card = {0};
    struct rdn_accel accel = {0}; struct rdn_gart gart = {0};
    uint32_t words[31], before[31], count, i;
    uint8_t alias[256];
    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        exercise_copy(0x22100000, sizes[i]);
        exercise_copy(0xf01234000ull, sizes[i]);
    }
    exercise_list();
    count = 0;
    check(!rdn_cp_dma_readback_build(&src, &dst, 16, words, sizeof(words), &count) &&
          count == 13 && !memcmp(words, literal, sizeof(literal)), "exact known 13-dword high-address packet");
    memset(words, 0x5a, sizeof(words)); memcpy(before, words, sizeof(words)); count = 0xbeef;
#define REJECT(s,d,b,o,c,w) do { \
    check(rdn_cp_dma_readback_build((s),(d),(b),(o),(c),(w)) == -EINVAL, "malformed request rejected"); \
    check(!memcmp(words,before,sizeof(words)) && count == 0xbeef, "failure leaves outputs unchanged"); \
} while (0)
    REJECT(NULL, &dst, 16, words, sizeof(words), &count);
    REJECT(&src, NULL, 16, words, sizeof(words), &count);
    REJECT(&src, &dst, 0, words, sizeof(words), &count);
    REJECT(&src, &dst, CAPACITY + 1, words, sizeof(words), &count);
    REJECT(&src, &dst, 16, NULL, sizeof(words), &count);
    REJECT(&src, &dst, 16, words, 51, &count);
    REJECT(&src, &dst, 16, words, sizeof(words), NULL);
    REJECT(&src, &dst, 16, words, sizeof(words), words);
    REJECT(&src, &dst, 16, &src, sizeof(words), &count);
    REJECT(&src, &dst, 16, words, sizeof(words), (uint32_t *)&src);
    REJECT(&src, &dst, 16, (void *)(UINTPTR_MAX - 8), sizeof(words), &count);
    REJECT((void *)(UINTPTR_MAX - 8), &dst, 16, words, sizeof(words), &count);
    bad = src; bad.reserved = 1; REJECT(&bad, &dst, 16, words, sizeof(words), &count);
    bad = src; bad.bytes = 15; REJECT(&bad, &dst, 16, words, sizeof(words), &count);
    bad = dst; bad.bytes = 15; REJECT(&src, &bad, 16, words, sizeof(words), &count);
    bad = dst; bad.address = src.address + CAPACITY - 1; REJECT(&src, &bad, 16, words, sizeof(words), &count);
    bad = src; bad.address = GPU_LIMIT; REJECT(&bad, &dst, 16, words, sizeof(words), &count);
    bad = src; bad.address = GPU_LIMIT - CAPACITY + 1; REJECT(&bad, &dst, 16, words, sizeof(words), &count);
    bad = dst; bad.address = GPU_LIMIT - CAPACITY + 1; REJECT(&src, &bad, 16, words, sizeof(words), &count);
    memset(alias, 0, sizeof(alias));
    REJECT(&src, &dst, 16, alias + 1, 124, (uint32_t *)(alias + 130));
#undef REJECT
    os.mmio_read32 = read_reg; os.mmio_write32 = write_reg;
    os.delay_us = delay; os.log = log_message; card.os = &os;
    accel.card = &card; gart.accel = &accel;
    reset_flush(1, 0);
    check(!rdn_gart_flush_checked(&gart) && reads == 1 && writes == 2 && !delays && !logs,
          "TLB success reported without waiting");
    check(write_offsets[0] == 0x5480 && write_values[0] == 1 &&
          write_offsets[1] == 0x1470 && write_values[1] == 1,
          "HDP flush precedes exact VM context request");
    reset_flush(1, 3);
    check(!rdn_gart_flush_checked(&gart) && reads == 4 && delays == 3 && writes == 2,
          "pending TLB response waits then succeeds");
    reset_flush(2, 0);
    check(rdn_gart_flush_checked(&gart) == -EIO && reads == 1 && !delays && logs == 1,
          "explicit TLB failure propagates");
    reset_flush(15, 0);
    check(rdn_gart_flush_checked(&gart) == -EIO && reads == 1 && logs == 1,
          "unknown nonzero TLB response fails closed");
    reset_flush(0, 0);
    check(rdn_gart_flush_checked(&gart) == -ETIMEDOUT && reads == 100000 &&
          delays == 100000 && writes == 2 && logs == 1, "TLB timeout is finite and observable");
    reset_flush(1, 99999);
    check(!rdn_gart_flush_checked(&gart) && reads == 100000 && delays == 99999,
          "last bounded TLB poll may succeed");
    reset_flush(1, 0);
    check(rdn_gart_flush_checked(NULL) == -EINVAL && !writes && !reads,
          "null GART rejects without MMIO");
    os.mmio_read32 = NULL;
    check(rdn_gart_flush_checked(&gart) == -EINVAL && !writes && !reads,
          "missing callback rejects without MMIO");
    os.mmio_read32 = read_reg;
    reset_flush(2, 0); rdn_gart_flush(&gart);
    check(writes == 2 && reads == 1, "legacy void wrapper remains callable");
    printf("%s: %u CP DMA/GART checks\n", failures ? "FAIL" : "PASS", checks);
    return failures ? 1 : 0;
}
