/*
 * guest/cartridge.c - implementation of the Simons' BASIC cartridge
 * emulation declared in cartridge.h: cold-start autostart detection,
 * and switching between 16K mode ($8000-$BFFF visible) and 8K mode
 * (freeing $A000-$BFFF back to BASIC RAM) via the I/O1 write protocol.
 *
 * Part of v-c64 - a bare-metal Commodore 64 unikernel running directly
 * on Linux /dev/kvm, with no QEMU involved.
 *
 * Author: Dariusz Nowak <hal.dariusz.nowak@gmail.com>
 */
#include "cartridge.h"
#include "c64bus.h"

/* true = cartridge loaded (rom_simons filled in by kernel_main from the
 * ROM file). If the ROM file wasn't provided in roms/, the array is
 * zero-filled by the linker (.bss) - we treat that as "no cartridge". */
static bool cart_loaded = false;

/* Banking mode: true = 16K (GAME=0/EXROM=1, autostart), false = 8K
 * (GAME=1/EXROM=1, dispatcher only at $8000-$9FFF). */
static bool cart_16k_mode = true;

void cartridge_reset(void)
{
    cart_16k_mode = true;

    if (!cart_loaded) {
        for (uint32_t i = 0; i < ROM_SIMONS_SIZE; i++) {
            if (rom_simons[i] != 0) {
                cart_loaded = true;
                break;
            }
        }
    }
}

bool cartridge_present(void)
{
    return cart_loaded;
}

bool cartridge_game_line(void)
{
    if (!cart_loaded) return true;      /* no cartridge -> GAME inactive (1) */
    return !cart_16k_mode;              /* 16K: GAME=0 (false) ; 8K: GAME=1 (true) */
}

bool cartridge_exrom_line(void)
{
    if (!cart_loaded) return true;      /* no cartridge -> EXROM inactive (1) */
    /* GAME=0/EXROM=1 = 16K mode (LOROM+HIROM), GAME=1/EXROM=0 = 8K mode. */
    return cart_16k_mode;
}

uint8_t cartridge_read_lorom(uint16_t address)
{
    if (!cart_loaded) return 0xFF;
    uint32_t off = (uint32_t)(address - 0x8000u);
    return rom_simons[off & (ROM_SIMONS_SIZE - 1)];
}

uint8_t cartridge_read_hirom(uint16_t address)
{
    if (!cart_loaded) return 0xFF;
    /* Second half of the 16 KB ROM image, mapped at $A000-$BFFF. */
    uint32_t off = 0x2000u + (uint32_t)(address - 0xA000u);
    return rom_simons[off & (ROM_SIMONS_SIZE - 1)];
}

uint8_t cartridge_io1_read(uint16_t address)
{
    (void)address;
    return 0xFF; /* Simons' BASIC doesn't provide a readback on I/O1 */
}

void cartridge_io1_write(uint16_t address, uint8_t value)
{
    (void)address;
    (void)value;
    /* Any write to $DE00-$DEFF switches the cartridge from 16K to 8K mode,
     * exactly as on the original hardware. */
    cart_16k_mode = false;
}

bool cartridge_autostart_vector(uint16_t *out_addr)
{
    if (!cart_loaded) return false;
    /* Standard C64 cartridge header: $8000/8001 = cold-start vector,
     * $8002/8003 = NMI vector, $8004-8008 = ASCII "CBM80" signature
     * confirming the vectors are valid (otherwise the KERNAL ignores them
     * and boots normally into BASIC). */
    static const uint8_t sig[5] = { 'C', 'B', 'M', '8', '0' };
    for (int i = 0; i < 5; i++)
        if (rom_simons[4 + i] != sig[i]) return false;

    *out_addr = (uint16_t)(rom_simons[0] | (rom_simons[1] << 8));
    return true;
}
