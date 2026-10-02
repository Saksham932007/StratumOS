/* StratumOS - interactive shell.
 *
 * Reads from the keyboard ring buffer, which the serial driver also feeds, so
 * the same shell drives a VGA console and a remote serial terminal without
 * knowing the difference.
 *
 * Line editing is done by redrawing: on every change the line is reprinted
 * from the prompt and the cursor walked back with backspaces. That is far
 * fewer moving parts than tracking terminal cursor positions, and it behaves
 * identically on the VGA text buffer and over a serial link - where ANSI
 * cursor codes would be the only alternative.
 */
#define LOG_TAG "shell"

#include <arch/cpu.h>
#include <arch/io.h>
#include <arch/irq.h>

#include <drivers/keyboard.h>
#include <drivers/pci.h>
#include <drivers/rtc.h>
#include <drivers/serial.h>
#include <drivers/timer.h>
#include <drivers/vga.h>

#include <kernel/bench.h>
#include <kernel/console.h>
#include <kernel/kernel.h>
#include <kernel/ksyms.h>
#include <kernel/ktest.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/printf.h>
#include <kernel/profile.h>
#include <kernel/sched.h>
#include <kernel/shell.h>
#include <kernel/string.h>
#include <kernel/syscall.h>
#include <kernel/usermode.h>

#include <mm/heap.h>
#include <mm/pmm.h>
#include <mm/vmm.h>

#define PROMPT "stratum> "

static char line[SHELL_LINE_MAX];
static size_t line_len;
static size_t cursor;

static char history[SHELL_HISTORY][SHELL_LINE_MAX];
static size_t history_count;
static size_t history_pos; /* 0 = editing a fresh line */

static const struct shell_command commands[];

/* ------------------------------------------------------------------------- */

static void print_bytes(u64 bytes)
{
    if (bytes >= MIB)
        kprintf("%llu MiB", bytes / MIB);
    else if (bytes >= KIB)
        kprintf("%llu KiB", bytes / KIB);
    else
        kprintf("%llu B", bytes);
}

/* ---- commands ---------------------------------------------------------- */

static int cmd_help(int argc, char **argv)
{
    if (argc > 1) {
        for (size_t i = 0; commands[i].name; i++) {
            if (strcmp(commands[i].name, argv[1]) == 0) {
                kprintf("%s - %s\nusage: %s\n", commands[i].name,
                        commands[i].help, commands[i].usage);
                return 0;
            }
        }
        kprintf("no such command: %s\n", argv[1]);
        return 1;
    }

    kprintf("Commands (use 'help <name>' for details):\n");
    for (size_t i = 0; commands[i].name; i++)
        kprintf("  %-10s %s\n", commands[i].name, commands[i].help);
    return 0;
}

static int cmd_clear(int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);
    vga_clear();
    return 0;
}

static int cmd_echo(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
        kprintf("%s%s", argv[i], i + 1 < argc ? " " : "");
    kprintf("\n");
    return 0;
}

static int cmd_version(int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);

    const struct boot_params *bp = kernel_boot_params();

    kprintf("%s %s\n", STRATUM_NAME, STRATUM_VERSION);
    kprintf("  built     : " __DATE__ " " __TIME__ " with GCC " __VERSION__
            "\n");
    kprintf("  boot via  : %s (%s)\n", bp ? bp->protocol_name : "?",
            bp ? bp->loader_name : "?");
    kprintf("  cmdline   : %s\n",
            (bp && bp->cmdline && *bp->cmdline) ? bp->cmdline : "(none)");
    kprintf("  image     : %p - %p (", (void *)__kernel_start,
            (void *)__kernel_end);
    print_bytes((u32)__kernel_end - (u32)__kernel_start);
    kprintf(")\n");
    return 0;
}

static int cmd_uptime(int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);

    u64 ms = timer_ms();
    u32 total = (u32)(ms / 1000);

    kprintf("up %u:%02u:%02u.%03u (%llu timer ticks at %u.%03u Hz)\n",
            total / 3600, (total / 60) % 60, total % 60, (u32)(ms % 1000),
            timer_ticks(), timer_actual_hz_milli() / 1000,
            timer_actual_hz_milli() % 1000);
    return 0;
}

