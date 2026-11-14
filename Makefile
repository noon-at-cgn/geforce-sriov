# SPDX-License-Identifier: MIT
# geforce-sriov top-level build. Components:
#   geforce-sriov                  host CLI            (src/)
#   hook/libgeforce_sriov.so       nvidia-vgpud hook   (hook/Makefile)
#   driver/geforce-sriov-patch     driver patcher      (driver/)
#   kernel/geforce_sriov.ko        livepatch module    (kernel/Makefile, `make module`)

VERSION := 0.4.1

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

CLI_SRCS := src/activate.c src/config.c src/hostinfo.c src/install.c src/nvpackage.c src/dkms.c src/nvlibs.c src/uninstall.c src/lifecycle.c src/loadedpatch.c src/hostcheck.c src/status.c src/check.c src/modules.c src/notify.c src/pci.c src/setup.c src/sha256.c src/unselected.c src/util.c
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
PATCH_TESTS := $(patsubst %.c,%,$(wildcard tests/test_patch*.c tests/test_modcheck.c tests/test_insn_find.c))

TESTS := tests/test_lifecycle tests/test_devices tests/test_contracts $(PATCH_TESTS)
# Shell fixtures for the driver step and its libraries; they build synthetic trees in a
# temporary directory and never touch NVIDIA's files.
DRIVER_FIXTURE := tests/fixtures-driver/test-driver.sh
FIXTURES := $(filter-out $(DRIVER_FIXTURE),$(wildcard tests/fixtures-driver/test-*.sh tests/hw/test-*.sh))
DRIVER_SHARDS := 1 2 3 4
DRIVER_SHARD_SUITES := $(addprefix test-driver.sh@,$(DRIVER_SHARDS))
CASE_SHARD_1 := earlier_cap_record plain restore_gone stock_replaced signed_reinstall restore_modified_and_replaced
CASE_SHARD_2 := after_display cuda_libs identity_refused rollback
CASE_SHARD_3 := placement_elf power_loss kernel_update unknown_anchors
CASE_SHARD_4 := cuda desktop_libs two_kernels plain_gate cuda_crc_and_provider display second_version signed_reinstall_elf plain_other_version ada_confirmed ada_refusals restore_companion unknown_refused interrupted_commit package_digest

.PHONY: all hook module check test check-report hw-sync trace trace-diff install uninstall clean

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

tests/test_lifecycle: tests/test_lifecycle.c tests/tmpdir.h $(CLI_SRCS) $(CLI_HDRS)
	$(CC) $(ALL_CPPFLAGS) $(CLI_CFLAGS) -o $@ tests/test_lifecycle.c $(CLI_SRCS) $(LDFLAGS)

tests/test_devices: tests/test_devices.c include/gs_devices.h
	$(CC) $(ALL_CPPFLAGS) $(ALL_CFLAGS) -o $@ tests/test_devices.c $(LDFLAGS)

tests/test_contracts: tests/test_contracts.c src/main.c src/config.c $(CLI_HDRS)
	$(CC) $(ALL_CPPFLAGS) $(CLI_CFLAGS) -ffunction-sections -Wl,--gc-sections -o $@ tests/test_contracts.c src/config.c $(LDFLAGS)

$(PATCH_TESTS): %: %.c tests/tmpdir.h $(PATCH_LIB) $(PATCH_HDRS)
	$(CC) $(ALL_CPPFLAGS) -Idriver $(ALL_CFLAGS) -o $@ $< $(PATCH_LIB) $(LDFLAGS)

# `make check` runs every suite as a target of its own, check/<suite>, through tests/run-suite.sh:
# the suite gets a scratch directory of its own as TMPDIR, and its log, exit status and one summary
# line stay in CHECK_OUT. The order of the suites is the order of this list; `make -jN check` runs
# them side by side.
CHECK_OUT ?= $(CURDIR)/check-out
SUITES := $(TESTS) $(FIXTURES) $(DRIVER_SHARD_SUITES) test-driver-cases hook
CHECK_TARGETS := $(addprefix check/,$(SUITES))
RUN_SUITE := tests/run-suite.sh "$(CHECK_OUT)"

.PHONY: $(CHECK_TARGETS)

check test: $(CHECK_TARGETS)

$(addprefix check/,$(TESTS)): check/%: all %
	@GS_CLI="$(CURDIR)/geforce-sriov" GS_SRC="$(CURDIR)" $(RUN_SUITE) $(notdir $*) ./$*

$(addprefix check/,$(FIXTURES)): check/%: all
	@$(RUN_SUITE) $(notdir $*) sh $*

# test-driver.sh is the longest suite by far, so it runs as shards of its independent cases
# (CASE_SHARD_<n>, balanced by measured time); test-driver-cases fails if a case is in no shard,
# in two, or no longer exists. tests/check-report.sh adds the summed counts of the shards.
$(addprefix check/,$(DRIVER_SHARD_SUITES)): check/test-driver.sh@%: all
	@GS_CASES="$(CASE_SHARD_$*)" $(RUN_SUITE) test-driver.sh@$* sh $(DRIVER_FIXTURE)

check/test-driver-cases:
	@$(RUN_SUITE) test-driver-cases sh -c 'sh "$(DRIVER_FIXTURE)" --list | sort >"$$TMPDIR/all"; \
		printf "%s\n" $(foreach n,$(DRIVER_SHARDS),$(CASE_SHARD_$(n))) | sort >"$$TMPDIR/sharded"; \
		diff "$$TMPDIR/all" "$$TMPDIR/sharded" && echo "test-driver-cases: every case is in exactly one shard"'

check/hook: all
	@$(RUN_SUITE) hook $(MAKE) --no-print-directory -C hook check

# The full check and one summary file: the commit on line 1, whether the tree was clean, one line
# per suite (its last line of output and its time), the exit code of the check on the last line.
# `make check-report REPORT=path` (default check-report.txt); `make -jN check-report` is in parallel.
# A failed suite does not stop the others; the target fails if any suite does.
REPORT ?= check-report.txt
check-report:
	@rm -rf "$(CHECK_OUT)"; start=$$(date +%s); \
	$(MAKE) -k check CHECK_OUT="$(CHECK_OUT)"; rc=$$?; \
	GS_REPORT_JOBS="$(or $(filter -j%,$(MAKEFLAGS)),-j1)" tests/check-report.sh "$(REPORT)" "$(CHECK_OUT)" $$rc $$(( $$(date +%s) - start )) $(notdir $(SUITES)) || rc=$$?; \
	echo "check-report: $(REPORT) (exit code $$rc)"; exit $$rc

# tests/hw/ is the source of truth for the pve smoke scripts. `make hw-sync DEST=<dir>` copies the
# scripts that run on pve (every tests/hw/*.sh except the test-*.sh that make check runs) to DEST,
# e.g. /home/nathan/geforce-sriov-win/v040/final; never edit the copies by hand.
hw-sync:
	@test -n "$(DEST)" || { echo "usage: make hw-sync DEST=<directory>" >&2; exit 2; }
	mkdir -p "$(DEST)"
	cp -p $(filter-out tests/hw/test-%,$(wildcard tests/hw/*.sh)) "$(DEST)/"

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
	rm -rf trace-out "$(CHECK_OUT)"
	$(MAKE) -C hook clean
	@if [ -d "$(KDIR)" ]; then $(MAKE) -C kernel KDIR="$(KDIR)" clean; fi
