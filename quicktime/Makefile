# RadeonNIAVC.component, built on the Mac itself (Xcode's gcc, Rez and the 10.4u SDK, so that it runs on
# Tiger and Leopard):  make   (in a directory with these files, rdnvd.h and librdnvd.dylib)
# The bundle:  RadeonNIAVC.component/Contents/{Info.plist, MacOS/RadeonNIAVC, MacOS/librdnvd.dylib,
# Resources/RadeonNIAVC.rsrc}. The library sits beside the executable (its install name is
# @loader_path/librdnvd.dylib).
#
# Copyright (c) 2026 kouta-kun and Claude
# SPDX-License-Identifier: MIT

SDK = /Developer/SDKs/MacOSX10.4u.sdk
CC = gcc-4.0
ARCH = -arch ppc -isysroot $(SDK) -mmacosx-version-min=10.4
BUNDLE = RadeonNIAVC.component
CONTENTS = $(BUNDLE)/Contents

all: $(CONTENTS)/MacOS/RadeonNIAVC $(CONTENTS)/MacOS/librdnvd.dylib $(CONTENTS)/Resources/RadeonNIAVC.rsrc $(CONTENTS)/Info.plist

$(CONTENTS)/MacOS/RadeonNIAVC: RadeonNIAVC.c RadeonNIAVCDispatch.h rdnvd.h
	mkdir -p $(CONTENTS)/MacOS
	$(CC) $(ARCH) -O2 -Wall -fno-strict-aliasing -fpascal-strings -I. -bundle -o $@ RadeonNIAVC.c \
		-framework CoreServices -framework QuickTime -L. -lrdnvd

$(CONTENTS)/MacOS/librdnvd.dylib: librdnvd.dylib
	mkdir -p $(CONTENTS)/MacOS
	cp librdnvd.dylib $@

$(CONTENTS)/Resources/RadeonNIAVC.rsrc: RadeonNIAVC.r
	mkdir -p $(CONTENTS)/Resources
	Rez -useDF -arch ppc -isysroot $(SDK) -d TARGET_API_MAC_OSX=1 -o $@ RadeonNIAVC.r

$(CONTENTS)/Info.plist: Info.plist
	mkdir -p $(CONTENTS)
	cp Info.plist $@

clean:
	rm -rf $(BUNDLE)
