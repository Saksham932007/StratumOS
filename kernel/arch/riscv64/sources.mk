# =============================================================================
# StratumOS - what the riscv64 build compiles
# =============================================================================
#
# Declarations only. See kernel/arch/x86/sources.mk for why this is separate
# from arch.mk.
# =============================================================================

# The measurement, expressed as a build dependency.
#
# These are the x86 kernel's own source files, compiled unmodified for a
# different architecture and linked into this one. If this list can grow, the
# kernel got more portable; if it shrinks, something regressed. `make
# portability` reports which other files *could* be here and what stops them.
#
# Named files rather than whole directories, because the honest answer is a
# subset: kernel/core also holds the scheduler and the x86 test suite, and
# claiming `core` would overstate the result.
ARCH_SHARED_FILES := $(KSRC)/core/printf.c \
                     $(KSRC)/core/string.c \
                     $(KSRC)/core/div64.c \
                     $(KSRC)/core/log.c

ARCH_C_SOURCES := $(ARCHDIR)/main.c \
                  $(ARCHDIR)/uart.c \
                  $(ARCHDIR)/trap.c \
                  $(ARCHDIR)/timer.c \
                  $(ARCHDIR)/paging.c

# trap_entry.S, not trap.S: it would compile to the same object path as
# trap.c. See the collision guard in the common Makefile.
ARCH_ASM_SOURCES := $(ARCHDIR)/boot.S $(ARCHDIR)/trap_entry.S

ARCH_C_OBJECTS   := $(ARCH_C_SOURCES:%.c=$(OUT)/%.o)
ARCH_ASM_OBJECTS := $(ARCH_ASM_SOURCES:%.S=$(OUT)/%.o)

ARCH_SOURCES := $(ARCH_C_SOURCES) $(ARCH_ASM_SOURCES)
ARCH_OBJECTS := $(ARCH_ASM_OBJECTS) $(ARCH_C_OBJECTS)

