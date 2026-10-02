; =============================================================================
; StratumOS - Stage 2 Bootloader
; =============================================================================
;
; Stage 1 freed us from the 512-byte cage. Stage 2 does the real work of
; turning a BIOS machine into an environment a 32-bit C kernel can run in:
;
;   1. Enable the A20 gate            - without it address bit 20 is forced
;                                       to 0 and every access above 1 MiB
;                                       silently wraps into low memory.
;   2. Query the BIOS memory map      - INT 15h AX=E820, with E801/88h
;                                       fallbacks. The kernel's physical
;                                       frame allocator is built from this.
;   3. Load the kernel ELF from disk  - LBA reads when the BIOS supports
;                                       them, true LBA->CHS translation when
;                                       it does not.
;   4. Enter 32-bit protected mode    - flat 4 GiB GDT, far jump to reload CS.
;   5. Load the ELF program headers   - copy each PT_LOAD segment to its
;                                       physical address and zero the BSS
;                                       tail, exactly as a Multiboot loader
;                                       would. This is *why* the same kernel
;                                       binary boots from GRUB and from here.
;   6. Jump to e_entry with EAX = boot magic, EBX = boot info pointer.
;
; Physical memory layout used by stage 2
; --------------------------------------
;   0x00004000   boot info structure handed to the kernel
;   0x00005000   E820 entry array (24 bytes each, up to 48 entries)
;   0x00007E00   stage 2 image (this code), 12 KiB reserved
;   0x00020000   kernel ELF staging buffer (read straight off disk)
;   0x00090000   protected-mode stack top
;   0x00100000   final kernel load address (per ELF program headers)
; =============================================================================

                bits    16
                org     0x7E00

%define BOOTINFO_ADDR       0x4000
%define E820_BUF            0x5000
%define E820_MAX            48
%define KERNEL_STAGE        0x20000         ; linear staging address
%define KERNEL_STAGE_SEG    0x2000          ; ...as a real-mode segment
%define PM_STACK_TOP        0x90000
%define STRATUM_MAGIC       0x53545241      ; 'STRA'
%define CODE_SEL            0x08
%define DATA_SEL            0x10
%define CHUNK_SECTORS       64              ; 32 KiB per extended read
%define STAGE2_SECTORS      24
%define CMDLINE_MAX         96              ; patched in place by mkimage.py

; Boot info field offsets - mirrored by struct stratum_boot_info in C.
%define BI_MAGIC            0
%define BI_VERSION          4
%define BI_FLAGS            8
%define BI_E820_COUNT       12
%define BI_E820_ADDR        16
%define BI_BOOT_DRIVE       20
%define BI_KERNEL_STAGE     24
%define BI_KERNEL_SECS      28
%define BI_LOADER_NAME      32
%define BI_CMDLINE          36
%define BI_MEM_LOWER        40
%define BI_MEM_UPPER        44
%define BI_SIZE             48

%define BI_FLAG_E820        0x01
%define BI_FLAG_E801        0x02
%define BI_FLAG_LBA         0x04

; -----------------------------------------------------------------------------
; Header. Stage 1 verifies the magic at offset 0 and jumps to the entry point
; at offset 112. Everything before that is data patched in by
; tools/mkimage.py once the kernel's real size is known - the assembler has no
; way to know how big the kernel will be, and the command line is a build-time
; choice rather than a source-code one.
;
; Layout (offsets are from the start of the loaded stage 2, i.e. 0x7E00):
;    +0    magic 'S2OS'
;    +4    kernel LBA
;    +8    kernel length in sectors
;    +12   reserved
;    +16   command line, NUL-terminated, CMDLINE_MAX bytes
;    +112  entry point
;
; STAGE2_ENTRY_OFF in boot/stage1.asm must match the entry offset below.
; -----------------------------------------------------------------------------
header:
                db      'S2OS'                  ; +0
hdr_kernel_lba: dd      STAGE2_SECTORS + 1      ; +4   patched by mkimage
hdr_kernel_secs:dd      0                       ; +8   patched by mkimage
                dd      0                       ; +12  reserved
