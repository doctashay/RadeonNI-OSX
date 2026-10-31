/*
 * Engine and memory clocks: the PowerPlay table and the static switch
 * between two of its states.
 *
 * Follows the Linux radeon driver: radeon_set_power_state() in
 * radeon_pm.c, evergreen_pm_misc(), evergreen_pm_prepare(),
 * evergreen_pm_finish() and evergreen_get_temp() in evergreen.c,
 * radeon_atom_set_engine_clock(), radeon_atom_set_memory_clock(),
 * radeon_atom_set_voltage() and the PowerPlay parsing in
 * radeon_atombios.c. Unlike Linux's profile method this does not hold the
 * clocks down to FirmwareInfo's defaults, which on this card are the slow
 * boot clocks.
 *
 * Copyright 2007-8 Advanced Micro Devices, Inc.
 * Copyright 2008 Red Hat Inc.
 * Copyright 2010 Advanced Micro Devices, Inc.
 * Copyright (c) 2026 kouta-kun and Claude
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <string.h>

#include "rdn_pm.h"
#include "rdn_reg.h"
#include "atom/atombios.h"

#ifndef EBUSY
#define EBUSY 16
#endif

#define CG_MULT_THERMAL_STATUS			0x740
#define		ASIC_T_MASK			0x07FF0000
#define		ASIC_T_SHIFT			16
#define EVERGREEN_CRTC_DISP_READ_REQUEST_DISABLE	(1 << 24)

/* ATOM_PPLIB_POWERPLAYTABLE, by offset: the table is little-endian. */
#define PP_NUM_STATES			5
#define PP_STATE_ENTRY_SIZE		6
#define PP_CLOCK_INFO_SIZE		7
#define PP_NON_CLOCK_SIZE		8
#define PP_STATE_ARRAY_OFFSET		9
#define PP_CLOCK_INFO_ARRAY_OFFSET	11
#define PP_NON_CLOCK_ARRAY_OFFSET	13
#define PP_HEADER_SIZE			15

/* ATOM_PPLIB_EVERGREEN_CLOCK_INFO: clocks are 24 bits, low word first. */
#define PP_CLOCK_SCLK			0
#define PP_CLOCK_MCLK			3
#define PP_CLOCK_VDDC			6
#define PP_CLOCK_VDDCI			8
#define PP_EVERGREEN_CLOCK_SIZE		12

#define PP_CLASSIFICATION_BOOT		0x0008

/* ATOM_FIRMWARE_INFO_V2_2 */
#define FWI_DEFAULT_SCLK		8
#define FWI_DEFAULT_MCLK		12
#define FWI_BOOT_VDDC			46
#define FWI_V2_2_SIZE			48

static unsigned get_le16(const uint8_t *p)
{
	return p[0] | (p[1] << 8);
}

static uint32_t get_le24(const uint8_t *p)
{
	return p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16);
}

static uint32_t get_le32(const uint8_t *p)
{
	return get_le24(p) | ((uint32_t)p[3] << 24);
}

static const uint8_t *data_table(struct rdn_card *card, int index,
				 uint16_t *size)
{
	struct atom_context *ctx = card->atom.ctx;
	uint16_t offset;

	if (!atom_parse_data_header(ctx, index, size, NULL, NULL, &offset))
		return NULL;
	return (const uint8_t *)ctx->bios + offset;
}

int rdn_pm_boot_state(struct rdn_card *card, struct rdn_pm_state *state)
{
	uint16_t size = 0;
	const uint8_t *info = data_table(card,
		GetIndexIntoMasterTable(DATA, FirmwareInfo), &size);

	if (!info || size < FWI_V2_2_SIZE)
		return -EINVAL;
	memset(state, 0, sizeof(*state));
	state->sclk = get_le32(info + FWI_DEFAULT_SCLK);
	state->mclk = get_le32(info + FWI_DEFAULT_MCLK);
	state->vddc = (uint16_t)get_le16(info + FWI_BOOT_VDDC);
	return 0;
}

/*
 * Calls `visit` for every clock mode of every state but the boot state,
 * whose entry does not hold what the card boots with.
 */
