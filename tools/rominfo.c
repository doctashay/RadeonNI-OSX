/* Inspect a captured board ROM without executing its command tables. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../hw/rdn_card.h"
#include "../hw/rdn_mode.h"
#include "../hw/rdn_pm.h"
#include "../hw/atom/atom.h"
#include "../hw/atom/atombios.h"

static void *alloc_mem(void *cookie, size_t size) { return calloc(1, size); }
static void free_mem(void *cookie, void *p) { free(p); }
static void log_msg(void *cookie, enum rdn_log_level level,
		    const char *fmt, va_list ap)
{
	vprintf(fmt, ap);
	putchar('\n');
}
static uint32_t read_reg(void *cookie, uint32_t reg)
{
	fprintf(stderr, "Unexpected register read 0x%x\n", reg);
	abort();
}
static void write_reg(void *cookie, uint32_t reg, uint32_t value)
{
	fprintf(stderr, "Unexpected register write 0x%x\n", reg);
	abort();
}

int main(int argc, char **argv)
{
	struct rdn_os os;
	struct rdn_card card;
	struct rdn_pm_state boot, performance;
	unsigned char *bios;
	FILE *f;
	long size;
	unsigned i, sum = 0, pcir;
	if (argc != 2 || !(f = fopen(argv[1], "rb")))
		return 1;
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	rewind(f);
	if (size < 512 || size > 0x200000)
		return 1;
	bios = malloc((size_t)size);
	assert(bios && fread(bios, 1, (size_t)size, f) == (size_t)size);
	fclose(f);
	assert(bios[0] == 0x55 && bios[1] == 0xaa);
	assert((unsigned)bios[2] * 512 == (unsigned)size);
	for (i = 0; i < (unsigned)size; i++)
		sum += bios[i];
	assert(!(sum & 255));
	pcir = bios[0x18] | ((unsigned)bios[0x19] << 8);
	assert(pcir + 8 <= (unsigned)size && !memcmp(bios + pcir, "PCIR", 4));
	printf("PCI %04x:%04x\n", bios[pcir + 4] | bios[pcir + 5] << 8,
	       bios[pcir + 6] | bios[pcir + 7] << 8);
	memset(&os, 0, sizeof(os));
	os.alloc = alloc_mem;
	os.free = free_mem;
	os.log = log_msg;
	os.mmio_read32 = read_reg;
	os.mmio_write32 = write_reg;
	assert(!rdn_card_init(&card, &os, bios));
	assert(!rdn_pm_boot_state(&card, &boot));
	assert(!rdn_pm_performance_state(&card, &performance));
	printf("Boot %u/%u MHz; performance %u/%u MHz\n",
	       boot.sclk / 100, boot.mclk / 100,
	       performance.sclk / 100, performance.mclk / 100);
	rdn_pm_log_states(&card);
	rdn_output_assume(&card);
	printf("Digital outputs %d\n", card.num_board);
	for (i = 0; i < (unsigned)card.num_board; i++) {
		const struct rdn_output *out = &card.board[i];
		printf("%s: transmitter %u link %c DIG %u DDC 0x%02x HPD %u\n",
		       out->name, out->transmitter_sel, out->linkb ? 'B' : 'A',
		       out->dig_encoder, out->i2c_id, out->hpd);
	}
#define PRINT_COMMAND(name) do { \
	uint8_t frev, crev; \
	assert(atom_parse_cmd_header(card.atom.ctx, \
		GetIndexIntoMasterTable(COMMAND, name), &frev, &crev)); \
	printf(#name " %u.%u\n", frev, crev); \
} while (0)
	PRINT_COMMAND(AdjustDisplayPll);
	PRINT_COMMAND(SelectCRTC_Source);
	PRINT_COMMAND(SetPixelClock);
	PRINT_COMMAND(DIGxEncoderControl);
	PRINT_COMMAND(UNIPHYTransmitterControl);
	PRINT_COMMAND(ProcessAuxChannelTransaction);
#undef PRINT_COMMAND
	rdn_card_fini(&card);
	free(bios);
	puts("PASS ROM metadata parsed without register access");
	return 0;
}
