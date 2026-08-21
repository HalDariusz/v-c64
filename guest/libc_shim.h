/*
 * guest/libc_shim.h - memset()/memcpy() declarations for the freestanding
 * environment.
 *
 * We don't pull in the system <string.h>: under -m32 -ffreestanding
 * without the full gcc-multilib/libc6-dev-i386 (32-bit headers) it isn't
 * available anyway, and the guest code shouldn't depend on the host's
 * libc regardless. Both functions are defined in guest/kernel.c.
 *
 * Part of v-c64 - a bare-metal Commodore 64 unikernel running directly
 * on Linux /dev/kvm, with no QEMU involved.
 *
 * Author: Dariusz Nowak <hal.dariusz.nowak@gmail.com>
 */
#ifndef LIBC_SHIM_H
#define LIBC_SHIM_H

void *memset(void *dst, int val, unsigned int n);
void *memcpy(void *dst, const void *src, unsigned int n);

#endif /* LIBC_SHIM_H */