static int cmd_date(int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);

    struct rtc_time t;
    rtc_read(&t);

    kprintf("%04u-%02u-%02u %02u:%02u:%02u UTC  (unix %llu)\n", t.year, t.month,
            t.day, t.hour, t.minute, t.second, rtc_unix_time());
    return 0;
}

static int cmd_cpuinfo(int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);
    cpu_print_info();
    return 0;
}

static int cmd_meminfo(int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);

    struct pmm_stats pm;
    struct vmm_stats vm;
    struct heap_stats hs;
    const struct boot_params *bp = kernel_boot_params();

    pmm_get_stats(&pm);
    vmm_get_stats(&vm);
    heap_get_stats(&hs);

    kprintf("Physical memory\n");
    kprintf("  total     : ");
    print_bytes((u64)pm.total_frames * PAGE_SIZE);
    kprintf(" (%u frames of %u B)\n", pm.total_frames, (unsigned)PAGE_SIZE);
    kprintf("  in use    : ");
    print_bytes((u64)pm.used_frames * PAGE_SIZE);
    kprintf(" (%u frames)\n", pm.used_frames);
    kprintf("  free      : ");
    print_bytes((u64)pm.free_frames * PAGE_SIZE);
    kprintf(" (%u frames)\n", pm.free_frames);
    kprintf("  allocator : %u allocs, %u frees\n", pm.alloc_calls,
            pm.free_calls);

    kprintf("Virtual memory\n");
    kprintf("  paging    : %s\n", vmm_is_enabled() ? "enabled" : "disabled");
    kprintf("  identity  : 0x00000000 - %p\n", (void *)VMM_IDENTITY_SIZE);
    kprintf("  tables    : %u page tables, %u pages mapped\n", vm.page_tables,
            vm.mapped_pages);
    kprintf("  faults    : %u\n", vm.page_faults);

    kprintf("Kernel heap\n");
    kprintf("  window    : %p (+", (void *)KHEAP_BASE);
    print_bytes(hs.region_bytes);
    kprintf(" committed, max ");
    print_bytes(KHEAP_MAX_SIZE);
    kprintf(")\n");
    kprintf("  in use    : ");
    print_bytes(hs.used_bytes);
    kprintf(" across %u blocks (%u free)\n", hs.block_count, hs.free_blocks);
    kprintf("  largest   : ");
    print_bytes(hs.largest_free);
    kprintf(" contiguous free\n");
    kprintf("  integrity : %s\n",
            heap_check() == 0 ? "consistent" : "PROBLEMS FOUND");

    if (bp) {
        kprintf("Firmware memory map (%u regions)\n", bp->region_count);
        for (u32 i = 0; i < bp->region_count; i++) {
            const struct mem_region *r = &bp->regions[i];
            kprintf("  [%2u] %08llx - %08llx  %-18s ", i, r->base,
                    r->base + r->length - 1, mem_type_name(r->type));
            print_bytes(r->length);
            kprintf("\n");
        }
    }
    return 0;
}

static void ps_row(const struct task *t, void *ctx)
{
    UNUSED(ctx);

    kprintf("  %4u  %-14s %-9s %8u %7u  %p\n", t->pid, t->name,
            task_state_name(t->state), t->ticks_total, t->switches,
            t->stack_base);
}

static int cmd_ps(int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);

    kprintf("  %4s  %-14s %-9s %8s %7s  %s\n", "PID", "NAME", "STATE", "TICKS",
            "SWITCH", "STACK");
    sched_foreach(ps_row, NULL);
    kprintf("  %u context switches total\n", sched_switch_count());
    return 0;
}

static int cmd_irq(int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);

    kprintf("  %3s  %-16s %10s\n", "IRQ", "HANDLER", "COUNT");
    for (unsigned i = 0; i < IRQ_COUNT; i++) {
        u32 n = irq_count(i);
        if (n == 0 && strcmp(irq_name(i), "(none)") == 0)
            continue;
        kprintf("  %3u  %-16s %10u\n", i, irq_name(i), n);
    }
    kprintf("  spurious: %u    syscalls: %u\n", irq_spurious_count(),
            syscall_count());
    return 0;
}

