// MDEC: macroblock decoder used by the FMV player (RLE -> dequantize -> IDCT -> YUV->RGB).
#include "hw.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static uint32_t in_buf[0x40000];
static int in_n, in_need;
static uint32_t cmd;
static uint8_t qt_y[64], qt_c[64];
static int16_t scale[64];
static double idct_m[8][8];
static uint8_t* out_buf;
static int out_n, out_pos;
static int enable_in, enable_out;
static int expect;      // 0 command, 1 data for decode, 2 quant tables, 3 scale table

static const uint8_t zigzag[64] = {
    0, 1, 5, 6, 14, 15, 27, 28, 2, 4, 7, 13, 16, 26, 29, 42, 3, 8, 12, 17, 25, 30, 41, 43, 9, 11, 18, 24, 31, 40, 44, 53,
    10, 19, 23, 32, 39, 45, 52, 54, 20, 22, 33, 38, 46, 51, 55, 60, 21, 34, 37, 47, 50, 56, 59, 61, 35, 36, 48, 49, 57, 58, 62, 63};
static uint8_t unzig[64];

static void idct(int16_t* blk) {
    double tmp[64];
    for (int x = 0; x < 8; x++)
        for (int y = 0; y < 8; y++) {
            double s = 0;
            for (int u = 0; u < 8; u++) s += blk[u * 8 + x] * idct_m[u][y];
            tmp[y * 8 + x] = s;
        }
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            double s = 0;
            for (int u = 0; u < 8; u++) s += tmp[y * 8 + u] * idct_m[u][x];
            int v = (int)lround(s);
            blk[y * 8 + x] = (int16_t)(v < -128 ? -128 : v > 127 ? 127 : v);
        }
}

// decodes one 8x8 block from the halfword stream; returns 0 at end of data
static int read_block(const uint16_t* hw, int nhw, int* pos, int16_t* blk, const uint8_t* qt) {
    memset(blk, 0, 64 * sizeof(int16_t));
    while (*pos < nhw && hw[*pos] == 0xFE00) (*pos)++;     // padding
    if (*pos >= nhw) return 0;
    uint16_t n = hw[(*pos)++];
    int q = n >> 10, k = 0;
    int val = ((int16_t)(n << 6)) >> 6;
    blk[unzig[0]] = (int16_t)(q == 0 ? val * 2 : val * qt[0]);
    while (*pos < nhw) {
        n = hw[(*pos)++];
        if (n == 0xFE00) break;
        k += (n >> 10) + 1;
        if (k > 63) break;
        val = ((int16_t)(n << 6)) >> 6;
        val = q == 0 ? val * 2 : (val * qt[k] * q + 4) / 8;
        blk[unzig[k]] = (int16_t)val;
    }
    return 1;
}

// Decodes one colour macroblock (Cr, Cb, Y0..Y3) from an RLE stream into 16x16 RGB24.
// Returns 0 at end of data.
int mdec_decode_mb_rgb(const uint16_t* hw, int nhw, int* pos, uint8_t* rgb) {
    int16_t cr[64], cb[64], y[4][64];
    if (!read_block(hw, nhw, pos, cr, qt_c) || !read_block(hw, nhw, pos, cb, qt_c)) return 0;
    for (int b = 0; b < 4; b++)
        if (!read_block(hw, nhw, pos, y[b], qt_y)) return 0;
    idct(cr); idct(cb);
    for (int b = 0; b < 4; b++) idct(y[b]);
    for (int py = 0; py < 16; py++)
        for (int px = 0; px < 16; px++) {
            int Y = y[(py >> 3) * 2 + (px >> 3)][(py & 7) * 8 + (px & 7)];
            int ci = (py >> 1) * 8 + (px >> 1);
            int r = (int)lround(Y + 1.402 * cr[ci]) + 128;
            int g = (int)lround(Y - 0.3437 * cb[ci] - 0.7143 * cr[ci]) + 128;
            int b = (int)lround(Y + 1.772 * cb[ci]) + 128;
            uint8_t* o = rgb + (py * 16 + px) * 3;
            o[0] = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
            o[1] = (uint8_t)(g < 0 ? 0 : g > 255 ? 255 : g);
            o[2] = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
        }
    return 1;
}

// Quantization table uploaded by the game's own movie player (captured, already in zigzag order).
void mdec_set_default_tables(void) {
    static const uint8_t q[64] = {
        2, 16, 16, 19, 16, 19, 22, 22, 22, 22, 22, 22, 26, 24, 26, 27, 27, 27, 26, 26, 26, 26, 27, 27,
        27, 29, 29, 29, 34, 34, 34, 29, 29, 29, 27, 27, 29, 29, 32, 32, 34, 34, 37, 38, 37, 35, 35, 34,
        35, 38, 38, 40, 40, 40, 48, 48, 46, 46, 56, 56, 58, 69, 69, 83};
    memcpy(qt_y, q, 64);
    memcpy(qt_c, q, 64);
}

