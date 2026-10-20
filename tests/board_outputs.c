/*
 * Hardware-free test of output detection from the VBIOS's object table
 * (rdn_output_detect() in rdn_modeset.c).
 *
 * 1. On the 7570's VBIOS with no display anywhere: detection fails cleanly
 *    after every candidate, and the table gives exactly the wiring built
 *    in for that board.
 * 2. A board wired differently (made from the same VBIOS by moving two DDC
 *    lines in the connector records, as on a retail HD 6670): the DVI port's
 *    own line answers with an EDID. The output chosen must be the table's
 *    for that connector (its transmitter, link and encoder), not the
 *    7570's transmitter with the DVI port's line, which sent the picture
 *    out of another port.
 *
 * A simulated EDID EEPROM sits on the I2C line pair under test.
 *
 * Built for x86 and for big-endian PowerPC; both must print the same.
 *
 * Usage: board_outputs <vbios.rom>
 *
 * Copyright (c) 2026 kouta-kun and Claude
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hw/rdn_card.h"
#include "../hw/rdn_mode.h"
#include "../hw/rdn_i2c.h"
#include "../hw/atom/atombios.h"
#include "../hw/atom/ObjectID.h"

#define MMIO_WORDS	(0x20000 / 4)

enum phase { IDLE, RX, ACK_OUT, TX, ACK_IN };

struct mock {
	uint32_t regs[MMIO_WORDS];
	uint64_t clock_ms;
	struct rdn_i2c_bus bus;	/* the line pair with a device, if present */
	int present;
	uint8_t mem[256];
	int scl, sda, sda_slave;
	enum phase phase;
	int nbits, first_byte, rw, acked;
	uint8_t shift, ptr, tx;
};

static struct mock m;

/* The slave: the same simulated 24C-style EEPROM as tests/i2c_edid.c. */
static void slave_falling(void)
{
	switch (m.phase) {
	case RX:
		if (m.nbits != 8)
			break;
		if (m.first_byte) {
			if ((m.shift >> 1) != 0x50) {
				m.phase = IDLE;
				break;
			}
			m.rw = m.shift & 1;
		} else {
			m.ptr = m.shift;
		}
		m.sda_slave = 0;
		m.phase = ACK_OUT;
		break;
	case ACK_OUT:
		if (m.rw) {
			m.tx = m.mem[m.ptr];
			m.nbits = 0;
			m.sda_slave = (m.tx >> 7) & 1;
			m.phase = TX;
		} else {
			m.sda_slave = 1;
			m.nbits = 0;
			m.shift = 0;
			m.first_byte = 0;
			m.phase = RX;
		}
		break;
	case TX:
		m.nbits++;
		if (m.nbits == 8) {
			m.sda_slave = 1;
			m.phase = ACK_IN;
		} else {
			m.sda_slave = (m.tx >> (7 - m.nbits)) & 1;
		}
		break;
	case ACK_IN:
		if (m.acked) {
			m.ptr++;
			m.tx = m.mem[m.ptr];
			m.nbits = 0;
			m.sda_slave = (m.tx >> 7) & 1;
			m.phase = TX;
		} else {
			m.sda_slave = 1;
			m.phase = IDLE;
		}
		break;
	default:
		break;
	}
}

static void slave_rising(void)
{
	if (m.phase == RX && m.nbits < 8) {
		m.shift = (uint8_t)((m.shift << 1) | m.sda);
		m.nbits++;
	} else if (m.phase == ACK_IN) {
		m.acked = !m.sda;
	}
}

static void lines_update(void)
{
	uint32_t en_clk = m.regs[m.bus.en_clk_reg / 4];
	uint32_t en_data = m.regs[m.bus.en_data_reg / 4];
	int scl_m = !(en_clk & m.bus.en_clk_mask);
	int sda_m = !(en_data & m.bus.en_data_mask);
	int old_scl = m.scl, old_sda = m.sda;
	int sda;

	if (!m.present) {
		m.scl = scl_m;
		m.sda = sda_m;
		return;
	}
	sda = sda_m && m.sda_slave;
	if (old_scl && scl_m && old_sda && !sda_m) {
		m.phase = RX;
		m.nbits = 0;
		m.shift = 0;
		m.first_byte = 1;
		m.sda_slave = 1;
	} else if (old_scl && scl_m && !old_sda && sda_m && m.sda_slave) {
		m.phase = IDLE;
	}
	m.scl = scl_m;
	m.sda = sda;
	if (old_scl && !scl_m)
		slave_falling();
	else if (!old_scl && scl_m)
		slave_rising();
	m.sda = sda_m && m.sda_slave;
}

