/*
 * host/diskimage.c - implementation of the .d64/.t64 disk image reader
 * and writer declared in diskimage.h: BAM parsing and allocation,
 * directory-chain walking and appending, PRG extraction with KERNAL-style
 * wildcard matching, and the BASIC-format directory listing byte layout.
 * See diskimage.h for the full design notes and scope.
 *
 * Part of v-c64 - a bare-metal Commodore 64 unikernel running directly
 * on Linux /dev/kvm, with no QEMU involved.
 *
 * Author: Dariusz Nowak <hal.dariusz.nowak@gmail.com>
 */

#define _GNU_SOURCE
#include "diskimage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>

/* Bezpieczny gorny limit na rozmiar wczytywanego obrazu (dysk/tasma nie
 * powinny byc wieksze niz kilka MB - to zabezpieczenie przed przypadkowym
 * wczytaniem czegos innego / bardzo duzego pliku o pasujacym rozszerzeniu). */
#define IMG_MAX_SIZE (8L * 1024 * 1024)

/* ---------------------------------------------------------------------- *
 * Dopasowanie nazwy z wildcardami jak w prawdziwym KERNAL-u: "*" napotkane
 * w dowolnym miejscu wzorca dopasowuje cala reszte nazwy (wiec samo "*"
 * oznacza "pierwszy plik"), a "?" dopasowuje dokladnie jeden dowolny znak.
 * ---------------------------------------------------------------------- */
static int name_matches(const char *entry, size_t entry_len, const char *want)
{
    size_t want_len = strlen(want);
    size_t wi = 0, ei = 0;
    while (wi < want_len) {
        char wc = want[wi];
        if (wc == '*') return 1;
        if (ei >= entry_len) return 0;
        if (wc != '?' && wc != entry[ei]) return 0;
        wi++;
        ei++;
    }
    return ei == entry_len;
}

/* ---------------------------------------------------------------------- *
 * D64 - standardowy obraz 35-sciezkowy (174848 B); tolerowany przy
 * odczycie takze rzadszy 40-sciezkowy wariant (196608 B) oraz dopisane na
 * koncu bajty informacji o bledach sektorow - uklad danych na sciezkach
 * 1-35 sie od tego nie zmienia. ZAPIS (SAVE) jest celowo ograniczony do
 * scisle standardowego 35-sciezkowego rozmiaru - to jedyny uklad BAM,
 * jaki ten kod umie bezpiecznie modyfikowac (patrz uczciwe uproszczenia
 * w README.md).
 *
 * Katalog: BAM (blok alokacji) lezy w T18/S0. Jego pierwsze dwa bajty
 * wskazuja T/S pierwszego bloku katalogu. Kazdy blok katalogu ma 8 wpisow
 * po 32 bajty: [0]=typ pliku (bit7=zamkniety,bit6=zablokowany,dolne 4
 * bity=SEQ/PRG/USR/REL), [1..2]=T/S pierwszego sektora danych,
 * [3..18]=nazwa (PETSCII, dopelniona bajtem $A0), [28..29]=rozmiar w
 * blokach (LE). Sam BAM: offset 4+(sciezka-1)*4 = [wolne_sektory,
 * bitmapa_24bit (bit=1 -> wolny)], offset 0x90 = nazwa dysku (16B, $A0),
 * offset 0xA2 = ID dysku (2B).
 * ---------------------------------------------------------------------- */

#define D64_SIZE_35    174848
#define D64_SIZE_40    196608
#define D64_BAM_TRACK  18
#define D64_BAM_SECTOR 0
#define D64_MAX_ENTRIES 144 /* 18 sektorow katalogu * 8 wpisow - gorny limit na sciezce 18 */

static int d64_spt(int track)
{
    if (track <= 17) return 21;
    if (track <= 24) return 19;
    if (track <= 30) return 18;
    return 17; /* 31-40 */
}

