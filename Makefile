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

TEXTUTIL_BIN := $(BUILD_DIR)/textutil
TEXTUTIL_OBJS := $(OBJDIR)/textutil_main.o $(OBJDIR)/textutil_txt.o \
	$(OBJDIR)/textutil_rtf.o $(OBJDIR)/textutil_rtfd.o \
	$(OBJDIR)/textutil_html.o $(OBJDIR)/textutil_wordml.o \
	$(OBJDIR)/textutil_info.o \
	$(OBJDIR)/textutil_format.o $(OBJDIR)/textutil_io.o \
	$(OBJDIR)/textutil_font.o $(OBJDIR)/textutil_usage.o \
	$(OBJDIR)/textutil_bidi.o

TOPS_BIN := $(BUILD_DIR)/tops
TOPS_OBJS := $(OBJDIR)/tops.o

# tiffutil is plain C (no Cocoa): it links the same CFLAGS the rest of the
# build uses for C, with no frameworks.
TIFFUTIL_BIN := $(BUILD_DIR)/tiffutil
TIFFUTIL_OBJS := $(OBJDIR)/buf.o $(OBJDIR)/cat.o $(OBJDIR)/compress.o \
	$(OBJDIR)/g4.o $(OBJDIR)/tiff_decode.o $(OBJDIR)/tiff_read.o \
	$(OBJDIR)/tiff_text.o $(OBJDIR)/tiff_write.o $(OBJDIR)/main.o

all: $(PB_COPY) $(PB_PASTE) $(OPEN_BIN) $(TIFF2ICNS_BIN) $(TOPS_BIN) \
	$(TIFFUTIL_BIN) $(TEXTUTIL_BIN)

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

# textutil is plain C like tiffutil: no Cocoa, no frameworks, nothing to link
# but the objects.  The objects are named apart from tiffutil's because both
# tools have a main.c and share the object directory.
$(TEXTUTIL_BIN): $(TEXTUTIL_OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $(TEXTUTIL_OBJS)

$(OBJDIR)/textutil_main.o: src/textutil/main.c
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/textutil/main.c

$(OBJDIR)/textutil_txt.o: src/textutil/txt.c
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/textutil/txt.c

$(OBJDIR)/textutil_rtf.o: src/textutil/rtf.c
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/textutil/rtf.c

$(OBJDIR)/textutil_rtfd.o: src/textutil/rtfd.c
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/textutil/rtfd.c

$(OBJDIR)/textutil_html.o: src/textutil/html.c
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/textutil/html.c

$(OBJDIR)/textutil_wordml.o: src/textutil/wordml.c
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/textutil/wordml.c

$(OBJDIR)/textutil_info.o: src/textutil/info.c
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/textutil/info.c

$(OBJDIR)/textutil_format.o: src/textutil/format.c
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/textutil/format.c

$(OBJDIR)/textutil_font.o: src/textutil/font.c
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/textutil/font.c

$(OBJDIR)/textutil_io.o: src/textutil/io.c
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/textutil/io.c

$(OBJDIR)/textutil_usage.o: src/textutil/usage.c
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/textutil/usage.c

$(OBJDIR)/textutil_bidi.o: src/textutil/bidi.c
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/textutil/bidi.c

$(TOPS_BIN): $(TOPS_OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(MFLAGS) -o $@ $(TOPS_OBJS) -framework Foundation -lobjc

$(OBJDIR)/tops.o: src/tops/tops.m
	@mkdir -p $(OBJDIR)
	$(CC) $(MFLAGS) -c -o $@ src/tops/tops.m

$(TIFFUTIL_BIN): $(TIFFUTIL_OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $(TIFFUTIL_OBJS)

$(OBJDIR)/buf.o: src/tiffutil/buf.c src/tiffutil/buf.h src/tiffutil/tiffutil.h
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/tiffutil/buf.c

$(OBJDIR)/cat.o: src/tiffutil/cat.c src/tiffutil/tiffutil.h
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/tiffutil/cat.c

$(OBJDIR)/compress.o: src/tiffutil/compress.c src/tiffutil/tiffutil.h
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/tiffutil/compress.c

$(OBJDIR)/g4.o: src/tiffutil/g4.c src/tiffutil/tiffutil.h
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/tiffutil/g4.c

$(OBJDIR)/tiff_decode.o: src/tiffutil/tiff_decode.c src/tiffutil/tiffutil.h
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/tiffutil/tiff_decode.c

$(OBJDIR)/tiff_read.o: src/tiffutil/tiff_read.c src/tiffutil/tiffutil.h
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/tiffutil/tiff_read.c

$(OBJDIR)/tiff_text.o: src/tiffutil/tiff_text.c src/tiffutil/tiffutil.h
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/tiffutil/tiff_text.c

$(OBJDIR)/tiff_write.o: src/tiffutil/tiff_write.c src/tiffutil/tiffutil.h
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/tiffutil/tiff_write.c

$(OBJDIR)/main.o: src/tiffutil/main.c src/tiffutil/tiffutil.h src/tiffutil/buf.h
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/tiffutil/main.c

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

# tiffutil.  The four gaps that kept this out of `test` -- the -cat family,
# palette sources, multi-directory input, and the positional argument grammar
# -- are all closed, so it runs as part of `test` now.  942 cases, all
# byte-compared against /usr/bin/tiffutil.
#
# It does not cover the three items NOTES.md still lists as open (16-bit LogLuv
# and YCbCr Photometric 6, the Lab profile's build timestamp, and third-party
# Group 4 extension codes).  Those are untested rather than failing, so they are
# not a reason to hold this target back; the harness is green because nothing in
# it exercises them.
tiffutil-parity: all
	MY="build/$(CONFIG)/tiffutil" bash tests/tiffutil-parity.sh

# textutil.  The harness compares the option parser, -info and the txt, rtf,
# rtfd and html writers against /usr/bin/textutil; the doc family is
# recognised but not written, so it is out of scope; see src/textutil/NOTES.md.
# Same MY convention and same reasoning about prerequisites and bash as above.
textutil-parity: all
	MY="build/$(CONFIG)/textutil" bash tests/textutil-parity.sh

test: parity tiff2icns-parity open-parity pbcopy-parity tiffutil-parity \
	textutil-parity

clean:
	rm -rf build

.PHONY: all parity tiff2icns-parity open-parity pbcopy-parity tiffutil-parity \
	textutil-parity test clean