// Native FMV player: replaces the game's libcd-stream/libpress movie loop.
// Reads STR sectors straight from the disc image, decodes the "BS" v2 bitstream to MDEC run-length
// codes, rebuilds frames with the MDEC macroblock decoder and plays the interleaved XA audio.
#include "hw.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

int iso_read_raw(int lba, uint8_t* out2352);
int iso_find(const char* name, uint32_t* size);
uint8_t* iso_read_file(const char* name, uint32_t* size);
int cd_last_setloc(void);
int xa_decode_sector(const uint8_t* s, int32_t hist[2][2], int16_t* out, int* rate);
int mdec_decode_mb_rgb(const uint16_t* hw, int nhw, int* pos, uint8_t* rgb);
void mdec_set_default_tables(void);
void platform_audio_push(const int16_t* lr, int frames);
int platform_audio_queued_ms(void);
void platform_present_rgb24(const uint8_t* rgb, int w, int h);
int platform_poll(void);                     // returns nonzero when the window was closed
uint16_t platform_pad_buttons(int port);

// ------------------------------------------------------------------ VLC table (AC coefficients)
typedef struct { const char* code; int run, level; } Vlc;
static const Vlc vlc[] = {
    {"11", 0, 1}, {"011", 1, 1}, {"0100", 0, 2}, {"0101", 2, 1}, {"00101", 0, 3}, {"00111", 3, 1},
    {"00110", 4, 1}, {"000110", 1, 2}, {"000111", 5, 1}, {"000101", 6, 1}, {"000100", 7, 1},
    {"0000110", 0, 4}, {"0000100", 2, 2}, {"0000111", 8, 1}, {"0000101", 9, 1},
    {"00100110", 0, 5}, {"00100001", 0, 6}, {"00100101", 1, 3}, {"00100100", 3, 2}, {"00100111", 10, 1},
    {"00100011", 11, 1}, {"00100010", 12, 1}, {"00100000", 13, 1},
    {"0000001010", 0, 7}, {"0000001100", 1, 4}, {"0000001011", 2, 3}, {"0000001111", 4, 2},
    {"0000001001", 5, 2}, {"0000001110", 14, 1}, {"0000001101", 15, 1}, {"0000001000", 16, 1},
    {"000000011101", 0, 8}, {"000000011000", 0, 9}, {"000000010011", 0, 10}, {"000000010000", 0, 11},
    {"000000011011", 1, 5}, {"000000010100", 2, 4}, {"000000011100", 3, 3}, {"000000010010", 4, 3},
    {"000000011110", 6, 2}, {"000000010101", 7, 2}, {"000000010001", 8, 2}, {"000000011111", 17, 1},
    {"000000011010", 18, 1}, {"000000011001", 19, 1}, {"000000010111", 20, 1}, {"000000010110", 21, 1},
    {"0000000011010", 0, 12}, {"0000000011001", 0, 13}, {"0000000011000", 0, 14}, {"0000000010111", 0, 15},
    {"0000000010110", 1, 6}, {"0000000010101", 1, 7}, {"0000000010100", 2, 5}, {"0000000010011", 3, 4},
    {"0000000010010", 5, 3}, {"0000000010001", 9, 2}, {"0000000010000", 10, 2}, {"0000000011111", 22, 1},
    {"0000000011110", 23, 1}, {"0000000011101", 24, 1}, {"0000000011100", 25, 1}, {"0000000011011", 26, 1},
    {"00000000011111", 0, 16}, {"00000000011110", 0, 17}, {"00000000011101", 0, 18}, {"00000000011100", 0, 19},
    {"00000000011011", 0, 20}, {"00000000011010", 0, 21}, {"00000000011001", 0, 22}, {"00000000011000", 0, 23},
    {"00000000010111", 0, 24}, {"00000000010110", 0, 25}, {"00000000010101", 0, 26}, {"00000000010100", 0, 27},
    {"00000000010011", 0, 28}, {"00000000010010", 0, 29}, {"00000000010001", 0, 30}, {"00000000010000", 0, 31},
    {"000000000011000", 0, 32}, {"000000000010111", 0, 33}, {"000000000010110", 0, 34}, {"000000000010101", 0, 35},
    {"000000000010100", 0, 36}, {"000000000010011", 0, 37}, {"000000000010010", 0, 38}, {"000000000010001", 0, 39},
    {"000000000010000", 0, 40}, {"000000000011111", 1, 8}, {"000000000011110", 1, 9}, {"000000000011101", 1, 10},
    {"000000000011100", 1, 11}, {"000000000011011", 1, 12}, {"000000000011010", 1, 13}, {"000000000011001", 1, 14},
    {"0000000000010011", 1, 15}, {"0000000000010010", 1, 16}, {"0000000000010001", 1, 17}, {"0000000000010000", 1, 18},
    {"0000000000010100", 6, 3}, {"0000000000011010", 11, 2}, {"0000000000011001", 12, 2}, {"0000000000011000", 13, 2},
    {"0000000000010111", 14, 2}, {"0000000000010110", 15, 2}, {"0000000000010101", 16, 2}, {"0000000000011111", 27, 1},
    {"0000000000011110", 28, 1}, {"0000000000011101", 29, 1}, {"0000000000011100", 30, 1}, {"0000000000011011", 31, 1},
};