static uint32_t os_mmio_read32(void *c, uint32_t off)
{
	uint32_t v = off / 4 < MMIO_WORDS ? m.regs[off / 4] : 0xffffffffu;

	(void)c;
	if (m.bus.valid && off == m.bus.y_clk_reg) {
		v &= ~m.bus.y_clk_mask;
		if (m.scl)
			v |= m.bus.y_clk_mask;
	}
	if (m.bus.valid && off == m.bus.y_data_reg) {
		v &= ~m.bus.y_data_mask;
		if (m.sda)
			v |= m.bus.y_data_mask;
	}
	return v;
}

static void os_mmio_write32(void *c, uint32_t off, uint32_t v)
{
	(void)c;
	if (off / 4 >= MMIO_WORDS)
		return;
	m.regs[off / 4] = v;
	if (m.bus.valid &&
	    (off == m.bus.en_clk_reg || off == m.bus.en_data_reg))
		lines_update();
}

static void os_delay_us(void *c, uint32_t usec)
{
	(void)c;
	(void)usec;
}

static uint64_t os_time_ms(void *c)
{
	(void)c;
	return m.clock_ms++;
}

static void *os_alloc(void *c, size_t size)
{
	(void)c;
	return calloc(1, size);
}

static void os_free(void *c, void *ptr)
{
	(void)c;
	free(ptr);
}

static void os_log(void *c, enum rdn_log_level level, const char *fmt,
		   va_list ap)
{
	(void)c;
	(void)level;
	/* Only the lines of the output list; the rest is not under test. */
	if (strncmp(fmt, "board:", 6) && strstr(fmt, "no EDID") == NULL &&
	    strstr(fmt, "EDID found") == NULL)
		return;
	vprintf(fmt, ap);
	printf("\n");
}

static uint16_t le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

/* Move the DDC line in the I2C record of connector object `conn_id`. */
static int patch_ddc(struct rdn_card *card, uint8_t conn_id, uint8_t from,
		     uint8_t to)
{
	struct atom_context *ctx = card->atom.ctx;
	int index = GetIndexIntoMasterTable(DATA, Object_Header);
	uint16_t size, off;
	uint8_t *base, *con;
	int n, j, k;

	if (!atom_parse_data_header(ctx, index, &size, NULL, NULL, &off))
		return -1;
	base = (uint8_t *)ctx->bios + off;
	con = base + le16(base + 6);
	n = con[0];
	for (j = 0; j < n; j++) {
		uint8_t *obj = con + 4 + 8 * j;
		uint8_t *rec;

		if ((le16(obj) & 0xff) != conn_id)
			continue;
		rec = base + le16(obj + 4);
		for (k = 0; k < 16 && rec[1] > 0 && rec[0] > 0 &&
		     rec[0] <= ATOM_MAX_OBJECT_RECORD_NUMBER; k++) {
			if (rec[0] == ATOM_I2C_RECORD_TYPE && rec[2] == from) {
				rec[2] = to;
				return 0;
			}
			rec += rec[1];
		}
	}
	return -1;
}

static void make_edid(uint8_t *e)
{
	static const uint8_t header[8] = {
		0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00
	};
	uint8_t sum = 0;
	int i;

	for (i = 0; i < 256; i++)
		e[i] = (uint8_t)(i * 37 + 11);
	memcpy(e, header, 8);
	e[126] = 0;
	for (i = 0; i < 127; i++)
		sum = (uint8_t)(sum + e[i]);
	e[127] = (uint8_t)(0x100 - sum);
}

static void os_setup(struct rdn_os *os)
{
	memset(os, 0, sizeof(*os));
	os->mmio_read32 = os_mmio_read32;
	os->mmio_write32 = os_mmio_write32;
	os->delay_us = os_delay_us;
	os->time_ms = os_time_ms;
	os->alloc = os_alloc;
	os->free = os_free;
	os->log = os_log;
}

