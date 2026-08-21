/*
 * guest/linker_raw.ld - lays out the bare-metal guest starting at
 * physical address 0x00000000, matching the RAM region (0x00000-0x7FFFF,
 * 512 KB) registered by kvm_host.c via KVM_SET_USER_MEMORY_REGION. The
 * resulting ELF file is then converted to a plain binary via
 * `objcopy -O binary`.
 *
 * The 512K MEMORY region acts as a hard build-time guarantee: if
 * code+data+bss exceeds the limit, the link fails with an error instead
 * of silently corrupting host memory (addresses above 0x7FFFF are
 * unmapped up to the VRAM region at 0xA0000).
 *
 * Part of v-c64 - a bare-metal Commodore 64 unikernel running directly
 * on Linux /dev/kvm, with no QEMU involved.
 *
 * Author: Dariusz Nowak <hal.dariusz.nowak@gmail.com>
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
