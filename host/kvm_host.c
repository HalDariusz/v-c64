/*
 * host/kvm_host.c - the host hypervisor: runs the bare-metal C64 guest
 * directly on /dev/kvm (Intel VT-x / AMD-V), with no QEMU involved.
 *
 * Registers 3 physical guest memory regions:
 *   RAM  0x00000-0x7FFFF (512 KB) - guest code/data (c64_guest.bin @ 0x0)
 *   VRAM 0xA0000-0xBFFFF (128 KB) - "VESA" 320x200x8bpp framebuffer
 *   BIOS 0xF0000-0xFFFFF (64 KB)  - x86 reset vector at 0xFFFF0
 *
 * The vCPU starts in real mode with CS:IP = F000:FFF0 (physical
 * 0xFFFF0), where a 5-byte "jmp far 0000:0000" leads to the guest loaded
 * at physical 0x00000 (see guest/boot.s).
 *
 * KVM_EXIT_IO handling implements a simple, custom protocol between the
 * host and the guest (ports 0x60, 0x5001, 0x5006, 0x5007, 0x5008 - see
 * guest/kernel.c for the exact meaning of each), plus KVM_EXIT_HLT for a
 * clean shutdown.
 *
 * Audio/video output: with SDL2 available at build time (USE_SDL2), a
 * single interactive window drives video, keyboard, and audio together;
 * otherwise a headless fallback periodically writes the VIC-II frame to
 * build/frame.ppm and streams SID audio to build/audio.wav.
 *
 * Part of v-c64 - a bare-metal Commodore 64 unikernel running directly
 * on Linux /dev/kvm, with no QEMU involved.
 *
 * Author: Dariusz Nowak <hal.dariusz.nowak@gmail.com>
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <termios.h>
#include <linux/kvm.h>

#ifdef USE_SDL2
#include <SDL2/SDL.h>
#endif

#include "diskimage.h"

#define GUEST_RAM_SIZE   (512u * 1024u)
#define GUEST_VRAM_SIZE  (128u * 1024u)
#define GUEST_BIOS_SIZE  (64u  * 1024u)

#define GUEST_RAM_BASE   0x00000000u
#define GUEST_VRAM_BASE  0x000A0000u
#define GUEST_BIOS_BASE  0x000F0000u

#define VIC2_W 320
#define VIC2_H 200

/* --- porty protokolu host<->gosc (musza byc zgodne z guest/kernel.c) ----- */
#define PORT_KEYBOARD       0x60
#define PORT_DEBUG_CONSOLE  0x5001
#define PORT_AUDIO_SAMPLE   0x5006
#define PORT_FASTLOAD_CMD   0x5007
#define PORT_FASTLOAD_DATA  0x5008

static volatile sig_atomic_t g_shutdown = 0;
static void on_signal(int sig) { (void)sig; g_shutdown = 1; }

/* --- paleta C64 (wartosci RGB powszechnie publikowane, np. przez VICE) --- */
static const uint8_t c64_palette[16][3] = {
    {0x00,0x00,0x00}, {0xFF,0xFF,0xFF}, {0x68,0x37,0x2B}, {0x70,0xA4,0xB2},
    {0x6F,0x3D,0x86}, {0x58,0x8D,0x43}, {0x35,0x28,0x79}, {0xB8,0xC7,0x6F},
    {0x6F,0x4F,0x25}, {0x43,0x39,0x00}, {0x9A,0x67,0x59}, {0x44,0x44,0x44},
    {0x6C,0x6C,0x6C}, {0x9A,0xD2,0x84}, {0x6C,0x5E,0xB5}, {0x95,0x95,0x95},
};

/* --- stan terminala (klawiatura hosta -> port 0x60) ---------------------- */
static struct termios g_orig_termios;
static bool g_termios_saved = false;

static uint8_t g_kbd_queue[512];
static unsigned g_kbd_head = 0, g_kbd_tail = 0;

static void kbd_push(uint8_t v)
{
    unsigned next = (g_kbd_tail + 1) % (unsigned)(sizeof(g_kbd_queue));
    if (next == g_kbd_head) return; /* kolejka pelna - pomin */
    g_kbd_queue[g_kbd_tail] = v;
    g_kbd_tail = next;
}

static uint8_t kbd_pop(void)
{
    if (g_kbd_head == g_kbd_tail) return 0;
    uint8_t v = g_kbd_queue[g_kbd_head];
    g_kbd_head = (g_kbd_head + 1) % (unsigned)(sizeof(g_kbd_queue));
    return v;
}

#define LSHIFT_SCANCODE 0x2A
#define CTRL_SCANCODE   0x1D

/* Kody skanowania (umowne, wspoldzielone z guest/kernel.c:scancode_to_c64)
 * dla klawiszy bez prostego odpowiednika ASCII - strzalki, funkcyjne,
 * Home/Clr, itp. Generowane przez parse_escape_seq() z sekwencji ucieczki
 * terminala (xterm/vt100), nie przez ascii_to_key(). */
#define SC_CRSR_LR 0x4D /* CRSR LEWO/PRAWO (wspoldzielony klawisz) */
#define SC_CRSR_UD 0x50 /* CRSR GORA/DOL (wspoldzielony klawisz) */
#define SC_F1F2    0x3B
#define SC_F3F4    0x3D
#define SC_F5F6    0x3F
#define SC_F7F8    0x41
#define SC_HOMECLR 0x47
#define SC_DEL     0x0E /* ten sam klawisz co BACKSPACE/INST-DEL */

/* RESTORE nie jest czescia macierzy klawiatury (patrz guest/kernel.c) -
 * ten bajt-wartownik jest wysylany na PORT_KEYBOARD zamiast pary
 * make/break i przechwytywany przez poll_keyboard() w gosciu PRZED
 * przekazaniem do macierzy. */
#define RESTORE_SIGNAL 0xFF

typedef struct { uint8_t sc; bool shift; } key_event_t;

