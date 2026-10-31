/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include "../hw/rdn_mode.h"
#include "../hw/atom/atombios.h"
#include "../hw/atom/ObjectID.h"

static struct rdn_card card;
static struct rdn_os os;
static struct atom_context ctx;
static uint8_t bios[512];
static uint32_t pci, subsystem;
static int count, clocks, fail_at, parsed_objects, object_table;
static uint8_t calls[16][8];

static uint32_t config(void *cookie, uint32_t off)
{ (void)cookie; assert(off == 0 || off == 0x2c); return off ? subsystem : pci; }
static void log_message(void *cookie, enum rdn_log_level level, const char *fmt, va_list ap)
{ (void)cookie; (void)level; (void)fmt; (void)ap; }

bool __wrap_atom_parse_cmd_header(struct atom_context *c, int index,
                                  uint8_t *frev, uint8_t *crev)
{
    assert(c == &ctx);
    *frev = 1;
    if (index == GetIndexIntoMasterTable(COMMAND, UNIPHYTransmitterControl)) {
        *crev = 4;
        return true;
    }
    assert(index == GetIndexIntoMasterTable(COMMAND, SetPixelClock));
    *crev = 6;
    return true;
}

bool __wrap_atom_parse_data_header(struct atom_context *c, int index,
                                   uint16_t *size, uint8_t *frev, uint8_t *crev,
                                   uint16_t *off)
{
    assert(c == &ctx);
    if (index == GetIndexIntoMasterTable(DATA, Object_Header)) {
        parsed_objects++;
        if (!object_table) return false;
        if (size) *size = 0xe0;
        if (frev) *frev = 1;
        if (crev) *crev = 2;
        *off = 0x20;
        return true;
    }
    assert(index == GetIndexIntoMasterTable(DATA, FirmwareInfo));
    if (size) *size = sizeof(ATOM_FIRMWARE_INFO_V2_1);
    if (frev) *frev = 2;
    if (crev) *crev = 1;
    *off = 0x100;
    return true;
}

int __wrap_atom_execute_table(struct atom_context *c, int index,
                              uint32_t *params, int words)
{
    assert(c == &ctx && words >= 2);
    if (index == GetIndexIntoMasterTable(COMMAND, UNIPHYTransmitterControl)) {
        assert(count < 16);
        memcpy(calls[count++], params, 8);
        return fail_at == count ? -5 : 0;
    }
    assert(index == GetIndexIntoMasterTable(COMMAND, SetPixelClock));
    clocks++;
    return 0;
}

static void setup(enum rdn_family family, uint16_t device)
{
    memset(&card, 0, sizeof(card));
    memset(&os, 0, sizeof(os));
    memset(&ctx, 0, sizeof(ctx));
    memset(bios, 0, sizeof(bios));
    memset(calls, 0, sizeof(calls));
    card.os = &os; card.atom.ctx = &ctx; card.family = family; ctx.bios = bios;
    os.cfg_read32 = config; os.log = log_message;
    pci = ((uint32_t)device << 16) | 0x1002; subsystem = 0x2b201028;
    count = clocks = fail_at = parsed_objects = object_table = 0;
    size_t off = 0x100 + offsetof(ATOM_FIRMWARE_INFO_V2_1, ulDefaultDispEngineClkFreq);
    bios[off] = 54000 & 255; bios[off + 1] = 54000 >> 8;
}

static void put16(uint8_t *p, unsigned value)
{ p[0] = value; p[1] = value >> 8; }

static void make_object_table(void)
{
    uint8_t *base = bios + 0x20;
    unsigned i;
    put16(base + 6, 0x20);
    put16(base + 14, 0x60);
    base[0x20] = 2;
    base[0x60] = 2;
    for (i = 0; i < 2; i++) {
        unsigned connector = (i ? CONNECTOR_OBJECT_ID_DISPLAYPORT : CONNECTOR_OBJECT_ID_HDMI_TYPE_A) |
                             (1 << ENUM_ID_SHIFT) | (GRAPH_OBJECT_TYPE_CONNECTOR << OBJECT_TYPE_SHIFT);
        unsigned encoder = (i ? ENCODER_OBJECT_ID_INTERNAL_UNIPHY : ENCODER_OBJECT_ID_INTERNAL_UNIPHY1) |
                           ((i ? 1 : 2) << ENUM_ID_SHIFT) | (GRAPH_OBJECT_TYPE_ENCODER << OBJECT_TYPE_SHIFT);
        uint8_t *obj = base + 0x24 + 8 * i, *path = base + 0x64 + 10 * i;
        uint8_t *rec = base + 0x90 + 8 * i;
        put16(obj, connector); put16(obj + 4, 0x90 + 8 * i);
        rec[0] = ATOM_I2C_RECORD_TYPE; rec[1] = 3; rec[2] = 0x93 + i;
        put16(path, 1); put16(path + 2, 10); put16(path + 4, connector);
        put16(path + 8, encoder);
    }
    object_table = 1;
}

static void output(int i, bool dp, uint8_t connector, uint8_t transmitter, bool linkb)
{
    struct rdn_output *o = &card.board[i];
    o->displayport = dp; o->connector_id = connector;
    o->transmitter_sel = transmitter; o->linkb = linkb;
    o->dig_encoder = 2 * transmitter + linkb;
    o->name = "model";
    if (card.num_board <= i) card.num_board = i + 1;
}

