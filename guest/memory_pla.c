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
    /* Bits not configured as output "float" high (pull-up), just like on
     * a real 6510. */
    return (uint8_t)((cpu_port_pr & cpu_port_ddr) | (uint8_t)(~cpu_port_ddr));
}

void pla_reset(void)
{
    cpu_port_ddr = 0x2F; /* default state after a KERNAL reset */
    cpu_port_pr  = 0x37; /* LORAM=HIRAM=CHAREN=1 -> all ROMs + I/O visible */
}

/* Access to the I/O space $D000-$DFFF, visible when CHAREN=1. */
static uint8_t io_read(uint16_t address)
{
    if (address <= 0xD3FF) return vic2_reg_read((uint8_t)(address & 0x3F));
    if (address <= 0xD7FF) return sid_reg_read((uint8_t)(address & 0x1F));
    if (address <= 0xDBFF) return (uint8_t)(color_ram[address - 0xD800] | 0xF0);
    if (address <= 0xDCFF) return cia1_reg_read((uint8_t)(address & 0x0F));
    if (address <= 0xDDFF) return cia2_reg_read((uint8_t)(address & 0x0F));
    if (address <= 0xDEFF) return cartridge_io1_read(address);
    /* $DF00-$DFFF: REU at $DF00-$DF1F, the rest is unassigned (open bus). */
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
        if (!game || !exrom) return cartridge_read_lorom(address); /* LOROM active */
        return c64_ram[address];
    }

    if (address >= 0xA000 && address <= 0xBFFF) {
        if (!game && exrom) return cartridge_read_hirom(address);  /* 16K mode */
        if (loram && hiram) return rom_basic[address - 0xA000];
        return c64_ram[address];
    }

    if (address >= 0xD000 && address <= 0xDFFF) {
        if (charen) return io_read(address);
        return rom_chargen[address - 0xD000];
    }

    if (address >= 0xE000) {
        if (hiram) {
            /* Cartridge autostart: if a cartridge with a valid "CBM80"
             * signature is present, the 6502 RESET vector points to its
             * cold-start routine instead of the normal KERNAL boot - just
             * like a real C64 KERNAL would detect and redirect it itself. */
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
        c64_ram[address] = val; /* write also visible to raw RAM reads (e.g. via the REU) */
        return;
    }

    uint8_t port = cpu_port_effective();
    bool charen = (port & CPU_PORT_CHAREN) != 0;

    /* RAM under the banked ROM/cartridge windows is always writable
     * (just like on a real C64 - ROM/I-O only shadows the read side). */
    if (address >= 0xD000 && address <= 0xDFFF && charen) {
        io_write(address, val);
        return;
    }

    c64_ram[address] = val;
}
