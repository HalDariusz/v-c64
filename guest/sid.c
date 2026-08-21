/*
 * guest/sid.c - implementation of the SID synthesizer declared in
 * sid.h: per-voice waveform generation (triangle/sawtooth/pulse/noise),
 * the ADSR envelope state machine, the simplified SVF filter, and
 * mixing the 3 voices down to the samples streamed to the host.
 *
 * Part of v-c64 - a bare-metal Commodore 64 unikernel running directly
 * on Linux /dev/kvm, with no QEMU involved.
 *
 * Author: Dariusz Nowak <hal.dariusz.nowak@gmail.com>
 */
#include "sid.h"
#include "libc_shim.h"

/* Tabele czasow narastania (attack) i opadania (decay/release) w ms,
 * indeksowane 4-bitowa wartoscia rejestru - wartosci wg opublikowanej
 * charakterystyki ukladu SID 6581 (dane techniczne, jawnodostepne). */
static const double attack_ms[16] = {
    2, 8, 16, 24, 38, 56, 68, 80, 100, 250, 500, 800, 1000, 3000, 5000, 8000
};
static const double decay_release_ms[16] = {
    6, 24, 48, 72, 114, 168, 204, 240, 300, 750, 1500, 2400, 3000, 9000, 15000, 24000
};

typedef enum { ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE, ENV_IDLE } env_state_t;

typedef struct {
    double phase;        /* 0.0 - 1.0, pozycja w cyklu fali */
    uint32_t noise_lfsr;
    double envelope;      /* 0.0 - 1.0 */
    env_state_t env_state;
    bool gate_prev;
} voice_rt_t;

static uint8_t regs[0x19];
static voice_rt_t voice[3];
static double filter_low, filter_band;

#define SID_CLOCK_HZ 985248.0
#define DT (1.0 / SID_SAMPLE_RATE)

static uint16_t voice_freq(int v)     { return (uint16_t)(regs[v*7+0] | (regs[v*7+1] << 8)); }
static uint16_t voice_pw(int v)       { return (uint16_t)((regs[v*7+2] | (regs[v*7+3] << 8)) & 0x0FFF); }
static uint8_t  voice_ctrl(int v)     { return regs[v*7+4]; }
static uint8_t  voice_attack(int v)   { return (uint8_t)(regs[v*7+5] >> 4); }
static uint8_t  voice_decay(int v)    { return (uint8_t)(regs[v*7+5] & 0x0F); }
static uint8_t  voice_sustain(int v)  { return (uint8_t)(regs[v*7+6] >> 4); }
static uint8_t  voice_release(int v)  { return (uint8_t)(regs[v*7+6] & 0x0F); }

void sid_reset(void)
{
    memset(regs, 0, sizeof(regs));
    memset(voice, 0, sizeof(voice));
    for (int v = 0; v < 3; v++) voice[v].noise_lfsr = 0x7FFFF8u;
    filter_low = filter_band = 0.0;
}

uint8_t sid_reg_read(uint8_t offset)
{
    offset &= 0x1F;
    switch (offset) {
        case 0x19: case 0x1A: return 0xFF; /* potencjometry - brak podlaczonego joysticka analog. */
        case 0x1B: return (uint8_t)(voice[2].noise_lfsr & 0xFF); /* Osc3 - odczyt "losowy" */
        case 0x1C: return (uint8_t)(voice[2].envelope * 255.0);  /* Env3 */
    }
    if (offset < 0x19) return regs[offset];
    return 0xFF;
}

void sid_reg_write(uint8_t offset, uint8_t value)
{
    offset &= 0x1F;
    if (offset < 0x19) regs[offset] = value;
}

/* --- generacja fali dla jednego glosu ------------------------------------ */

static double gen_waveform(int v, uint16_t freq)
{
    uint8_t ctrl = voice_ctrl(v);
    if (ctrl & 0x08) return 0.0; /* TEST: akumulator fazy zatrzymany */

    /* Priorytet przy kilku jednoczesnie ustawionych bitach fali - realny SID
     * generuje egzotyczne fale mieszane analogowo; tutaj wybieramy jedna,
     * w kolejnosci najbardziej charakterystycznej dla typowego uzycia. */
    if (ctrl & 0x80) { /* NOISE */
        /* LFSR przesuwany raz na probke - przyblizenie, nie jest cyklowo
         * zsynchronizowany z rejestrem czestotliwosci jak w oryginale. */
        uint32_t *lfsr = &voice[v].noise_lfsr;
        uint32_t bit = ((*lfsr >> 22) ^ (*lfsr >> 17)) & 1u;
        *lfsr = ((*lfsr << 1) | bit) & 0x7FFFFFu;
        uint8_t top8 = (uint8_t)(*lfsr >> 15);
        return ((double)top8 / 127.5) - 1.0;
    }
    if (ctrl & 0x40) { /* PULSE */
        double pw = (double)voice_pw(v) / 4095.0;
        return (voice[v].phase < pw) ? 1.0 : -1.0;
    }
    if (ctrl & 0x20) { /* SAWTOOTH */
        return voice[v].phase * 2.0 - 1.0;
    }
    if (ctrl & 0x10) { /* TRIANGLE (+ przyblizony ring-mod z glosu 3) */
        double tri = (voice[v].phase < 0.5)
                       ? (voice[v].phase * 4.0 - 1.0)
                       : (3.0 - voice[v].phase * 4.0);
        if ((ctrl & 0x04) && v != 2) { /* RING MOD: modulacja glosem 3 */
            double s3 = (voice[2].phase < 0.5) ? 1.0 : -1.0;
            tri *= s3;
        }
        return tri;
    }
    (void)freq;
    return 0.0;
}