static void expect(int i, uint8_t connector, uint8_t transmitter, bool linkb)
{
    assert(calls[i][0] == connector && calls[i][1] == 0);
    assert(calls[i][2] == ((1 << 1) | ((linkb ? 1 : 0) << 2) | (transmitter << 6)));
    assert(calls[i][3] == ATOM_TRANSMITTER_ACTION_INIT && calls[i][4] == 4);
}

int main(void)
{
    enum rdn_family family;
    const enum rdn_family families[] = {RDN_FAMILY_TURKS, RDN_FAMILY_CAYMAN,
                                        RDN_FAMILY_BARTS, RDN_FAMILY_CAICOS};
    unsigned f;
    for (f = 0; f < sizeof(families) / sizeof(families[0]); f++) {
        family = families[f];
        setup(family, family == RDN_FAMILY_TURKS ? 0x6758 : 0x6719);
        make_object_table();
        assert(!rdn_display_init(&card) && parsed_objects == 1 && card.num_board == 2);
        assert(count == 2 && clocks == 1);
        expect(0, CONNECTOR_OBJECT_ID_DISPLAYPORT, 0, false);
        expect(1, CONNECTOR_OBJECT_ID_HDMI_TYPE_A, 1, true);

        setup(family, family == RDN_FAMILY_TURKS ? 0x6758 : 0x6719);
        output(0, false, CONNECTOR_OBJECT_ID_HDMI_TYPE_A, 1, true);
        output(1, false, CONNECTOR_OBJECT_ID_SINGLE_LINK_DVI_D, 2, false);
        output(2, true, CONNECTOR_OBJECT_ID_DISPLAYPORT, 0, false);
        card.board[3] = card.board[0]; card.board[3].i2c_id = 0x94; card.num_board++;
        assert(!rdn_display_init(&card) && count == 3 && clocks == 1);
        expect(0, CONNECTOR_OBJECT_ID_DISPLAYPORT, 0, false);
        expect(1, CONNECTOR_OBJECT_ID_HDMI_TYPE_A, 1, true);
        expect(2, CONNECTOR_OBJECT_ID_SINGLE_LINK_DVI_D, 2, false);

        setup(family, 0x6758);
        output(0, true, CONNECTOR_OBJECT_ID_DISPLAYPORT, 1, false);
        output(1, true, CONNECTOR_OBJECT_ID_DISPLAYPORT, 1, true);
        assert(!rdn_display_init(&card) && count == 2);
        expect(0, CONNECTOR_OBJECT_ID_DISPLAYPORT, 1, false);
        expect(1, CONNECTOR_OBJECT_ID_DISPLAYPORT, 1, true);

        setup(family, 0x6758);
        output(0, true, CONNECTOR_OBJECT_ID_DISPLAYPORT, 0, false);
        output(1, false, CONNECTOR_OBJECT_ID_HDMI_TYPE_A, 3, false);
        assert(rdn_display_init(&card) < 0 && !count && !clocks);
        card.board[1].transmitter_sel = 1; card.board[1].dig_encoder = RDN_NUM_CRTC;
        assert(rdn_display_init(&card) < 0 && !count && !clocks);
        card.board[1].dig_encoder = 2; card.board[1].displayport = true;
        assert(rdn_display_init(&card) < 0 && !count && !clocks);

        setup(family, 0x6758);
        output(0, true, CONNECTOR_OBJECT_ID_DISPLAYPORT, 0, false);
        output(1, false, CONNECTOR_OBJECT_ID_HDMI_TYPE_A, 1, false);
        fail_at = 1;
        assert(rdn_display_init(&card) == -5 && count == 1 && !clocks);
        card.num_board = RDN_MAX_BOARD_OUTPUTS + 1; count = 0;
        assert(rdn_display_init(&card) < 0 && !count && !clocks);
    }
    setup(RDN_FAMILY_TURKS, 0x675d);
    assert(!rdn_display_init(&card) && count == 2 && parsed_objects == 1 && clocks == 1);
    expect(0, CONNECTOR_OBJECT_ID_DISPLAYPORT, 2, false);
    expect(1, CONNECTOR_OBJECT_ID_DUAL_LINK_DVI_I, 0, false);
    setup(RDN_FAMILY_TURKS, 0x675d);
    output(0, false, CONNECTOR_OBJECT_ID_SINGLE_LINK_DVI_D, 1, true);
    card.output = &card.board[0]; card.num_board = 0;
    assert(!rdn_display_init(&card) && count == 3);
    expect(2, CONNECTOR_OBJECT_ID_SINGLE_LINK_DVI_D, 1, true);
    setup(RDN_FAMILY_TURKS, 0x6758);
    assert(rdn_display_init(&card) < 0 && !count && !clocks);
    setup(RDN_FAMILY_CAYMAN, 0x6704);
    assert(rdn_display_init(&card) < 0 && !count && !clocks);
    setup(RDN_FAMILY_TURKS, 0x675d); subsystem = 0x12341002;
    assert(rdn_display_init(&card) < 0 && !count && !clocks);
    setup(RDN_FAMILY_TURKS, 0x675d); subsystem = 0xffffffff;
    assert(rdn_display_init(&card) < 0 && !count && !clocks);
    setup(RDN_FAMILY_TURKS, 0x675d); os.cfg_read32 = NULL;
    assert(rdn_display_init(&card) < 0 && !count && !clocks);
    puts("PASS four-family board-driven transmitter init, link isolation, 7570 fallback and failures");
    return 0;
}