static int cmd_pci(int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);

    u32 n = pci_device_count();

    if (n == 0) {
        kprintf("no PCI devices (or the bus was not scanned)\n");
        return 0;
    }

    kprintf("  %-8s %-9s %-24s %s\n", "ADDRESS", "ID", "VENDOR", "CLASS");
    for (u32 i = 0; i < n; i++) {
        const struct pci_device *d = pci_device_at(i);
        if (!d)
            continue;
        kprintf("  %02x:%02x.%u %04x:%04x %-24s %s\n", d->bus, d->slot, d->func,
                d->vendor_id, d->device_id, pci_vendor_name(d->vendor_id),
                pci_class_name(d->class_code, d->subclass));
    }
    return 0;
}

static int cmd_pagemap(int argc, char **argv)
{
    u32 addr;

    if (argc < 2) {
        kprintf("usage: pagemap <address>\n");
        return 1;
    }

    if (!str_to_u32(argv[1], &addr)) {
        kprintf("'%s' is not a valid address\n", argv[1]);
        return 1;
    }

    u32 pte = vmm_pte(addr);

    kprintf("virtual   : %p\n", (void *)addr);
    kprintf("dir index : %u    table index: %u\n", PDE_INDEX(addr),
            PTE_INDEX(addr));

    if (!(pte & PTE_PRESENT)) {
        kprintf("mapping   : not present\n");
        return 0;
    }

    paddr_t phys = 0;
    vmm_translate(addr, &phys);

    kprintf("physical  : %p\n", (void *)phys);
    kprintf("pte       : %08x\n", pte);
    kprintf("flags     :%s%s%s%s%s%s\n", (pte & PTE_PRESENT) ? " present" : "",
            (pte & PTE_WRITE) ? " write" : " read-only",
            (pte & PTE_USER) ? " user" : " supervisor",
            (pte & PTE_ACCESSED) ? " accessed" : "",
            (pte & PTE_DIRTY) ? " dirty" : "",
            (pte & PTE_OWNED) ? " kernel-owned" : "");
    return 0;
}

static int cmd_hexdump(int argc, char **argv)
{
    u32 addr, count = 64;

    if (argc < 2) {
        kprintf("usage: hexdump <address> [bytes]\n");
        return 1;
    }
    if (!str_to_u32(argv[1], &addr)) {
        kprintf("'%s' is not a valid address\n", argv[1]);
        return 1;
    }
    if (argc > 2 && !str_to_u32(argv[2], &count)) {
        kprintf("'%s' is not a valid length\n", argv[2]);
        return 1;
    }
    if (count > 1024)
        count = 1024;

    /* Refuse to read from an unmapped page: the whole point of having page
     * tables is to be able to check instead of faulting. */
    for (u32 off = 0; off < count; off += PAGE_SIZE) {
        if (!vmm_translate(PAGE_TRUNC(addr + off), NULL)) {
            kprintf("%p is not mapped - refusing to read it\n",
                    (void *)(addr + off));
            return 1;
        }
    }

    const u8 *p = (const u8 *)addr;

    for (u32 i = 0; i < count; i += 16) {
        kprintf("%08x  ", addr + i);

        for (u32 j = 0; j < 16; j++) {
            if (i + j < count)
                kprintf("%02x ", p[i + j]);
            else
                kprintf("   ");
            if (j == 7)
                kprintf(" ");
        }

        kprintf(" |");
        for (u32 j = 0; j < 16 && i + j < count; j++) {
            u8 c = p[i + j];
            kprintf("%c", (c >= 0x20 && c < 0x7F) ? (char)c : '.');
        }
        kprintf("|\n");
    }
    return 0;
}

static int cmd_log(int argc, char **argv)
{
    if (argc < 2) {
        kprintf("log level is %s\n", log_level_name(log_get_level()));
        kprintf("usage: log <panic|error|warn|info|debug|trace>\n");
        return 0;
    }

    if (!log_set_level_by_name(argv[1])) {
        kprintf("unknown level '%s'\n", argv[1]);
        return 1;
    }

    kprintf("log level set to %s\n", log_level_name(log_get_level()));
    return 0;
}

static int cmd_selftest(int argc, char **argv)
{
    unsigned failed;

    if (argc > 1) {
        if (strcmp(argv[1], "list") == 0) {
            ktest_list();
            return 0;
        }
        failed = ktest_run_one(argv[1]);
    } else {
        failed = ktest_run_all();
    }

    return failed == 0 ? 0 : 1;
}

