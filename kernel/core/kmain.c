/* StratumOS - kernel entry point and boot sequence.
 *
 * The order of initialisation here is not arbitrary; most of it is forced.
 *
 *   serial first         - so that a failure in anything below is reportable
 *   CPU identification   - before any feature is relied upon
 *   boot params          - everything downstream needs the memory map
 *   GDT -> IDT -> PIC    - descriptors before traps, traps before devices
 *   devices, then STI    - a handler must exist before its interrupt is
 *                          enabled, and the PIC must be remapped before
 *                          interrupts are unmasked at all
 *   PMM -> VMM -> heap   - each strictly depends on the previous
 *   scheduler last       - it needs the heap for task stacks
 *
 * The single most important line is where sti() appears: after the PIC has
 * been remapped and after every enabled line has a handler. Enabling
 * interrupts before that turns the first timer tick into a double fault,
 * because the 8259's power-on mapping puts IRQ 0 on vector 8.
 */
#define LOG_TAG "boot"

#include <arch/acpi.h>
#include <arch/apic.h>
#include <arch/cpu.h>
#include <arch/gdt.h>
#include <arch/harden.h>
#include <arch/idt.h>
#include <arch/io.h>
#include <arch/irq.h>
#include <arch/longmode.h>

#include <drivers/ata.h>
#include <drivers/e1000.h>
#include <drivers/keyboard.h>
#include <drivers/pci.h>
#include <drivers/rtc.h>
#include <drivers/serial.h>
#include <drivers/timer.h>
#include <drivers/vga.h>

#include <kernel/bench.h>
#include <kernel/console.h>
#include <kernel/kernel.h>
#include <kernel/ktest.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/printf.h>
#include <kernel/profile.h>
#include <kernel/sched.h>
#include <kernel/shell.h>
#include <kernel/smp.h>
#include <kernel/string.h>
#include <kernel/syscall.h>
#include <kernel/usermode.h>

#include <mm/heap.h>
#include <mm/pmm.h>
#include <mm/vmm.h>

#include <fs/blockdev.h>
#include <fs/fat16.h>
#include <net/net.h>

/* QEMU `isa-debug-exit` turns a port write into a process exit status of
 * (code << 1) | 1, so these become 3, 5 and 35 respectively. */
#define EXIT_TESTS_PASSED 0x01
#define EXIT_TESTS_FAILED 0x02

static const struct boot_params *boot_params;
static struct kernel_cmdline cmdline;

const struct boot_params *kernel_boot_params(void)
{
    return boot_params;
}

const struct kernel_cmdline *kernel_cmdline(void)
{
    return &cmdline;
}

/* ------------------------------------------------------------------------- */

static bool cmdline_has(const char *cmd, const char *flag)
{
    size_t flag_len = strlen(flag);

    /* Match whole space-delimited words so that "nodemo" is not found inside
     * "nodemonitor". */
    for (const char *p = cmd; *p; p++) {
        if (p != cmd && p[-1] != ' ')
            continue;
        if (strncmp(p, flag, flag_len) == 0 &&
            (p[flag_len] == '\0' || p[flag_len] == ' ' || p[flag_len] == '='))
            return true;
    }
    return false;
}

static const char *cmdline_value(const char *cmd, const char *key)
{
    size_t key_len = strlen(key);

    for (const char *p = cmd; *p; p++) {
        if (p != cmd && p[-1] != ' ')
            continue;
        if (strncmp(p, key, key_len) == 0 && p[key_len] == '=')
            return p + key_len + 1;
    }
    return NULL;
}

static void parse_cmdline(const char *cmd)
{
    memset(&cmdline, 0, sizeof(cmdline));

    if (!cmd)
        return;

    cmdline.autotest = cmdline_has(cmd, "autotest");
    cmdline.autobench = cmdline_has(cmd, "autobench");
    cmdline.quiet = cmdline_has(cmd, "quiet");
    cmdline.no_usermode = cmdline_has(cmd, "nousermode");
    cmdline.no_sched_demo = cmdline_has(cmd, "nodemo");
    cmdline.loglevel = cmdline_value(cmd, "loglevel");
}

static void banner(void)
{
    char title[72];
    int width;

    /* Draw the box around the measured text rather than against hand-counted
     * dashes, so the frame cannot drift out of alignment when the version
     * string changes length. */
    width = ksnprintf(title, sizeof(title),
                      "%s %s  -  x86 kernel: real mode to ring 3", STRATUM_NAME,
                      STRATUM_VERSION);

    vga_set_attr(vga_attr(VGA_LIGHT_CYAN, VGA_BLACK));
    kprintf("\n  .");
    for (int i = 0; i < width + 2; i++)
        kprintf("-");
    kprintf(".\n  | %s |\n  '", title);
    for (int i = 0; i < width + 2; i++)
        kprintf("-");
    kprintf("'\n");
    vga_set_attr(vga_attr(VGA_LIGHT_GREY, VGA_BLACK));
}

