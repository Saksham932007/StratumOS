; Segment selectors, kept in sync with kernel/include/arch/gdt.h.
SEL_KERNEL_CODE equ 0x08
SEL_KERNEL_DATA equ 0x10
SEL_USER_CODE   equ 0x1B        ; index 3, RPL 3
SEL_USER_DATA   equ 0x23        ; index 4, RPL 3
; One TSS descriptor per CPU from index 5 on; the asm only needs the first.
SEL_TSS_FIRST   equ 0x28
