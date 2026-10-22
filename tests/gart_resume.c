/* Restoring translation must not replace live mappings with the dummy page. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "rdn_gart.h"

static uint32_t read_reg(void *cookie, uint32_t offset)
{
	(void)cookie;
	(void)offset;
	return 1;
}

static void write_reg(void *cookie, uint32_t offset, uint32_t value)
{
	(void)cookie;
	(void)offset;
	(void)value;
}

static void delay(void *cookie, uint32_t us) { (void)cookie; (void)us; }
static void log_message(void *cookie, enum rdn_log_level level,
			const char *format, va_list args)
{
	(void)cookie; (void)level; (void)format; (void)args;
}

int main(void)
{
	struct rdn_os os = {0};
	struct rdn_card card = {0};
	struct rdn_accel accel = {0};
	struct rdn_gart gart = {0};
	void *saved;

	os.mmio_read32 = read_reg;
	os.mmio_write32 = write_reg;
	os.delay_us = delay;
	os.log = log_message;
	card.os = &os;
	accel.card = &card;
	accel.aperture_size = RDN_GART_TABLE_BYTES;
	accel.aperture = calloc(1, accel.aperture_size);
	accel.vram_base = 0xf00000000ull;
	accel.vram_size = 1024ull * 1024 * 1024;
	saved = malloc(accel.aperture_size);
	assert(saved && accel.aperture);
	assert(!rdn_gart_enable(&gart, &accel, 0, 0x1000));
	rdn_gart_set_page(&gart, 17, 0x12345000);
	rdn_gart_set_page(&gart, RDN_GART_PAGES - 1, 0x87654000);
	memcpy(saved, (const void *)accel.aperture, accel.aperture_size);
	rdn_gart_disable(&gart);
	assert(!gart.ready);
	assert(!rdn_gart_resume(&gart) && gart.ready);
	assert(!memcmp(saved, (const void *)accel.aperture, accel.aperture_size));
	assert(rdn_gart_resume(NULL) != 0);
	free(saved);
	free((void *)accel.aperture);
	return 0;
}
