/*
 * host/diskimage.h - lightweight host-side read/write access to disk
 * (.d64) and tape (.t64) images.
 *
 * This is NOT an emulation of the 1541 drive or a datasette (no IEC, no
 * GCR, no signal timing on the CASSETTE port) - it's a shortcut similar
 * in spirit to the existing fast-loader: the host itself parses/modifies
 * the image structure (directory, BAM, sector chains) and hands
 * data to/from the guest as if it were a plain .prg file from disk/.
 *
 * Scope:
 *   - .d64 (standard 35-track, 40-track also tolerated on read): full
 *     read and WRITE support (SAVE appends/overwrites a file on the
 *     single mounted .d64 image in the disk directory).
 *   - .t64 ("tape image" - an emulator-specific format, no GCR): READ
 *     ONLY - in practice used to distribute ready-made dumps, not
 *     designed as a writable format (see diskimage.c).
 *   - directory listing (LOAD"$",8 + LIST) for both formats.
 *
 * Part of v-c64 - a bare-metal Commodore 64 unikernel running directly
 * on Linux /dev/kvm, with no QEMU involved.
 *
 * Author: Dariusz Nowak <hal.dariusz.nowak@gmail.com>
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* Przeszukuje wszystkie pliki .d64/.t64 (rowniez .D64/.T64) w katalogu
 * disk_dir w poszukiwaniu wpisu o nazwie "name" (obslugiwany koncowy
 * wildcard "*" - dopasowuje reszte, tez jako samo "*" = "pierwszy plik" -
 * oraz "?" w dowolnym miejscu jako pojedynczy dowolny znak).
 *
 * Przy trafieniu zwraca zaalokowany (malloc) bufor w formacie .prg
 * (2-bajtowy adres zaladowania, little-endian, po ktorym nastepuja dane
 * pliku) - dokladnie ten sam uklad, jakiego oczekuje reszta fastloadera.
 * Wywolujacy musi go zwolnic przez free(). *out_len to calkowity rozmiar
 * bufora (naglowek + dane). Zwraca NULL, jesli nic nie pasuje w zadnym
 * obrazie znalezionym w katalogu. */
uint8_t *diskimage_find_prg(const char *disk_dir, const char *name, size_t *out_len);

/* Buduje listing katalogu (gotowy "program" BASIC do LIST, dokladnie w
 * takim ukladzie bajtow, jaki tworzy prawdziwy KERNAL dla LOAD"$",8) dla
 * PIERWSZEGO obrazu .d64 lub .t64 znalezionego w disk_dir. "pattern" to
 * opcjonalny filtr nazw (jak w LOAD"$:S*",8) - moze byc NULL lub pustym
 * lancuchem, wtedy pokazane sa wszystkie pliki.
 *
 * Zwraca zaalokowany (malloc) bufor w formacie .prg (2-bajtowy adres $0801
 * + dane), ktory wywolujacy musi zwolnic. Zwraca NULL, jesli w disk_dir
 * nie ma zadnego rozpoznanego obrazu. */
uint8_t *diskimage_directory_listing(const char *disk_dir, const char *pattern, size_t *out_len);

/* Zapisuje plik "name" (SAVE) do JEDYNEGO obrazu .d64 znalezionego w
 * disk_dir - dziala tylko wtedy, gdy w katalogu jest dokladnie jeden plik
 * .d64 (w przeciwnym razie zwraca 0, a wywolujacy powinien spasc z
 * powrotem na zwykly zapis pliku .prg na hosta, tak jak dotychczas -
 * dzieki temu istniejace przeplywy pracy nie zmieniaja zachowania, dopoki
 * uzytkownik swiadomie nie zamontuje dokladnie jednego zapisywalnego
 * obrazu). Jesli plik o tej nazwie juz istnieje na obrazie, jest
 * nadpisywany (stare sektory danych sa zwalniane w BAM).
 *
 * "data" to bufor w formacie .prg (2-bajtowy adres zaladowania LE + surowe
 * dane), data_len to jego calkowity rozmiar. Zwraca 1 przy sukcesie
 * (obraz na dysku hosta zostal faktycznie nadpisany), 0 gdy zapis sie nie
 * udal (brak/wiele obrazow .d64, obraz nie jest standardowym 35-sciezkowym
 * D64, brak wolnego miejsca na dane lub w katalogu, zla nazwa). */
int diskimage_save_prg(const char *disk_dir, const char *name, const uint8_t *data, size_t data_len);