static long d64_ts_offset(int track, int sector)
{
    if (track < 1 || track > 40) return -1;
    long off = 0;
    for (int t = 1; t < track; t++) off += d64_spt(t) * 256;
    return off + (long)sector * 256;
}

static const char *d64_filetype_str(uint8_t t)
{
    switch (t & 0x0F) {
        case 0: return "DEL";
        case 1: return "SEQ";
        case 2: return "PRG";
        case 3: return "USR";
        case 4: return "REL";
        default: return "???";
    }
}

typedef struct {
    uint8_t type;
    int track, sector;          /* pierwszy sektor danych */
    char name[17];
    int name_len;
    int blocks;
    int slot_track, slot_sector, slot_index; /* polozenie WPISU katalogowego */
} d64_dirent_t;

static int d64_read_all_entries(const uint8_t *img, size_t img_size, d64_dirent_t *out, int max_out)
{
    long bam_off = d64_ts_offset(D64_BAM_TRACK, D64_BAM_SECTOR);
    if (bam_off < 0 || (size_t)(bam_off + 256) > img_size) return 0;
    int dir_t = img[bam_off], dir_s = img[bam_off + 1];
    int n = 0;
    for (int guard = 0; dir_t != 0 && guard < 40; guard++) {
        long off = d64_ts_offset(dir_t, dir_s);
        if (off < 0 || (size_t)(off + 256) > img_size) break;
        const uint8_t *sec = img + off;
        int next_t = sec[0], next_s = sec[1];
        for (int i = 0; i < 8 && n < max_out; i++) {
            const uint8_t *entry = sec + 2 + i * 32;
            uint8_t ftype = entry[0];
            if ((ftype & 0x0F) == 0) continue; /* pusty/skasowany wpis */
            d64_dirent_t *de = &out[n];
            de->type = ftype;
            de->track = entry[1];
            de->sector = entry[2];
            int elen = 0;
            for (int k = 0; k < 16; k++) {
                if (entry[3 + k] == 0xA0) break;
                de->name[elen++] = (char)entry[3 + k];
            }
            de->name[elen] = 0;
            de->name_len = elen;
            de->blocks = entry[28] | (entry[29] << 8);
            de->slot_track = dir_t;
            de->slot_sector = dir_s;
            de->slot_index = i;
            n++;
        }
        dir_t = next_t;
        dir_s = next_s;
    }
    return n;
}

static uint8_t *d64_read_chain(const uint8_t *img, size_t img_size, int t, int s, size_t *out_len)
{
    uint8_t *out = malloc(65538);
    if (!out) return NULL;
    size_t len = 0;
    for (int g = 0; t != 0 && g < 664 && len < 65536; g++) {
        long doff = d64_ts_offset(t, s);
        if (doff < 0 || (size_t)(doff + 256) > img_size) { free(out); return NULL; }
        const uint8_t *dsec = img + doff;
        int nt = dsec[0], ns = dsec[1];
        size_t chunk = (nt == 0) ? (size_t)(ns >= 1 ? ns - 1 : 0) : 254;
        if (len + chunk > 65536) chunk = 65536 - len;
        memcpy(out + len, dsec + 2, chunk);
        len += chunk;
        t = nt;
        s = ns;
    }
    *out_len = len;
    return out;
}

static uint8_t *d64_find(const uint8_t *img, size_t img_size, const char *name, size_t *out_len)
{
    if (img_size < D64_SIZE_35) return NULL;
    d64_dirent_t entries[D64_MAX_ENTRIES];
    int n = d64_read_all_entries(img, img_size, entries, D64_MAX_ENTRIES);
    for (int i = 0; i < n; i++) {
        if (!name_matches(entries[i].name, (size_t)entries[i].name_len, name)) continue;
        return d64_read_chain(img, img_size, entries[i].track, entries[i].sector, out_len);
    }
    return NULL;
}

