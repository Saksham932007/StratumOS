# =============================================================================
# StratumOS - build system
# =============================================================================
#
#   make              build the kernel, the bootable disk image and the ISO
#   make run          boot the custom bootloader image in QEMU
#   make run-iso      boot via GRUB/Multiboot2 in QEMU
#   make test         host unit tests + both boot paths under QEMU, headless
#   make fuzz         libFuzzer over the real parsers, bounded (needs clang)
#   make run-x86-64   boot on a CPU that has long mode, for `longmode`
#   make run-net      boot with an Ethernet card, dumping frames to a pcap
#   make debug        start QEMU stopped, waiting for GDB on :1234
#   make gdb          attach GDB to a waiting QEMU
#   make clean        remove build output
#
# A cross-compiler (i686-elf-gcc) is used when one is on PATH; otherwise the
# host GCC is driven with -m32, which works on any x86-64 Linux with multilib
# installed. Both are checked for, and the choice is reported, because
# "it built on my machine" is not a build system.
# =============================================================================

# ---- toolchain selection ----------------------------------------------------

CROSS_PREFIX := i686-elf-
HAVE_CROSS   := $(shell command -v $(CROSS_PREFIX)gcc >/dev/null 2>&1 && echo yes)

ifeq ($(HAVE_CROSS),yes)
  CC       := $(CROSS_PREFIX)gcc
  LD       := $(CROSS_PREFIX)ld
  OBJCOPY  := $(CROSS_PREFIX)objcopy
  OBJDUMP  := $(CROSS_PREFIX)objdump
  ARCHFLAG :=
  TOOLCHAIN := $(CROSS_PREFIX)gcc (cross)
else
  CC       := gcc
  LD       := ld
  OBJCOPY  := objcopy
  OBJDUMP  := objdump
  ARCHFLAG := -m32
  TOOLCHAIN := host gcc -m32
endif

AS     := nasm
PYTHON := python3
QEMU   := qemu-system-i386

# ---- layout -----------------------------------------------------------------

BUILD     := build
KSRC      := kernel
INCLUDE   := $(KSRC)/include
BOOTSRC   := boot
USERSRC   := user
TOOLS     := tools

# Two kernels from one link: the bootable image is stripped of debug info so
# that stage 2 has plenty of room to stage it below the EBDA, while the
# unstripped copy keeps full DWARF for GDB.
KERNEL_ELF   := $(BUILD)/stratum.elf
KERNEL_DEBUG := $(BUILD)/stratum.debug.elf
DISK_IMG   := $(BUILD)/stratum.img
ISO        := $(BUILD)/stratum.iso
# Separate images whose embedded command line runs the self-test suite and
# then shuts the machine down, so CI needs no keystrokes and no timeout.
TEST_IMG   := $(BUILD)/stratum-test.img
TEST_ISO   := $(BUILD)/stratum-test.iso
# A quiet image for the interactive shell test: no demo tasks and no ring-3
# payload at boot, so the transcript the harness reads is deterministic.
SHELL_IMG  := $(BUILD)/stratum-shell.img
# Benchmarks plus a profile, then shut down. Separate from the self-test image
# so correctness and measurement are asserted independently.
BENCH_IMG  := $(BUILD)/stratum-bench.img
STAGE1_BIN := $(BUILD)/stage1.bin
STAGE2_BIN := $(BUILD)/stage2.bin

# ---- flags ------------------------------------------------------------------

# -ffreestanding            no hosted C library is present
# -nostdlib                 ...and none should be linked
# -fno-pie -fno-pic         the kernel is linked to a fixed address
# -fno-stack-protector      the guard would call into a libc that is not there
# -fno-omit-frame-pointer   keeps the panic backtrace walkable
# -fno-asynchronous-...     no unwind tables; nothing here throws
# -mgeneral-regs-only       forbids SSE/MMX, which the kernel never enables in
#                           CR4 and which the compiler would otherwise use for
#                           struct copies - producing a #UD at boot
WARNINGS := -Wall -Wextra -Werror -Wshadow -Wpointer-arith -Wcast-align \
            -Wstrict-prototypes -Wmissing-prototypes -Wredundant-decls \
            -Wno-unused-parameter

