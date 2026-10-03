/* StratumOS - RISC-V 64 (rv64imac) on QEMU's `virt` machine.
 *
 * The second architecture. See docs/PORTING.md for what did and did not
 * carry over from x86, which is the point of this port rather than a side
 * effect of it.
 *
 * PRIVILEGE, AND WHY THE KERNEL DOES NOT STAY IN M-MODE
 * ----------------------------------------------------
 * A hart comes out of reset in machine mode, which can do anything - and
 * which cannot use `satp`, because M-mode instruction fetches and data
 * accesses bypass translation entirely. So paging is not something an
 * M-mode kernel can switch on for itself; it has to drop to supervisor mode
 * first.
 *
 * That makes the boot sequence here the same shape as the x86 one and for
 * the same kind of reason: a privilege transition you have to perform
 * because the mode you start in cannot do the thing you need.
 *
 *     x86:    16-bit real mode -> 32-bit protected mode -> paging
 *     RISC-V: machine mode     -> supervisor mode       -> paging (Sv39)
 *
 * The transition is `mret`, with mstatus.MPP set to S and mepc pointing at
 * the supervisor entry - the direct analogue of a far jump that reloads CS.
 *
 * TWO TRAP VECTORS
 * ----------------
 * mtvec and stvec. The timer stays in M-mode, because the CLINT's mtimecmp
 * is an M-mode device and delegating machine timer interrupts is not
 * permitted; everything a supervisor kernel should handle - ecall from S,
 * illegal instruction, page fault - is delegated to stvec through medeleg.
 *
 * So this port has to get *both* right, where the x86 side has one IDT.
 * That asymmetry is a real difference between the architectures and not a
 * shortcoming of either.
 */
#ifndef _ARCH_RISCV64_RISCV_H
#define _ARCH_RISCV64_RISCV_H

#include <kernel/types.h>

/* ---- QEMU `virt` memory map -------------------------------------------- */

#define VIRT_UART0           0x10000000ul /* NS16550A                          */
#define VIRT_CLINT           0x02000000ul /* core-local interruptor            */
#define VIRT_CLINT_MSIP      0x02000000ul
#define VIRT_CLINT_MTIMECMP  (VIRT_CLINT + 0x4000ul) /* + 8 per hart       */
#define VIRT_CLINT_MTIME     (VIRT_CLINT + 0xBFF8ul)
#define VIRT_TEST            0x00100000ul /* SiFive test device: exit the VM   */
#define VIRT_RAM_BASE        0x80000000ul

/* The CLINT runs at a fixed 10 MHz on this machine, which is what makes a
 * tick interval expressible without calibrating against anything. On real
 * hardware the frequency comes from the device tree. */
#define VIRT_TIMEBASE_HZ     10000000ul

/* What the test device wants written to it. The low half-word is the
 * command; the high half-word of FINISHER_PASS/FAIL carries an exit code,
 * which is how a CI run gets a status out of the emulator - the same job
 * `isa-debug-exit` does on x86. */
#define TEST_FINISHER_PASS   0x5555u
#define TEST_FINISHER_FAIL   0x3333u

/* ---- mstatus / sstatus ------------------------------------------------- */

#define MSTATUS_MIE          (1ul << 3)
#define MSTATUS_SIE          (1ul << 1)
#define MSTATUS_MPIE         (1ul << 7)
#define MSTATUS_SPIE         (1ul << 5)
#define MSTATUS_MPP_MASK     (3ul << 11)
#define MSTATUS_MPP_S        (1ul << 11)
#define MSTATUS_MPP_M        (3ul << 11)
#define MSTATUS_SUM          (1ul << 18) /* supervisor may access user pages      */

/* ---- interrupt enables ------------------------------------------------- */

#define MIE_MTIE             (1ul << 7) /* machine timer                             */
#define MIE_MSIE             (1ul << 3) /* machine software                          */
#define SIE_STIE             (1ul << 5)
#define SIE_SEIE             (1ul << 9)

/* ---- trap causes ------------------------------------------------------- */

#define CAUSE_INTERRUPT_FLAG (1ul << 63)

#define EXC_INST_MISALIGNED  0
#define EXC_INST_FAULT       1
#define EXC_ILLEGAL_INST     2
#define EXC_BREAKPOINT       3
#define EXC_LOAD_MISALIGNED  4
#define EXC_LOAD_FAULT       5
#define EXC_STORE_MISALIGNED 6
#define EXC_STORE_FAULT      7
#define EXC_ECALL_U          8
#define EXC_ECALL_S          9
#define EXC_ECALL_M          11
#define EXC_INST_PAGE_FAULT  12
#define EXC_LOAD_PAGE_FAULT  13
#define EXC_STORE_PAGE_FAULT 15

#define IRQ_S_TIMER          5
#define IRQ_M_TIMER          7

/* ---- Sv39 ------------------------------------------------------------- */

