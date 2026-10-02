; =============================================================================
; Real-mode serial output for the bootloader.
;
; The BIOS teletype call writes to the display, which is invisible when the
; machine is running headless - exactly the situation in CI, and exactly when
; a boot failure is hardest to diagnose. Talking to the 16550 directly costs
; about 40 bytes and makes the entire boot path observable over a serial line,
; from the first instruction of the MBR onwards.
;
; The port is left configured the same way the kernel expects (115200 8N1), so
; output continues seamlessly across the handover into protected mode.
; =============================================================================

%define COM1            0x3F8
%define COM1_IER        0x3F9
%define COM1_FCR        0x3FA
%define COM1_LCR        0x3FB
%define COM1_MCR        0x3FC
%define COM1_LSR        0x3FD
%define LSR_THR_EMPTY   0x20

; ser_init - configure COM1 for 115200 8N1. Clobbers AX, DX.
ser_init:
                mov     dx, COM1_IER
                xor     al, al
                out     dx, al                  ; no UART interrupts
                mov     dx, COM1_LCR
                mov     al, 0x80
                out     dx, al                  ; DLAB on
                mov     dx, COM1
                mov     al, 1
                out     dx, al                  ; divisor = 1 -> 115200
                mov     dx, COM1_IER
                xor     al, al
                out     dx, al
                mov     dx, COM1_LCR
                mov     al, 0x03
                out     dx, al                  ; 8N1, DLAB off
                mov     dx, COM1_FCR
                mov     al, 0xC7
                out     dx, al                  ; enable and clear the FIFOs
                mov     dx, COM1_MCR
                mov     al, 0x0B
                out     dx, al                  ; DTR, RTS, OUT2
                ret

; ser_putc - write AL to COM1. Preserves every register.
; The wait is bounded so a machine with no UART does not hang here.
ser_putc:
                push    ax
                push    cx
                push    dx
                mov     [ser_tmp], al
                mov     cx, 0x4000
                mov     dx, COM1_LSR
.spin:
                in      al, dx
                test    al, LSR_THR_EMPTY
                jnz     .send
                loop    .spin
                jmp     .done
.send:
                mov     dx, COM1
                mov     al, [ser_tmp]
                out     dx, al
.done:
                pop     dx
                pop     cx
                pop     ax
                ret

ser_tmp:        db      0
