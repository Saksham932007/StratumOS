# =============================================================================
# StratumOS - build system
# =============================================================================
#
#   make              build the kernel, the bootable disk image and the ISO
#   make run          boot the custom bootloader image in QEMU
#   make run-iso      boot via GRUB/Multiboot2 in QEMU
#   make test         host unit tests + both boot paths under QEMU, headless
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
          -I$(INCLUDE) $(WARNINGS) -MMD -MP

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
                    $(wildcard $(KSRC)/shell/*.c))

ASM_SOURCES := $(sort $(wildcard $(KSRC)/arch/x86/*.asm))

C_OBJECTS   := $(C_SOURCES:%.c=$(BUILD)/%.o)
ASM_OBJECTS := $(ASM_SOURCES:%.asm=$(BUILD)/%.o)

# boot.asm must be linked first so that .multiboot lands at the start of the
# image, where the Multiboot2 specification requires the header to be.
BOOT_OBJ    := $(BUILD)/$(KSRC)/arch/x86/boot.o
LINK_ORDER  := $(BOOT_OBJ) $(filter-out $(BOOT_OBJ),$(ASM_OBJECTS)) $(C_OBJECTS)

OBJECTS := $(ASM_OBJECTS) $(C_OBJECTS)
DEPS    := $(C_OBJECTS:.o=.d)

# ---- top-level targets ------------------------------------------------------

# Every image is built by `all`. Building only some of them invites the
# classic confusion of testing a stale artefact and drawing conclusions from it.
.PHONY: all
all: $(DISK_IMG) $(ISO) $(TEST_IMG) $(TEST_ISO) $(SHELL_IMG)
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

$(KERNEL_DEBUG): $(OBJECTS) linker/kernel.ld
	@mkdir -p $(dir $@)
	@echo "  LD      $@"
	@$(LD) $(LDFLAGS) $(LINK_ORDER) -o $@

$(KERNEL_ELF): $(KERNEL_DEBUG) $(TOOLS)/check-kernel.py
	@echo "  STRIP   $@"
	@$(OBJCOPY) --strip-debug --strip-unneeded $< $@
	@echo "  CHECK   multiboot2 header, ELF type, section layout"
	@$(PYTHON) $(TOOLS)/check-kernel.py $@
	@printf "  SIZE    %s bytes bootable, %s bytes with debug info\n" \
		"$$(stat -c%s $@)" "$$(stat -c%s $<)"

# ---- bootloader -------------------------------------------------------------

$(STAGE1_BIN): $(BOOTSRC)/stage1.asm
	@mkdir -p $(dir $@)
	@echo "  AS      $< (flat binary)"
	@$(AS) -f bin -I $(BOOTSRC)/ $< -o $@

$(STAGE2_BIN): $(BOOTSRC)/stage2.asm
	@mkdir -p $(dir $@)
	@echo "  AS      $< (flat binary)"
	@$(AS) -f bin -I $(BOOTSRC)/ $< -o $@

# ---- images -----------------------------------------------------------------

$(DISK_IMG): $(STAGE1_BIN) $(STAGE2_BIN) $(KERNEL_ELF) $(TOOLS)/mkimage.py
	@echo "  IMAGE   $@"
	@$(PYTHON) $(TOOLS)/mkimage.py \
		--stage1 $(STAGE1_BIN) --stage2 $(STAGE2_BIN) \
		--kernel $(KERNEL_ELF) --output $@

$(TEST_IMG): $(STAGE1_BIN) $(STAGE2_BIN) $(KERNEL_ELF) $(TOOLS)/mkimage.py
	@echo "  IMAGE   $@ (self-test)"
	@$(PYTHON) $(TOOLS)/mkimage.py --quiet \
		--stage1 $(STAGE1_BIN) --stage2 $(STAGE2_BIN) \
		--kernel $(KERNEL_ELF) --output $@ --cmdline "autotest"

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

$(SHELL_IMG): $(STAGE1_BIN) $(STAGE2_BIN) $(KERNEL_ELF) $(TOOLS)/mkimage.py
	@echo "  IMAGE   $@ (interactive)"
	@$(PYTHON) $(TOOLS)/mkimage.py --quiet \
		--stage1 $(STAGE1_BIN) --stage2 $(STAGE2_BIN) \
		--kernel $(KERNEL_ELF) --output $@ \
		--cmdline "nodemo nousermode loglevel=warn"

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

.PHONY: test-boot
test-boot: $(TEST_IMG) $(TEST_ISO) $(SHELL_IMG)
	@echo "  RUN     QEMU boot tests (both boot paths)"
	@$(PYTHON) $(TOOLS)/run-tests.py --build-dir $(BUILD)

.PHONY: test
test: test-host test-boot
	@echo
	@echo "  all tests passed"

# ---- debugging --------------------------------------------------------------

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
	@$(PYTHON) $(TOOLS)/check-kernel.py --verbose $<

.PHONY: symbols
symbols: $(KERNEL_DEBUG)
	@nm -n $< | less

# ---- housekeeping -----------------------------------------------------------

.PHONY: format
format:
	@command -v clang-format >/dev/null 2>&1 || \
		{ echo "clang-format is not installed"; exit 1; }
	@find $(KSRC) tests -name '*.c' -o -name '*.h' | xargs clang-format -i
	@echo "  formatted $$(find $(KSRC) tests -name '*.c' -o -name '*.h' | wc -l) files"

.PHONY: format-check
format-check:
	@command -v clang-format >/dev/null 2>&1 || \
		{ echo "clang-format is not installed; skipping"; exit 0; }
	@find $(KSRC) tests -name '*.c' -o -name '*.h' | \
		xargs clang-format --dry-run --Werror

.PHONY: lines
lines:
	@echo "Lines of code by area:"
	@for d in boot kernel/arch kernel/core kernel/mm kernel/drivers \
	          kernel/shell kernel/include tools tests; do \
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