static int pp_walk(struct rdn_card *card,
		   void (*visit)(void *arg, unsigned state, unsigned mode,
				 unsigned classification,
				 const struct rdn_pm_state *clocks),
		   void *arg)
{
	uint16_t size = 0;
	const uint8_t *pp = data_table(card,
		GetIndexIntoMasterTable(DATA, PowerPlayInfo), &size);
	unsigned states, entry, clock_size, non_clock_size;
	unsigned state_off, clock_off, non_clock_off, i, j;

	if (!pp || size < PP_HEADER_SIZE)
		return -EINVAL;
	states = pp[PP_NUM_STATES];
	entry = pp[PP_STATE_ENTRY_SIZE];
	clock_size = pp[PP_CLOCK_INFO_SIZE];
	non_clock_size = pp[PP_NON_CLOCK_SIZE];
	state_off = get_le16(pp + PP_STATE_ARRAY_OFFSET);
	clock_off = get_le16(pp + PP_CLOCK_INFO_ARRAY_OFFSET);
	non_clock_off = get_le16(pp + PP_NON_CLOCK_ARRAY_OFFSET);
	if (entry < 2 || clock_size < PP_EVERGREEN_CLOCK_SIZE ||
	    non_clock_size < 2 || state_off + states * entry > size)
		return -EINVAL;

	for (i = 0; i < states; i++) {
		const uint8_t *s = pp + state_off + i * entry;
		unsigned non_clock = non_clock_off + s[0] * non_clock_size;
		unsigned classification;

		if (non_clock + 2 > size)
			return -EINVAL;
		classification = get_le16(pp + non_clock);
		for (j = 1; j < entry; j++) {
			unsigned clock = clock_off + s[j] * clock_size;
			struct rdn_pm_state c;

			if (clock + PP_EVERGREEN_CLOCK_SIZE > size)
				return -EINVAL;
			c.sclk = get_le24(pp + clock + PP_CLOCK_SCLK);
			c.mclk = get_le24(pp + clock + PP_CLOCK_MCLK);
			c.vddc = (uint16_t)get_le16(pp + clock + PP_CLOCK_VDDC);
			c.vddci = (uint16_t)get_le16(pp + clock + PP_CLOCK_VDDCI);
			visit(arg, i, j - 1, classification, &c);
		}
	}
	return 0;
}

/* 0xff01 and its like are flags, not voltages (radeon_atombios.c). */
static bool is_voltage(unsigned mv)
{
	return mv && (mv & 0xff00) != 0xff00;
}

static void visit_best(void *arg, unsigned state, unsigned mode,
		       unsigned classification, const struct rdn_pm_state *c)
{
	struct rdn_pm_state *best = arg;

	if (classification & PP_CLASSIFICATION_BOOT)
		return;
	if (c->sclk > best->sclk ||
	    (c->sclk == best->sclk && c->mclk > best->mclk))
		*best = *c;
}

int rdn_pm_performance_state(struct rdn_card *card, struct rdn_pm_state *state)
{
	int r;

	memset(state, 0, sizeof(*state));
	r = pp_walk(card, visit_best, state);
	if (r)
		return r;
	if (!state->sclk || !state->mclk)
		return -EINVAL;
	if (!is_voltage(state->vddc) && card->family != RDN_FAMILY_CAYMAN)
		state->vddc = 0;
	if (!is_voltage(state->vddci))
		state->vddci = 0;
	return 0;
}

static void visit_log(void *arg, unsigned state, unsigned mode,
		      unsigned classification, const struct rdn_pm_state *c)
{
	struct rdn_card *card = arg;

	rdn_log(card->os, RDN_LOG_INFO,
		"power state %u (class 0x%04X) mode %u: engine %u0 kHz, memory %u0 kHz, vddc %u, vddci %u",
		state, classification, mode, (unsigned)c->sclk,
		(unsigned)c->mclk, c->vddc, c->vddci);
}

void rdn_pm_log_states(struct rdn_card *card)
{
	struct rdn_pm_state boot;

	if (!rdn_pm_boot_state(card, &boot))
		rdn_log(card->os, RDN_LOG_INFO,
			"boot state: engine %u0 kHz, memory %u0 kHz, vddc %u",
			(unsigned)boot.sclk, (unsigned)boot.mclk, boot.vddc);
	if (pp_walk(card, visit_log, card))
		rdn_log(card->os, RDN_LOG_ERROR, "PowerPlay table not understood");
}

