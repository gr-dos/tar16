# Build tar.exe for 16-bit MS-DOS with Open Watcom
WATCOM ?= $(HOME)/opt/watcom
export WATCOM
export INCLUDE := $(WATCOM)/h
export PATH := $(WATCOM)/binl64:$(WATCOM)/binl:$(PATH)

CFLAGS = -bt=dos -ml -0 -os -w4 -zq

tar.exe: tar.c
	wcl $(CFLAGS) -k8192 -fe=$@ $<

clean:
	rm -f tar.exe *.o *.obj *.err

.PHONY: clean