static void advance_envelope(int v)
{
    uint8_t ctrl = voice_ctrl(v);
    bool gate = (ctrl & 0x01) != 0;

    if (gate && !voice[v].gate_prev) voice[v].env_state = ENV_ATTACK;
    if (!gate && voice[v].gate_prev) voice[v].env_state = ENV_RELEASE;
    voice[v].gate_prev = gate;

    switch (voice[v].env_state) {
        case ENV_ATTACK: {
            double ms = attack_ms[voice_attack(v)];
            voice[v].envelope += DT / (ms / 1000.0);
            if (voice[v].envelope >= 1.0) { voice[v].envelope = 1.0; voice[v].env_state = ENV_DECAY; }
            break;
        }
        case ENV_DECAY: {
            double target = voice_sustain(v) / 15.0;
            double ms = decay_release_ms[voice_decay(v)];
            double k = DT / (ms / 1000.0 / 3.0);
            if (k > 1.0) k = 1.0;
            voice[v].envelope += (target - voice[v].envelope) * k;
            if (voice[v].envelope <= target + 0.001) voice[v].env_state = ENV_SUSTAIN;
            break;
        }
        case ENV_SUSTAIN: {
            double target = voice_sustain(v) / 15.0;
            voice[v].envelope = target;
            break;
        }
        case ENV_RELEASE: {
            double ms = decay_release_ms[voice_release(v)];
            double k = DT / (ms / 1000.0 / 3.0);
            if (k > 1.0) k = 1.0;
            voice[v].envelope += (0.0 - voice[v].envelope) * k;
            if (voice[v].envelope < 0.0005) { voice[v].envelope = 0.0; voice[v].env_state = ENV_IDLE; }
            break;
        }
        case ENV_IDLE:
            voice[v].envelope = 0.0;
            break;
    }
}

int16_t sid_generate_sample(void)
{
    double mixed_filtered = 0.0;
    double mixed_dry = 0.0;
    uint8_t resfilt = regs[0x17];
    uint8_t mode_vol = regs[0x18];

    for (int v = 0; v < 3; v++) {
        uint16_t freq = voice_freq(v);
        double phase_inc = ((double)freq * SID_CLOCK_HZ / 16777216.0) * DT;
        voice[v].phase += phase_inc;
        if (voice[v].phase >= 1.0) voice[v].phase -= (double)(long)voice[v].phase;

        advance_envelope(v);

        if (v == 2 && (mode_vol & 0x80) && !(resfilt & 0x04)) {
            continue; /* glos 3 odlaczony od wyjscia (i nie idzie przez filtr) */
        }

        double sample = gen_waveform(v, freq) * voice[v].envelope;
        if (resfilt & (1u << v)) mixed_filtered += sample;
        else mixed_dry += sample;
    }

    /* Uproszczony filtr state-variable (Chamberlin). Wspolczynnik f jest
     * przyblizeniem "malego kata" 2*sin(pi*fc/fs) bez uzycia funkcji
     * transcendentnych (brak libm w srodowisku freestanding). */
    uint16_t fc = (uint16_t)(((regs[0x16] << 3) | (regs[0x15] & 0x07)) & 0x7FF);
    double cutoff_hz = 30.0 + ((double)fc / 2047.0) * (12000.0 - 30.0);
    double f = 2.0 * 3.14159265358979 * cutoff_hz * DT;
    if (f > 1.9) f = 1.9;
    double resonance = (double)((resfilt >> 4) & 0x0F);
    double q = 1.0 / (0.7 + resonance / 15.0 * 1.5);

    double high = mixed_filtered - filter_low - q * filter_band;
    filter_band += f * high;
    filter_low  += f * filter_band;

    double filtered_out = 0.0;
    if (mode_vol & 0x10) filtered_out += filter_low;
    if (mode_vol & 0x20) filtered_out += filter_band;
    if (mode_vol & 0x40) filtered_out += high;

    double vol = (double)(mode_vol & 0x0F) / 15.0;
    double out = (mixed_dry + filtered_out) * vol / 3.0;

    if (out > 1.0) out = 1.0;
    if (out < -1.0) out = -1.0;
    return (int16_t)(out * 32000.0);
}