static int cmd_ring3(int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);

    if (usermode_ran()) {
        kprintf("the ring-3 demo has already run this boot\n");
        return 0;
    }

    if (!usermode_spawn_demo()) {
        kprintf("could not start the ring-3 demo\n");
        return 1;
    }

    kprintf("ring-3 demo started; its output follows\n");
    task_sleep_ms(400);
    return 0;
}

/* Addresses are fetched through volatile variables so that the optimiser
 * cannot see what they are. GCC is entitled to compile a provably-NULL store
 * into `ud2` - which it does at -O2 - and the resulting #UD, while handled
 * correctly, is not the page fault this command is supposed to demonstrate. */
static volatile u32 fault_null_addr = 0;
static volatile u32 fault_unmapped_addr = 0xC9000000u;
static volatile u32 fault_readonly_addr;

/* Both operands live in volatile storage. With a literal numerator GCC proves
 * the division is undefined and emits `ud2` instead of `idiv`, so the #DE this
 * command exists to show never happens. */
static volatile int fault_dividend = 1;
static volatile int fault_divisor; /* left zero */

static int cmd_fault(int argc, char **argv)
{
    const char *what = (argc > 1) ? argv[1] : "help";

    /* Deliberate fault injection. This exists so the exception path can be
     * exercised on demand rather than only by accident, and so the quality of
     * the diagnostics can be judged. Each of these halts the kernel. */
    if (strcmp(what, "null") == 0) {
        kprintf("writing to address 0 (unmapped null page)...\n");
        volatile u32 *p = (volatile u32 *)fault_null_addr;
        *p = 1;
    } else if (strcmp(what, "unmapped") == 0) {
        kprintf("reading an unmapped address...\n");
        volatile u32 *p = (volatile u32 *)fault_unmapped_addr;
        kprintf("read %08x\n", *p);
    } else if (strcmp(what, "readonly") == 0) {
        /* Exercises CR0.WP: a ring-0 write to a page marked read-only must
         * fault, not silently succeed. */
        kprintf("writing to a read-only page from ring 0...\n");
        if (!vmm_alloc_at(0xC9100000u, PTE_PRESENT)) {
            kprintf("could not map a test page\n");
            return 1;
        }
        fault_readonly_addr = 0xC9100000u;
        volatile u32 *p = (volatile u32 *)fault_readonly_addr;
        *p = 0xDEAD;
    } else if (strcmp(what, "div0") == 0) {
        kprintf("dividing by zero...\n");
        int quotient = fault_dividend / fault_divisor;
        kprintf("result %d (we should never get here)\n", quotient);
    } else if (strcmp(what, "ud") == 0) {
        kprintf("executing an invalid opcode...\n");
        __asm__ volatile("ud2");
    } else if (strcmp(what, "panic") == 0) {
        panic("deliberate panic requested from the shell");
    } else {
        kprintf("usage: fault <null|unmapped|readonly|div0|ud|panic>\n");
        kprintf("each of these deliberately crashes the kernel so the\n");
        kprintf("exception handlers and backtrace can be inspected.\n");
        return 1;
    }

    return 0;
}

static void stress_worker(void *arg)
{
    u32 rounds = (u32)(uintptr_t)arg;

    for (u32 i = 0; i < rounds; i++) {
        void *blocks[8];

        for (int j = 0; j < 8; j++)
            blocks[j] = kmalloc((size_t)(32 + ((i + (u32)j) * 61) % 900));
        for (int j = 7; j >= 0; j--)
            kfree(blocks[j]);

        if ((i & 0x3F) == 0)
            sched_yield();
    }
}

