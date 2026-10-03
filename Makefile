# =============================================================================
# StratumOS - build system
# =============================================================================
#
#   make                     build everything for the default architecture
#   make ARCH=riscv64        ...for the second one
#   make run                 boot it in QEMU
#   make test                host unit tests + boot tests, headless
#   make fuzz                libFuzzer over the real parsers (needs clang)
#   make portability         how much of the kernel builds for riscv64
#   make clean               remove build output
#
#   x86 also has: run-iso, run-net, run-x86-64, run-direct, run-serial,
#                 bench, debug, gdb, image-check, fs, user
#
# ARCHITECTURES
# -------------
# One variable selects the toolchain, the flags, the source set, the linker
# script, the object tree and what `run` and `test` mean:
#
#   make                 ARCH=x86      i386, the kernel's home architecture
#   make ARCH=riscv64    riscv64       rv64imac on QEMU's `virt` board
#
# Each architecture's build lives beside the code it builds, for the same
# reason the sources do - sources.mk for *what* it compiles, arch.mk for
# *how*. Adding a third means adding a directory, not editing a switch
# statement in here. The contract they have to satisfy is documented at the
# top of kernel/arch/x86/arch.mk, and why they are two files rather than one
# at the top of kernel/arch/x86/sources.mk.
#
# Objects and artefacts go to $(BUILD)/$(ARCH), so two architectures can be
# built side by side and neither can pick up the other's stale objects.
# =============================================================================

PYTHON := python3

# ---- architecture selection -------------------------------------------------

ARCH ?= x86

KSRC    := kernel
ARCHDIR := $(KSRC)/arch/$(ARCH)

