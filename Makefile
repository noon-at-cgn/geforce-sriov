# SPDX-License-Identifier: MIT
# geforce-sriov top-level build. Components:
#   geforce-sriov                  host CLI            (src/)
#   hook/libgeforce_sriov.so       nvidia-vgpud hook   (hook/Makefile)
#   driver/geforce-sriov-patch     driver patcher      (driver/)
#   kernel/geforce_sriov.ko        livepatch module    (kernel/Makefile, `make module`)

VERSION := 0.3.0

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
VFS ?=
KDIR ?= /lib/modules/$(shell uname -r)/build

CLI_SRCS := src/activate.c src/config.c src/hostinfo.c src/install.c src/nvpackage.c src/dkms.c src/cudalibs.c src/uninstall.c src/lifecycle.c src/loadedpatch.c src/doctor.c src/status.c src/check.c src/fbslot.c src/modules.c src/pci.c src/setup.c src/sha256.c src/util.c
# The confirmed NVIDIA versions: one table, read by the driver step as it is and turned into the
# CLI's rows here, so that a version is added in one place.
VERSIONS_TBL := scripts/gs-driver-versions.tbl
VERSIONS_INC := src/gs_versions.inc
CLI_HDRS := include/gs_lifecycle.h include/gs_devices.h include/gs_hook.h src/gs_internal.h $(VERSIONS_INC)
# Patcher: driver/main.c holds main(); every other driver/*.c is library code that the
# patcher tests (tests/test_patch*.c) link against.
PATCH_MAIN := driver/main.c
PATCH_LIB := $(filter-out $(PATCH_MAIN),$(wildcard driver/*.c))
PATCH_HDRS := $(wildcard driver/*.h)
PATCH_TESTS := $(patsubst %.c,%,$(wildcard tests/test_patch*.c tests/test_modcheck.c))

TESTS := tests/test_lifecycle tests/test_devices tests/test_contracts tests/test_fbowner $(PATCH_TESTS)
# Shell fixtures for the driver step and its libraries; they build synthetic trees in a
# temporary directory and never touch NVIDIA's files.
FIXTURES := $(wildcard tests/fixtures-driver/test-*.sh)

.PHONY: all hook module check test trace trace-diff install uninstall clean fb-owner-digests

all: geforce-sriov hook driver/geforce-sriov-patch

# One initializer per block of the table, with the fields the CLI uses. A value becomes a C string
# as it is, so a quote or a backslash in the table stops the build.
$(VERSIONS_INC): $(VERSIONS_TBL) Makefile
	awk '/^#/ || /^[ \t]*$$/ { next } \
	     /["\\]/ { print "$<: line " NR ": a quote or a backslash cannot be used" >"/dev/stderr"; bad = 1; exit 1 } \
	     { key = $$0; sub(/=.*/, "", key); value = $$0; sub(/^[^=]*=/, "", value) } \
	     key == "host" { if (rows++) print "},"; printf "{ " } \
	     key ~ /^(host|second|public|guest_ampere|guest_ada|package|stock_plugin|mixed_plugin|patched_plugin)$$/ { \
	         if (!rows) { print "$<: line " NR ": a block starts with its host= line" >"/dev/stderr"; bad = 1; exit 1 } \
	         printf ".%s = \"%s\", ", key, value } \
	     END { if (bad || !rows) exit 1; print "}," }' $(VERSIONS_TBL) >$@.tmp
	mv $@.tmp $@

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

tests/test_contracts: tests/test_contracts.c src/main.c src/config.c $(CLI_HDRS)
	$(CC) $(ALL_CPPFLAGS) $(CLI_CFLAGS) -ffunction-sections -Wl,--gc-sections -o $@ tests/test_contracts.c src/config.c $(LDFLAGS)

