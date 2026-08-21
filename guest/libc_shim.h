/*
 * libc_shim.h - deklaracje memset()/memcpy() dla srodowiska freestanding.
 *
 * Nie wlaczamy systemowego <string.h>: w -m32 -ffreestanding bez pelnego
 * gcc-multilib/libc6-dev-i386 (naglowki 32-bitowe) jest on niedostepny, a i
 * tak nie chcemy zadnej zaleznosci od libc hosta w kodzie goscia. Definicje
 * tych dwoch funkcji znajduja sie w guest/kernel.c.
 */
#ifndef LIBC_SHIM_H
#define LIBC_SHIM_H

void *memset(void *dst, int val, unsigned int n);
void *memcpy(void *dst, const void *src, unsigned int n);

#endif /* LIBC_SHIM_H */
