/* SPDX-License-Identifier: MIT */
#ifndef RDN_DEVICE_IDS_H
#define RDN_DEVICE_IDS_H

#include <stdint.h>

enum rdn_family {
	RDN_FAMILY_TURKS, RDN_FAMILY_CAYMAN, RDN_FAMILY_UNKNOWN,
	RDN_FAMILY_BARTS, RDN_FAMILY_CAICOS,
	RDN_FAMILY_REDWOOD, RDN_FAMILY_CEDAR, RDN_FAMILY_JUNIPER,
	RDN_FAMILY_CYPRESS, RDN_FAMILY_ARUBA
};

enum rdn_engine {
	RDN_ENGINE_NONE, RDN_ENGINE_EVERGREEN, RDN_ENGINE_CAYMAN
};

struct rdn_family_caps {
	const char *name;
	unsigned gfx_level, dce_version, crtc_count;
	enum rdn_engine engine;
	unsigned pfp_words, me_words, rlc_words, mc_words, dma_engines;
	unsigned integrated;
};

/* Engine NONE keeps known silicon separate from implemented acceleration. */
#define RDN_FAMILY_CAPS(X) \
	X(TURKS,   4, 50, 6, EVERGREEN, 1120, 1376, 768, 6024, 1, 0) \
	X(CAYMAN,  5, 50, 6, CAYMAN,    2176, 2176, 1024, 6037, 2, 0) \
	X(BARTS,   4, 50, 6, NONE,         0,    0,   0,    0, 0, 0) \
	X(CAICOS,  4, 50, 4, NONE,         0,    0,   0,    0, 0, 0) \
	X(REDWOOD, 4, 40, 6, NONE,         0,    0,   0,    0, 0, 0) \
	X(CEDAR,   4, 40, 4, NONE,         0,    0,   0,    0, 0, 0) \
	X(JUNIPER, 4, 40, 6, NONE,         0,    0,   0,    0, 0, 0) \
	X(CYPRESS, 4, 40, 6, NONE,         0,    0,   0,    0, 0, 0) \
	X(ARUBA,   5, 61, 4, NONE,         0,    0,   0,    0, 0, 1)

static inline const struct rdn_family_caps *rdn_family_caps(enum rdn_family family)
{
	static const struct rdn_family_caps unknown = {"UNKNOWN", 0, 0, 0,
		RDN_ENGINE_NONE, 0, 0, 0, 0, 0, 0};
	switch (family) {
#define RDN_CAPS_CASE(name, gfx, dce, crtcs, engine, pfp, me, rlc, mc, dma, igp) \
	case RDN_FAMILY_##name: { \
		static const struct rdn_family_caps caps = {#name, gfx, dce, crtcs, \
			RDN_ENGINE_##engine, pfp, me, rlc, mc, dma, igp}; \
		return &caps; \
	}
	RDN_FAMILY_CAPS(RDN_CAPS_CASE)
#undef RDN_CAPS_CASE
	default: return &unknown;
	}
}

static inline enum rdn_engine rdn_family_engine(enum rdn_family family)
{
	switch (family) {
#define RDN_ENGINE_CASE(name, gfx, dce, crtcs, engine, pfp, me, rlc, mc, dma, igp) \
	case RDN_FAMILY_##name: return RDN_ENGINE_##engine;
	RDN_FAMILY_CAPS(RDN_ENGINE_CASE)
#undef RDN_ENGINE_CASE
	default: return RDN_ENGINE_NONE;
	}
}

