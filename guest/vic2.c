#include "vic2.h"
#include "c64bus.h"
#include "cia.h"
#include "libc_shim.h"

uint8_t vic2_framebuffer[VIC2_SCREEN_W * VIC2_SCREEN_H];

static uint8_t regs[0x40];
static uint16_t raster_line;
static int line_cycle_acc;
static uint16_t raster_irq_compare;
static uint8_t sprite_fg_collision;   /* akumulowane w trakcie klatki */
static uint8_t sprite_sprite_collision;

/* Maska "ta komorka to piksel pierwszoplanowy" dla calego ekranu - potrzebna
 * do priorytetu sprite/tlo oraz kolizji sprite-tlo. */
static uint8_t fg_mask[VIC2_SCREEN_W * VIC2_SCREEN_H];
/* bitmaska (bity 0-7 = sprite 0-7) sprite'ow, ktore juz narysowaly
 * niepusty piksel w danej komorce w trakcie biezacej klatki - do wykrywania
 * kolizji sprite-sprite. */
static uint8_t sprite_hit_mask[VIC2_SCREEN_W * VIC2_SCREEN_H];

#define REG(x) (regs[(x)])

void vic2_reset(void)
{
    memset(regs, 0, sizeof(regs));
    memset(vic2_framebuffer, 0, sizeof(vic2_framebuffer));
    memset(fg_mask, 0, sizeof(fg_mask));
    raster_line = 0;
    line_cycle_acc = 0;
    raster_irq_compare = 0;
    sprite_fg_collision = 0;
    sprite_sprite_collision = 0;
    REG(0x11) = 0x1B; /* DEN=1, RSEL=1, YSCROLL=3 - stan typowy po resecie */
    REG(0x16) = 0xC8; /* CSEL=1, XSCROLL=0 */
    REG(0x18) = 0x14; /* domyslny wskaznik VM=$0400, CB=$1000 (wzgledem banku) */
    REG(0x20) = 14;   /* jasnoniebieska ramka - domyslny kolor C64 po resecie */
    REG(0x21) = 6;    /* niebieskie tlo */
}

uint8_t vic2_reg_read(uint8_t offset)
{
    offset &= 0x3F;
    switch (offset) {
        case 0x12: return (uint8_t)(raster_line & 0xFF);
        case 0x11: return (uint8_t)((REG(0x11) & 0x7F) | ((raster_line & 0x100) ? 0x80 : 0));
        case 0x19: return (uint8_t)(REG(0x19) | 0x70); /* niezdefiniowane bity = 1 */
        case 0x1E: { uint8_t v = sprite_sprite_collision; sprite_sprite_collision = 0; return v; }
        case 0x1F: { uint8_t v = sprite_fg_collision; sprite_fg_collision = 0; return v; }
    }
    if (offset <= 0x2E) return REG(offset);
    return 0xFF;
}

void vic2_reg_write(uint8_t offset, uint8_t value)
{
    offset &= 0x3F;
    switch (offset) {
        case 0x11:
            REG(0x11) = value;
            raster_irq_compare = (uint16_t)((raster_irq_compare & 0x0FF) | ((value & 0x80) ? 0x100 : 0));
            break;
        case 0x12:
            raster_irq_compare = (uint16_t)((raster_irq_compare & 0x100) | value);
            break;
        case 0x19:
            REG(0x19) &= (uint8_t)~(value & 0x0F); /* zapis 1 kasuje odpowiedni bit */
            break;
        case 0x1A:
            REG(0x1A) = (uint8_t)(value & 0x0F);
            break;
        case 0x1E:
        case 0x1F:
            break; /* rejestry kolizji tylko do odczytu */
        default:
            if (offset <= 0x2E) REG(offset) = value;
            break;
    }
}

bool vic2_irq_pending(void)
{
    return (REG(0x19) & REG(0x1A) & 0x0F) != 0;
}

uint16_t vic2_current_raster_line(void) { return raster_line; }

/* --- dostep do pamieci widzianej przez VIC-II (z podmiana CHARGEN) ------ */