int main(int argc, char **argv)
{
	static uint8_t bios[0x20000], bios2[0x20000];
	uint8_t edid[RDN_EDID_MAX_SIZE];
	static struct rdn_card card;
	struct rdn_os os;
	struct rdn_i2c_bus bus;
	size_t len;
	FILE *f;
	int i, r, dvi = 0, dp = 0;
	struct rdn_mode link_mode = { 0 };
	struct rdn_output link_output = { 0 };

	card.output = &link_output;
	link_output.connector_id = CONNECTOR_OBJECT_ID_DUAL_LINK_DVI_I;
	link_mode.clock = 165000;
	if (rdn_output_is_duallink(&card, &link_mode) ||
	    !rdn_output_mode_supported(&card, &link_mode, false))
		return 1;
	link_mode.clock = 268000;
	if (!rdn_output_is_duallink(&card, &link_mode))
		return 1;
	if (!rdn_output_mode_supported(&card, &link_mode, false) ||
	    rdn_output_mode_supported(&card, &link_mode, true))
		return 1;
	link_mode.clock = 340000;
	if (rdn_output_mode_supported(&card, &link_mode, false))
		return 1;
	link_mode.clock = 268000;
	link_output.displayport = true;
	if (rdn_output_is_duallink(&card, &link_mode))
		return 1;
	link_output.displayport = false;
	link_output.connector_id = CONNECTOR_OBJECT_ID_SINGLE_LINK_DVI_D;
	if (rdn_output_is_duallink(&card, &link_mode))
		return 1;
	memset(&card, 0, sizeof(card));
	if (argc == 2 && !strcmp(argv[1], "--links-only")) {
		printf("PASS: DVI link count follows connector type and pixel clock\n");
		return 0;
	}

	if (argc != 2) {
		fprintf(stderr, "usage: %s <vbios.rom>\n", argv[0]);
		return 2;
	}
	f = fopen(argv[1], "rb");
	if (!f || (len = fread(bios, 1, sizeof(bios), f)) < 0x200) {
		fprintf(stderr, "cannot read %s\n", argv[1]);
		return 2;
	}
	fclose(f);
	memcpy(bios2, bios, sizeof(bios));
	os_setup(&os);

	/* 1. No display anywhere, the 7570's own table. */
	memset(&m, 0, sizeof(m));
	m.scl = m.sda = 1;
	if (rdn_card_init(&card, &os, bios)) {
		fprintf(stderr, "VBIOS not usable\n");
		return 2;
	}
	r = rdn_output_detect(&card, edid);
	printf("detect with no display: %d, %d board outputs\n", r,
	       card.num_board);
	if (r > 0) {
		printf("FAIL: found an EDID where there is none\n");
		return 1;
	}
	for (i = 0; i < card.num_board; i++) {
		const struct rdn_output *o = &card.board[i];

		printf("board output %d: %s, connector 0x%02x, UNIPHY%d%s, "
		       "DIG %d, DDC 0x%02x, HPD %d%s\n", i, o->name,
		       o->connector_id, o->transmitter_sel,
		       o->linkb ? " link B" : "", o->dig_encoder, o->i2c_id,
		       o->hpd, o->displayport ? ", DisplayPort" : "");
		if (!o->displayport && o->transmitter_sel == 0 &&
		    !o->linkb && o->dig_encoder == 0 && o->i2c_id == 0x93 &&
		    o->hpd == 0)
			dvi = 1;
		if (o->displayport && o->transmitter_sel == 2 && !o->linkb &&
		    o->dig_encoder == 4 && o->i2c_id == 0x92 && o->hpd == 3)
			dp = 1;
	}
	if (!dvi || !dp) {
		printf("FAIL: the built-in wiring (DVI-I %d, DisplayPort %d) "
		       "is not what the object table says\n", dvi, dp);
		return 1;
	}
	rdn_card_fini(&card);

	/*
	 * 2. A board like the retail 6670: the DVI connector's DDC line is
	 * 0x92 (the 7570's DisplayPort connector's), the DisplayPort-type
	 * connector's is 0x94. A monitor on the DVI port: its EDID answers on
	 * 0x92.
	 */
	memset(&m, 0, sizeof(m));
	m.scl = m.sda = m.sda_slave = 1;
	memset(&card, 0, sizeof(card));
	if (rdn_card_init(&card, &os, bios2)) {
		fprintf(stderr, "VBIOS not usable\n");
		return 2;
	}
	if (patch_ddc(&card, 0x13, 0x92, 0x94) ||
	    patch_ddc(&card, 0x02, 0x93, 0x92)) {
		printf("FAIL: cannot patch the connector records\n");
		return 1;
	}
	rdn_i2c_bus_by_id(&card, 0x92, &bus);
	if (!bus.valid) {
		printf("FAIL: no I2C line 0x92 in the VBIOS\n");
		return 1;
	}
	m.bus = bus;
	m.present = 1;
	make_edid(m.mem);
	r = rdn_output_detect(&card, edid);
	printf("detect on a rewired board: %d\n", r);
	if (r <= 0 || !card.output) {
		printf("FAIL: the display on line 0x92 was not found\n");
		return 1;
	}
	printf("chosen: %s, UNIPHY%d%s, DIG %d, DDC 0x%02x%s\n",
	       card.output->name, card.output->transmitter_sel,
	       card.output->linkb ? " link B" : "", card.output->dig_encoder,
	       card.output->i2c_id,
	       card.output->displayport ? ", DisplayPort" : "");
	if (card.output->i2c_id != 0x92 || card.output->displayport ||
	    card.output->transmitter_sel != 0 || card.output->dig_encoder != 0) {
		printf("FAIL: the DVI port's line answered but the output "
		       "is not that connector's own transmitter\n");
		return 1;
	}
	rdn_card_fini(&card);
	printf("PASS: output detection follows the object table, built-in "
	       "wiring kept\n");
	return 0;
}