cmdline:        db      'console=vga,serial'    ; +16  patched by mkimage
                times   CMDLINE_MAX - ($ - cmdline) db 0
; ---- offset 112 -------------------------------------------------------------
%if ($ - $$) != 112
  %error "stage2 entry point is not at offset 112; update stage1.asm to match"
%endif
entry:
                cli
                xor     ax, ax
                mov     ds, ax
                mov     es, ax
                mov     ss, ax
                mov     sp, 0x7C00
                cld
                sti

                mov     [boot_drive], dl

                call    ser_init
                mov     si, msg_banner
                call    puts

                call    a20_enable
                test    ax, ax
                jnz     .a20_ok
                mov     si, msg_a20_fail
                jmp     fatal
.a20_ok:
                mov     si, msg_a20
                call    puts

                call    detect_memory
                mov     si, msg_mmap
                call    puts

                call    disk_probe
                call    load_kernel

                mov     si, msg_loaded
                call    puts

                call    build_bootinfo

                mov     si, msg_pmode
                call    puts

                cli
                lgdt    [gdt_descriptor]
                mov     eax, cr0
                or      eax, 1                  ; CR0.PE
                mov     cr0, eax
                jmp     CODE_SEL:pm_entry       ; far jump reloads CS

; =============================================================================
; A20 gate
; =============================================================================
; Three escalating methods, each verified by a wraparound test.
; Returns AX=1 on success, AX=0 if the line is still stuck.
; -----------------------------------------------------------------------------
a20_enable:
                call    a20_test
                test    ax, ax
                jnz     .ok

                ; --- Method 1: ask the BIOS --------------------------------
                mov     ax, 0x2401
                int     0x15
                call    a20_test
                test    ax, ax
                jnz     .ok

                ; --- Method 2: 8042 keyboard controller output port --------
                call    kbc_wait_in
                mov     al, 0xAD                ; disable keyboard
                out     0x64, al
                call    kbc_wait_in
                mov     al, 0xD0                ; read output port
                out     0x64, al
                call    kbc_wait_out
                in      al, 0x60
                mov     bl, al
                call    kbc_wait_in
                mov     al, 0xD1                ; write output port
                out     0x64, al
                call    kbc_wait_in
                mov     al, bl
                or      al, 2                   ; set A20 enable bit
                out     0x60, al
                call    kbc_wait_in
                mov     al, 0xAE                ; re-enable keyboard
                out     0x64, al
                call    kbc_wait_in
                call    a20_test
                test    ax, ax
                jnz     .ok

                ; --- Method 3: "fast A20" via System Control Port A --------
                in      al, 0x92
                test    al, 2
                jnz     .fast_done
                or      al, 2
                and     al, 0xFE                ; bit 0 would trigger a reset
                out     0x92, al
.fast_done:
                call    a20_test
.ok:
                ret

; Compare 0000:0500 against FFFF:0510. Those are the same linear byte when
; A20 is forced low, so a write to one shows up in the other.
; Returns AX=1 (enabled) or AX=0 (wrapping).
a20_test:
                pushf
                push    ds
                push    es
                push    di
                push    si
                cli

                xor     ax, ax
                mov     es, ax
                mov     di, 0x0500

                mov     ax, 0xFFFF
                mov     ds, ax
                mov     si, 0x0510

                mov     al, [es:di]             ; save both bytes
                push    ax
                mov     al, [ds:si]
                push    ax

                mov     byte [es:di], 0x00
                mov     byte [ds:si], 0xFF
                cmp     byte [es:di], 0xFF      ; did the far write alias?

                pop     ax
                mov     [ds:si], al             ; restore originals
                pop     ax
                mov     [es:di], al

                mov     ax, 0
                je      .done                   ; aliased -> A20 is off
                mov     ax, 1
.done:
                pop     si
                pop     di
                pop     es
                pop     ds
                popf
                ret

kbc_wait_in:                                    ; input buffer empty?
                in      al, 0x64
                test    al, 2
                jnz     kbc_wait_in
                ret

kbc_wait_out:                                   ; output buffer full?
                in      al, 0x64
                test    al, 1
                jz      kbc_wait_out
                ret

