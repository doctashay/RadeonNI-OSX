/* Nonblocking GFX IB publication. SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <string.h>
#include "../hw/rdn_accel.h"
#include "../hw/rdn_accel_reg.h"

#define AP_WORDS 192u
#define RING_OFFSET 64u
static unsigned checks, failures;
struct model {
    struct rdn_os os;
    struct rdn_card card;
    struct rdn_accel accel;
    uint32_t mem[AP_WORDS], rptr, hw_wptr, seq;
    uint32_t offsets[4], values[4];
    unsigned writes, reads, delays;
};
static void check(int good, const char *name)
{
    checks++;
    if (!good) { failures++; printf("FAIL %u: %s\n", checks, name); }
}
static uint32_t read_reg(void *cookie, uint32_t reg)
{
    struct model *m = cookie;
    if (reg == CP_RB_RPTR) { m->reads++; return m->rptr; }
    if (reg == CP_RB_WPTR) return m->hw_wptr;
    return 0;
}
static void write_reg(void *cookie, uint32_t reg, uint32_t value)
{
    struct model *m = cookie;
    if (m->writes < 4) { m->offsets[m->writes] = reg; m->values[m->writes] = value; }
    m->writes++;
    if (reg == CP_RB_WPTR) m->hw_wptr = value;
}
static void delay(void *cookie, uint32_t us)
{ struct model *m = cookie; (void)us; m->delays++; }
void rdn_gpu_init(struct rdn_accel *accel) { (void)accel; }
static void init(struct model *m, uint32_t words, uint32_t wptr, uint32_t rptr, int swapped)
{
    memset(m, 0, sizeof(*m)); memset(m->mem, 0xa5, sizeof(m->mem));
    m->os.cookie = m; m->os.mmio_read32 = read_reg;
    m->os.mmio_write32 = write_reg; m->os.delay_us = delay;
    m->card.os = &m->os; m->accel.card = &m->card;
    m->accel.aperture = m->mem; m->accel.aperture_size = sizeof(m->mem);
    m->accel.ring_offset = RING_OFFSET; m->accel.ring_words = words;
    m->accel.wptr = wptr; m->accel.ready = true; m->accel.swapped = swapped;
    m->accel.fence_emitted = 0x12345677; m->rptr = rptr; m->hw_wptr = wptr;
    m->seq = 0xdeadbeef;
}
static uint32_t swap32(uint32_t x)
{ return (x >> 24) | ((x >> 8) & 0xff00) | ((x << 8) & 0xff0000) | (x << 24); }
static int exact_commands(const struct model *m, uint32_t start)
{
    uint32_t expected[AP_WORDS], i, mask = m->accel.ring_words - 1;
    uint32_t words[32] = {
        0xc0001800, 1, 0xc0016800, 0x142, 0,
        0xc0023200, 0x12345000, 0x7a, 121,
        0x80000000, 0x80000000, 0x80000000, 0x80000000,
        0x80000000, 0x80000000, 0x80000000,
        0xc0034300, 0x09900000, 0xffffffff, 0, 10,
        0xc0004600, 0x16, 0xc0016800, 0x10, 0x28000,
        0xc0016800, 0x141, 0x12345678, 0x80000000, 0x80000000, 0x80000000
    };
    words[4] = ((start + 2) & mask) + 7;
    if (m->accel.swapped) words[6] |= 2;
    memset(expected, 0xa5, sizeof(expected));
    for (i = 0; i < 32; i++)
        expected[RING_OFFSET / 4 + ((start + i) & mask)] =
            rdn_swap_le32(m->accel.swapped ? swap32(words[i]) : words[i]);
    return !memcmp(expected, m->mem, sizeof(expected));
}
static int unchanged(const struct model *m, const struct rdn_accel *before,
                     const uint32_t *memory)
{
    return !memcmp(&m->accel, before, sizeof(*before)) &&
        !memcmp(m->mem, memory, sizeof(m->mem)) && !m->writes && !m->delays &&
        m->seq == 0xdeadbeef;
}
static void error_case(struct model *m, uint64_t addr, uint32_t words, int expected,
                       const char *name)
{
    struct rdn_accel before = m->accel; uint32_t saved[AP_WORDS]; int ret;
    memcpy(saved, m->mem, sizeof(saved));
    ret = rdn_ib_try_submit(&m->accel, addr, words, &m->seq);
    check(ret == expected && unchanged(m, &before, saved), name);
}
int main(void)
{
    struct model m, legacy;
    uint32_t size, start, rptr, space, saved[AP_WORDS];
    struct rdn_accel before;
    int swapped, ret;
    for (size = 64; size <= 128; size *= 2) for (swapped = 0; swapped < 2; swapped++)
    for (start = 0; start < size; start += 16) for (rptr = 0; rptr < size; rptr++) {
        init(&m, size, start, rptr, swapped); before = m.accel;
        memcpy(saved, m.mem, sizeof(saved));
        space = (rptr + size - start - 1) % size;
        ret = rdn_ib_try_submit(&m.accel, 0x7a12345000ull, 121, &m.seq);
        if (space < 32) {
            check(ret == -EBUSY && unchanged(&m, &before, saved) && m.reads == 1,
                  "insufficient combined space never partially publishes or waits");
        } else {
            check(!ret && m.seq == 0x12345678 && m.accel.fence_emitted == m.seq &&
                  m.accel.wptr == ((start + 32) % size) && !m.delays && m.reads == 1,
                  "one RPTR snapshot reserves both padded commits without waiting");
            check(exact_commands(&m, start), "literal IB and fence packets, padding, byte order and guards");
            check(m.writes == 4 && m.offsets[0] == RDN_HDP_MEM_COHERENCY_FLUSH_CNTL &&
                  m.values[0] == 1 && m.offsets[1] == CP_RB_WPTR &&
                  m.values[1] == ((start + 16) % size) &&
                  m.offsets[2] == RDN_HDP_MEM_COHERENCY_FLUSH_CNTL && m.values[2] == 1 &&
                  m.offsets[3] == CP_RB_WPTR && m.values[3] == ((start + 32) % size),
                  "two existing HDP flush and WPTR commits preserve order across wrap");
            init(&legacy, size, start, rptr, swapped);
            ret = rdn_ib_submit(&legacy.accel, 0x7a12345000ull, 121, &legacy.seq);
            check(!ret && !memcmp(m.mem, legacy.mem, sizeof(m.mem)) &&
                  !memcmp(m.offsets, legacy.offsets, sizeof(m.offsets)) &&
                  !memcmp(m.values, legacy.values, sizeof(m.values)) &&
                  legacy.seq == m.seq && !legacy.delays,
                  "legacy and nonblocking submission emit identical bytes and MMIO writes");
        }
    }
#define RESET_MODEL() init(&m, 128, 0, 0, 0)
#define BAD(name) error_case(&m, 0x7a12345000ull, 121, -EINVAL, name)
    RESET_MODEL(); m.rptr = 128; BAD("RPTR at ring size rejects without masking");
    RESET_MODEL(); m.rptr = 0xffffffff; BAD("invalid all-ones RPTR rejects without writes");
    RESET_MODEL(); m.accel.wptr = 1; BAD("uncommitted WPTR rejects");
    RESET_MODEL(); m.accel.wptr = 128; BAD("out-of-range WPTR rejects");
    RESET_MODEL(); m.accel.ring_words = 0; BAD("empty ring rejects");
    RESET_MODEL(); m.accel.ring_words = 32; BAD("ring cannot consume its final unused slot");
    RESET_MODEL(); m.accel.ring_words = 96; BAD("non-power-of-two ring rejects");
    RESET_MODEL(); m.accel.ring_words = 0x80000000; BAD("ring byte extent cannot overflow");
    RESET_MODEL(); m.accel.ring_offset = sizeof(m.mem) - 4; BAD("ring beyond aperture rejects");
    RESET_MODEL(); m.accel.ring_offset = 0xfffffffc; BAD("offset beyond aperture rejects");
    RESET_MODEL(); m.accel.ring_offset++; BAD("unaligned ring rejects");
    RESET_MODEL(); m.accel.aperture = 0; BAD("null aperture rejects");
    RESET_MODEL(); m.accel.aperture = (uint32_t *)((uint8_t *)m.mem + 1); BAD("unaligned aperture rejects");
    RESET_MODEL(); m.accel.ready = false; BAD("stopped accelerator rejects");
    RESET_MODEL(); m.accel.card = 0; BAD("null card rejects");
    RESET_MODEL(); m.card.os = 0; BAD("null OS rejects");
    RESET_MODEL(); m.os.mmio_read32 = 0; BAD("missing read callback rejects");
    RESET_MODEL(); m.os.mmio_write32 = 0; BAD("missing write callback rejects");
    RESET_MODEL(); error_case(&m, 0x1001, 121, -EINVAL, "unaligned IB rejects");
    RESET_MODEL(); error_case(&m, 1ull << 40, 121, -EINVAL, "address above40 bits rejects");
    RESET_MODEL(); error_case(&m, (1ull << 40) - 4, 2, -EINVAL, "IB extent crossing40 bits rejects");
    RESET_MODEL(); error_case(&m, 0x1000, 0, -EINVAL, "empty IB rejects");
    RESET_MODEL(); m.accel.fence_emitted = 0xffffffff;
    check(!rdn_ib_try_submit(&m.accel, (1ull << 40) - 4, 1, &m.seq) &&
          !m.seq && !m.accel.fence_emitted, "last40-bit word and existing fence wrap remain supported");
    RESET_MODEL(); check(!rdn_ib_try_submit(&m.accel, 0, 1, 0) &&
          m.accel.fence_emitted == 0x12345678, "null optional sequence output supported");
    check(rdn_ib_try_submit(0, 0, 1, 0) == -EINVAL, "null accelerator rejects");
    printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
