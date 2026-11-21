# Audiophile USB community driver - builds with Xcode or the Xcode Command Line Tools.
#   make            build HAL driver, CoreMIDI driver and apusb-diag (ad-hoc signed)
#   make test       run portable unit tests
#   make pkg        build the installer package (see build.sh)
#   make clean
# SPDX-License-Identifier: MIT

VERSION        ?= 0.1.0
ARCHS          ?= x86_64
MIN_MACOS      ?= 12.0
CC             ?= clang
SIGN_ID        ?= -
BUILD          := build

ARCHFLAGS      := $(foreach a,$(ARCHS),-arch $(a))
CFLAGS         += -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations \
                  -fvisibility=hidden -mmacosx-version-min=$(MIN_MACOS) $(ARCHFLAGS) -Isrc/common
LDFLAGS        += -mmacosx-version-min=$(MIN_MACOS) $(ARCHFLAGS)

PORTABLE_SRC   := src/common/apusb_modes.c src/common/apusb_convert.c src/common/apusb_ring.c src/common/apusb_midiman.c
USB_SRC        := src/common/apusb_usb.c src/common/apusb_stream.c

HAL_BUNDLE     := $(BUILD)/AudiophileUSB.driver
MIDI_BUNDLE    := $(BUILD)/AudiophileUSBMIDI.plugin
DIAG           := $(BUILD)/apusb-diag
DAEMON         := $(BUILD)/audiophileusbd

.PHONY: all test smoke pkg clean
all: $(HAL_BUNDLE) $(MIDI_BUNDLE) $(DIAG) $(DAEMON)

$(HAL_BUNDLE): src/hal/AudiophileUSBDriver.c src/hal/Info.plist $(PORTABLE_SRC) $(USB_SRC) src/common/*.h
	@mkdir -p $@/Contents/MacOS $@/Contents/Resources
	sed 's/@VERSION@/$(VERSION)/g' src/hal/Info.plist > $@/Contents/Info.plist
	$(CC) $(CFLAGS) -bundle -o $@/Contents/MacOS/AudiophileUSB src/hal/AudiophileUSBDriver.c $(PORTABLE_SRC) $(USB_SRC) \
	    $(LDFLAGS) -framework CoreFoundation -framework CoreAudio -framework IOKit
	codesign --force --sign "$(SIGN_ID)" --timestamp=none $@

$(MIDI_BUNDLE): src/midi/AudiophileUSBMIDI.c src/midi/Info.plist $(PORTABLE_SRC) $(USB_SRC) src/common/*.h
	@mkdir -p $@/Contents/MacOS $@/Contents/Resources
	sed 's/@VERSION@/$(VERSION)/g' src/midi/Info.plist > $@/Contents/Info.plist
	$(CC) $(CFLAGS) -bundle -o $@/Contents/MacOS/AudiophileUSBMIDI src/midi/AudiophileUSBMIDI.c $(PORTABLE_SRC) $(USB_SRC) \
	    $(LDFLAGS) -framework CoreFoundation -framework CoreMIDI -framework IOKit
	codesign --force --sign "$(SIGN_ID)" --timestamp=none $@

$(DIAG): src/diag/apusb-diag.c $(PORTABLE_SRC) $(USB_SRC) src/common/*.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ src/diag/apusb-diag.c $(PORTABLE_SRC) $(USB_SRC) \
	    $(LDFLAGS) -framework CoreFoundation -framework CoreAudio -framework CoreMIDI -framework IOKit
	codesign --force --sign "$(SIGN_ID)" --timestamp=none $@

$(DAEMON): src/daemon/audiophileusbd.c src/common/apusb_usb.c src/common/*.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ src/daemon/audiophileusbd.c src/common/apusb_usb.c $(LDFLAGS) -framework CoreFoundation -framework IOKit
	codesign --force --sign "$(SIGN_ID)" --timestamp=none $@

test:
	@mkdir -p $(BUILD)
	$(CC) -std=gnu11 -Wall -Wextra -Werror -Isrc/common -o $(BUILD)/test_portable tests/test_portable.c $(PORTABLE_SRC) -lm
	$(BUILD)/test_portable

smoke: all
	$(CC) $(CFLAGS) -o $(BUILD)/plugin_smoke tests/plugin_smoke.c $(LDFLAGS) -framework CoreFoundation -framework CoreAudio -framework CoreMIDI
	@nm -gU $(HAL_BUNDLE)/Contents/MacOS/AudiophileUSB | grep -q AudiophileUSB_Create || (echo "factory symbol not exported" && false)
	@nm -gU $(MIDI_BUNDLE)/Contents/MacOS/AudiophileUSBMIDI | grep -q AudiophileUSBMIDI_Create || (echo "factory symbol not exported" && false)
	$(BUILD)/plugin_smoke $(HAL_BUNDLE) $(MIDI_BUNDLE)

pkg: all
	VERSION=$(VERSION) ./build.sh --pkg-only

clean:
	rm -rf $(BUILD) dist
