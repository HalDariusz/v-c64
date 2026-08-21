/*
 * guest/reu.h - emulation of the 128 KB RAM Expansion Unit (REU)
 * controller at $DF00-$DF1F, compatible with the register interface used
 * by the 1700/1750 REC chip.
 *
 * Part of v-c64 - a bare-metal Commodore 64 unikernel running directly
 * on Linux /dev/kvm, with no QEMU involved.
 *
 * Author: Dariusz Nowak <hal.dariusz.nowak@gmail.com>
 */
#ifndef REU_H
#define REU_H

#include <stdint.h>
#include <stdbool.h>

#define REU_SIZE (128u * 1024u)

void reu_reset(void);

uint8_t reu_reg_read(uint8_t offset);   /* offset relative to $DF00, 0x00-0x1F */
void    reu_reg_write(uint8_t offset, uint8_t value);

bool reu_irq_pending(void);

#endif /* REU_H */
