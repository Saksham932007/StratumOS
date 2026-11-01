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

#include <arch/acpi.h>
#include <arch/apic.h>
#include <arch/cpu.h>
#include <arch/harden.h>
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
#include <kernel/ksyms.h>
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
#include <fs/vfs.h>
#include <net/inet.h>
#include <net/net.h>

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
    kprintf("  kernel at : %p\n", (void *)KERNEL_VIRT_BASE);
    kprintf("  linear map: %p - %p (%u MiB of physical memory)\n",
            (void *)KERNEL_VIRT_BASE,
            (void *)(KERNEL_VIRT_BASE + VMM_LINEAR_SIZE),
            (unsigned)(VMM_LINEAR_SIZE / MIB));
    kprintf("  user space: 0x00001000 - %p\n", (void *)KERNEL_VIRT_BASE);
    kprintf("  tables    : %u page tables, %u pages mapped\n", vm.page_tables,
            vm.mapped_pages);
    kprintf("  faults    : %u total\n", vm.page_faults);
    kprintf("  spaces    : %u address spaces created\n", vm.address_spaces);
    kprintf("  COW       : %u faults, %u needed a copy, %u resolved by "
            "dropping the last sharer\n",
            vm.cow_faults, vm.cow_copies, vm.cow_faults - vm.cow_copies);
    kprintf("  shared    : %u frames held by more than one address space\n",
            pm.shared_frames);

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

    /* The address space column is the one worth reading. A kernel thread
     * shows "kernel" because it shares the kernel's page directory; a process
     * shows the physical address of its own, and two processes showing the
     * same one would mean fork() handed out a shared address space. */
    char space[12];

    if (!t->page_dir)
        strlcpy(space, "-", sizeof(space));
    else if (t->page_dir == vmm_kernel_pd_phys())
        strlcpy(space, "kernel", sizeof(space));
    else
        ksnprintf(space, sizeof(space), "%08x", (unsigned)t->page_dir);

    kprintf("  %4u %5u  %-14s %-9s %-4s %-9s %8u %7u\n", t->pid, t->parent_pid,
            t->name, task_state_name(t->state), t->user ? "ring3" : "ring0",
            space, t->ticks_total, t->switches);
}

static int cmd_ps(int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);

    kprintf("  %4s %5s  %-14s %-9s %-4s %-9s %8s %7s\n", "PID", "PPID", "NAME",
            "STATE", "RING", "VMSPACE", "TICKS", "SWITCH");
    sched_foreach(ps_row, NULL);
    kprintf("  %u tasks, %u context switches total\n", task_count(),
            sched_switch_count());
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

/* ---- processors --------------------------------------------------------- */

/* Drive the boot processor into 64-bit long mode, run a payload there, come
 * back, and print everything the payload saw. */
static int cmd_longmode(int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);

    const char *why = NULL;

    kprintf("Long mode (x86-64)\n");
    kprintf("  CPUID       : %s\n", longmode_supported()
                                        ? "CPUID.80000001H:EDX.LM is set"
                                        : "this processor has no long mode");

    if (!longmode_available(&why)) {
        kprintf("  available   : no - %s\n", why);
        return 1;
    }

    struct longmode_result r;

    kprintf("  transition  : 32-bit protected -> 64-bit long -> 32-bit "
            "protected\n");

    bool ok = longmode_round_trip(&r);

    kprintf("\nPaging\n");
    kprintf("  4-level     : pml4 %p -> pdpt %p -> pd %p\n",
            (void *)r.pml4_phys, (void *)r.pdpt_phys, (void *)r.pd_phys);
    kprintf("  identity    : %u MiB in 2 MiB pages (%u PD entries)\n",
            r.identity_mib, r.identity_mib / 2);
    kprintf("  entries     : 64 bits wide; CR4.PAE required, so paging is "
            "disabled to set it\n");

    kprintf("\nStages reached\n");
    static const struct {
        u32 bit;
        const char *what;
    } stages[] = {
        {LM_F_ENTERED, "the trampoline's own GDT, 32-bit, identity-mapped"},
        {LM_F_COMPAT, "CR0.PG set with the PML4: IA-32e compatibility mode"},
        {LM_F_LONG, "far jump to a descriptor with L set: 64-bit mode"},
        {LM_F_VERIFIED, "the 64-bit payload ran to completion"},
        {LM_F_BACK32, "back to 32-bit code, still on 64-bit paging"},
        {LM_F_RETURNED, "kernel CR3, kernel GDT, kernel stack restored"},
    };

    for (size_t i = 0; i < ARRAY_SIZE(stages); i++)
        kprintf("  [%s] %s\n", (r.flags & stages[i].bit) ? "ok" : "--",
                stages[i].what);

    kprintf("\nWhat 64-bit mode proved\n");
    kprintf("  CS          : 0x%02x (the descriptor whose L bit is set)\n",
            r.cs);
    kprintf("  EFER        : 0x%08llx  LME %s, LMA %s\n", r.efer,
            (r.efer & EFER_LME_BIT) ? "set" : "CLEAR",
            (r.efer & EFER_LMA_BIT) ? "set" : "CLEAR");
    kprintf("  CR4.PAE     : %s\n", (r.cr4 & CR4_PAE_BIT) ? "set" : "CLEAR");
    kprintf("  64-bit imm  : 0x%016llx %s\n", r.wide,
            r.wide == LM_WIDE_VALUE ? "" : "<- WRONG");
    kprintf("  0xffffffff+1: 0x%016llx %s\n", r.crossed,
            r.crossed == LM_CROSSED_VALUE ? "(32-bit mode gives 0)"
                                          : "<- WRONG");
    kprintf("  r15         : 0x%016llx %s\n", r.r15,
            r.r15 == LM_R15_VALUE ? "(a register 32-bit mode lacks)"
                                  : "<- WRONG");
    kprintf("  lea [rip+x] : %p, expected %p %s\n", (void *)(u32)r.rip_lea,
            (void *)r.rip_expected,
            (u64)r.rip_expected == r.rip_lea ? "" : "<- WRONG");
    kprintf("  [64-bit ptr]: 0x%016llx from phys %p %s\n", r.probe_value,
            (void *)r.probe_phys,
            r.probe_value == LM_PROBE_MAGIC ? "(only the 4-level walk maps it)"
                                            : "<- WRONG");

    kprintf("\n  round trip  : %llu cycles, trampoline %u bytes\n", r.cycles,
            r.trampoline_bytes);
    kprintf("  kernel      : still 32-bit, still running - CR4.PAE %s, "
            "paging %s\n",
            (read_cr4() & CR4_PAE_BIT) ? "SET (wrong)" : "clear",
            (read_cr0() & 0x80000000u) ? "on" : "OFF (wrong)");

    if (ok) {
        kprintf("\n  result      : round trip complete\n");
        return 0;
    }

    kprintf("\n  result      : FAILED - %s\n",
            r.failure ? r.failure : "unknown");
    return 1;
}

/* ---- networking --------------------------------------------------------- */

