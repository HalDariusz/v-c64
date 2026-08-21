; guest/boot.s - bare-metal boot code for the x86 guest, run directly by
; kvm_host.c (no BIOS/GRUB). The hypervisor sets the x86 reset vector at
; physical address 0xFFFF0 to perform a "jmp far 0000:0000" jump, which
; takes the CPU (in real mode) to physical address 0x00000, where this
; file begins (see linker_raw.ld: .text section @ 0x00000000).
;
; Steps performed:
;   1) an immediate jump over the Multiboot1 header (present only for
;      formal spec compliance / in case it's ever run under GRUB),
;   2) the classic real mode -> protected mode transition (A20, GDT,
;      CR0.PE),
;   3) building a minimal multiboot_info_min_t structure describing the
;      linear VESA buffer at physical 0xA0000 (320x200x8bpp - a mode the
;      hypervisor guarantees simply by mapping that memory region, with
;      no need to call a real VBE BIOS, which doesn't exist here),
;   4) calling kernel_main(&mb_info).
;
; Part of v-c64 - a bare-metal Commodore 64 unikernel running directly
; on Linux /dev/kvm, with no QEMU involved.
;
; Author: Dariusz Nowak <hal.dariusz.nowak@gmail.com>

BITS 16
section .text

global _start
extern kernel_main

MB_MAGIC    equ 0x1BADB002
MB_FLAGS    equ 0x00000004      ; bit2: prosba o tryb graficzny
MB_CHECKSUM equ -(MB_MAGIC + MB_FLAGS)

_start:
    jmp short real_start

align 4
mb_header:
    dd MB_MAGIC
    dd MB_FLAGS
    dd MB_CHECKSUM
    dd 0            ; mode_type: 0 = linear graphics (nie tekstowy)
    dd 320          ; width
    dd 200          ; height
    dd 8            ; depth (bpp)

real_start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7000          ; tymczasowy stos trybu rzeczywistego

    ; Fast A20 gate - nieszkodliwe nawet gdy KVM juz udostepnia pelna
    ; przestrzen adresowa bez maskowania A20.
    in al, 0x92
    or al, 0x02
    out 0x92, al

    lgdt [gdt_descriptor]

    mov eax, cr0
    or eax, 1
    mov cr0, eax

    jmp CODE_SEG:protected_start

; --- GDT: plaski model pamieci 4 GB (kod + dane) ---------------------------
align 8
gdt_start:
    dq 0x0000000000000000              ; deskryptor zerowy
gdt_code:
    dw 0xFFFF, 0x0000
    db 0x00, 10011010b, 11001111b, 0x00
gdt_data:
    dw 0xFFFF, 0x0000
    db 0x00, 10010010b, 11001111b, 0x00
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

CODE_SEG equ gdt_code - gdt_start
DATA_SEG equ gdt_data - gdt_start

; --- kod 32-bitowy ----------------------------------------------------------
BITS 32
protected_start:
    mov ax, DATA_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, stack_top

    mov dword [mb_info + 0], 0x2BADB002    ; magic (jak w rejestrze EAX Multiboot)
    mov dword [mb_info + 4], MB_FLAGS
    mov dword [mb_info + 8], 0x000A0000    ; framebuffer_addr - VRAM hosta
    mov dword [mb_info + 12], 320          ; framebuffer_width
    mov dword [mb_info + 16], 200          ; framebuffer_height
    mov dword [mb_info + 20], 8            ; framebuffer_bpp

    push mb_info
    call kernel_main

hang:
    hlt
    jmp hang

section .bss
align 16
stack_bottom:
    resb 16384          ; 16 KB stosu, jak wymaga specyfikacja
stack_top:

align 4
mb_info:
    resb 24

; Wylacza domysly executable stack marker w obiekcie ELF (kosmetyczne -
; nie ma tu w ogole stronicowania/NX, ale wycisza ostrzezenie linkera).
section .note.GNU-stack noalloc noexec nowrite progbits
