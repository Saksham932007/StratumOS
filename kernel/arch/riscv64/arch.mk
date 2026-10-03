# =============================================================================
# StratumOS - the riscv64 architecture's build
# =============================================================================
#
# See kernel/arch/x86/arch.mk for the contract this has to satisfy, and
# docs/PORTING.md for what the port found.
#
# clang rather than gcc, because it is a cross compiler for every target it
# supports without a separate toolchain per architecture - which is why this
# port needed no new compiler installed at all. ld.lld links it.
#
# The asymmetry with the x86 side is honest and worth noticing: that file is
# 500 lines because i386 needs a two-stage bootloader, a FAT16 image, a GRUB
# ISO, embedded ring-3 programs and a three-pass link to embed a symbol
# table. This one is 120 because a riscv64 kernel is an ELF the loader places
# in RAM. The difference is the platform, not the effort.
# =============================================================================

CC      := clang
LD      := ld.lld
OBJCOPY := llvm-objcopy
OBJDUMP := llvm-objdump
QEMU    := qemu-system-riscv64
AS      := $(CC)

TOOLCHAIN := clang --target=riscv64-unknown-elf + ld.lld

# ---- flags ------------------------------------------------------------------
#
# rv64imac: integer, multiply, atomics, compressed. No floating point, because
# a kernel that does not use it should not have to save it on a trap - the
# same reason the x86 build passes -mgeneral-regs-only.
#
# -mcmodel=medany makes every reference PC-relative within +/-2 GiB, which is
# what lets the kernel be linked at 0x80000000 without relocations. The
# default (medlow) assumes everything sits in the low 2 GiB and produces
# relocations the linker cannot resolve for an address this high.

RVTARGET := --target=riscv64-unknown-elf -march=rv64imac -mabi=lp64 \
            -mcmodel=medany

CFLAGS := $(RVTARGET) -std=gnu11 -O2 -g \
          -ffreestanding -fno-builtin -fno-stack-protector -fno-pic \
          -fno-omit-frame-pointer \
          -I$(INCLUDE) -Wall -Wextra -Werror $(CFLAGS_EXTRA)

ASFLAGS := $(RVTARGET) -ffreestanding

LDFLAGS := -T $(TOOLS)/link-riscv64.ld

# ---- artefacts --------------------------------------------------------------

KERNEL_ELF   := $(OUT)/stratum.elf
KERNEL_DEBUG := $(KERNEL_ELF)
KERNEL_BIN   := $(OUT)/stratum.bin

OBJECTS := $(ARCH_OBJECTS) $(SHARED_OBJECTS)

# ---- assembling -------------------------------------------------------------

$(OUT)/%.o: %.S
	@mkdir -p $(dir $@)
	@echo "  AS      $<"
	@$(AS) $(ASFLAGS) -c $< -o $@

# ---- linking ----------------------------------------------------------------

$(KERNEL_ELF): $(OBJECTS) $(TOOLS)/link-riscv64.ld
	@mkdir -p $(dir $@)
	@echo "  LD      $@"
	@$(LD) $(LDFLAGS) $(OBJECTS) -o $@
	@$(OBJCOPY) -O binary $@ $(KERNEL_BIN)
	@printf "  SIZE    %s bytes loadable, %s bytes with symbols\n" \
		"$$(stat -c%s $(KERNEL_BIN))" "$$(stat -c%s $@)"

# ---- the three common verbs -------------------------------------------------

.PHONY: arch-all
arch-all: $(KERNEL_ELF)
	@echo
	@echo "  StratumOS built for $(ARCH) with $(TOOLCHAIN)"
	@printf "  %-22s %s\n" "kernel ELF" "$(KERNEL_ELF)"
	@echo
	@echo "  make ARCH=riscv64 run    boot it, no firmware, machine mode"
	@echo "  make ARCH=riscv64 test   58 checks, unattended"
	@echo "  make portability         what else could be linked here"
	@echo

# -bios none: no OpenSBI underneath, so the kernel is started directly in
# machine mode and owns the whole machine. -machine virt is QEMU's generic
# RISC-V board: a 16550 UART, a CLINT, a PLIC and virtio devices.
QEMU_ARGS := -machine virt -bios none -nographic -smp 1 -m 128M

.PHONY: arch-run
arch-run: run

.PHONY: run
run: $(KERNEL_ELF)
	$(QEMU) $(QEMU_ARGS) -kernel $(KERNEL_ELF)

.PHONY: arch-test
arch-test: test-boot

.PHONY: test-boot
test-boot: $(KERNEL_ELF)
	@echo "  RUN     riscv64 under $(QEMU)"
	@$(PYTHON) $(TOOLS)/run-riscv64.py --elf $(KERNEL_ELF)

.PHONY: disasm
disasm: $(KERNEL_ELF)
	@$(OBJDUMP) -d $< | less

.PHONY: sections
sections: $(KERNEL_ELF)
	@$(OBJDUMP) -h $<

.PHONY: symbols
symbols: $(KERNEL_ELF)
	@llvm-nm -n $< | less

.PHONY: arch-toolchain
arch-toolchain:
	@printf "  %-12s " "QEMU";    $(QEMU) --version 2>/dev/null | head -1 || echo "MISSING (make run will not work)"
