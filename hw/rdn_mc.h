/*
 * The memory controller: its microcode, and where video memory sits in its address space.
 *
 * Copyright (c) 2026 kouta-kun and Claude
 * SPDX-License-Identifier: MIT
 */

#ifndef RDN_MC_H
#define RDN_MC_H

#include "rdn_card.h"

/* TURKS_mc.bin: this many big-endian words. */
#define RDN_MC_UCODE_WORDS	6024

/*
 * Load the microcode of the memory controller's sequencer and let it train
 * the memory, as Linux does on a card with GDDR5 whose sequencer is not
 * running. Video memory is reset by this: call it after ASIC_Init and
 * before anything is kept there, with no CRTC scanning out.
 *
 * Returns 0 when the microcode was loaded and the training finished, 1
 * when there was nothing to do (not GDDR5, or the sequencer runs already),
 * or a negative errno value.
 */
int rdn_mc_load_microcode(struct rdn_card *card, const void *fw, size_t size);

/*
 * Move video memory to `new_base` in the GPU's address space, as radeon's evergreen_mc_stop(), evergreen_mc_program()
 * and evergreen_mc_resume() do: blank the display controllers, black out the memory controller, set
 * MC_VM_FB_LOCATION, the system aperture, the CPU aperture's base (HDP_NONSURFACE_*) and the AGP registers, point
 * every display controller's surface at the new base, unblack. The BIOS puts video memory at 0xF00000000 on this
 * card; the UVD block's firmware window only works below 4 GB (docs/UVD.md), where radeon puts it (address 0).
 * Everything in this library reads MC_VM_FB_LOCATION when it needs the base, so the move is invisible to the
 * rest, but call it before the mode set and before anything is kept in video memory: the display controllers
 * stay off afterwards. `new_base` is a multiple of 16 MB. The card must be posted.
 *
 * Returns 0, 1 when video memory is there already, or a negative errno value.
 */
int rdn_mc_relocate(struct rdn_card *card, uint64_t new_base);

/* Test aid: the display controllers stop reading video memory (hold) or resume (!hold). */
void rdn_display_hold(struct rdn_card *card, bool hold);

#endif /* RDN_MC_H */
