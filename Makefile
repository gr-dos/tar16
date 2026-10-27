# Build tar.exe for 16-bit MS-DOS with Open Watcom and libc16.
#   make                         fetch the pinned libc16 release into
#                                build/libc16, then build tar.exe
#   make LIBC16_ROOT=../libc16   use a libc16 checkout built with its make
#
# tar.exe is a large-model program (-ml), compiled and linked against
# libc16's large library only (its ABI is in libc16's docs/ABI.md), with
# the Open Watcom snapshot libc16 was built with: the libc16 tree's
# tools/fetch-watcom fetches it into build/tools.

LIBC16_ROOT ?= build/libc16
# A release tree, or a checkout, whose make stages the same layout in
# build/stage.
LIBC16_DIR  := $(if $(wildcard $(LIBC16_ROOT)/build/stage/VERSION),$(LIBC16_ROOT)/build/stage,$(LIBC16_ROOT))
LIBC16      := $(LIBC16_DIR)/lib/LIBC16L.LIB
TOOLS_ENV   := build/tools.env
WATCOM_ENV   = . ./$(TOOLS_ENV) && PATH=$$WATCOM/binl64:$$PATH

VERSION := $(shell cat VERSION)

# -s: libc16 has no stack-check routine.
CC     = wcc
CFLAGS = -q -zq -zl -zld -x -ml -0 -os -s -bt=dos -w4 \
         -I$(LIBC16_DIR)/include '-DVERSION="$(VERSION)"'

tar.exe: build/tar.obj $(LIBC16) $(TOOLS_ENV)
	$(WATCOM_ENV) && wlink option quiet, nodefaultlibs, stack=8192, map=build/tar.map \
	  format dos name $@ file build/tar.obj library $(LIBC16)

build/tar.obj: tar.c VERSION $(LIBC16) $(TOOLS_ENV)
	mkdir -p build
	$(WATCOM_ENV) && $(CC) $(CFLAGS) -fo=$@ tar.c

# The pinned libc16 release (scripts/fetch-libc16). A LIBC16_ROOT given
# on the command line must already be built.
ifeq ($(LIBC16_ROOT),build/libc16)
$(LIBC16): scripts/fetch-libc16
	scripts/fetch-libc16 $(LIBC16_ROOT)
endif

# The Open Watcom snapshot libc16 names, as shell assignments.
$(TOOLS_ENV): $(LIBC16)
	mkdir -p build
	watcom=$$(sh $(LIBC16_DIR)/tools/fetch-watcom $(CURDIR)/build/tools) \
	  && printf 'WATCOM=%s\nexport WATCOM\n' "$$watcom" > $@

test: tar.exe
	sh tests/run.sh $(CURDIR)/tar.exe

clean:
	rm -f tar.exe *.o *.obj *.err build/tar.obj build/tar.map

.PHONY: test clean
