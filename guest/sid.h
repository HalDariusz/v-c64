/*
 * sid.h - emulacja syntezatora MOS 6581/8580 SID ($D400-$D7FF):
 * 3 glosy (trojkat/pila/impuls/szum + ADSR), filtr analogowy (uproszczony
 * SVF low/band/high-pass) i mikser do bufora audio hosta.
 *
 * Uwaga o dokladnosci: to jest synteza programowa czasu rzeczywistego,
 * NIE probka-po-probce cyklicznie dokladna reimplementacja analogowa jak
 * reSID. Ksztalty fal, obwiednia ADSR i filtr sa fizycznie wiarygodnymi,
 * ale uproszczonymi przyblizeniami (patrz komentarze w sid.c).
 */
#ifndef SID_H
#define SID_H

#include <stdint.h>
#include <stdbool.h>

#define SID_SAMPLE_RATE 44100

void sid_reset(void);

uint8_t sid_reg_read(uint8_t offset);
void    sid_reg_write(uint8_t offset, uint8_t value);

/* Generuje pojedyncza probke audio (16-bit signed, mono) na podstawie
 * biezacego stanu rejestrow. Wywolywane przez kernel.c w tempie
 * SID_SAMPLE_RATE probek/s (przeliczonym z cykli zegara procesora). */
int16_t sid_generate_sample(void);

#endif /* SID_H */
