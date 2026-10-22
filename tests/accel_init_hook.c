/* Exercise the real CP startup against a small register/ring model. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "../hw/rdn_accel.h"
#include "../hw/rdn_accel_reg.h"

#define APERTURE_BYTES 65536
#define RING_OFFSET 4096
#define RING_BYTES 32768

struct model {
	struct rdn_os os;
	struct rdn_card card;
	struct rdn_accel accel;
	uint32_t aperture[APERTURE_BYTES / 4];
	uint32_t registers[RDN_MMIO_SIZE / 4];
	uint32_t pfp_words, me_words, consumed, written;
	unsigned gpu_init, callbacks, resumes, order;
	unsigned firmware_order, hook_order, resume_order;
	int diagnostic_error;
	bool fail_ring;
};
static unsigned checks;
#define CHECK(condition) do { ++checks; assert(condition); } while (0)

static uint32_t ring_word(struct model *m, uint32_t index)
{
	const uint8_t *p = (const uint8_t *)m->aperture + RING_OFFSET + 4 * (index % (RING_BYTES / 4));
	return m->accel.swapped ? ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
		((uint32_t)p[2] << 8) | p[3] : ((uint32_t)p[3] << 24) |
		((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
}

static void consume_ring(struct model *m)
{
	if (m->registers[CP_ME_CNTL / 4] & (CP_ME_HALT | CP_PFP_HALT))
		return;
	while (m->consumed != m->written) {
		uint32_t header = ring_word(m, m->consumed);
		uint32_t count = (header >> 30) == 2 ? 0 : ((header >> 16) & 0x3fff) + 1;
		assert(count + 1 <= (m->written - m->consumed) % (RING_BYTES / 4));
		if ((header >> 30) == 3 && ((header >> 8) & 0xff) == PACKET3_SET_CONFIG_REG && count >= 2) {
			uint32_t reg = PACKET3_SET_CONFIG_REG_START + 4 * ring_word(m, m->consumed + 1);
			if (!m->fail_ring && reg == RDN_SCRATCH_REG(0))
				m->registers[reg / 4] = ring_word(m, m->consumed + 2);
		}
		m->consumed = (m->consumed + count + 1) % (RING_BYTES / 4);
	}
}

static uint32_t read32(void *cookie, uint32_t reg)
{
	struct model *m = cookie;
	assert(reg < RDN_MMIO_SIZE);
	if (reg == CP_RB_RPTR)
		return m->consumed;
	return m->registers[reg / 4];
}

static void write32(void *cookie, uint32_t reg, uint32_t value)
{
	struct model *m = cookie;
	assert(reg < RDN_MMIO_SIZE);
	m->registers[reg / 4] = value;
	++m->order;
	if (reg == CP_PFP_UCODE_DATA)
		++m->pfp_words;
	if (reg == CP_ME_RAM_DATA) {
		++m->me_words;
		m->firmware_order = m->order;
	}
	if (reg == GRBM_SOFT_RESET && value) {
		++m->resumes;
		m->resume_order = m->order;
	}
	if (reg == CP_RB_WPTR)
		m->written = value;
	if (reg == CP_RB_WPTR || reg == CP_ME_CNTL)
		consume_ring(m);
}

static void delay_us(void *cookie, uint32_t usec) { (void)cookie; (void)usec; }
static void log_message(void *cookie, enum rdn_log_level level, const char *format, va_list args)
{
	(void)cookie; (void)level; (void)format; (void)args;
}

void rdn_gpu_init(struct rdn_accel *accel)
{
	struct model *m = accel->card->os->cookie;
	++m->gpu_init;
	accel->cfg.max_hw_contexts = 4;
}

static void before_resume(void *cookie)
{
	struct model *m = cookie;
	++m->callbacks;
	m->hook_order = ++m->order;
	CHECK(m->gpu_init == 1 && !m->resumes && !m->accel.ready);
	CHECK(m->pfp_words == EVERGREEN_PFP_UCODE_SIZE);
	CHECK(m->me_words == EVERGREEN_PM4_UCODE_SIZE);
	CHECK(m->registers[CP_ME_CNTL / 4] == (CP_ME_HALT | CP_PFP_HALT));
	CHECK(m->accel.vram_base == UINT64_C(0xf00000000));
	m->diagnostic_error = -EIO;
}

static struct model model;
static uint8_t pfp[EVERGREEN_PFP_UCODE_SIZE * 4], me[EVERGREEN_PM4_UCODE_SIZE * 4];

static void reset(void)
{
	memset(&model, 0, sizeof(model));
	model.os.cookie = &model;
	model.os.mmio_read32 = read32;
	model.os.mmio_write32 = write32;
	model.os.delay_us = delay_us;
	model.os.log = log_message;
	model.card.os = &model.os;
	model.registers[MC_VM_FB_LOCATION / 4] = 0x0f3f0f00;
	model.registers[CONFIG_MEMSIZE / 4] = 1024;
}

int main(void)
{
	struct rdn_accel_fw fw = {pfp, sizeof(pfp), me, sizeof(me)};
	bool swapped;
	for (swapped = false; ; swapped = true) {
		reset();
		CHECK(rdn_accel_init_hook(&model.accel, &model.card, model.aperture,
			APERTURE_BYTES, &fw, RING_OFFSET, RING_BYTES, swapped,
			before_resume, &model) == 0);
		CHECK(model.callbacks == 1 && model.resumes == 1 && model.accel.ready);
		CHECK(model.firmware_order < model.hook_order && model.hook_order < model.resume_order);
		CHECK(model.diagnostic_error == -EIO);
		CHECK(model.registers[RDN_SCRATCH_REG(0) / 4] == 0xdeadbeef);
		if (swapped) break;
	}
	reset();
	CHECK(rdn_accel_init(&model.accel, &model.card, model.aperture,
		APERTURE_BYTES, &fw, RING_OFFSET, RING_BYTES, false) == 0);
	CHECK(!model.callbacks && model.resumes == 1 && model.accel.ready);
	reset();
	--fw.me_size;
	CHECK(rdn_accel_init_hook(&model.accel, &model.card, model.aperture,
		APERTURE_BYTES, &fw, RING_OFFSET, RING_BYTES, false,
		before_resume, &model) == -EINVAL);
	CHECK(!model.callbacks && !model.resumes && !model.pfp_words && !model.me_words);
	++fw.me_size;
	reset();
	CHECK(rdn_accel_init_hook(&model.accel, &model.card, NULL,
		APERTURE_BYTES, &fw, RING_OFFSET, RING_BYTES, false,
		before_resume, &model) == -EINVAL);
	CHECK(!model.callbacks && !model.resumes && !model.gpu_init);
	reset();
	model.fail_ring = true;
	CHECK(rdn_accel_init_hook(&model.accel, &model.card, model.aperture,
		APERTURE_BYTES, &fw, RING_OFFSET, RING_BYTES, false,
		before_resume, &model) == -EINVAL);
	CHECK(model.callbacks == 1 && model.resumes == 1 && !model.accel.ready);
	printf("CP startup hook: %u checks passed\n", checks);
	return 0;
}