; =============================================================================
; Memory detection
; =============================================================================
detect_memory:
                call    e820_scan
                call    e801_scan
                ret

; INT 15h AX=E820 - the authoritative map. Each entry is 24 bytes:
;   qword base, qword length, dword type, dword ACPI 3.0 extended attributes.
e820_scan:
                push    es
                xor     ax, ax
                mov     es, ax
                mov     di, E820_BUF
                xor     ebx, ebx
                xor     bp, bp
.next:
                mov     eax, 0xE820
                mov     edx, 0x534D4150         ; 'SMAP'
                mov     ecx, 24
                mov     dword [es:di + 20], 1   ; assume valid if BIOS is 20-byte
                int     0x15
                jc      .done                   ; CF on the first call = no E820
                cmp     eax, 0x534D4150
                jne     .done
                jcxz    .skip
                cmp     cl, 20
                jb      .skip

                mov     eax, [es:di + 8]        ; drop zero-length regions
                or      eax, [es:di + 12]
                jz      .skip

                cmp     cl, 24                  ; honour ACPI 3.0 "ignore" bit
                jb      .keep
                test    byte [es:di + 20], 1
                jz      .skip
.keep:
                inc     bp
                add     di, 24
                cmp     bp, E820_MAX
                jae     .done
.skip:
                test    ebx, ebx                ; EBX=0 means that was the last
                jnz     .next
.done:
                mov     [e820_count], bp
                test    bp, bp
                jz      .no_entries
                or      byte [bi_flags], BI_FLAG_E820
.no_entries:
                pop     es
                ret

; INT 15h AX=E801 / AH=88h - coarse low/high memory sizes. Kept as a sanity
; cross-check, and as the only option on pre-E820 BIOSes.
e801_scan:
                xor     ax, ax
                mov     [mem_lower], ax
                mov     [mem_upper], ax

                mov     ax, 0xE801
                xor     bx, bx
                xor     cx, cx
                xor     dx, dx
                int     0x15
                jc      .try_88

                test    cx, cx                  ; some BIOSes answer in AX/BX
                jnz     .use_cx
                mov     cx, ax
                mov     dx, bx
.use_cx:
                mov     [mem_lower], cx         ; KiB between 1 MiB and 16 MiB
                mov     [mem_upper], dx         ; 64 KiB blocks above 16 MiB
                or      byte [bi_flags], BI_FLAG_E801
                ret
.try_88:
                mov     ah, 0x88
                int     0x15
                jc      .fail
                mov     [mem_lower], ax
                or      byte [bi_flags], BI_FLAG_E801
.fail:
                ret

; =============================================================================
; Disk access
; =============================================================================
; disk_probe - pick extended (LBA) reads if available, otherwise cache the
; drive geometry for LBA->CHS translation.
; -----------------------------------------------------------------------------
disk_probe:
                mov     ah, 0x41
                mov     bx, 0x55AA
                mov     dl, [boot_drive]
                int     0x13
                jc      .no_lba
                cmp     bx, 0xAA55
                jne     .no_lba
                test    cl, 1                   ; packet access subset
                jz      .no_lba
                mov     byte [use_lba], 1
                or      byte [bi_flags], BI_FLAG_LBA
                ret
.no_lba:
                mov     byte [use_lba], 0
                ; INT 13h AH=08h: CL[5:0] = sectors/track, DH = max head.
                push    es
                mov     ah, 0x08
                mov     dl, [boot_drive]
                xor     di, di
                mov     es, di                  ; guard against buggy BIOSes
                int     0x13
                pop     es
                jc      .defaults
                and     cl, 0x3F
                jz      .defaults
                mov     [spt], cl
                movzx   ax, dh
                inc     ax                      ; heads = max_head + 1
                mov     [heads], al
                ret
.defaults:
                mov     byte [spt], 18          ; 1.44 MiB floppy geometry
                mov     byte [heads], 2
                ret

; -----------------------------------------------------------------------------
; disk_read - read CX sectors from LBA EAX into ES:0000.
; Returns CF set on failure. Clobbers caller-saved scratch registers.
; -----------------------------------------------------------------------------
disk_read:
                cmp     byte [use_lba], 1
                je      disk_read_lba
                jmp     disk_read_chs