typedef struct { uint16_t value; uint8_t len; uint8_t run; uint16_t level; } Code;
static Code codes[sizeof vlc / sizeof vlc[0]];
static int ncodes;

static void vlc_init(void) {
    if (ncodes) return;
    for (unsigned i = 0; i < sizeof vlc / sizeof vlc[0]; i++) {
        uint16_t v = 0;
        int len = (int)strlen(vlc[i].code);
        for (int k = 0; k < len; k++) v = (uint16_t)((v << 1) | (vlc[i].code[k] - '0'));
        codes[ncodes++] = (Code){v, (uint8_t)len, (uint8_t)vlc[i].run, (uint16_t)vlc[i].level};
    }
}

// ------------------------------------------------------------------ bit reader (16-bit LE words, MSB first)
typedef struct { const uint8_t* d; int n; int pos; } Bits;   // pos in bits

static inline int peek_bit(Bits* b, int i) {
    int p = b->pos + i;
    int word = p >> 4;
    if (word * 2 + 1 >= b->n) return 0;
    uint16_t w = (uint16_t)(b->d[word * 2] | (b->d[word * 2 + 1] << 8));
    return (w >> (15 - (p & 15))) & 1;
}
static inline uint32_t get_bits(Bits* b, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v = (v << 1) | (uint32_t)peek_bit(b, i);
    b->pos += n;
    return v;
}

// DC size codes (v3): luminance and chrominance tables, value = DC difference bit count
static const char* dc_luma[9] = {"100", "00", "01", "101", "110", "1110", "11110", "111110", "1111110"};
static const char* dc_chroma[9] = {"00", "01", "10", "110", "1110", "11110", "111110", "1111110", "11111110"};

static int read_dc_size(Bits* b, const char* const* tab) {
    for (int size = 0; size < 9; size++) {
        const char* code = tab[size];
        int len = (int)strlen(code), ok = 1;
        for (int i = 0; i < len && ok; i++) ok = peek_bit(b, i) == code[i] - '0';
        if (ok) { b->pos += len; return size; }
    }
    return -1;
}

// Decodes a v2/v3 frame bitstream into MDEC halfword codes. Returns the number of codes, -1 on error.
static int bs_decode(const uint8_t* frame, int size, int mbs, uint16_t* out, int max) {
    if (size < 8) return -1;
    uint16_t qscale = (uint16_t)(frame[4] | (frame[5] << 8));
    uint16_t version = (uint16_t)(frame[6] | (frame[7] << 8));
    if (version != 2 && version != 3) { LOGE("movie: unsupported bitstream version %u", version); return -1; }
    Bits b = {frame + 8, size - 8, 0};
    int n = 0;
    int pred[3] = {0, 0, 0};   // v3 DC predictors: Cr, Cb, Y
    for (int mb = 0; mb < mbs; mb++) {
        for (int blk = 0; blk < 6; blk++) {
            if (n + 2 >= max) return -1;
            int dc;
            if (version == 2) {
                dc = (int)get_bits(&b, 10);
                if (dc & 0x200) dc -= 0x400;
            } else {
                // v3: Huffman-coded DC difference, predicted per component, in steps of 4
                // (validated bit-exact against the game's own decoder)
                int comp = blk < 2 ? blk : 2;
                int sz = read_dc_size(&b, comp == 2 ? dc_luma : dc_chroma);
                if (sz < 0) return -1;
                int diff = 0;
                if (sz) {
                    int v = (int)get_bits(&b, sz);
                    diff = (v >> (sz - 1)) ? v : v - (1 << sz) + 1;
                }
                pred[comp] += diff * 4;
                dc = pred[comp];
            }
            out[n++] = (uint16_t)((qscale << 10) | (dc & 0x3FF));
            for (int guard = 0; guard < 64; guard++) {
                if (n + 2 >= max) return -1;
                if (peek_bit(&b, 0) == 1 && peek_bit(&b, 1) == 0) {   // EOB "10"
                    b.pos += 2;
                    out[n++] = 0xFE00;
                    break;
                }
                // escape "000001"
                if (!peek_bit(&b, 0) && !peek_bit(&b, 1) && !peek_bit(&b, 2) && !peek_bit(&b, 3) &&
                    !peek_bit(&b, 4) && peek_bit(&b, 5)) {
                    b.pos += 6;
                    int run = (int)get_bits(&b, 6);
                    int lvl = (int)get_bits(&b, 10);
                    out[n++] = (uint16_t)((run << 10) | (lvl & 0x3FF));
                    continue;
                }
                int found = 0;
                uint32_t acc = 0;
                for (int len = 1; len <= 16 && !found; len++) {
                    acc = (acc << 1) | (uint32_t)peek_bit(&b, len - 1);
                    for (int k = 0; k < ncodes; k++) {
                        if (codes[k].len == len && codes[k].value == acc) {
                            b.pos += len;
                            int sign = (int)get_bits(&b, 1);
                            int lvl = sign ? -codes[k].level : codes[k].level;
                            out[n++] = (uint16_t)((codes[k].run << 10) | (lvl & 0x3FF));
                            found = 1;
                            break;
                        }
                    }
                }
                if (!found) return -1;
            }
        }
    }
    return n;
}