static int cmd_net(int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);

    struct e1000_info ei;
    const struct net_config *cfg = net_get_config();
    const struct net_stats *s = net_stats();
    char b1[MAC_STR_LEN], b2[IPV4_STR_LEN], b3[IPV4_STR_LEN], b4[IPV4_STR_LEN];

    e1000_get_info(&ei);

    if (!ei.present) {
        kprintf("no supported Ethernet controller on this machine\n");
        kprintf("(QEMU: add -netdev user,id=n0 -device e1000,netdev=n0)\n");
        return 1;
    }

    kprintf("Controller\n");
    kprintf("  device    : %04x:%04x at %02x:%02x.%u, IRQ %u\n", ei.vendor_id,
            ei.device_id, ei.bus, ei.slot, ei.func, ei.irq);
    kprintf("  registers : phys %p, mapped uncached\n", (void *)ei.mmio_phys);
    kprintf("  MAC       : %s%s\n", mac_str(&ei.mac, b1, sizeof(b1)),
            ei.mac_from_eeprom ? " (read from the EEPROM)" : "");
    kprintf("  link      : %s", ei.link_up ? "up" : "down");
    if (ei.link_up)
        kprintf(", %u Mb/s, %s duplex", ei.link_speed_mbps,
                ei.full_duplex ? "full" : "half");
    kprintf("\n");

    kprintf("Rings\n");
    kprintf("  receive   : %u descriptors at phys %p, head %u tail %u\n",
            ei.rx_ring_entries, (void *)ei.rx_ring_phys, ei.rx_head,
            ei.rx_tail);
    kprintf("  transmit  : %u descriptors at phys %p, head %u tail %u\n",
            ei.tx_ring_entries, (void *)ei.tx_ring_phys, ei.tx_head,
            ei.tx_tail);
    kprintf("  device    : %u rx, %u tx, %u CRC error(s), %u no-buffer\n",
            ei.dev_rx_packets, ei.dev_tx_packets, ei.dev_crc_errors,
            ei.dev_rx_no_buffers);
    kprintf("  driver    : %u interrupt(s), %u tx-ring-full, %u overrun(s), "
            "%u reentry(s)\n",
            ei.interrupts, ei.tx_ring_full, ei.rx_overruns, ei.rx_reentries);

    kprintf("Addressing\n");
    kprintf("  address   : %s\n", ipv4_str(cfg->addr, b2, sizeof(b2)));
    kprintf("  netmask   : %s\n", ipv4_str(cfg->netmask, b3, sizeof(b3)));
    kprintf("  gateway   : %s\n", ipv4_str(cfg->gateway, b4, sizeof(b4)));

    kprintf("Frames\n");
    kprintf("  ethernet  : %u in (%u B), %u out (%u B)\n", s->rx_frames,
            s->rx_bytes, s->tx_frames, s->tx_bytes);
    kprintf("  dropped   : %u short, %u not ours, %u unknown ethertype\n",
            s->rx_dropped_short, s->rx_dropped_not_ours,
            s->rx_unknown_ethertype);
    kprintf("  arp       : %u in, %u out (%u request(s), %u reply(s) sent)\n",
            s->arp_rx, s->arp_tx, s->arp_requests_sent, s->arp_replies_sent);
    kprintf("  ipv4      : %u in, %u out; %u bad checksum, %u fragment(s), "
            "%u not ours\n",
            s->ip_rx, s->ip_tx, s->ip_bad_checksum, s->ip_fragments_dropped,
            s->ip_not_ours);
    kprintf("  icmp      : %u in; %u echo request(s) -> %u reply(s) sent; "
            "%u reply(s) received\n",
            s->icmp_rx, s->icmp_echo_requests, s->icmp_echo_replies_sent,
            s->icmp_echo_replies_received);
    kprintf("  udp       : %u in, %u out; %u no port, %u bad checksum\n",
            s->udp_rx, s->udp_tx, s->udp_no_port, s->udp_bad_checksum);
    kprintf("  tcp       : %u in, %u out; %u bad checksum, %u no port, "
            "%u reset(s) sent\n",
            s->tcp_rx, s->tcp_tx, s->tcp_bad_checksum, s->tcp_no_port,
            s->tcp_resets_sent);

    struct tcp_status ts;

    tcp_get_status(&ts);
    kprintf("Listening\n");
    kprintf("  udp echo  : port %u\n", udp_echo_port());
    kprintf("  tcp echo  : port %u, state %s\n", tcp_listen_port(),
            tcp_state_name(ts.state));
    if (ts.state != TCP_LISTEN && ts.state != TCP_CLOSED) {
        char rb[IPV4_STR_LEN];

        kprintf("  peer      : %s:%u\n", ipv4_str(ts.remote, rb, sizeof(rb)),
                ts.remote_port);
        kprintf("  sequence  : snd_una %u snd_nxt %u rcv_nxt %u\n", ts.snd_una,
                ts.snd_nxt, ts.rcv_nxt);
    }
    kprintf("  accepted  : %u connection(s), %u byte(s) echoed, "
            "%u retransmit(s), %u out of order\n",
            s->tcp_connections_accepted, s->tcp_bytes_echoed, ts.retransmits,
            ts.out_of_order_dropped);
    /* The congestion state, reported because a congestion window nobody can
     * see is a congestion window nobody can tell is working. The first two
     * numbers are what the sender believes about the path; the last three
     * are how it came to believe it. */
    kprintf("  congestion: cwnd %u, ssthresh %u%s\n", ts.cwnd, ts.ssthresh,
            ts.in_recovery ? " (in fast recovery)" : "");
    kprintf("  round trip: srtt %u ms, var %u ms, rto %u ms\n", ts.srtt_ms,
            ts.rttvar_ms, ts.rto_ms);
    kprintf("  loss      : %u fast retransmit(s), %u timeout(s), "
            "%u duplicate ack(s)\n",
            ts.fast_retransmits, ts.timeouts, ts.dup_acks);

    const struct udp_last *ul = udp_last_datagram();

    if (ul->valid) {
        char sb[IPV4_STR_LEN];

        kprintf("  last udp  : %u byte(s) from %s:%u to port %u\n", ul->len,
                ipv4_str(ul->src, sb, sizeof(sb)), ul->src_port, ul->dst_port);
    }

    return 0;
}

static int cmd_arp(int argc, char **argv)
{
    if (!net_device()) {
        kprintf("no network device\n");
        return 1;
    }

    /* `arp <address>` asks; a bare `arp` shows the cache. */
    if (argc >= 2) {
        ipv4_addr target;

        if (!ipv4_parse(argv[1], &target)) {
            kprintf("'%s' is not an IPv4 address\n", argv[1]);
            return 1;
        }

        struct mac_addr mac;

        if (arp_resolve(target, &mac)) {
            char mb[MAC_STR_LEN];

            kprintf("already cached: %s\n", mac_str(&mac, mb, sizeof(mb)));
            return 0;
        }

        kprintf("sent a who-has; polling for the reply\n");

        /* Polled rather than blocking on a wait queue, because there is no
         * wait queue - and polling here means `arp` works even if the
         * network thread is not being scheduled. */
        for (u32 i = 0; i < 200; i++) {
            net_poll(8);
            if (arp_resolve(target, &mac)) {
                char mb[MAC_STR_LEN], ib[IPV4_STR_LEN];

                kprintf("%s is at %s\n", ipv4_str(target, ib, sizeof(ib)),
                        mac_str(&mac, mb, sizeof(mb)));
                return 0;
            }
            task_sleep_ms(5);
        }

        kprintf("no reply after 1 second\n");
        return 1;
    }

    u32 n = arp_cache_count();

    if (!n) {
        kprintf("the ARP cache is empty\n");
        return 0;
    }

    kprintf("%-16s %-18s %s\n", "ADDRESS", "MAC", "AGE");
    for (u32 i = 0; i < n; i++) {
        const struct arp_entry *e = arp_cache_at(i);
        char ib[IPV4_STR_LEN], mb[MAC_STR_LEN];

        if (!e)
            break;

        kprintf("%-16s %-18s %llu ms\n", ipv4_str(e->ip, ib, sizeof(ib)),
                mac_str(&e->mac, mb, sizeof(mb)), timer_ms() - e->learned_ms);
    }

    return 0;
}

