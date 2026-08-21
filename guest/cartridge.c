#include "cartridge.h"
#include "c64bus.h"

/* true = kartridz zaladowany (rom_simons wypelniony przez kernel_main z pliku
 * ROM). Jesli plik ROM nie zostal dostarczony w roms/, tablica jest wypelniona
 * zerami przez linker (.bss) - traktujemy to jako "brak kartridza". */
static bool cart_loaded = false;

/* Tryb bankowania: true = 16K (GAME=0/EXROM=1, autostart), false = 8K
 * (GAME=1/EXROM=1, tylko dispatcher pod $8000-$9FFF). */
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
    if (!cart_loaded) return true;      /* brak kartridza -> GAME nieaktywne (1) */
    return !cart_16k_mode;              /* 16K: GAME=0 (false) ; 8K: GAME=1 (true) */
}

bool cartridge_exrom_line(void)
{
    if (!cart_loaded) return true;      /* brak kartridza -> EXROM nieaktywne (1) */
    /* GAME=0/EXROM=1 = tryb 16K (LOROM+HIROM), GAME=1/EXROM=0 = tryb 8K. */
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
    /* Druga polowa 16 KB obrazu ROM mapowana pod $A000-$BFFF. */
    uint32_t off = 0x2000u + (uint32_t)(address - 0xA000u);
    return rom_simons[off & (ROM_SIMONS_SIZE - 1)];
}

uint8_t cartridge_io1_read(uint16_t address)
{
    (void)address;
    return 0xFF; /* Simons' BASIC nie udostepnia odczytu w I/O1 */
}

void cartridge_io1_write(uint16_t address, uint8_t value)
{
    (void)address;
    (void)value;
    /* Kazdy zapis pod $DE00-$DEFF przelacza kartridz z trybu 16K na 8K,
     * dokladnie tak jak w oryginalnym sprzecie. */
    cart_16k_mode = false;
}

bool cartridge_autostart_vector(uint16_t *out_addr)
{
    if (!cart_loaded) return false;
    /* Standardowy naglowek kartridza C64: $8000/8001 = wektor cold-start,
     * $8002/8003 = wektor NMI, $8004-8008 = sygnatura ASCII "CBM80"
     * potwierdzajaca, ze wektory sa prawidlowe (inaczej KERNAL je ignoruje
     * i startuje normalnie do BASIC-a). */
    static const uint8_t sig[5] = { 'C', 'B', 'M', '8', '0' };
    for (int i = 0; i < 5; i++)
        if (rom_simons[4 + i] != sig[i]) return false;

    *out_addr = (uint16_t)(rom_simons[0] | (rom_simons[1] << 8));
    return true;
}
