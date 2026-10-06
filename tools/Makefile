# SPDX-License-Identifier: GPL-2.0-only
ifeq ($(origin CC),default)
CC = arm-linux-gnueabihf-gcc
endif
KERNEL_SRC ?= ../build/src
CFLAGS ?= -O3 -Wall -mcpu=cortex-a8 -mfpu=neon -mfloat-abi=hard -mno-unaligned-access
CPPFLAGS += -idirafter $(KERNEL_SRC)/include/uapi
LDFLAGS += -static -pthread

PROGRAMS = wbprobe wbdrive wvinfo wbbench epdc-timing wbcurve
.PHONY: all clean
all: $(PROGRAMS)
%: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LDFLAGS) -o $@
clean:
	rm -f $(PROGRAMS)
