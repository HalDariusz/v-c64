/*
 * cia.h - emulacja ukladow CIA 6526 #1 ($DC00) i #2 ($DD00):
 *  - timery A/B generujace przerwania (CIA1 -> IRQ, CIA2 -> NMI),
 *  - macierz klawiatury 8x8 mapowana na skankody x86 (port 0x60 hosta),
 *  - wybor banku 16 KB VIC-II (CIA2 PRA bity 0-1, logika odwrocona).
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

/* Awansuje timery obu CIA o cpu_cycles cykli zegara. */
void cia_tick(int cpu_cycles);

bool cia1_irq_pending(void);   /* -> polaczone z linia IRQ 6510 */
bool cia2_nmi_pending(void);   /* -> polaczone z linia NMI 6510 */

/* Bank VIC-II (0-3) wybrany przez CIA2 PRA bity 0-1 (logika odwrocona). */
uint8_t cia2_vic_bank(void);

/* Uaktualnia stan wcisniecia klawisza w macierzy 8x8 (row/col: 0-7).
 * Wywolywane przez kernel.c po odebraniu skankodu x86 z portu 0x60. */
void cia_keyboard_set(uint8_t row, uint8_t col, bool pressed);

#endif /* CIA_H */
