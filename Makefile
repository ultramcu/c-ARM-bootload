# SPDX-License-Identifier: MIT
# Copyright (c) 2026 MaIII Themd
#
# Host-side build of the armbl self-test. The library itself is
# pure C99 with no STM32 dependency, so it compiles fine on any
# host toolchain.

CC      ?= gcc
CFLAGS  ?= -std=c99 -Wall -Wextra -Wpedantic -Wstrict-prototypes -O2

TARGET = check
SRC    = armbl.c example/host_check.c

$(TARGET): $(SRC) armbl.h
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET)

test: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET) *.o example/*.o

.PHONY: test clean