/* dopisuje jedna "linie" listingu katalogu obrazu D64 do bufora danych
 * BASIC-owego pseudo-programu (patrz listing_emit_line nizej) */
static void d64_build_listing(const uint8_t *img, size_t img_size, const char *pattern,
                               uint8_t *data, size_t *pos, size_t cap);

/* ---------------------------------------------------------------------- *
 * T64 - "obraz tasmy" wymyslony przez autorow emulatorow: bezposredni
 * katalog wpisow (bez GCR/przebiegu sygnalu), kazdy wpis wskazuje wprost
 * offset danych w pliku. Naglowek 64 B, wpisy katalogu po 32 B zaczynaja
 * sie od offsetu 64: [0]=typ wpisu C64s (0=wolny,1=zwykly plik),
 * [2..3]=adres poczatkowy LE, [4..5]=adres koncowy LE, [8..11]=offset
 * danych w pliku (LE, 32-bit), [16..31]=nazwa (PETSCII, dopelniona $20).
 *
 * Celowo TYLKO DO ODCZYTU: T64 zostal pomyslany jako format dystrybucji
 * gotowych zrzutow (jego katalog ma z gory ustalona, zwykle bardzo
 * ciasna, liczbe wpisow - dopisanie nowego pliku wymagaloby przesuniecia
 * calej reszty danych w pliku), a nie jako zapisywalny nosnik - w
 * przeciwienstwie do D64 to nie jest realistyczny model prawdziwej
 * tasmy/napedu, wiec nie ma sensu udawac tu pelnej symetrii z SAVE.
 * ---------------------------------------------------------------------- */

#define T64_MAX_ENTRIES 500 /* zabezpieczenie przed nierealistycznie duzym naglowkiem */

static uint8_t *t64_find(const uint8_t *img, size_t img_size, const char *name, size_t *out_len)
{
    if (img_size < 64) return NULL;

    int used = img[36] | (img[37] << 8);
    long max_entries = (long)((img_size - 64) / 32);
    if (used <= 0 || used > max_entries) used = (int)max_entries;
    if (used > T64_MAX_ENTRIES) used = T64_MAX_ENTRIES;

    for (int i = 0; i < used; i++) {
        long eoff = 64 + (long)i * 32;
        if ((size_t)(eoff + 32) > img_size) break;
        const uint8_t *e = img + eoff;

        if (e[0] == 0) continue; /* wolny wpis */

        uint16_t start = (uint16_t)(e[2] | (e[3] << 8));
        uint16_t end   = (uint16_t)(e[4] | (e[5] << 8));
        uint32_t foff  = (uint32_t)(e[8] | (e[9] << 8) | (e[10] << 16) | ((uint32_t)e[11] << 24));

        int elen = 16;
        while (elen > 0 && e[16 + elen - 1] == 0x20) elen--;
        char ename[17];
        memcpy(ename, e + 16, (size_t)elen);
        ename[elen] = 0;

        if (!name_matches(ename, (size_t)elen, name)) continue;

        size_t data_len = (end > start) ? (size_t)(end - start) : 0;
        if (data_len == 0 || foff >= img_size) continue;
        if (foff + data_len > img_size) data_len = img_size - foff;
        if (data_len > 65536) data_len = 65536;

        uint8_t *out = malloc(data_len + 2);
        if (!out) return NULL;
        out[0] = (uint8_t)(start & 0xFF);
        out[1] = (uint8_t)(start >> 8);
        memcpy(out + 2, img + foff, data_len);
        *out_len = data_len + 2;
        return out;
    }
    return NULL;
}

static void t64_build_listing(const uint8_t *img, size_t img_size, const char *pattern,
                               uint8_t *data, size_t *pos, size_t cap);

/* ---------------------------------------------------------------------- *
 * Wspolna budowa listingu katalogu (uzywana przez diskimage_directory_listing)
 * ---------------------------------------------------------------------- */

