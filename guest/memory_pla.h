/*
 * guest/memory_pla.h - 6510 CPU PLA banking logic: switches the CPU's
 * view between 64 KB RAM, ROMs (BASIC/KERNAL/CHARGEN), the I/O space, and
 * the cartridge, based on the CPU port register ($00/$01) and the
 * expansion port's /GAME, /EXROM lines. Implements the
 * fake6502_mem_read/write hooks required by the fake6502 core.
 *
 * Part of v-c64 - a bare-metal Commodore 64 unikernel running directly
 * on Linux /dev/kvm, with no QEMU involved.
 *
 * Author: Dariusz Nowak <hal.dariusz.nowak@gmail.com>
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
