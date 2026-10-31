# Builds RadeonNIRomProbe.kext on Tiger with Apple's gcc 4.0.1, like
# ../RadeonNI. See RomProbe.cpp for what it is for.

KEXT     = RadeonNIRomProbe
KROOT    = /System/Library/Frameworks/Kernel.framework
BUILD    = build
BUNDLE   = $(BUILD)/$(KEXT).kext

CPPFLAGS = -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
	-nostdinc -I../RadeonNI/compat -I../../hw -I$(KROOT)/PrivateHeaders -I$(KROOT)/Headers
CFLAGS   = -arch ppc -Os -Wall -fno-builtin -fno-common -static \
	-mlong-branch -force_cpusubtype_ALL
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti -fapple-kext -fcheck-new

HW = rdn_atom rdn_post rdn_mc rdn_gpu rdn_cayman rdn_cp rdn_blit rdn_gart rdn_modeset rdn_dp rdn_i2c rdn_mode rdn_dmt rdn_pm rdn_watermark
OBJS = $(BUILD)/RomProbe.o $(BUILD)/CaymanBringup.o $(BUILD)/kmod_info.o
OBJS += $(addprefix $(BUILD)/hw/,$(addsuffix .o,$(HW))) $(BUILD)/hw/atom.o

all: $(BUNDLE)

$(BUILD)/hw/%.o: ../../hw/%.c $(wildcard ../../hw/*.h)
	@mkdir -p $(dir $@)
	gcc $(CPPFLAGS) $(CFLAGS) -std=gnu99 -Wno-unused -c -o $@ $<

$(BUILD)/hw/atom.o: ../../hw/atom/atom.c
	@mkdir -p $(dir $@)
	gcc $(CPPFLAGS) $(CFLAGS) -std=gnu99 -Wno-unused -c -o $@ $<

$(BUILD)/%.o: %.cpp $(wildcard ../../hw/*.h ../../hw/atom/*.h)
	@mkdir -p $(dir $@)
	g++ $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	gcc $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BUNDLE): $(OBJS) Info.plist
	@mkdir -p $(BUNDLE)/Contents/MacOS
	g++ -arch ppc -static -nostdlib -r $(OBJS) -lkmodc++ -lkmod -lcc_kext \
		-o $(BUNDLE)/Contents/MacOS/$(KEXT)
	cp Info.plist $(BUNDLE)/Contents/Info.plist

clean:
	rm -rf $(BUILD)

.PHONY: all clean
