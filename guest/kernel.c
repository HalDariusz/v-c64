/*
 * guest/kernel.c - main execution loop of the bare-metal C64 guest.
 *
 * Initializes every emulated chip, single-steps the 6502 CPU core
 * (fake6502), times VIC-II/CIA/SID off the CPU cycles actually consumed,
 * dispatches IRQ/NMI interrupts, reads the keyboard (host port 0x60),
 * intercepts KERNAL LOAD/SAVE for the instant .PRG/.D64 fast-loader
 * (ports 0x5007/0x5008), streams SID audio to the host (port 0x5006),
 * drives the SID hypercall player loader ($F700, see do_sid_load()), and
 * writes diagnostic output to the host console (port 0x5001).
 *
 * Part of v-c64 - a bare-metal Commodore 64 unikernel running directly
 * on Linux /dev/kvm, with no QEMU involved.
 *
 * Author: Dariusz Nowak <hal.dariusz.nowak@gmail.com>
 */
#include "kernel.h"
#include "fake6502.h"
#include "memory_pla.h"
#include "c64bus.h"
#include "vic2.h"
#include "cia.h"
#include "sid.h"
#include "reu.h"
#include "cartridge.h"
#include "libc_shim.h"
#include <stdint.h>
#include <stdbool.h>

/* ROM-y wygenerowane przez regule Makefile: xxd -i -n <nazwa>_file, katalog roms */
#include "rom_kernal.h"
#include "rom_basic.h"
#include "rom_chargen.h"
#include "rom_simons.h"

/* --- minimalna libc (freestanding: brak libc do zlinkowania) ------------- */

void *memset(void *dst, int val, unsigned int n)
{
    uint8_t *p = (uint8_t *)dst;
    for (unsigned int i = 0; i < n; i++) p[i] = (uint8_t)val;
    return dst;
}

