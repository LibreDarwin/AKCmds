# Copyright (C) 2026, LibreDarwin
# SPDX-License-Identifier: BSD-3-Clause
# Clean-room reimplementation of Apple's AppKit command-line tools:
# pbcopy/pbpaste, tiffutil, tiff2icns, tops, open, textutil.
#
# Build layout: every artifact lives under build/; final tools go to
# build/release/ or build/debug/ per CONFIG.
#
# Portable to both GNU make and BSD make (bmake): no pattern rules, no
# ifeq/ifdef/.if conditionals and no $(if)/$(shell) functions.  Per-config
# flags come from make/<CONFIG>.mk so both make variants behave identically.
#
# Apple ships pbcopy and pbpaste as two distinct binaries built from the
# same source, dispatching on argv[0]; we do the same (two linked copies of
# one object).

CONFIG ?= release
SDK    ?= /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk
CC     := /Users/sunneva/xnuports-root/devel/xcode-tools/build/release/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang

-include make/$(CONFIG).mk

BUILD_DIR := build/$(CONFIG)
OBJDIR    := $(BUILD_DIR)/obj

CFLAGS := $(OPT) -std=c11 -D_DARWIN_C_SOURCE -isysroot "$(SDK)" -Wall -Wextra
MFLAGS := $(OPT) -fobjc-exceptions -isysroot "$(SDK)" -Wall -Wextra \
	  -Wno-deprecated-declarations

PB_COPY := $(BUILD_DIR)/pbcopy
PB_PASTE := $(BUILD_DIR)/pbpaste
PB_OBJS := $(OBJDIR)/pbcopy.o

all: $(PB_COPY) $(PB_PASTE)

$(PB_COPY): $(PB_OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(MFLAGS) -o $@ $(PB_OBJS) -framework Cocoa -framework Foundation -framework AppKit -framework CoreFoundation

$(PB_PASTE): $(PB_OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(MFLAGS) -o $@ $(PB_OBJS) -framework Cocoa -framework Foundation -framework AppKit -framework CoreFoundation

$(OBJDIR)/pbcopy.o: src/pbcopy/pbcopy.m
	@mkdir -p $(OBJDIR)
	$(CC) $(MFLAGS) -c -o $@ src/pbcopy/pbcopy.m

test: all

clean:
	rm -rf build

.PHONY: all test clean