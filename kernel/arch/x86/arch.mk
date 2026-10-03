# =============================================================================
# StratumOS - the i386 architecture's build
# =============================================================================
#
# Everything the x86 build needs that the common Makefile should not know
# about: the toolchain, the flags, the three-pass symbol-table link, the
# bootloader, the disk images and the GRUB ISO.
#
# It lives beside the code it builds for the same reason kernel/arch/x86/ does:
# adding a third architecture should mean adding a directory, not editing a
# switch statement in the middle of the build system.
#
# THE CONTRACT
#
# An architecture is two files. sources.mk declares what it compiles and is
# read first; arch.mk defines how, and is read once the shared object list
# exists. See sources.mk for why that order is forced rather than chosen.
#
#   sources.mk must define
#     ARCH_SHARED_DIRS or ARCH_SHARED_FILES   which portable sources it links
#     ARCH_C_SOURCES    ARCH_ASM_SOURCES      its own
#     ARCH_C_OBJECTS    ARCH_ASM_OBJECTS      and where those land
#     ARCH_OBJECTS                            all of its own objects
#
#   arch.mk must define
#     CC LD AS OBJCOPY OBJDUMP QEMU TOOLCHAIN the tools, and a name for them
#     CFLAGS                                  for the shared $(OUT)/%.o rule
#     KERNEL_ELF                              the kernel it produces
#     a rule for $(OUT)/%.o from its assembly dialect
#     arch-all  arch-run  arch-test           the three common verbs
#     arch-toolchain                          what `make toolchain` adds
#
# and may define anything else for its own targets - this one adds run-iso,
# run-net, bench, debug, gdb, fs, user and more, none of which the common
# Makefile knows about.
# =============================================================================

# ---- toolchain --------------------------------------------------------------
#
# A cross-compiler (i686-elf-gcc) is used when one is on PATH; otherwise the
# host GCC is driven with -m32, which works on any x86-64 Linux with multilib
# installed. Both are checked for, and the choice is reported, because
# "it built on my machine" is not a build system.

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

AS   := nasm
QEMU := qemu-system-i386

# ---- flags ------------------------------------------------------------------
#
# -ffreestanding            no hosted C library is present
# -nostdlib                 ...and none should be linked
# -fno-pie -fno-pic         the kernel is linked to a fixed address
# -fno-stack-protector      the guard would call into a libc that is not there
# -fno-omit-frame-pointer   keeps the panic backtrace walkable
# -fno-asynchronous-...     no unwind tables; nothing here throws
# -mgeneral-regs-only       forbids SSE/MMX, which the kernel never enables in
#                           CR4 and which the compiler would otherwise use for
#                           struct copies - producing a #UD at boot

CFLAGS := $(ARCHFLAG) -std=gnu11 -O2 -g3 \
          -ffreestanding -nostdlib -fno-builtin \
          -fno-pie -fno-pic -fno-stack-protector \
          -fno-omit-frame-pointer -fno-asynchronous-unwind-tables \
          -mgeneral-regs-only -march=i686 \
          -I$(INCLUDE) $(WARNINGS) -MMD -MP $(CFLAGS_EXTRA)

ASFLAGS := -f elf32 -g -F dwarf -I $(ARCHDIR)/
# --no-warn-rwx-segments: a kernel image is one writable, executable blob by
# design; the warning is aimed at userspace hardening and is pure noise here.
LDFLAGS := -m elf_i386 -T linker/kernel.ld -nostdlib -z noexecstack \
           --build-id=none --no-warn-rwx-segments

# ---- artefacts --------------------------------------------------------------
#
# Two kernels from one link: the bootable image is stripped of debug info so
# that stage 2 has plenty of room to stage it below the EBDA, while the
# unstripped copy keeps full DWARF for GDB.
KERNEL_ELF   := $(OUT)/stratum.elf
KERNEL_DEBUG := $(OUT)/stratum.debug.elf
DISK_IMG     := $(OUT)/stratum.img
ISO          := $(OUT)/stratum.iso
# Separate images whose embedded command line runs the self-test suite and
# then shuts the machine down, so CI needs no keystrokes and no timeout.
TEST_IMG     := $(OUT)/stratum-test.img
TEST_ISO     := $(OUT)/stratum-test.iso
# A quiet image for the interactive shell test: no demo tasks and no ring-3
# payload at boot, so the transcript the harness reads is deterministic.
SHELL_IMG    := $(OUT)/stratum-shell.img
# Benchmarks plus a profile, then shut down. Separate from the self-test image
# so correctness and measurement are asserted independently.
BENCH_IMG    := $(OUT)/stratum-bench.img
STAGE1_BIN   := $(OUT)/stage1.bin
STAGE2_BIN   := $(OUT)/stage2.bin

# ---- the ring-3 programs ----------------------------------------------------
#
# Separate ELFs, linked for user space and embedded in the kernel image as
# blobs. Defined before LINK_ORDER, because that is expanded immediately.
#
# `init` is the program the kernel starts; `hello` exists so that exec() has
# a genuinely different image to replace it with, which is the only way to
# show that exec replaced an address space rather than reloading one; `fuzz`
# attacks the syscall boundary from ring 3, which is the only seat from which
# the kernel's pointer validation can actually be attacked.
USER_PROGS := init hello fuzz
USER_ELFS  := $(USER_PROGS:%=$(OUT)/user/%.elf)
USER_BLOBS := $(USER_PROGS:%=$(OUT)/user/%_blob.o)