static uint8_t vic2_read_mem(uint16_t bank_base, uint16_t local_addr)
{
    uint16_t abs_addr = (uint16_t)(bank_base + local_addr);
    uint8_t bank = (uint8_t)(bank_base / 0x4000);
    if (bank == 0 && abs_addr >= 0x1000 && abs_addr <= 0x1FFF)
        return rom_chargen[abs_addr - 0x1000];
    if (bank == 2 && abs_addr >= 0x9000 && abs_addr <= 0x9FFF)
        return rom_chargen[abs_addr - 0x9000];
    return c64_ram[abs_addr];
}

/* --- renderowanie pola tekstu/bitmapy ----------------------------------- */

static void render_char_bitmap_layer(uint16_t bank_base)
{
    uint8_t ctrl1 = REG(0x11);
    uint8_t ctrl2 = REG(0x16);
    bool ecm = (ctrl1 & 0x40) != 0;
    bool bmm = (ctrl1 & 0x20) != 0;
    bool mcm = (ctrl2 & 0x10) != 0;
    bool rsel = (ctrl1 & 0x08) != 0;
    bool csel = (ctrl2 & 0x04) != 0;

    int win_h = rsel ? 200 : 192;
    int win_w = csel ? 320 : 304;
    int off_y = (VIC2_SCREEN_H - win_h) / 2;
    int off_x = (VIC2_SCREEN_W - win_w) / 2;

    uint8_t border = (uint8_t)(REG(0x20) & 0x0F);
    for (int i = 0; i < VIC2_SCREEN_W * VIC2_SCREEN_H; i++) {
        vic2_framebuffer[i] = border;
        fg_mask[i] = 0;
    }

    uint16_t vm_base = (uint16_t)(((REG(0x18) >> 4) & 0x0F) * 0x400);
    uint16_t cb_base = (uint16_t)(((REG(0x18) >> 1) & 0x07) * 0x800);
    uint16_t bmp_base = (uint16_t)(((REG(0x18) >> 3) & 0x01) * 0x2000);

    uint8_t bg0 = (uint8_t)(REG(0x21) & 0x0F);
    uint8_t bg1 = (uint8_t)(REG(0x22) & 0x0F);
    uint8_t bg2 = (uint8_t)(REG(0x23) & 0x0F);

    int rows = win_h / 8;
    int cols = win_w / 8;

    for (int row = 0; row < rows && row < 25; row++) {
        for (int col = 0; col < cols && col < 40; col++) {
            uint16_t cell = (uint16_t)(row * 40 + col);
            uint8_t screen_byte = vic2_read_mem(bank_base, (uint16_t)(vm_base + cell));
            uint8_t col_ram_val = (uint8_t)(color_ram[cell] & 0x0F);

            for (int ln = 0; ln < 8; ln++) {
                int py = off_y + row * 8 + ln;
                if (py < 0 || py >= VIC2_SCREEN_H) continue;

                if (bmm) {
                    uint16_t bmp_off = (uint16_t)(bmp_base + cell * 8 + ln);
                    uint8_t data = vic2_read_mem(bank_base, bmp_off);
                    uint8_t hi = (uint8_t)((screen_byte >> 4) & 0x0F);
                    uint8_t lo = (uint8_t)(screen_byte & 0x0F);
                    if (!mcm) {
                        for (int b = 0; b < 8; b++) {
                            int px = off_x + col * 8 + b;
                            if (px < 0 || px >= VIC2_SCREEN_W) continue;
                            bool bit = (data & (0x80 >> b)) != 0;
                            uint8_t colr = bit ? hi : lo;
                            vic2_framebuffer[py * VIC2_SCREEN_W + px] = colr;
                            fg_mask[py * VIC2_SCREEN_W + px] = bit ? 1 : 0;
                        }
                    } else {
                        for (int pair = 0; pair < 4; pair++) {
                            uint8_t bits = (uint8_t)((data >> (6 - pair * 2)) & 0x03);
                            uint8_t colr;
                            bool fg;
                            switch (bits) {
                                case 0: colr = bg0; fg = false; break;
                                case 1: colr = hi;  fg = false; break;
                                case 2: colr = lo;  fg = true;  break;
                                default: colr = col_ram_val; fg = true; break;
                            }
                            for (int sub = 0; sub < 2; sub++) {
                                int px = off_x + col * 8 + pair * 2 + sub;
                                if (px < 0 || px >= VIC2_SCREEN_W) continue;
                                vic2_framebuffer[py * VIC2_SCREEN_W + px] = colr;
                                fg_mask[py * VIC2_SCREEN_W + px] = fg ? 1 : 0;
                            }
                        }
                    }
                } else {
                    uint8_t char_code = screen_byte;
                    uint16_t glyph_addr;
                    uint8_t bg = bg0;
                    if (ecm) {
                        uint8_t bank2 = (uint8_t)((char_code >> 6) & 0x03);
                        char_code &= 0x3F;
                        switch (bank2) { case 0: bg = bg0; break; case 1: bg = bg1; break;
                                         case 2: bg = bg2; break; default: bg = (uint8_t)(REG(0x24) & 0x0F); break; }
                    }
                    glyph_addr = (uint16_t)(cb_base + (uint16_t)char_code * 8 + ln);
                    uint8_t data = vic2_read_mem(bank_base, glyph_addr);

                    if (mcm && (col_ram_val & 0x08)) {
                        for (int pair = 0; pair < 4; pair++) {
                            uint8_t bits = (uint8_t)((data >> (6 - pair * 2)) & 0x03);
                            uint8_t colr; bool fg;
                            switch (bits) {
                                case 0: colr = bg0; fg = false; break;
                                case 1: colr = bg1; fg = false; break;
                                case 2: colr = bg2; fg = false; break;
                                default: colr = (uint8_t)(col_ram_val & 0x07); fg = true; break;
                            }
                            for (int sub = 0; sub < 2; sub++) {
                                int px = off_x + col * 8 + pair * 2 + sub;
                                if (px < 0 || px >= VIC2_SCREEN_W) continue;
                                vic2_framebuffer[py * VIC2_SCREEN_W + px] = colr;
                                fg_mask[py * VIC2_SCREEN_W + px] = fg ? 1 : 0;
                            }
                        }
                    } else {
                        for (int b = 0; b < 8; b++) {
                            int px = off_x + col * 8 + b;
                            if (px < 0 || px >= VIC2_SCREEN_W) continue;
                            bool bit = (data & (0x80 >> b)) != 0;
                            vic2_framebuffer[py * VIC2_SCREEN_W + px] = bit ? col_ram_val : bg;
                            fg_mask[py * VIC2_SCREEN_W + px] = bit ? 1 : 0;
                        }
                    }
                }
            }
        }
    }
}

