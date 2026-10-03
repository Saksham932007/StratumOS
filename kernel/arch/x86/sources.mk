# =============================================================================
# StratumOS - what the i386 build compiles
# =============================================================================
#
# Declarations only, no rules. The build has a two-phase shape and this is the
# first phase: the common Makefile has to know which shared sources an
# architecture wants *before* it can define the object list that the
# architecture's link rule depends on, and a rule's prerequisites are expanded
# when the rule is read.
#
# Including one file twice to work around that produces "overriding recipe"
# warnings for every rule in it, which is how this split came about. Phase two
# is arch.mk.
# =============================================================================

# Every portable directory. This architecture is the one the kernel was
# written on, so it links all of them - and the difference between this list
# and riscv64's is a measurement rather than a preference. See
# docs/PORTING.md and `make portability`.
ARCH_SHARED_DIRS := core mm drivers fs net shell

ARCH_C_SOURCES   := $(sort $(wildcard $(ARCHDIR)/*.c))
ARCH_ASM_SOURCES := $(sort $(wildcard $(ARCHDIR)/*.asm))

ARCH_C_OBJECTS   := $(ARCH_C_SOURCES:%.c=$(OUT)/%.o)
ARCH_ASM_OBJECTS := $(ARCH_ASM_SOURCES:%.asm=$(OUT)/%.o)

ARCH_SOURCES := $(ARCH_C_SOURCES) $(ARCH_ASM_SOURCES)
ARCH_OBJECTS := $(ARCH_ASM_OBJECTS) $(ARCH_C_OBJECTS)