# boot.asm must be linked first so that .multiboot lands at the start of the
# image, where the Multiboot2 specification requires the header to be.
BOOT_OBJ   := $(OUT)/$(ARCHDIR)/boot.o
LINK_ORDER := $(BOOT_OBJ) $(filter-out $(BOOT_OBJ),$(ARCH_ASM_OBJECTS)) \
              $(SHARED_OBJECTS) $(ARCH_C_OBJECTS) $(USER_BLOBS)

OBJECTS := $(ARCH_OBJECTS) $(SHARED_OBJECTS) $(USER_BLOBS)

# ---- the three common verbs -------------------------------------------------

# Every image is built by `all`. Building only some of them invites the
# classic confusion of testing a stale artefact and drawing conclusions from it.
.PHONY: arch-all
arch-all: $(DISK_IMG) $(ISO) $(TEST_IMG) $(TEST_ISO) $(SHELL_IMG) $(BENCH_IMG)
	@echo
	@echo "  StratumOS built for $(ARCH) with $(TOOLCHAIN)"
	@printf "  %-22s %s\n" "kernel ELF" "$(KERNEL_ELF)"
	@printf "  %-22s %s\n" "disk image (stage1+2)" "$(DISK_IMG)"
	@printf "  %-22s %s\n" "GRUB ISO (multiboot2)" "$(ISO)"
	@printf "  %-22s %s\n" "self-test images" "$(TEST_IMG), $(TEST_ISO)"
	@echo
	@echo "  make run            boot the custom bootloader"
	@echo "  make run-iso        boot through GRUB"
	@echo "  make test           run every test, headless"
	@echo "  make fuzz           fuzz the parsers (see docs/FUZZING.md)"
	@echo "  make run-net        boot with a network card (see docs/NETWORK.md)"
	@echo "  make ARCH=riscv64   build the other architecture"
	@echo

.PHONY: arch-run
arch-run: run

.PHONY: arch-test
arch-test: test-boot

# ---- assembling -------------------------------------------------------------

$(OUT)/%.o: %.asm
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

KSYMS_EMPTY := $(OUT)/ksyms_empty.c
KSYMS_A     := $(OUT)/ksyms_a.c
KSYMS_B     := $(OUT)/ksyms_b.c

$(KSYMS_EMPTY): $(TOOLS)/gen-ksyms.py
	@mkdir -p $(dir $@)
	@$(PYTHON) $(TOOLS)/gen-ksyms.py --empty -o $@

# Generated sources live in $(OUT), so they need their own rule; the generic
# $(OUT)/%.o: %.c pattern would look for them outside the build tree.
$(OUT)/ksyms_%.o: $(OUT)/ksyms_%.c
	@echo "  CC      $< "
	@$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/pass_a.elf: $(OBJECTS) $(OUT)/ksyms_empty.o linker/kernel.ld
	@mkdir -p $(dir $@)
	@echo "  LD      pass A (enumerate symbols)"
	@$(LD) $(LDFLAGS) $(LINK_ORDER) $(OUT)/ksyms_empty.o -o $@

$(KSYMS_A): $(OUT)/pass_a.elf $(TOOLS)/gen-ksyms.py
	@$(PYTHON) $(TOOLS)/gen-ksyms.py $< -o $@

$(OUT)/pass_b.elf: $(OBJECTS) $(OUT)/ksyms_a.o linker/kernel.ld
	@echo "  LD      pass B (addresses settle)"
	@$(LD) $(LDFLAGS) $(LINK_ORDER) $(OUT)/ksyms_a.o -o $@

$(KSYMS_B): $(OUT)/pass_b.elf $(TOOLS)/gen-ksyms.py
	@$(PYTHON) $(TOOLS)/gen-ksyms.py $< -o $@

$(KERNEL_DEBUG): $(OBJECTS) $(OUT)/ksyms_b.o linker/kernel.ld
	@mkdir -p $(dir $@)
	@echo "  LD      $@ (pass C, final)"
	@$(LD) $(LDFLAGS) $(LINK_ORDER) $(OUT)/ksyms_b.o -o $@
	@echo "  VERIFY  embedded symbol table describes this kernel"
	@$(PYTHON) $(TOOLS)/gen-ksyms.py $@ -o $(OUT)/ksyms_check.c \
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

# ---- the ring-3 programs ----------------------------------------------------
#
# Built as genuinely separate programs: their own ELF, linked at a *user*
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

$(OUT)/user/%.o: $(USERSRC)/%.c $(USERSRC)/syscall.h
	@mkdir -p $(dir $@)
	@echo "  CC      $< (ring 3)"
	@$(CC) $(USER_CFLAGS) -c $< -o $@

