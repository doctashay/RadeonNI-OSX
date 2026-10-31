/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../hw/rdn_accel.h"
#include "../hw/rdn_mode.h"
#include "../hw/rdn_cursor.h"
#include "../hw/rdn_pm.h"
#include "../hw/rdn_mc.h"
#include "../hw/rdn_gart.h"
#include "../hw/rdn_dma.h"
#include "../hw/rdn_reg.h"

static unsigned reads, writes, heads, posts;
static uint32_t memory_mb = 1024;
static int post_error;
static uint32_t read_reg(void *cookie, uint32_t reg)
{
    const struct rdn_card *card = cookie;
    unsigned i;
    reads++;
    for (i = 0; i < RDN_NUM_CRTC; i++) {
        if (reg == EVERGREEN_CRTC_CONTROL + rdn_crtc_offset(i)) {
            assert(i < rdn_crtc_count(card));
            heads |= 1u << i;
        }
    }
    return reg == CONFIG_MEMSIZE ? memory_mb : 0;
}
int __wrap_atom_asic_init(struct atom_context *ctx)
{
    (void)ctx;
    posts++;
    if (!post_error) memory_mb = 1024;
    return post_error;
}

static void write_reg(void *cookie, uint32_t reg, uint32_t value)
{ (void)cookie; (void)reg; (void)value; writes++; }
static void log_message(void *cookie, enum rdn_log_level level, const char *fmt, va_list ap)
{ (void)cookie; (void)level; (void)fmt; (void)ap; }
static void delay(void *cookie, uint32_t usec) { (void)cookie; (void)usec; }

int main(void)
{
    const enum rdn_family families[] = {RDN_FAMILY_BARTS, RDN_FAMILY_CAICOS};
    unsigned f;
    assert(rdn_family_crtc_count(RDN_FAMILY_TURKS) == 6);
    assert(rdn_family_crtc_count(RDN_FAMILY_CAYMAN) == 6);
    assert(!rdn_family_crtc_count(RDN_FAMILY_UNKNOWN));
    for (f = 0; f < sizeof(families) / sizeof(families[0]); f++) {
        struct rdn_card card = {0};
        struct rdn_os os = {0};
        struct rdn_accel accel;
        struct rdn_mode mode = {0};
        struct rdn_fb fb = {0};
        struct rdn_pm_state from = {0}, to = {0};
        uint32_t aperture[1024];
        struct rdn_lut_entry entries[1] = {{0}};
        unsigned count;
        card.family = families[f]; card.os = &os;
        os.cookie = &card; os.mmio_read32 = read_reg; os.mmio_write32 = write_reg;
        os.log = log_message; os.delay_us = delay;
        count = rdn_crtc_count(&card);
        assert(count == (f ? 4 : 6));
        reads = writes = heads = 0;
        assert(rdn_card_posted(&card) && heads == (1u << count) - 1 && !writes);
        heads = 0;
        assert(!rdn_card_post(&card) && heads == (1u << count) - 1);
        memory_mb = 0; posts = 0; heads = 0; post_error = 0;
        assert(!rdn_card_post(&card) && posts == 1 && memory_mb == 1024);
        assert(heads == (1u << count) - 1);
        memory_mb = 0; posts = 0; post_error = -5;
        assert(rdn_card_post(&card) == -5 && posts == 1 && !memory_mb);
        memory_mb = 1024; post_error = 0;
        reads = writes = 0;
        memset(&accel, 0, sizeof(accel)); accel.card = &card;
        rdn_gpu_init(&accel);
        assert(!reads && !writes && !accel.cfg.active_simds);
        memset(aperture, 0x5a, sizeof(aperture));
        assert(rdn_accel_init(&accel, &card, aperture, sizeof(aperture), NULL,
                              0, sizeof(aperture), false) < 0);
        assert(!reads && !writes && !accel.card && !accel.ready && aperture[0] == 0x5a5a5a5a);
        rdn_accel_fini(&accel);
        assert(!reads && !writes);
        assert(rdn_mc_load_microcode(&card, NULL, 0) < 0);
        assert(rdn_mc_relocate(&card, 0) < 0);
        assert(rdn_pm_set(&card, &from, &to, 0) < 0);
        assert(!reads && !writes);
        card.display_crtc = count;
        rdn_cursor_set(&card, 0, 0, 0, true);
        rdn_lut_set(&card, 0, 1, entries);
        rdn_bandwidth_update(&card);
        rdn_bandwidth_update_clocks(&card);
        assert(rdn_modeset(&card, &mode, &fb, false) < 0);
        assert(rdn_output_disable(&card, &mode, false) < 0);
        assert(!reads && !writes);
    }
    {
        const enum rdn_family pending[] = {
            RDN_FAMILY_REDWOOD, RDN_FAMILY_CEDAR, RDN_FAMILY_JUNIPER,
            RDN_FAMILY_CYPRESS, RDN_FAMILY_ARUBA, RDN_FAMILY_UNKNOWN
        };
        for (f = 0; f < sizeof(pending) / sizeof(pending[0]); f++) {
            struct rdn_card card = {0};
            struct rdn_os os = {0};
            struct rdn_accel accel = {0};
            struct rdn_gart gart = {0};
            struct rdn_dma dma = {0};
            uint32_t aperture[1024] = {0};
            card.family = pending[f]; card.os = &os;
            os.cookie = &card; os.mmio_read32 = read_reg; os.mmio_write32 = write_reg;
            os.log = log_message; os.delay_us = delay;
            reads = writes = 0;
            accel.card = &card;
            rdn_gpu_init(&accel);
            assert(!accel.cfg.max_simds);
            assert(rdn_mc_load_microcode(&card, NULL, 0) < 0);
            assert(rdn_mc_relocate(&card, 0) < 0);
            assert(rdn_rlc_start(&accel, NULL, 0) < 0);
            gart.accel = &accel;
            assert(rdn_gart_flush_checked(&gart) < 0);
            assert(rdn_gart_resume(&gart) < 0);
            assert(rdn_gart_enable(&gart, &accel, 0, 0) < 0);
            assert(rdn_gart_enable_with_scratch(&gart, &accel, 0, 0, 4096) < 0);
            gart.ready = true;
            rdn_gart_disable(&gart);
            accel.ready = true;
            assert(rdn_dma_init(&dma, &accel, 0, 0, 4096, 8192) < 0);
            assert(rdn_accel_init(&accel, &card, aperture, sizeof(aperture), NULL,
                                  0, sizeof(aperture), false) < 0);
            assert(!reads && !writes && !accel.ready);
        }
    }
    puts("PASS framebuffer POST, CRTC bounds, and zero-I/O rejection for unimplemented family engines");
    return 0;
}