void *memcpy(void *dst, const void *src, unsigned int n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (unsigned int i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

/* --- porty I/O protokolu hipernadzorca <-> gosc (patrz host/kvm_host.c) -- */

#define PORT_KEYBOARD       0x60   /* IN byte: kolejny skankod x86 (0 = brak) */
#define PORT_DEBUG_CONSOLE  0x5001 /* OUT byte: log diagnostyczny na terminalu hosta */
#define PORT_AUDIO_SAMPLE   0x5006 /* OUT word: probka SID (16-bit signed) */
#define PORT_FASTLOAD_CMD   0x5007 /* OUT: 1=nazwa zaczyna sie,2=nazwa kompletna; IN: status/dlugosc/adres */
#define PORT_FASTLOAD_DATA  0x5008 /* OUT: kolejny bajt nazwy pliku; IN: kolejny bajt danych */

static inline void outb(uint16_t port, uint8_t val) { asm volatile("outb %0,%1" :: "a"(val), "Nd"(port)); }
static inline void outw(uint16_t port, uint16_t val) { asm volatile("outw %0,%1" :: "a"(val), "Nd"(port)); }
static inline uint8_t inb(uint16_t port) { uint8_t r; asm volatile("inb %1,%0" : "=a"(r) : "Nd"(port)); return r; }

static void debug_puts(const char *s)
{
    while (*s) outb(PORT_DEBUG_CONSOLE, (uint8_t)*s++);
}

static void debug_puthex16(uint16_t v)
{
    static const char hex[] = "0123456789ABCDEF";
    char buf[5];
    for (int i = 3; i >= 0; i--) { buf[i] = hex[v & 0xF]; v >>= 4; }
    buf[4] = 0;
    debug_puts(buf);
}

/* --- fast-loader: przechwycenie procedury KERNAL LOAD ($F4A5) ------------ */

static void do_fast_load(fake6502_context *cpu)
{
    uint8_t fnlen = c64_ram[0xB7];
    uint16_t fnaddr = (uint16_t)(c64_ram[0xBB] | (c64_ram[0xBC] << 8));

    outb(PORT_FASTLOAD_CMD, 0x01); /* rozpocznij transmisje nazwy pliku */
    for (uint8_t i = 0; i < fnlen; i++)
        outb(PORT_FASTLOAD_DATA, fake6502_mem_read(cpu, (uint16_t)(fnaddr + i)));
    outb(PORT_FASTLOAD_CMD, 0x02); /* nazwa kompletna - host wykonuje zaladowanie */

    uint8_t status = inb(PORT_FASTLOAD_CMD);
    uint16_t ret_addr = fake6502_pull_16(cpu); /* symulacja RTS z JSR $FFD5 */

    if (!status) {
        debug_puts("LOAD: file not found\n");
        fake6502_carry_set(cpu);
        cpu->cpu.pc = (uint16_t)(ret_addr + 1);
        return;
    }

    uint16_t length = (uint16_t)(inb(PORT_FASTLOAD_DATA) | (inb(PORT_FASTLOAD_DATA) << 8));
    uint16_t load_addr = (uint16_t)(inb(PORT_FASTLOAD_DATA) | (inb(PORT_FASTLOAD_DATA) << 8));

    /* Adres docelowy zawsze z naglowka pliku .prg (dziala poprawnie dla
     * zdecydowanej wiekszosci realnych przypadkow uzycia - LOAD"name",8 -
     * bez probowania odtwarzac niuansow adresu wtornego $B9=1 wzgledem
     * niepewnie zdefiniowanego wskaznika w stronie zerowej). */
    uint16_t dest = load_addr;

    for (uint16_t i = 0; i < length; i++)
        c64_ram[(uint16_t)(dest + i)] = inb(PORT_FASTLOAD_DATA);

    uint16_t end_addr = (uint16_t)(dest + length);
    cpu->cpu.x = (uint8_t)(end_addr & 0xFF);
    cpu->cpu.y = (uint8_t)(end_addr >> 8);
    fake6502_carry_clear(cpu);
    cpu->cpu.pc = (uint16_t)(ret_addr + 1);

    /* VARTAB ($2D/$2E): tak jak prawdziwy KERNAL, przesuwamy poczatek
     * tablicy zmiennych BASIC-a tuz za zaladowany program. */
    c64_ram[0x2D] = (uint8_t)(end_addr & 0xFF);
    c64_ram[0x2E] = (uint8_t)(end_addr >> 8);

    debug_puts("LOAD: OK, end=$");
    debug_puthex16(end_addr);
    debug_puts("\n");
}

/* --- fast-saver: przechwycenie procedury KERNAL SAVE ($F5DD) ------------- */

static void do_fast_save(fake6502_context *cpu)
{
    uint8_t fnlen = c64_ram[0xB7];
    uint16_t fnaddr = (uint16_t)(c64_ram[0xBB] | (c64_ram[0xBC] << 8));

    /* Konwencja wywolania KERNAL SAVE: A = adres strony zerowej
     * wskazujacy 2-bajtowy adres startu danych, X/Y = adres koncowy. */
    uint8_t zp_ptr = cpu->cpu.a;
    uint16_t start_addr = (uint16_t)(c64_ram[zp_ptr] | (c64_ram[(uint8_t)(zp_ptr + 1)] << 8));
    uint16_t end_addr = (uint16_t)(cpu->cpu.x | (cpu->cpu.y << 8));
    uint16_t length = (uint16_t)(end_addr - start_addr);

    outb(PORT_FASTLOAD_CMD, 0x03); /* rozpocznij transmisje nazwy pliku (SAVE) */
    for (uint8_t i = 0; i < fnlen; i++)
        outb(PORT_FASTLOAD_DATA, fake6502_mem_read(cpu, (uint16_t)(fnaddr + i)));
    outb(PORT_FASTLOAD_CMD, 0x04); /* nazwa kompletna - teraz naglowek + dane */
    outb(PORT_FASTLOAD_DATA, (uint8_t)(start_addr & 0xFF));
    outb(PORT_FASTLOAD_DATA, (uint8_t)(start_addr >> 8));
    outb(PORT_FASTLOAD_DATA, (uint8_t)(length & 0xFF));
    outb(PORT_FASTLOAD_DATA, (uint8_t)(length >> 8));
    for (uint16_t i = 0; i < length; i++)
        outb(PORT_FASTLOAD_DATA, c64_ram[(uint16_t)(start_addr + i)]);
    outb(PORT_FASTLOAD_CMD, 0x05); /* wykonaj zapis na hoscie */

    uint8_t status = inb(PORT_FASTLOAD_CMD);
    uint16_t ret_addr = fake6502_pull_16(cpu);
    if (status) fake6502_carry_clear(cpu); else fake6502_carry_set(cpu);
    cpu->cpu.pc = (uint16_t)(ret_addr + 1);

    debug_puts(status ? "SAVE: OK\n" : "SAVE: error\n");
}

/* --- SID-load: wlasny "hypercall" (PC == $F700, nie jest to prawdziwa
 * procedura KERNAL) do wczytywania plikow PSID/RSID. Host parsuje naglowek
 * i zwraca gotowe adresy load/init/play + liczbe utworow, ktore ladujemy do
 * "skrzynki pocztowej" w stronie zerowej ($02-$0A) na potrzeby programu goscia
 * (patrz tools/sidplayer). Uzywa tej samej konwencji nazwy pliku co LOAD
 * ($B7/$BB/$BC), zeby program-gosc mogl ja ustawiac identycznie jak dla LOAD. */
#define SID_MB_LOAD   0x02  /* +0/+1: adres zaladowania danych */
#define SID_MB_INIT   0x04  /* +0/+1: adres rutyny init */
#define SID_MB_PLAY   0x06  /* +0/+1: adres rutyny play (0 = utwor sam instaluje IRQ) */
#define SID_MB_SONGS  0x08  /* liczba podutworow */
#define SID_MB_START  0x09  /* domyslny podutwor, juz 0-based (do wpisania w A przy init) */
#define SID_MB_STATUS 0x0A  /* 1 = OK, 0 = blad/nie znaleziono */

static void do_sid_load(fake6502_context *cpu)
{
    uint8_t fnlen = c64_ram[0xB7];
    uint16_t fnaddr = (uint16_t)(c64_ram[0xBB] | (c64_ram[0xBC] << 8));

    outb(PORT_FASTLOAD_CMD, 0x06); /* SIDLOAD: rozpocznij nazwe pliku */
    for (uint8_t i = 0; i < fnlen; i++)
        outb(PORT_FASTLOAD_DATA, fake6502_mem_read(cpu, (uint16_t)(fnaddr + i)));
    outb(PORT_FASTLOAD_CMD, 0x07); /* SIDLOAD: wykonaj */

    uint8_t status = inb(PORT_FASTLOAD_CMD);
    c64_ram[SID_MB_STATUS] = status;

    if (status) {
        uint16_t load_addr = (uint16_t)(inb(PORT_FASTLOAD_DATA) | (inb(PORT_FASTLOAD_DATA) << 8));
        uint16_t init_addr = (uint16_t)(inb(PORT_FASTLOAD_DATA) | (inb(PORT_FASTLOAD_DATA) << 8));
        uint16_t play_addr = (uint16_t)(inb(PORT_FASTLOAD_DATA) | (inb(PORT_FASTLOAD_DATA) << 8));
        uint8_t songs = inb(PORT_FASTLOAD_DATA);
        uint8_t start_song = inb(PORT_FASTLOAD_DATA);
        uint16_t length = (uint16_t)(inb(PORT_FASTLOAD_DATA) | (inb(PORT_FASTLOAD_DATA) << 8));

        for (uint16_t i = 0; i < length; i++)
            c64_ram[(uint16_t)(load_addr + i)] = inb(PORT_FASTLOAD_DATA);

        c64_ram[SID_MB_LOAD]     = (uint8_t)(load_addr & 0xFF);
        c64_ram[SID_MB_LOAD + 1] = (uint8_t)(load_addr >> 8);
        c64_ram[SID_MB_INIT]     = (uint8_t)(init_addr & 0xFF);
        c64_ram[SID_MB_INIT + 1] = (uint8_t)(init_addr >> 8);
        c64_ram[SID_MB_PLAY]     = (uint8_t)(play_addr & 0xFF);
        c64_ram[SID_MB_PLAY + 1] = (uint8_t)(play_addr >> 8);
        c64_ram[SID_MB_SONGS]    = songs;
        c64_ram[SID_MB_START]    = start_song;

        debug_puts("SIDLOAD: OK, init=$");
        debug_puthex16(init_addr);
        debug_puts(" play=$");
        debug_puthex16(play_addr);
        debug_puts("\n");
    } else {
        debug_puts("SIDLOAD: error\n");
    }

    uint16_t ret_addr = fake6502_pull_16(cpu);
    if (status) fake6502_carry_clear(cpu); else fake6502_carry_set(cpu);
    cpu->cpu.pc = (uint16_t)(ret_addr + 1);
}

/* --- klawiatura: skankody x86 (Set 1) -> macierz 8x8 C64 ----------------- */

typedef struct { uint8_t row, col; } key_map_t;
#define KM_NONE ((key_map_t){0xFF, 0xFF})

static key_map_t scancode_to_c64(uint8_t sc)
{
    switch (sc) {
        case 0x1E: return (key_map_t){1, 2}; /* A */
        case 0x30: return (key_map_t){3, 4}; /* B */
        case 0x2E: return (key_map_t){2, 4}; /* C */
        case 0x20: return (key_map_t){2, 2}; /* D */
        case 0x12: return (key_map_t){1, 6}; /* E */
        case 0x21: return (key_map_t){2, 5}; /* F */
        case 0x22: return (key_map_t){3, 2}; /* G */
        case 0x23: return (key_map_t){3, 5}; /* H */
        case 0x17: return (key_map_t){4, 1}; /* I */
        case 0x24: return (key_map_t){4, 2}; /* J */
        case 0x25: return (key_map_t){4, 5}; /* K */
        case 0x26: return (key_map_t){5, 2}; /* L */
        case 0x32: return (key_map_t){4, 4}; /* M */
        case 0x31: return (key_map_t){4, 7}; /* N */
        case 0x18: return (key_map_t){4, 6}; /* O */
        case 0x19: return (key_map_t){5, 1}; /* P */
        case 0x10: return (key_map_t){7, 6}; /* Q */
        case 0x13: return (key_map_t){2, 1}; /* R */
        case 0x1F: return (key_map_t){1, 5}; /* S */
        case 0x14: return (key_map_t){2, 6}; /* T */
        case 0x16: return (key_map_t){3, 6}; /* U */
        case 0x2F: return (key_map_t){3, 7}; /* V */
        case 0x11: return (key_map_t){1, 1}; /* W */
        case 0x2D: return (key_map_t){2, 7}; /* X */
        case 0x15: return (key_map_t){3, 1}; /* Y */
        case 0x2C: return (key_map_t){1, 4}; /* Z */

        case 0x02: return (key_map_t){7, 0}; /* 1 */
        case 0x03: return (key_map_t){7, 3}; /* 2 */
        case 0x04: return (key_map_t){1, 0}; /* 3 */
        case 0x05: return (key_map_t){1, 3}; /* 4 */
        case 0x06: return (key_map_t){2, 0}; /* 5 */
        case 0x07: return (key_map_t){2, 3}; /* 6 */
        case 0x08: return (key_map_t){3, 0}; /* 7 */
        case 0x09: return (key_map_t){3, 3}; /* 8 */
        case 0x0A: return (key_map_t){4, 0}; /* 9 */
        case 0x0B: return (key_map_t){4, 3}; /* 0 */

        case 0x39: return (key_map_t){7, 4}; /* SPACE */
        case 0x1C: return (key_map_t){0, 1}; /* RETURN */
        case 0x0E: return (key_map_t){0, 0}; /* BACKSPACE -> INST/DEL */
        case 0x33: return (key_map_t){5, 7}; /* , */
        case 0x34: return (key_map_t){5, 4}; /* . */
        case 0x35: return (key_map_t){6, 7}; /* / */
        case 0x27: return (key_map_t){6, 2}; /* ; */
        case 0x0C: return (key_map_t){5, 3}; /* - */
        case 0x0D: return (key_map_t){5, 0}; /* = -> C64 + */
        case 0x1A: return (key_map_t){5, 6}; /* @ */
        case 0x1B: return (key_map_t){6, 1}; /* * */
        case 0x2A: return (key_map_t){1, 7}; /* Left Shift */
        case 0x36: return (key_map_t){6, 4}; /* Right Shift */
        case 0x1D: return (key_map_t){7, 2}; /* Left Ctrl */
        case 0x01: return (key_map_t){7, 7}; /* Esc -> RUN/STOP */

        /* --- reszta pelnej matrycy C64 (klawisze bez prostego odpowiednika
         * ASCII - patrz host/kvm_host.c:parse_escape_seq oraz ascii_to_key
         * po strone tego, co faktycznie generuje te kody) ------------------ */
        case 0x4D: return (key_map_t){0, 2}; /* CRSR LEWO/PRAWO (wspoldzielony klawisz) */
        case 0x50: return (key_map_t){0, 7}; /* CRSR GORA/DOL (wspoldzielony klawisz) */
        case 0x3B: return (key_map_t){0, 4}; /* F1/F2 */
        case 0x3D: return (key_map_t){0, 5}; /* F3/F4 */
        case 0x3F: return (key_map_t){0, 6}; /* F5/F6 */
        case 0x41: return (key_map_t){0, 3}; /* F7/F8 */
        case 0x47: return (key_map_t){6, 3}; /* HOME/CLR */
        case 0x2B: return (key_map_t){6, 0}; /* funt (£) */
        case 0x38: return (key_map_t){7, 5}; /* C= (Commodore) */
        case 0x29: return (key_map_t){6, 6}; /* strzalka w gore / pi */
        case 0x56: return (key_map_t){7, 1}; /* strzalka w lewo (PETSCII) */
        case 0x0F: return (key_map_t){6, 5}; /* = (dedykowany klawisz C64, inny niz "+") */

        default: return KM_NONE;
    }
}

/* RESTORE na prawdziwym C64 NIE jest czescia macierzy klawiatury - to
 * osobny przycisk podlaczony wprost do linii NMI procesora. Host sygnalizuje
 * go umownym bajtem-wartownikiem 0xFF (poza normalnym zakresem "make"/
 * "break" dla realnie zmapowanych klawiszy), ktory poll_keyboard()
 * przechwytuje PRZED przekazaniem do scancode_to_c64()/macierzy i zamienia
 * na jednorazowe zadanie NMI, obslugiwane w petli glownej obok
 * cia2_nmi_pending() - patrz host/kvm_host.c (terminal F12). */
static bool g_restore_pending = false;

static void poll_keyboard(void)
{
    /* Celowo tylko JEDNO zdarzenie na klatke (~50 Hz): host kolejkuje kod
     * "make" i "break" tego samego znaku bezposrednio po sobie (patrz
     * kvm_host.c: poll_host_stdin), a gdyby ta funkcja oproznila cala
     * kolejke naraz, 6502 nigdy nie zdazylby zobaczyc stanu "wcisniety"
     * pomiedzy nimi - rutyna skanujaca KERNAL-a odpytuje macierz tylko
     * raz na przerwanie zegarowe. Ograniczenie do jednego zdarzenia na
     * klatke daje ~20 ms "przytrzymania" klawisza, co w zupelnosci
     * wystarcza do wykrycia przez KERNAL. */
    uint8_t sc = inb(PORT_KEYBOARD);
    if (sc == 0) return;
    if (sc == 0xFF) { g_restore_pending = true; return; }
    bool release = (sc & 0x80) != 0;
    key_map_t km = scancode_to_c64((uint8_t)(sc & 0x7F));
    /* Zamiana row/col: empirycznie zweryfikowana transpozycja pomiedzy
     * tabela scancode_to_c64() a konwencja wiersz/kolumna oczekiwana przez
     * rutyne skanujaca KERNAL-a (patrz komentarz w README/testach). */
    if (km.row != 0xFF) cia_keyboard_set(km.col, km.row, !release);
}

/* --- start ----------------------------------------------------------------- */

static void load_roms(void)
{
    uint32_t n;

    n = rom_kernal_file_len < ROM_KERNAL_SIZE ? rom_kernal_file_len : ROM_KERNAL_SIZE;
    memcpy(rom_kernal, rom_kernal_file, n);

    n = rom_basic_file_len < ROM_BASIC_SIZE ? rom_basic_file_len : ROM_BASIC_SIZE;
    memcpy(rom_basic, rom_basic_file, n);

    n = rom_chargen_file_len < ROM_CHARGEN_SIZE ? rom_chargen_file_len : ROM_CHARGEN_SIZE;
    memcpy(rom_chargen, rom_chargen_file, n);

    n = rom_simons_file_len < ROM_SIMONS_SIZE ? rom_simons_file_len : ROM_SIMONS_SIZE;
    memcpy(rom_simons, rom_simons_file, n);
}

void kernel_main(multiboot_info_min_t *mb_info)
{
    debug_puts("=== C64-on-KVM: unikernel boot ===\n");

    load_roms();

    pla_reset();
    cartridge_reset();
    vic2_reset();
    cia_reset();
    sid_reset();
    reu_reset();

    fake6502_context cpu;
    memset(&cpu, 0, sizeof(cpu));
    fake6502_reset(&cpu);

    debug_puts("PC after reset = $");
    debug_puthex16(cpu.cpu.pc);
    debug_puts("\n");

    uint8_t *vram = (uint8_t *)(uintptr_t)mb_info->framebuffer_addr;

    const double cycles_per_sample = 985248.0 / (double)SID_SAMPLE_RATE;
    double audio_acc = 0.0;

    for (;;) {
        if (cpu.cpu.pc == 0xF4A5) {
            do_fast_load(&cpu);
            continue;
        }
        if (cpu.cpu.pc == 0xF5DD) {
            do_fast_save(&cpu);
            continue;
        }
        if (cpu.cpu.pc == 0xF700) {
            do_sid_load(&cpu);
            continue;
        }
        uint32_t before = (uint32_t)cpu.emu.clockticks;
        fake6502_step(&cpu);
        uint32_t delta = (uint32_t)cpu.emu.clockticks - before;
        if (delta == 0) delta = 2;

        cia_tick((int)delta);
        bool frame_done = vic2_tick((int)delta);

        audio_acc += (double)delta;
        while (audio_acc >= cycles_per_sample) {
            audio_acc -= cycles_per_sample;
            outw(PORT_AUDIO_SAMPLE, (uint16_t)sid_generate_sample());
        }

        if (cia2_nmi_pending() || g_restore_pending) {
            fake6502_nmi(&cpu);
            g_restore_pending = false;
        }
        if (!(cpu.cpu.flags & FAKE6502_INTERRUPT_FLAG) &&
            (cia1_irq_pending() || vic2_irq_pending() || reu_irq_pending())) {
            fake6502_irq(&cpu);
        }

        if (frame_done) {
            poll_keyboard();
            memcpy(vram, vic2_framebuffer, sizeof(vic2_framebuffer));
        }
    }
}