static int cmd_dhcp(int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);

    if (!net_up()) {
        kprintf("no network, or the link is down\n");
        return 1;
    }

    const struct dhcp_lease *l = dhcp_get_lease();

    if (!dhcp_start()) {
        kprintf("could not start the exchange\n");
        return 1;
    }

    kprintf("DISCOVER sent; waiting for an offer\n");

    /* Polled here rather than left to the network thread, for the same
     * reason `arp` and `ping` are: a command that depends on another thread
     * being scheduled is a command that fails differently under load. */
    for (u32 i = 0;
         i < 3000 && dhcp_state() != DHCP_BOUND && dhcp_state() != DHCP_FAILED;
         i++) {
        net_poll(8);
        task_sleep_ms(5);
    }

    char ab[IPV4_STR_LEN], mb[IPV4_STR_LEN], gb[IPV4_STR_LEN], db[IPV4_STR_LEN],
        sb[IPV4_STR_LEN];

    kprintf("state     : %s\n", dhcp_state_name(dhcp_state()));
    kprintf("messages  : %u discover, %u offer, %u request, %u ack, %u nak, "
            "%u timeout\n",
            l->discovers_sent, l->offers_received, l->requests_sent,
            l->acks_received, l->naks_received, l->timeouts);

    if (dhcp_state() != DHCP_BOUND) {
        kprintf("the configuration was left as it was\n");
        return 1;
    }

    kprintf("address   : %s\n", ipv4_str(l->addr, ab, sizeof(ab)));
    kprintf("netmask   : %s\n", ipv4_str(l->netmask, mb, sizeof(mb)));
    kprintf("gateway   : %s\n", ipv4_str(l->gateway, gb, sizeof(gb)));
    kprintf("dns       : %s\n", ipv4_str(l->dns, db, sizeof(db)));
    kprintf("server    : %s\n", ipv4_str(l->server, sb, sizeof(sb)));
    /* Reported and not acted on: there is no renewal timer. Said here so
     * that a number which looks like a promise is visibly not one. */
    kprintf("lease     : %u second(s) (not renewed; see net/inet.h)\n",
            l->lease_seconds);
    return 0;
}

static int cmd_dns(int argc, char **argv)
{
    /* A bare `dns` shows the resolver and the cache; `dns <name>` asks. */
    if (argc < 2) {
        char sb[IPV4_STR_LEN];
        const struct dns_stats *st = dns_get_stats();

        kprintf("resolver  : %s\n", ipv4_str(dns_server(), sb, sizeof(sb)));
        kprintf("queries   : %u sent, %u reply(s), %u answer(s), "
                "%u not found\n",
                st->queries_sent, st->replies_received, st->answers,
                st->not_found);
        kprintf("failures  : %u timeout(s), %u error(s), %u retry(s)\n",
                st->timeouts, st->errors, st->retries);
        kprintf("cache     : %u entry(s), %u hit(s)\n", dns_cache_count(),
                st->cache_hits);

        for (u32 i = 0; i < dns_cache_count(); i++) {
            char nm[DNS_NAME_MAX], ab[IPV4_STR_LEN];
            ipv4_addr a;

            if (!dns_cache_entry(i, nm, sizeof(nm), &a))
                break;

            kprintf("  %-32s %s\n", nm, ipv4_str(a, ab, sizeof(ab)));
        }

        return 0;
    }

    /* `dns server <address>` sets it, for a machine with no DHCP. */
    if (argc >= 3 && strcmp(argv[1], "server") == 0) {
        ipv4_addr s;

        if (!ipv4_parse(argv[2], &s)) {
            kprintf("'%s' is not an IPv4 address\n", argv[2]);
            return 1;
        }

        dns_set_server(s);
        return 0;
    }

    if (!net_up()) {
        kprintf("no network, or the link is down\n");
        return 1;
    }

    ipv4_addr addr;
    i32 rc = dns_resolve(argv[1], &addr, 5000);

    if (rc < 0) {
        kprintf("%s: %s\n", argv[1], sys_strerror(rc));
        return 1;
    }

    char ab[IPV4_STR_LEN];

    kprintf("%s is %s\n", argv[1], ipv4_str(addr, ab, sizeof(ab)));
    return 0;
}

/* The buffer `fetch` reads a response body into. Static rather than on the
 * stack because a shell command runs on a kernel thread's stack and 16 KiB
 * of it would not fit - the same reason `cat` has a bound. */
static u8 fetch_buf[16 * KIB];

static int cmd_fetch(int argc, char **argv)
{
    if (argc < 2) {
        kprintf("usage: fetch <url> [-h]\n");
        kprintf("  http:// only; there is no TLS. See net/inet.h.\n");
        return 1;
    }

    if (!net_up()) {
        kprintf("no network, or the link is down\n");
        return 1;
    }

    /* `-h` prints only what came back about the response, for a URL whose
     * body is not worth putting on a serial console. */
    bool headers_only = (argc >= 3 && strcmp(argv[2], "-h") == 0);

    char host[HTTP_HOST_MAX], path[HTTP_URL_MAX];
    u16 port;
    bool tls;

    if (!http_parse_url(argv[1], host, sizeof(host), &port, path, sizeof(path),
                        &tls)) {
        if (tls)
            kprintf("https is not supported: there is no TLS in this kernel, "
                    "and a half-checked\ncertificate validator would be worse "
                    "than saying so. See net/inet.h.\n");
        else
            kprintf("'%s' is not a URL this client can use\n", argv[1]);
        return 1;
    }

    kprintf("GET %s from %s port %u\n", path, host, port);

    struct http_response r;
    i32 n = http_get(argv[1], fetch_buf, sizeof(fetch_buf), &r, 10000);

    if (n < 0) {
        kprintf("fetch failed: %s\n", sys_strerror(n));
        return 1;
    }

    kprintf("status    : %d %s\n", r.status, r.reason);
    if (r.content_type[0])
        kprintf("type      : %s\n", r.content_type);
    kprintf("headers   : %u byte(s)\n", r.header_bytes);
    kprintf("body      : %u byte(s)%s%s\n", r.body_bytes,
            r.chunked ? ", chunked" : "",
            r.truncated ? " (truncated to the buffer)" : "");

    if (headers_only || !r.body_bytes)
        return 0;

    kprintf("----\n");

    /* Printed with non-printable bytes shown as dots: a response body is a
     * stranger's bytes, and sending them to a terminal unfiltered lets a
     * remote server move the cursor around. */
    for (u32 i = 0; i < r.body_bytes; i++) {
        u8 c = fetch_buf[i];

        if (c == '\n' || c == '\t' || (c >= 0x20 && c < 0x7F))
            kprintf("%c", (char)c);
        else if (c != '\r')
            kprintf(".");
    }

    if (r.body_bytes && fetch_buf[r.body_bytes - 1] != '\n')
        kprintf("\n");

    kprintf("----\n");
    return 0;
}