static key_event_t ascii_to_key(int c)
{
    switch (c) {
        case 'a': case 'A': return (key_event_t){0x1E, false};
        case 'b': case 'B': return (key_event_t){0x30, false};
        case 'c': case 'C': return (key_event_t){0x2E, false};
        case 'd': case 'D': return (key_event_t){0x20, false};
        case 'e': case 'E': return (key_event_t){0x12, false};
        case 'f': case 'F': return (key_event_t){0x21, false};
        case 'g': case 'G': return (key_event_t){0x22, false};
        case 'h': case 'H': return (key_event_t){0x23, false};
        case 'i': case 'I': return (key_event_t){0x17, false};
        case 'j': case 'J': return (key_event_t){0x24, false};
        case 'k': case 'K': return (key_event_t){0x25, false};
        case 'l': case 'L': return (key_event_t){0x26, false};
        case 'm': case 'M': return (key_event_t){0x32, false};
        case 'n': case 'N': return (key_event_t){0x31, false};
        case 'o': case 'O': return (key_event_t){0x18, false};
        case 'p': case 'P': return (key_event_t){0x19, false};
        case 'q': case 'Q': return (key_event_t){0x10, false};
        case 'r': case 'R': return (key_event_t){0x13, false};
        case 's': case 'S': return (key_event_t){0x1F, false};
        case 't': case 'T': return (key_event_t){0x14, false};
        case 'u': case 'U': return (key_event_t){0x16, false};
        case 'v': case 'V': return (key_event_t){0x2F, false};
        case 'w': case 'W': return (key_event_t){0x11, false};
        case 'x': case 'X': return (key_event_t){0x2D, false};
        case 'y': case 'Y': return (key_event_t){0x15, false};
        case 'z': case 'Z': return (key_event_t){0x2C, false};
        case '1': return (key_event_t){0x02, false};
        case '2': return (key_event_t){0x03, false};
        case '3': return (key_event_t){0x04, false};
        case '4': return (key_event_t){0x05, false};
        case '5': return (key_event_t){0x06, false};
        case '6': return (key_event_t){0x07, false};
        case '7': return (key_event_t){0x08, false};
        case '8': return (key_event_t){0x09, false};
        case '9': return (key_event_t){0x0A, false};
        case '0': return (key_event_t){0x0B, false};
        case ' ': return (key_event_t){0x39, false};
        case '\r': case '\n': return (key_event_t){0x1C, false};
        case 0x7F: case 0x08: return (key_event_t){0x0E, false};
        case ',': return (key_event_t){0x33, false};
        case '.': return (key_event_t){0x34, false};
        case '/': return (key_event_t){0x35, false};
        case ';': return (key_event_t){0x27, false};
        case '-': return (key_event_t){0x0C, false};
        case '+': return (key_event_t){0x0D, false}; /* dedykowany klawisz C64 "+" */
        case '=': return (key_event_t){0x0F, false}; /* odrebny, dedykowany klawisz C64 "=" */
        case 0x1B: return (key_event_t){0x01, false}; /* Esc -> RUN/STOP (uwaga: patrz parse_escape_seq -
                                                         * poll_host_stdin() woła ta galaz tylko wtedy, gdy
                                                         * ESC NIE jest poczatkiem sekwencji ucieczki) */
        /* znaki wymagajace Shift na realnej klawiaturze C64: */
        case '!': return (key_event_t){0x02, true};  /* Shift+1 */
        case '"': return (key_event_t){0x03, true};  /* Shift+2 */
        case '#': return (key_event_t){0x04, true};  /* Shift+3 */
        case '%': return (key_event_t){0x06, true};  /* Shift+5 */
        case '&': return (key_event_t){0x07, true};  /* Shift+6 */
        case '(': return (key_event_t){0x09, true};  /* Shift+8 */
        case ')': return (key_event_t){0x0A, true};  /* Shift+9 */
        case ':': return (key_event_t){0x27, true};  /* Shift+; */
        case '<': return (key_event_t){0x33, true};  /* Shift+, */
        case '>': return (key_event_t){0x34, true};  /* Shift+. */
        case '?': return (key_event_t){0x35, true};  /* Shift+/ */
        case '\'': return (key_event_t){0x08, true}; /* Shift+7 */
        case '$': return (key_event_t){0x05, true};  /* Shift+4 */
        case '@': return (key_event_t){0x1A, false}; /* dedykowany klawisz C64 */
        case '*': return (key_event_t){0x1B, false}; /* dedykowany klawisz C64 */
        /* umowne odpowiedniki klawiszy graficznych C64 bez wlasnego ASCII -
         * konwencja przejeta z innych emulatorow (np. VICE): */
        case '\\': return (key_event_t){0x2B, false}; /* funt (£) */
        case '^':  return (key_event_t){0x29, false}; /* strzalka w gore / pi */
        case '_':  return (key_event_t){0x56, false}; /* strzalka w lewo (PETSCII) */
        default: return (key_event_t){0x00, false};
    }
}

/* Wysyla pojedyncze zdarzenie klawisza (make+break, z opcjonalnym Shift
 * i/lub Ctrl jako "obudowa" wokol niego) do kolejki hosta. */
static void kbd_push_key(key_event_t ev, bool ctrl)
{
    if (ev.sc == 0) return;
    if (ctrl) kbd_push(CTRL_SCANCODE);
    if (ev.shift) kbd_push(LSHIFT_SCANCODE);
    kbd_push(ev.sc);
    kbd_push((uint8_t)(ev.sc | 0x80));
    if (ev.shift) kbd_push((uint8_t)(LSHIFT_SCANCODE | 0x80));
    if (ctrl) kbd_push((uint8_t)(CTRL_SCANCODE | 0x80));
}

/* Rozpoznaje sekwencje ucieczki ANSI/xterm (strzalki, Home/End/Delete,
 * F1-F8, F12) w oknie buf[0..len) - buf[0] to zawsze 0x1B (ESC). Zwraca
 * liczbe skonsumowanych bajtow (>0): moze to byc rozpoznana sekwencja
 * (*out_ev.sc != 0 lub *out_restore == true), albo kompletna, ale
 * nieobslugiwana sekwencja (cicho ignorowana - *out_ev.sc == 0), albo
 * (gdy buf[1] nie jest '[' ani 'O') sam ESC potraktowany jako RUN/STOP.
 * Zwraca 0, jesli sekwencja jest NIEKOMPLETNA (potrzeba bajtow spoza
 * biezacego okna) - patrz komentarz w poll_host_stdin() o tym, jak to jest
 * (celowo, w duchu "lekkiej" wersji) obslugiwane. */
