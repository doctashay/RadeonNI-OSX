# Host-side build: hardware library, Linux tools and tests.
#
#   make            build everything for x86 and big-endian PowerPC
#   make test       run the tests natively and under qemu-ppc
#
# The kext is not built here; it is built inside the Tiger guest.

PPC_CC   ?= third_party/ppc-toolchain/bin/powerpc-linux-gcc
QEMU_PPC ?= third_party/qemu/qemu-ppc

VBIOS      ?= private/vbios.rom
REF_PHASE  ?= traces/ref-radeon-2.phase-a1.txt
EDID       ?= private/monitor-edid.bin
# A Linux modeset to the EDID's preferred mode (phase a4 of the trace guest)
REF_MODESET ?= traces/ref-radeon-3.phase-a4.txt
# The modeset before it in the same run: from the preferred mode to the
# EDID's second detailed timing (phase a3)
REF_MODESET2 ?= traces/ref-radeon-3.phase-a3.txt
# The same guest's driver start, for the one-time display setup
REF_INIT   ?= traces/ref-radeon-3.phase-a1.txt

FW_PFP     ?= firmware/TURKS_pfp.bin
FW_ME      ?= firmware/TURKS_me.bin
FW_MC      ?= firmware/TURKS_mc.bin

CFLAGS_COMMON = -std=gnu99 -O2 -g -Wall -Wextra -Wno-unused-parameter

# Files taken from Linux are built with the warnings they do not pass
# switched off, so that our own code can keep -Wextra.
CFLAGS_UPSTREAM = $(CFLAGS_COMMON) -Wno-sign-compare -Wno-type-limits \
	-Wno-unused-variable -Wno-unused-but-set-variable