int rdn_pm_bios_state(struct rdn_card *card, enum rdn_pm_bios_kind kind,
                      struct rdn_pm_bios_state *out)
{
 uint16_t size=0;
 const uint8_t *p=data_table(card,GetIndexIntoMasterTable(DATA,PowerPlayInfo),&size);
 struct rdn_pm_bios_state selected;
 if(!out || !p || size<33 || p[2]<4 || p[2]>5)return -EINVAL;
 unsigned entry=p[6],clockSize=p[7],nonSize=p[8];
 unsigned states=get_le16(p+9),clocks=get_le16(p+11),non=get_le16(p+13);
 if(entry<2 || entry-1>RDN_PM_BIOS_LEVELS || clockSize<PP_EVERGREEN_CLOCK_SIZE || nonSize<11 ||
    states+(uint32_t)p[5]*entry>size)return -EINVAL;
 for(unsigned i=0;i<p[5];i++) {
  const uint8_t *s=p+states+i*entry;
  unsigned offset=non+s[0]*nonSize;
  if(offset+nonSize>size)return -EINVAL;
  const uint8_t *n=p+offset;unsigned cls=get_le16(n),cls2=get_le16(n+9);
  bool match=kind==RDN_PM_BIOS_PERFORMANCE?((cls&7)==5 && !(cls&0x418)):
             kind==RDN_PM_BIOS_VIDEO?((cls&0x400)!=0):((cls2&2)!=0);
  if(!match)continue;
  memset(&selected,0,sizeof(selected));
  selected.classification=cls;selected.classification2=cls2;selected.caps=get_le32(n+4);
  if(nonSize>=19){selected.vclk=get_le32(n+11);selected.dclk=get_le32(n+15);}
  for(unsigned j=1;j<entry;j++) {
   offset=clocks+s[j]*clockSize;
   if(offset+PP_EVERGREEN_CLOCK_SIZE>size)return -EINVAL;
   const uint8_t *q=p+offset;struct rdn_pm_state *l=&selected.level[selected.count++];
   l->sclk=get_le24(q);l->mclk=get_le24(q+3);l->vddc=get_le16(q+6);l->vddci=get_le16(q+8);
   if(!l->sclk || !l->mclk || rdn_pm_resolve_state(card,l))return -EINVAL;
   if(j>1 && (l->sclk<selected.level[j-2].sclk || l->mclk<selected.level[j-2].mclk))return -EINVAL;
  }
  *out=selected;return 0;
 }
 return -EINVAL;
}

struct idle_selection {
	struct rdn_pm_state *state;
	uint32_t mclk;
};

static void visit_idle(void *arg, unsigned state, unsigned mode,
		       unsigned classification, const struct rdn_pm_state *c)
{
	struct idle_selection *selection = arg;
	struct rdn_pm_state *idle = selection->state;
	if (!(classification & 1) || !c->sclk || !c->mclk || !is_voltage(c->vddc) ||
	    (selection->mclk && c->mclk != selection->mclk))
		return;
	if (!idle->sclk || c->sclk < idle->sclk ||
	    (c->sclk == idle->sclk && c->mclk < idle->mclk))
		*idle = *c;
}

int rdn_pm_display_idle_state(struct rdn_card *card, uint32_t mclk,
			      struct rdn_pm_state *state)
{
	struct idle_selection selection;
	int r;
	memset(state, 0, sizeof(*state));
	selection.state = state;
	selection.mclk = mclk;
	r = pp_walk(card, visit_idle, &selection);
	return r ? r : (!state->sclk || !state->mclk ? -EINVAL : 0);
}

int rdn_pm_idle_state(struct rdn_card *card, struct rdn_pm_state *state)
{
	return rdn_pm_display_idle_state(card, 0, state);
}

/*
 * One 32-bit parameter, little-endian in the interpreter's word, and the
 * same word back.
 */
static int exec32(struct rdn_card *card, int index, uint32_t *value)
{
	uint32_t ps[4];
	int r;

	memset(ps, 0, sizeof(ps));
	ps[0] = cpu_to_le32(*value);
	r = atom_execute_table(card->atom.ctx, index, ps, 4);
	*value = le32_to_cpu(ps[0]);
	if (r)
		rdn_log(card->os, RDN_LOG_ERROR,
			"AtomBIOS command table %d failed (%d)", index, r);
	return r;
}

