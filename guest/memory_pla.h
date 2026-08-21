/*
 * memory_pla.h - logika bankowania PLA procesora 6510: przelacza widok CPU
 * pomiedzy 64 KB RAM, ROM-ami (BASIC/KERNAL/CHARGEN), przestrzenia I/O oraz
 * kartridzem, na podstawie rejestru portu procesora ($00/$01) i linii
 * /GAME, /EXROM gniazda rozszerzen. Implementuje hooki fake6502_mem_read/write
 * wymagane przez rdzen fake6502.
 */
#ifndef MEMORY_PLA_H
#define MEMORY_PLA_H

#include <stdint.h>
#include "fake6502.h"

void pla_reset(void);

/* Wymagane przez fake6502.c: */
uint8_t fake6502_mem_read(fake6502_context *c, uint16_t address);
void    fake6502_mem_write(fake6502_context *c, uint16_t address, uint8_t val);

#endif /* MEMORY_PLA_H */