static int parse_escape_seq(const unsigned char *buf, int len, key_event_t *out_ev, bool *out_restore)
{
    out_ev->sc = 0;
    out_ev->shift = false;
    *out_restore = false;

    if (len < 2) return 0;

    if (buf[1] == '[') {
        if (len < 3) return 0;
        unsigned char c2 = buf[2];
        switch (c2) {
            case 'A': out_ev->sc = SC_CRSR_UD; out_ev->shift = true;  return 3; /* gora */
            case 'B': out_ev->sc = SC_CRSR_UD; out_ev->shift = false; return 3; /* dol */
            case 'C': out_ev->sc = SC_CRSR_LR; out_ev->shift = false; return 3; /* prawo */
            case 'D': out_ev->sc = SC_CRSR_LR; out_ev->shift = true;  return 3; /* lewo */
            case 'H': out_ev->sc = SC_HOMECLR; out_ev->shift = false; return 3; /* Home */
            case 'F': out_ev->sc = SC_HOMECLR; out_ev->shift = true;  return 3; /* End -> Shift+Home (CLR) */
            default: break;
        }
        if (c2 >= '0' && c2 <= '9') {
            /* forma vt220: ESC [ liczba(y) ~ */
            int i = 3;
            while (i < len && buf[i] >= '0' && buf[i] <= '9') i++;
            if (i >= len) return 0; /* niekompletne - patrz poll_host_stdin */
            if (buf[i] != '~') return i + 1; /* nierozpoznane, ale kompletne - porzuc */
            int num = 0;
            for (int k = 2; k < i; k++) num = num * 10 + (buf[k] - '0');
            switch (num) {
                case 1: case 7:  out_ev->sc = SC_HOMECLR; out_ev->shift = false; break; /* Home */
                case 4: case 8:  out_ev->sc = SC_HOMECLR; out_ev->shift = true;  break; /* End -> Shift+Home */
                case 3:  out_ev->sc = SC_DEL; out_ev->shift = false; break;             /* Delete */
                case 15: out_ev->sc = SC_F5F6; out_ev->shift = false; break;            /* F5 */
                case 17: out_ev->sc = SC_F5F6; out_ev->shift = true;  break;            /* F6 */
                case 18: out_ev->sc = SC_F7F8; out_ev->shift = false; break;            /* F7 */
                case 19: out_ev->sc = SC_F7F8; out_ev->shift = true;  break;            /* F8 */
                case 24: *out_restore = true; break;                                     /* F12 -> RESTORE */
                default: break; /* nierozpoznane (np. F9-F11, Ctrl+strzalki...) - ignorujemy */
            }
            return i + 1;
        }
        return 3; /* nierozpoznana krotka sekwencja CSI - porzuc */
    }

    if (buf[1] == 'O') {
        if (len < 3) return 0;
        switch (buf[2]) {
            case 'P': out_ev->sc = SC_F1F2; out_ev->shift = false; return 3; /* F1 */
            case 'Q': out_ev->sc = SC_F1F2; out_ev->shift = true;  return 3; /* F2 */
            case 'R': out_ev->sc = SC_F3F4; out_ev->shift = false; return 3; /* F3 */
            case 'S': out_ev->sc = SC_F3F4; out_ev->shift = true;  return 3; /* F4 */
            default: return 3;
        }
    }

    /* ESC + cokolwiek innego niz '[' / 'O': to NIE jest sekwencja, ktora
     * rozpoznajemy - sam ESC byl nacisniety osobno (RUN/STOP), a kolejny
     * bajt zostaje do normalnego przetworzenia przez wywolujacego. */
    *out_ev = ascii_to_key(0x1B);
    return 1;
}

static void poll_host_stdin(void)
{
    unsigned char buf[64];
    ssize_t n;
    while ((n = read(STDIN_FILENO, buf, sizeof(buf))) > 0) {
        int i = 0;
        while (i < n) {
            unsigned char b = buf[i];

            if (b == 0x1B) {
                /* Sekwencje ucieczki sa w praktyce (lokalny pty, jak w tym
                 * projekcie) zawsze dostarczane w calosci w jednym read() -
                 * jesli akurat przetnie sie ona na granicy bufora (bardzo
                 * rzadkie przy odpytywaniu ~50 Hz), celowo porzucamy reszte
                 * TEGO odczytu zamiast pamietac stan miedzy wywolaniami -
                 * to swiadomy kompromis "lekkiej" wersji (patrz README);
                 * najgorszy skutek to jedno pominiete nacisniecie strzalki. */
                key_event_t ev;
                bool restore = false;
                int consumed = parse_escape_seq(buf + i, (int)(n - i), &ev, &restore);
                if (consumed == 0) break;
                if (restore) kbd_push(RESTORE_SIGNAL);
                if (ev.sc != 0) kbd_push_key(ev, false);
                i += consumed;
                continue;
            }

            if (b >= 1 && b <= 26 && b != 0x08 && b != 0x0A && b != 0x0D) {
                /* Ctrl+litera (kod ASCII 1-26 = Ctrl+A..Ctrl+Z) - z
                 * wylaczeniem kodow majacych juz inne, bardziej oczywiste
                 * znaczenie terminalowe: 0x08 (Backspace/DEL), 0x0A i 0x0D
                 * (nowa linia/powrot karetki -> RETURN). */
                key_event_t base = ascii_to_key((int)('A' + (b - 1)));
                kbd_push_key(base, true);
                i++;
                continue;
            }

            key_event_t ev = ascii_to_key(b);
            kbd_push_key(ev, false);
            i++;
        }
    }
}

#ifdef USE_SDL2
/* --- okno SDL2: prawdziwa interaktywnosc w jednym oknie (obraz + klawiatura
 * + dzwiek), zamiast rozdzielonego terminala/podgladu obrazu/aplaya. Budowane
 * tylko gdy `pkg-config sdl2` jest dostepne (patrz Makefile) - bez tego,
 * kvm_host dziala tak jak dotychczas (headless: terminal + frame.ppm +
 * audio.wav/fifo). Klawiatura z terminala (poll_host_stdin) dziala nadal
 * rownolegle - mozna wpisywac w oknie SDL LUB w terminalu, ktory je odpalil. */

static SDL_Window   *g_sdl_win = NULL;
static SDL_Renderer *g_sdl_ren = NULL;
static SDL_Texture  *g_sdl_tex = NULL;
static SDL_AudioDeviceID g_sdl_audio = 0;
static uint8_t g_sdl_rgb[VIC2_W * VIC2_H * 3];

static void sdl_init(void)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "[kvm_host] SDL_Init: %s - continuing without an SDL window.\n", SDL_GetError());
        return;
    }
    g_sdl_win = SDL_CreateWindow("v-c64", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                  VIC2_W * 2, VIC2_H * 2, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!g_sdl_win) {
        fprintf(stderr, "[kvm_host] SDL_CreateWindow: %s - continuing without an SDL window.\n", SDL_GetError());
        return;
    }
    g_sdl_ren = SDL_CreateRenderer(g_sdl_win, -1, SDL_RENDERER_ACCELERATED);
    if (!g_sdl_ren) g_sdl_ren = SDL_CreateRenderer(g_sdl_win, -1, SDL_RENDERER_SOFTWARE);
    g_sdl_tex = SDL_CreateTexture(g_sdl_ren, SDL_PIXELFORMAT_RGB24,
                                   SDL_TEXTUREACCESS_STREAMING, VIC2_W, VIC2_H);
    SDL_RenderSetLogicalSize(g_sdl_ren, VIC2_W, VIC2_H);
    SDL_StartTextInput();

    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof(want));
    want.freq = 44100;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 1024;
    g_sdl_audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (g_sdl_audio) SDL_PauseAudioDevice(g_sdl_audio, 0);

    fprintf(stderr, "[kvm_host] SDL2 window active: video + keyboard + audio in one window.\n");
}

static void sdl_shutdown(void)
{
    if (!g_sdl_win && !g_sdl_ren && !g_sdl_tex) return;
    if (g_sdl_audio) SDL_CloseAudioDevice(g_sdl_audio);
    if (g_sdl_tex) SDL_DestroyTexture(g_sdl_tex);
    if (g_sdl_ren) SDL_DestroyRenderer(g_sdl_ren);
    if (g_sdl_win) SDL_DestroyWindow(g_sdl_win);
    SDL_Quit();
}

