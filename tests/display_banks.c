/* Per-head register isolation on DCE4/5. */
#include <stdio.h>
#include <string.h>
#include "../hw/rdn_mode.h"
#include "../hw/rdn_cursor.h"

static uint32_t regs[0x20000 / 4];
static unsigned writes, bad, head, checks, failures;
static int peers;
static const uint32_t offsets[6] = {0, 0xc00, 0x9800, 0xa400, 0xb000, 0xbc00};

static uint32_t read_reg(void *cookie, uint32_t reg)
{
	(void)cookie;
	if (reg == 0xca0 + head * 0x20) return 1u << 4;
	if (reg >= sizeof(regs) || (reg & 3)) { bad++; return 0; }
	return regs[reg / 4];
}

static void write_reg(void *cookie, uint32_t reg, uint32_t value)
{
	uint32_t offset = offsets[head];
	(void)cookie;
	writes++;
	if (!((peers && ((reg >= 0x7400 && reg < 0x7c00) || reg == 0xc10 || reg == 0xc14)) ||
	      (reg >= 0x6800 + offset && reg < 0x7000 + offset) ||
	      reg == 0xca0 + head * 0x20 || reg == 0xbf0 + head * 0x20 ||
	      reg == 0xbf4 + head * 0x20)) bad++;
	if (reg < sizeof(regs) && !(reg & 3)) regs[reg / 4] = value;
	else bad++;
}

static void delay(void *cookie, uint32_t us) { (void)cookie; (void)us; }
static void log_message(void *cookie, enum rdn_log_level level, const char *fmt, va_list ap)
{ (void)cookie; (void)level; (void)fmt; (void)ap; }
static void check(int valid, const char *name)
{
	checks++;
	if (!valid) { failures++; printf("FAIL: %s\n", name); }
}

int main(void)
{
	struct rdn_os os = {0};
	struct rdn_card card = {0};
	struct rdn_lut_entry entry = {1023, 512, 0};
	unsigned before;
	os.mmio_read32 = read_reg; os.mmio_write32 = write_reg;
	os.delay_us = delay; os.log = log_message; card.os = &os;
	card.sclk = 80000; card.mclk = 60000;
	card.wm_clock = 148500; card.wm_hdisplay = 1920; card.wm_htotal = 2200;
	card.display_heads = 2;
	for (head = 0; head < 6; head++) {
		memset(regs, 0, sizeof(regs)); writes = bad = 0;
		card.display_crtc = head;
		rdn_cursor_set(&card, 0x100000, 10, 20, true);
		check(writes && !bad && regs[(0x699c + offsets[head])/4] == 0x100000,
		      "cursor writes only its controller bank");
		writes = bad = 0;
		rdn_lut_set(&card, 7, 1, &entry);
		check(writes == 4 && !bad, "colour table writes only its controller bank");
		writes = bad = 0;
		rdn_bandwidth_update(&card);
		check(writes && !bad, "watermarks use the selected display and DMIF pipe");
	}
	head = 0; card.display_crtc = 6; before = writes;
	rdn_cursor_set(&card, 0, 0, 0, true);
	rdn_lut_set(&card, 0, 1, &entry);
	rdn_bandwidth_update(&card);
	check(writes == before, "invalid controller causes no writes");
	struct rdn_card second = card;
	struct rdn_card *heads[6] = {&card, &second, 0, 0, 0, 0};
	card.display_crtc = 0; card.display_peers = heads;
	second.display_crtc = 1; second.crtc_on = true;
	second.sclk = 10000; second.mclk = 15000;
	card.sclk = 50000; card.mclk = 40000;
	peers = 1; writes = bad = 0;
	regs[(0x6b0c + offsets[1])/4] = 0x12345678;
	rdn_bandwidth_update_clocks(&card);
	check(writes && !bad && second.sclk == 50000 && second.mclk == 40000,
	      "clock changes update active peer watermarks");
	check(regs[(0x6b0c + offsets[1])/4] == 0x12345678,
	      "peer clock update does not reallocate line buffers");
	printf("%s: %u display-bank checks\n", failures ? "FAIL" : "PASS", checks);
	return failures != 0;
}
