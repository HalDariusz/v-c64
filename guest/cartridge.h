/*
 * cartridge.h - emulacja gniazda rozszerzen C64 z Simons' BASIC (16 KB).
 *
 * Simons' BASIC startuje w trybie 16K (linie GAME=0 / EXROM=1): ROM widoczny
 * jednoczesnie pod $8000-$9FFF (LOROM) i $A000-$BFFF (HIROM), co pozwala
 * KERNAL-owi wykryc podpis autostartu "CBM80" pod $8004-$8008 i wystartowac
 * kartridz automatycznie. Po inicjalizacji Simons' BASIC zapisuje pod adres
 * I/O1 ($DE00), co przelacza kartridz w tryb 8K (GAME=1) zwalniajac
 * $A000-$BFFF z powrotem pod BASIC-RAM, zachowujac jedynie dispatcher
 * rozszerzonych komend pod $8000-$9FFF.
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