static void sdl_render(const uint8_t *vram)
{
    if (!g_sdl_tex) return;
    for (int y = 0; y < VIC2_H; y++) {
        for (int x = 0; x < VIC2_W; x++) {
            uint8_t idx = vram[y * VIC2_W + x] & 0x0F;
            uint8_t *px = &g_sdl_rgb[(y * VIC2_W + x) * 3];
            px[0] = c64_palette[idx][0];
            px[1] = c64_palette[idx][1];
            px[2] = c64_palette[idx][2];
        }
    }
    SDL_UpdateTexture(g_sdl_tex, NULL, g_sdl_rgb, VIC2_W * 3);
    SDL_RenderClear(g_sdl_ren);
    SDL_RenderCopy(g_sdl_ren, g_sdl_tex, NULL, NULL);
    SDL_RenderPresent(g_sdl_ren);
}

static void sdl_pump_events(void)
{
    if (!g_sdl_win) return;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_QUIT:
                g_shutdown = 1;
                break;

            case SDL_TEXTINPUT:
                for (const char *p = e.text.text; *p; p++) {
                    key_event_t ev = ascii_to_key((unsigned char)*p);
                    kbd_push_key(ev, false);
                }
                break;

            case SDL_KEYDOWN: {
                if (e.key.repeat) break; /* bez auto-powtarzania - spojnie z trybem terminalowym (tap-only) */
                SDL_Keycode sym = e.key.keysym.sym;
                bool ctrl = (SDL_GetModState() & KMOD_CTRL) != 0;
                if (ctrl && sym >= SDLK_a && sym <= SDLK_z) {
                    key_event_t base = ascii_to_key('A' + (int)(sym - SDLK_a));
                    kbd_push_key(base, true);
                    break;
                }
                key_event_t ev = {0, false};
                bool restore = false;
                switch (sym) {
                    case SDLK_LEFT:     ev = (key_event_t){SC_CRSR_LR, true};  break;
                    case SDLK_RIGHT:    ev = (key_event_t){SC_CRSR_LR, false}; break;
                    case SDLK_UP:       ev = (key_event_t){SC_CRSR_UD, true};  break;
                    case SDLK_DOWN:     ev = (key_event_t){SC_CRSR_UD, false}; break;
                    case SDLK_HOME:     ev = (key_event_t){SC_HOMECLR, false}; break;
                    case SDLK_END:      ev = (key_event_t){SC_HOMECLR, true};  break;
                    case SDLK_DELETE:
                    case SDLK_BACKSPACE: ev = (key_event_t){SC_DEL, false};    break;
                    case SDLK_RETURN:
                    case SDLK_KP_ENTER: ev = (key_event_t){0x1C, false};       break;
                    case SDLK_ESCAPE:   ev = (key_event_t){0x01, false};       break; /* RUN/STOP */
                    case SDLK_F1:       ev = (key_event_t){SC_F1F2, false};    break;
                    case SDLK_F2:       ev = (key_event_t){SC_F1F2, true};     break;
                    case SDLK_F3:       ev = (key_event_t){SC_F3F4, false};    break;
                    case SDLK_F4:       ev = (key_event_t){SC_F3F4, true};     break;
                    case SDLK_F5:       ev = (key_event_t){SC_F5F6, false};    break;
                    case SDLK_F6:       ev = (key_event_t){SC_F5F6, true};     break;
                    case SDLK_F7:       ev = (key_event_t){SC_F7F8, false};    break;
                    case SDLK_F8:       ev = (key_event_t){SC_F7F8, true};     break;
                    case SDLK_F12:      restore = true;                        break; /* RESTORE */
                    default: break; /* SPACJA i znaki drukowalne przychodza przez SDL_TEXTINPUT wyzej */
                }
                if (restore) kbd_push(RESTORE_SIGNAL);
                else kbd_push_key(ev, false);
                break;
            }

            default:
                break;
        }
    }
}
#endif /* USE_SDL2 */

static void setup_terminal(void)
{
    /* Niezaleznie od tego, czy stdin jest terminalem, potokiem czy plikiem,
     * MUSI byc nieblokujacy - w przeciwnym razie pojedynczy read() bez
     * dostepnych danych zamrozilby caly hipernadzorca (a wiec i gosc, i
     * pacing audio) do czasu nadejscia kolejnych bajtow. VMIN/VTIME ponizej
     * dotycza tylko trybu kanonicznego terminala; O_NONBLOCK jest
     * zabezpieczeniem dzialajacym zawsze. */
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags >= 0) fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    if (!isatty(STDIN_FILENO)) return;
    tcgetattr(STDIN_FILENO, &g_orig_termios);
    g_termios_saved = true;
    struct termios raw = g_orig_termios;
    raw.c_lflag &= (tcflag_t)~(ICANON | ECHO); /* natychmiastowe znaki, bez echa; ISIG zostaje wlaczone (Ctrl+C dziala normalnie) */
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

static void restore_terminal(void)
{
    if (g_termios_saved) tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
}

/* --- audio: strumieniowy zapis do build/audio.wav ------------------------ */

static FILE *g_wav = NULL;
static uint32_t g_wav_samples = 0;

/* Opcjonalny "live" odsluch na glosniku: FIFO build/audio.fifo, do ktorego
 * probki leca rownolegle z zapisem do audio.wav. Otwierany nonblock -
 * jesli nikt nie czyta z drugiej strony (np. `aplay` uruchomiony przez
 * c64-run.sh PRZED startem kvm_host), open() zwroci ENXIO i po prostu nie
 * ma live audio w tym uruchomieniu - dziala tylko wav (uczciwe uproszczenie,
 * bez petli ponawiania polaczenia w trakcie dzialania). */
static int g_audio_fifo_fd = -1;

static void audio_fifo_open(void)
{
    const char *path = "build/audio.fifo";
    mkfifo(path, 0666); /* EEXIST celowo ignorowany - fifo z poprzedniego uruchomienia */
    g_audio_fifo_fd = open(path, O_WRONLY | O_NONBLOCK);
}

