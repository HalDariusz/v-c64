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

/* State of the /GAME and /EXROM lines (true = inactive/high, as with no cartridge). */
bool cartridge_game_line(void);
bool cartridge_exrom_line(void);

bool cartridge_present(void);

/* Reads the cartridge ROM in the LOROM ($8000-$9FFF) / HIROM ($A000-$BFFF) window. */
uint8_t cartridge_read_lorom(uint16_t address);
uint8_t cartridge_read_hirom(uint16_t address);

/* Access to the I/O1 ($DE00-$DEFF) / I/O2 ($DF00-$DFFF, when not REU) space. */
uint8_t cartridge_io1_read(uint16_t address);
void    cartridge_io1_write(uint16_t address, uint8_t value);

/* If the loaded cartridge has a valid "CBM80" autostart signature at
 * $8004-$8008, returns true and sets *out_addr to the cold-start vector
 * ($8000/$8001) - used to redirect the 6502 RESET vector ($FFFC/$FFFD). */
bool cartridge_autostart_vector(uint16_t *out_addr);

#endif /* CARTRIDGE_H */