$(OUT)/user/%.elf: $(OUT)/user/%.o $(USERSRC)/user.ld
	@echo "  LD      $@ (ring 3)"
	@$(LD) -m elf_i386 -T $(USERSRC)/user.ld -nostdlib \
		--no-warn-rwx-segments --build-id=none $< -o $@

# Stripped before embedding: the debug info is three times the size of the
# program, and it would be carried inside the kernel image for no benefit.
$(OUT)/user/%.stripped.elf: $(OUT)/user/%.elf
	@$(OBJCOPY) --strip-all $< $@

# One section name per program, so two blobs do not collide in the link.
$(OUT)/user/%_blob.o: $(OUT)/user/%.stripped.elf
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
.PRECIOUS: $(OUT)/user/%.o $(OUT)/user/%.elf $(OUT)/user/%.stripped.elf

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

FSROOT := $(OUT)/fsroot
FS_IMG := $(OUT)/stratum-fs.img
FS_KIB := 16384

$(FS_IMG): $(USER_PROGS:%=$(OUT)/user/%.stripped.elf) $(TOOLS)/mkfat.py
	@rm -rf $(FSROOT)
	@mkdir -p $(FSROOT)/bin $(FSROOT)/etc
	@for p in $(USER_PROGS); do \
		cp $(OUT)/user/$$p.stripped.elf $(FSROOT)/bin/$$(echo $$p | tr a-z A-Z); \
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
	@rm -rf $(OUT)/isoroot
	@mkdir -p $(OUT)/isoroot/boot/grub
	@cp $(KERNEL_ELF) $(OUT)/isoroot/boot/stratum.elf
	@cp $(TOOLS)/grub.cfg $(OUT)/isoroot/boot/grub/grub.cfg
	@grub-mkrescue -o $@ $(OUT)/isoroot >/dev/null 2>&1 || \
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
	@rm -rf $(OUT)/isoroot-test
	@mkdir -p $(OUT)/isoroot-test/boot/grub
	@cp $(KERNEL_ELF) $(OUT)/isoroot-test/boot/stratum.elf
	@cp $(TOOLS)/grub-test.cfg $(OUT)/isoroot-test/boot/grub/grub.cfg
	@grub-mkrescue -o $@ $(OUT)/isoroot-test >/dev/null 2>&1 || \
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

# With an Ethernet controller attached, for `net`, `ping` and the echo ports.
# QEMU attaches a default e1000 anyway, but naming it explicitly pins the
# device model, and the dump gives a capture to read afterwards - which is how
# the driver's first real bug was found. See docs/NETWORK.md.
QEMU_NET := -netdev user,id=n0 \
            -device e1000,netdev=n0 \
            -object filter-dump,id=d0,netdev=n0,file=$(OUT)/net.pcap

.PHONY: run-net
run-net: $(SHELL_IMG)
	@echo "  frames will be dumped to $(OUT)/net.pcap"
	$(QEMU) $(QEMU_COMMON) $(QEMU_SERIAL) $(QEMU_NET) \
		-drive format=raw,file=$(SHELL_IMG),index=0,media=disk

# The same 32-bit image on a processor that has x86-64, which is what real
# hardware looks like and the only way to exercise the long-mode transition:
# qemu-system-i386 masks CPUID.80000001H:EDX.LM even with -cpu max, so on it
# this kernel correctly reports that long mode is unavailable. Type
# `longmode` at the shell. See docs/LONGMODE.md.
QEMU64 ?= qemu-system-x86_64

.PHONY: run-x86-64
run-x86-64: $(SHELL_IMG)
	@command -v $(QEMU64) >/dev/null 2>&1 || \
		{ echo "  $(QEMU64) is not installed"; exit 1; }
	$(QEMU64) $(QEMU_COMMON) $(QEMU_SERIAL) \
		-drive format=raw,file=$(SHELL_IMG),index=0,media=disk

# `-kernel` makes QEMU act as the Multiboot loader itself, which is the
# fastest way to iterate on the kernel without rebuilding an image.
.PHONY: run-direct
run-direct: $(KERNEL_ELF)
	$(QEMU) $(QEMU_COMMON) $(QEMU_SERIAL) -kernel $(KERNEL_ELF)

.PHONY: run-serial
run-serial: $(DISK_IMG)
	$(QEMU) $(QEMU_COMMON) -display none -serial stdio \
		-drive format=raw,file=$(DISK_IMG),index=0,media=disk

# ---- testing ----------------------------------------------------------------

.PHONY: test-boot
test-boot: $(TEST_IMG) $(TEST_ISO) $(SHELL_IMG) $(BENCH_IMG)
	@echo "  RUN     QEMU boot tests (both boot paths)"
	@$(PYTHON) $(TOOLS)/run-tests.py --build-dir $(OUT)

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

.PHONY: arch-toolchain
arch-toolchain:
	@printf "  %-12s " "NASM";    $(AS) --version 2>/dev/null | head -1 || echo "MISSING"
	@printf "  %-12s " "QEMU";    $(QEMU) --version 2>/dev/null | head -1 || echo "MISSING (make run will not work)"
	@printf "  %-12s " "GRUB";    grub-mkrescue --version 2>/dev/null | head -1 || echo "MISSING (no ISO will be built)"