static void wav_write_header_placeholder(FILE *f)
{
    uint8_t hdr[44] = {0};
    memcpy(hdr + 0, "RIFF", 4);
    memcpy(hdr + 8, "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    uint32_t fmt_len = 16; memcpy(hdr + 16, &fmt_len, 4);
    uint16_t audio_fmt = 1; memcpy(hdr + 20, &audio_fmt, 2);
    uint16_t channels = 1; memcpy(hdr + 22, &channels, 2);
    uint32_t rate = 44100; memcpy(hdr + 24, &rate, 4);
    uint32_t byte_rate = rate * 2; memcpy(hdr + 28, &byte_rate, 4);
    uint16_t block_align = 2; memcpy(hdr + 32, &block_align, 2);
    uint16_t bits = 16; memcpy(hdr + 34, &bits, 2);
    memcpy(hdr + 36, "data", 4);
    fwrite(hdr, 1, 44, f);
}

static void wav_finalize(void)
{
    if (!g_wav) return;
    uint32_t data_bytes = g_wav_samples * 2u;
    uint32_t riff_size = 36 + data_bytes;
    fseek(g_wav, 4, SEEK_SET);
    fwrite(&riff_size, 4, 1, g_wav);
    fseek(g_wav, 40, SEEK_SET);
    fwrite(&data_bytes, 4, 1, g_wav);
    fclose(g_wav);
    g_wav = NULL;
}

/* --- fast-loader: serwowanie plikow .PRG z katalogu hosta ---------------- */

static char g_disk_dir[512] = "disk";
static char g_fl_filename[256];
static int g_fl_filename_len = 0;
static uint8_t g_fl_response[16 + 65536];
static uint32_t g_fl_response_len = 0;
static uint32_t g_fl_response_pos = 0;
static uint8_t g_fl_status = 0;

static void fastload_execute(void)
{
    g_fl_filename[g_fl_filename_len] = 0;

    /* "$" (ew. "$:wzorzec") to magiczna nazwa KERNAL-a oznaczajaca katalog
     * dysku, nie zwykly plik - obsluzone od razu, bez sprawdzania host'owego
     * systemu plikow (patrz diskimage_directory_listing). */
    if (g_fl_filename[0] == '$') {
        const char *pattern = g_fl_filename + 1;
        if (*pattern == ':') pattern++;
        size_t list_len = 0;
        uint8_t *listing = diskimage_directory_listing(g_disk_dir, pattern, &list_len);
        if (listing) {
            size_t data_len = list_len >= 2 ? list_len - 2 : 0;
            if (data_len > 65536) data_len = 65536;
            uint16_t load_addr = (uint16_t)(listing[0] | (listing[1] << 8));
            g_fl_response[0] = (uint8_t)(data_len & 0xFF);
            g_fl_response[1] = (uint8_t)((data_len >> 8) & 0xFF);
            g_fl_response[2] = (uint8_t)(load_addr & 0xFF);
            g_fl_response[3] = (uint8_t)(load_addr >> 8);
            memcpy(g_fl_response + 4, listing + 2, data_len);
            free(listing);
            g_fl_response_len = 4 + (uint32_t)data_len;
            g_fl_response_pos = 0;
            g_fl_status = 1;
            fprintf(stderr, "[kvm_host] LOAD '$': directory sent (%zu bytes)\n", data_len);
        } else {
            fprintf(stderr, "[kvm_host] LOAD '$': no mounted images in '%s'\n", g_disk_dir);
            g_fl_status = 0;
            g_fl_response_len = 0;
            g_fl_response_pos = 0;
        }
        return;
    }

    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", g_disk_dir, g_fl_filename);

    FILE *f = fopen(path, "rb");
    if (!f) {
        /* Nie ma zwyklego pliku .prg o tej nazwie - poszukaj jej wewnatrz
         * zamontowanych obrazow dysku (.d64) lub tasmy (.t64) w katalogu
         * dyskietki (patrz diskimage.c). */
        size_t img_len = 0;
        uint8_t *img_buf = diskimage_find_prg(g_disk_dir, g_fl_filename, &img_len);
        if (img_buf) {
            size_t data_len = img_len >= 2 ? img_len - 2 : 0;
            if (data_len > 65536) data_len = 65536;
            uint16_t load_addr = (uint16_t)(img_buf[0] | (img_buf[1] << 8));

            g_fl_response[0] = (uint8_t)(data_len & 0xFF);
            g_fl_response[1] = (uint8_t)((data_len >> 8) & 0xFF);
            g_fl_response[2] = (uint8_t)(load_addr & 0xFF);
            g_fl_response[3] = (uint8_t)(load_addr >> 8);
            memcpy(g_fl_response + 4, img_buf + 2, data_len);
            free(img_buf);

            g_fl_response_len = 4 + (uint32_t)data_len;
            g_fl_response_pos = 0;
            g_fl_status = 1;
            fprintf(stderr, "[kvm_host] LOAD '%s' (from disk/tape image): %zu bytes @ $%04X\n",
                    g_fl_filename, data_len, load_addr);
            return;
        }

        fprintf(stderr, "[kvm_host] LOAD: '%s' not found\n", path);
        g_fl_status = 0;
        g_fl_response_len = 0;
        g_fl_response_pos = 0;
        return;
    }

    uint8_t hdr2[2];
    if (fread(hdr2, 1, 2, f) != 2) { fclose(f); g_fl_status = 0; return; }
    fseek(f, 0, SEEK_END);
    long total = ftell(f);
    fseek(f, 2, SEEK_SET);
    long data_len = total - 2;
    if (data_len < 0) data_len = 0;
    if (data_len > 65536) data_len = 65536;

    uint16_t load_addr = (uint16_t)(hdr2[0] | (hdr2[1] << 8));
    g_fl_response[0] = (uint8_t)(data_len & 0xFF);
    g_fl_response[1] = (uint8_t)((data_len >> 8) & 0xFF);
    g_fl_response[2] = (uint8_t)(load_addr & 0xFF);
    g_fl_response[3] = (uint8_t)(load_addr >> 8);
    size_t got = fread(g_fl_response + 4, 1, (size_t)data_len, f);
    fclose(f);

    g_fl_response_len = 4 + (uint32_t)got;
    g_fl_response_pos = 0;
    g_fl_status = 1;
    fprintf(stderr, "[kvm_host] LOAD '%s': %ld bytes @ $%04X\n", g_fl_filename, (long)got, load_addr);
}

/* --- SID-load: parsowanie naglowka PSID/RSID i serwowanie danych utworu -----
 *
 * Format PSID/RSID (v1/v2+, wszystkie pola liczbowe big-endian) jest jawnie
 * udokumentowana specyfikacja publiczna (High Voltage SID Collection).
 * Odpowiedz dla goscia (przez port 0x5008, w tej kolejnosci):
 *   load_lo, load_hi, init_lo, init_hi, play_lo, play_hi,
 *   songs, start_song_0based, data_len_lo, data_len_hi, [dane...] */

static uint8_t g_sid_filebuf[65536 + 2];

static void fastload_sid_execute(void)
{
    g_fl_filename[g_fl_filename_len] = 0;
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", g_disk_dir, g_fl_filename);

    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[kvm_host] SIDLOAD: '%s' not found\n", path);
        g_fl_status = 0;
        g_fl_response_len = 0;
        g_fl_response_pos = 0;
        return;
    }

    uint8_t hdr[0x7C];
    size_t got_hdr = fread(hdr, 1, sizeof(hdr), f);
    if (got_hdr < 0x76 || (memcmp(hdr, "PSID", 4) != 0 && memcmp(hdr, "RSID", 4) != 0)) {
        fprintf(stderr, "[kvm_host] SIDLOAD: '%s' is not a PSID/RSID file\n", g_fl_filename);
        fclose(f);
        g_fl_status = 0;
        return;
    }

    uint16_t dataOffset   = (uint16_t)((hdr[6] << 8) | hdr[7]);
    uint16_t loadAddress  = (uint16_t)((hdr[8] << 8) | hdr[9]);
    uint16_t initAddress  = (uint16_t)((hdr[10] << 8) | hdr[11]);
    uint16_t playAddress  = (uint16_t)((hdr[12] << 8) | hdr[13]);
    uint16_t songs        = (uint16_t)((hdr[14] << 8) | hdr[15]);
    uint16_t startSong    = (uint16_t)((hdr[16] << 8) | hdr[17]);

    fseek(f, dataOffset, SEEK_SET);
    size_t n = fread(g_sid_filebuf, 1, sizeof(g_sid_filebuf), f);
    fclose(f);

    uint16_t real_load;
    uint8_t *dataptr;
    size_t datalen;
    if (loadAddress == 0) {
        if (n < 2) { g_fl_status = 0; return; }
        real_load = (uint16_t)(g_sid_filebuf[0] | (g_sid_filebuf[1] << 8));
        dataptr = g_sid_filebuf + 2;
        datalen = n - 2;
    } else {
        real_load = loadAddress;
        dataptr = g_sid_filebuf;
        datalen = n;
    }
    if (initAddress == 0) initAddress = real_load;
    if (datalen > 65536) datalen = 65536;

    uint32_t pos = 0;
    g_fl_response[pos++] = (uint8_t)(real_load & 0xFF);
    g_fl_response[pos++] = (uint8_t)(real_load >> 8);
    g_fl_response[pos++] = (uint8_t)(initAddress & 0xFF);
    g_fl_response[pos++] = (uint8_t)(initAddress >> 8);
    g_fl_response[pos++] = (uint8_t)(playAddress & 0xFF);
    g_fl_response[pos++] = (uint8_t)(playAddress >> 8);
    g_fl_response[pos++] = (uint8_t)(songs & 0xFF);
    g_fl_response[pos++] = (uint8_t)(startSong > 0 ? startSong - 1 : 0);
    g_fl_response[pos++] = (uint8_t)(datalen & 0xFF);
    g_fl_response[pos++] = (uint8_t)((datalen >> 8) & 0xFF);
    memcpy(g_fl_response + pos, dataptr, datalen);
    pos += (uint32_t)datalen;

    g_fl_response_len = pos;
    g_fl_response_pos = 0;
    g_fl_status = 1;
    fprintf(stderr,
        "[kvm_host] SIDLOAD '%s': load=$%04X init=$%04X play=$%04X songs=%u start=%u dlen=%zu\n",
        g_fl_filename, real_load, initAddress, playAddress, songs, startSong, datalen);
}