static int cmd_ping(int argc, char **argv)
{
    if (argc < 2) {
        kprintf("usage: ping <address> [count]\n");
        return 1;
    }

    if (!net_up()) {
        kprintf("no network, or the link is down\n");
        return 1;
    }

    ipv4_addr target;

    if (!ipv4_parse(argv[1], &target)) {
        kprintf("'%s' is not an IPv4 address\n", argv[1]);
        return 1;
    }

    u32 count = 4;

    if (argc >= 3) {
        u32 v = 0;

        if (str_to_u32(argv[2], &v) && v >= 1 && v <= 20)
            count = v;
    }

    char ib[IPV4_STR_LEN];
    u32 sent = 0, received = 0;

    kprintf("pinging %s, %u time(s), 32 bytes of payload\n",
            ipv4_str(target, ib, sizeof(ib)), count);

    /* The identifier distinguishes our replies from anyone else's on a busy
     * link. Derived from the clock so two pings in the same boot do not
     * collide. */
    u16 id = (u16)(timer_ms() & 0xFFFF);

    for (u32 seq = 1; seq <= count; seq++) {
        icmp_clear_last_reply();

        u64 start = timer_ms();

        if (!icmp_echo_request(target, id, (u16)seq, 32)) {
            /* The common cause is an unresolved ARP entry: the request went
             * out and this datagram was dropped. Saying so is the difference
             * between a confusing first failure and an expected one. */
            kprintf("seq %u: not sent (address not resolved yet?)\n", seq);
            net_poll(8);
            task_sleep_ms(50);
            continue;
        }

        sent++;

        bool got = false;

        for (u32 i = 0; i < 200 && !got; i++) {
            net_poll(8);

            const struct icmp_reply *r = icmp_last_reply();

            if (r->valid && r->id == id && r->seq == seq) {
                kprintf("seq %u: reply from %s in %llu ms\n", seq,
                        ipv4_str(r->from, ib, sizeof(ib)), r->at_ms - start);
                received++;
                got = true;
                break;
            }

            task_sleep_ms(5);
        }

        if (!got)
            kprintf("seq %u: no reply within 1 second\n", seq);
    }

    kprintf("%u sent, %u received, %u lost\n", sent, received, sent - received);
    return received ? 0 : 1;
}

static int cmd_udpsend(int argc, char **argv)
{
    if (argc < 4) {
        kprintf("usage: udpsend <address> <port> <text...>\n");
        return 1;
    }

    ipv4_addr target;

    if (!ipv4_parse(argv[1], &target)) {
        kprintf("'%s' is not an IPv4 address\n", argv[1]);
        return 1;
    }

    u32 port = 0;

    if (!str_to_u32(argv[2], &port) || port == 0 || port > 65535) {
        kprintf("'%s' is not a port number\n", argv[2]);
        return 1;
    }

    /* The remaining arguments, rejoined with spaces. The shell split them,
     * and a datagram of one word is a poor test of a length field. */
    char text[256];
    size_t used = 0;

    for (int i = 3; i < argc && used < sizeof(text) - 1; i++) {
        size_t n = strlen(argv[i]);

        if (used + n + 1 >= sizeof(text))
            n = sizeof(text) - 1 - used;

        memcpy(text + used, argv[i], n);
        used += n;

        if (i + 1 < argc && used < sizeof(text) - 1)
            text[used++] = ' ';
    }
    text[used] = '\0';

    char ib[IPV4_STR_LEN];

    if (!udp_send(target, 12345, (u16)port, text, used)) {
        kprintf("not sent (address not resolved yet? try `arp %s` first)\n",
                argv[1]);
        return 1;
    }

    kprintf("sent %u byte(s) to %s:%u from port 12345\n", (unsigned)used,
            ipv4_str(target, ib, sizeof(ib)), port);

    /* Poll briefly, so that an echo server's reply lands before the prompt
     * comes back and `net` can show it. */
    for (u32 i = 0; i < 100; i++) {
        net_poll(8);
        task_sleep_ms(5);
    }

    return 0;
}

static int cmd_cpus(int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);

    const struct acpi_info *ai = acpi_get_info();
    struct apic_stats as;
    struct smp_stats ss;

    apic_get_stats(&as);
    smp_get_stats(&ss);

    kprintf("Firmware\n");
    if (ai->available)
        kprintf("  ACPI      : %s from \"%s\", %u table(s)%s\n",
                ai->used_xsdt ? "XSDT" : "RSDT", ai->oem_id, ai->tables_seen,
                ai->madt_found ? ", MADT parsed" : ", no MADT");
    else
        kprintf("  ACPI      : no tables on this machine\n");

    if (ai->madt_found) {
        kprintf("  reports   : %u processor(s), %u I/O APIC(s), "
                "%u interrupt override(s)\n",
                ai->cpus_reported, ai->ioapic_count, ai->iso_count);
        for (u32 i = 0; i < ai->iso_count; i++)
            kprintf("              ISA IRQ %u arrives as GSI %u\n",
                    ai->isos[i].source, ai->isos[i].gsi);
    }

    kprintf("Local APIC\n");
    if (apic_available())
        kprintf("  enabled   : id %u, version %02x; %u IPI(s) sent, %u EOI(s), "
                "%u spurious, %u error(s)\n",
                apic_id(), apic_version() & 0xFF, as.ipis_sent, as.eois,
                as.spurious, as.errors);
    else
        kprintf("  absent    : this kernel is running on the 8259s alone\n");

    kprintf("Processors (%u online of %u reported)\n", smp_cpu_count(),
            smp_cpus_present());
    kprintf("  %4s %5s %5s %-9s %10s %8s %8s %12s\n", "CPU", "APIC", "ROLE",
            "STATE", "STACK", "PINGS", "TLB", "IDLE LOOPS");

    for (u32 i = 0; i < SMP_MAX_CPUS; i++) {
        const struct cpu *c = smp_cpu(i);

        if (!c)
            continue;

        kprintf("  %4u %5u %5s %-9s %10p %8u %8u %12llu%s\n", c->index,
                c->apic_id, c->is_bsp ? "bsp" : "ap",
                c->online ? "online" : "OFFLINE", (void *)c->stack_top,
                c->ipi_ping, c->ipi_tlb, c->idle_loops,
                c->index == smp_cpu_index() ? "  <- this one" : "");
    }

    kprintf("  IPIs      : %u ping(s) sent, %u shootdown(s) sent, "
            "%u served, %u start failure(s)\n",
            ss.pings_sent, ss.shootdowns_sent, ss.shootdowns_served,
            ss.start_failures);

    if (!smp_enabled())
        kprintf("  (one processor: the scheduler, the locks and the IPI "
                "paths all still work, there is just nobody to talk to)\n");
    else
        kprintf("  The scheduler runs on cpu %u only; the others service "
                "interrupts. See docs/SMP.md.\n",
                0u);

    return 0;
}

/* Prove the inter-processor interrupt path from the keyboard, rather than
 * inferring it from the fact that nothing has broken. */
