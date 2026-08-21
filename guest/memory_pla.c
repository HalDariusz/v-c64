/*
 * guest/memory_pla.c - implementation of the PLA banking logic declared
 * in memory_pla.h: read6502()/write6502() (via the fake6502_mem_read/
 * write hooks) switch between RAM, BASIC/KERNAL/CHARGEN ROM, I/O
 * registers, and the cartridge, based on the $00/$01 CPU port bits and
 * the expansion port's /GAME, /EXROM lines.
 *
 * Part of v-c64 - a bare-metal Commodore 64 unikernel running directly
 * on Linux /dev/kvm, with no QEMU involved.
 *
 * Author: Dariusz Nowak <hal.dariusz.nowak@gmail.com>
 */
#include "memory_pla.h"
#include "c64bus.h"
#include "cartridge.h"
#include "vic2.h"
#include "cia.h"
#include "sid.h"
#include "reu.h"

uint8_t c64_ram[C64_RAM_SIZE];
uint8_t rom_kernal[ROM_KERNAL_SIZE];
uint8_t rom_basic[ROM_BASIC_SIZE];
uint8_t rom_chargen[ROM_CHARGEN_SIZE];
uint8_t rom_simons[ROM_SIMONS_SIZE];
uint8_t color_ram[COLOR_RAM_SIZE];

uint8_t cpu_port_ddr;
uint8_t cpu_port_pr;

uint8_t cpu_port_effective(void)
{
    /* Bity nieskonfigurowane jako wyjscie "plywaja" wysoko (podciagniecie),
     * tak jak na prawdziwym 6510. */
    return (uint8_t)((cpu_port_pr & cpu_port_ddr) | (uint8_t)(~cpu_port_ddr));
}

void pla_reset(void)
{
    cpu_port_ddr = 0x2F; /* domyslny stan po resecie KERNAL-a */
    cpu_port_pr  = 0x37; /* LORAM=HIRAM=CHAREN=1 -> wszystkie ROM-y + I/O widoczne */
}

/* Dostep do przestrzeni I/O $D000-$DFFF, widoczny gdy CHAREN=1. */
static uint8_t io_read(uint16_t address)
{
    if (address <= 0xD3FF) return vic2_reg_read((uint8_t)(address & 0x3F));
    if (address <= 0xD7FF) return sid_reg_read((uint8_t)(address & 0x1F));
    if (address <= 0xDBFF) return (uint8_t)(color_ram[address - 0xD800] | 0xF0);
    if (address <= 0xDCFF) return cia1_reg_read((uint8_t)(address & 0x0F));
    if (address <= 0xDDFF) return cia2_reg_read((uint8_t)(address & 0x0F));
    if (address <= 0xDEFF) return cartridge_io1_read(address);
    /* $DF00-$DFFF: REU pod $DF00-$DF1F, reszta nieprzypisana (open bus). */
    if (address <= 0xDF1F) return reu_reg_read((uint8_t)(address - 0xDF00));
    return 0xFF;
}

static void io_write(uint16_t address, uint8_t value)
{
    if (address <= 0xD3FF) { vic2_reg_write((uint8_t)(address & 0x3F), value); return; }
    if (address <= 0xD7FF) { sid_reg_write((uint8_t)(address & 0x1F), value); return; }
    if (address <= 0xDBFF) { color_ram[address - 0xD800] = (uint8_t)(value & 0x0F); return; }
    if (address <= 0xDCFF) { cia1_reg_write((uint8_t)(address & 0x0F), value); return; }
    if (address <= 0xDDFF) { cia2_reg_write((uint8_t)(address & 0x0F), value); return; }
    if (address <= 0xDEFF) { cartridge_io1_write(address, value); return; }
    if (address <= 0xDF1F) { reu_reg_write((uint8_t)(address - 0xDF00), value); return; }
}

uint8_t fake6502_mem_read(fake6502_context *c, uint16_t address)
{
    (void)c;

    if (address <= 0x0001)
        return (address == 0x0000) ? cpu_port_ddr : cpu_port_effective();

    uint8_t port = cpu_port_effective();
    bool loram  = (port & CPU_PORT_LORAM)  != 0;
    bool hiram  = (port & CPU_PORT_HIRAM)  != 0;
    bool charen = (port & CPU_PORT_CHAREN) != 0;
    bool game   = cartridge_game_line();
    bool exrom  = cartridge_exrom_line();

    if (address >= 0x8000 && address <= 0x9FFF) {
        if (!game || !exrom) return cartridge_read_lorom(address); /* LOROM aktywny */
        return c64_ram[address];
    }

    if (address >= 0xA000 && address <= 0xBFFF) {
        if (!game && exrom) return cartridge_read_hirom(address);  /* tryb 16K */
        if (loram && hiram) return rom_basic[address - 0xA000];
        return c64_ram[address];
    }

    if (address >= 0xD000 && address <= 0xDFFF) {
        if (charen) return io_read(address);
        return rom_chargen[address - 0xD000];
    }

    if (address >= 0xE000) {
        if (hiram) {
            /* Autostart kartridza: jesli obecny jest kartridz z prawidlowa
             * sygnatura "CBM80", wektor RESET 6502 wskazuje na jego
             * cold-start zamiast na normalny start KERNAL-a - dokladnie
             * tak, jak realny KERNAL C64 sam by to wykryl i przekierowal. */
            uint16_t cart_vec;
            if ((address == 0xFFFC || address == 0xFFFD) &&
                cartridge_autostart_vector(&cart_vec)) {
                return (address == 0xFFFC) ? (uint8_t)(cart_vec & 0xFF)
                                            : (uint8_t)(cart_vec >> 8);
            }
            return rom_kernal[address - 0xE000];
        }
        return c64_ram[address];
    }

    return c64_ram[address];
}

void fake6502_mem_write(fake6502_context *c, uint16_t address, uint8_t val)
{
    (void)c;

    if (address <= 0x0001) {
        if (address == 0x0000) cpu_port_ddr = val; else cpu_port_pr = val;
        c64_ram[address] = val; /* zapis widoczny takze przy odczycie surowej RAM (np. przez REU) */
        return;
    }

    uint8_t port = cpu_port_effective();
    bool charen = (port & CPU_PORT_CHAREN) != 0;

    /* RAM pod bankowanymi oknami ROM/kartridza jest zawsze zapisywalna
     * (tak jak na prawdziwym C64 - ROM/I-O tylko przeslania odczyt). */
    if (address >= 0xD000 && address <= 0xDFFF && charen) {
        io_write(address, val);
        return;
    }

    c64_ram[address] = val;
}