/* Dopisuje jedna "linie" BASIC-owego programu listingu katalogu do bufora
 * (format uzywany przez prawdziwy KERNAL dla LOAD"$",8 + LIST):
 * [wskaznik_nastepnej_linii LE][numer_linii LE][tekst...][0x00]. Wskaznik
 * nastepnej linii jest PELNYM adresem pamieci (bufor zaczyna sie zawsze od
 * $0801), wiec trzeba go doliczyc na biezaco w trakcie budowania. */
static void listing_emit_line(uint8_t *data, size_t *pos, uint16_t line_number,
                               const char *text, size_t text_len)
{
    size_t line_len = 2 + 2 + text_len + 1;
    uint16_t this_addr = (uint16_t)(0x0801 + *pos);
    uint16_t next_addr = (uint16_t)(this_addr + line_len);
    data[(*pos)++] = (uint8_t)(next_addr & 0xFF);
    data[(*pos)++] = (uint8_t)(next_addr >> 8);
    data[(*pos)++] = (uint8_t)(line_number & 0xFF);
    data[(*pos)++] = (uint8_t)(line_number >> 8);
    memcpy(data + *pos, text, text_len);
    *pos += text_len;
    data[(*pos)++] = 0x00;
}

static void d64_build_listing(const uint8_t *img, size_t img_size, const char *pattern,
                               uint8_t *data, size_t *pos, size_t cap)
{
    long bam_off = d64_ts_offset(D64_BAM_TRACK, D64_BAM_SECTOR);
    const uint8_t *bam = img + bam_off;

    char dname[17];
    int dlen = 0;
    for (int k = 0; k < 16; k++) {
        if (bam[0x90 + k] == 0xA0) break;
        dname[dlen++] = (char)bam[0x90 + k];
    }
    dname[dlen] = 0;
    char id[3] = { (char)bam[0xA2], (char)bam[0xA3], 0 };

    char header[40];
    int hl = snprintf(header, sizeof(header), "\"%-16s\" %s", dname, id);
    listing_emit_line(data, pos, 0, header, (size_t)hl);

    d64_dirent_t entries[D64_MAX_ENTRIES];
    int n = d64_read_all_entries(img, img_size, entries, D64_MAX_ENTRIES);
    for (int i = 0; i < n && *pos + 40 < cap; i++) {
        if (pattern && pattern[0] &&
            !name_matches(entries[i].name, (size_t)entries[i].name_len, pattern))
            continue;
        int closed = (entries[i].type & 0x80) != 0;
        int locked = (entries[i].type & 0x40) != 0;
        char line[40];
        int ll = snprintf(line, sizeof(line), " \"%s\"%*s%c%s%s",
                           entries[i].name,
                           (int)(16 - entries[i].name_len) + 2, "",
                           closed ? ' ' : '*',
                           d64_filetype_str(entries[i].type),
                           locked ? "<" : "");
        listing_emit_line(data, pos, (uint16_t)entries[i].blocks, line, (size_t)ll);
    }

    int free_blocks = 0;
    for (int t = 1; t <= 35; t++) free_blocks += bam[4 + (t - 1) * 4];
    listing_emit_line(data, pos, (uint16_t)free_blocks, "BLOCKS FREE.", 12);
}

