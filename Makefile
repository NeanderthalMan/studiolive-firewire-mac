# SPDX-License-Identifier: GPL-2.0-or-later
CFLAGS  ?= -Wall -Wextra -Wno-unused-parameter -O2
FW       = -framework CoreFoundation -framework IOKit
BUNDLE   = build/StudioLiveFW.driver

all: build/slrecord driver build/halcheck build/cacheck build/mkaggregate build/tone menu

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

# The menu bar app. Swift wants top-level code in a file named main.swift, so
# the app's is, and the test's is copied to one. mkaggregate rides inside the
# bundle for the "Turn On Drift Compensation" item.
MENU = build/StudioLiveMenu.app

menu: $(MENU)/Contents/MacOS/StudioLiveMenu

$(MENU)/Contents/MacOS/StudioLiveMenu: menubar/main.swift menubar/Status.swift menubar/Info.plist build/mkaggregate
	@mkdir -p $(MENU)/Contents/MacOS $(MENU)/Contents/Resources
	cp menubar/Info.plist $(MENU)/Contents/Info.plist
	cp build/mkaggregate $(MENU)/Contents/Resources/mkaggregate
	swiftc -O -o $@ menubar/main.swift menubar/Status.swift
	codesign --force --sign - $(MENU)/Contents/Resources/mkaggregate
	codesign --force --sign - $(MENU)

# Unit tests: no console, no Core Audio server, no sudo. Each test #includes
# the .c it covers so it can reach the static functions; the exit status is the
# failure count, so `make test` is the gate.
TESTS = build/tests/test_slfw build/tests/test_plugin build/tests/test_menu

test: $(TESTS)
	@for t in $(TESTS); do $$t || exit 1; done

build/tests/test_slfw: tests/test_slfw.c tests/check.h src/slfw.c src/slfw.h
	@mkdir -p build/tests
	$(CC) $(CFLAGS) -o $@ tests/test_slfw.c $(FW)

build/tests/test_plugin: tests/test_plugin.c tests/check.h driver/plugin.c src/slfw.c src/slfw.h
	@mkdir -p build/tests
	$(CC) $(CFLAGS) -o $@ tests/test_plugin.c $(FW) -framework CoreAudio

build/tests/test_menu: tests/test_menu.swift menubar/Status.swift
	@mkdir -p build/tests/menu
	cp tests/test_menu.swift build/tests/menu/main.swift
	swiftc -o $@ build/tests/menu/main.swift menubar/Status.swift

clean:
	rm -rf build

.PHONY: all driver menu test clean