static int cmd_ipi(int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);

    if (!smp_enabled()) {
        kprintf("only one processor is online; there is nobody to ping\n");
        return 1;
    }

    u32 before[SMP_MAX_CPUS];

    for (u32 i = 0; i < SMP_MAX_CPUS; i++) {
        const struct cpu *c = smp_cpu(i);

        before[i] = c ? c->ipi_ping : 0;
    }

    kprintf("broadcasting a ping IPI to every other processor...\n");
    smp_ping_others();

    /* The IPI is delivered asynchronously; give the other processors a
     * moment to take it. A sleep rather than a spin, because this task has
     * no business holding a CPU while it waits. */
    task_sleep_ms(50);

    u32 answered = 0;

    for (u32 i = 0; i < SMP_MAX_CPUS; i++) {
        const struct cpu *c = smp_cpu(i);

        if (!c || c->index == smp_cpu_index())
            continue;

        kprintf("  cpu %u: %u -> %u ping(s)%s\n", c->index, before[i],
                c->ipi_ping,
                c->ipi_ping > before[i] ? "" : "   <- no response!");

        if (c->ipi_ping > before[i])
            answered++;
    }

    kprintf("%u of %u other processor(s) answered\n", answered,
            smp_cpu_count() - 1);

    kprintf("\nnow a TLB shootdown, which waits for every processor to "
            "acknowledge:\n");

    struct smp_stats ss_before, ss_after;

    smp_get_stats(&ss_before);
    smp_tlb_shootdown(KERNEL_VIRT_BASE);
    smp_get_stats(&ss_after);

    kprintf("  %u shootdown(s) sent, %u served (was %u)\n",
            ss_after.shootdowns_sent - ss_before.shootdowns_sent,
            ss_after.shootdowns_served - ss_before.shootdowns_served,
            ss_before.shootdowns_served);

    return answered == smp_cpu_count() - 1 ? 0 : 1;
}

/* ---- the filesystem ---------------------------------------------------- */

static int cmd_disk(int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);

    struct ata_stats as;

    ata_get_stats(&as);

    kprintf("ATA drives\n");

    if (ata_drive_count() == 0) {
        kprintf("  none found\n");
    } else {
        kprintf("  %-4s %-26s %12s %7s\n", "DEV", "MODEL", "SECTORS", "ADDR");
        for (u32 i = 0; i < ATA_MAX_DRIVES; i++) {
            const struct ata_drive *d = ata_get_drive(i);

            if (!d)
                continue;

            kprintf("  hd%-2u %-26s %12llu %7s\n", i, d->model, d->sectors,
                    d->lba48 ? "LBA48" : "LBA28");
        }
    }

    kprintf("  %u read(s), %u sector(s), %u command(s), %u error(s), "
            "%u timeout(s)\n",
            as.reads, as.sectors_read, as.commands, as.errors, as.timeouts);

    kprintf("Block devices\n");
    kprintf("  %-8s %-8s %12s %12s\n", "NAME", "TYPE", "FIRST LBA", "SECTORS");

    for (u32 i = 0; i < BLOCKDEV_MAX; i++) {
        const struct blockdev *b = blockdev_get(i);

        if (!b)
            continue;

        char type[8];

        if (b->partition_type == 0)
            strlcpy(type, "disk", sizeof(type));
        else
            ksnprintf(type, sizeof(type), "%02x", b->partition_type);

        kprintf("  %-8s %-8s %12llu %12llu\n", b->name, type, b->first_lba,
                b->sectors);
    }

    return 0;
}

static int cmd_mount(int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);

    if (!fat16_mounted()) {
        kprintf("nothing is mounted\n");
        kprintf("(the GRUB ISO boot path has no FAT partition; the raw disk "
                "image does)\n");
        return 1;
    }

    const struct fat_info *f = fat16_get_info();

    kprintf("FAT16 \"%s\" on %s, mounted at /\n", f->label,
            fat16_device_name());
    kprintf("  geometry  : %u-byte sectors, %u per cluster (%u KiB "
            "clusters)\n",
            f->bytes_per_sector, f->sectors_per_cluster,
            f->cluster_bytes / KIB);
    kprintf("  layout    : %u reserved, FAT at +%u (%u x %u sectors), "
            "root at +%u (%u entries), data at +%u\n",
            f->reserved_sectors, f->fat_start, f->num_fats, f->sectors_per_fat,
            f->root_start, f->root_entries, f->data_start);
    kprintf("  size      : %u sectors, %u clusters\n", f->total_sectors,
            f->cluster_count);
    kprintf("  activity  : %u sector read(s), %u cache hit(s), %u lookup(s), "
            "%u file(s) read whole\n",
            f->reads, f->cache_hits, f->lookups, f->opens);
    return 0;
}

/* The name the stat type goes by when printed. */
static const char *type_name(unsigned type)
{
    switch (type) {
    case S_TYPE_FILE:
        return "file";
    case S_TYPE_DIR:
        return "dir";
    case S_TYPE_CHAR:
        return "char";
    case S_TYPE_PIPE:
        return "pipe";
    case S_TYPE_SOCKET:
        return "socket";
    default:
        return "?";
    }
}

/* `ls` through the VFS rather than through the FAT driver.
 *
 * This is where the abstraction earns its keep visibly: `ls /dev` now works,
 * and it works without this function knowing that /dev is not on a disk.
 * Before, `ls` called fat16_readdir() directly, so the only listable thing in
 * the system was the FAT partition - and on the GRUB ISO path, where nothing
 * is mounted, there was nothing to list at all.
 *
 * It also costs something, and the cost is worth naming: the old version
 * printed FAT's attribute bits (read-only, hidden, system), and
 * `struct sys_dirent` has no room for them because they are not a property
 * every filesystem has. The type and the size are what all three backends can
 * answer, so that is what is printed. `mount` still reports FAT-specific
 * detail, which is the right place for it.
 */
static int cmd_ls(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : ".";
    char abs[VFS_PATH_MAX];

    if (!fd_resolve(path, abs, sizeof(abs))) {
        kprintf("%s: path too long\n", path);
        return 1;
    }

    i32 fd = fd_open(abs, O_RDONLY);

    if (fd < 0) {
        kprintf("%s: %s\n", abs, sys_strerror(fd));
        return 1;
    }

    struct sys_stat st;

    if (fd_stat(fd, &st) < 0) {
        kprintf("%s: cannot stat\n", abs);
        fd_close(fd);
        return 1;
    }

    if (st.type != S_TYPE_DIR) {
        kprintf("  %-14s %8u  %s\n", abs, st.size, type_name(st.type));
        fd_close(fd);
        return 0;
    }

    kprintf("  %-14s %8s  %s\n", "NAME", "SIZE", "TYPE");

    u32 files = 0, dirs = 0, others = 0, bytes = 0;
    struct sys_dirent de;

    for (u32 i = 0; i < 4096; i++) {
        i32 got = fd_getdent(fd, i, &de);

        if (got <= 0)
            break;

        kprintf("  %-14s %8u  %s\n", de.name, de.size, type_name(de.type));

        switch (de.type) {
        case S_TYPE_DIR:
            dirs++;
            break;
        case S_TYPE_FILE:
            files++;
            bytes += de.size;
            break;
        default:
            others++;
            break;
        }
    }

    fd_close(fd);

    kprintf("  %u file(s), %u bytes; %u director(y|ies)", files, bytes, dirs);
    if (others)
        kprintf("; %u device(s)", others);
    kprintf("\n");

    return 0;
}

/* ---- writing ------------------------------------------------------------
 *
 * These exist so that the write path can be driven by hand and, more
 * importantly, by the test harness over the serial console - which is how the
 * `filesystem-durability` scenario creates a file in one boot and another
 * tool checks the disk image afterwards. A write path verified only by the
 * kernel's own assertions is a write path verified by the thing under test.
 */
