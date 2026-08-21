/*
 * vic2.h - emulacja grafiki VIC-II (6569/PAL): tryby tekstowe i graficzne,
 * 8 sprite'ow sprzetowych, przerwania rastra oraz skalowanie wyjscia do
 * bufora 320x200 (docelowo kopiowanego do fizycznego bufora VESA hosta
 * pod 0xA0000).
 */
#ifndef VIC2_H
#define VIC2_H

#include <stdint.h>
#include <stdbool.h>

#define VIC2_SCREEN_W          320
#define VIC2_SCREEN_H          200
#define VIC2_LINES_PER_FRAME   312   /* PAL: 312 linii rastra / klatke */
#define VIC2_CYCLES_PER_LINE    63   /* PAL: ~985248 Hz / (312 * 50.125 Hz) */

void vic2_reset(void);

uint8_t vic2_reg_read(uint8_t offset);
void    vic2_reg_write(uint8_t offset, uint8_t value);

/* Awansuje symulacje o cpu_cycles cykli zegara 6510/6502.
 * Zwraca true dokladnie wtedy, gdy w tym kroku zakonczyla sie pelna klatka
 * (linia rastra zawinela sie do 0) - wowczas vic2_framebuffer zawiera
 * swiezo wyrenderowany obraz. */
bool vic2_tick(int cpu_cycles);

/* true, jesli jakies zrodlo przerwan VIC-II jest zatwierdzone i wlaczone
 * (do polaczenia z linia IRQ 6510 w kernel.c). */
bool vic2_irq_pending(void);

/* Numer aktualnej linii rastra (0..311), do CIA/debug. */
uint16_t vic2_current_raster_line(void);

/* Bufor 320x200, 8 bpp, indeksy 0-15 = standardowa paleta C64. */
extern uint8_t vic2_framebuffer[VIC2_SCREEN_W * VIC2_SCREEN_H];

#endif /* VIC2_H */