static void t64_build_listing(const uint8_t *img, size_t img_size, const char *pattern,
                               uint8_t *data, size_t *pos, size_t cap)
{
    int nend = 24;
    while (nend > 0 && img[40 + nend - 1] == 0x20) nend--;
    char tname[25];
    memcpy(tname, img + 40, (size_t)nend);
    tname[nend] = 0;

    char header[40];
    int hl = snprintf(header, sizeof(header), "\"%-16s\" T64",
                       nend ? tname : "TAPE");
    listing_emit_line(data, pos, 0, header, (size_t)hl);

    int used = img[36] | (img[37] << 8);
    long max_entries = (long)((img_size - 64) / 32);
    if (used <= 0 || used > max_entries) used = (int)max_entries;
    if (used > T64_MAX_ENTRIES) used = T64_MAX_ENTRIES;

    for (int i = 0; i < used && *pos + 40 < cap; i++) {
        long eoff = 64 + (long)i * 32;
        if ((size_t)(eoff + 32) > img_size) break;
        const uint8_t *e = img + eoff;
        if (e[0] == 0) continue;

        uint16_t start = (uint16_t)(e[2] | (e[3] << 8));
        uint16_t end   = (uint16_t)(e[4] | (e[5] << 8));
        int elen = 16;
        while (elen > 0 && e[16 + elen - 1] == 0x20) elen--;
        char ename[17];
        memcpy(ename, e + 16, (size_t)elen);
        ename[elen] = 0;

        if (pattern && pattern[0] && !name_matches(ename, (size_t)elen, pattern)) continue;

        int blocks = end > start ? (int)((end - start + 253) / 254) : 0;
        char line[40];
        int ll = snprintf(line, sizeof(line), " \"%s\"%*s PRG", ename, (int)(16 - elen) + 2, "");
        listing_emit_line(data, pos, (uint16_t)blocks, line, (size_t)ll);
    }

    listing_emit_line(data, pos, 0, "BLOCKS FREE.", 12);
}

/* ---------------------------------------------------------------------- *
 * D64 - zapis (SAVE): alokacja sektorow danych z BAM, wpis katalogowy,
 * ewentualne zwolnienie starego lancucha przy nadpisaniu istniejacego
 * pliku, ewentualne doalokowanie nowego sektora katalogu na sciezce 18,
 * gdy istniejace sektory katalogu sa pelne.
 * ---------------------------------------------------------------------- */

static void d64_free_chain(uint8_t *img, long bam_off, int t, int s)
{
    for (int g = 0; t != 0 && g < 664; g++) {
        long off = d64_ts_offset(t, s);
        if (off < 0) break;
        uint8_t *sec = img + off;
        int nt = sec[0], ns = sec[1];
        uint8_t *bament = img + bam_off + 4 + (t - 1) * 4;
        int byte_idx = s / 8, bit_idx = s % 8;
        if (!(bament[1 + byte_idx] & (1 << bit_idx))) {
            bament[1 + byte_idx] = (uint8_t)(bament[1 + byte_idx] | (1 << bit_idx));
            bament[0]++;
        }
        t = nt;
        s = ns;
    }
}

/* Szuka wolnego sektora na dowolnej sciezce danych (1-35, z pominieciem
 * zarezerwowanej na katalog sciezki 18). Zwraca 1 i alokuje (czysci bit w
 * BAM, zmniejsza licznik) przy sukcesie. */
static int d64_alloc_sector(uint8_t *img, long bam_off, int *out_t, int *out_s)
{
    for (int t = 1; t <= 35; t++) {
        if (t == D64_BAM_TRACK) continue;
        uint8_t *bament = img + bam_off + 4 + (t - 1) * 4;
        if (bament[0] == 0) continue;
        int spt = d64_spt(t);
        for (int s = 0; s < spt; s++) {
            int byte_idx = s / 8, bit_idx = s % 8;
            if (bament[1 + byte_idx] & (1 << bit_idx)) {
                bament[1 + byte_idx] = (uint8_t)(bament[1 + byte_idx] & ~(1 << bit_idx));
                bament[0]--;
                *out_t = t;
                *out_s = s;
                return 1;
            }
        }
    }
    return 0;
}

/* Jak wyzej, ale ograniczone do sciezki katalogowej 18 (sektor 0 to sam
 * BAM, nigdy nie alokowany) - uzywane wylacznie do rozszerzenia lancucha
 * sektorow katalogu, gdy wszystkie istniejace sa pelne. */