static int cmd_write(int argc, char **argv)
{
    if (argc < 3) {
        kprintf("usage: write <path> <text...>\n");
        kprintf("       appends a line to a file, creating it if needed\n");
        return 1;
    }

    char abs[VFS_PATH_MAX];

    if (!fd_resolve(argv[1], abs, sizeof(abs))) {
        kprintf("%s: path too long\n", argv[1]);
        return 1;
    }

    i32 fd = fd_open(abs, O_WRONLY | O_CREAT | O_APPEND);

    if (fd < 0) {
        kprintf("%s: %s\n", abs, sys_strerror(fd));
        return 1;
    }

    u32 total = 0;

    for (int i = 2; i < argc; i++) {
        const char *word = argv[i];
        u32 len = (u32)strlen(word);
        i32 wrote = fd_write(fd, word, len);

        if (wrote < 0) {
            kprintf("%s: %s\n", abs, sys_strerror(wrote));
            fd_close(fd);
            return 1;
        }

        total += (u32)wrote;

        /* A separator between words, and a newline at the end, so that the
         * file holds the line the user typed rather than its words run
         * together. */
        const char *tail = (i + 1 < argc) ? " " : "\n";
        i32 n = fd_write(fd, tail, 1);

        if (n > 0)
            total += (u32)n;
    }

    if (fd_close(fd) < 0) {
        kprintf("%s: close failed\n", abs);
        return 1;
    }

    kprintf("wrote %u byte(s) to %s\n", total, abs);
    return 0;
}

/* One helper for the four commands that take a path and either work or do
 * not. Each is two lines of its own, so the table below stays readable. */
static int path_command(int argc, char **argv, const char *usage,
                        i32 (*op)(const char *), const char *verb)
{
    if (argc < 2) {
        kprintf("usage: %s\n", usage);
        return 1;
    }

    char abs[VFS_PATH_MAX];

    if (!fd_resolve(argv[1], abs, sizeof(abs))) {
        kprintf("%s: path too long\n", argv[1]);
        return 1;
    }

    i32 e = op(abs);

    if (e < 0) {
        kprintf("%s: %s\n", abs, sys_strerror(e));
        return 1;
    }

    kprintf("%s %s\n", verb, abs);
    return 0;
}

static int cmd_rm(int argc, char **argv)
{
    return path_command(argc, argv, "rm <path>", vfs_unlink, "removed");
}

static int cmd_mkdir(int argc, char **argv)
{
    return path_command(argc, argv, "mkdir <path>", vfs_mkdir, "created");
}

static int cmd_rmdir(int argc, char **argv)
{
    return path_command(argc, argv, "rmdir <path>", vfs_rmdir, "removed");
}

static int cmd_touch(int argc, char **argv)
{
    if (argc < 2) {
        kprintf("usage: touch <path>\n");
        return 1;
    }

    char abs[VFS_PATH_MAX];

    if (!fd_resolve(argv[1], abs, sizeof(abs))) {
        kprintf("%s: path too long\n", argv[1]);
        return 1;
    }

    /* Not O_EXCL: touching a file that exists is a no-op, not an error. */
    i32 fd = fd_open(abs, O_WRONLY | O_CREAT);

    if (fd < 0) {
        kprintf("%s: %s\n", abs, sys_strerror(fd));
        return 1;
    }

    fd_close(fd);
    kprintf("touched %s\n", abs);
    return 0;
}

static int cmd_mv(int argc, char **argv)
{
    if (argc < 3) {
        kprintf("usage: mv <from> <to>\n");
        return 1;
    }

    char from[VFS_PATH_MAX];
    char to[VFS_PATH_MAX];

    if (!fd_resolve(argv[1], from, sizeof(from)) ||
        !fd_resolve(argv[2], to, sizeof(to))) {
        kprintf("path too long\n");
        return 1;
    }

    i32 e = vfs_rename(from, to);

    if (e < 0) {
        kprintf("%s -> %s: %s\n", from, to, sys_strerror(e));
        return 1;
    }

    kprintf("renamed %s to %s\n", from, to);
    return 0;
}

static int cmd_cp(int argc, char **argv)
{
    if (argc < 3) {
        kprintf("usage: cp <from> <to>\n");
        return 1;
    }

    char from[VFS_PATH_MAX];
    char to[VFS_PATH_MAX];

    if (!fd_resolve(argv[1], from, sizeof(from)) ||
        !fd_resolve(argv[2], to, sizeof(to))) {
        kprintf("path too long\n");
        return 1;
    }

    i32 in = fd_open(from, O_RDONLY);

    if (in < 0) {
        kprintf("%s: %s\n", from, sys_strerror(in));
        return 1;
    }

    /* O_EXCL: a copy that silently overwrote its destination would be a
     * destructive command that looks harmless. */
    i32 out = fd_open(to, O_WRONLY | O_CREAT | O_EXCL | O_TRUNC);

    if (out < 0) {
        kprintf("%s: %s\n", to, sys_strerror(out));
        fd_close(in);
        return 1;
    }

    char buf[512];
    u32 total = 0;
    bool ok = true;

    for (;;) {
        i32 got = fd_read(in, buf, sizeof(buf));

        if (got < 0) {
            kprintf("%s: %s\n", from, sys_strerror(got));
            ok = false;
            break;
        }

        if (got == 0)
            break;

        i32 put = fd_write(out, buf, (u32)got);

        if (put < got) {
            kprintf("%s: %s\n", to,
                    put < 0 ? sys_strerror(put) : "short write (disk full?)");
            ok = false;
            break;
        }

        total += (u32)put;
    }

    fd_close(in);
    fd_close(out);

    if (!ok)
        return 1;

    kprintf("copied %u byte(s) to %s\n", total, to);
    return 0;
}

static int cmd_sync(int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);

    i32 e = vfs_sync();

    if (e < 0) {
        kprintf("sync: %s\n", sys_strerror(e));
        return 1;
    }

    kprintf("sync: every filesystem flushed\n");
    return 0;
}

static int cmd_df(int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);

    if (!fat16_mounted()) {
        kprintf("nothing is mounted\n");
        return 1;
    }

    u32 free_clusters = 0, total_clusters = 0;

    if (!fat16_space(&free_clusters, &total_clusters)) {
        kprintf("df: cannot read the allocation table\n");
        return 1;
    }

    const struct fat_info *f = fat16_get_info();
    u32 kib = f->cluster_bytes / 1024;

    kprintf("  %-10s %10s %10s %10s %5s  %s\n", "FILESYSTEM", "SIZE", "USED",
            "FREE", "USE%", "MOUNT");
    kprintf("  %-10s %9uK %9uK %9uK %4u%%  %s\n", "fat16", total_clusters * kib,
            (total_clusters - free_clusters) * kib, free_clusters * kib,
            total_clusters
                ? (total_clusters - free_clusters) * 100 / total_clusters
                : 0,
            "/");
    kprintf("  counted by scanning the allocation table: %u of %u clusters "
            "free, %u bytes each\n",
            free_clusters, total_clusters, f->cluster_bytes);

    u32 writes = 0, sectors = 0, allocated = 0, freed = 0, dirents = 0;

    fat16_write_stats(&writes, &sectors, &allocated, &freed, &dirents);
    kprintf("  writes    : %u call(s), %u sector(s) written\n", writes,
            sectors);
    kprintf("  clusters  : %u allocated, %u freed\n", allocated, freed);
    kprintf("  dirents   : %u update(s)\n", dirents);

    return 0;
}

