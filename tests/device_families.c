/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include "../hw/rdn_device_ids.h"

int main(void)
{
    unsigned device, known = 0, desktop = 0;
    const enum rdn_family pending[] = {
        RDN_FAMILY_BARTS, RDN_FAMILY_CAICOS, RDN_FAMILY_REDWOOD,
        RDN_FAMILY_CEDAR, RDN_FAMILY_JUNIPER, RDN_FAMILY_CYPRESS,
        RDN_FAMILY_ARUBA, RDN_FAMILY_UNKNOWN, (enum rdn_family)-1,
        (enum rdn_family)999
    };
    unsigned i;
    for (i = 0; i < sizeof(pending) / sizeof(pending[0]); i++) {
        const struct rdn_family_caps *caps = rdn_family_caps(pending[i]);
        assert(!rdn_family_has_acceleration(pending[i]));
        assert(caps->engine == RDN_ENGINE_NONE);
        assert(!caps->pfp_words && !caps->me_words && !caps->rlc_words);
        assert(!caps->mc_words && !caps->dma_engines);
    }
    assert(rdn_family_engine(RDN_FAMILY_TURKS) == RDN_ENGINE_EVERGREEN);
    assert(rdn_family_engine(RDN_FAMILY_CAYMAN) == RDN_ENGINE_CAYMAN);
    assert(rdn_family_caps(RDN_FAMILY_TURKS)->pfp_words == 1120);
    assert(rdn_family_caps(RDN_FAMILY_TURKS)->me_words == 1376);
    assert(rdn_family_caps(RDN_FAMILY_CAYMAN)->pfp_words == 2176);
    assert(rdn_family_caps(RDN_FAMILY_CAYMAN)->mc_words == 6037);
    assert(rdn_family_caps(RDN_FAMILY_REDWOOD)->dce_version == 40);
    assert(rdn_family_crtc_count(RDN_FAMILY_CEDAR) == 4);
    assert(rdn_family_crtc_count(RDN_FAMILY_CYPRESS) == 6);
    assert(rdn_family_caps(RDN_FAMILY_ARUBA)->integrated);
    assert(rdn_family_caps(RDN_FAMILY_ARUBA)->dce_version == 61);
    assert(!rdn_family_caps(RDN_FAMILY_UNKNOWN)->gfx_level);
    for (device = 0; device <= 0xffff; device++) {
        enum rdn_family family = rdn_family_from_device((uint16_t)device);
        if (family != RDN_FAMILY_UNKNOWN) {
            known++;
            desktop += !rdn_device_is_mobile((uint16_t)device);
            assert(rdn_family_from_pci((device << 16) | 0x1002) == family);
            assert(rdn_family_from_pci((device << 16) | 0x10de) == RDN_FAMILY_UNKNOWN);
        }
    }
    assert(known == 69 && desktop == 51);
    assert(rdn_family_from_device(0x675d) == RDN_FAMILY_TURKS);
    assert(rdn_family_from_device(0x6858) == RDN_FAMILY_TURKS);
    assert(rdn_family_from_device(0x6718) == RDN_FAMILY_CAYMAN);
    assert(rdn_family_from_device(0x6719) == RDN_FAMILY_CAYMAN);
    assert(rdn_family_from_device(0x671a) == RDN_FAMILY_UNKNOWN);
    assert(rdn_family_from_device(0x6738) == RDN_FAMILY_BARTS);
    assert(rdn_family_from_device(0x6779) == RDN_FAMILY_CAICOS);
    assert(rdn_family_from_device(0x684c) == RDN_FAMILY_UNKNOWN);
    assert(rdn_device_is_mobile(0x6740));
    assert(!rdn_device_is_mobile(0x6746));
    puts("PASS all 65536 device IDs, 69 classified, 51 desktop; unsupported families rejected");
    return 0;
}
