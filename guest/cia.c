/*
 * guest/cia.c - implementation of the CIA 1/2 emulation declared in
 * cia.h: Timer A/B countdown and interrupt generation, the 8x8 keyboard
 * matrix scan, and the CIA2 Port A VIC-II bank-select bits.
 *
 * Part of v-c64 - a bare-metal Commodore 64 unikernel running directly
 * on Linux /dev/kvm, with no QEMU involved.
 *
 * Author: Dariusz Nowak <hal.dariusz.nowak@gmail.com>
 */
#include "cia.h"
#include "libc_shim.h"

/* Registers common to a single CIA 6526 chip. */
typedef struct {
    uint8_t pra, prb;
    uint8_t ddra, ddrb;
    uint16_t timer_a, timer_a_latch;
    uint16_t timer_b, timer_b_latch;
    uint8_t cra, crb;
    uint8_t icr_mask;      /* mask of enabled interrupt sources (bits 0-4) */
    uint8_t icr_flags;     /* latched, unacknowledged sources */
    uint8_t tod_10ths, tod_sec, tod_min, tod_hr;
} cia_regs_t;

static cia_regs_t cia1, cia2;
static bool keymatrix[8][8]; /* [row][col], true = pressed */

#define ICR_TA   0x01
#define ICR_TB   0x02
#define ICR_TOD  0x04
#define ICR_SP   0x08
#define ICR_FLG  0x10
#define ICR_IR   0x80

static void cia_regs_reset(cia_regs_t *c)
{
    memset(c, 0, sizeof(*c));
    c->timer_a = c->timer_a_latch = 0xFFFF;
    c->timer_b = c->timer_b_latch = 0xFFFF;
}

void cia_reset(void)
{
    cia_regs_reset(&cia1);
    cia_regs_reset(&cia2);
    memset(keymatrix, 0, sizeof(keymatrix));
}

static uint8_t effective_port(uint8_t pr, uint8_t ddr)
{
    /* bits configured as input "float" high (pull-up). */
    return (pr & ddr) | (uint8_t)(~ddr);
}

/* --- keyboard matrix ---------------------------------------------------- */

void cia_keyboard_set(uint8_t row, uint8_t col, bool pressed)
{
    if (row < 8 && col < 8) keymatrix[row][col] = pressed;
}

/* Returns the bits read on the "driving_cols" side (true=PRB/rows,
 * false=PRA/cols), while the opposite side drives (actively low) its
 * lines per drive_mask/drive_val. A 0 bit in drive_val at a position
 * where drive_mask=1 (output) means that line is actively driven. */
static uint8_t keyboard_scan(bool driving_cols, uint8_t drive_mask, uint8_t drive_val)
{
    uint8_t result = 0xFF;
    for (int d = 0; d < 8; d++) {
        if (!(drive_mask & (1u << d))) continue;      /* line not configured as output */
        if (drive_val & (1u << d)) continue;           /* driven high - inactive */
        for (int r = 0; r < 8; r++) {
            bool pressed = driving_cols ? keymatrix[r][d] : keymatrix[d][r];
            if (pressed) result &= (uint8_t)~(1u << r);
        }
    }
    return result;
}

/* --- CIA1: keyboard + timers -> IRQ -------------------------------------- */

uint8_t cia1_reg_read(uint8_t offset)
{
    offset &= 0x0F;
    switch (offset) {
        case 0x00: return effective_port(cia1.pra, cia1.ddra);
        case 0x01: {
            /* The KERNAL's standard scan direction: PRA drives the columns
             * (outputs, active low), PRB reads back the rows. */
            uint8_t rows = keyboard_scan(true, cia1.ddra, cia1.pra);
            return rows & effective_port(cia1.prb, cia1.ddrb);
        }
        case 0x02: return cia1.ddra;
        case 0x03: return cia1.ddrb;
        case 0x04: return (uint8_t)(cia1.timer_a & 0xFF);
        case 0x05: return (uint8_t)(cia1.timer_a >> 8);
        case 0x06: return (uint8_t)(cia1.timer_b & 0xFF);
        case 0x07: return (uint8_t)(cia1.timer_b >> 8);
        case 0x08: return cia1.tod_10ths;
        case 0x09: return cia1.tod_sec;
        case 0x0A: return cia1.tod_min;
        case 0x0B: return cia1.tod_hr;
        case 0x0C: return 0x00; /* serial shift - unused */
        case 0x0D: {
            uint8_t v = cia1.icr_flags;
            cia1.icr_flags = 0; /* reading clears the latch (and the IRQ line) */
            return v;
        }
        case 0x0E: return cia1.cra;
        case 0x0F: return cia1.crb;
    }
    return 0xFF;
}

void cia1_reg_write(uint8_t offset, uint8_t value)
{
    offset &= 0x0F;
    switch (offset) {
        case 0x00: cia1.pra = value; break;
        case 0x01: cia1.prb = value; break;
        case 0x02: cia1.ddra = value; break;
        case 0x03: cia1.ddrb = value; break;
        case 0x04: cia1.timer_a_latch = (uint16_t)((cia1.timer_a_latch & 0xFF00) | value); break;
        case 0x05: cia1.timer_a_latch = (uint16_t)((cia1.timer_a_latch & 0x00FF) | (value << 8));
                   cia1.timer_a = cia1.timer_a_latch; break;
        case 0x06: cia1.timer_b_latch = (uint16_t)((cia1.timer_b_latch & 0xFF00) | value); break;
        case 0x07: cia1.timer_b_latch = (uint16_t)((cia1.timer_b_latch & 0x00FF) | (value << 8));
                   cia1.timer_b = cia1.timer_b_latch; break;
        case 0x08: cia1.tod_10ths = value; break;
        case 0x09: cia1.tod_sec = value; break;
        case 0x0A: cia1.tod_min = value; break;
        case 0x0B: cia1.tod_hr = value; break;
        case 0x0C: break;
        case 0x0D:
            if (value & ICR_IR) cia1.icr_mask |= (uint8_t)(value & 0x1F);
            else cia1.icr_mask &= (uint8_t)~(value & 0x1F);
            break;
        case 0x0E:
            cia1.cra = value;
            if (value & 0x10) cia1.timer_a = cia1.timer_a_latch; /* force load */
            break;
        case 0x0F:
            cia1.crb = value;
            if (value & 0x10) cia1.timer_b = cia1.timer_b_latch;
            break;
    }
}

