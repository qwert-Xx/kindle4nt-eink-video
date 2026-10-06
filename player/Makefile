# SPDX-License-Identifier: GPL-2.0-only
ifeq ($(origin CC),default)
CC = arm-linux-gnueabihf-gcc
endif
KERNEL_SRC ?= ../build/src
CFLAGS ?= -O3 -Wall -mcpu=cortex-a8 -mfpu=neon -mfloat-abi=hard -mno-unaligned-access
CPPFLAGS += -idirafter $(KERNEL_SRC)/include/uapi
LDFLAGS += -static -pthread

PROGRAMS = k4-wb k4-a2
TESTS = ../tests/test_k4wb ../tests/test_neon
.PHONY: all test clean
all: $(PROGRAMS)
%: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LDFLAGS) -o $@
../tests/test_k4wb: ../tests/test_k4wb.c k4-wb.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LDFLAGS) -o $@
../tests/test_neon: ../tests/test_neon.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LDFLAGS) -o $@
test: $(TESTS)
	qemu-arm-static ../tests/test_k4wb
	qemu-arm-static ../tests/test_neon
clean:
	rm -f $(PROGRAMS) $(TESTS)