/* Linux v6.12 include/drm/drm_pciids.h: family and mobility, not a board rating. */
#define RDN_PCI_DEVICES(X) \
	X(0x6740, RDN_FAMILY_TURKS, 1) \
	X(0x6741, RDN_FAMILY_TURKS, 1) \
	X(0x6742, RDN_FAMILY_TURKS, 1) \
	X(0x6743, RDN_FAMILY_TURKS, 1) \
	X(0x6744, RDN_FAMILY_TURKS, 1) \
	X(0x6745, RDN_FAMILY_TURKS, 1) \
	X(0x6746, RDN_FAMILY_TURKS, 0) \
	X(0x6747, RDN_FAMILY_TURKS, 0) \
	X(0x6748, RDN_FAMILY_TURKS, 0) \
	X(0x6749, RDN_FAMILY_TURKS, 0) \
	X(0x674a, RDN_FAMILY_TURKS, 0) \
	X(0x6750, RDN_FAMILY_TURKS, 0) \
	X(0x6751, RDN_FAMILY_TURKS, 0) \
	X(0x6758, RDN_FAMILY_TURKS, 0) \
	X(0x6759, RDN_FAMILY_TURKS, 0) \
	X(0x675b, RDN_FAMILY_TURKS, 0) \
	X(0x675d, RDN_FAMILY_TURKS, 0) \
	X(0x675f, RDN_FAMILY_TURKS, 0) \
	X(0x6840, RDN_FAMILY_TURKS, 1) \
	X(0x6841, RDN_FAMILY_TURKS, 1) \
	X(0x6842, RDN_FAMILY_TURKS, 1) \
	X(0x6843, RDN_FAMILY_TURKS, 1) \
	X(0x6849, RDN_FAMILY_TURKS, 0) \
	X(0x6850, RDN_FAMILY_TURKS, 0) \
	X(0x6858, RDN_FAMILY_TURKS, 0) \
	X(0x6859, RDN_FAMILY_TURKS, 0) \
	X(0x6700, RDN_FAMILY_CAYMAN, 0) \
	X(0x6701, RDN_FAMILY_CAYMAN, 0) \
	X(0x6702, RDN_FAMILY_CAYMAN, 0) \
	X(0x6703, RDN_FAMILY_CAYMAN, 0) \
	X(0x6704, RDN_FAMILY_CAYMAN, 0) \
	X(0x6705, RDN_FAMILY_CAYMAN, 0) \
	X(0x6706, RDN_FAMILY_CAYMAN, 0) \
	X(0x6707, RDN_FAMILY_CAYMAN, 0) \
	X(0x6708, RDN_FAMILY_CAYMAN, 0) \
	X(0x6709, RDN_FAMILY_CAYMAN, 0) \
	X(0x6718, RDN_FAMILY_CAYMAN, 0) \
	X(0x6719, RDN_FAMILY_CAYMAN, 0) \
	X(0x671c, RDN_FAMILY_CAYMAN, 0) \
	X(0x671d, RDN_FAMILY_CAYMAN, 0) \
	X(0x671f, RDN_FAMILY_CAYMAN, 0) \
	X(0x6720, RDN_FAMILY_BARTS, 1) \
	X(0x6721, RDN_FAMILY_BARTS, 1) \
	X(0x6722, RDN_FAMILY_BARTS, 0) \
	X(0x6723, RDN_FAMILY_BARTS, 0) \
	X(0x6724, RDN_FAMILY_BARTS, 1) \
	X(0x6725, RDN_FAMILY_BARTS, 1) \
	X(0x6726, RDN_FAMILY_BARTS, 0) \
	X(0x6727, RDN_FAMILY_BARTS, 0) \
	X(0x6728, RDN_FAMILY_BARTS, 0) \
	X(0x6729, RDN_FAMILY_BARTS, 0) \
	X(0x6738, RDN_FAMILY_BARTS, 0) \
	X(0x6739, RDN_FAMILY_BARTS, 0) \
	X(0x673e, RDN_FAMILY_BARTS, 0) \
	X(0x6760, RDN_FAMILY_CAICOS, 1) \
	X(0x6761, RDN_FAMILY_CAICOS, 1) \
	X(0x6762, RDN_FAMILY_CAICOS, 0) \
	X(0x6763, RDN_FAMILY_CAICOS, 0) \
	X(0x6764, RDN_FAMILY_CAICOS, 1) \
	X(0x6765, RDN_FAMILY_CAICOS, 1) \
	X(0x6766, RDN_FAMILY_CAICOS, 0) \
	X(0x6767, RDN_FAMILY_CAICOS, 0) \
	X(0x6768, RDN_FAMILY_CAICOS, 0) \
	X(0x6770, RDN_FAMILY_CAICOS, 0) \
	X(0x6771, RDN_FAMILY_CAICOS, 0) \
	X(0x6772, RDN_FAMILY_CAICOS, 0) \
	X(0x6778, RDN_FAMILY_CAICOS, 0) \
	X(0x6779, RDN_FAMILY_CAICOS, 0) \
	X(0x677b, RDN_FAMILY_CAICOS, 0)

static inline enum rdn_family rdn_family_from_device(uint16_t device)
{
	switch (device) {
#define RDN_DEVICE_CASE(id, family, mobile) case id: return family;
	RDN_PCI_DEVICES(RDN_DEVICE_CASE)
#undef RDN_DEVICE_CASE
	default: return RDN_FAMILY_UNKNOWN;
	}
}

static inline enum rdn_family rdn_family_from_pci(uint32_t pci)
{
	return (pci & 0xffff) == 0x1002 ?
		rdn_family_from_device((uint16_t)(pci >> 16)) : RDN_FAMILY_UNKNOWN;
}

static inline int rdn_device_is_mobile(uint16_t device)
{
	switch (device) {
#define RDN_MOBILE_CASE(id, family, mobile) case id: return mobile;
	RDN_PCI_DEVICES(RDN_MOBILE_CASE)
#undef RDN_MOBILE_CASE
	default: return 0;
	}
}

static inline const char *rdn_family_name(enum rdn_family family)
{
	return rdn_family_caps(family)->name;
}

static inline unsigned rdn_family_crtc_count(enum rdn_family family)
{
	return rdn_family_caps(family)->crtc_count;
}

static inline int rdn_family_has_acceleration(enum rdn_family family)
{
	return rdn_family_engine(family) != RDN_ENGINE_NONE;
}

#endif