/* --- fast-saver: przyjmowanie danych SAVE od goscia i zapis na hoscie ----- */

typedef enum {
    FL_MODE_IDLE = 0,
    FL_MODE_LOAD_FNAME,
    FL_MODE_SAVE_FNAME,
    FL_MODE_SAVE_DATA,
    FL_MODE_SID_FNAME,
} fastload_mode_t;

static fastload_mode_t g_fl_mode = FL_MODE_IDLE;
static uint8_t g_fl_save_buf[65536];
static uint32_t g_fl_save_len = 0;      /* liczba bajtow danych (bez naglowka adresu) */
static uint32_t g_fl_save_pos = 0;      /* ile bajtow danych juz odebrano */
static uint8_t g_fl_save_hdr[4];        /* start_lo, start_hi, len_lo, len_hi */
static int g_fl_save_hdr_stage = 0;     /* 0-3: zbieranie 4 bajtow naglowka */

/* Po cmd=0x04 (nazwa kompletna) gosc wysyla przez port 0x5008, w kolejnosci:
 *   start_addr_lo, start_addr_hi, length_lo, length_hi, [dane...]
 * po czym cmd=0x05 wykonuje faktyczny zapis pliku. */
static void fastload_save_data_byte(uint8_t b)
{
    if (g_fl_save_hdr_stage < 4) {
        g_fl_save_hdr[g_fl_save_hdr_stage++] = b;
        if (g_fl_save_hdr_stage == 4) {
            g_fl_save_len = (uint32_t)(g_fl_save_hdr[2] | (g_fl_save_hdr[3] << 8));
            if (g_fl_save_len > sizeof(g_fl_save_buf)) g_fl_save_len = sizeof(g_fl_save_buf);
            g_fl_save_pos = 0;
        }
        return;
    }
    if (g_fl_save_pos < g_fl_save_len) g_fl_save_buf[g_fl_save_pos++] = b;
}

/* Bufor pomocniczy do przekazania danych SAVE w formacie .prg (2B adres +
 * dane) do diskimage_save_prg() - statyczny, zeby nie obciazac stosu. */
static uint8_t g_fl_save_prgbuf[2 + 65536];

static void fastload_save_execute(void)
{
    g_fl_filename[g_fl_filename_len] = 0;

    /* Jesli w katalogu dyskietki jest zamontowany dokladnie jeden obraz
     * .d64, SAVE trafia na niego (tak jak na prawdziwym C64 - dyskietka
     * jest jedna, wlozona do napedu). W przeciwnym razie (brak obrazu,
     * albo wiecej niz jeden - dwuznacznosc) spada na dotychczasowe
     * zachowanie: zwykly plik .prg w katalogu hosta. */
    g_fl_save_prgbuf[0] = g_fl_save_hdr[0];
    g_fl_save_prgbuf[1] = g_fl_save_hdr[1];
    memcpy(g_fl_save_prgbuf + 2, g_fl_save_buf, g_fl_save_pos);
    if (diskimage_save_prg(g_disk_dir, g_fl_filename, g_fl_save_prgbuf, 2 + g_fl_save_pos)) {
        g_fl_status = 1;
        fprintf(stderr, "[kvm_host] SAVE '%s': %u bytes (to mounted .d64 image)\n",
                g_fl_filename, g_fl_save_pos);
        return;
    }

    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", g_disk_dir, g_fl_filename);

    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "[kvm_host] SAVE: cannot write '%s'\n", path);
        g_fl_status = 0;
        return;
    }
    /* Standardowy format .prg: 2-bajtowy naglowek adresu zaladowania +
     * surowe dane - dzieki temu plik da sie potem wczytac przez LOAD. */
    fwrite(g_fl_save_hdr, 1, 2, f); /* start_addr_lo, start_addr_hi */
    fwrite(g_fl_save_buf, 1, g_fl_save_pos, f);
    fclose(f);

    g_fl_status = 1;
    fprintf(stderr, "[kvm_host] SAVE '%s': %u bytes\n", g_fl_filename, g_fl_save_pos);
}

/* --- ramka wideo: okresowy zrzut VRAM -> build/frame.ppm ------------------ */

static void dump_frame_ppm(const uint8_t *vram)
{
    FILE *f = fopen("build/frame.ppm", "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", VIC2_W, VIC2_H);
    uint8_t row[VIC2_W * 3];
    for (int y = 0; y < VIC2_H; y++) {
        for (int x = 0; x < VIC2_W; x++) {
            uint8_t idx = vram[y * VIC2_W + x] & 0x0F;
            row[x*3+0] = c64_palette[idx][0];
            row[x*3+1] = c64_palette[idx][1];
            row[x*3+2] = c64_palette[idx][2];
        }
        fwrite(row, 1, sizeof(row), f);
    }
    fclose(f);
}