/* ---- background tasks -------------------------------------------------- */

/* Repaints the VGA status bar. Exists to make preemption visible: if the clock
 * in the corner keeps moving while the shell is busy, the scheduler is
 * genuinely switching tasks. */
static void status_task(void *arg)
{
    UNUSED(arg);

    for (;;) {
        char bar[VGA_WIDTH + 1];
        struct pmm_stats pm;
        u64 ms = timer_ms();
        u32 secs = (u32)(ms / 1000);

        pmm_get_stats(&pm);

        ksnprintf(bar, sizeof(bar),
                  " %s %s  up %02u:%02u:%02u  mem %u/%u MiB  tasks:%u  "
                  "ctxsw:%u",
                  STRATUM_NAME, STRATUM_VERSION, secs / 3600, (secs / 60) % 60,
                  secs % 60, (pm.used_frames * 4) / 1024,
                  (pm.total_frames * 4) / 1024, 1u, sched_switch_count());

        vga_status_line(bar, vga_attr(VGA_BLACK, VGA_LIGHT_GREY));
        task_sleep_ms(250);
    }
}

/* A pair of silent CPU-bound tasks, so that `ps` has something to show and
 * the round-robin can be observed sharing time. */
static void demo_worker(void *arg)
{
    u32 id = (u32)(uintptr_t)arg;
    u64 spins = 0;

    for (;;) {
        for (int i = 0; i < 20000; i++)
            __asm__ volatile("" ::: "memory");
        spins++;

        if ((spins % 40) == 0)
            pr_debug("worker %u reached %llu spins", id, spins);

        task_sleep_ms(20 + id * 10);
    }
}

/* Benchmarks and a profile, then shut down. Separate from autotest so a CI
 * run can assert on correctness without waiting for measurements, and so the
 * benchmark numbers are not interleaved with test output. */
static void autobench_task(void *arg)
{
    UNUSED(arg);

    task_sleep_ms(300);

    kprintf("\n");
    unsigned failed = bench_run_all();

    kprintf("\n");
    if (profile_start()) {
        /* The workload has to be CPU-bound. An earlier version slept 5 ms
         * between short bursts of work, so at a 100 Hz sample rate almost
         * every sample landed in the idle task - a correct profile of a
         * machine that was, in fact, idle. */
        u64 deadline = timer_ms() + 700;

        while (timer_ms() < deadline) {
            for (int i = 0; i < 250; i++) {
                void *p = kmalloc(64 + (size_t)(i & 127));
                kfree(p);
            }

            char buf[48];
            for (int i = 0; i < 60; i++)
                ksnprintf(buf, sizeof(buf), "%d %08x", i, (unsigned)i);
        }

        profile_stop();
        profile_report(12);
    }

    kprintf("\nstratum: autobench complete, shutting down\n");
    timer_busy_wait_ms(50);
    cpu_qemu_exit(failed == 0 ? EXIT_TESTS_PASSED : EXIT_TESTS_FAILED);
    cpu_halt_forever();
}

static void autotest_task(void *arg)
{
    UNUSED(arg);

    /* Let the boot settle so the timer and the ring-3 demo have run. */
    task_sleep_ms(500);

    kprintf("\n");
    unsigned failed = ktest_run_all();

    kprintf("ktest: %s\n",
            failed == 0 ? "ALL TESTS PASSED" : "THERE WERE FAILURES");
    kprintf("stratum: autotest complete, shutting down\n");

    timer_busy_wait_ms(50);
    cpu_qemu_exit(failed == 0 ? EXIT_TESTS_PASSED : EXIT_TESTS_FAILED);

    /* Only reached when isa-debug-exit is absent, i.e. an interactive run. */
    kprintf("stratum: (no isa-debug-exit device; halting instead)\n");
    cpu_halt_forever();
}

/* ------------------------------------------------------------------------- */

