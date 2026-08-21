/*
 * guest/c64bus.h - shared C64 bus state: RAM, ROM images, CPU port.
 *
 * Every chip (VIC-II, CIA 1/2, SID, REU, cartridge) sees the same 64 KB
 * 6510 CPU memory image through memory_pla.c, which is the single place
 * implementing the PLA banking logic.
 *
 * Part of v-c64 - a bare-metal Commodore 64 unikernel running directly
 * on Linux /dev/kvm, with no QEMU involved.
 *
 * Author: Dariusz Nowak <hal.dariusz.nowak@gmail.com>
 */
#ifndef C64BUS_H
#define C64BUS_H

#include <stdint.h>
#include <stdbool.h>

#define C64_RAM_SIZE     65536u
#define ROM_KERNAL_SIZE   8192u
#define ROM_BASIC_SIZE    8192u
#define ROM_CHARGEN_SIZE  4096u
#define ROM_SIMONS_SIZE  16384u
#define COLOR_RAM_SIZE    1024u  /* tylko dolne 4 bity kazdego bajtu sa realne */

/* Pelne 64 KB RAM widziane przez 6510 (bankowanie PLA nie zmienia tej tablicy,
 * jedynie decyduje co widzi CPU pod danym adresem). */
extern uint8_t c64_ram[C64_RAM_SIZE];

/* Obrazy ROM - wypelniane przez kernel_main() z tablic wygenerowanych przez xxd. */
extern uint8_t rom_kernal[ROM_KERNAL_SIZE];
extern uint8_t rom_basic[ROM_BASIC_SIZE];
extern uint8_t rom_chargen[ROM_CHARGEN_SIZE];
extern uint8_t rom_simons[ROM_SIMONS_SIZE];

/* Pamiec kolorow $D800-$DBE7 (nibble RAM). */
extern uint8_t color_ram[COLOR_RAM_SIZE];

/* Rejestr kierunku portu ($00) i rejestr danych portu ($01) procesora 6510. */
extern uint8_t cpu_port_ddr;
extern uint8_t cpu_port_pr;

/* Zwraca efektywny stan linii LORAM/HIRAM/CHAREN z uwzglednieniem
 * podciagniecia (pull-up) na wejsciach niekonfigurowanych jako wyjscia. */
uint8_t cpu_port_effective(void);

#define CPU_PORT_LORAM  0x01u
#define CPU_PORT_HIRAM  0x02u
#define CPU_PORT_CHAREN 0x04u

#endif /* C64BUS_H */
