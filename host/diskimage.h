/* diskimage.h - lekki odczyt/zapis obrazow dysku (.d64) i tasmy (.t64) hosta.
 *
 * To NIE jest emulacja stacji 1541 ani magnetofonu (bez IEC, bez GCR, bez
 * przebiegu sygnalu na porcie CASSETTE) - to skrot analogiczny do
 * istniejacego fastloadera: host sam parsuje/modyfikuje strukture obrazu
 * (katalog, BAM, lancuchy sektorow) i podaje/przyjmuje dane goscia tak,
 * jakby to byl zwykly plik .prg z katalogu disk/.
 *
 * Zakres:
 *   - .d64 (35-sciezkowy standard, tolerowany takze 40-sciezkowy przy
 *     odczycie): pelny odczyt i ZAPIS (SAVE dopisuje/nadpisuje plik na
 *     jedynym zamontowanym obrazie .d64 w katalogu dyskietki).
 *   - .t64 ("obraz tasmy" - format wlasny emulatorow, bez GCR): tylko
 *     ODCZYT - w praktyce sluzy do dystrybucji gotowych zrzutow, nie jest
 *     pomyslany jako format zapisywalny (patrz diskimage.c).
 *   - listing katalogu (LOAD"$",8 + LIST) dla obu formatow.
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