void kmain(u32 magic, u32 info_addr)
{
    /* 1. Serial, before anything that could fail. It needs no memory
     *    manager, no interrupts and no display. */
    bool have_serial = serial_init(COM1_BASE, 115200);

    /* 2. The display. */
    vga_init();
    console_init();

    banner();
    log_boot_step("serial COM1", have_serial,
                  have_serial ? "115200 8N1" : "no UART detected");
    log_boot_step("VGA text mode", true, "80x25");

    /* 3. What kind of CPU is this, and what can it do? */
    cpu_detect();
    const struct cpu_info *ci = cpu_get_info();
    log_boot_step("CPU detect", ci->has_cpuid, ci->vendor);

    /* 4. Normalise whatever the bootloader told us. Without a memory map
     *    there is no point continuing. */
    boot_params = boot_parse(magic, info_addr);
    if (!boot_params)
        panic("no usable boot information (magic %08x, info %p)", magic,
              (void *)info_addr);

    parse_cmdline(boot_params->cmdline);

    if (cmdline.loglevel && !log_set_level_by_name(cmdline.loglevel))
        pr_warn("unknown loglevel '%s' on the command line", cmdline.loglevel);
    if (cmdline.quiet)
        console_disable(CONSOLE_SINK_VGA);

    log_boot_step("boot protocol", true, boot_params->protocol_name);

    /* 5. Descriptors. The GDT must be live before the IDT references its
     *    code selector. */
    gdt_init();
    log_boot_step("GDT + TSS", true, "flat 4 GiB, rings 0 and 3");

    idt_init();
    log_boot_step("IDT", true, "256 vectors");

    /* 6. Remap the PIC away from the exception vectors, with every line still
     *    masked. */
    irq_init();
    log_boot_step("PIC remap", true, "IRQ 0-15 -> vector 32-47");

    /* 7. Devices. Each unmasks its own line as it installs its handler, so no
     *    interrupt can arrive before someone is ready for it. */
    timer_init(TIMER_HZ);
    log_boot_step("PIT timer", true, "100 Hz");

    keyboard_init();
    log_boot_step("PS/2 keyboard", true, "scancode set 1");

    /* Serial becomes an input device too, so the shell is usable over a
     * serial line with no display attached. */
    serial_console_init();
    log_boot_step("serial console", have_serial, "input on IRQ 4");

    /* 8. NOW interrupts can be enabled. */
    sti();
    log_boot_step("interrupts", irq_enabled(), "enabled");

    /* 9. Memory management, in dependency order. */
    pmm_init(boot_params);
    struct pmm_stats pm;
    pmm_get_stats(&pm);
    log_boot_step("physical memory", pm.free_frames > 0, "bitmap allocator");

    vmm_init();
    log_boot_step("paging", vmm_is_enabled(), "4 KiB pages, recursive PD");

    heap_init();
    log_boot_step("kernel heap", true, "first-fit, guarded, growable");

    /* Hardening goes on now, after paging and the heap and before any user
     * program or second address space exists. Turning SMAP on later would
     * fault inside code that had already been written without the stac/clac
     * discipline, and narrowing the kernel's own text has to happen while
     * there is one address space to narrow it in. */
    vmm_protect_kernel_text();
    harden_init();
    {
        const struct harden_state *h = harden_get_state();
        log_boot_step("hardening", h->kernel_text_ro,
                      h->smap_enabled ? "W^X, guard pages, SMEP + SMAP"
                      : h->smep_enabled
                          ? "W^X, guard pages, SMEP (no SMAP on this CPU)"
                          : "W^X, guard pages (no SMEP/SMAP on this CPU)");
    }

    /* 10. Non-essential hardware. A failure here is worth reporting but not
     *     worth refusing to boot over. */
    rtc_init();
    log_boot_step("RTC", true, "CMOS clock");

    pci_init();
    log_boot_step("PCI", true, "legacy 0xCF8 enumeration");

    /* Processors. ACPI first, because the MADT is the only thing that says
     * how many there are and where the local APIC's registers live; then the
     * local APIC, which is what can send an interrupt to another processor at
     * all; then the other processors themselves.
     *
     * All three come after paging and the heap, because the firmware tables
     * sit outside the linear map and have to be mapped, and after the
     * hardening step, because a processor that came up before SMAP was
     * enabled would be running with it off. */
    acpi_init();
    {
        const struct acpi_info *ai = acpi_get_info();
        char detail[56];

        if (ai->available)
            ksnprintf(detail, sizeof(detail), "%s, %u tables, %u cpu(s)",
                      ai->used_xsdt ? "XSDT" : "RSDT", ai->tables_seen,
                      ai->cpus_reported);
        else
            strlcpy(detail, "no tables on this machine", sizeof(detail));

        log_boot_step("ACPI", ai->available, detail);
    }

    bool have_apic = apic_init_bsp();

    log_boot_step("local APIC", have_apic,
                  have_apic ? "enabled, IPIs available"
                            : "absent; uniprocessor only");

    smp_init();
    {
        char detail[56];

        ksnprintf(detail, sizeof(detail), "%u of %u processor(s) online",
                  smp_cpu_count(), smp_cpus_present());
        log_boot_step("processors", true, detail);
    }

    /* Detection only. The transition itself takes interrupts down and is a
     * demonstration rather than a step this kernel needs, so it happens on
     * demand - `longmode` in the shell, or the `longmode` test suite - and
     * not on every boot. Reporting it here costs one CPUID and answers the
     * question a reader of this log would actually have. */
    {
        const char *why = NULL;
        bool usable = longmode_available(&why);

        /* `ok` is whether the detection worked, not whether the feature is
         * there: a 32-bit processor without long mode is running this kernel
         * exactly as intended, and a [FAIL] beside it reads as something
         * broken. The hardening step words SMEP and SMAP the same way. */
        log_boot_step("x86-64", true,
                      usable ? "long mode available; `longmode` to enter it"
                             : (why ? why : "not available on this CPU"));
    }

    /* Networking. After the PCI scan that found the controller and after
     * interrupts, because the driver installs a handler; before the
     * filesystem only by convention. A machine with no supported controller
     * carries on without one. */
    {
        bool have_net = e1000_init();

        char net_detail[80];

        if (have_net) {
            net_init();

            char mb[MAC_STR_LEN], ib[IPV4_STR_LEN];
            struct e1000_info ei;

            e1000_get_info(&ei);
            ksnprintf(net_detail, sizeof(net_detail), "%s, %s, link %s",
                      mac_str(&ei.mac, mb, sizeof(mb)),
                      ipv4_str(net_get_config()->addr, ib, sizeof(ib)),
                      ei.link_up ? "up" : "down");
        } else {
            strlcpy(net_detail, "no supported controller", sizeof(net_detail));
        }

        /* `ok` is whether the step did its job, not whether the hardware
         * exists - a machine with no Ethernet controller boots exactly as
         * intended, and [FAIL] beside it reads as something broken. Same
         * reasoning as the x86-64 line above. */
        log_boot_step("network", true, net_detail);
    }

    /* Storage. The ATA driver polls, so it needs nothing from the interrupt
     * layer; the block layer needs the heap only indirectly, through the
     * filesystem it hands to. Mounting is attempted and allowed to fail: the
     * GRUB ISO boot path has no FAT partition at all, and a kernel that
     * refused to boot without one would be a kernel that could not be
     * tested through both of its loaders. */
    ata_init();
    log_boot_step("ATA disks", true,
                  ata_drive_count() ? "PIO, polled" : "none found");

    blockdev_init();

    const struct blockdev *fs_dev = blockdev_first_fs();

    if (fs_dev && fat16_mount(fs_dev)) {
        const struct fat_info *fi = fat16_get_info();
        char detail[48];

        ksnprintf(detail, sizeof(detail), "FAT16 \"%s\" on %s", fi->label,
                  fat16_device_name());
        log_boot_step("filesystem", true, detail);
    } else if (fs_dev) {
        /* A partition that claims to hold a filesystem and does not mount is
         * a real failure, and is reported as one. */
        log_boot_step("filesystem", false,
                      "a FAT partition exists but would not mount");
    } else {
        /* No partition at all is the GRUB ISO path, where the only drive is
         * an ATAPI CD-ROM. Not a failure: the kernel runs the programs
         * embedded in its own image, which is why those still exist. */
        log_boot_step("filesystem", true,
                      "none present; using the embedded programs");
    }

    /* 11. Tasks, then the syscall gate they will use. */
    sched_init();
    log_boot_step("scheduler", true, "round robin, 100 Hz preemption");

    /* The network's polling thread, now that there is something to schedule.
     * Does nothing if no controller was found. */
    net_start();

    syscall_init();
    log_boot_step("syscalls", true, "int 0x80");

    kprintf("\n");
    pr_info("%s %s is up: %u MiB RAM, %u PCI devices, %u test suites",
            STRATUM_NAME, STRATUM_VERSION,
            (unsigned)(boot_params->mem_usable / MIB), pci_device_count(),
            ktest_count());

    /* 12. Start the workload. */
    if (!task_create("statusd", status_task, NULL))
        pr_warn("could not start the status bar task");

    if (!cmdline.no_sched_demo) {
        task_create("worker-1", demo_worker, (void *)(uintptr_t)1);
        task_create("worker-2", demo_worker, (void *)(uintptr_t)2);
    }

    if (!cmdline.no_usermode) {
        if (!usermode_spawn_demo())
            pr_warn("the ring-3 demo could not be started");
    }

    if (cmdline.autobench) {
        pr_info("autobench requested on the command line");
        if (!task_create("autobench", autobench_task, NULL))
            panic("could not start the autobench task");
    } else if (cmdline.autotest) {
        pr_info("autotest requested on the command line");
        if (!task_create("autotest", autotest_task, NULL))
            panic("could not start the autotest task");
    } else {
        if (!task_create("shell", (task_entry_t)shell_task, NULL))
            panic("could not start the shell");
    }

    /* 13. This context becomes the idle task. It never returns. */
    sched_start();
}