int rdn_pm_resolve_state(struct rdn_card *card, struct rdn_pm_state *state)
{
	struct rdn_pm_state resolved = *state;
	uint8_t frev, crev;
	uint32_t ps;
	int r, index = GetIndexIntoMasterTable(COMMAND, SetVoltage);
	if ((resolved.vddci & 0xff00) == 0xff00)
		return -EINVAL;
	if ((resolved.vddc & 0xff00) != 0xff00)
		return 0;
	if (resolved.vddc != ATOM_VIRTUAL_VOLTAGE_ID0 ||
	    !atom_parse_cmd_header(card->atom.ctx, index, &frev, &crev))
		return -EINVAL;
	if (frev != 1 || (crev != 2 && crev != 3))
		return -EINVAL;
	/* NI uses the ASIC maximum for virtual voltage ID0, as rv770_get_max_vddc(). */
	ps = crev == 2 ? SET_VOLTAGE_GET_MAX_VOLTAGE :
		VOLTAGE_TYPE_VDDC | (ATOM_GET_VOLTAGE_LEVEL << 8);
	r = exec32(card, index, &ps);
	if (r)
		return r;
	resolved.vddc = ps >> 16;
	if (!is_voltage(resolved.vddc) || resolved.vddc < 500 || resolved.vddc > 1500)
		return -EINVAL;
	*state = resolved;
	return 0;
}

bool rdn_pm_memory_clock_ready(struct rdn_card *card)
{
 uint32_t type=(rdn_rreg(card,MC_SEQ_MISC0)&MC_SEQ_MISC0_GDDR5_MASK)>>MC_SEQ_MISC0_GDDR5_SHIFT;
 return type!=MC_SEQ_MISC0_GDDR5_VALUE || (rdn_rreg(card,0x28c8)&1);
}

int rdn_pm_tuning_state(struct rdn_card *card,unsigned core,unsigned memory,struct rdn_pm_state *out)
{
 struct rdn_pm_state stock;int err;
 if(!out || card->family!=RDN_FAMILY_TURKS ||
    (rdn_rreg(card,MC_SEQ_MISC0)&MC_SEQ_MISC0_GDDR5_MASK)==(MC_SEQ_MISC0_GDDR5_VALUE<<MC_SEQ_MISC0_GDDR5_SHIFT))return -EINVAL;
 err=rdn_pm_performance_state(card,&stock);if(err)return err;
 err=rdn_pm_resolve_state(card,&stock);if(err)return err;
 if(core>10000||memory>10000 || core*100<stock.sclk || memory*100<stock.mclk ||
    core*100>stock.sclk*105/100 || memory*100>stock.mclk*105/100 || !is_voltage(stock.vddc))return -EINVAL;
 stock.sclk=core*100;stock.mclk=memory*100;*out=stock;return 0;
}

int rdn_pm_get_clocks(struct rdn_card *card, uint32_t *sclk, uint32_t *mclk)
{
	int r;

	*sclk = *mclk = 0;
	r = exec32(card, GetIndexIntoMasterTable(COMMAND, GetEngineClock), sclk);
	if (r)
		return r;
	return exec32(card, GetIndexIntoMasterTable(COMMAND, GetMemoryClock), mclk);
}

/* radeon_atom_set_voltage() */
static int set_voltage(struct rdn_card *card, unsigned mv, unsigned type)
{
	int index = GetIndexIntoMasterTable(COMMAND, SetVoltage);
	uint8_t frev = 0, crev = 0;
	uint32_t ps;

	if (!atom_parse_cmd_header(card->atom.ctx, index, &frev, &crev))
		return -EINVAL;
	/*
	 * Revisions 2 and 3 take the same block: type, mode (0: set), level
	 * in mV as a little-endian word. Revision 1 takes an index instead.
	 */
	if (crev != 2 && crev != 3) {
		rdn_log(card->os, RDN_LOG_ERROR,
			"SetVoltage is revision %d.%d, need 1.2 or 1.3", frev, crev);
		return -EINVAL;
	}
	ps = type | ((uint32_t)mv << 16);
	return exec32(card, index, &ps);
}

static int set_voltages(struct rdn_card *card, const struct rdn_pm_state *from,
			const struct rdn_pm_state *to)
{
	int r;

	if (is_voltage(to->vddc) && to->vddc != from->vddc) {
		r = set_voltage(card, to->vddc, SET_VOLTAGE_TYPE_ASIC_VDDC);
		if (r)
			return r;
	}
	if (is_voltage(to->vddci) && to->vddci != from->vddci) {
		r = set_voltage(card, to->vddci, SET_VOLTAGE_TYPE_ASIC_VDDCI);
		if (r)
			return r;
	}
	return 0;
}

