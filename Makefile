# Copyright (C) 2026, LibreDarwin
# SPDX-License-Identifier: BSD-3-Clause
# Clean-room reimplementation of Apple's CoreGraphics.
#
# Build layout: every artifact lives under build/; the framework goes to
# build/<CONFIG>/CoreGraphics.framework per CONFIG.
#
# Portable to both GNU make and BSD make (bmake): no pattern rules, no
# ifeq/ifdef/.if conditionals and no $(if)/$(shell) functions.  Per-config
# flags come from make/<CONFIG>.mk so both make variants behave identically.
# Note that bmake treats a `%' in a target as a literal suffix, so the
# compile rules below are written out one per source file.

CONFIG ?= release
SDK    ?= /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk
# Apple's Xcode toolchain, to match SDK above.  The from-source tree at
# xnuports-root/devel/xcode-tools/... was used here first and cannot be:
# it ships eight libclang_rt* files, all of them profiling or kext
# runtimes, and no asan or ubsan entry at all, so CONFIG=asan failed at
# the link with "file not found: libclang_rt.asan_osx_dynamic.dylib".
# Assigned rather than ?= because make predefines CC as "cc", so a ?=
# would leave the default in place; override on the command line
# (make CC=...) to build against the Internal SDK's compiler instead.
CC     := /Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang

-include make/$(CONFIG).mk

BUILD_DIR := build/$(CONFIG)
OBJDIR    := $(BUILD_DIR)/obj

# -mmacosx-version-min must agree with the -platform_version passed to the
# linker, otherwise ld sees two different minos values in the objects and
# the link commands and warns.
CFLAGS := $(OPT) -std=c11 -D_DARWIN_C_SOURCE -mmacosx-version-min=26.5 \
	-isysroot "$(SDK)" -Isrc -Wall -Wextra

# Apple's exact dylib identity, read out of the original binary's load
# commands (LC_ID_DYLIB: current version 1965.5.1, compatibility 64.0.0,
# LC_BUILD_VERSION: platform macOS, minos 26.5, sdk 26.5).
FW_ID       := $(BUILD_DIR)/CoreGraphics.framework
FW_DYLIB   := $(FW_ID)/Versions/A/CoreGraphics
FW_CFG      := src/Info.plist src/version.plist src/CodeResources

# Listed twice on purpose: FW_HDRS holds the bare names for the install and
# copy loops, FW_HDR_DEPS the same headers as make prerequisites.  Deriving
# one from the other would need addprefix/patsubst, which bmake lacks.
FW_HDRS     := CGBase.h CGGeometry.h CGAffineTransform.h CoreGraphics.h
FW_HDR_DEPS := src/CGBase.h src/CGGeometry.h src/CGAffineTransform.h \
               src/CoreGraphics.h src/CGSPI.h

PREFIX  ?= /usr/local
DESTDIR ?=

OBJS := $(OBJDIR)/CGGeometry.o $(OBJDIR)/CGAffineTransform.o $(OBJDIR)/CGError.o

all: $(FW_DYLIB)

# The target is the dylib, not $(FW_ID).  Making the framework *directory*
# the target is what turned a failed link into a silent success: the recipe
# below opens with `rm -rf' and `mkdir -p', so a link that died left the
# directory in place, and make then saw the target present and newer than
# its prerequisites and reported "Nothing to be done for 'all'" on every
# later run -- handing back a framework containing no dylib at all, which
# the parity harness rejected with "ours is not bound to our framework".
# A file target cannot get into that state: if the link fails the file is
# absent, so the next run simply retries.  $(FW_ID) is never mentioned as a
# target, so make does not treat it as an intermediate and try to delete it.
#
# The framework container mirrors Apple's *layout*; only the dylib is ours.
# As in BomCmds, the install name is @rpath rather than Apple's absolute
# /System/... path, so a build-tree binary resolves to OUR dylib instead of
# the copy in Apple's dyld shared cache (which does not export the API we
# implement).
#
# The container is not byte-identical, and cannot be: Apple's
# Versions/A/_CodeSignature/CodeResources omits every file from the seal
# (empty "files"/"files2" plus a "^.*" => omit rule), because the copy in the
# shared cache has had its resources stripped and there is nothing left to
# hash.  We ship real Info.plist and version.plist files, so our signature has
# to seal them -- reusing Apple's CodeResources verbatim would declare a seal
# that does not describe the bundle it is shipped in, and codesign rejects it
# with "code has no resources but signature indicates they must be present".
# So src/CodeResources is the same omit-everything boilerplate we start from
# and codesign then rewrites it to describe this bundle.
$(FW_DYLIB): $(OBJS) $(FW_CFG) $(FW_HDR_DEPS)
	@rm -rf $(FW_ID)
	@mkdir -p $(FW_ID)/Versions/A/_CodeSignature $(FW_ID)/Versions/A/Resources \
		$(FW_ID)/Versions/A/XPCServices
	$(CC) $(CFLAGS) -dynamiclib \
		-Wl,-install_name,@rpath/CoreGraphics.framework/Versions/A/CoreGraphics \
		-Wl,-compatibility_version,64.0.0 -Wl,-current_version,1965.5.1 \
		-o $(FW_DYLIB) $(OBJS) -framework CoreFoundation
	cp src/Info.plist $(FW_ID)/Versions/A/Resources/
	cp src/version.plist $(FW_ID)/Versions/A/Resources/
	ln -sfn A $(FW_ID)/Versions/Current
	ln -sfn Versions/Current/CoreGraphics $(FW_ID)/CoreGraphics
	ln -sfn Versions/Current/Resources $(FW_ID)/Resources
# Apple's runtime CoreGraphics.framework carries an XPCServices symlink onto a
# real (empty) directory, so both the directory and the link are part of the
# container; the symlink alone would dangle.
	ln -sfn Versions/Current/XPCServices $(FW_ID)/XPCServices
# Sign last, over the finished container.  The linker's ad-hoc signature only
# covers the Mach-O, so signing before the plists and symlinks are in place
# leaves a CodeResources that no CodeDirectory accounts for.
	codesign --force --sign - --timestamp=none $(FW_ID)

$(OBJDIR)/CGGeometry.o: src/CGGeometry.c src/CGGeometry.h src/CGBase.h
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/CGGeometry.c

$(OBJDIR)/CGAffineTransform.o: src/CGAffineTransform.c src/CGAffineTransform.h src/CGGeometry.h src/CGBase.h src/CGInternal.h src/CGSPI.h
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/CGAffineTransform.c

$(OBJDIR)/CGError.o: src/CGError.c src/CGInternal.h src/CGBase.h
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ src/CGError.c

install: all
	install -d $(DESTDIR)$(PREFIX)/include/CoreGraphics
	for h in $(FW_HDRS); do \
	    install -m 0444 src/$$h $(DESTDIR)$(PREFIX)/include/CoreGraphics/$$h; \
	done
	install -d $(DESTDIR)/System/Library/Frameworks
	cp -R $(FW_ID) $(DESTDIR)/System/Library/Frameworks/

clean:
	rm -rf build

.PHONY: all install clean