# The owner of the Ada VFs' framebuffer slots: the file that goes into nvidia.ko, compiled for
# userspace against declarations of what it uses and run against a model of the RM.
FB_OWNER := driver-patches/fb-owner
tests/test_fbowner: tests/fbowner/test_fbowner.c tests/fbowner/stub.h $(FB_OWNER)/gs-fb-owner.c $(FB_OWNER)/gs-fb-owner.h
	$(CC) $(ALL_CPPFLAGS) -DGS_FB_TEST -Itests/fbowner -I$(FB_OWNER) $(ALL_CFLAGS) -Wdeclaration-after-statement \
		-Wno-unused-parameter -o $@ tests/fbowner/test_fbowner.c $(LDFLAGS)

# After a reviewed change to the owner's files: record their digests in the table of confirmed
# versions (every block that records the owner). tests/fixtures-driver/test-fb-owner.sh fails until then.
fb-owner-digests:
	@c=$$(sha256sum $(FB_OWNER)/gs-fb-owner.c | cut -d' ' -f1); h=$$(sha256sum $(FB_OWNER)/gs-fb-owner.h | cut -d' ' -f1); \
	p=$$(sha256sum $(FB_OWNER)/fb-owner.patch | cut -d' ' -f1); \
	sed "s/^owner_c=.*/owner_c=$$c/; s/^owner_h=.*/owner_h=$$h/; s/^owner_patch=.*/owner_patch=$$p/" $(VERSIONS_TBL) >$(VERSIONS_TBL).tmp && \
	mv $(VERSIONS_TBL).tmp $(VERSIONS_TBL) && echo "$(VERSIONS_TBL): owner_c=$$c owner_h=$$h owner_patch=$$p"

$(PATCH_TESTS): %: %.c $(PATCH_LIB) $(PATCH_HDRS)
	$(CC) $(ALL_CPPFLAGS) -Idriver $(ALL_CFLAGS) -o $@ $< $(PATCH_LIB) $(LDFLAGS)

check test: all $(TESTS)
	@set -e; for t in $(TESTS); do echo "== $$t"; GS_CLI="$(CURDIR)/geforce-sriov" GS_SRC="$(CURDIR)" ./$$t; done
	@set -e; for s in $(FIXTURES); do echo "== $$s"; sh $$s; done
	$(MAKE) -C hook check

# The golden trace of the lifecycle suite: what every CLI run and every simulated-host run
# printed, and the simulated host's log of writes and commands, one file per run in TRACE.
# `make trace-diff BASE=dir` compares a fresh trace with an earlier one; a refactor that
# changes no behaviour leaves it empty.
TRACE ?= trace-out
trace: all tests/test_lifecycle
	rm -rf "$(TRACE)" && mkdir -p "$(TRACE)"
	GS_TRACE_DIR="$(abspath $(TRACE))" GS_CLI="$(CURDIR)/geforce-sriov" GS_SRC="$(CURDIR)" ./tests/test_lifecycle >/dev/null
	@echo "trace: $$(ls "$(TRACE)" | wc -l) files in $(TRACE)"

trace-diff: trace
	@test -n "$(BASE)" || { echo "usage: make trace-diff BASE=<earlier trace directory>"; exit 2; }
	diff -ru "$(BASE)" "$(TRACE)" && echo "trace-diff: identical to $(BASE)"

# Live install: as root, without DESTDIR. Staging only: make install DESTDIR=/tmp/stage.
# Select GPUs with DEVICES="0000:01:00.0 0000:02:00.0"; set the VF count per GPU with VFS=N.
install: all
	DESTDIR="$(DESTDIR)" ./geforce-sriov install --prefix "$(PREFIX)" --source "$(CURDIR)" \
		$(foreach d,$(DEVICES),--device $(d)) \
		$(if $(VFS),--vfs $(VFS))

uninstall:
	./geforce-sriov uninstall

clean:
	rm -f geforce-sriov driver/geforce-sriov-patch $(TESTS) $(VERSIONS_INC) $(VERSIONS_INC).tmp
	rm -rf trace-out
	$(MAKE) -C hook clean
	@if [ -d "$(KDIR)" ]; then $(MAKE) -C kernel KDIR="$(KDIR)" clean; fi
