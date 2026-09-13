# SPDX-License-Identifier: GPL-2.0-or-later
CFLAGS  ?= -Wall -Wextra -Wno-unused-parameter -O2
FW       = -framework CoreFoundation -framework IOKit
BUNDLE   = build/StudioLiveFW.driver

all: build/slrecord driver build/halcheck build/cacheck build/mkaggregate build/tone

build/slrecord: tools/slrecord.c src/slfw.c src/slfw.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tools/slrecord.c src/slfw.c $(FW)

driver: $(BUNDLE)/Contents/MacOS/StudioLiveFW

# Ad-hoc signed: there is no Developer ID on this Mac, and a locally installed
# HAL plug-in does not need one.
$(BUNDLE)/Contents/MacOS/StudioLiveFW: driver/plugin.c driver/Info.plist src/slfw.c src/slfw.h
	@mkdir -p $(BUNDLE)/Contents/MacOS
	cp driver/Info.plist $(BUNDLE)/Contents/Info.plist
	$(CC) $(CFLAGS) -bundle -o $@ driver/plugin.c src/slfw.c $(FW) -framework CoreAudio
	codesign --force --sign - $(BUNDLE)

build/halcheck: tools/halcheck.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tools/halcheck.c -framework CoreFoundation -framework CoreAudio

build/cacheck: tools/cacheck.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tools/cacheck.c -framework CoreFoundation -framework CoreAudio

build/mkaggregate: tools/mkaggregate.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tools/mkaggregate.c -framework CoreFoundation -framework CoreAudio

build/tone: tools/tone.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tools/tone.c -framework CoreFoundation -framework CoreAudio

clean:
	rm -rf build

.PHONY: all driver clean
