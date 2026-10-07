// SPU: 24 ADPCM voices with ADSR, CD/XA input, mixed in software to 44.1 kHz stereo.
#include "hw.h"
#include <stdlib.h>
#include <string.h>

static uint8_t sram[512 * 1024];
static uint16_t regs[512];          // 0x1F801C00..0x1F801FFF as halfwords
#define REG(a) regs[((a) - 0x1F801C00) >> 1]

typedef struct {
    uint32_t addr, repeat, start;  // byte addresses in SPU RAM
    uint32_t counter;              // pitch counter (12-bit fraction)
    int16_t buf[28];
    int16_t prev[2];               // ADPCM history
    int16_t last[4];               // last 4 output samples (interpolation)
    int pos;                       // sample index inside buf
    int phase;                     // 0 off, 1 attack, 2 decay, 3 sustain, 4 release
    int32_t level;
    int wait;
    int key_on_pending;
} Voice;
static Voice voices[24];
static uint32_t endx;
static uint32_t xfer_addr;

// ------------------------------------------------------------------ ADPCM
static const int F0[5] = {0, 60, 115, 98, 122}, F1[5] = {0, 0, -52, -55, -60};

static void decode_block(Voice* v) {
    const uint8_t* b = &sram[v->addr & 0x7FFF0];
    int shift = b[0] & 15, filt = (b[0] >> 4) & 7;
    if (shift > 12) shift = 9;
    if (filt > 4) filt = 4;
    for (int i = 0; i < 28; i++) {
        int nib = (b[2 + i / 2] >> ((i & 1) * 4)) & 15;
        int s = (int16_t)(nib << 12) >> shift;
        s += (v->prev[0] * F0[filt] + v->prev[1] * F1[filt] + 32) >> 6;
        s = s < -32768 ? -32768 : s > 32767 ? 32767 : s;
        v->prev[1] = v->prev[0];
        v->prev[0] = (int16_t)s;
        v->buf[i] = (int16_t)s;
    }
    if (b[1] & 4) v->repeat = v->addr;   // loop start
}

// ------------------------------------------------------------------ ADSR
static int adsr_tick(Voice* v, int vi) {
    uint16_t lo = REG(0x1F801C08 + vi * 16), hi = REG(0x1F801C0A + vi * 16);
    int exp = 0, dec = 0, shift = 0, step = 0;
    int32_t sustain = ((lo & 15) + 1) * 0x800;
    switch (v->phase) {
        case 1: exp = lo >> 15; shift = (lo >> 10) & 31; step = 7 - ((lo >> 8) & 3); break;
        case 2: exp = 1; dec = 1; shift = ((lo >> 4) & 15) * 4; step = -8; break;
        case 3:
            exp = hi >> 15; dec = (hi >> 14) & 1; shift = (hi >> 8) & 31;
            step = dec ? -8 + ((hi >> 6) & 3) : 7 - ((hi >> 6) & 3);
            break;
        case 4: exp = (hi >> 5) & 1; dec = 1; shift = (hi & 31) * 4; step = -8; break;
        default: return 0;
    }
    if (v->wait > 0) { v->wait--; return v->level; }
    int cycles = 1 << (shift - 11 > 0 ? shift - 11 : 0);
    int32_t s = step << (11 - shift > 0 ? 11 - shift : 0);
    if (exp && !dec && v->level > 0x6000) cycles *= 4;
    if (exp && dec) s = s * v->level / 0x8000;
    v->wait = cycles - 1;
    v->level += s;
    if (v->level > 0x7FFF) v->level = 0x7FFF;
    if (v->level < 0) v->level = 0;
    if (v->phase == 1 && v->level >= 0x7FFF) v->phase = 2;
    else if (v->phase == 2 && v->level <= sustain) v->phase = 3;
    else if (v->phase == 4 && v->level <= 0) v->phase = 0;
    return v->level;
}

int g_spu_stats[4];   // debug: key-ons, DMA words, FIFO halfwords, key-ons of a voice whose start block is all zero
static void key_on(int i) {
    Voice* v = &voices[i];
    g_spu_stats[0]++;
    v->start = (uint32_t)REG(0x1F801C06 + i * 16) * 8;
    v->addr = v->start;
    v->counter = 0;
    v->pos = 0;
    v->prev[0] = v->prev[1] = 0;
    memset(v->last, 0, sizeof v->last);
    v->phase = 1;
    v->level = 0;
    v->wait = 0;
    endx &= ~(1u << i);
    decode_block(v);
    { int z = 1; for (int k = 0; k < 16; k++) if (sram[(v->addr + k) & 0x7FFFF]) z = 0; g_spu_stats[3] += z; }
}

