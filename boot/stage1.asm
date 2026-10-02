; =============================================================================
; StratumOS - Stage 1 Bootloader (Master Boot Record)
; =============================================================================
;
; The BIOS loads exactly one sector (512 bytes) from the boot device to
; physical address 0x7C00 and jumps to it in 16-bit real mode. That is not
; enough room to do anything interesting, so stage 1 has exactly one job:
; load stage 2 and transfer control to it.
;
; Design notes
; ------------
;  * Segment registers are zeroed and ORG is 0x7C00, so a label's value IS
;    its linear address. Mixing `ORG 0x7C00` with `ds = 0x07C0` (a common
;    beginner's bug) double-counts the base and sends every memory reference
;    0x7C00 bytes too high.
;
;  * Reads prefer INT 13h AH=42h (LBA / "extended read"), which works on hard
;    disks, USB sticks and CD-ROM emulation and needs no geometry guessing.
;    We probe for it with AH=41h first and fall back to classic CHS AH=02h,
;    retrying three times with a controller reset in between - real floppy
;    and USB-FDD emulation genuinely do fail the first read.
;
;  * DL holds the BIOS drive number on entry. It is preserved and handed to
;    stage 2 so the same device is used for subsequent reads.
;
; Memory map while stage 1 runs
; -----------------------------
;   0x00000 - 0x004FF   IVT + BIOS data area
;   0x00500 - 0x07BFF   free
;   0x07C00 - 0x07DFF   this sector
;   0x07E00 - 0x0ADFF   stage 2 (24 sectors) <- loaded here
;   0x07C00 downwards   stack
; =============================================================================

                bits    16
                org     0x7C00

%define STAGE2_SEG      0x0000          ; stage 2 load segment
%define STAGE2_OFF      0x7E00          ; ...and offset -> linear 0x7E00
%define STAGE2_LBA      1               ; stage 2 begins at sector 1
%define STAGE2_SECTORS  24              ; 12 KiB reserved for stage 2
%define STAGE2_MAGIC    'S2OS'          ; stage 2 header signature
; Stage 2 begins with a header that mkimage patches (kernel location, command
; line); its code starts after it. Must match the entry offset in stage2.asm.
%define STAGE2_ENTRY_OFF 112
%define CHS_RETRIES     3

; -----------------------------------------------------------------------------
; Entry point
; -----------------------------------------------------------------------------
start:
                cli
                xor     ax, ax
                mov     ds, ax
                mov     es, ax
                mov     ss, ax
                mov     sp, 0x7C00      ; stack grows down, away from us
                sti

                mov     [boot_drive], dl

                call    ser_init
                mov     si, msg_stage1
                call    puts

; -----------------------------------------------------------------------------
; Probe for INT 13h extensions (LBA addressing)
; -----------------------------------------------------------------------------
                mov     ah, 0x41
                mov     bx, 0x55AA
                mov     dl, [boot_drive]
                int     0x13
                jc      .use_chs
                cmp     bx, 0xAA55
                jne     .use_chs
                test    cl, 1                   ; bit 0 = packet access support
                jz      .use_chs

; -----------------------------------------------------------------------------
; Extended (LBA) read
; -----------------------------------------------------------------------------
.use_lba:
                mov     si, dap
                mov     dl, [boot_drive]
                mov     ah, 0x42
                int     0x13
                jnc     .loaded

; -----------------------------------------------------------------------------
; Legacy CHS read. LBA 1 on any standard geometry is C=0 H=0 S=2, because
; CHS sector numbers are 1-based while LBA is 0-based.
; -----------------------------------------------------------------------------
.use_chs:
                mov     cx, CHS_RETRIES
.chs_attempt:
                push    cx
                mov     ah, 0x02
                mov     al, STAGE2_SECTORS
                mov     ch, 0                   ; cylinder 0
                mov     cl, 2                   ; sector 2 (= LBA 1)
                mov     dh, 0                   ; head 0
                mov     dl, [boot_drive]
                xor     bx, bx
                mov     es, bx
                mov     bx, STAGE2_OFF
                int     0x13
                pop     cx
                jnc     .loaded

                ; Reset the disk controller before retrying.
                xor     ah, ah
                mov     dl, [boot_drive]
                int     0x13
                loop    .chs_attempt

                mov     si, msg_disk_err
                jmp     fatal

; -----------------------------------------------------------------------------
; Validate and enter stage 2
; -----------------------------------------------------------------------------
.loaded:
                cmp     dword [STAGE2_OFF], STAGE2_MAGIC
                jne     .bad_magic

                mov     dl, [boot_drive]        ; hand the drive to stage 2
                jmp     STAGE2_SEG:(STAGE2_OFF + STAGE2_ENTRY_OFF)

.bad_magic:
                mov     si, msg_bad_magic
                jmp     fatal

; -----------------------------------------------------------------------------
; fatal - print SI and stop the machine for good
; -----------------------------------------------------------------------------
fatal:
                call    puts
                cli
.halt:
                hlt
                jmp     .halt

; -----------------------------------------------------------------------------
; puts - write the NUL-terminated string at DS:SI to both the display and the
; serial port, so the boot is legible with or without a monitor attached.
; clobbers: ax, si
; -----------------------------------------------------------------------------
puts:
                push    bx
.next:
                lodsb
                test    al, al
                jz      .done
                call    ser_putc
                mov     bh, 0
                mov     ah, 0x0E
                int     0x10
                jmp     .next
.done:
                pop     bx
                ret

%include "serial.inc"

; -----------------------------------------------------------------------------
; Disk Address Packet for INT 13h AH=42h
; -----------------------------------------------------------------------------
                align   4
dap:
                db      0x10                    ; packet size
                db      0                       ; reserved
                dw      STAGE2_SECTORS          ; sectors to transfer
                dw      STAGE2_OFF              ; destination offset
                dw      STAGE2_SEG              ; destination segment
                dq      STAGE2_LBA              ; starting LBA (64-bit)

boot_drive:     db      0

msg_stage1:     db      'StratumOS stage1', 13, 10, 0
msg_disk_err:   db      'E: stage2 read failed', 13, 10, 0
msg_bad_magic:  db      'E: stage2 corrupt', 13, 10, 0

; -----------------------------------------------------------------------------
; Pad to a 512-byte sector ending in the MBR boot signature.
; The %if guards against silently truncating the code if it ever outgrows
; the sector - a build error is far better than a mysterious triple fault.
; -----------------------------------------------------------------------------
%if ($ - $$) > 510
  %error "stage1 exceeds 510 bytes"
%endif
                times   510 - ($ - $$) db 0
                dw      0xAA55