/* --- sprite'y ------------------------------------------------------------ */

static void render_sprites(uint16_t bank_base)
{
    uint16_t vm_base = (uint16_t)(((REG(0x18) >> 4) & 0x0F) * 0x400);
    memset(sprite_hit_mask, 0, sizeof(sprite_hit_mask));

    for (int s = 7; s >= 0; s--) {
        if (!(REG(0x15) & (1u << s))) continue;

        int x = REG(0x00 + s * 2) | (((REG(0x10) >> s) & 1) << 8);
        int y = REG(0x01 + s * 2);
        bool xexp = (REG(0x1D) & (1u << s)) != 0;
        bool yexp = (REG(0x17) & (1u << s)) != 0;
        bool mc = (REG(0x1C) & (1u << s)) != 0;
        bool behind = (REG(0x1B) & (1u << s)) != 0;
        uint8_t own_color = (uint8_t)(REG(0x27 + s) & 0x0F);
        uint8_t mcol0 = (uint8_t)(REG(0x25) & 0x0F);
        uint8_t mcol1 = (uint8_t)(REG(0x26) & 0x0F);

        uint8_t ptr = vic2_read_mem(bank_base, (uint16_t)(vm_base + 0x3F8 + s));
        uint16_t data_base = (uint16_t)(ptr * 64);

        int screen_x0 = x - 24; /* offset standardowy sprite'ow C64 wzgledem widocznego okna */
        int screen_y0 = y - 50;

        for (int ln = 0; ln < 21; ln++) {
            int lines = yexp ? 2 : 1;
            for (int rep = 0; rep < lines; rep++) {
                int py = screen_y0 + ln * (yexp ? 2 : 1) + rep;
                if (py < 0 || py >= VIC2_SCREEN_H) continue;

                uint32_t b0 = vic2_read_mem(bank_base, (uint16_t)(data_base + ln * 3 + 0));
                uint32_t b1 = vic2_read_mem(bank_base, (uint16_t)(data_base + ln * 3 + 1));
                uint32_t b2 = vic2_read_mem(bank_base, (uint16_t)(data_base + ln * 3 + 2));
                uint32_t bits24 = (b0 << 16) | (b1 << 8) | b2;

                if (!mc) {
                    for (int b = 0; b < 24; b++) {
                        if (!(bits24 & (0x800000u >> b))) continue;
                        int pxw = xexp ? 2 : 1;
                        for (int e = 0; e < pxw; e++) {
                            int px = screen_x0 + b * pxw + e;
                            if (px < 0 || px >= VIC2_SCREEN_W) continue;
                            int idx = py * VIC2_SCREEN_W + px;
                            if (fg_mask[idx]) sprite_fg_collision |= (uint8_t)(1u << s);
                            if (sprite_hit_mask[idx]) sprite_sprite_collision |= (uint8_t)(sprite_hit_mask[idx] | (1u << s));
                            sprite_hit_mask[idx] |= (uint8_t)(1u << s);
                            if (behind && fg_mask[idx]) continue;
                            vic2_framebuffer[idx] = own_color;
                        }
                    }
                } else {
                    for (int pair = 0; pair < 12; pair++) {
                        uint8_t bits = (uint8_t)((bits24 >> (22 - pair * 2)) & 0x03);
                        if (bits == 0) continue;
                        uint8_t colr = (bits == 1) ? mcol0 : (bits == 2) ? own_color : mcol1;
                        int pxw = xexp ? 4 : 2;
                        for (int e = 0; e < pxw; e++) {
                            int px = screen_x0 + pair * pxw + e;
                            if (px < 0 || px >= VIC2_SCREEN_W) continue;
                            int idx = py * VIC2_SCREEN_W + px;
                            if (fg_mask[idx]) sprite_fg_collision |= (uint8_t)(1u << s);
                            if (sprite_hit_mask[idx]) sprite_sprite_collision |= (uint8_t)(sprite_hit_mask[idx] | (1u << s));
                            sprite_hit_mask[idx] |= (uint8_t)(1u << s);
                            if (behind && fg_mask[idx]) continue;
                            vic2_framebuffer[idx] = colr;
                        }
                    }
                }
            }
        }
    }
}

