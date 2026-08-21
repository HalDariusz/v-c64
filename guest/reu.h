/*
 * reu.h - emulacja kontrolera RAM Expansion Unit (REU) 128 KB pod $DF00-$DF1F,
 * zgodna z rejestrowym interfejsem REC uzywanym w 1700/1750 REU.
 */
#ifndef REU_H
#define REU_H

#include <stdint.h>
#include <stdbool.h>

#define REU_SIZE (128u * 1024u)

void reu_reset(void);

uint8_t reu_reg_read(uint8_t offset);   /* offset wzgledem $DF00, 0x00-0x1F */
void    reu_reg_write(uint8_t offset, uint8_t value);

bool reu_irq_pending(void);

#endif /* REU_H */