static int cmd_pwd(int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);

    kprintf("%s\n", fd_cwd());
    return 0;
}

static int cmd_cd(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "/";
    i32 e = fd_chdir(path);

    if (e < 0) {
        kprintf("%s: %s\n", path, sys_strerror(e));
        return 1;
    }

    return 0;
}

/* What the kernel's own descriptor table and the open-file pool look like.
 * The pool is a fixed global array, so "how full is it" is a question with a
 * real answer, and a leak shows up here as a number that only goes up. */
static int cmd_fds(int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);

    kprintf("open files : %u of %u in use, peak %u\n", vfs_open_count(),
            VFS_OPEN_MAX, vfs_open_peak());
    kprintf("vfs calls  : %u\n", vfs_call_count());
    kprintf("cwd        : %s\n", fd_cwd());

    u32 reads = 0, in = 0, out = 0;

    devfs_counters(&reads, &in, &out);
    kprintf("console    : %u read(s), %u byte(s) in, %u byte(s) out\n", reads,
            in, out);

    u32 opens = 0, freads = 0, fbytes = 0;

    fatfs_counters(&opens, &freads, &fbytes);
    kprintf("fat16      : %u open(s), %u read(s), %u byte(s)\n", opens, freads,
            fbytes);

    kprintf("\n  %-4s %-10s %-6s %8s  %s\n", "FD", "BACKEND", "TYPE", "OFFSET",
            "PATH");

    u32 shown = 0;

    for (i32 fd = 0; fd < (i32)SYS_FDS_MAX; fd++) {
        struct vfs_file *f = fd_get(fd);

        if (!f)
            continue;

        kprintf("  %-4d %-10s %-6s %8u  %s\n", fd,
                f->ops && f->ops->name ? f->ops->name : "?", type_name(f->type),
                f->offset, f->path);
        shown++;
    }

    if (!shown)
        kprintf("  (this task holds none - the shell is a kernel thread and "
                "uses kprintf)\n");

    return 0;
}

/* `cat` through the VFS too, which means `cat /dev/zero` and `cat
 * /etc/motd.txt` are the same code path. Reads in chunks rather than whole,
 * so that a large file does not need a buffer the size of the file - and so
 * the incremental read path, the one exec() does not use, is exercised by
 * hand as well as by the test suite.
 *
 * A character device has no size and no end, so the byte budget is what stops
 * `cat /dev/zero` from running until the machine is rebooted. A real `cat`
 * would stream forever and rely on the user interrupting it; there is no
 * interrupt key wired to a kernel thread here, so the limit is explicit.
 */
#define CAT_MAX_BYTES (64 * KIB)

static int cmd_cat(int argc, char **argv)
{
    if (argc < 2) {
        kprintf("usage: cat <path>\n");
        return 1;
    }

    char abs[VFS_PATH_MAX];

    if (!fd_resolve(argv[1], abs, sizeof(abs))) {
        kprintf("%s: path too long\n", argv[1]);
        return 1;
    }

    i32 fd = fd_open(abs, O_RDONLY);

    if (fd < 0) {
        kprintf("%s: %s\n", abs, sys_strerror(fd));
        return 1;
    }

    struct sys_stat st;

    if (fd_stat(fd, &st) == 0 && st.type == S_TYPE_DIR) {
        kprintf("%s: %s\n", abs, sys_strerror(SYS_EISDIR));
        fd_close(fd);
        return 1;
    }

    char buf[256];
    u32 total = 0;
    u32 printable = 0, binary = 0;
    bool truncated = false;

    for (;;) {
        u32 want = sizeof(buf);

        if (total + want > CAT_MAX_BYTES)
            want = CAT_MAX_BYTES - total;

        if (want == 0) {
            truncated = true;
            break;
        }

        i32 got = fd_read(fd, buf, want);

        if (got < 0) {
            kprintf("\n%s: %s\n", abs, sys_strerror(got));
            fd_close(fd);
            return 1;
        }

        if (got == 0)
            break;

        for (i32 i = 0; i < got; i++) {
            char c = buf[i];

            if (c == '\n' || c == '\t' || (c >= 0x20 && c < 0x7F)) {
                console_putc(c);
                printable++;
            } else if (c == '\r') {
                /* The files on this image have CRLF line endings, because
                 * they were written by a tool on a machine that does. */
            } else {
                binary++;
            }
        }

        total += (u32)got;
    }

    fd_close(fd);

    if (truncated)
        kprintf("\n[stopped after %u bytes]\n", total);
    else if (binary)
        kprintf("\n[%u of %u bytes were not printable]\n", binary, total);
    else if (printable == 0)
        kprintf("[%s is empty]\n", abs);

    return 0;
}

/* exec()'s namespace. There is no filesystem yet, so the programs a process
 * can exec into are the ones embedded in the kernel image; printing them is
 * how you find out what `exec` will accept. */
static int cmd_programs(int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);

    kprintf("Programs embedded in the kernel image (exec's namespace):\n");

    for (u32 i = 0;; i++) {
        const char *name = usermode_program_name(i);

        if (!name)
            break;

        kprintf("  %s%s\n", name,
                strcmp(name, "init") == 0 ? "   (started at boot)" : "");
    }

    if (fat16_mounted()) {
        kprintf("Programs on the filesystem (what exec() prefers):\n");
        struct fat_dirent entry;
        bool any = false;

        for (u32 i = 0;; i++) {
            if (!fat16_readdir("/bin", i, &entry))
                break;
            if (entry.is_dir)
                continue;
            kprintf("  /bin/%-12s %8u bytes\n", entry.name, entry.size);
            any = true;
        }

        if (!any)
            kprintf("  (/bin is empty or missing)\n");
    } else {
        kprintf("No filesystem is mounted, so the embedded copies are all "
                "there is.\n");
    }

    u32 from_disk = 0, embedded = 0;

    usermode_get_load_counts(&from_disk, &embedded);
    kprintf("  %u exec() call(s) this boot; %u image(s) loaded from disk, "
            "%u from the kernel image\n",
            usermode_exec_count(), from_disk, embedded);
    return 0;
}

/* Run the ring-3 syscall fuzzer. It is a program like any other, so this
 * just execs it into a child - which is also a demonstration that the fuzzer
 * needs no privilege the shell does not already have. */
static int cmd_fuzz(int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);

    u32 pid = usermode_spawn_named("fuzz");

    if (!pid) {
        kprintf("could not start the syscall fuzzer\n");
        return 1;
    }

    kprintf("syscall fuzzer started as pid %u; its output follows\n", pid);

    /* Wait for that process specifically. The shell may have other
     * collectable children - `stress` and `ring3` both leave some - so a bare
     * wait() could return one of those instead.
     *
     * The bound is on how many *other* children get collected first, not on
     * time: task_wait() blocks, so each iteration makes progress. 64 is more
     * children than any shell command creates, and reaching it would mean
     * wait() is returning pids nobody asked for. */
    for (u32 others = 0; others < 64; others++) {
        int status = 0;
        int got = task_wait(&status);

        if (got < 0) {
            kprintf("the fuzzer vanished without exiting\n");
            return 1;
        }

        if ((u32)got != pid)
            continue; /* someone else's zombie; keep waiting */

        kprintf("pid %u exited with %d\n", pid, status);
        return status == 0 ? 0 : 1;
    }

    kprintf("gave up waiting for pid %u after collecting 64 other children\n",
            pid);
    return 1;
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
static volatile u32 fault_text_addr;
static volatile u32 fault_guard_addr;

