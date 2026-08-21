/*
 * guest/cia.h - emulation of the CIA 6526 #1 ($DC00) and #2 ($DD00) chips:
 *  - Timers A/B generating interrupts (CIA1 -> IRQ, CIA2 -> NMI),
 *  - 8x8 keyboard matrix mapped from x86 scancodes (host port 0x60),
 *  - 16 KB VIC-II bank selection (CIA2 PRA bits 0-1, inverted logic).
 *
 * Part of v-c64 - a bare-metal Commodore 64 unikernel running directly
 * on Linux /dev/kvm, with no QEMU involved.
 *
 * Author: Dariusz Nowak <hal.dariusz.nowak@gmail.com>
 */
#ifndef CIA_H
#define CIA_H

#include <stdint.h>
#include <stdbool.h>

void cia_reset(void);

uint8_t cia1_reg_read(uint8_t offset);
void    cia1_reg_write(uint8_t offset, uint8_t value);

uint8_t cia2_reg_read(uint8_t offset);
void    cia2_reg_write(uint8_t offset, uint8_t value);

/* Advances both CIAs' timers by cpu_cycles clock cycles. */
void cia_tick(int cpu_cycles);

bool cia1_irq_pending(void);   /* -> wired to the 6510 IRQ line */
bool cia2_nmi_pending(void);   /* -> wired to the 6510 NMI line */

/* VIC-II bank (0-3) selected by CIA2 PRA bits 0-1 (inverted logic). */
uint8_t cia2_vic_bank(void);

/* Updates the pressed state of a key in the 8x8 matrix (row/col: 0-7).
 * Called by kernel.c after receiving an x86 scancode from port 0x60. */
void cia_keyboard_set(uint8_t row, uint8_t col, bool pressed);

#endif /* CIA_H */