static int cmd_stress(int argc, char **argv)
{
    u32 workers = 4, rounds = 200;

    if (argc > 1 && !str_to_u32(argv[1], &workers))
        return 1;
    if (argc > 2 && !str_to_u32(argv[2], &rounds))
        return 1;
    if (workers > 8)
        workers = 8;

    kprintf("starting %u workers x %u alloc/free rounds\n", workers, rounds);

    struct heap_stats before;
    heap_get_stats(&before);

    for (u32 i = 0; i < workers; i++)
        if (!task_create("stress", stress_worker, (void *)(uintptr_t)rounds)) {
            kprintf("could not create worker %u\n", i);
            break;
        }

    /* Wait for them to finish by watching the free counter settle. */
    u32 quiet = 0;
    u32 last = 0;
    for (int spin = 0; spin < 4000 && quiet < 10; spin++) {
        struct heap_stats now;
        heap_get_stats(&now);
        quiet = (now.free_calls == last) ? quiet + 1 : 0;
        last = now.free_calls;
        task_sleep_ms(10);
    }

    struct heap_stats after;
    heap_get_stats(&after);

    kprintf("heap: %u allocs, %u frees during the run\n",
            after.alloc_calls - before.alloc_calls,
            after.free_calls - before.free_calls);
    kprintf("heap integrity: %s\n",
            heap_check() == 0 ? "consistent" : "PROBLEMS FOUND");
    kprintf("in use before: %u B, after: %u B\n", before.used_bytes,
            after.used_bytes);
    return 0;
}

static int cmd_bench(int argc, char **argv)
{
    if (argc > 1) {
        if (strcmp(argv[1], "list") == 0) {
            bench_list();
            return 0;
        }
        return (int)bench_run_one(argv[1]);
    }

    return (int)bench_run_all();
}

static int cmd_syms(int argc, char **argv)
{
    u32 addr;

    if (!ksyms_available()) {
        kprintf("this kernel has no embedded symbol table\n");
        return 1;
    }

    if (argc < 2) {
        kprintf("%u symbols embedded, covering %p - %p\n", ksym_count,
                (void *)__text_start, (void *)__text_end);
        kprintf("usage: syms <address>\n");
        return 0;
    }

    if (!str_to_u32(argv[1], &addr)) {
        kprintf("'%s' is not a valid address\n", argv[1]);
        return 1;
    }

    u32 offset = 0;
    const char *name = ksym_lookup(addr, &offset);

    if (!name) {
        kprintf("%p is not inside any known function\n", (void *)addr);
        return 1;
    }

    kprintf("%p  ", (void *)addr);
    ksym_print(addr);
    kprintf("\n");
    return 0;
}

/* A mixed workload to profile, so that `profile run` demonstrates something
 * without the user having to arrange a load by hand. */
static void profile_workload(void *arg)
{
    u32 ms = (u32)(uintptr_t)arg;
    u64 deadline = timer_ms() + ms;

    while (timer_ms() < deadline) {
        /* Deliberately lopsided: lots of allocator traffic, some string
         * formatting, a little page-table work. A flat profile would mean
         * the profiler is not discriminating. */
        for (int i = 0; i < 400; i++) {
            void *p = kmalloc(64 + (size_t)(i & 255));
            kfree(p);
        }

        char buf[48];
        for (int i = 0; i < 100; i++)
            ksnprintf(buf, sizeof(buf), "%d %08x %s", i, (unsigned)i, "x");

        for (int i = 0; i < 20; i++) {
            paddr_t out;
            vmm_translate((vaddr_t)__kernel_start + (u32)i * 4096, &out);
        }
    }
}

static int cmd_profile(int argc, char **argv)
{
    const char *action = (argc > 1) ? argv[1] : "show";

    if (strcmp(action, "start") == 0) {
        if (!profile_start()) {
            kprintf("could not start the profiler\n");
            return 1;
        }
        kprintf("profiler started; run a workload, then 'profile stop'\n");
        return 0;
    }

    if (strcmp(action, "stop") == 0) {
        profile_stop();
        kprintf("profiler stopped\n");
        profile_report(20);
        return 0;
    }

    if (strcmp(action, "reset") == 0) {
        profile_reset();
        kprintf("counters cleared\n");
        return 0;
    }

    if (strcmp(action, "show") == 0) {
        u32 top = 20;
        if (argc > 2 && !str_to_u32(argv[2], &top))
            return 1;
        profile_report(top);
        return 0;
    }

    if (strcmp(action, "run") == 0) {
        u32 ms = 1500;

        if (argc > 2 && !str_to_u32(argv[2], &ms))
            return 1;
        if (ms > 10000)
            ms = 10000;

        if (!profile_start()) {
            kprintf("could not start the profiler\n");
            return 1;
        }

        kprintf("profiling a mixed workload for %u ms...\n", ms);

        /* Run the load in a task so the shell's own read loop does not
         * dominate the profile. */
        if (!task_create("prof-load", profile_workload,
                         (void *)(uintptr_t)ms)) {
            profile_stop();
            kprintf("could not start the workload task\n");
            return 1;
        }

        task_sleep_ms(ms + 250);
        profile_stop();
        kprintf("\n");
        profile_report(20);
        return 0;
    }

    kprintf("usage: profile <run [ms]|start|stop|show [n]|reset>\n");
    return 1;
}

