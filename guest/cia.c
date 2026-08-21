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

/* Rejestry wspolne dla jednego ukladu CIA 6526. */
typedef struct {
    uint8_t pra, prb;
    uint8_t ddra, ddrb;
    uint16_t timer_a, timer_a_latch;
    uint16_t timer_b, timer_b_latch;
    uint8_t cra, crb;
    uint8_t icr_mask;      /* maska zatwierdzonych zrodel przerwan (bity 0-4) */
    uint8_t icr_flags;     /* zatrzaskniete, niepotwierdzone zrodla */
    uint8_t tod_10ths, tod_sec, tod_min, tod_hr;
} cia_regs_t;

static cia_regs_t cia1, cia2;
static bool keymatrix[8][8]; /* [row][col], true = wcisniety */

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
    /* bity skonfigurowane jako wejscie "plywaja" wysoko (podciagniecie). */
    return (pr & ddr) | (uint8_t)(~ddr);
}

/* --- macierz klawiatury ------------------------------------------------ */

void cia_keyboard_set(uint8_t row, uint8_t col, bool pressed)
{
    if (row < 8 && col < 8) keymatrix[row][col] = pressed;
}

/* Zwraca bity odczytane po stronie "read_is_row" (true=PRB/rows,
 * false=PRA/cols), gdy strona przeciwna steruje (aktywnie niskim
 * poziomem) liniami wg drive_mask/drive_val. Bit=0 w drive_val przy
 * odpowiadajacym bicie drive_mask=1(wyjscie) oznacza aktywne sterowanie. */
static uint8_t keyboard_scan(bool driving_cols, uint8_t drive_mask, uint8_t drive_val)
{
    uint8_t result = 0xFF;
    for (int d = 0; d < 8; d++) {
        if (!(drive_mask & (1u << d))) continue;      /* linia nieskonfigurowana jako wyjscie */
        if (drive_val & (1u << d)) continue;           /* wyjscie stanu wysokiego - nieaktywne */
        for (int r = 0; r < 8; r++) {
            bool pressed = driving_cols ? keymatrix[r][d] : keymatrix[d][r];
            if (pressed) result &= (uint8_t)~(1u << r);
        }
    }
    return result;
}

/* --- CIA1: klawiatura + timery -> IRQ ---------------------------------- */

uint8_t cia1_reg_read(uint8_t offset)
{
    offset &= 0x0F;
    switch (offset) {
        case 0x00: return effective_port(cia1.pra, cia1.ddra);
        case 0x01: {
            /* Standardowy kierunek skanowania KERNAL-a: PRA steruje kolumnami
             * (wyjscia, aktywne niskim stanem), PRB odczytuje wiersze. */
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
        case 0x0C: return 0x00; /* serial shift - nieuzywane */
        case 0x0D: {
            uint8_t v = cia1.icr_flags;
            cia1.icr_flags = 0; /* odczyt kasuje zatrzask (i linie IRQ) */
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

/* --- CIA2: bank VIC-II + timery -> NMI ---------------------------------- */

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

/* --- timery: wspolna logika co CIA1/CIA2 -------------------------------- */

/* Odlicza jeden 16-bitowy timer o `cycles` cykli. Przy niedomiarze zatrzaskuje
 * zrodlo przerwania, ewentualnie zatrzymuje sie (tryb one-shot, CR bit3=1)
 * albo przeladowuje sie z zatrzasku i liczy dalej (tryb ciagly). */
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
            *timer = latch ? latch : 1; /* zabezpieczenie przed petla przy latch=0 */
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