CFLAGS := $(ARCHFLAG) -std=gnu11 -O2 -g3 \
          -ffreestanding -nostdlib -fno-builtin \
          -fno-pie -fno-pic -fno-stack-protector \
          -fno-omit-frame-pointer -fno-asynchronous-unwind-tables \
          -mgeneral-regs-only -march=i686 \
          -I$(INCLUDE) $(WARNINGS) -MMD -MP $(CFLAGS_EXTRA)

ASFLAGS  := -f elf32 -g -F dwarf -I $(KSRC)/arch/x86/
# --no-warn-rwx-segments: a kernel image is one writable, executable blob by
# design; the warning is aimed at userspace hardening and is pure noise here.
LDFLAGS  := -m elf_i386 -T linker/kernel.ld -nostdlib -z noexecstack \
            --build-id=none --no-warn-rwx-segments

# ---- sources ----------------------------------------------------------------

C_SOURCES := $(sort $(wildcard $(KSRC)/core/*.c) \
                    $(wildcard $(KSRC)/arch/x86/*.c) \
                    $(wildcard $(KSRC)/mm/*.c) \
                    $(wildcard $(KSRC)/drivers/*.c) \
                    $(wildcard $(KSRC)/fs/*.c) \
                    $(wildcard $(KSRC)/net/*.c) \
                    $(wildcard $(KSRC)/shell/*.c))

ASM_SOURCES := $(sort $(wildcard $(KSRC)/arch/x86/*.asm))

C_OBJECTS   := $(C_SOURCES:%.c=$(BUILD)/%.o)
ASM_OBJECTS := $(ASM_SOURCES:%.asm=$(BUILD)/%.o)

# A .c and a .asm with the same base name in the same directory would both
# compile to the same object path, and whichever rule ran second would
# silently win - which presents as an undefined reference to a symbol that is
# plainly there in the source. Caught once, while adding longmode.c beside
# what is now longmode_tramp.asm; not worth catching twice.
OBJECT_COLLISIONS := $(strip $(filter $(ASM_OBJECTS),$(C_OBJECTS)))
ifneq ($(OBJECT_COLLISIONS),)
$(error two sources compile to the same object: $(OBJECT_COLLISIONS) - rename one of them, as gdt.c and gdt_flush.asm already do)
endif

# The ring-3 programs are separate ELFs, linked for user space and embedded in
# the kernel image as blobs. Defined here, before LINK_ORDER, because that is
# expanded immediately.
#
# `init` is the program the kernel starts; `hello` exists so that exec() has
# a genuinely different image to replace it with, which is the only way to
# show that exec replaced an address space rather than reloading one; `fuzz`
# attacks the syscall boundary from ring 3, which is the only seat from which
# the kernel's pointer validation can actually be attacked.
USER_PROGS := init hello fuzz
USER_ELFS  := $(USER_PROGS:%=$(BUILD)/user/%.elf)
USER_BLOBS := $(USER_PROGS:%=$(BUILD)/user/%_blob.o)

# boot.asm must be linked first so that .multiboot lands at the start of the
# image, where the Multiboot2 specification requires the header to be.
BOOT_OBJ    := $(BUILD)/$(KSRC)/arch/x86/boot.o
LINK_ORDER  := $(BOOT_OBJ) $(filter-out $(BOOT_OBJ),$(ASM_OBJECTS)) \
               $(C_OBJECTS) $(USER_BLOBS)

OBJECTS := $(ASM_OBJECTS) $(C_OBJECTS) $(USER_BLOBS)
DEPS    := $(C_OBJECTS:.o=.d)

# ---- top-level targets ------------------------------------------------------

# Every image is built by `all`. Building only some of them invites the
# classic confusion of testing a stale artefact and drawing conclusions from it.
.PHONY: all
all: $(DISK_IMG) $(ISO) $(TEST_IMG) $(TEST_ISO) $(SHELL_IMG) $(BENCH_IMG)
	@echo
	@echo "  StratumOS built with $(TOOLCHAIN)"
	@printf "  %-22s %s\n" "kernel ELF" "$(KERNEL_ELF)"
	@printf "  %-22s %s\n" "disk image (stage1+2)" "$(DISK_IMG)"
	@printf "  %-22s %s\n" "GRUB ISO (multiboot2)" "$(ISO)"
	@printf "  %-22s %s\n" "self-test images" "$(TEST_IMG), $(TEST_ISO)"
	@echo
	@echo "  make run      boot the custom bootloader"
	@echo "  make run-iso  boot through GRUB"
	@echo "  make test     run every test, headless"
	@echo "  make fuzz     fuzz the parsers (see docs/FUZZING.md)"
	@echo "  make run-net  boot with a network card (see docs/NETWORK.md)"
	@echo

.PHONY: kernel
kernel: $(KERNEL_ELF)

# ---- compilation ------------------------------------------------------------

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.asm
	@mkdir -p $(dir $@)
	@echo "  AS      $<"
	@$(AS) $(ASFLAGS) $< -o $@

# ---- the symbol table, and the multi-pass link it requires -----------------
#
# Embedding a symbol table changes the addresses the table describes, so one
# link pass cannot produce a correct one. Three passes converge:
#
#   A  link against an empty table   -> enumerate the symbol NAMES
#   B  link against the real table   -> addresses settle, because the table's
#                                       size depends on names and count only
#   C  regenerate and relink         -> byte-identical layout to B
#
# Pass C then verifies that the embedded table really does describe the kernel
# it is embedded in, rather than trusting the argument above.

KSYMS_EMPTY := $(BUILD)/ksyms_empty.c
KSYMS_A     := $(BUILD)/ksyms_a.c
KSYMS_B     := $(BUILD)/ksyms_b.c

$(KSYMS_EMPTY): $(TOOLS)/gen-ksyms.py
	@mkdir -p $(dir $@)
	@$(PYTHON) $(TOOLS)/gen-ksyms.py --empty -o $@

# Generated sources live in $(BUILD), so they need their own rule; the generic
# $(BUILD)/%.o: %.c pattern would look for them outside the build tree.
$(BUILD)/ksyms_%.o: $(BUILD)/ksyms_%.c
	@echo "  CC      $< "
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/pass_a.elf: $(OBJECTS) $(BUILD)/ksyms_empty.o linker/kernel.ld
	@mkdir -p $(dir $@)
	@echo "  LD      pass A (enumerate symbols)"
	@$(LD) $(LDFLAGS) $(LINK_ORDER) $(BUILD)/ksyms_empty.o -o $@

$(KSYMS_A): $(BUILD)/pass_a.elf $(TOOLS)/gen-ksyms.py
	@$(PYTHON) $(TOOLS)/gen-ksyms.py $< -o $@

$(BUILD)/pass_b.elf: $(OBJECTS) $(BUILD)/ksyms_a.o linker/kernel.ld
	@echo "  LD      pass B (addresses settle)"
	@$(LD) $(LDFLAGS) $(LINK_ORDER) $(BUILD)/ksyms_a.o -o $@

$(KSYMS_B): $(BUILD)/pass_b.elf $(TOOLS)/gen-ksyms.py
	@$(PYTHON) $(TOOLS)/gen-ksyms.py $< -o $@

$(KERNEL_DEBUG): $(OBJECTS) $(BUILD)/ksyms_b.o linker/kernel.ld
	@mkdir -p $(dir $@)
	@echo "  LD      $@ (pass C, final)"
	@$(LD) $(LDFLAGS) $(LINK_ORDER) $(BUILD)/ksyms_b.o -o $@
	@echo "  VERIFY  embedded symbol table describes this kernel"
	@$(PYTHON) $(TOOLS)/gen-ksyms.py $@ -o $(BUILD)/ksyms_check.c \
		--verify $(KSYMS_B)

$(KERNEL_ELF): $(KERNEL_DEBUG) $(TOOLS)/check-kernel.py
	@echo "  STRIP   $@"
	@$(OBJCOPY) --strip-debug --strip-unneeded $< $@
	@# Validated against the unstripped ELF, because some checks need its
	@# symbol table (locating the embedded user program). --matches then
	@# proves the stripped image a loader sees is byte-for-byte identical,
	@# rather than assuming --strip-debug only touched debug sections.
	@echo "  CHECK   multiboot2 header, higher-half split, embedded program"
	@$(PYTHON) $(TOOLS)/check-kernel.py $(KERNEL_DEBUG) --matches $@
	@printf "  SIZE    %s bytes bootable, %s bytes with debug info\n" \
		"$$(stat -c%s $@)" "$$(stat -c%s $<)"

# ---- the ring-3 program -----------------------------------------------------
#
# Built as a genuinely separate program: its own ELF, linked at a *user*
# address (see user/user.ld), with no kernel headers beyond the syscall ABI.
#
# That separation is not cosmetic. A program linked at a kernel address and
# then mapped elsewhere would have every absolute reference - every string
# literal - pointing into the kernel's half, where ring 3 cannot read. Linking
# it for user space is the only way its own addresses are usable by it.
#
# objcopy then turns the ELF into an object file with three symbols
# (_binary_init_elf_start/_end/_size) so the kernel can embed and load it. The
# `cd` keeps those symbol names short and independent of the build path.

USER_CFLAGS := $(ARCHFLAG) -std=gnu11 -O2 -g3 \
               -ffreestanding -nostdlib -fno-builtin \
               -fno-pie -fno-pic -fno-stack-protector \
               -fno-asynchronous-unwind-tables \
               -mgeneral-regs-only -march=i686 \
               -I$(INCLUDE) -I$(USERSRC) $(WARNINGS)

$(BUILD)/user/%.o: $(USERSRC)/%.c $(USERSRC)/syscall.h
	@mkdir -p $(dir $@)
	@echo "  CC      $< (ring 3)"
	@$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/user/%.elf: $(BUILD)/user/%.o $(USERSRC)/user.ld
	@echo "  LD      $@ (ring 3)"
	@$(LD) -m elf_i386 -T $(USERSRC)/user.ld -nostdlib \
		--no-warn-rwx-segments --build-id=none $< -o $@

# Stripped before embedding: the debug info is three times the size of the
# program, and it would be carried inside the kernel image for no benefit.
$(BUILD)/user/%.stripped.elf: $(BUILD)/user/%.elf
	@$(OBJCOPY) --strip-all $< $@

# One section name per program, so two blobs do not collide in the link.
$(BUILD)/user/%_blob.o: $(BUILD)/user/%.stripped.elf
	@echo "  BLOB    $@"
	@cd $(dir $<) && $(OBJCOPY) -I binary -O elf32-i386 -B i386 \
		--rename-section .data=.rodata.userblob.$*,alloc,load,readonly,data,contents \
		--set-section-alignment .rodata.userblob.$*=4 \
		$*.stripped.elf $(notdir $@)
	@# objcopy derives the blob's symbol names from the input filename, so
	@# rename them back to the stable _binary_<prog>_elf_* the kernel expects.
	@$(OBJCOPY) \
		--redefine-sym _binary_$*_stripped_elf_start=_binary_$*_elf_start \
		--redefine-sym _binary_$*_stripped_elf_end=_binary_$*_elf_end \
		--redefine-sym _binary_$*_stripped_elf_size=_binary_$*_elf_size \
		$@

# Keep the intermediates: make would otherwise delete them after each build
# and relink the kernel every time.
.PRECIOUS: $(BUILD)/user/%.o $(BUILD)/user/%.elf $(BUILD)/user/%.stripped.elf

.PHONY: user
user: $(USER_ELFS)
	@for e in $(USER_ELFS); do echo "== $$e"; $(OBJDUMP) -h $$e; done

# ---- bootloader -------------------------------------------------------------

$(STAGE1_BIN): $(BOOTSRC)/stage1.asm
	@mkdir -p $(dir $@)
	@echo "  AS      $< (flat binary)"
	@$(AS) -f bin -I $(BOOTSRC)/ $< -o $@

$(STAGE2_BIN): $(BOOTSRC)/stage2.asm
	@mkdir -p $(dir $@)
	@echo "  AS      $< (flat binary)"
	@$(AS) -f bin -I $(BOOTSRC)/ $< -o $@

# ---- the on-disk filesystem -------------------------------------------------
#
# A FAT16 partition carrying the same ring-3 programs that are embedded in the
# kernel image. Both exist on purpose: `exec` prefers the disk, and falls back
# to the embedded copy when there is no filesystem - which is the case on the
# GRUB ISO boot path, and is why the kernel can be tested through both of its
# loaders.
#
# tools/mkfat.py writes the image rather than mformat/mcopy, so the layout is
# chosen here and the test suite can assert on specific bytes in specific
# clusters. See its header for the rest of the argument.

FSROOT   := $(BUILD)/fsroot
FS_IMG   := $(BUILD)/stratum-fs.img
FS_KIB   := 16384

$(FS_IMG): $(USER_PROGS:%=$(BUILD)/user/%.stripped.elf) $(TOOLS)/mkfat.py
	@rm -rf $(FSROOT)
	@mkdir -p $(FSROOT)/bin $(FSROOT)/etc
	@for p in $(USER_PROGS); do \
		cp $(BUILD)/user/$$p.stripped.elf $(FSROOT)/bin/$$(echo $$p | tr a-z A-Z); \
	done
	@printf 'StratumOS root filesystem\r\nFAT16, read-only, mounted at boot.\r\n' \
		> $(FSROOT)/README.TXT
	@printf 'console=vga,serial\r\ninit=/bin/INIT\r\n' > $(FSROOT)/etc/MOTD.TXT
	@echo "  MKFAT   $@"
	@$(PYTHON) $(TOOLS)/mkfat.py --root $(FSROOT) --output $@ \
		--size-kib $(FS_KIB) --quiet

.PHONY: image-check
image-check: $(DISK_IMG)
	@$(PYTHON) $(TOOLS)/check-image.py $(DISK_IMG) --verbose

.PHONY: fs
fs: $(FS_IMG)
	@$(PYTHON) $(TOOLS)/mkfat.py --root $(FSROOT) --output $(FS_IMG) \
		--size-kib $(FS_KIB)

# ---- images -----------------------------------------------------------------

$(DISK_IMG): $(STAGE1_BIN) $(STAGE2_BIN) $(KERNEL_ELF) $(FS_IMG) $(TOOLS)/mkimage.py
	@echo "  IMAGE   $@"
	@$(PYTHON) $(TOOLS)/mkimage.py \
		--stage1 $(STAGE1_BIN) --stage2 $(STAGE2_BIN) \
		--kernel $(KERNEL_ELF) --fs $(FS_IMG) --output $@
	@$(PYTHON) $(TOOLS)/check-image.py $@

$(TEST_IMG): $(STAGE1_BIN) $(STAGE2_BIN) $(KERNEL_ELF) $(FS_IMG) $(TOOLS)/mkimage.py
	@echo "  IMAGE   $@ (self-test)"
	@$(PYTHON) $(TOOLS)/mkimage.py --quiet \
		--stage1 $(STAGE1_BIN) --stage2 $(STAGE2_BIN) \
		--kernel $(KERNEL_ELF) --fs $(FS_IMG) --output $@ \
		--cmdline "autotest"
	@$(PYTHON) $(TOOLS)/check-image.py $@

$(ISO): $(KERNEL_ELF) $(TOOLS)/grub.cfg
	@if ! command -v grub-mkrescue >/dev/null 2>&1; then \
		echo "  SKIP    $@ (grub-mkrescue not installed)"; \
		exit 0; \
	fi
	@echo "  ISO     $@"
	@rm -rf $(BUILD)/isoroot
	@mkdir -p $(BUILD)/isoroot/boot/grub
	@cp $(KERNEL_ELF) $(BUILD)/isoroot/boot/stratum.elf
	@cp $(TOOLS)/grub.cfg $(BUILD)/isoroot/boot/grub/grub.cfg
	@grub-mkrescue -o $@ $(BUILD)/isoroot >/dev/null 2>&1 || \
		(echo "  ERROR   grub-mkrescue failed"; exit 1)

$(SHELL_IMG): $(STAGE1_BIN) $(STAGE2_BIN) $(KERNEL_ELF) $(FS_IMG) $(TOOLS)/mkimage.py
	@echo "  IMAGE   $@ (interactive)"
	@$(PYTHON) $(TOOLS)/mkimage.py --quiet \
		--stage1 $(STAGE1_BIN) --stage2 $(STAGE2_BIN) \
		--kernel $(KERNEL_ELF) --fs $(FS_IMG) --output $@ \
		--cmdline "nodemo nousermode loglevel=warn"
	@$(PYTHON) $(TOOLS)/check-image.py $@

$(BENCH_IMG): $(STAGE1_BIN) $(STAGE2_BIN) $(KERNEL_ELF) $(FS_IMG) $(TOOLS)/mkimage.py
	@echo "  IMAGE   $@ (benchmarks)"
	@$(PYTHON) $(TOOLS)/mkimage.py --quiet \
		--stage1 $(STAGE1_BIN) --stage2 $(STAGE2_BIN) \
		--kernel $(KERNEL_ELF) --fs $(FS_IMG) --output $@ \
		--cmdline "autobench nodemo nousermode loglevel=warn"
	@$(PYTHON) $(TOOLS)/check-image.py $@

$(TEST_ISO): $(KERNEL_ELF) $(TOOLS)/grub-test.cfg
	@if ! command -v grub-mkrescue >/dev/null 2>&1; then \
		echo "  SKIP    $@ (grub-mkrescue not installed)"; \
		exit 0; \
	fi
	@echo "  ISO     $@ (self-test)"
	@rm -rf $(BUILD)/isoroot-test
	@mkdir -p $(BUILD)/isoroot-test/boot/grub
	@cp $(KERNEL_ELF) $(BUILD)/isoroot-test/boot/stratum.elf
	@cp $(TOOLS)/grub-test.cfg $(BUILD)/isoroot-test/boot/grub/grub.cfg
	@grub-mkrescue -o $@ $(BUILD)/isoroot-test >/dev/null 2>&1 || \
		(echo "  ERROR   grub-mkrescue failed"; exit 1)

# ---- running ----------------------------------------------------------------

QEMU_COMMON := -m 128M -no-reboot -no-shutdown
QEMU_SERIAL := -serial mon:stdio

.PHONY: run
run: $(DISK_IMG)
	$(QEMU) $(QEMU_COMMON) $(QEMU_SERIAL) \
		-drive format=raw,file=$(DISK_IMG),index=0,media=disk

.PHONY: run-iso
run-iso: $(ISO)
	$(QEMU) $(QEMU_COMMON) $(QEMU_SERIAL) -cdrom $(ISO)

# The same 32-bit image on a processor that has x86-64, which is what real
# hardware looks like and the only way to exercise the long-mode transition:
# qemu-system-i386 masks CPUID.80000001H:EDX.LM even with -cpu max, so on it
# this kernel correctly reports that long mode is unavailable. Type
# `longmode` at the shell. See docs/LONGMODE.md.
# With an Ethernet controller attached, for `net`, `ping` and the echo ports.
# QEMU attaches a default e1000 anyway, but naming it explicitly pins the
# device model, and the dump gives a capture to read afterwards - which is how
# the driver's first real bug was found. See docs/NETWORK.md.
QEMU_NET := -netdev user,id=n0 \
            -device e1000,netdev=n0 \
            -object filter-dump,id=d0,netdev=n0,file=$(BUILD)/net.pcap

.PHONY: run-net
run-net: $(SHELL_IMG)
	@echo "  frames will be dumped to $(BUILD)/net.pcap"
	$(QEMU) $(QEMU_COMMON) $(QEMU_SERIAL) $(QEMU_NET) \
		-drive format=raw,file=$(SHELL_IMG),index=0,media=disk

QEMU64 ?= qemu-system-x86_64

.PHONY: run-x86-64
run-x86-64: $(SHELL_IMG)
	@command -v $(QEMU64) >/dev/null 2>&1 || \
		{ echo "  $(QEMU64) is not installed"; exit 1; }
	$(QEMU64) $(QEMU_COMMON) $(QEMU_SERIAL) \
		-drive format=raw,file=$(SHELL_IMG),index=0,media=disk

# `-kernel` makes QEMU act as the Multiboot loader itself, which is the
# fastest way to iterate: no ISO rebuild, no GRUB menu.
.PHONY: run-direct
run-direct: $(KERNEL_ELF)
	$(QEMU) $(QEMU_COMMON) $(QEMU_SERIAL) -kernel $(KERNEL_ELF)

.PHONY: run-serial
run-serial: $(DISK_IMG)
	$(QEMU) $(QEMU_COMMON) -nographic \
		-drive format=raw,file=$(DISK_IMG),index=0,media=disk

# ---- testing ----------------------------------------------------------------

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
	@$(CC) -m32 -std=gnu11 -Wall -Wextra -Werror -g \
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
.PHONY: fuzz-seed
fuzz-seed: $(FS_IMG) $(USER_ELFS)
	@$(PYTHON) $(TOOLS)/fuzz-seed.py --corpus $(FUZZ_CORPUS) \
		--user-elf $(BUILD)/user/init.stripped.elf \
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

.PHONY: test-boot
test-boot: $(TEST_IMG) $(TEST_ISO) $(SHELL_IMG) $(BENCH_IMG)
	@echo "  RUN     QEMU boot tests (both boot paths)"
	@$(PYTHON) $(TOOLS)/run-tests.py --build-dir $(BUILD)

.PHONY: test
test: test-host test-boot
	@echo
	@echo "  all tests passed"

# ---- debugging --------------------------------------------------------------

.PHONY: bench
bench: $(BENCH_IMG)
	@echo "  RUN     microbenchmarks and a profile under QEMU"
	@$(QEMU) $(QEMU_COMMON) -display none -serial stdio \
		-device isa-debug-exit,iobase=0xf4,iosize=0x04 \
		-drive format=raw,file=$(BENCH_IMG),index=0,media=disk || true

.PHONY: debug
debug: $(DISK_IMG)
	@echo "QEMU is stopped and listening on :1234 - run 'make gdb' elsewhere."
	$(QEMU) $(QEMU_COMMON) $(QEMU_SERIAL) -s -S \
		-drive format=raw,file=$(DISK_IMG),index=0,media=disk

.PHONY: gdb
gdb: $(KERNEL_DEBUG)
	gdb -q $(KERNEL_DEBUG) \
		-ex "set architecture i386" \
		-ex "target remote :1234" \
		-ex "break kmain" \
		-ex "layout src"

.PHONY: disasm
disasm: $(KERNEL_DEBUG)
	@$(OBJDUMP) -d -M intel $< | less

.PHONY: sections
sections: $(KERNEL_ELF)
	@$(OBJDUMP) -h $<
	@echo
	@$(PYTHON) $(TOOLS)/check-kernel.py --verbose $(KERNEL_DEBUG) \
		--matches $(KERNEL_ELF)

.PHONY: symbols
symbols: $(KERNEL_DEBUG)
	@nm -n $< | less

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
	@for d in boot kernel/arch kernel/core kernel/mm kernel/drivers \
	          kernel/shell kernel/include user tools tests ; do \
		n=$$(find $$d -type f \( -name '*.c' -o -name '*.h' -o -name '*.asm' \
		     -o -name '*.inc' -o -name '*.py' \) -exec cat {} + 2>/dev/null | wc -l); \
		printf "  %-20s %6s\n" "$$d" "$$n"; \
	done

.PHONY: clean
clean:
	@rm -rf $(BUILD)
	@echo "  cleaned"

.PHONY: toolchain
toolchain:
	@echo "Toolchain in use: $(TOOLCHAIN)"
	@printf "  %-12s " "CC";      $(CC) --version | head -1
	@printf "  %-12s " "LD";      $(LD) --version | head -1
	@printf "  %-12s " "NASM";    $(AS) --version 2>/dev/null | head -1 || echo "MISSING"
	@printf "  %-12s " "QEMU";    $(QEMU) --version 2>/dev/null | head -1 || echo "MISSING (make run will not work)"
	@printf "  %-12s " "GRUB";    grub-mkrescue --version 2>/dev/null | head -1 || echo "MISSING (no ISO will be built)"
	@printf "  %-12s " "Python";  $(PYTHON) --version

-include $(DEPS)