static void render_frame(void)
{
    uint8_t bank = cia2_vic_bank();
    uint16_t bank_base = (uint16_t)(bank * 0x4000);

    if (!(REG(0x11) & 0x10)) { /* DEN=0: ekran wylaczony, samo obramowanie */
        memset(vic2_framebuffer, (uint8_t)(REG(0x20) & 0x0F), sizeof(vic2_framebuffer));
        memset(fg_mask, 0, sizeof(fg_mask));
        return;
    }

    render_char_bitmap_layer(bank_base);
    render_sprites(bank_base);

    if (sprite_sprite_collision) {
        REG(0x19) |= 0x04;
        if (REG(0x1A) & 0x04) REG(0x19) |= 0x80;
    }
    if (sprite_fg_collision) {
        REG(0x19) |= 0x02;
        if (REG(0x1A) & 0x02) REG(0x19) |= 0x80;
    }
}

bool vic2_tick(int cpu_cycles)
{
    line_cycle_acc += cpu_cycles;
    bool frame_done = false;

    while (line_cycle_acc >= VIC2_CYCLES_PER_LINE) {
        line_cycle_acc -= VIC2_CYCLES_PER_LINE;
        raster_line++;

        if (raster_line == raster_irq_compare) {
            REG(0x19) |= 0x01;
            if (REG(0x1A) & 0x01) REG(0x19) |= 0x80;
        }

        if (raster_line >= VIC2_LINES_PER_FRAME) {
            raster_line = 0;
            render_frame();
            frame_done = true;
        }
    }
    return frame_done;
}