static int d64_alloc_dir_sector(uint8_t *img, long bam_off, int *out_t, int *out_s)
{
    uint8_t *bament = img + bam_off + 4 + (D64_BAM_TRACK - 1) * 4;
    if (bament[0] == 0) return 0;
    int spt = d64_spt(D64_BAM_TRACK);
    for (int s = 1; s < spt; s++) {
        int byte_idx = s / 8, bit_idx = s % 8;
        if (bament[1 + byte_idx] & (1 << bit_idx)) {
            bament[1 + byte_idx] = (uint8_t)(bament[1 + byte_idx] & ~(1 << bit_idx));
            bament[0]--;
            *out_t = D64_BAM_TRACK;
            *out_s = s;
            return 1;
        }
    }
    return 0;
}

static int d64_save(uint8_t *img, size_t img_size, const char *name,
                     const uint8_t *data, size_t data_len)
{
    if (img_size < D64_SIZE_35) return 0;
    if (data_len < 2 || data_len > 65536) return 0;
    /* UWAGA: adres zaladowania (pierwsze 2 bajty "data") NIE jest osobnymi
     * metadanymi w D64 - jest czescia samego strumienia bajtow pliku na
     * dysku (dokladnie tak samo, jak w zwyklym pliku .prg), wiec trafia do
     * lancucha sektorow w calosci, razem z reszta danych. */
    size_t payload_len = data_len;

    size_t name_len = strlen(name);
    if (name_len == 0 || name_len > 16) return 0;

    long bam_off = d64_ts_offset(D64_BAM_TRACK, D64_BAM_SECTOR);

    /* jesli plik o tej nazwie juz istnieje, zwolnij jego stary lancuch
     * danych i zapamietaj jego wpis katalogowy do ponownego uzycia -
     * dzieki temu SAVE dziala jak nadpisanie, tak jak na prawdziwym C64 */
    d64_dirent_t entries[D64_MAX_ENTRIES];
    int n = d64_read_all_entries(img, img_size, entries, D64_MAX_ENTRIES);
    int reuse_t = -1, reuse_s = -1, reuse_i = -1;
    for (int i = 0; i < n; i++) {
        if ((size_t)entries[i].name_len == name_len &&
            memcmp(entries[i].name, name, name_len) == 0) {
            d64_free_chain(img, bam_off, entries[i].track, entries[i].sector);
            reuse_t = entries[i].slot_track;
            reuse_s = entries[i].slot_sector;
            reuse_i = entries[i].slot_index;
            break;
        }
    }

    /* alokuj sektory danych (co najmniej 1, nawet dla pustego pliku) */
    int needed = (int)((payload_len + 253) / 254);
    if (needed == 0) needed = 1;
    if (needed > 664) return 0; /* wiecej sektorow niz ma caly dysk */

    int *chain_t = malloc((size_t)needed * sizeof(int));
    int *chain_s = malloc((size_t)needed * sizeof(int));
    if (!chain_t || !chain_s) { free(chain_t); free(chain_s); return 0; }

    int allocated = 0;
    for (; allocated < needed; allocated++) {
        if (!d64_alloc_sector(img, bam_off, &chain_t[allocated], &chain_s[allocated])) break;
    }
    if (allocated < needed) {
        /* brak miejsca - wycofaj czesciowa alokacje */
        for (int j = 0; j < allocated; j++) {
            uint8_t *bament = img + bam_off + 4 + (chain_t[j] - 1) * 4;
            int bi = chain_s[j] / 8, bb = chain_s[j] % 8;
            bament[1 + bi] = (uint8_t)(bament[1 + bi] | (1 << bb));
            bament[0]++;
        }
        free(chain_t);
        free(chain_s);
        return 0;
    }

    /* zapisz dane do zaalokowanych sektorow, tworzac lancuch */
    size_t remaining = payload_len;
    const uint8_t *src = data;
    for (int i = 0; i < needed; i++) {
        long off = d64_ts_offset(chain_t[i], chain_s[i]);
        uint8_t *sec = img + off;
        size_t chunk = remaining < 254 ? remaining : 254;
        memcpy(sec + 2, src, chunk);
        if (chunk < 254) memset(sec + 2 + chunk, 0, 254 - chunk);
        src += chunk;
        remaining -= chunk;
        if (i + 1 < needed) {
            sec[0] = (uint8_t)chain_t[i + 1];
            sec[1] = (uint8_t)chain_s[i + 1];
        } else {
            sec[0] = 0;
            sec[1] = (uint8_t)(chunk + 1);
        }
    }

    /* znajdz miejsce na wpis katalogowy: ponownie uzyty slot po
     * nadpisaniu, pierwszy wolny (typ==0) w istniejacych sektorach
     * katalogu, albo nowy sektor katalogu doalokowany na sciezce 18 */
    int slot_t = reuse_t, slot_s = reuse_s, slot_i = reuse_i;
    if (slot_t < 0) {
        int dir_t = img[bam_off], dir_s = img[bam_off + 1];
        int last_t = dir_t, last_s = dir_s;
        for (int guard = 0; dir_t != 0 && guard < 40 && slot_t < 0; guard++) {
            long off = d64_ts_offset(dir_t, dir_s);
            uint8_t *sec = img + off;
            for (int i = 0; i < 8; i++) {
                if (sec[2 + i * 32] == 0) { slot_t = dir_t; slot_s = dir_s; slot_i = i; break; }
            }
            last_t = dir_t;
            last_s = dir_s;
            int nt = sec[0], ns = sec[1];
            if (nt == 0) break;
            dir_t = nt;
            dir_s = ns;
        }
        if (slot_t < 0) {
            int new_t, new_s;
            if (!d64_alloc_dir_sector(img, bam_off, &new_t, &new_s)) {
                /* brak miejsca na katalog - wycofaj alokacje danych */
                for (int i = 0; i < needed; i++) {
                    uint8_t *bament = img + bam_off + 4 + (chain_t[i] - 1) * 4;
                    int bi = chain_s[i] / 8, bb = chain_s[i] % 8;
                    bament[1 + bi] = (uint8_t)(bament[1 + bi] | (1 << bb));
                    bament[0]++;
                }
                free(chain_t);
                free(chain_s);
                return 0;
            }
            long loff = d64_ts_offset(last_t, last_s);
            img[loff] = (uint8_t)new_t;
            img[loff + 1] = (uint8_t)new_s;
            long noff = d64_ts_offset(new_t, new_s);
            memset(img + noff, 0, 256);
            slot_t = new_t;
            slot_s = new_s;
            slot_i = 0;
        }
    }

    long soff = d64_ts_offset(slot_t, slot_s);
    uint8_t *slot = img + soff + 2 + slot_i * 32;
    memset(slot, 0, 32);
    slot[0] = 0x82; /* PRG, zamkniety */
    slot[1] = (uint8_t)chain_t[0];
    slot[2] = (uint8_t)chain_s[0];
    memset(slot + 3, 0xA0, 16);
    memcpy(slot + 3, name, name_len);
    slot[28] = (uint8_t)(needed & 0xFF);
    slot[29] = (uint8_t)(needed >> 8);

    free(chain_t);
    free(chain_s);
    return 1;
}

