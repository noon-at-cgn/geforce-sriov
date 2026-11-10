# SPDX-License-Identifier: MIT
# geforce-sriov top-level build. Components:
#   geforce-sriov                  host CLI            (src/)
#   hook/libgeforce_sriov.so       nvidia-vgpud hook   (hook/Makefile)
#   driver/geforce-sriov-patch     driver patcher      (driver/)
#   kernel/geforce_sriov.ko        livepatch module    (kernel/Makefile, `make module`)

VERSION := 0.2.2

CC ?= cc
CFLAGS ?= -O2 -g
WARNFLAGS := -std=c11 -Wall -Wextra -Werror
# Stricter warnings for the CLI sources.
CLI_WARNFLAGS := -Wshadow -Wformat=2 -Wstrict-prototypes -Wmissing-prototypes
ALL_CPPFLAGS := -D_GNU_SOURCE -Iinclude $(CPPFLAGS)
ALL_CFLAGS := $(WARNFLAGS) $(CFLAGS)
CLI_CFLAGS := $(ALL_CFLAGS) $(CLI_WARNFLAGS)

PREFIX ?= /usr/local
DESTDIR ?=
DEVICES ?=
BOOT_DISPLAY_RELEASE ?=
VFS ?=
KDIR ?= /lib/modules/$(shell uname -r)/build

CLI_SRCS := src/config.c src/install.c src/lifecycle.c src/pci.c src/setup.c src/sha256.c src/util.c
CLI_HDRS := include/gs_lifecycle.h include/gs_devices.h include/gs_hook.h
# Patcher: driver/main.c holds main(); every other driver/*.c is library code that the
# patcher tests (tests/test_patch*.c) link against.
PATCH_MAIN := driver/main.c
PATCH_LIB := $(filter-out $(PATCH_MAIN),$(wildcard driver/*.c))
PATCH_HDRS := $(wildcard driver/*.h)
PATCH_TESTS := $(patsubst %.c,%,$(wildcard tests/test_patch*.c))

TESTS := tests/test_lifecycle tests/test_devices $(PATCH_TESTS)

.PHONY: all hook module check test install uninstall clean

all: geforce-sriov hook driver/geforce-sriov-patch

geforce-sriov: src/main.c $(CLI_SRCS) $(CLI_HDRS)
	$(CC) $(ALL_CPPFLAGS) $(CLI_CFLAGS) -o $@ src/main.c $(CLI_SRCS) $(LDFLAGS)

hook:
	$(MAKE) -C hook all

hook/libgeforce_sriov.so: hook

driver/geforce-sriov-patch: $(PATCH_MAIN) $(PATCH_LIB) $(PATCH_HDRS)
	$(CC) $(ALL_CPPFLAGS) -Idriver $(ALL_CFLAGS) -o $@ $(PATCH_MAIN) $(PATCH_LIB) $(LDFLAGS)

module:
	$(MAKE) -C kernel KDIR=$(KDIR)

tests/test_lifecycle: tests/test_lifecycle.c $(CLI_SRCS) $(CLI_HDRS)
	$(CC) $(ALL_CPPFLAGS) $(CLI_CFLAGS) -o $@ tests/test_lifecycle.c $(CLI_SRCS) $(LDFLAGS)

tests/test_devices: tests/test_devices.c include/gs_devices.h
	$(CC) $(ALL_CPPFLAGS) $(ALL_CFLAGS) -o $@ tests/test_devices.c $(LDFLAGS)

$(PATCH_TESTS): %: %.c $(PATCH_LIB) $(PATCH_HDRS)
	$(CC) $(ALL_CPPFLAGS) -Idriver $(ALL_CFLAGS) -o $@ $< $(PATCH_LIB) $(LDFLAGS)

check test: all $(TESTS)
	@set -e; for t in $(TESTS); do echo "== $$t"; GS_CLI="$(CURDIR)/geforce-sriov" GS_SRC="$(CURDIR)" ./$$t; done
	$(MAKE) -C hook check

# Live install: as root, without DESTDIR. Staging only: make install DESTDIR=/tmp/stage.
# Select GPUs with DEVICES="0000:01:00.0 0000:02:00.0"; grant the boot display GPU among them a
# console release with BOOT_DISPLAY_RELEASE="0000:02:00.0"; set the VF count per GPU with VFS=N.
install: all
	DESTDIR="$(DESTDIR)" ./geforce-sriov install --prefix "$(PREFIX)" --source "$(CURDIR)" \
		$(foreach d,$(DEVICES),--device $(d)) \
		$(foreach d,$(BOOT_DISPLAY_RELEASE),--boot-display-release $(d)) \
		$(if $(VFS),--vfs $(VFS))

uninstall:
	./geforce-sriov uninstall

clean:
	rm -f geforce-sriov driver/geforce-sriov-patch $(TESTS)
	$(MAKE) -C hook clean
	@if [ -d "$(KDIR)" ]; then $(MAKE) -C kernel KDIR="$(KDIR)" clean; fi
