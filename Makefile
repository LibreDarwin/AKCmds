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

OPEN_BIN := $(BUILD_DIR)/open
OPEN_OBJS := $(OBJDIR)/open.o

TIFF2ICNS_BIN := $(BUILD_DIR)/tiff2icns
TIFF2ICNS_OBJS := $(OBJDIR)/tiff2icns.o

TOPS_BIN := $(BUILD_DIR)/tops
TOPS_OBJS := $(OBJDIR)/tops.o

all: $(PB_COPY) $(PB_PASTE) $(OPEN_BIN) $(TIFF2ICNS_BIN) $(TOPS_BIN)

$(PB_COPY): $(PB_OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(MFLAGS) -o $@ $(PB_OBJS) -framework Cocoa -framework Foundation -framework AppKit -framework CoreFoundation

$(PB_PASTE): $(PB_OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(MFLAGS) -o $@ $(PB_OBJS) -framework Cocoa -framework Foundation -framework AppKit -framework CoreFoundation

$(OBJDIR)/pbcopy.o: src/pbcopy/pbcopy.m
	@mkdir -p $(OBJDIR)
	$(CC) $(MFLAGS) -c -o $@ src/pbcopy/pbcopy.m

$(OPEN_BIN): $(OPEN_OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(MFLAGS) -o $@ $(OPEN_OBJS) -framework Cocoa -framework Foundation \
	    -framework AppKit -framework CoreFoundation -framework ApplicationServices \
	    -framework CoreServices -lobjc

$(OBJDIR)/open.o: src/open/open.m
	@mkdir -p $(OBJDIR)
	$(CC) $(MFLAGS) -c -o $@ src/open/open.m

$(TIFF2ICNS_BIN): $(TIFF2ICNS_OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(MFLAGS) -o $@ $(TIFF2ICNS_OBJS) -framework Cocoa -framework Foundation \
	    -framework AppKit -framework CoreFoundation -framework CoreServices \
	    -framework ImageIO -lobjc

$(OBJDIR)/tiff2icns.o: src/tiff2icns/tiff2icns.m
	@mkdir -p $(OBJDIR)
	$(CC) $(MFLAGS) -c -o $@ src/tiff2icns/tiff2icns.m

$(TOPS_BIN): $(TOPS_OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(MFLAGS) -o $@ $(TOPS_OBJS) -framework Foundation -lobjc

$(OBJDIR)/tops.o: src/tops/tops.m
	@mkdir -p $(OBJDIR)
	$(CC) $(MFLAGS) -c -o $@ src/tops/tops.m

# Byte-parity check against Apple's /usr/bin/tops, run by tests/parity.sh.
# MY is passed as a relative path and resolved against the project root by the
# script, because this Makefile must stay free of $(shell)/$(CURDIR) to work
# under both GNU make and bmake.
#
#   make parity   # verify the binary for the current CONFIG
#   make test     # alias for parity
#
# The script is not listed as a prerequisite: make would otherwise treat it as
# a source file and try to build it. It is tracked in tests/, not local/,
# because .gitignore drops all of local/ and the build must not depend on
# anything that a fresh clone cannot see.
# It is run with bash explicitly: the script uses process substitution, which
# macOS /bin/sh (bash in POSIX mode) rejects.
parity: all
	MY="build/$(CONFIG)/tops" bash tests/parity.sh --all

# Byte-parity check against Apple's /usr/bin/tiff2icns, run by
# tests/tiff2icns-parity.sh.  Same MY convention and same reasoning about
# prerequisites and bash as parity above; that script needs python3 to build
# its TIFF fixtures.
tiff2icns-parity: all
	MY="build/$(CONFIG)/tiff2icns" bash tests/tiff2icns-parity.sh

# Same idea for open.  The harness only runs invocations that both tools
# reject before anything can be launched, and it fails the case if one of
# them ever starts succeeding.
open-parity: all
	MY="build/$(CONFIG)/open" bash tests/open-parity.sh

# pbcopy/pbpaste.  -pboard has no private-pasteboard mode, so the harness is
# restricted to ruler/find/font and refuses any invocation that would reach
# general; see the comment at the top of the script before changing that.
pbcopy-parity: all
	MY="build/$(CONFIG)/pbcopy" bash tests/pbcopy-parity.sh

test: parity tiff2icns-parity open-parity pbcopy-parity

clean:
	rm -rf build

.PHONY: all parity tiff2icns-parity open-parity pbcopy-parity test clean