disk_read_lba:
                mov     [dap_lba], eax
                mov     dword [dap_lba + 4], 0
                mov     [dap_count], cx
                mov     word [dap_off], 0
                mov     [dap_seg], es

                mov     cx, 4                   ; retries
.attempt:
                push    cx
                mov     si, dap
                mov     dl, [boot_drive]
                mov     ah, 0x42
                int     0x13
                pop     cx
                jnc     .ok
                xor     ah, ah                  ; reset controller, try again
                mov     dl, [boot_drive]
                int     0x13
                loop    .attempt
                stc
                ret
.ok:
                clc
                ret

; One sector at a time, so a transfer never straddles a track boundary -
; INT 13h AH=02h is not required to handle that.
;
;   sector   = (lba % spt) + 1          (CHS sectors are 1-based)
;   head     = (lba / spt) % heads
;   cylinder = (lba / spt) / heads
disk_read_chs:
                mov     [chs_remaining], cx
                mov     [chs_lba], eax
                mov     word [chs_off], 0
.one:
                cmp     word [chs_remaining], 0
                je      .done

                mov     eax, [chs_lba]
                xor     edx, edx
                movzx   ebx, byte [spt]
                div     ebx                     ; EAX = lba/spt, EDX = lba%spt
                mov     [chs_sector], dl
                inc     byte [chs_sector]

                xor     edx, edx
                movzx   ebx, byte [heads]
                div     ebx                     ; EAX = cylinder, EDX = head
                mov     [chs_cyl], ax
                mov     [chs_head], dl

                mov     cx, 4                   ; retries
.retry:
                push    cx
                mov     ah, 0x02
                mov     al, 1
                mov     ch, [chs_cyl]           ; cylinder bits 7:0
                mov     cl, [chs_cyl + 1]       ; cylinder bits 9:8 ...
                shl     cl, 6                   ; ... go in CL bits 7:6
                or      cl, [chs_sector]
                mov     dh, [chs_head]
                mov     dl, [boot_drive]
                mov     bx, [chs_off]
                int     0x13
                pop     cx
                jnc     .sector_ok
                xor     ah, ah
                mov     dl, [boot_drive]
                int     0x13
                loop    .retry
                stc
                ret
.sector_ok:
                add     word [chs_off], 512
                inc     dword [chs_lba]
                dec     word [chs_remaining]
                jmp     .one
.done:
                clc
                ret

; -----------------------------------------------------------------------------
; load_kernel - stream the kernel ELF into the staging buffer at 0x20000.
; Reads in 32 KiB chunks, bumping the destination segment by 0x800 paragraphs
; each time so we never cross a real-mode segment wrap.
; -----------------------------------------------------------------------------
load_kernel:
                mov     eax, [hdr_kernel_secs]
                test    eax, eax
                jz      .empty
                mov     [k_remaining], eax
                mov     eax, [hdr_kernel_lba]
                mov     [k_lba], eax
                mov     word [k_seg], KERNEL_STAGE_SEG

                mov     si, msg_loadk
                call    puts
.loop:
                mov     eax, [k_remaining]
                test    eax, eax
                jz      .done
                cmp     eax, CHUNK_SECTORS
                jbe     .have
                mov     eax, CHUNK_SECTORS
.have:
                mov     [k_chunk], ax
                mov     ax, [k_seg]
                mov     es, ax
                mov     cx, [k_chunk]
                mov     eax, [k_lba]
                call    disk_read
                jc      .err

                movzx   eax, word [k_chunk]
                add     [k_lba], eax
                sub     [k_remaining], eax
                mov     ax, [k_chunk]
                shl     ax, 5                   ; sectors * 512 / 16 paragraphs
                add     [k_seg], ax

                mov     al, '.'                 ; progress feedback
                call    putc
                jmp     .loop
.done:
                xor     ax, ax
                mov     es, ax
                mov     si, msg_crlf
                call    puts
                ret
.err:
                xor     ax, ax
                mov     es, ax
                mov     si, msg_kerr
                jmp     fatal
