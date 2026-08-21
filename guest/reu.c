/*
 * guest/reu.c - implementation of the 128 KB REU DMA controller declared
 * in reu.h: the $DF00-$DF1F register set, and the C64->REU, REU->C64,
 * and Swap DMA transfer operations against the expansion RAM buffer.
 *
 * Part of v-c64 - a bare-metal Commodore 64 unikernel running directly
 * on Linux /dev/kvm, with no QEMU involved.
 *
 * Author: Dariusz Nowak <hal.dariusz.nowak@gmail.com>
 */
#include "reu.h"
#include "c64bus.h"
#include "libc_shim.h"

static uint8_t reu_ram[REU_SIZE];

static uint8_t status_reg;      /* $DF00 (odczyt) */
static uint8_t command_reg;     /* $DF01 */
static uint16_t c64_addr;       /* $DF02/$DF03 */
static uint16_t reu_addr16;     /* $DF04/$DF05 */
static uint8_t reu_bank;        /* $DF06 bity 0-2 */
static uint16_t xfer_len;       /* $DF07/$DF08 - 0 oznacza 65536 (jak w oryginale) */
static uint8_t irq_mask;        /* $DF09 */
static uint8_t addr_ctrl;       /* $DF0A */

void reu_reset(void)
{
    memset(reu_ram, 0, sizeof(reu_ram));
    status_reg = 0x10; /* wersja chipu / brak zdarzen */
    command_reg = 0xFF;
    c64_addr = 0;
    reu_addr16 = 0;
    reu_bank = 0;
    xfer_len = 0;
    irq_mask = 0;
    addr_ctrl = 0;
}

static uint32_t reu_effective_addr(void)
{
    return (((uint32_t)(reu_bank & 0x07) << 16) | reu_addr16) & (REU_SIZE - 1);
}

static void do_transfer(void)
{
    uint32_t len = xfer_len ? xfer_len : 65536u;
    uint8_t type = command_reg & 0x03; /* 00=C64->REU, 01=REU->C64, 10=swap, 11=verify */
    uint16_t c_addr = c64_addr;
    uint32_t r_addr = reu_effective_addr();
    bool fault = false;

    for (uint32_t i = 0; i < len; i++) {
        switch (type) {
            case 0x00: /* store: C64 -> REU */
                reu_ram[r_addr & (REU_SIZE - 1)] = c64_ram[c_addr];
                break;
            case 0x01: /* load: REU -> C64 */
                c64_ram[c_addr] = reu_ram[r_addr & (REU_SIZE - 1)];
                break;
            case 0x02: { /* swap */
                uint8_t t = c64_ram[c_addr];
                c64_ram[c_addr] = reu_ram[r_addr & (REU_SIZE - 1)];
                reu_ram[r_addr & (REU_SIZE - 1)] = t;
                break;
            }
            case 0x03: /* verify */
            default:
                if (c64_ram[c_addr] != reu_ram[r_addr & (REU_SIZE - 1)]) fault = true;
                break;
        }
        if (!(addr_ctrl & 0x40)) c_addr = (uint16_t)(c_addr + 1);
        if (!(addr_ctrl & 0x80)) r_addr = r_addr + 1;
    }

    if (!(addr_ctrl & 0x40)) c64_addr = c_addr;
    if (!(addr_ctrl & 0x80)) { reu_addr16 = (uint16_t)(r_addr & 0xFFFF); reu_bank = (uint8_t)((r_addr >> 16) & 0x07); }

    status_reg |= 0x40; /* End of Block */
    if (fault) status_reg |= 0x20;
    if (irq_mask & 0x80) status_reg |= 0x80; /* zglos IRQ, jesli wlaczony */

    command_reg &= (uint8_t)~0x80; /* Execute skasowany po zakonczeniu */
}

uint8_t reu_reg_read(uint8_t offset)
{
    offset &= 0x1F;
    switch (offset) {
        case 0x00: { uint8_t v = status_reg; status_reg &= 0x1F; return v; } /* odczyt kasuje bity zdarzen */
        case 0x01: return command_reg;
        case 0x02: return (uint8_t)(c64_addr & 0xFF);
        case 0x03: return (uint8_t)(c64_addr >> 8);
        case 0x04: return (uint8_t)(reu_addr16 & 0xFF);
        case 0x05: return (uint8_t)(reu_addr16 >> 8);
        case 0x06: return (uint8_t)(reu_bank | 0xF8);
        case 0x07: return (uint8_t)(xfer_len & 0xFF);
        case 0x08: return (uint8_t)(xfer_len >> 8);
        case 0x09: return irq_mask;
        case 0x0A: return addr_ctrl;
    }
    return 0xFF;
}

void reu_reg_write(uint8_t offset, uint8_t value)
{
    offset &= 0x1F;
    switch (offset) {
        case 0x01:
            command_reg = value;
            if (value & 0x80) do_transfer();
            break;
        case 0x02: c64_addr = (uint16_t)((c64_addr & 0xFF00) | value); break;
        case 0x03: c64_addr = (uint16_t)((c64_addr & 0x00FF) | (value << 8)); break;
        case 0x04: reu_addr16 = (uint16_t)((reu_addr16 & 0xFF00) | value); break;
        case 0x05: reu_addr16 = (uint16_t)((reu_addr16 & 0x00FF) | (value << 8)); break;
        case 0x06: reu_bank = (uint8_t)(value & 0x07); break;
        case 0x07: xfer_len = (uint16_t)((xfer_len & 0xFF00) | value); break;
        case 0x08: xfer_len = (uint16_t)((xfer_len & 0x00FF) | (value << 8)); break;
        case 0x09: irq_mask = value; break;
        case 0x0A: addr_ctrl = value; break;
        default: break;
    }
}

bool reu_irq_pending(void)
{
    return (status_reg & 0x80) != 0;
}
