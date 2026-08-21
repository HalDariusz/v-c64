; boot.s - rozruch bare-metal goscia x86 uruchamianego bezposrednio przez
; kvm_host.c (bez BIOS-u/GRUB-a). Hipernadzorca ustawia wektor resetu x86
; pod fizycznym adresem 0xFFFF0 tak, aby wykonal skok "jmp far 0000:0000",
; co przenosi CPU (w trybie rzeczywistym) do fizycznego adresu 0x00000,
; gdzie zaczyna sie ten plik (patrz linker_raw.ld: sekcja .text @ 0x00000000).
;
; Kolejne kroki:
;   1) natychmiastowy skok nad naglowkiem Multiboot1 (obecnym dla zgodnosci
;      formalnej ze specyfikacja / ewentualnym uruchomieniem przez GRUB),
;   2) klasyczne przejscie real mode -> protected mode (A20, GDT, CR0.PE),
;   3) zbudowanie minimalnej struktury multiboot_info_min_t opisujacej
;      liniowy bufor VESA pod fizycznym 0xA0000 (320x200x8bpp - tryb, ktory
;      hipernadzorca gwarantuje samym faktem zmapowania tego regionu pamieci,
;      bez potrzeby wywolywania realnego BIOS-u VBE, ktorego tu nie ma),
;   4) wywolanie kernel_main(&mb_info).

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