/* Both operands live in volatile storage. With a literal numerator GCC proves
 * the division is undefined and emits `ud2` instead of `idiv`, so the #DE this
 * command exists to show never happens. */
static volatile int fault_dividend = 1;
static volatile int fault_divisor; /* left zero */

/* What is actually switched on, as opposed to what the README claims. Every
 * line is read back from the hardware or the page tables rather than from a
 * flag the kernel set earlier. */
static int cmd_harden(int argc, char **argv)
{
    UNUSED(argc);
    UNUSED(argv);

    const struct harden_state *h = harden_get_state();

    kprintf("Kernel/user separation\n");
    kprintf("  null page       : %s\n",
            vmm_translate(0, NULL) ? "MAPPED - a NULL dereference would not "
                                     "fault!"
                                   : "unmapped");
    kprintf("  CR0.WP          : %s\n",
            (read_cr0() & (1u << 16))
                ? "set - ring 0 honours read-only pages"
                : "CLEAR - read-only kernel pages are advisory!");
    kprintf("  kernel .text    : %s\n",
            h->kernel_text_ro ? "read-only" : "WRITABLE");
    kprintf("  kernel .rodata  : %s\n",
            h->kernel_text_ro ? "read-only" : "WRITABLE");

    kprintf("  SMEP (CR4.20)   : %s\n",
            h->smep_enabled     ? "enabled - ring 0 cannot execute user pages"
            : h->smep_available ? "available but not enabled"
                                : "not supported by this CPU");
    kprintf("  SMAP (CR4.21)   : %s\n",
            h->smap_enabled     ? "enabled - ring 0 cannot touch user pages "
                                  "without EFLAGS.AC"
            : h->smap_available ? "available but not enabled"
                                : "not supported by this CPU");
    if (h->smap_enabled)
        kprintf("  declared windows: %u user accesses so far\n",
                h->user_access_windows);

    kprintf("Stacks\n");
    kprintf("  task stacks at  : %p, %u KiB each\n", (void *)KSTACK_BASE,
            (unsigned)(TASK_STACK_SIZE / KIB));
    kprintf("  guard pages     : %s\n",
            h->stack_guard_pages ? "one unmapped page below every stack"
                                 : "none");
    kprintf("  canaries        : %s (%u failures)\n",
            h->stack_canaries ? "checked on every context switch" : "none",
            sched_canary_failures());

    /* Prove the guard page rather than asserting it: read the page below
     * this task's own stack out of the page tables. */
    vaddr_t guard = (vaddr_t)task_current()->stack_base - PAGE_SIZE;
    kprintf("  this task's guard page at %p is %s\n", (void *)guard,
            vmm_translate(guard, NULL) ? "MAPPED - the guard is gone!"
                                       : "unmapped, as it should be");

    kprintf("Address spaces\n");
    kprintf("  kernel half     : all %u directory slots pre-backed, so "
            "every\n",
            1023u - KERNEL_PDE_FIRST);
    kprintf("                    address space sees identical kernel "
            "mappings\n");

    kprintf("\nNot yet implemented: NX (needs PAE), KASLR. See "
            "docs/SECURITY.md.\n");
    return 0;
}

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
    } else if (strcmp(what, "text") == 0) {
        /* The negative half of the W^X check. The `harden` suite asserts the
         * PTEs have no write bit; this asserts the CPU agrees. */
        kprintf("writing to the kernel's own .text from ring 0...\n");
        fault_text_addr = (u32)__text_start;
        volatile u32 *p = (volatile u32 *)fault_text_addr;
        *p = 0x90909090;
    } else if (strcmp(what, "stackguard") == 0) {
        /* The negative half of the guard-page check. Writing below this
         * task's own stack must land on the unmapped guard page. Done by
         * address rather than by recursing, so the fault is at a known place
         * and the diagnosis is checkable. */
        kprintf("writing below this task's kernel stack...\n");
        fault_guard_addr = (u32)task_current()->stack_base - 16;
        volatile u32 *p = (volatile u32 *)fault_guard_addr;
        *p = 0xDEADBEEF;
    } else if (strcmp(what, "panic") == 0) {
        panic("deliberate panic requested from the shell");
    } else {
        kprintf("usage: fault "
                "<null|unmapped|readonly|text|stackguard|div0|ud|panic>\n");
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
    {"fuzz", "fuzz", "attack the syscall boundary from ring 3", cmd_fuzz},
    {"programs", "programs", "list the programs exec() can run", cmd_programs},
    {"cpus", "cpus", "processors, ACPI and the local APIC", cmd_cpus},
    {"net", "net", "the Ethernet controller, addressing and per-layer counters",
     cmd_net},
    {"arp", "arp [address]", "show the ARP cache, or resolve an address",
     cmd_arp},
    {"ping", "ping <address> [count]", "ICMP echo, and wait for the reply",
     cmd_ping},
    {"udpsend", "udpsend <address> <port> <text...>", "send one UDP datagram",
     cmd_udpsend},
    {"dhcp", "dhcp", "ask a DHCP server for an address", cmd_dhcp},
    {"dns", "dns [name|server <address>]",
     "resolve a name, or show the "
     "resolver and its cache",
     cmd_dns},
    {"fetch", "fetch <url> [-h]", "fetch an http:// URL over TCP", cmd_fetch},
    {"longmode", "longmode", "enter 64-bit long mode, prove it, and return",
     cmd_longmode},
    {"ipi", "ipi", "ping every other processor and time a TLB shootdown",
     cmd_ipi},
    {"disk", "disk", "ATA drives and block devices", cmd_disk},
    {"mount", "mount", "the mounted filesystem's geometry and activity",
     cmd_mount},
    {"ls", "ls [path]", "list a directory", cmd_ls},
    {"cat", "cat <path>", "print a file or device", cmd_cat},
    {"cd", "cd [path]", "change the working directory", cmd_cd},
    {"write", "write <path> <text...>", "append a line to a file", cmd_write},
    {"touch", "touch <path>", "create an empty file", cmd_touch},
    {"rm", "rm <path>", "remove a file", cmd_rm},
    {"mkdir", "mkdir <path>", "create a directory", cmd_mkdir},
    {"rmdir", "rmdir <path>", "remove an empty directory", cmd_rmdir},
    {"mv", "mv <from> <to>", "rename a file or directory", cmd_mv},
    {"cp", "cp <from> <to>", "copy a file", cmd_cp},
    {"sync", "sync", "flush every filesystem to its device", cmd_sync},
    {"df", "df", "free space, and what writing has done", cmd_df},
    {"pwd", "pwd", "print the working directory", cmd_pwd},
    {"fds", "fds",
     "open files, the descriptor pool and per-backend "
     "counters",
     cmd_fds},
    {"stress", "stress [workers] [rounds]",
     "hammer the heap from several tasks", cmd_stress},
    {"harden", "harden", "report the mitigations that are switched on",
     cmd_harden},
    {"fault", "fault <null|unmapped|readonly|text|stackguard|div0|ud|panic>",
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