# Discovered rather than listed, so that the error message below stays true
# when an architecture is added.
ARCHES := $(sort $(patsubst $(KSRC)/arch/%/arch.mk,%,$(wildcard $(KSRC)/arch/*/arch.mk)))

ifeq ($(wildcard $(ARCHDIR)/arch.mk),)
$(error unknown ARCH '$(ARCH)' - available: $(ARCHES))
endif

# ---- layout -----------------------------------------------------------------

BUILD   := build
OUT     := $(BUILD)/$(ARCH)
INCLUDE := $(KSRC)/include
BOOTSRC := boot
USERSRC := user
TOOLS   := tools

WARNINGS := -Wall -Wextra -Werror -Wshadow -Wpointer-arith -Wcast-align \
            -Wstrict-prototypes -Wmissing-prototypes -Wredundant-decls \
            -Wno-unused-parameter

# ---- the portable sources ---------------------------------------------------
#
# Listed here, once, because which of them an architecture can actually link
# is the measurement docs/PORTING.md reports - and a measurement wants one
# definition of what is being measured.
#
# An arch.mk selects either whole directories (ARCH_SHARED_DIRS) or named
# files (ARCH_SHARED_FILES). x86 takes every directory; riscv64 names four
# files, because kernel/core also holds the scheduler and the x86 test suite
# and claiming the directory would overstate the result.

SHARED_DIRS := core mm drivers fs net shell

# ---- the architecture's own build, in two phases ---------------------------
#
# sources.mk declares *what* this architecture compiles; arch.mk defines *how*.
# They are separate because a rule's prerequisites are expanded when the rule
# is read, so the shared object list has to exist before arch.mk's link rule
# is seen - and the shared object list depends on what sources.mk selected.
#
# The alternative, including one file twice, warns about an overriding recipe
# for every rule in it. Two phases is what the dependency actually is.

include $(ARCHDIR)/sources.mk

SHARED_SOURCES := $(sort $(ARCH_SHARED_FILES) \
                    $(foreach d,$(ARCH_SHARED_DIRS),$(wildcard $(KSRC)/$(d)/*.c)))
SHARED_OBJECTS := $(SHARED_SOURCES:%.c=$(OUT)/%.o)

include $(ARCHDIR)/arch.mk

DEPS := $(SHARED_OBJECTS:.o=.d) $(ARCH_C_OBJECTS:.o=.d)

# A .c and an assembly file with the same base name in the same directory
# both compile to the same object path. On the x86 side whichever rule ran
# second silently won, which presents as an undefined reference to a symbol
# that is plainly there; on the riscv side the same object reaches the linker
# twice and every symbol in it is a duplicate.
#
# Caught twice - once with longmode.c beside what is now longmode_tramp.asm,
# and again immediately afterwards, because that guard only looked at the x86
# lists and the second architecture walked into the same hole with trap.c and
# trap.S. Now there is one guard, and it runs for whichever architecture is
# being built, which is what the earlier version should have been.
OBJECT_COLLISIONS := $(strip $(filter $(ARCH_ASM_OBJECTS), \
                             $(ARCH_C_OBJECTS) $(SHARED_OBJECTS)))
ifneq ($(OBJECT_COLLISIONS),)
$(error two $(ARCH) sources compile to the same object: $(OBJECT_COLLISIONS) - rename one of them, as gdt.c and gdt_flush.asm already do)
endif

# ---- the common verbs -------------------------------------------------------

.PHONY: all
all: arch-all

.PHONY: kernel
kernel: $(KERNEL_ELF)

.PHONY: test
test: test-host arch-test
	@echo
	@echo "  all $(ARCH) tests passed"

# ---- compiling --------------------------------------------------------------
#
# One rule for every C file, architecture and shared alike, with the flags
# coming from the arch.mk. Assembly differs enough between nasm and clang
# that each architecture brings its own rule.

$(OUT)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# ---- host tests -------------------------------------------------------------
#
# Not architecture-specific: these compile the kernel's real formatter,
# string and division sources for the *host* and diff them against glibc, so
# they are the same test whichever kernel is being built. Built under
# $(BUILD) rather than $(OUT) for that reason.

HOST_TEST_SRC := tests/host/test_printf.c \
                 $(KSRC)/core/printf.c $(KSRC)/core/string.c \
                 $(KSRC)/core/div64.c

.PHONY: test-host
test-host: $(BUILD)/test_printf
	@echo "  RUN     host unit tests"
	@./$<

$(BUILD)/test_printf: $(HOST_TEST_SRC)
	@mkdir -p $(dir $@)
	@echo "  CC      $@ (host, 32-bit)"
	@gcc -m32 -std=gnu11 -Wall -Wextra -Werror -g \
		-I$(INCLUDE) $(HOST_TEST_SRC) -o $@

# ---- fuzzing -----------------------------------------------------------------
#
# libFuzzer over the *real* kernel sources, compiled for the host with
# AddressSanitizer and UndefinedBehaviorSanitizer. See tests/fuzz/shim.h for
# what the shim supplies and docs/FUZZING.md for the argument.
#
# clang rather than gcc, because libFuzzer is a clang runtime. 32-bit, because
# the kernel's vaddr_t is 32 bits and its pointer arithmetic assumes it - and
# because the ELF loader's mapped pages have to land at the addresses the
# kernel asked for, which only works in a 32-bit address space.

FUZZ_CC      ?= clang
FUZZ_TARGETS := elf fat heap acpi
FUZZ_BINS    := $(FUZZ_TARGETS:%=$(BUILD)/fuzz/fuzz_%)
FUZZ_CORPUS  := tests/fuzz/corpus

# -fno-omit-frame-pointer so a report names the right frames; -g for line
# numbers; -O1 because -O2 inlines away the frames a report needs and -O0
# makes the fuzzer four times slower for no extra coverage.
FUZZ_FLAGS := -m32 -std=gnu11 -g -O1 -fno-omit-frame-pointer \
              -fsanitize=fuzzer,address,undefined \
              -fno-sanitize-recover=undefined \
              -DSTRATUM_FUZZING=1 \
              -I$(INCLUDE) -Itests/fuzz \
              -Wall -Wextra -Wno-unused-parameter

# Each target pulls in only what it needs. Linking the whole kernel would
# drag in the arch layer, which does not compile for a hosted target.
FUZZ_SRC_COMMON := tests/fuzz/shim.c $(KSRC)/core/printf.c \
                   $(KSRC)/core/string.c $(KSRC)/core/div64.c

$(BUILD)/fuzz/fuzz_elf: tests/fuzz/fuzz_elf.c $(KSRC)/core/elf.c \
                        $(FUZZ_SRC_COMMON)
	@mkdir -p $(dir $@) $(FUZZ_CORPUS)/elf
	@echo "  FUZZCC  $@"
	@$(FUZZ_CC) $(FUZZ_FLAGS) $^ -o $@

$(BUILD)/fuzz/fuzz_fat: tests/fuzz/fuzz_fat.c $(KSRC)/fs/fat16.c \
                        $(FUZZ_SRC_COMMON)
	@mkdir -p $(dir $@) $(FUZZ_CORPUS)/fat
	@echo "  FUZZCC  $@"
	@$(FUZZ_CC) $(FUZZ_FLAGS) $^ -o $@

# The one target that uses the kernel's own allocator rather than the shim's,
# because it is the thing being fuzzed.
$(BUILD)/fuzz/fuzz_heap: tests/fuzz/fuzz_heap.c $(KSRC)/mm/heap.c \
                         $(FUZZ_SRC_COMMON)
	@mkdir -p $(dir $@) $(FUZZ_CORPUS)/heap
	@echo "  FUZZCC  $@"
	@$(FUZZ_CC) $(FUZZ_FLAGS) -DSTRATUM_FUZZ_REAL_HEAP=1 $^ -o $@

$(BUILD)/fuzz/fuzz_acpi: tests/fuzz/fuzz_acpi.c $(KSRC)/arch/x86/acpi.c \
                         $(FUZZ_SRC_COMMON)
	@mkdir -p $(dir $@) $(FUZZ_CORPUS)/acpi
	@echo "  FUZZCC  $@"
	@$(FUZZ_CC) $(FUZZ_FLAGS) $^ -o $@

.PHONY: fuzz-build
fuzz-build:
	@command -v $(FUZZ_CC) >/dev/null 2>&1 || \
		{ echo "  SKIP    fuzzing ($(FUZZ_CC) is not installed)"; exit 0; }
	@$(MAKE) --no-print-directory $(FUZZ_BINS)

# Seed each corpus from inputs the kernel itself produces, which is what makes
# the first minute of fuzzing useful rather than spent rediscovering what an
# ELF header looks like.
# Seeded from the current architecture's own artefacts - its ring-3 ELF and
# its FAT16 image - which is why this depends on them rather than on paths.
# Only x86 produces either today, so `make fuzz` with ARCH=riscv64 would have
# nothing to seed from; the targets being fuzzed are host builds of shared
# parsers and are the same either way.
.PHONY: fuzz-seed
fuzz-seed: $(FS_IMG) $(USER_ELFS)
	@$(PYTHON) $(TOOLS)/fuzz-seed.py --corpus $(FUZZ_CORPUS) \
		--user-elf $(OUT)/user/init.stripped.elf \
		--fs-image $(FS_IMG)

# A short run, for CI and for a sanity check after a change. Long runs are
# what a developer does by hand; a bounded one is what belongs in a pipeline.
FUZZ_RUNS ?= 20000

.PHONY: fuzz
fuzz: fuzz-build fuzz-seed
	@for t in $(FUZZ_TARGETS); do \
		[ -x $(BUILD)/fuzz/fuzz_$$t ] || continue; \
		printf "  FUZZ    %-6s " $$t; \
		$(BUILD)/fuzz/fuzz_$$t $(FUZZ_CORPUS)/$$t \
			-runs=$(FUZZ_RUNS) -max_total_time=60 -timeout=10 \
			-rss_limit_mb=2048 -print_final_stats=1 \
			> $(BUILD)/fuzz/$$t.log 2>&1 \
			&& echo "ok   ($$(grep -oE 'cov: [0-9]+' $(BUILD)/fuzz/$$t.log | tail -1), $$(grep -oE 'exec/s: [0-9]+' $(BUILD)/fuzz/$$t.log | tail -1))" \
			|| { echo "FAILED - see $(BUILD)/fuzz/$$t.log"; tail -30 $(BUILD)/fuzz/$$t.log; exit 1; }; \
	done
	@echo "  all fuzz targets survived $(FUZZ_RUNS) runs each"

# Reproduce one crash file against one target, with the kernel's own log
# output turned on - which is most of what makes a reproducer readable.
.PHONY: fuzz-repro
fuzz-repro:
	@[ -n "$(TARGET)" ] && [ -n "$(CASE)" ] || \
		{ echo "usage: make fuzz-repro TARGET=elf CASE=path/to/crash"; exit 1; }
	@$(MAKE) --no-print-directory $(BUILD)/fuzz/fuzz_$(TARGET)
	STRATUM_FUZZ_VERBOSE=1 $(BUILD)/fuzz/fuzz_$(TARGET) $(CASE)


# ---- portability ------------------------------------------------------------
#
# How much of the kernel compiles for the second architecture as it stands.
# The number is the measurement docs/PORTING.md reports, so it is generated
# rather than quoted. Informational, not a gate - see DESIGN-DECISIONS 35.
.PHONY: portability
portability:
	@$(PYTHON) $(TOOLS)/portability.py

# ---- housekeeping -----------------------------------------------------------

.PHONY: format
format:
	@command -v clang-format >/dev/null 2>&1 || \
		{ echo "clang-format is not installed"; exit 1; }
	@find $(KSRC) $(USERSRC) tests -name '*.c' -o -name '*.h' | xargs clang-format -i
	@echo "  formatted $$(find $(KSRC) $(USERSRC) tests -name '*.c' -o -name '*.h' | wc -l) files"

.PHONY: format-check
format-check:
	@command -v clang-format >/dev/null 2>&1 || \
		{ echo "clang-format is not installed; skipping"; exit 0; }
	@find $(KSRC) $(USERSRC) tests -name '*.c' -o -name '*.h' | \
		xargs clang-format --dry-run --Werror

.PHONY: lines
lines:
	@echo "Lines of code by area:"
	@for d in boot kernel/arch/x86 kernel/arch/riscv64 kernel/core kernel/mm \
	          kernel/drivers kernel/fs kernel/net kernel/shell kernel/include \
	          user tools tests ; do \
		n=$$(find $$d -type f \( -name '*.c' -o -name '*.h' -o -name '*.asm' \
		     -o -name '*.S' -o -name '*.inc' -o -name '*.py' -o -name '*.mk' \) \
		     -exec cat {} + 2>/dev/null | wc -l); \
		printf "  %-20s %6s\n" "$$d" "$$n"; \
	done

# Removes every architecture's tree, not just the one being built: a `clean`
# that leaves another architecture's objects behind is a `clean` that will one
# day be blamed for a stale build.
.PHONY: clean
clean:
	@rm -rf $(BUILD)
	@echo "  cleaned"

.PHONY: toolchain
toolchain:
	@echo "Architecture:     $(ARCH)   (available: $(ARCHES))"
	@echo "Toolchain in use: $(TOOLCHAIN)"
	@printf "  %-12s " "CC";      $(CC) --version | head -1
	@printf "  %-12s " "LD";      $(LD) --version 2>/dev/null | head -1 || echo "MISSING"
	@$(MAKE) --no-print-directory arch-toolchain
	@printf "  %-12s " "Python";  $(PYTHON) --version

-include $(DEPS)