/* evergreen_pm_prepare() and evergreen_pm_finish(), for the one CRTC in use. */
static void crtc_read_requests(struct rdn_card *card, bool on)
{
	uint32_t tmp;

	if (!card->crtc_on)
		return;
	tmp = rdn_rreg(card, EVERGREEN_CRTC_CONTROL);
	if (on)
		tmp &= ~EVERGREEN_CRTC_DISP_READ_REQUEST_DISABLE;
	else
		tmp |= EVERGREEN_CRTC_DISP_READ_REQUEST_DISABLE;
	rdn_wreg(card, EVERGREEN_CRTC_CONTROL, tmp);
}

int rdn_pm_set(struct rdn_card *card, const struct rdn_pm_state *from,
	       const struct rdn_pm_state *to, unsigned what)
{
	/* upvolt before raising clocks, downvolt after lowering clocks */
	struct rdn_pm_state resolved = *to;
	bool voltage_after, memory_change;
	uint32_t crtc_control = 0;
	uint32_t v, sclk, mclk;
	int r;
	if (!rdn_family_has_acceleration(card->family))
		return -EINVAL;
	r = rdn_pm_resolve_state(card, &resolved);
	if (r)
		return r;
	to = &resolved;
	voltage_after = to->sclk < from->sclk;

	if (rdn_rreg(card, GRBM_STATUS) & GUI_ACTIVE)
		return -EBUSY;

	memory_change = (what & RDN_PM_MCLK) && to->mclk != from->mclk;
	if (memory_change && card->crtc_on) {
		crtc_control = rdn_rreg(card, EVERGREEN_CRTC_CONTROL);
		crtc_read_requests(card, false);
	}
	/* Lower-clock latency must be covered before the clock drops. */
	if ((what & RDN_PM_SCLK) && to->sclk < from->sclk) {
		card->sclk = to->sclk;
		card->mclk = from->mclk;
		rdn_bandwidth_update_clocks(card);
	}

	if ((what & RDN_PM_VOLTAGE) && !voltage_after)
		r = set_voltages(card, from, to);
	if (!r && (what & RDN_PM_SCLK) && to->sclk != from->sclk) {
		v = to->sclk;
		r = exec32(card, GetIndexIntoMasterTable(COMMAND, SetEngineClock), &v);
	}
	if (!r && (what & RDN_PM_MCLK) && to->mclk != from->mclk) {
		v = to->mclk;
		r = exec32(card, GetIndexIntoMasterTable(COMMAND, SetMemoryClock), &v);
	}
	if (!r && (what & RDN_PM_VOLTAGE) && voltage_after)
		r = set_voltages(card, from, to);

	if (memory_change && card->crtc_on)
		rdn_wreg(card, EVERGREEN_CRTC_CONTROL, crtc_control);

	/* The display's watermarks depend on both clocks: read them back. */
	if (!rdn_pm_get_clocks(card, &sclk, &mclk) && sclk && mclk) {
		/* The VBIOS reads 649.96 MHz for 650: round to the MHz. */
		card->sclk = (sclk + 50) / 100 * 100;
		card->mclk = (mclk + 50) / 100 * 100;
		rdn_bandwidth_update_clocks(card);
	}
	return r;
}

int rdn_pm_fan_percent(struct rdn_card *card, unsigned percent)
{
	uint32_t control0, control2, duty100, value0, value2;
	if (card->family != RDN_FAMILY_CAYMAN || percent < 60 || percent > 100)
		return -EINVAL;
	control0 = rdn_rreg(card, 0x76c);
	control2 = rdn_rreg(card, 0x774);
	duty100 = rdn_rreg(card, 0x770) & 0xff;
	if (!duty100 || control0 == 0xffffffff || control2 == 0xffffffff)
		return -EINVAL;
	/* Cayman SpeedFanControl register bank; static PWM uses the BIOS duty scale. */
	value0 = (control0 & ~0xffu) | ((duty100 * percent + 99) / 100);
	value2 = (control2 & ~(0xffu | (7u << 11))) | (1u << 11);
	rdn_wreg(card, 0x76c, value0);
	rdn_wreg(card, 0x774, value2);
	if (rdn_rreg(card, 0x76c) != value0 || rdn_rreg(card, 0x774) != value2) {
		rdn_wreg(card, 0x774, control2);
		rdn_wreg(card, 0x76c, control0);
		return -5;
	}
	return 0;
}