.empty:
                mov     si, msg_kempty
                jmp     fatal

; -----------------------------------------------------------------------------
; build_bootinfo - publish everything we learned at BOOTINFO_ADDR.
; -----------------------------------------------------------------------------
build_bootinfo:
                push    es
                xor     ax, ax
                mov     es, ax
                mov     di, BOOTINFO_ADDR
                mov     cx, BI_SIZE
                xor     al, al
                push    di
                rep     stosb
                pop     di

                mov     dword [es:di + BI_MAGIC], STRATUM_MAGIC
                mov     dword [es:di + BI_VERSION], 1
                movzx   eax, byte [bi_flags]
                mov     [es:di + BI_FLAGS], eax
                movzx   eax, word [e820_count]
                mov     [es:di + BI_E820_COUNT], eax
                mov     dword [es:di + BI_E820_ADDR], E820_BUF
                movzx   eax, byte [boot_drive]
                mov     [es:di + BI_BOOT_DRIVE], eax
                mov     dword [es:di + BI_KERNEL_STAGE], KERNEL_STAGE
                mov     eax, [hdr_kernel_secs]
                mov     [es:di + BI_KERNEL_SECS], eax
                mov     dword [es:di + BI_LOADER_NAME], loader_name
                mov     dword [es:di + BI_CMDLINE], cmdline
                movzx   eax, word [mem_lower]
                mov     [es:di + BI_MEM_LOWER], eax
                movzx   eax, word [mem_upper]
                mov     [es:di + BI_MEM_UPPER], eax

                pop     es
                ret

; =============================================================================
; Real-mode console helpers
; =============================================================================
; Output goes to both the display and COM1 - see boot/serial.inc for why.
puts:
                push    bx
.next:
                lodsb
                test    al, al
                jz      .done
                call    putc
                jmp     .next
.done:
                pop     bx
                ret

putc:
                push    bx
                call    ser_putc
                mov     bh, 0
                mov     ah, 0x0E
                int     0x10
                pop     bx
                ret

%include "serial.inc"

fatal:
                call    puts
                cli
.halt:
                hlt
                jmp     .halt

; =============================================================================
; 32-bit protected mode
; =============================================================================
                bits    32
pm_entry:
                mov     ax, DATA_SEL
                mov     ds, ax
                mov     es, ax
                mov     fs, ax
                mov     gs, ax
                mov     ss, ax
                mov     esp, PM_STACK_TOP
                cld

                ; --- validate the ELF header ------------------------------
                mov     esi, KERNEL_STAGE
                cmp     dword [esi], 0x464C457F ; \x7F E L F
                jne     pm_bad_elf
                cmp     byte [esi + 4], 1       ; EI_CLASS  = ELFCLASS32
                jne     pm_bad_elf
                cmp     byte [esi + 5], 1       ; EI_DATA   = ELFDATA2LSB
                jne     pm_bad_elf
                cmp     word [esi + 16], 2      ; e_type    = ET_EXEC
                jne     pm_bad_elf
                cmp     word [esi + 18], 3      ; e_machine = EM_386
                jne     pm_bad_elf

                movzx   eax, word [esi + 42]    ; e_phentsize
                mov     [ph_size], eax
                movzx   ecx, word [esi + 44]    ; e_phnum
                test    ecx, ecx
                jz      pm_bad_elf
                mov     ebp, [esi + 28]         ; e_phoff
                add     ebp, esi                ; -> first program header

                ; --- copy every PT_LOAD segment to its physical address ---
.ph_loop:
                cmp     dword [ebp + 0], 1      ; p_type == PT_LOAD
                jne     .ph_next

                push    ecx
                push    esi

                mov     edi, [ebp + 12]         ; p_paddr      -> destination
                add     esi, [ebp + 4]          ; + p_offset   -> source
                mov     ecx, [ebp + 16]         ; p_filesz
                test    ecx, ecx
                jz      .no_copy
                rep     movsb
.no_copy:
                mov     ecx, [ebp + 20]         ; p_memsz
                sub     ecx, [ebp + 16]         ; - p_filesz = .bss tail
                jbe     .no_zero
                xor     eax, eax
                rep     stosb                   ; EDI already past the copy
