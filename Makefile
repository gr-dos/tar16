# Build tar.exe for 16-bit MS-DOS with Open Watcom
WATCOM ?= $(HOME)/opt/watcom
export WATCOM
export INCLUDE := $(WATCOM)/h
export PATH := $(WATCOM)/binl64:$(WATCOM)/binl:$(PATH)

VERSION := $(shell cat VERSION)
CFLAGS = -bt=dos -ml -0 -os -w4 -zq '-DVERSION="$(VERSION)"'

tar.exe: tar.c VERSION
	wcl $(CFLAGS) -k8192 -fe=$@ tar.c

test: tar.exe
	sh tests/run.sh $(CURDIR)/tar.exe

clean:
	rm -f tar.exe *.o *.obj *.err

.PHONY: test clean