int rdn_pm_fan_full(struct rdn_card *card)
{
 return rdn_pm_fan_percent(card,100);
}

/* evergreen_get_temp(), the branch for everything but Juniper */
int rdn_pm_temperature(struct rdn_card *card)
{
	uint32_t temp = (rdn_rreg(card, CG_MULT_THERMAL_STATUS) & ASIC_T_MASK) >>
			ASIC_T_SHIFT;
	int actual;

	if (temp & 0x400)
		actual = -256;
	else if (temp & 0x200)
		actual = 255;
	else if (temp & 0x100)
		actual = (int)(temp & 0x1ff) - 0x200;
	else
		actual = temp & 0xff;
	return actual * 1000 / 2;
}

void rdn_pm_board_sensors(struct rdn_card *card, struct rdn_user_board_sensors *out)
{
 uint16_t size = 0;
 const uint8_t *p;
 memset(out, 0, sizeof(*out));
 out->version = 1;
 out->size = sizeof(*out);
 out->fan_status = RDN_SENSOR_NO_DESCRIPTION;
 out->rail_status = RDN_SENSOR_NO_DESCRIPTION;
 out->voltage_control_id = 0xffffffff;
 p = data_table(card, GetIndexIntoMasterTable(DATA, VoltageObjectInfo), &size);
 if (p && size >= 4 && p[2] == 1 && p[3] == 2) {
  unsigned off = 4;
  while (off + 18 <= size) {
   unsigned bytes = p[off + 1];
   if (bytes < 18 || bytes > size - off) break;
   if (p[off] == SET_VOLTAGE_TYPE_ASIC_VDDC) {
    out->voltage_control_id = p[off + 2];
    /* A voltage programming interface does not establish an ADC channel. */
    out->rail_status = RDN_SENSOR_NOT_IMPLEMENTED;
    break;
   }
   off += bytes;
  }
 }
 p = data_table(card, GetIndexIntoMasterTable(DATA, PowerPlayInfo), &size);
 if (!p || size < 33 || p[2] < 4 || p[2] > 5) return;
 out->thermal_controller = p[25];
 out->fan_pulses = p[28] & 15;
 out->fan_min_rpm = p[29] * 100;
 out->fan_max_rpm = p[30] * 100;
 if ((p[28] & 0x80) || !out->fan_pulses) {
  out->fan_status = RDN_SENSOR_NOT_PRESENT;
  return;
 }
 out->fan_status = RDN_SENSOR_NOT_IMPLEMENTED;
 if (card->family != RDN_FAMILY_CAYMAN || p[25] != 15) return;
 out->fan_status = RDN_SENSOR_INVALID;
 p = data_table(card, GetIndexIntoMasterTable(DATA, FirmwareInfo), &size);
 if (!p || p[2] != 2 || p[3] != 2 || size < sizeof(ATOM_FIRMWARE_INFO_V2_2)) return;
 uint32_t ref = get_le16(p + offsetof(ATOM_FIRMWARE_INFO_V2_2, usCoreReferenceClock));
 uint32_t pin = rdn_rreg(card, 0x660);
 if (ref < 1000 || ref > 10000 || (pin & (1u << 8))) return;
 out->tach_clock_hz = ref * 10000;
 if (pin & (1u << 9)) out->tach_clock_hz /= 4;
 /* Cayman SpeedFanControl programs 0x788; 0x78c is its period counter. */
 uint32_t control = rdn_rreg(card, 0x788);
 out->tach_edges = (control & 7) + 1;
 out->tach_period = rdn_rreg(card, 0x78c);
 if (control == 0xffffffff || !out->tach_period || out->tach_period == 0xffffffff ||
     out->tach_edges != out->fan_pulses) return;
 uint64_t rpm = ((uint64_t)out->tach_clock_hz * 60 + out->tach_period / 2) / out->tach_period;
 if (!rpm || rpm > 30000) return;
 out->fan_rpm = (uint32_t)rpm;
 out->fan_status = RDN_SENSOR_VALID;
}