// ------------------------------------------------------------------ player
static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

// Looks up the movie that starts at `lba` in ACE2.STH (type 8 entries); returns its length in sectors.
__attribute__((unused)) static int movie_length(int lba) {
    uint32_t sz;
    int stp = iso_find("ACE2.STP", NULL);
    uint8_t* sth = iso_read_file("ACE2.STH", &sz);
    int len = 0;
    if (sth && stp >= 0) {
        uint32_t n;
        memcpy(&n, sth + 4, 4);
        for (uint32_t i = 0; i < n && 16 + 8 * (i + 1) <= sz; i++) {
            uint32_t a, b;
            memcpy(&a, sth + 16 + 8 * i, 4);
            memcpy(&b, sth + 20 + 8 * i, 4);
            if ((a >> 28) == 8 && stp + (int)b == lba) len = (int)(a & 0x0FFFFFFF);
        }
    }
    free(sth);
    return len;
}

// Simple linear resampler XA -> 44.1 kHz
typedef struct { double pos; int16_t prev[2]; } Resampler;
static int resample(Resampler* r, const int16_t* in, int n, int rate, int16_t* out) {
    double step = (double)rate / 44100.0;
    int m = 0;
    while (r->pos < n) {
        int i = (int)r->pos;
        double f = r->pos - i;
        for (int ch = 0; ch < 2; ch++) {
            int a = i == 0 ? r->prev[ch] : in[(i - 1) * 2 + ch];
            int b = in[i * 2 + ch];
            out[m * 2 + ch] = (int16_t)(a + (b - a) * f);
        }
        m++;
        r->pos += step;
    }
    r->pos -= n;
    if (n) { r->prev[0] = in[(n - 1) * 2]; r->prev[1] = in[(n - 1) * 2 + 1]; }
    return m;
}