/* --- tempo czasu rzeczywistego ---------------------------------------------
 * Gosc dziala na goleej wirtualizacji sprzetowej, wiec bez zadnego throttlingu
 * 6502 wykonywalby sie znacznie szybciej niz prawdziwe 985248 Hz C64. Zamiast
 * emulowac PIT/RTC (poza zakresem specyfikacji), wykorzystujemy fakt, ze
 * kernel.c generuje dokladnie jedna probke audio na kazde ~22.35 cyklu 6502
 * (985248 Hz / 44100 Hz) - throttlowanie tempa probek audio do prawdziwego
 * czasu throttluje wiec posrednio cala maszyne (KVM_RUN jest synchroniczny:
 * gdy host spi, vCPU po prostu nie dziala). */
#define AUDIO_PACING_BATCH 256
static struct timespec g_pace_start;
static bool g_pace_started = false;

static void audio_pace(void)
{
    if (!g_pace_started) {
        clock_gettime(CLOCK_MONOTONIC, &g_pace_start);
        g_pace_started = true;
        return;
    }
    if (g_wav_samples % AUDIO_PACING_BATCH != 0) return;

    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    double actual = (double)(now.tv_sec - g_pace_start.tv_sec) +
                    (double)(now.tv_nsec - g_pace_start.tv_nsec) / 1e9;
    double expected = (double)g_wav_samples / 44100.0;
    double behind = expected - actual;
    if (behind > 0.0 && behind < 1.0) {
        struct timespec req;
        req.tv_sec = (time_t)behind;
        req.tv_nsec = (long)((behind - (double)req.tv_sec) * 1e9);
        nanosleep(&req, NULL);
    }
}

/* --- KVM: pomocnicze ------------------------------------------------------ */

static void *map_region(size_t size)
{
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); exit(1); }
    memset(p, 0, size);
    return p;
}

static void register_memory_region(int vm_fd, uint32_t slot, uint64_t guest_addr,
                                    uint64_t size, void *hva)
{
    struct kvm_userspace_memory_region region = {
        .slot = slot,
        .flags = 0,
        .guest_phys_addr = guest_addr,
        .memory_size = size,
        .userspace_addr = (uint64_t)(uintptr_t)hva,
    };
    if (ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &region) < 0) {
        perror("KVM_SET_USER_MEMORY_REGION");
        exit(1);
    }
}

static void setup_flat_segment(struct kvm_segment *seg, uint16_t selector,
                                uint32_t base, uint8_t type, uint8_t db)
{
    seg->selector = selector;
    seg->base = base;
    seg->limit = 0xFFFFu;
    seg->type = type;
    seg->present = 1;
    seg->dpl = 0;
    seg->db = db;
    seg->s = 1;
    seg->l = 0;
    seg->g = 0;
    seg->avl = 0;
    seg->unusable = 0;
}