.no_zero:
                pop     esi
                pop     ecx
.ph_next:
                add     ebp, [ph_size]
                dec     ecx
                jnz     .ph_loop

                ; --- hand over, Multiboot-style ---------------------------
                mov     edx, [KERNEL_STAGE + 24]        ; e_entry
                mov     eax, STRATUM_MAGIC
                mov     ebx, BOOTINFO_ADDR
                jmp     edx

pm_bad_elf:
                mov     esi, pm_msg_elf
                call    pm_puts
                cli
.halt:
                hlt
                jmp     .halt

; BIOS teletype is gone once we are in protected mode, so write straight
; into the VGA text framebuffer.
pm_puts:
                mov     edi, 0xB8000
                mov     ah, 0x4F                ; white on red
.next:
                mov     al, [esi]
                test    al, al
                jz      .done
                mov     [edi], ax
                add     edi, 2
                inc     esi
                jmp     .next
.done:
                ret

pm_msg_elf:     db      'FATAL: kernel is not a valid 32-bit x86 ELF', 0

; =============================================================================
; Global Descriptor Table - flat 4 GiB code and data, ring 0
; =============================================================================
                align   8
gdt_start:
                dq      0                       ; null descriptor
gdt_code:
                dw      0xFFFF                  ; limit 15:0
                dw      0x0000                  ; base 15:0
                db      0x00                    ; base 23:16
                db      10011010b               ; P=1 DPL=0 S=1 type=code,R/X
                db      11001111b               ; G=1 D/B=1 limit 19:16 = F
                db      0x00                    ; base 31:24
gdt_data:
                dw      0xFFFF
                dw      0x0000
                db      0x00
                db      10010010b               ; P=1 DPL=0 S=1 type=data,R/W
                db      11001111b
                db      0x00
gdt_end:

gdt_descriptor:
                dw      gdt_end - gdt_start - 1
                dd      gdt_start

; =============================================================================
; Stage 2 data
; =============================================================================
boot_drive:     db      0
use_lba:        db      0
spt:            db      18
heads:          db      2
bi_flags:       db      0

e820_count:     dw      0
mem_lower:      dw      0
mem_upper:      dw      0

chs_remaining:  dw      0
chs_off:        dw      0
chs_cyl:        dw      0
chs_sector:     db      0
chs_head:       db      0
chs_lba:        dd      0

k_remaining:    dd      0
k_lba:          dd      0
k_seg:          dw      0
k_chunk:        dw      0

ph_size:        dd      0

                align   4
dap:
                db      0x10                    ; packet size
                db      0                       ; reserved
dap_count:      dw      0                       ; sectors to transfer
dap_off:        dw      0                       ; destination offset
dap_seg:        dw      0                       ; destination segment
dap_lba:        dq      0                       ; starting LBA

loader_name:    db      'StratumOS stage2', 0

msg_banner:     db      'StratumOS stage2', 13, 10, 0
msg_a20:        db      '  [ok] A20 gate', 13, 10, 0
msg_a20_fail:   db      'E: cannot enable A20', 13, 10, 0
msg_mmap:       db      '  [ok] BIOS memory map', 13, 10, 0
msg_loadk:      db      '  [..] kernel ', 0
msg_loaded:     db      '  [ok] kernel image staged', 13, 10, 0
msg_pmode:      db      '  [->] entering protected mode', 13, 10, 0
msg_kerr:       db      13, 10, 'E: kernel read failed', 13, 10, 0
msg_kempty:     db      'E: kernel length not patched', 13, 10, 0
msg_crlf:       db      13, 10, 0

; -----------------------------------------------------------------------------
; Pad to exactly STAGE2_SECTORS sectors so the image layout is fixed and
; mkimage can patch the header at a known offset.
; -----------------------------------------------------------------------------
%if ($ - $$) > (STAGE2_SECTORS * 512)
  %error "stage2 exceeds its reserved sector count"
%endif
                times   (STAGE2_SECTORS * 512) - ($ - $$) db 0