HW_OBJS   = hw/rdn_uvd.o hw/rdn_gart.o hw/rdn_mc.o hw/rdn_watermark.o hw/rdn_cayman_dpm.o hw/rdn_smc.o hw/rdn_pm.o hw/rdn_cursor.o hw/rdn_mem.o hw/rdn_gpu.o hw/rdn_cp.o hw/rdn_blit.o hw/rdn_selftest.o hw/rdn_pattern.o hw/rdn_modeset.o hw/rdn_dp.o hw/rdn_mode.o hw/rdn_dmt.o hw/rdn_i2c.o hw/rdn_post.o hw/rdn_atom.o hw/atom/atom.o
HW_HDRS   = $(wildcard hw/*.h hw/atom/*.h hw/linux/*.h)
HW_OBJS  += hw/rdn_cayman_gating.o hw/rdn_cayman_memory.o hw/rdn_mc_layout.o hw/rdn_cayman.o hw/rdn_dma.o

TESTS     = atom_replay edid_modes i2c_edid board_outputs dp_link modeset_replay accel_replay blit_ops mem_alloc pm_states mc_replay gart_replay handover engine_detect arb_alias d3d_ps
TESTS    += family_framebuffer transmitter_init device_families cayman_gating_model cayman_dpm_model smc_model pm_virtual uvd_model uvd_guarded_model mc_layout_model display_banks gart_scratch mem_segment mem_range_compat

X86_TESTS = $(addprefix build/x86/,$(TESTS))
PPC_TESTS = $(addprefix build/ppc/,$(TESTS))

# The tool talks to the real card through sysfs, so it is only useful on the
# x86 host. The PowerPC build is there to keep it compiling big-endian.
all: $(X86_TESTS) $(PPC_TESTS) build/x86/rdn_tool build/ppc/rdn_tool

build/x86/rominfo: build/x86/tools/rominfo.o $(addprefix build/x86/,$(HW_OBJS))
	$(CC) -o $@ $^

build/ppc/rominfo: build/ppc/tools/rominfo.o $(addprefix build/ppc/,$(HW_OBJS))
	$(PPC_CC) -static -o $@ $^

build/x86/family_framebuffer: build/x86/tests/family_framebuffer.o $(addprefix build/x86/,$(HW_OBJS))
	$(CC) -Wl,--wrap=atom_asic_init -o $@ $^

build/ppc/family_framebuffer: build/ppc/tests/family_framebuffer.o $(addprefix build/ppc/,$(HW_OBJS))
	$(PPC_CC) -static -Wl,--wrap=atom_asic_init -o $@ $^

TRANSMITTER_WRAPS = -Wl,--wrap=atom_execute_table -Wl,--wrap=atom_parse_cmd_header \
	-Wl,--wrap=atom_parse_data_header

build/x86/transmitter_init: build/x86/tests/transmitter_init.o $(addprefix build/x86/,$(HW_OBJS))
	$(CC) $(TRANSMITTER_WRAPS) -o $@ $^

build/ppc/transmitter_init: build/ppc/tests/transmitter_init.o $(addprefix build/ppc/,$(HW_OBJS))
	$(PPC_CC) -static $(TRANSMITTER_WRAPS) -o $@ $^

build/x86/pm_virtual: build/x86/tests/pm_virtual.o $(addprefix build/x86/,$(HW_OBJS))
	$(CC) -Wl,--wrap=atom_execute_table -o $@ $^

build/ppc/pm_virtual: build/ppc/tests/pm_virtual.o $(addprefix build/ppc/,$(HW_OBJS))
	$(PPC_CC) -static -Wl,--wrap=atom_execute_table -o $@ $^

build/x86/rdn_tool: build/x86/tools/rdn_tool.o $(addprefix build/x86/,$(HW_OBJS))
	$(CC) -o $@ $^

build/ppc/rdn_tool: build/ppc/tools/rdn_tool.o $(addprefix build/ppc/,$(HW_OBJS))
	$(PPC_CC) -static -o $@ $^

build/x86/hw/atom/%.o: hw/atom/%.c $(HW_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_UPSTREAM) -c -o $@ $<

build/ppc/hw/atom/%.o: hw/atom/%.c $(HW_HDRS)
	@mkdir -p $(dir $@)
	$(PPC_CC) $(CFLAGS_UPSTREAM) -c -o $@ $<

build/x86/%.o: %.c $(HW_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_COMMON) -c -o $@ $<

build/ppc/%.o: %.c $(HW_HDRS)
	@mkdir -p $(dir $@)
	$(PPC_CC) $(CFLAGS_COMMON) -c -o $@ $<

build/x86/%: build/x86/tests/%.o $(addprefix build/x86/,$(HW_OBJS))
	$(CC) -o $@ $^

build/ppc/%: build/ppc/tests/%.o $(addprefix build/ppc/,$(HW_OBJS))
	$(PPC_CC) -static -o $@ $^

.SECONDARY:

# The inputs are not in the repository (VBIOS dump, reference trace). Without
# them the tests are skipped, loudly. Each test runs natively and under
# qemu-ppc; the two outputs must be identical and end in a PASS line.
run_test = \
	x86=$$(build/x86/$(1) $(2)); ppc=$$($(QEMU_PPC) build/ppc/$(1) $(2)); \
	echo "x86 $(1): $$(echo "$$x86" | tail -1)"; \
	echo "ppc $(1): $$(echo "$$ppc" | tail -1)"; \
	[ "$$x86" = "$$ppc" ] || { echo "FAIL $(1): x86 and ppc differ"; exit 1; }; \
	case "$$(echo "$$x86" | tail -1)" in PASS*) ;; *) echo "$$x86"; exit 1 ;; esac

test-uvd: build/x86/uvd_guarded_model build/ppc/uvd_guarded_model
	@set -e; $(call run_test,uvd_guarded_model,)

test-mc-layout: build/x86/mc_layout_model build/ppc/mc_layout_model
	@set -e; $(call run_test,mc_layout_model,)

test-gart-scratch: build/x86/gart_scratch build/ppc/gart_scratch
	@set -e; $(call run_test,gart_scratch,)

test-mem-segment: build/x86/mem_segment build/ppc/mem_segment build/x86/mem_range_compat build/ppc/mem_range_compat
	@set -e; $(call run_test,mem_segment,); \
		$(call run_test,mem_range_compat,)

test-fence-packet: build/x86/fence_packet build/ppc/fence_packet
	@set -e; $(call run_test,fence_packet,)

test-pm-virtual: build/x86/pm_virtual build/ppc/pm_virtual
	@set -e; $(call run_test,pm_virtual,)

test: all test-fence-packet test-smc test-cayman-dpm test-pm-virtual test-uvd test-mc-layout test-gart-scratch test-mem-segment
	@if [ ! -f $(VBIOS) ] || [ ! -f $(REF_PHASE) ]; then \
		echo "SKIP: need $(VBIOS) and $(REF_PHASE)"; \
	else \
		set -e; \
		$(call run_test,mem_alloc,); \
		$(call run_test,engine_detect,); \
		$(call run_test,uvd_model,$(VBIOS)); \
		$(call run_test,arb_alias,); \
		$(call run_test,d3d_ps,); \
		if [ -f private/monitor-edid.bin ] && [ -f private/g5-monitor-edid.bin ]; then \
			$(call run_test,edid_modes,private/monitor-edid.bin private/g5-monitor-edid.bin); \
		else echo "SKIP edid_modes: need the two EDID files in private/"; fi; \
		$(call run_test,blit_ops,); \
		$(call run_test,atom_replay,$(VBIOS) $(REF_PHASE)); \
		$(call run_test,i2c_edid,$(VBIOS)); \
		$(call run_test,board_outputs,$(VBIOS)); \
		$(call run_test,dp_link,$(VBIOS)); \
		$(call run_test,pm_states,$(VBIOS)); \
		if [ -f $(REF_MODESET) ] && [ -f $(EDID) ]; then \
			$(call run_test,modeset_replay,$(VBIOS) $(EDID) $(REF_MODESET) - 0 $(REF_INIT)); \
			$(call run_test,modeset_replay,$(VBIOS) $(EDID) $(REF_MODESET2) 0 1); \
		else echo "SKIP modeset_replay: need $(EDID) and $(REF_MODESET)"; fi; \
		if [ -f $(FW_PFP) ] && [ -f $(FW_ME) ] && [ -f $(REF_INIT) ]; then \
			$(call run_test,accel_replay,$(VBIOS) $(FW_PFP) $(FW_ME) $(REF_INIT)); \
		else echo "SKIP accel_replay: need $(FW_PFP), $(FW_ME) and $(REF_INIT)"; fi; \
		if [ -f $(FW_MC) ] && [ -f $(REF_INIT) ]; then \
			$(call run_test,mc_replay,$(FW_MC) $(REF_INIT)); \
		else echo "SKIP mc_replay: need $(FW_MC) and $(REF_INIT)"; fi; \
		if [ -f $(REF_INIT) ]; then \
			$(call run_test,gart_replay,$(REF_INIT)); \
		else echo "SKIP gart_replay: need $(REF_INIT)"; fi; \
		$(call run_test,handover,); \
	fi

# Mesa-based tests need scripts/build-mesa.sh to have run.
MESA_BUILD ?= third_party/mesa-26.2.4/build-x86/src/gallium/targets/rdn
build/x86/rdn_gltest: mesa/tests/rdn_gltest.c $(MESA_BUILD)/librdngl.so
	$(CC) -O2 -Wall -Imesa/frontend/include -Ithird_party/mesa-26.2.4/include \
		-o $@ $< -L$(MESA_BUILD) -lrdngl -Wl,-rpath,$(CURDIR)/$(MESA_BUILD)

clean:
	rm -rf build/x86 build/ppc

.PHONY: all test test-uvd test-mc-layout test-gart-scratch test-mem-segment clean

test-smc: build/x86/smc_model build/ppc/smc_model
	@set -e; $(call run_test,smc_model,)

build/x86/pmcheck: build/x86/tools/pmcheck.o $(addprefix build/x86/,$(HW_OBJS))
	$(CC) -Wl,--wrap=atom_execute_table -o $@ $^

build/ppc/pmcheck: build/ppc/tools/pmcheck.o $(addprefix build/ppc/,$(HW_OBJS))
	$(PPC_CC) -static -Wl,--wrap=atom_execute_table -o $@ $^

build/x86/cayman_dpm_model: build/x86/tests/cayman_dpm_model.o $(addprefix build/x86/,$(HW_OBJS))
	$(CC) -Wl,--wrap=atom_execute_table -o $@ $^
build/ppc/cayman_dpm_model: build/ppc/tests/cayman_dpm_model.o $(addprefix build/ppc/,$(HW_OBJS))
	$(PPC_CC) -static -Wl,--wrap=atom_execute_table -o $@ $^
test-cayman-dpm: build/x86/cayman_dpm_model build/ppc/cayman_dpm_model
	@set -e; $(call run_test,cayman_dpm_model,)

# Packet model supplies its own register and CP callbacks.
build/x86/dma_model: tests/dma_model.c hw/rdn_dma.c $(HW_HDRS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS_COMMON) -Ihw tests/dma_model.c hw/rdn_dma.c -o $@

build/ppc/dma_model: tests/dma_model.c hw/rdn_dma.c $(HW_HDRS)
	@mkdir -p $(@D)
	$(PPC_CC) $(CFLAGS_COMMON) -static -Ihw tests/dma_model.c hw/rdn_dma.c -o $@
