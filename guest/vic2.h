/*
 * guest/vic2.h - VIC-II (6569/PAL) graphics emulation: text and bitmap
 * modes, 8 hardware sprites, raster interrupts, and scaling of the
 * output into a 320x200 buffer (ultimately copied to the host's physical
 * VESA buffer at 0xA0000).
 *
 * Part of v-c64 - a bare-metal Commodore 64 unikernel running directly
 * on Linux /dev/kvm, with no QEMU involved.
 *
 * Author: Dariusz Nowak <hal.dariusz.nowak@gmail.com>
 */
#ifndef VIC2_H
#define VIC2_H

#include <stdint.h>
#include <stdbool.h>

#define VIC2_SCREEN_W          320
#define VIC2_SCREEN_H          200
#define VIC2_LINES_PER_FRAME   312   /* PAL: 312 raster lines / frame */
#define VIC2_CYCLES_PER_LINE    63   /* PAL: ~985248 Hz / (312 * 50.125 Hz) */

void vic2_reset(void);

uint8_t vic2_reg_read(uint8_t offset);
void    vic2_reg_write(uint8_t offset, uint8_t value);

/* Advances the simulation by cpu_cycles cycles of the 6510/6502 clock.
 * Returns true exactly when a full frame finished in this step (the
 * raster line wrapped around to 0) - at that point vic2_framebuffer
 * holds a freshly rendered frame. */
bool vic2_tick(int cpu_cycles);

/* true if some VIC-II interrupt source is both latched and enabled
 * (to be wired into the 6510 IRQ line in kernel.c). */
bool vic2_irq_pending(void);

/* Current raster line number (0..311), for CIA/debug use. */
uint16_t vic2_current_raster_line(void);

/* 320x200 buffer, 8 bpp, indices 0-15 = standard C64 palette. */
extern uint8_t vic2_framebuffer[VIC2_SCREEN_W * VIC2_SCREEN_H];

#endif /* VIC2_H */