static void key_off(int i) {
    if (voices[i].phase) { voices[i].phase = 4; voices[i].wait = 0; }
}

// ------------------------------------------------------------------ XA / CD input
#define XA_BUF 65536
static int16_t xa_buf[XA_BUF * 2];
static int xa_r, xa_w, xa_rate = 37800;
static uint32_t xa_frac;

void spu_xa_push(const int16_t* lr, int frames, int rate) {
    xa_rate = rate;
    for (int i = 0; i < frames; i++) {
        int next = (xa_w + 1) % XA_BUF;
        if (next == xa_r) break;   // full: drop
        xa_buf[xa_w * 2] = lr[i * 2];
        xa_buf[xa_w * 2 + 1] = lr[i * 2 + 1];
        xa_w = next;
    }
}

static void xa_sample(int* l, int* r) {
    *l = *r = 0;
    if (xa_r == xa_w) return;
    *l = xa_buf[xa_r * 2];
    *r = xa_buf[xa_r * 2 + 1];
    xa_frac += (uint32_t)((double)xa_rate / 44100.0 * 65536.0);
    while (xa_frac >= 65536 && xa_r != xa_w) { xa_frac -= 65536; xa_r = (xa_r + 1) % XA_BUF; }
}

// ------------------------------------------------------------------ mixer
// volume register: bit15 = 0 -> fixed 15-bit signed volume; bit15 = 1 -> sweep (approximated as full)
static int vol(uint16_t v) {
    if (v & 0x8000) return 0x7FFF;
    return (int16_t)(uint16_t)(v << 1);
}

int spu_active_voices(void) {   // debug: bitmask-count of voices playing, with their phase digits
    int n = 0;
    for (int i = 0; i < 24; i++) if (voices[i].phase) n++;
    return n;
}
double g_mix_energy[3];   // debug: sum of squares of XA / voices, sample count
uint16_t spu_reg_debug(uint32_t a) { return REG(a); }
void spu_render(int16_t* out, int frames) {
    uint16_t cnt = REG(0x1F801DAA);
    int enabled = (cnt & 0x8000) != 0, unmuted = (cnt & 0x4000) != 0;
    int mvl = vol(REG(0x1F801D80)), mvr = vol(REG(0x1F801D82));
    int cdl = (int16_t)REG(0x1F801DB0), cdr = (int16_t)REG(0x1F801DB2);
    for (int f = 0; f < frames; f++) {
        int suml = 0, sumr = 0;
        if (enabled) {
            for (int i = 0; i < 24; i++) {
                Voice* v = &voices[i];
                if (!v->phase) continue;
                int env = adsr_tick(v, i);
                // linear interpolation between the last two samples
                int frac = v->counter & 0xFFF;
                int s = v->last[3] + (((v->buf[v->pos] - v->last[3]) * frac) >> 12);
                s = s * env >> 15;
                int vl = vol(REG(0x1F801C00 + i * 16)), vr = vol(REG(0x1F801C02 + i * 16));
                suml += s * vl >> 15;
                sumr += s * vr >> 15;
                REG(0x1F801C0C + i * 16) = (uint16_t)env;
                // advance
                uint32_t pitch = REG(0x1F801C04 + i * 16);
                if (pitch > 0x4000) pitch = 0x4000;
                v->counter += pitch;
                while (v->counter >= 0x1000) {
                    v->counter -= 0x1000;
                    v->last[3] = v->buf[v->pos];
                    if (++v->pos >= 28) {
                        v->pos = 0;
                        uint8_t flags = sram[(v->addr & 0x7FFF0) + 1];
                        if (flags & 1) {                      // loop end
                            endx |= 1u << i;
                            v->addr = v->repeat;
                            if (!(flags & 2)) { v->phase = 0; v->level = 0; }
                        } else {
                            v->addr = (v->addr + 16) & 0x7FFFF;
                        }
                        decode_block(v);
                    }
                }
            }
        }
        int xl, xr;
        xa_sample(&xl, &xr);
        {   // debug: energy of the XA (music) part as heard, vs the voices, for AC2_PERFLOG
            int hx = (cnt & 1) ? (xl * cdl >> 15) * mvl >> 15 : 0, hv = (suml * mvl) >> 15;
            g_mix_energy[0] += (double)hx * hx; g_mix_energy[1] += (double)hv * hv; g_mix_energy[2] += 1;
        }
        if (cnt & 1) { suml += xl * cdl >> 15; sumr += xr * cdr >> 15; }
        int l = unmuted ? suml * mvl >> 15 : 0, r = unmuted ? sumr * mvr >> 15 : 0;
        if (cnt & 1) { if (!unmuted) { l = xl * cdl >> 15; r = xr * cdr >> 15; } }
        out[f * 2] = (int16_t)(l < -32768 ? -32768 : l > 32767 ? 32767 : l);
        out[f * 2 + 1] = (int16_t)(r < -32768 ? -32768 : r > 32767 ? 32767 : r);
    }
}