/* Three levels of 512 entries, 4 KiB pages, and a 39-bit virtual address
 * split 9 + 9 + 9 + 12. An entry at the root maps a 1 GiB "gigapage", one at
 * the middle level a 2 MiB "megapage", one at the leaf a 4 KiB page - the
 * same early-termination idea as x86's PSE and PDPT large pages.
 *
 * The part with no x86 counterpart: bits 63:39 of a virtual address must all
 * equal bit 38, so the address space has a hole in the middle rather than
 * being flat. x86-64 has the same rule; 32-bit x86 does not, and the kernel
 * being ported from had never needed to express it. */
#define SV39_LEVELS          3
#define SV39_ENTRIES         512
#define SV39_PAGE_SIZE       4096ul
#define SV39_GIGAPAGE        (1ul << 30)
#define SV39_MEGAPAGE        (1ul << 21)

#define PTE_V                (1ul << 0) /* valid                                          */
#define PTE_R                (1ul << 1)
#define PTE_W                (1ul << 2)
#define PTE_X                (1ul << 3)
#define PTE_U                (1ul << 4) /* accessible from user mode                      */
#define PTE_G                (1ul << 5) /* global                                         */
#define PTE_A                (1ul << 6) /* accessed                                       */
#define PTE_D                (1ul << 7) /* dirty                                          */

/* The physical page number lives in bits 53:10, which is why a PTE is not
 * simply "the address with flags in the low bits" the way an x86 one is: the
 * address is shifted *left* by 10 after being shifted right by 12. Getting
 * that wrong produces a page table that faults on everything, which is at
 * least loud. */
#define PTE_PPN_SHIFT        10
#define pte_from_phys(pa, flags) \
    ((((u64)(pa) >> 12) << PTE_PPN_SHIFT) | (flags))
#define pte_to_phys(pte)    (((u64)(pte) >> PTE_PPN_SHIFT) << 12)

#define SATP_MODE_SV39      (8ul << 60)
#define satp_from_root(pa)  (SATP_MODE_SV39 | ((u64)(pa) >> 12))

#define SV39_VPN(va, level) (((u64)(va) >> (12 + 9 * (level))) & 0x1FFul)

/* Is this a canonical Sv39 address? See the note above. */
static inline bool sv39_canonical(u64 va)
{
    u64 top = va >> 39;

    return top == 0 || top == 0x1FFFFFFul;
}

/* ---- CSR access -------------------------------------------------------- */

#define csr_read(name)                                  \
    ({                                                  \
        u64 _v;                                         \
        __asm__ volatile("csrr %0, " #name : "=r"(_v)); \
        _v;                                             \
    })

#define csr_write(name, v)                                \
    do {                                                  \
        u64 _x = (v);                                     \
        __asm__ volatile("csrw " #name ", %0" ::"r"(_x)); \
    } while (0)

#define csr_set(name, v)                                  \
    do {                                                  \
        u64 _x = (v);                                     \
        __asm__ volatile("csrs " #name ", %0" ::"r"(_x)); \
    } while (0)

#define csr_clear(name, v)                                \
    do {                                                  \
        u64 _x = (v);                                     \
        __asm__ volatile("csrc " #name ", %0" ::"r"(_x)); \
    } while (0)

static inline void sfence_vma(void)
{
    /* The whole TLB. RISC-V's `sfence.vma` with both operands zero is the
     * equivalent of reloading CR3 on x86; with a virtual address in rs1 it
     * is `invlpg`. The fence is also an ordering barrier, which is why it is
     * required after writing a page table and not merely advisable. */
    __asm__ volatile("sfence.vma zero, zero" ::: "memory");
}

static inline void wfi(void)
{
    __asm__ volatile("wfi");
}

/* MMIO, which on this architecture is just a volatile access - there is no
 * separate I/O space and no `in`/`out` instruction, so the x86 distinction
 * between port I/O and memory-mapped I/O does not exist here at all. */
static inline void mmio_write8(u64 addr, u8 v)
{
    *(volatile u8 *)addr = v;
}

static inline u8 mmio_read8(u64 addr)
{
    return *(volatile u8 *)addr;
}

static inline void mmio_write32(u64 addr, u32 v)
{
    *(volatile u32 *)addr = v;
}

static inline void mmio_write64(u64 addr, u64 v)
{
    *(volatile u64 *)addr = v;
}

static inline u64 mmio_read64(u64 addr)
{
    return *(volatile u64 *)addr;
}

/* ---- the arch layer's interface to the portable code ------------------- */

void uart_init(void);
void console_putc(char c); /* the one symbol kernel/core/printf.c needs */

void trap_init_machine(void);
void trap_init_supervisor(void);
u64 trap_count(u64 cause);
u64 trap_total(void);

void timer_init_machine(void);
u64 timer_ticks(void);
u64 timer_now(void);

bool paging_init(void);
bool paging_enabled(void);
u64 paging_root(void);
bool paging_map_page(u64 va, u64 pa, u64 flags);
bool paging_translate(u64 va, u64 *pa_out);

NORETURN void machine_exit(bool pass, u16 code);

#endif /* _ARCH_RISCV64_RISCV_H */