/* --- CIA2: VIC-II bank + timers -> NMI ------------------------------------ */

uint8_t cia2_reg_read(uint8_t offset)
{
    offset &= 0x0F;
    switch (offset) {
        case 0x00: return effective_port(cia2.pra, cia2.ddra);
        case 0x01: return effective_port(cia2.prb, cia2.ddrb);
        case 0x02: return cia2.ddra;
        case 0x03: return cia2.ddrb;
        case 0x04: return (uint8_t)(cia2.timer_a & 0xFF);
        case 0x05: return (uint8_t)(cia2.timer_a >> 8);
        case 0x06: return (uint8_t)(cia2.timer_b & 0xFF);
        case 0x07: return (uint8_t)(cia2.timer_b >> 8);
        case 0x08: return cia2.tod_10ths;
        case 0x09: return cia2.tod_sec;
        case 0x0A: return cia2.tod_min;
        case 0x0B: return cia2.tod_hr;
        case 0x0C: return 0x00;
        case 0x0D: {
            uint8_t v = cia2.icr_flags;
            cia2.icr_flags = 0;
            return v;
        }
        case 0x0E: return cia2.cra;
        case 0x0F: return cia2.crb;
    }
    return 0xFF;
}

void cia2_reg_write(uint8_t offset, uint8_t value)
{
    offset &= 0x0F;
    switch (offset) {
        case 0x00: cia2.pra = value; break;
        case 0x01: cia2.prb = value; break;
        case 0x02: cia2.ddra = value; break;
        case 0x03: cia2.ddrb = value; break;
        case 0x04: cia2.timer_a_latch = (uint16_t)((cia2.timer_a_latch & 0xFF00) | value); break;
        case 0x05: cia2.timer_a_latch = (uint16_t)((cia2.timer_a_latch & 0x00FF) | (value << 8));
                   cia2.timer_a = cia2.timer_a_latch; break;
        case 0x06: cia2.timer_b_latch = (uint16_t)((cia2.timer_b_latch & 0xFF00) | value); break;
        case 0x07: cia2.timer_b_latch = (uint16_t)((cia2.timer_b_latch & 0x00FF) | (value << 8));
                   cia2.timer_b = cia2.timer_b_latch; break;
        case 0x08: cia2.tod_10ths = value; break;
        case 0x09: cia2.tod_sec = value; break;
        case 0x0A: cia2.tod_min = value; break;
        case 0x0B: cia2.tod_hr = value; break;
        case 0x0C: break;
        case 0x0D:
            if (value & ICR_IR) cia2.icr_mask |= (uint8_t)(value & 0x1F);
            else cia2.icr_mask &= (uint8_t)~(value & 0x1F);
            break;
        case 0x0E:
            cia2.cra = value;
            if (value & 0x10) cia2.timer_a = cia2.timer_a_latch;
            break;
        case 0x0F:
            cia2.crb = value;
            if (value & 0x10) cia2.timer_b = cia2.timer_b_latch;
            break;
    }
}

uint8_t cia2_vic_bank(void)
{
    uint8_t pra = effective_port(cia2.pra, cia2.ddra);
    return (uint8_t)(3 - (pra & 0x03));
}

/* --- timers: logic shared by CIA1/CIA2 ------------------------------------ */

/* Counts down one 16-bit timer by `cycles` cycles. On underflow, it latches
 * the interrupt source, then either stops (one-shot mode, CR bit3=1) or
 * reloads from the latch and keeps counting (continuous mode). */
static void timer_tick_one(uint16_t *timer, uint16_t latch, uint8_t *cr,
                            uint8_t icr_bit, cia_regs_t *c, int cycles)
{
    if (!(*cr & 0x01)) return;
    while (cycles > 0) {
        if (*timer > (uint16_t)cycles) {
            *timer = (uint16_t)(*timer - cycles);
            cycles = 0;
        } else {
            cycles -= *timer;
            c->icr_flags |= icr_bit;
            if (c->icr_mask & icr_bit) c->icr_flags |= ICR_IR;
            if (*cr & 0x08) { *cr &= (uint8_t)~0x01; *timer = latch; break; }
            *timer = latch ? latch : 1; /* guard against an infinite loop when latch=0 */
        }
    }
}

static void timer_tick(cia_regs_t *c, int cycles)
{
    timer_tick_one(&c->timer_a, c->timer_a_latch, &c->cra, ICR_TA, c, cycles);
    timer_tick_one(&c->timer_b, c->timer_b_latch, &c->crb, ICR_TB, c, cycles);
}

void cia_tick(int cpu_cycles)
{
    timer_tick(&cia1, cpu_cycles);
    timer_tick(&cia2, cpu_cycles);
}

bool cia1_irq_pending(void) { return (cia1.icr_flags & ICR_IR) != 0; }
bool cia2_nmi_pending(void) { return (cia2.icr_flags & ICR_IR) != 0; }
