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

/* Minimalny podzbior pol multiboot_info_t potrzebny temu projektowi -
 * budowany bezposrednio przez boot.s (poniewaz nie korzystamy z
 * prawdziwego bootloadera zgodnego z Multiboot, hipernadzorca KVM sam
 * odgrywa jego role: uruchamia gosciu w trybie rzeczywistym pod adresem
 * 0x00000, a boot.s przechodzi do trybu chronionego i przygotowuje ta
 * strukture dla kernel_main()). */
typedef struct multiboot_info_min {
    uint32_t magic;             /* 0x2BADB002 */
    uint32_t flags;
    uint32_t framebuffer_addr;  /* fizyczny adres bufora VESA (0xA0000) */
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint32_t framebuffer_bpp;
} multiboot_info_min_t;

void kernel_main(multiboot_info_min_t *mb_info);

#endif /* KERNEL_H */