/* ---------------------------------------------------------------------- *
 * API publiczne
 * ---------------------------------------------------------------------- */

uint8_t *diskimage_find_prg(const char *disk_dir, const char *name, size_t *out_len)
{
    DIR *dir = opendir(disk_dir);
    if (!dir) return NULL;

    uint8_t *result = NULL;
    struct dirent *de;
    while (!result && (de = readdir(dir)) != NULL) {
        size_t nlen = strlen(de->d_name);
        int is_d64 = nlen > 4 && strcasecmp(de->d_name + nlen - 4, ".d64") == 0;
        int is_t64 = nlen > 4 && strcasecmp(de->d_name + nlen - 4, ".t64") == 0;
        if (!is_d64 && !is_t64) continue;

        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", disk_dir, de->d_name);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz <= 0 || sz > IMG_MAX_SIZE) { fclose(f); continue; }
        uint8_t *img = malloc((size_t)sz);
        if (!img) { fclose(f); continue; }
        size_t got = fread(img, 1, (size_t)sz, f);
        fclose(f);

        size_t len = 0;
        uint8_t *found = is_d64 ? d64_find(img, got, name, &len)
                                 : t64_find(img, got, name, &len);
        free(img);
        if (found) {
            result = found;
            *out_len = len;
            fprintf(stderr, "[diskimage] '%s' found in image '%s'\n", name, de->d_name);
        }
    }
    closedir(dir);
    return result;
}