int g_no_movies;
int movie_play(int start_lba, int nsectors) {
    if (g_no_movies || g_replaying) return 1;
    vlc_init();
    mdec_set_default_tables();
    LOGI("movie: native playback LBA %d, %d sectors", start_lba, nsectors);
    uint8_t raw[2352];
    uint8_t* frame = malloc(512 * 1024);
    uint16_t* rle = malloc(sizeof(uint16_t) * 400000);
    uint8_t* rgb = malloc(640 * 480 * 3);
    uint8_t mbbuf[16 * 16 * 3];
    int32_t hist[2][2] = {{0, 0}, {0, 0}};
    Resampler rs = {0, {0, 0}};
    int16_t pcm[4032 * 2], pcm44[12000 * 2];
    int cur_frame = -1, frame_bytes = 0, have = 0;
    uint16_t prev_buttons = platform_pad_buttons(0);
    double t0 = now_s();
    int shown = 0, skipped = 0;

    for (int s = 0; s < nsectors; s++) {
        if (!iso_read_raw(start_lba + s, raw)) break;
        uint8_t submode = raw[18];
        if (submode & 0x04) {
            int rate;
            int n = xa_decode_sector(raw, hist, pcm, &rate);
            int m = resample(&rs, pcm, n, rate, pcm44);
            // keep at most ~0.4 s of audio queued so it stays in sync with the picture
            while (platform_audio_queued_ms() > 400) {
                struct timespec ts = {0, 2000000};
                nanosleep(&ts, NULL);
            }
            platform_audio_push(pcm44, m);
            continue;
        }
        const uint8_t* d = raw + 24;
        if (d[0] != 0x60 || d[1] != 0x01) continue;            // not a video sector
        int idx = d[4] | (d[5] << 8), count = d[6] | (d[7] << 8);
        int fno = d[8] | (d[9] << 8) | (d[10] << 16) | (d[11] << 24);
        int w = d[16] | (d[17] << 8), h = d[18] | (d[19] << 8);
        if (fno != cur_frame) { cur_frame = fno; frame_bytes = 0; have = 0; }
        if (idx < count && idx * 2016 + 2016 <= 512 * 1024) {
            memcpy(frame + idx * 2016, d + 32, 2016);
            have++;
            if (idx * 2016 + 2016 > frame_bytes) frame_bytes = idx * 2016 + 2016;
        }
        if (have != count) continue;

        // a complete frame: decode and show it at its time (150 sectors per second)
        int mbw = (w + 15) / 16, mbh = (h + 15) / 16;
        int n = bs_decode(frame, frame_bytes, mbw * mbh, rle, 400000);
        if (n > 0 && w <= 640 && h <= 480) {
            int pos = 0;
            for (int mx = 0; mx < mbw; mx++)
                for (int my = 0; my < mbh; my++) {
                    if (!mdec_decode_mb_rgb(rle, n, &pos, mbbuf)) break;
                    for (int y = 0; y < 16; y++) {
                        int py = my * 16 + y;
                        if (py >= h) break;
                        for (int x = 0; x < 16; x++) {
                            int px = mx * 16 + x;
                            if (px >= w) break;
                            memcpy(rgb + (py * w + px) * 3, mbbuf + (y * 16 + x) * 3, 3);
                        }
                    }
                }
            double due = t0 + (s + 1) / 150.0;
            double now = now_s();
            if (now < due) {
                double wait = due - now;
                struct timespec ts = {(time_t)wait, (long)((wait - (time_t)wait) * 1e9)};
                nanosleep(&ts, NULL);
            }
            if (now - due < 0.2) { platform_present_rgb24(rgb, w, h); shown++; }
        } else if (n <= 0) {
            LOGD("movie: frame %d could not be decoded", fno);
        }
        have = 0;
        if (platform_poll()) { g_quit = 1; break; }
        if (g_quit) break;
        // skip with any newly pressed button
        uint16_t bt = platform_pad_buttons(0);
        if ((uint16_t)(~bt & prev_buttons) != 0) { skipped = 1; break; }
        prev_buttons = bt;
    }
    LOGI("movie: %d frames shown%s", shown, skipped ? " (skipped)" : "");
    free(frame);
    free(rle);
    free(rgb);
    return skipped;
}

// ------------------------------------------------------------------ hooks
int hle_hook(CPU* c, uint32_t addr) {
    if (dbg_env("AC2_NOHOOK")) return 0;
    if (addr == 0x801614E0) {
        // intro movie loop: a0 points to {length in sectors | type<<28, CdlLOC (BCD m,s,f)} of the movie
        uint32_t loc = mem_r32(c->r[4]);
        #define BCD(v) ((((v) >> 4) & 15) * 10 + ((v) & 15))
        int lba = (BCD(loc & 0xFF) * 60 + BCD((loc >> 8) & 0xFF)) * 75 + BCD((loc >> 16) & 0xFF) - 150;
        int len = (int)(mem_r32(c->r[4] - 4) & 0x0FFFFFFF);
        LOGD("movie hook: entry %08x %08x -> LBA %d, %d sectors, %u frames", mem_r32(c->r[4] - 4), loc, lba, len, c->r[5]);
        if (len <= 0 || lba <= 0) { LOGE("movie: bad entry, using the game's player"); return 0; }
        movie_play(lba, len);
        // the original loop ends with DecDCTReset(0), StUnSetRing, CdControlB(CdlPause, 0, 0)
        c->r[4] = 0;
        call_indirect(c, 0x80165488);
        call_indirect(c, 0x8016CAB0);
        c->r[4] = 9; c->r[5] = 0; c->r[6] = 0;
        call_indirect(c, 0x80169ED0);
        c->r[2] = 0;
        return 1;
    }
    return 0;
}