// ------------------------------------------------------------------ registers
uint16_t spu_read(uint32_t a) {
    if (a == 0x1F801DAE) {   // SPUSTAT: mirrors SPUCNT bits 0-5, transfers are instant (never busy)
        uint16_t cnt = REG(0x1F801DAA);
        return (cnt & 0x3F) | ((cnt & 0x20) ? 0x0080 : 0);
    }
    if (a == 0x1F801D9C) return (uint16_t)endx;
    if (a == 0x1F801D9E) return (uint16_t)(endx >> 16);
    if (a == 0x1F801DA6) return (uint16_t)(xfer_addr / 8);
    return REG(a);
}

void spu_write(uint32_t a, uint16_t v) {
    if (dbg_env("AC2_SPULOG") && (a == 0x1F801D84 || a == 0x1F801D86 || a == 0x1F801D98 || a == 0x1F801D9A ||
                                 a == 0x1F801DAA || a == 0x1F801D80 || a == 0x1F801D82 || a == 0x1F801DA2 ||
                                 (a < 0x1F801D80 && (a & 15) <= 2 && (v & 0x8000)))) {
        static int n;
        if (n++ < 3000) LOGI("spu %08x = %04x", a, v);
    }
    REG(a) = v;
    switch (a) {
        case 0x1F801D88: for (int i = 0; i < 16; i++) if (v >> i & 1) key_on(i); break;
        case 0x1F801D8A: for (int i = 0; i < 8; i++) if (v >> i & 1) key_on(16 + i); break;
        case 0x1F801D8C: for (int i = 0; i < 16; i++) if (v >> i & 1) key_off(i); break;
        case 0x1F801D8E: for (int i = 0; i < 8; i++) if (v >> i & 1) key_off(16 + i); break;
        case 0x1F801DA6: xfer_addr = (uint32_t)v * 8; break;
        case 0x1F801DA8:                                 // manual transfer FIFO
            g_spu_stats[2]++;
            sram[xfer_addr & 0x7FFFF] = (uint8_t)v;
            sram[(xfer_addr + 1) & 0x7FFFF] = (uint8_t)(v >> 8);
            xfer_addr = (xfer_addr + 2) & 0x7FFFF;
            break;
        case 0x1F801D9C: case 0x1F801D9E: break;
        default: break;
    }
    if (a >= 0x1F801C00 && a < 0x1F801D80 && (a & 15) == 0xE) {
        // writing the repeat address takes effect immediately
        voices[(a - 0x1F801C00) >> 4].repeat = (uint32_t)v * 8;
    }
}

void spu_dma_write(const uint32_t* src, uint32_t words) {
    g_spu_stats[1] += words;
    for (uint32_t i = 0; i < words; i++) {
        memcpy(&sram[xfer_addr & 0x7FFFC], &src[i], 4);
        xfer_addr = (xfer_addr + 4) & 0x7FFFF;
    }
}

void spu_dma_read(uint32_t* dst, uint32_t words) {
    for (uint32_t i = 0; i < words; i++) {
        memcpy(&dst[i], &sram[xfer_addr & 0x7FFFC], 4);
        xfer_addr = (xfer_addr + 4) & 0x7FFFF;
    }
}

void spu_init(void) {
    memset(sram, 0, sizeof sram);
    memset(regs, 0, sizeof regs);
}
