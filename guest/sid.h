/*
 * guest/sid.h - emulation of the MOS 6581/8580 SID synthesizer
 * ($D400-$D7FF): 3 voices (triangle/sawtooth/pulse/noise + ADSR), a
 * simplified analog filter (SVF low/band/high-pass), and a mixer feeding
 * the host's audio buffer.
 *
 * A note on accuracy: this is real-time software synthesis, NOT a
 * sample-by-sample, cycle-accurate analog reimplementation like reSID.
 * The waveforms, ADSR envelope, and filter are physically plausible but
 * simplified approximations (see the comments in sid.c).
 *
 * Part of v-c64 - a bare-metal Commodore 64 unikernel running directly
 * on Linux /dev/kvm, with no QEMU involved.
 *
 * Author: Dariusz Nowak <hal.dariusz.nowak@gmail.com>
 */
#ifndef SID_H
#define SID_H

#include <stdint.h>
#include <stdbool.h>

#define SID_SAMPLE_RATE 44100

void sid_reset(void);

uint8_t sid_reg_read(uint8_t offset);
void    sid_reg_write(uint8_t offset, uint8_t value);

/* Generates a single audio sample (16-bit signed, mono) from the current
 * register state. Called by kernel.c at a rate of SID_SAMPLE_RATE
 * samples/s (derived from the CPU clock cycles). */
int16_t sid_generate_sample(void);

#endif /* SID_H */
