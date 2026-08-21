/*
 * guest/cartridge.h - emulation of the C64 expansion port with Simons'
 * BASIC (16 KB).
 *
 * Simons' BASIC starts in 16K mode (GAME=0 / EXROM=1 lines): the ROM is
 * visible at both $8000-$9FFF (LOROM) and $A000-$BFFF (HIROM), letting
 * the KERNAL detect the "CBM80" autostart signature at $8004-$8008 and
 * start the cartridge automatically. After initialization, Simons' BASIC
 * writes to the I/O1 address ($DE00), switching the cartridge to 8K mode
 * (GAME=1), which frees $A000-$BFFF back to BASIC RAM while keeping only
 * the extended-command dispatcher at $8000-$9FFF.
 *
 * Part of v-c64 - a bare-metal Commodore 64 unikernel running directly
 * on Linux /dev/kvm, with no QEMU involved.
 *
 * Author: Dariusz Nowak <hal.dariusz.nowak@gmail.com>
 */
#ifndef CARTRIDGE_H
#define CARTRIDGE_H

#include <stdint.h>
#include <stdbool.h>

void cartridge_reset(void);

/* Stan linii /GAME i /EXROM (true = nieaktywna/wysoka, jak przy braku kartridza). */
bool cartridge_game_line(void);
bool cartridge_exrom_line(void);

bool cartridge_present(void);

/* Odczyt ROM-u kartridza w oknie LOROM ($8000-$9FFF) / HIROM ($A000-$BFFF). */
uint8_t cartridge_read_lorom(uint16_t address);
uint8_t cartridge_read_hirom(uint16_t address);

/* Dostep do przestrzeni I/O1 ($DE00-$DEFF) / I/O2 ($DF00-$DFFF, gdy nie REU). */
uint8_t cartridge_io1_read(uint16_t address);
void    cartridge_io1_write(uint16_t address, uint8_t value);

/* Jesli zaladowany kartridz ma poprawna sygnature autostartu "CBM80" pod
 * $8004-$8008, zwraca true i ustawia *out_addr na wektor cold-start
 * ($8000/$8001) - do przekierowania wektora RESET 6502 ($FFFC/$FFFD). */
bool cartridge_autostart_vector(uint16_t *out_addr);

#endif /* CARTRIDGE_H */
