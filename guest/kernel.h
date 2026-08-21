/*
 * guest/kernel.h - types shared between boot.s and kernel.c.
 *
 * Part of v-c64 - a bare-metal Commodore 64 unikernel running directly
 * on Linux /dev/kvm, with no QEMU involved.
 *
 * Author: Dariusz Nowak <hal.dariusz.nowak@gmail.com>
 */
#ifndef KERNEL_H
#define KERNEL_H

#include <stdint.h>

/* Minimal subset of the multiboot_info_t fields this project needs -
 * built directly by boot.s (since we don't use a real Multiboot-compliant
 * bootloader; the KVM hypervisor itself plays that role: it starts the
 * guest in real mode at address 0x00000, and boot.s switches to protected
 * mode and prepares this structure for kernel_main()). */
typedef struct multiboot_info_min {
    uint32_t magic;             /* 0x2BADB002 */
    uint32_t flags;
    uint32_t framebuffer_addr;  /* physical address of the VESA buffer (0xA0000) */
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint32_t framebuffer_bpp;
} multiboot_info_min_t;

void kernel_main(multiboot_info_min_t *mb_info);

#endif /* KERNEL_H */