static void decode(void) {
    int depth = (cmd >> 27) & 3, is_signed = (cmd >> 26) & 1, bit15 = (cmd >> 25) & 1;
    const uint16_t* hw = (const uint16_t*)in_buf;
    int nhw = in_n * 2, pos = 0;
    int bytes_per_px = depth == 2 ? 3 : depth == 3 ? 2 : 1;
    free(out_buf);
    out_buf = malloc((size_t)(nhw / 6 + 64) * 256 * 3 + 1024);
    out_n = 0;
    out_pos = 0;
    int16_t cr[64], cb[64], y[4][64];
    for (;;) {
        if (depth >= 2) {
            if (!read_block(hw, nhw, &pos, cr, qt_c) || !read_block(hw, nhw, &pos, cb, qt_c)) break;
            int ok = 1;
            for (int b = 0; b < 4 && ok; b++) ok = read_block(hw, nhw, &pos, y[b], qt_y);
            if (!ok) break;
            idct(cr); idct(cb);
            for (int b = 0; b < 4; b++) idct(y[b]);
            for (int py = 0; py < 16; py++)
                for (int px = 0; px < 16; px++) {
                    int yb = (py >> 3) * 2 + (px >> 3);
                    int Y = y[yb][(py & 7) * 8 + (px & 7)];
                    int ci = (py >> 1) * 8 + (px >> 1);
                    double R = Y + 1.402 * cr[ci], G = Y - 0.3437 * cb[ci] - 0.7143 * cr[ci], B = Y + 1.772 * cb[ci];
                    int r = (int)lround(R), g = (int)lround(G), bl = (int)lround(B);
                    r = r < -128 ? -128 : r > 127 ? 127 : r;
                    g = g < -128 ? -128 : g > 127 ? 127 : g;
                    bl = bl < -128 ? -128 : bl > 127 ? 127 : bl;
                    if (!is_signed) { r += 128; g += 128; bl += 128; }
                    if (depth == 2) {
                        out_buf[out_n++] = (uint8_t)r; out_buf[out_n++] = (uint8_t)g; out_buf[out_n++] = (uint8_t)bl;
                    } else {
                        uint16_t c = (uint16_t)(((r & 255) >> 3) | (((g & 255) >> 3) << 5) | (((bl & 255) >> 3) << 10) | (bit15 << 15));
                        out_buf[out_n++] = (uint8_t)c; out_buf[out_n++] = (uint8_t)(c >> 8);
                    }
                }
        } else {
            // monochrome (4/8-bit)
            int16_t blk[64];
            if (!read_block(hw, nhw, &pos, blk, qt_y)) break;
            idct(blk);
            for (int i = 0; i < 64; i++) {
                int v = blk[i] + (is_signed ? 0 : 128);
                if (depth == 1) out_buf[out_n++] = (uint8_t)v;
                else if (i & 1) out_buf[out_n - 1] |= (uint8_t)((v >> 4) << 4);
                else out_buf[out_n++] = (uint8_t)(v >> 4);
            }
        }
    }
    (void)bytes_per_px;
}

static void feed(uint32_t w) {
    if (expect == 0) {
        cmd = w;
        LOGD("mdec cmd %08x", w);
        int op = w >> 29;
        in_n = 0;
        if (op == 1) { expect = 1; in_need = w & 0xFFFF; }
        else if (op == 2) { expect = 2; in_need = (w & 1) ? 32 : 16; }
        else if (op == 3) { expect = 3; in_need = 32; }
        if (expect && in_need == 0) expect = 0;
        return;
    }
    if (in_n < (int)(sizeof in_buf / 4)) in_buf[in_n++] = w;
    if (in_n < in_need) return;
    if (expect == 1) {
        static int dumped;
        const char* dir = getenv("AC2_DUMP_MDEC");
        if (dir && dumped < 3) {
            char path[512];
            snprintf(path, sizeof path, "%s/mdec_in_%d.bin", dir, dumped++);
            FILE* f = fopen(path, "wb");
            if (f) { fwrite(&cmd, 4, 1, f); fwrite(in_buf, 4, in_n, f); fclose(f); }
        }
        decode();
    }
    else if (expect == 2) {
        const uint8_t* b = (const uint8_t*)in_buf;
        const char* dir = getenv("AC2_DUMP_MDEC");
        if (dir) {
            char path[512];
            snprintf(path, sizeof path, "%s/mdec_qt.bin", dir);
            FILE* f = fopen(path, "wb");
            if (f) { fwrite(b, 1, (cmd & 1) ? 128 : 64, f); fclose(f); }
        }
        memcpy(qt_y, b, 64);
        if (cmd & 1) memcpy(qt_c, b + 64, 64);
    } else {
        memcpy(scale, in_buf, sizeof scale);
    }
    expect = 0;
}

void mdec_write(uint32_t a, uint32_t v) {
    if (a == 0x1F801820) { feed(v); return; }
    if (v & 0x80000000u) { expect = 0; in_n = 0; out_n = out_pos = 0; }
    enable_in = (v >> 30) & 1;
    enable_out = (v >> 29) & 1;
}

uint32_t mdec_read(uint32_t a) {
    if (a == 0x1F801820) { uint32_t w; mdec_dma_out(&w, 1); return w; }
    uint32_t s = 0;
    if (out_pos >= out_n) s |= 1u << 31;                 // output fifo empty
    if (expect) s |= 1u << 29;                           // busy
    if (enable_in) s |= 1u << 28;
    if (enable_out && out_pos < out_n) s |= 1u << 27;
    s |= ((cmd >> 25) & 0xF) << 23;
    s |= (uint32_t)((in_need - in_n - 1) & 0xFFFF);
    return s;
}

void mdec_dma_in(const uint32_t* src, uint32_t words) { for (uint32_t i = 0; i < words; i++) feed(src[i]); }

void mdec_dma_out(uint32_t* dst, uint32_t words) {
    for (uint32_t i = 0; i < words; i++) {
        uint32_t w = 0;
        for (int k = 0; k < 4; k++) w |= (uint32_t)(out_pos < out_n ? out_buf[out_pos++] : 0) << (8 * k);
        dst[i] = w;
    }
}

__attribute__((constructor)) static void mdec_tables(void) {
    for (int i = 0; i < 64; i++) unzig[zigzag[i]] = (uint8_t)i;
    for (int u = 0; u < 8; u++)
        for (int x = 0; x < 8; x++)
            idct_m[u][x] = (u == 0 ? sqrt(0.125) : 0.5) * cos((2 * x + 1) * u * M_PI / 16.0);
}