static int cmd_reboot(int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);
    kprintf("rebooting...\n");
    timer_busy_wait_ms(100);
    cpu_reset();
}

static int cmd_halt(int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);
    kprintf("halting. it is now safe to close the emulator.\n");
    timer_busy_wait_ms(50);
    cpu_qemu_exit(0x01);
    cpu_halt_forever();
}

static const struct shell_command commands[] = {
    {"help", "help [command]", "list commands, or explain one", cmd_help},
    {"clear", "clear", "clear the screen", cmd_clear},
    {"echo", "echo <words...>", "print its arguments", cmd_echo},
    {"version", "version", "kernel version and build info", cmd_version},
    {"uptime", "uptime", "time since boot", cmd_uptime},
    {"date", "date", "read the hardware clock", cmd_date},
    {"cpuinfo", "cpuinfo", "CPU vendor, model and features", cmd_cpuinfo},
    {"meminfo", "meminfo", "physical, virtual and heap usage", cmd_meminfo},
    {"ps", "ps", "list tasks", cmd_ps},
    {"irq", "irq", "interrupt counters", cmd_irq},
    {"pci", "pci", "enumerate the PCI bus", cmd_pci},
    {"pagemap", "pagemap <address>", "resolve a virtual address", cmd_pagemap},
    {"hexdump", "hexdump <address> [bytes]", "dump memory", cmd_hexdump},
    {"log", "log [level]", "show or set the log level", cmd_log},
    {"selftest", "selftest [suite|list]", "run the in-kernel test suite",
     cmd_selftest},
    {"bench", "bench [name|list]", "measure kernel hot paths", cmd_bench},
    {"profile", "profile <run|start|stop|show|reset>", "sampling profiler",
     cmd_profile},
    {"syms", "syms <address>", "resolve an address to a symbol", cmd_syms},
    {"ring3", "ring3", "run the user-mode demo", cmd_ring3},
    {"stress", "stress [workers] [rounds]",
     "hammer the heap from several tasks", cmd_stress},
    {"fault", "fault <null|unmapped|readonly|div0|ud|panic>",
     "crash on purpose, to show the handlers", cmd_fault},
    {"reboot", "reboot", "reset the machine", cmd_reboot},
    {"halt", "halt", "stop the machine", cmd_halt},
    {NULL, NULL, NULL, NULL},
};

/* ---- line editing ------------------------------------------------------ */

static void redraw(void)
{
    kprintf("\r%s%s", PROMPT, line);

    /* One trailing space erases the character left behind by a deletion. */
    kprintf(" ");

    for (size_t i = line_len + 1; i > cursor; i--)
        kprintf("\b");
}

static void history_add(const char *text)
{
    if (!*text)
        return;

    /* Skip an immediate repeat: pressing Enter twice should not fill the
     * history with the same line. */
    if (history_count &&
        strcmp(history[(history_count - 1) % SHELL_HISTORY], text) == 0)
        return;

    strlcpy(history[history_count % SHELL_HISTORY], text, SHELL_LINE_MAX);
    history_count++;
}

static void history_recall(size_t back)
{
    if (back == 0 || back > history_count || back > SHELL_HISTORY) {
        history_pos = 0;
        line[0] = '\0';
        line_len = cursor = 0;
        redraw();
        return;
    }

    history_pos = back;
    strlcpy(line, history[(history_count - back) % SHELL_HISTORY],
            SHELL_LINE_MAX);
    line_len = strlen(line);
    cursor = line_len;
    redraw();
}

