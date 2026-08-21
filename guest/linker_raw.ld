/*
 * linker_raw.ld - uklada goscia bare-metal od adresu fizycznego 0x00000000,
 * zgodnie z regionem RAM (0x00000-0x7FFFF, 512 KB) rejestrowanym przez
 * kvm_host.c w KVM_SET_USER_MEMORY_REGION. Wynikowy plik ELF jest nastepnie
 * konwertowany do czystego binarium przez `objcopy -O binary`.
 *
 * Region MEMORY o dlugosci 512K dziala jako twarda gwarancja budowy: jesli
 * kod+dane+bss przekrocza limit, konsolidacja zakonczy sie bledem zamiast
 * cichego uszkodzenia pamieci hosta (adresy powyzej 0x7FFFF nie sa
 * zmapowane az do regionu VRAM pod 0xA0000).
 */

ENTRY(_start)

MEMORY
{
    RAM (rwx) : ORIGIN = 0x00000000, LENGTH = 512K
}

SECTIONS
{
    . = 0x00000000;

    .text : {
        *(.text)
        *(.text.*)
    } > RAM

    .rodata : {
        *(.rodata)
        *(.rodata.*)
    } > RAM

    .data : {
        *(.data)
        *(.data.*)
    } > RAM

    .bss : {
        *(COMMON)
        *(.bss)
        *(.bss.*)
    } > RAM

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
        *(.note.*)
    }
}