int main(int argc, char **argv)
{
    const char *guest_path = (argc > 1) ? argv[1] : "build/c64_guest.bin";
    if (argc > 2) snprintf(g_disk_dir, sizeof(g_disk_dir), "%s", argv[2]);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    int kvm_fd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (kvm_fd < 0) {
        perror("open /dev/kvm");
        fprintf(stderr,
            "Hint: add yourself to the 'kvm' group (sudo usermod -aG kvm $USER, "
            "then log in again) or run via 'sudo'.\n");
        return 1;
    }

    int api_ver = ioctl(kvm_fd, KVM_GET_API_VERSION, 0);
    if (api_ver != 12) {
        fprintf(stderr, "Unexpected KVM API version: %d\n", api_ver);
        return 1;
    }

    int vm_fd = ioctl(kvm_fd, KVM_CREATE_VM, 0);
    if (vm_fd < 0) { perror("KVM_CREATE_VM"); return 1; }

    void *ram  = map_region(GUEST_RAM_SIZE);
    void *vram = map_region(GUEST_VRAM_SIZE);
    void *bios = map_region(GUEST_BIOS_SIZE);

    /* wektor resetu x86: 0xFFFF0 = jmp far 0000:0000 (5 bajtow) */
    uint8_t *reset_vec = (uint8_t *)bios + (0xFFFF0 - GUEST_BIOS_BASE);
    reset_vec[0] = 0xEA; reset_vec[1] = 0x00; reset_vec[2] = 0x00;
    reset_vec[3] = 0x00; reset_vec[4] = 0x00;

    FILE *gf = fopen(guest_path, "rb");
    if (!gf) { fprintf(stderr, "Cannot open %s: %s\n", guest_path, strerror(errno)); return 1; }
    size_t n = fread(ram, 1, GUEST_RAM_SIZE, gf);
    fclose(gf);
    fprintf(stderr, "[kvm_host] loaded %zu bytes of guest code from %s\n", n, guest_path);

    register_memory_region(vm_fd, 0, GUEST_RAM_BASE,  GUEST_RAM_SIZE,  ram);
    register_memory_region(vm_fd, 1, GUEST_VRAM_BASE, GUEST_VRAM_SIZE, vram);
    register_memory_region(vm_fd, 2, GUEST_BIOS_BASE, GUEST_BIOS_SIZE, bios);

    int vcpu_fd = ioctl(vm_fd, KVM_CREATE_VCPU, 0);
    if (vcpu_fd < 0) { perror("KVM_CREATE_VCPU"); return 1; }

    int mmap_size = ioctl(kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
    if (mmap_size < 0) { perror("KVM_GET_VCPU_MMAP_SIZE"); return 1; }
    struct kvm_run *run = mmap(NULL, (size_t)mmap_size, PROT_READ | PROT_WRITE,
                                MAP_SHARED, vcpu_fd, 0);
    if (run == MAP_FAILED) { perror("mmap vcpu"); return 1; }

    struct kvm_sregs sregs;
    if (ioctl(vcpu_fd, KVM_GET_SREGS, &sregs) < 0) { perror("KVM_GET_SREGS"); return 1; }

    setup_flat_segment(&sregs.cs, 0xF000, 0x000F0000, 0x9B, 0); /* code, base=selector*16 */
    setup_flat_segment(&sregs.ds, 0x0000, 0x00000000, 0x93, 0);
    setup_flat_segment(&sregs.es, 0x0000, 0x00000000, 0x93, 0);
    setup_flat_segment(&sregs.fs, 0x0000, 0x00000000, 0x93, 0);
    setup_flat_segment(&sregs.gs, 0x0000, 0x00000000, 0x93, 0);
    setup_flat_segment(&sregs.ss, 0x0000, 0x00000000, 0x93, 0);
    sregs.cr0 = 0x00000010; /* ET=1 (wymagane przez KVM), PE=0 - PE wlacza sam gosc */
    sregs.cr3 = 0;
    sregs.cr4 = 0;
    sregs.efer = 0;
    if (ioctl(vcpu_fd, KVM_SET_SREGS, &sregs) < 0) { perror("KVM_SET_SREGS"); return 1; }

    struct kvm_regs regs;
    memset(&regs, 0, sizeof(regs));
    regs.rip = 0xFFF0;
    regs.rflags = 0x2; /* zarezerwowany bit 1 musi byc ustawiony */
    if (ioctl(vcpu_fd, KVM_SET_REGS, &regs) < 0) { perror("KVM_SET_REGS"); return 1; }

    mkdir("build", 0755);
    setup_terminal();

    g_wav = fopen("build/audio.wav", "wb");
    if (g_wav) wav_write_header_placeholder(g_wav);
    audio_fifo_open();

#ifdef USE_SDL2
    sdl_init();
#endif

    fprintf(stderr,
        "[kvm_host] started. Type in this terminal to send text to the C64 "
        "keyboard. Ctrl+C ends the run cleanly.\n"
        "[kvm_host] video: build/frame.ppm (refreshed every ~200ms), "
        "audio: build/audio.wav%s, disk directory: %s/\n",
        (g_audio_fifo_fd >= 0) ? " + live build/audio.fifo" : "", g_disk_dir);

    struct timespec last_dump; clock_gettime(CLOCK_MONOTONIC, &last_dump);
    struct timespec last_sdl_render = last_dump;

    while (!g_shutdown) {
        int rc = ioctl(vcpu_fd, KVM_RUN, 0);
        if (rc < 0) {
            if (errno == EINTR) break;
            perror("KVM_RUN");
            break;
        }

        switch (run->exit_reason) {
            case KVM_EXIT_HLT:
                fprintf(stderr, "\n[kvm_host] KVM_EXIT_HLT - guest shut down cleanly.\n");
                g_shutdown = 1;
                break;

            case KVM_EXIT_IO: {
                uint8_t *data = (uint8_t *)run + run->io.data_offset;
                uint16_t port = run->io.port;

                if (run->io.direction == KVM_EXIT_IO_IN) {
                    switch (port) {
                        case PORT_KEYBOARD:
                            poll_host_stdin();
#ifdef USE_SDL2
                            sdl_pump_events();
#endif
                            *data = kbd_pop();
                            break;
                        case PORT_FASTLOAD_CMD:
                            *data = g_fl_status;
                            break;
                        case PORT_FASTLOAD_DATA:
                            *data = (g_fl_response_pos < g_fl_response_len)
                                        ? g_fl_response[g_fl_response_pos++] : 0;
                            break;
                        default:
                            *data = 0xFF;
                            break;
                    }
                } else { /* KVM_EXIT_IO_OUT */
                    uint32_t value = 0;
                    memcpy(&value, data, run->io.size);
                    switch (port) {
                        case PORT_DEBUG_CONSOLE:
                            fputc((int)(value & 0xFF), stderr);
                            break;
                        case PORT_AUDIO_SAMPLE: {
                            int16_t s = (int16_t)(value & 0xFFFF);
                            if (g_wav) {
                                fwrite(&s, 2, 1, g_wav);
                                g_wav_samples++;
                            }
                            if (g_audio_fifo_fd >= 0) {
                                ssize_t w = write(g_audio_fifo_fd, &s, 2);
                                if (w < 0 && errno != EAGAIN) {
                                    close(g_audio_fifo_fd);
                                    g_audio_fifo_fd = -1;
                                }
                            }
#ifdef USE_SDL2
                            if (g_sdl_audio) SDL_QueueAudio(g_sdl_audio, &s, 2);
#endif
                            audio_pace();
                            break;
                        }
                        case PORT_FASTLOAD_CMD:
                            switch (value) {
                                case 0x01: /* LOAD: rozpocznij nazwe pliku */
                                    g_fl_mode = FL_MODE_LOAD_FNAME;
                                    g_fl_filename_len = 0;
                                    break;
                                case 0x02: /* LOAD: wykonaj */
                                    fastload_execute();
                                    g_fl_mode = FL_MODE_IDLE;
                                    break;
                                case 0x03: /* SAVE: rozpocznij nazwe pliku */
                                    g_fl_mode = FL_MODE_SAVE_FNAME;
                                    g_fl_filename_len = 0;
                                    break;
                                case 0x04: /* SAVE: nazwa kompletna, oczekuj naglowka+danych */
                                    g_fl_mode = FL_MODE_SAVE_DATA;
                                    g_fl_save_hdr_stage = 0;
                                    g_fl_save_pos = 0;
                                    break;
                                case 0x05: /* SAVE: wykonaj zapis */
                                    fastload_save_execute();
                                    g_fl_mode = FL_MODE_IDLE;
                                    break;
                                case 0x06: /* SIDLOAD: rozpocznij nazwe pliku */
                                    g_fl_mode = FL_MODE_SID_FNAME;
                                    g_fl_filename_len = 0;
                                    break;
                                case 0x07: /* SIDLOAD: wykonaj (parsuj PSID/RSID) */
                                    fastload_sid_execute();
                                    g_fl_mode = FL_MODE_IDLE;
                                    break;
                                default:
                                    break;
                            }
                            break;
                        case PORT_FASTLOAD_DATA:
                            switch (g_fl_mode) {
                                case FL_MODE_LOAD_FNAME:
                                case FL_MODE_SAVE_FNAME:
                                case FL_MODE_SID_FNAME:
                                    if (g_fl_filename_len < (int)sizeof(g_fl_filename) - 1)
                                        g_fl_filename[g_fl_filename_len++] = (char)(value & 0xFF);
                                    break;
                                case FL_MODE_SAVE_DATA:
                                    fastload_save_data_byte((uint8_t)(value & 0xFF));
                                    break;
                                default:
                                    break;
                            }
                            break;
                        default:
                            break;
                    }
                }
                break;
            }

            case KVM_EXIT_SHUTDOWN:
                fprintf(stderr, "\n[kvm_host] KVM_EXIT_SHUTDOWN (triple fault?) - aborting.\n");
                g_shutdown = 1;
                break;

            default:
                fprintf(stderr, "\n[kvm_host] unhandled exit_reason=%d - aborting.\n",
                        run->exit_reason);
                g_shutdown = 1;
                break;
        }

        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        double ms = (double)(now.tv_sec - last_dump.tv_sec) * 1000.0 +
                    (double)(now.tv_nsec - last_dump.tv_nsec) / 1e6;
        if (ms >= 200.0) {
            dump_frame_ppm((const uint8_t *)vram);
            last_dump = now;
        }
#ifdef USE_SDL2
        double sdl_ms = (double)(now.tv_sec - last_sdl_render.tv_sec) * 1000.0 +
                         (double)(now.tv_nsec - last_sdl_render.tv_nsec) / 1e6;
        if (sdl_ms >= 40.0) { /* ~25 FPS - okno SDL nie musi czekac na powolny zapis PPM na dysk */
            sdl_render((const uint8_t *)vram);
            last_sdl_render = now;
        }
#endif
    }

#ifdef USE_SDL2
    sdl_shutdown();
#endif
    wav_finalize();
    if (g_audio_fifo_fd >= 0) close(g_audio_fifo_fd);
    restore_terminal();
    fprintf(stderr, "[kvm_host] shutting down. Audio samples written: %u\n", g_wav_samples);
    return 0;
}