static void insert_char(char c)
{
    if (line_len + 1 >= SHELL_LINE_MAX)
        return;

    memmove(&line[cursor + 1], &line[cursor], line_len - cursor + 1);
    line[cursor] = c;
    line_len++;
    cursor++;

    /* Appending at the end is the common case and needs no redraw. */
    if (cursor == line_len)
        kprintf("%c", c);
    else
        redraw();
}

static void delete_before_cursor(void)
{
    if (cursor == 0)
        return;

    memmove(&line[cursor - 1], &line[cursor], line_len - cursor + 1);
    cursor--;
    line_len--;

    if (cursor == line_len)
        kprintf("\b \b");
    else
        redraw();
}

static void delete_at_cursor(void)
{
    if (cursor >= line_len)
        return;

    memmove(&line[cursor], &line[cursor + 1], line_len - cursor);
    line_len--;
    redraw();
}

/* ---- parsing and execution -------------------------------------------- */

void shell_run_line(const char *text)
{
    static char scratch[SHELL_LINE_MAX];
    char *argv[SHELL_MAX_ARGS + 1];
    int argc = 0;

    strlcpy(scratch, text, sizeof(scratch));

    char *p = scratch;
    while (*p && argc < SHELL_MAX_ARGS) {
        while (*p == ' ' || *p == '\t')
            *p++ = '\0';
        if (!*p)
            break;

        argv[argc++] = p;

        while (*p && *p != ' ' && *p != '\t')
            p++;
    }
    argv[argc] = NULL;

    if (argc == 0)
        return;

    for (size_t i = 0; commands[i].name; i++) {
        if (strcmp(argv[0], commands[i].name) == 0) {
            int rc = commands[i].fn(argc, argv);
            if (rc != 0)
                pr_debug("%s returned %d", argv[0], rc);
            return;
        }
    }

    kprintf("%s: command not found. Try 'help'.\n", argv[0]);
}

void shell_init(void)
{
    line[0] = '\0';
    line_len = cursor = 0;
    history_count = 0;
    history_pos = 0;
}

NORETURN void shell_task(void *arg)
{
    UNUSED(arg);

    shell_init();

    kprintf("\n");
    vga_set_attr(vga_attr(VGA_LIGHT_CYAN, VGA_BLACK));
    kprintf("%s %s - type 'help' for commands, 'selftest' to run the "
            "test suite\n",
            STRATUM_NAME, STRATUM_VERSION);
    vga_set_attr(vga_attr(VGA_LIGHT_GREY, VGA_BLACK));
    kprintf("%s", PROMPT);

    for (;;) {
        int key = keyboard_getchar();

        switch (key) {
        case '\n':
        case '\r':
            kprintf("\n");
            line[line_len] = '\0';
            history_add(line);
            shell_run_line(line);
            line[0] = '\0';
            line_len = cursor = 0;
            history_pos = 0;
            kprintf("%s", PROMPT);
            break;

        case '\b':
        case 0x7F:
            delete_before_cursor();
            break;

        case KEY_DELETE:
            delete_at_cursor();
            break;

        case KEY_LEFT:
            if (cursor > 0) {
                cursor--;
                kprintf("\b");
            }
            break;

        case KEY_RIGHT:
            if (cursor < line_len) {
                kprintf("%c", line[cursor]);
                cursor++;
            }
            break;

        case KEY_HOME:
            cursor = 0;
            redraw();
            break;

        case KEY_END:
            cursor = line_len;
            redraw();
            break;

        case KEY_UP:
            history_recall(history_pos + 1);
            break;

        case KEY_DOWN:
            history_recall(history_pos ? history_pos - 1 : 0);
            break;

        case 0x03: /* Ctrl-C: abandon the line */
            kprintf("^C\n%s", PROMPT);
            line[0] = '\0';
            line_len = cursor = 0;
            history_pos = 0;
            break;

        case 0x0C: /* Ctrl-L: clear and redraw */
            vga_clear();
            redraw();
            break;

        case 0x01: /* Ctrl-A */
            cursor = 0;
            redraw();
            break;

        case 0x05: /* Ctrl-E */
            cursor = line_len;
            redraw();
            break;

        case 0x15: /* Ctrl-U: kill the line */
            line[0] = '\0';
            line_len = cursor = 0;
            redraw();
            break;

        default:
            if (key >= 0x20 && key < 0x7F)
                insert_char((char)key);
            break;
        }
    }
}