uint8_t *diskimage_directory_listing(const char *disk_dir, const char *pattern, size_t *out_len)
{
    DIR *dir = opendir(disk_dir);
    if (!dir) return NULL;

    size_t cap = 16384;
    uint8_t *listing = malloc(2 + cap);
    if (!listing) { closedir(dir); return NULL; }
    listing[0] = 0x01;
    listing[1] = 0x08; /* adres zaladowania $0801 */
    uint8_t *data = listing + 2;
    size_t pos = 0;
    int done = 0;

    struct dirent *de;
    while (!done && (de = readdir(dir)) != NULL) {
        size_t nlen = strlen(de->d_name);
        int is_d64 = nlen > 4 && strcasecmp(de->d_name + nlen - 4, ".d64") == 0;
        int is_t64 = nlen > 4 && strcasecmp(de->d_name + nlen - 4, ".t64") == 0;
        if (!is_d64 && !is_t64) continue;

        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", disk_dir, de->d_name);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz <= 0 || sz > IMG_MAX_SIZE) { fclose(f); continue; }
        uint8_t *img = malloc((size_t)sz);
        if (!img) { fclose(f); continue; }
        size_t got = fread(img, 1, (size_t)sz, f);
        fclose(f);

        if (is_d64 && got >= D64_SIZE_35) {
            d64_build_listing(img, got, pattern, data, &pos, cap);
            done = 1;
        } else if (is_t64 && got >= 64) {
            t64_build_listing(img, got, pattern, data, &pos, cap);
            done = 1;
        }
        free(img);
    }
    closedir(dir);

    if (!done) { free(listing); return NULL; }

    data[pos++] = 0x00; /* koniec programu: wskaznik nastepnej linii = 0 */
    data[pos++] = 0x00;

    *out_len = 2 + pos;
    return listing;
}

int diskimage_save_prg(const char *disk_dir, const char *name, const uint8_t *data, size_t data_len)
{
    DIR *dir = opendir(disk_dir);
    if (!dir) return 0;

    char image_path[1024] = {0};
    int image_count = 0;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        size_t nlen = strlen(de->d_name);
        if (nlen > 4 && strcasecmp(de->d_name + nlen - 4, ".d64") == 0) {
            image_count++;
            snprintf(image_path, sizeof(image_path), "%s/%s", disk_dir, de->d_name);
        }
    }
    closedir(dir);
    if (image_count != 1) return 0; /* dwuznacznosc albo brak obrazu */

    FILE *f = fopen(image_path, "r+b");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < D64_SIZE_35 || sz > IMG_MAX_SIZE) { fclose(f); return 0; }
    uint8_t *img = malloc((size_t)sz);
    if (!img) { fclose(f); return 0; }
    if (fread(img, 1, (size_t)sz, f) != (size_t)sz) { free(img); fclose(f); return 0; }

    int ok = d64_save(img, (size_t)sz, name, data, data_len);
    if (ok) {
        fseek(f, 0, SEEK_SET);
        fwrite(img, 1, (size_t)sz, f);
        fprintf(stderr, "[diskimage] SAVE '%s' written to image '%s'\n", name, image_path);
    }
    fclose(f);
    free(img);
    return ok;
}
