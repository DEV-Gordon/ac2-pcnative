// GPU: command processor + software rasterizer into a 16-bit VRAM.
// Internal resolution: the VRAM is stored at S x the PS1 size (S = 1, 2 or 4). Polygons are rasterized
// at full resolution; texture/CLUT reads, sprites, lines, fills and CPU transfers work on native
// "texels" that are S x S blocks. With S = 1 this is exactly the PS1 behaviour.
//
// Threading: drawing commands (polygons, sprites, lines, fills) become jobs carrying a snapshot of the draw
// state. Every worker runs every job in order but only touches its own bands of scaled rows, so the result is
// identical to the single-threaded one. The main thread waits for the workers (gpu_sync) before anything that
// reads or writes VRAM itself, and whenever a job would sample pixels still being drawn or overwrite pixels a
// queued job has yet to sample (see submit).
#include "hw.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <unistd.h>
#include <time.h>

static int S = 1, SSHIFT = 0;               // scale and log2(scale)
static int VW = 1024, VH = 512;             // stored VRAM size
static uint16_t* vram;
// Native-resolution mirror holding the top-left sample of every S x S block. All texture/CLUT/CPU reads use it:
// at 4x the scaled VRAM is 16 MB and texture rows are 16 KB apart, so sampling it directly thrashes the cache.
// With S = 1 it aliases vram. Every write to a block's top-left sample must also go to the mirror.
static uint16_t* vnat;
#define VR(x, y) vram[((y) & (VH - 1)) * VW + ((x) & (VW - 1))]          /* scaled coordinates */
#define VN(x, y) vnat[((y) & 511) * 1024 + ((x) & 1023)]                  /* native coordinates */

// draw state
static int tp_x, tp_y, tp_depth, semi_mode, dither, draw_to_disp, tex_disable;
static int rect_flip_x, rect_flip_y;
static int allow_tex_disable;   // GP1(09h): E1 bit 11 only works when this is enabled
static int tw_mask_x, tw_mask_y, tw_off_x, tw_off_y;
static int da_x1, da_y1, da_x2, da_y2;     // drawing area (inclusive)
static int off_x, off_y;
static int mask_set, mask_check;
// display state
static int disp_x, disp_y, disp_enabled, dmode;
static int h_range1 = 0x260, h_range2 = 0xC60, v_range1 = 16, v_range2 = 256;
static int dma_dir, gpu_irq, field;
// command buffer
static uint32_t cmd[16];
static int ncmd, need;
// image transfers
static int xfer_mode;                       // 0 none, 1 CPU->VRAM, 2 VRAM->CPU
static int xf_x, xf_y, xf_w, xf_h, xf_i, xf_n, xf_cx, xf_cy;
// polylines
static int poly_line, poly_gouraud, poly_semi;
static uint32_t pl_color, pl_next_color, pl_xy;
static int pl_expect_color;

static long sync_why[8];
const uint16_t* gpu_vram(void) { sync_why[5]++; gpu_sync(); return vram; }
int gpu_scale(void) { return S; }

static void native_block(int x, int y, uint16_t c) {   // writes one native pixel as an S x S block
    for (int j = 0; j < S; j++)
        for (int i = 0; i < S; i++) VR((x << SSHIFT) + i, (y << SSHIFT) + j) = c;
    if (S > 1) VN(x, y) = c;
}

static inline int sext11(uint32_t v) { return ((int)(v << 21)) >> 21; }

// Snapshot of the state a drawing job needs (the globals keep changing while jobs are queued).
typedef struct {
    int da_x1, da_y1, da_x2, da_y2, off_x, off_y;
    int mask_set, mask_check, semi_mode, dither;
    int tw_mask_x, tw_mask_y, tw_off_x, tw_off_y;
    int tp_x, tp_y, tp_depth, rect_flip_x, rect_flip_y, tex_disable;
} DS;

static DS snapshot(void) {
    DS s = {da_x1, da_y1, da_x2, da_y2, off_x, off_y, mask_set, mask_check, semi_mode, dither,
            tw_mask_x, tw_mask_y, tw_off_x, tw_off_y, tp_x, tp_y, tp_depth, rect_flip_x, rect_flip_y, tex_disable};
    return s;
}

// Row ownership: scaled row y belongs to worker (y / 8) % nt. Bands are a multiple of S, so the S scaled rows of
// one native row (and its mirror row) always have the same owner.
typedef struct { int tid, nt; } Own;
#define BAND_SHIFT 3
static inline int owned(const Own* o, int sy) { return o->nt == 1 || (int)(((unsigned)sy >> BAND_SHIFT) % (unsigned)o->nt) == o->tid; }

// ------------------------------------------------------------------ pixels
static const int dither_tab[4][4] = {{-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2}};

static inline uint16_t blend(uint16_t back, uint16_t front, int mode) {
    int br = back & 31, bg = (back >> 5) & 31, bb = (back >> 10) & 31;
    int fr = front & 31, fg = (front >> 5) & 31, fb = (front >> 10) & 31;
    int r, g, b;
    switch (mode) {
        case 0: r = (br + fr) >> 1; g = (bg + fg) >> 1; b = (bb + fb) >> 1; break;
        case 1: r = br + fr; g = bg + fg; b = bb + fb; break;
        case 2: r = br - fr; g = bg - fg; b = bb - fb; break;
        default: r = br + (fr >> 2); g = bg + (fg >> 2); b = bb + (fb >> 2); break;
    }
    r = r < 0 ? 0 : r > 31 ? 31 : r;
    g = g < 0 ? 0 : g > 31 ? 31 : g;
    b = b < 0 ? 0 : b > 31 ? 31 : b;
    return (uint16_t)(r | (g << 5) | (b << 10) | (front & 0x8000));
}

// debug: AC2_WATCH="x0,y0,x1,y1" logs the first write into that VRAM region per GP0 command (forces 1 thread)
static int watch_on = -1, wx0, wy0, wx1, wy1, watch_logged;
static const char* watch_what = "draw";
static void watch_init(void) {
    const char* w = getenv("AC2_WATCH");
    watch_on = w && sscanf(w, "%d,%d,%d,%d", &wx0, &wy0, &wx1, &wy1) == 4;
}
static void watch_hit(int x, int y) {
    if (watch_logged) return;
    watch_logged = 1;
    LOGI("WATCH %s writes (%d,%d): cmd %08x %08x %08x %08x %08x area %d,%d-%d,%d off %d,%d", watch_what, x, y,
         cmd[0], cmd[1], cmd[2], cmd[3], cmd[4], da_x1, da_y1, da_x2, da_y2, off_x, off_y);
}
#define WATCH(x, y) do { if (watch_on > 0 && (x) >= wx0 && (x) <= wx1 && (y) >= wy0 && (y) <= wy1) watch_hit(x, y); } while (0)

static inline void plot(const DS* st, const Own* o, int x, int y, uint16_t c, int semi) {   // scaled coordinates
    if (!owned(o, y & (VH - 1))) return;
    WATCH((x >> SSHIFT) & 1023, (y >> SSHIFT) & 511);
    uint16_t* p = &VR(x, y);
    if (st->mask_check && (*p & 0x8000)) return;
    if (semi) c = blend(*p, c, st->semi_mode);
    *p = c | (st->mask_set ? 0x8000 : 0);
    if (S > 1 && !((x | y) & (S - 1))) VN(x >> SSHIFT, y >> SSHIFT) = *p;
}

// texel fetch with texture window; returns 0 for transparent
static inline uint16_t texel(const DS* st, int u, int v, int tpx, int tpy, int depth, int clut_x, int clut_y) {
    u = (u & ~(st->tw_mask_x * 8)) | ((st->tw_off_x & st->tw_mask_x) * 8);
    v = (v & ~(st->tw_mask_y * 8)) | ((st->tw_off_y & st->tw_mask_y) * 8);
    u &= 255; v &= 255;
    int y = tpy + v;
    switch (depth) {
        case 0: {
            uint16_t w = VN(tpx + (u >> 2), y);
            int idx = (w >> ((u & 3) * 4)) & 15;
            return VN(clut_x + idx, clut_y);
        }
        case 1: {
            uint16_t w = VN(tpx + (u >> 1), y);
            int idx = (w >> ((u & 1) * 8)) & 255;
            return VN(clut_x + idx, clut_y);
        }
        default:
            return VN(tpx + u, y);
    }
}

static inline uint16_t shade(int r, int g, int b, int x, int y, int dith) {
    if (dith) {
        int d = dither_tab[(y >> SSHIFT) & 3][(x >> SSHIFT) & 3];   // original pattern size
        r += d; g += d; b += d;
    }
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    return (uint16_t)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
}

// texel (15-bit) modulated by an 8-bit colour (128 = 1.0)
static inline uint16_t modulate(uint16_t t, int r, int g, int b, int x, int y, int dith) {
    int tr = (t & 31) << 3, tg = ((t >> 5) & 31) << 3, tb = ((t >> 10) & 31) << 3;
    uint16_t c = shade((tr * r) >> 7, (tg * g) >> 7, (tb * b) >> 7, x, y, dith);
    return c | (t & 0x8000);
}

// ------------------------------------------------------------------ triangles
typedef struct { int x, y, r, g, b, u, v; } V;

typedef struct {
    int textured, gouraud, semi, raw;
    int tpx, tpy, depth, clut_x, clut_y;
} Prim;

static inline int64_t edge(const V* a, const V* b, int x, int y) {
    return (int64_t)(b->x - a->x) * (y - a->y) - (int64_t)(b->y - a->y) * (x - a->x);
}

// Narrows [lo, hi] to the x where t + e * (x - x0) >= 0 (t = biased edge value at x0). Returns 0 if empty.
static inline int span_edge(int64_t t, int64_t e, int x0, int* lo, int* hi) {
    if (e > 0) {
        if (t < 0) { int64_t k = (-t + e - 1) / e; if (x0 + k > *lo) *lo = (int)(x0 + k > *hi + 1 ? *hi + 1 : x0 + k); }
    } else if (e < 0) {
        if (t < 0) return 0;
        int64_t k = t / -e;
        if (x0 + k < *hi) *hi = (int)(x0 + k);
    } else if (t < 0) return 0;
    return 1;
}

typedef struct { int64_t r, g, b, u, v; } Attr;
typedef struct { int64_t rx, gx, bx, ux, vx; } Grad;

// One scanline of a triangle. textured/gouraud/raw/depth are compile-time constants at every call site, so each
// primitive kind gets its own loop; all state the loop needs is hoisted into locals.
static inline __attribute__((always_inline)) void span(const DS* st, int x0, int x1, int y, Attr* at, const Grad* gr,
                                                       const V* a, const Prim* p, int dith, int textured, int gouraud,
                                                       int raw, int depth) {
    int64_t r = at->r, g = at->g, bl = at->b, u = at->u, v = at->v;
    const int sh = SSHIFT, vw = VW;
    uint16_t* row = &vram[(size_t)(y & (VH - 1)) * vw];
    const int mchk = st->mask_check, sm = st->semi_mode, psemi = p->semi, watch = watch_on > 0;
    const uint16_t ms = st->mask_set ? 0x8000 : 0;
    const int uand = ~(st->tw_mask_x * 8) & 255, uor = (st->tw_off_x & st->tw_mask_x) * 8 & 255;
    const int vand = ~(st->tw_mask_y * 8) & 255, vor = (st->tw_off_y & st->tw_mask_y) * 8 & 255;
    const int tpx = p->tpx, tpy = p->tpy, clx = p->clut_x;
    const uint16_t* clut = &vnat[(p->clut_y & 511) * 1024];
    uint16_t* nrow = S > 1 && !(y & (S - 1)) ? &vnat[((y >> sh) & 511) * 1024] : NULL;   // mirror row, if any
    for (int x = x0; x <= x1; x++) {
        int cr = a->r, cg = a->g, cb = a->b;
        if (gouraud) { cr = (int)(r >> 16); cg = (int)(g >> 16); cb = (int)(bl >> 16); }
        uint16_t col;
        int stp;
        int draw = 1;
        if (textured) {
            int tu = ((int)(u >> 16) & uand) | uor, tv = ((int)(v >> 16) & vand) | vor;
            const uint16_t* trow = &vnat[((tpy + tv) & 511) * 1024];
            uint16_t t;
            if (depth == 0) t = clut[(clx + ((trow[(tpx + (tu >> 2)) & 1023] >> ((tu & 3) * 4)) & 15)) & 1023];
            else if (depth == 1) t = clut[(clx + ((trow[(tpx + (tu >> 1)) & 1023] >> ((tu & 1) * 8)) & 255)) & 1023];
            else t = trow[(tpx + tu) & 1023];
            draw = t != 0;
            col = raw ? t : modulate(t, cr, cg, cb, x, y, dith);
            stp = psemi && (t & 0x8000);
        } else {
            col = shade(cr, cg, cb, x, y, dith);
            stp = psemi;
        }
        if (draw) {
            if (watch) WATCH((x >> sh) & 1023, (y >> sh) & 511);
            uint16_t* d = &row[x & (vw - 1)];
            if (!(mchk && (*d & 0x8000))) {
                if (stp) col = blend(*d, col, sm);
                *d = col | ms;
                if (nrow && !(x & (S - 1))) nrow[(x >> sh) & 1023] = *d;
            }
        }
        if (gouraud) { r += gr->rx; g += gr->gx; bl += gr->bx; }
        if (textured) { u += gr->ux; v += gr->vx; }
    }
}

static void triangle(const DS* st, const Own* o, V a, V b, V c, const Prim* p) {
    int64_t area = edge(&a, &b, c.x, c.y);
    if (area == 0) return;
    if (area < 0) { V t = b; b = c; c = t; area = -area; }
    int minx = a.x < b.x ? (a.x < c.x ? a.x : c.x) : (b.x < c.x ? b.x : c.x);
    int maxx = a.x > b.x ? (a.x > c.x ? a.x : c.x) : (b.x > c.x ? b.x : c.x);
    int miny = a.y < b.y ? (a.y < c.y ? a.y : c.y) : (b.y < c.y ? b.y : c.y);
    int maxy = a.y > b.y ? (a.y > c.y ? a.y : c.y) : (b.y > c.y ? b.y : c.y);
    if (maxx - minx >= (1024 << SSHIFT) || maxy - miny >= (512 << SSHIFT)) return;
    int ax1 = st->da_x1 << SSHIFT, ay1 = st->da_y1 << SSHIFT;
    int ax2 = ((st->da_x2 + 1) << SSHIFT) - 1, ay2 = ((st->da_y2 + 1) << SSHIFT) - 1;
    if (minx < ax1) minx = ax1;
    if (miny < ay1) miny = ay1;
    if (maxx > ax2) maxx = ax2;
    if (maxy > ay2) maxy = ay2;
    if (minx > maxx || miny > maxy) return;

    int dith = st->dither && (p->gouraud || (p->textured && !p->raw));
    // top-left rule: edges that are not top/left exclude their pixels
    #define BIAS(p0, p1) ((p1.y > p0.y || (p1.y == p0.y && p1.x < p0.x)) ? 0 : -1)
    int64_t b0 = BIAS(b, c), b1 = BIAS(c, a), b2 = BIAS(a, b);

    // edge functions are linear: step them incrementally
    int64_t e0x = -(int64_t)(c.y - b.y), e1x = -(int64_t)(a.y - c.y), e2x = -(int64_t)(b.y - a.y);
    int64_t e0y = (int64_t)(c.x - b.x), e1y = (int64_t)(a.x - c.x), e2y = (int64_t)(b.x - a.x);
    int64_t w0r = edge(&b, &c, minx, miny), w1r = edge(&c, &a, minx, miny), w2r = edge(&a, &b, minx, miny);

    // attribute plane gradients in 16.16 fixed point (one division per triangle, like the hardware)
    #define GRAD(fa, fb, fc, gx, gy, base) do { \
        int64_t da_ = (int64_t)(fb) - (fa), db_ = (int64_t)(fc) - (fa); \
        gx = ((da_ * (c.y - a.y) - db_ * (b.y - a.y)) << 16) / area; \
        gy = ((db_ * (b.x - a.x) - da_ * (c.x - a.x)) << 16) / area; \
        base = ((int64_t)(fa) << 16) + gx * (minx - a.x) + gy * (miny - a.y) + 0x8000; } while (0)
    int64_t rx = 0, ry = 0, rr = 0, gxx = 0, gyy = 0, gg = 0, bx = 0, by = 0, bb = 0;
    int64_t ux = 0, uy = 0, uu = 0, vx = 0, vy = 0, vv = 0;
    if (p->gouraud) {
        GRAD(a.r, b.r, c.r, rx, ry, rr);
        GRAD(a.g, b.g, c.g, gxx, gyy, gg);
        GRAD(a.b, b.b, c.b, bx, by, bb);
    }
    if (p->textured) {
        GRAD(a.u, b.u, c.u, ux, uy, uu);
        GRAD(a.v, b.v, c.v, vx, vy, vv);
    }
    Grad gr = {rx, gxx, bx, ux, vx};
    int kind = p->textured ? 4 | (p->gouraud ? 2 : 0) | (p->raw ? 1 : 0) | (p->depth << 3) : (p->gouraud ? 2 : 0);
    for (int y = miny; y <= maxy; y++) {
        // exact span of this row: the pixels where all three (biased) edge functions are >= 0
        int lo = minx, hi = maxx;
        if (!owned(o, y) || !span_edge(w0r + b0, e0x, minx, &lo, &hi) || !span_edge(w1r + b1, e1x, minx, &lo, &hi) ||
            !span_edge(w2r + b2, e2x, minx, &lo, &hi) || lo > hi) goto next_row;
        {
            int64_t k = lo - minx;
            Attr at = {rr + rx * k, gg + gxx * k, bb + bx * k, uu + ux * k, vv + vx * k};
            // constant flags let the compiler emit one specialised loop per primitive kind
            #define SPAN(t, g, r, d) span(st, lo, hi, y, &at, &gr, &a, p, dith, t, g, r, d)
            switch (kind) {
                case 0: case 1: SPAN(0, 0, 0, 0); break;
                case 2: case 3: SPAN(0, 1, 0, 0); break;
                case 4 + 8 * 0: SPAN(1, 0, 0, 0); break;
                case 5 + 8 * 0: SPAN(1, 0, 1, 0); break;
                case 6 + 8 * 0: SPAN(1, 1, 0, 0); break;
                case 7 + 8 * 0: SPAN(1, 1, 1, 0); break;
                case 4 + 8 * 1: SPAN(1, 0, 0, 1); break;
                case 5 + 8 * 1: SPAN(1, 0, 1, 1); break;
                case 6 + 8 * 1: SPAN(1, 1, 0, 1); break;
                case 7 + 8 * 1: SPAN(1, 1, 1, 1); break;
                case 4 + 8 * 2: SPAN(1, 0, 0, 2); break;
                case 5 + 8 * 2: SPAN(1, 0, 1, 2); break;
                case 6 + 8 * 2: SPAN(1, 1, 0, 2); break;
                default: SPAN(1, 1, 1, 2); break;
            }
            #undef SPAN
        }
    next_row:
        w0r += e0y; w1r += e1y; w2r += e2y;
        rr += ry; gg += gyy; bb += by; uu += uy; vv += vy;
    }
}

// ------------------------------------------------------------------ lines, sprites, fills
static void line_seg(const DS* st, const Own* o, int x0, int y0, uint32_t c0, int x1, int y1, uint32_t c1, int gouraud,
                     int semi) {
    int dx = x1 - x0, dy = y1 - y0;
    if (abs(dx) >= 1024 || abs(dy) >= 512) return;
    int steps = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
    for (int i = 0; i <= steps; i++) {
        int x = steps ? x0 + dx * i / steps : x0, y = steps ? y0 + dy * i / steps : y0;
        if (x < st->da_x1 || x > st->da_x2 || y < st->da_y1 || y > st->da_y2) continue;
        if (!owned(o, y << SSHIFT)) continue;
        int r = c0 & 255, g = (c0 >> 8) & 255, b = (c0 >> 16) & 255;
        if (gouraud && steps) {
            r += ((int)(c1 & 255) - r) * i / steps;
            g += ((int)((c1 >> 8) & 255) - g) * i / steps;
            b += ((int)((c1 >> 16) & 255) - b) * i / steps;
        }
        uint16_t col = shade(r, g, b, x << SSHIFT, y << SSHIFT, st->dither && gouraud);
        for (int sj = 0; sj < S; sj++)
            for (int si = 0; si < S; si++) plot(st, o, (x << SSHIFT) + si, (y << SSHIFT) + sj, col, semi);
    }
}

static void rect(const DS* st, const Own* o, const uint32_t* c) {
    uint32_t op = c[0] >> 24;
    int textured = (op & 4) != 0, semi = (op & 2) != 0, raw = (op & 1) != 0;
    int size = (op >> 3) & 3;
    int k = 1;
    uint32_t xy = c[k++];
    int x = sext11(xy) + st->off_x, y = sext11(xy >> 16) + st->off_y;
    int u0 = 0, v0 = 0, clut_x = 0, clut_y = 0;
    if (textured) {
        uint32_t uv = c[k++];
        u0 = uv & 255; v0 = (uv >> 8) & 255;
        clut_x = ((uv >> 16) & 0x3F) * 16; clut_y = (uv >> 22) & 511;
    }
    int w, h;
    if (size == 0) { uint32_t wh = c[k++]; w = wh & 0x3FF; h = (wh >> 16) & 0x1FF; }
    else if (size == 1) w = h = 1;
    else if (size == 2) w = h = 8;
    else w = h = 16;
    int r = c[0] & 255, g = (c[0] >> 8) & 255, b = (c[0] >> 16) & 255;
    if (st->tex_disable) textured = 0;
    int j0 = st->da_y1 - y > 0 ? st->da_y1 - y : 0, j1 = st->da_y2 - y + 1 < h ? st->da_y2 - y + 1 : h;
    int i0 = st->da_x1 - x > 0 ? st->da_x1 - x : 0, i1 = st->da_x2 - x + 1 < w ? st->da_x2 - x + 1 : w;
    if (i0 >= i1 || j0 >= j1) return;
    const uint16_t ms = st->mask_set ? 0x8000 : 0;
    const int sm = st->semi_mode;
    if (!textured && !st->mask_check && watch_on <= 0) {
        // flat rectangle (screen clears, menu shading): straight row fills, vectorisable
        const uint16_t col = shade(r, g, b, 0, 0, 0);
        const int sx0 = (x + i0) << SSHIFT, sx1 = (x + i1) << SSHIFT;
        for (int sy = (y + j0) << SSHIFT; sy < (y + j1) << SSHIFT; sy++) {
            if (!owned(o, sy)) continue;
            uint16_t* row = &vram[(size_t)(sy & (VH - 1)) * VW];
            if (S > 1 && !(sy & (S - 1))) {   // keep the native mirror in step (blend uses the same back pixel)
                uint16_t* nrow = &vnat[((sy >> SSHIFT) & 511) * 1024];
                for (int sx = sx0; sx < sx1; sx += S) {
                    uint16_t* p = &nrow[(sx >> SSHIFT) & 1023];
                    *p = (semi ? blend(*p, col, sm) : col) | ms;
                }
            }
            if (!semi) {
                for (int sx = sx0; sx < sx1; sx++) row[sx] = col | ms;
            } else {
                switch (sm) {
                    case 0: for (int sx = sx0; sx < sx1; sx++) row[sx] = blend(row[sx], col, 0) | ms; break;
                    case 1: for (int sx = sx0; sx < sx1; sx++) row[sx] = blend(row[sx], col, 1) | ms; break;
                    case 2: for (int sx = sx0; sx < sx1; sx++) row[sx] = blend(row[sx], col, 2) | ms; break;
                    default: for (int sx = sx0; sx < sx1; sx++) row[sx] = blend(row[sx], col, 3) | ms; break;
                }
            }
        }
        return;
    }
    // build each native row once (colour + flags), then replicate it into the S scaled rows
    uint16_t rc[1024];
    uint8_t rf[1024];   // 0 transparent, 1 opaque, 2 semi-transparent
    for (int j = j0; j < j1; j++) {
        int py = y + j;
        if (!owned(o, py << SSHIFT)) continue;
        int any_semi = 0;
        for (int i = i0; i < i1; i++) {
            if (textured) {
                int u = st->rect_flip_x ? u0 - i : u0 + i, v = st->rect_flip_y ? v0 - j : v0 + j;
                uint16_t t = texel(st, u, v, st->tp_x, st->tp_y, st->tp_depth, clut_x, clut_y);
                if (!t) { rf[i] = 0; continue; }
                rc[i] = raw ? t : modulate(t, r, g, b, 0, 0, 0);
                rf[i] = semi && (t & 0x8000) ? 2 : 1;
            } else {
                rc[i] = shade(r, g, b, 0, 0, 0);
                rf[i] = semi ? 2 : 1;
            }
            any_semi |= rf[i] == 2;
        }
        if (watch_on > 0)
            for (int i = i0; i < i1; i++) if (rf[i]) WATCH((x + i) & 1023, py & 511);
        for (int sj = 0; sj < S; sj++) {
            uint16_t* row = &vram[(size_t)(((py << SSHIFT) + sj) & (VH - 1)) * VW];
            for (int i = i0; i < i1; i++) {
                if (!rf[i]) continue;
                int bx = (x + i) << SSHIFT;
                for (int si = 0; si < S; si++) {
                    uint16_t* p = &row[(bx + si) & (VW - 1)];
                    if (st->mask_check && (*p & 0x8000)) continue;
                    uint16_t cc = rc[i];
                    if (any_semi && rf[i] == 2) cc = blend(*p, cc, sm);
                    *p = cc | ms;
                    if (S > 1 && sj == 0 && si == 0) VN(x + i, py) = *p;
                }
            }
        }
    }
}

// GP0(02h) fill: ignores mask, draw area and offset; wraps around VRAM
static void fill(const Own* o, int x, int y, int w, int h, uint16_t col) {
    for (int j = 0; j < h; j++) {
        int ny = (y + j) & 511;
        if (!owned(o, ny << SSHIFT)) continue;
        for (int sj = 0; sj < S; sj++) {
            uint16_t* row = &vram[(size_t)((ny << SSHIFT) + sj) * VW];
            for (int i = 0; i < (w << SSHIFT); i++) row[((x << SSHIFT) + i) & (VW - 1)] = col;
        }
        if (S > 1)
            for (int i = 0; i < w; i++) VN(x + i, ny) = col;
    }
}

// ------------------------------------------------------------------ job queue
enum { J_TRI, J_RECT, J_LINE, J_FILL };
typedef struct {
    int type;
    DS st;
    union {
        struct { V a, b, c; Prim p; } tri;
        uint32_t rc[4];
        struct { int x0, y0, x1, y1, gouraud, semi; uint32_t c0, c1; } line;
        struct { int x, y, w, h; uint16_t col; } fill;
    } u;
} Job;

static void run_job(const Job* j, const Own* o) {
    switch (j->type) {
        case J_TRI: triangle(&j->st, o, j->u.tri.a, j->u.tri.b, j->u.tri.c, &j->u.tri.p); break;
        case J_RECT: rect(&j->st, o, j->u.rc); break;
        case J_LINE:
            line_seg(&j->st, o, j->u.line.x0, j->u.line.y0, j->u.line.c0, j->u.line.x1, j->u.line.y1, j->u.line.c1,
                     j->u.line.gouraud, j->u.line.semi);
            break;
        default: fill(o, j->u.fill.x, j->u.fill.y, j->u.fill.w, j->u.fill.h, j->u.fill.col); break;
    }
}

#define QCAP 4096
#define MAX_THREADS 16
static Job queue[QCAP];
static atomic_uint q_head;
static atomic_uint q_tail[MAX_THREADS];
static atomic_int q_sleepers;
static pthread_mutex_t q_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t q_cv = PTHREAD_COND_INITIALIZER;
static int nthreads;                          // 0 = draw inline on the calling thread
int g_gpu_skip_draw;

// Native-coordinate boxes (inclusive; empty when x0 > x1) of what a job writes and samples.
typedef struct { int x0, y0, x1, y1; } Box;
static const Box NOBOX = {1, 0, 0, 0};
static inline int box_empty(Box b) { return b.x0 > b.x1; }
static inline int box_ov(Box a, Box b) {
    return !box_empty(a) && !box_empty(b) && a.x0 <= b.x1 && b.x0 <= a.x1 && a.y0 <= b.y1 && b.y0 <= a.y1;
}
// What queued jobs write / sample since the last sync, as a grid of 16x16-pixel cells (one 64-bit mask per
// 16 lines). A bounding box is far too coarse: texture pages and CLUTs are spread all over VRAM.
typedef struct { uint64_t row[32]; int any; } Grid;
static Grid pend_w, pend_r;
static inline uint64_t cell_mask(Box b) {
    int c0 = b.x0 >> 4, c1 = b.x1 >> 4;
    return (c1 >= 63 ? ~0ull : (2ull << c1) - 1) & ~((1ull << c0) - 1);
}
static inline void grid_add(Grid* g, Box b) {
    if (box_empty(b)) return;
    uint64_t m = cell_mask(b);
    for (int r = b.y0 >> 4; r <= b.y1 >> 4; r++) g->row[r] |= m;
    g->any = 1;
}
static inline int grid_ov(const Grid* g, Box b) {
    if (!g->any || box_empty(b)) return 0;
    uint64_t m = cell_mask(b);
    for (int r = b.y0 >> 4; r <= b.y1 >> 4; r++) if (g->row[r] & m) return 1;
    return 0;
}
static inline void grid_clear(Grid* g) { if (g->any) memset(g, 0, sizeof *g); }

static inline void cpu_relax(void) {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#endif
}

static void* worker(void* arg) {
    const Own o = {(int)(intptr_t)arg, nthreads};
    unsigned t = 0;
    for (;;) {
        if (atomic_load_explicit(&q_head, memory_order_acquire) != t) {
            run_job(&queue[t % QCAP], &o);
            atomic_store_explicit(&q_tail[o.tid], ++t, memory_order_release);
            continue;
        }
        for (int spin = 0; spin < 300 && atomic_load_explicit(&q_head, memory_order_acquire) == t; spin++) cpu_relax();
        if (atomic_load(&q_head) != t) continue;
        pthread_mutex_lock(&q_mu);
        atomic_fetch_add(&q_sleepers, 1);
        while (atomic_load(&q_head) == t) pthread_cond_wait(&q_cv, &q_mu);
        atomic_fetch_sub(&q_sleepers, 1);
        pthread_mutex_unlock(&q_mu);
    }
    return NULL;
}

static void start_threads(void) {
    static int started;
    if (started) return;
    started = 1;
    if (watch_on < 0) watch_init();
    const char* env = getenv("AC2_THREADS");
    long n;
    if (env) n = atol(env);
    else if (S == 1) n = 0;   // 1x is far from the limit: keep the simplest path
    else { n = sysconf(_SC_NPROCESSORS_ONLN) - 1; if (n < 1) n = 1; }   // one core stays with the game CPU
    if (n > MAX_THREADS) n = MAX_THREADS;
    if (watch_on > 0) n = 0;
    for (int i = 0; i < n; i++) {
        pthread_t th;
        nthreads = (int)n;   // set before the first worker reads it
        if (pthread_create(&th, NULL, worker, (void*)(intptr_t)i) != 0) { LOGE("gpu: cannot start worker %d", i); nthreads = i; break; }
        pthread_detach(th);
    }
    if (nthreads) LOGI("gpu: %d render threads", nthreads);
}

void gpu_sync_stat(void) {
    LOGI("gpu syncs: raw %ld war %ld self %ld copy %ld xfer %ld vram %ld", sync_why[0], sync_why[1], sync_why[2], sync_why[3], sync_why[4], sync_why[5]);
    memset(sync_why, 0, sizeof sync_why);
}
void gpu_sync(void) {
    if (!nthreads) return;
    unsigned h = atomic_load(&q_head);
    for (int i = 0; i < nthreads; i++)
        for (int spin = 0; atomic_load_explicit(&q_tail[i], memory_order_acquire) != h; spin++) {
            if (spin < 200) { cpu_relax(); continue; }
            struct timespec ts = {0, 20000};   // don't steal the core (or its hyper-thread sibling) from the workers
            nanosleep(&ts, NULL);
        }
    grid_clear(&pend_w);
    grid_clear(&pend_r);
}

// Texture page and CLUT areas a textured primitive samples.
static void tex_boxes(int tpx, int tpy, int depth, int clut_x, int clut_y, Box* t, Box* c) {
    int x1 = tpx + (depth == 0 ? 64 : depth == 1 ? 128 : 256) - 1;
    *t = x1 > 1023 ? (Box){0, tpy, 1023, tpy + 255} : (Box){tpx, tpy, x1, tpy + 255};
    *c = NOBOX;
    if (depth < 2) {
        int cx1 = clut_x + (depth == 0 ? 16 : 256) - 1;
        *c = cx1 > 1023 ? (Box){0, clut_y, 1023, clut_y} : (Box){clut_x, clut_y, cx1, clut_y};
    }
}

// Queues a job that writes inside w and samples r1/r2. Waits for the workers first when the job would read pixels
// still being drawn or overwrite pixels a queued job has yet to sample; a job that samples its own target area
// runs whole on this thread (its result depends on pixel order).
static void submit(Job* j, Box w, Box r1, Box r2) {
    static const Own all = {0, 1};
    if (g_gpu_skip_draw) return;   // fast-forwarding a debug state: the picture is not shown
    if (!nthreads) { run_job(j, &all); return; }
    if (grid_ov(&pend_w, r1) || grid_ov(&pend_w, r2)) { sync_why[0]++; gpu_sync(); }
    if (grid_ov(&pend_r, w)) { sync_why[1]++; gpu_sync(); }
    if (box_ov(w, r1) || box_ov(w, r2)) { sync_why[2]++; gpu_sync(); run_job(j, &all); return; }
    grid_add(&pend_w, w);
    grid_add(&pend_r, r1);
    grid_add(&pend_r, r2);
    unsigned h = atomic_load_explicit(&q_head, memory_order_relaxed);
    for (int i = 0; i < nthreads; i++)
        while (h - atomic_load_explicit(&q_tail[i], memory_order_acquire) >= QCAP) sched_yield();
    queue[h % QCAP] = *j;
    atomic_store(&q_head, h + 1);
    if (atomic_load(&q_sleepers) > 0) {
        pthread_mutex_lock(&q_mu);
        pthread_cond_broadcast(&q_cv);
        pthread_mutex_unlock(&q_mu);
    }
}

// drawing jobs only write inside the drawing area
static inline Box draw_box(const DS* st) { return (Box){st->da_x1, st->da_y1, st->da_x2, st->da_y2}; }

// ------------------------------------------------------------------ commands
static void set_drawmode(uint32_t v) {
    tp_x = (v & 15) * 64;
    tp_y = ((v >> 4) & 1) * 256;
    semi_mode = (v >> 5) & 3;
    tp_depth = (v >> 7) & 3;
    if (tp_depth == 3) tp_depth = 2;
    dither = (v >> 9) & 1;
    draw_to_disp = (v >> 10) & 1;
    tex_disable = allow_tex_disable ? (v >> 11) & 1 : 0;
    rect_flip_x = (v >> 12) & 1;
    rect_flip_y = (v >> 13) & 1;
}

static void polygon(void) {
    uint32_t op = cmd[0] >> 24;
    int quad = (op & 8) != 0, gouraud = (op & 16) != 0, textured = (op & 4) != 0;
    int n = quad ? 4 : 3;
    Prim p = {textured, gouraud, (op & 2) != 0, (op & 1) != 0, tp_x, tp_y, tp_depth, 0, 0};
    V v[4];
    int k = 0;
    uint32_t color = cmd[k++] & 0xFFFFFF;
    for (int i = 0; i < n; i++) {
        if (gouraud && i > 0) color = cmd[k++] & 0xFFFFFF;
        uint32_t xy = cmd[k++];
        v[i].x = (sext11(xy) + off_x) << SSHIFT;
        v[i].y = (sext11(xy >> 16) + off_y) << SSHIFT;
        v[i].r = color & 255; v[i].g = (color >> 8) & 255; v[i].b = (color >> 16) & 255;
        v[i].u = v[i].v = 0;
        if (textured) {
            uint32_t uv = cmd[k++];
            v[i].u = uv & 255;
            v[i].v = (uv >> 8) & 255;
            if (i == 0) { p.clut_x = ((uv >> 16) & 0x3F) * 16; p.clut_y = (uv >> 22) & 511; }
            if (i == 1) {
                uint32_t tp = uv >> 16;
                p.tpx = (tp & 15) * 64; p.tpy = ((tp >> 4) & 1) * 256;
                p.depth = (tp >> 7) & 3; if (p.depth == 3) p.depth = 2;
                // polygons also update the global draw mode texture page bits
                tp_x = p.tpx; tp_y = p.tpy; tp_depth = p.depth; semi_mode = (tp >> 5) & 3;
            }
        }
    }
    if (!gouraud) for (int i = 1; i < n; i++) { v[i].r = v[0].r; v[i].g = v[0].g; v[i].b = v[0].b; }
    if (textured && tex_disable) p.textured = 0;
    Box rt = NOBOX, rc = NOBOX;
    if (p.textured) tex_boxes(p.tpx, p.tpy, p.depth, p.clut_x, p.clut_y, &rt, &rc);
    Job j;
    j.type = J_TRI;
    j.st = snapshot();
    j.u.tri.a = v[0]; j.u.tri.b = v[1]; j.u.tri.c = v[2]; j.u.tri.p = p;
    submit(&j, draw_box(&j.st), rt, rc);
    if (quad) {
        j.u.tri.a = v[1]; j.u.tri.b = v[2]; j.u.tri.c = v[3];
        submit(&j, draw_box(&j.st), rt, rc);
    }
}

static void line_job(int x0, int y0, uint32_t c0, int x1, int y1, uint32_t c1, int gouraud, int semi) {
    Job j;
    j.type = J_LINE;
    j.st = snapshot();
    j.u.line.x0 = x0; j.u.line.y0 = y0; j.u.line.x1 = x1; j.u.line.y1 = y1;
    j.u.line.c0 = c0; j.u.line.c1 = c1; j.u.line.gouraud = gouraud; j.u.line.semi = semi;
    submit(&j, draw_box(&j.st), NOBOX, NOBOX);
}

static void rect_job(void) {
    uint32_t op = cmd[0] >> 24;
    Box rt = NOBOX, rc = NOBOX;
    if ((op & 4) && !tex_disable) {
        uint32_t uv = cmd[2];
        tex_boxes(tp_x, tp_y, tp_depth, ((uv >> 16) & 0x3F) * 16, (uv >> 22) & 511, &rt, &rc);
    }
    Job j;
    j.type = J_RECT;
    j.st = snapshot();
    memcpy(j.u.rc, cmd, sizeof j.u.rc);
    submit(&j, draw_box(&j.st), rt, rc);
}

static int cmd_length(uint32_t w) {
    uint32_t op = w >> 24;
    if (op >= 0x20 && op < 0x40) {
        int n = (op & 8) ? 4 : 3, per = 1 + ((op & 4) ? 1 : 0) + ((op & 16) ? 1 : 0);
        return 1 + n * per - ((op & 16) ? 1 : 0);
    }
    if (op >= 0x40 && op < 0x60) return (op & 16) ? 4 : 3;     // single line (polylines handled apart)
    if (op >= 0x60 && op < 0x80) return 2 + ((op & 4) ? 1 : 0) + (((op >> 3) & 3) == 0 ? 1 : 0);
    if (op == 0x02) return 3;
    if (op >= 0x80 && op < 0xA0) return 4;
    if (op >= 0xA0 && op < 0xE0) return 3;
    return 1;
}

static void execute(void) {
    uint32_t op = cmd[0] >> 24;
    if (watch_on < 0) watch_init();
    watch_logged = 0;
    watch_what = "draw";
    if (dbg_env("AC2_GPULOG") && (op == 0x02 || (op >= 0x80 && op < 0xE0))) {
        static int n;
        if (n++ < 200000) LOGI("gp0 %08x %08x %08x %08x  (area %d,%d-%d,%d off %d,%d)", cmd[0], cmd[1], cmd[2], cmd[3],
                            da_x1, da_y1, da_x2, da_y2, off_x, off_y);
    }
    if (op >= 0x20 && op < 0x40) { polygon(); return; }
    if (op >= 0x40 && op < 0x60) {
        int g = (op & 16) != 0;
        uint32_t c0 = cmd[0] & 0xFFFFFF, c1 = g ? cmd[2] & 0xFFFFFF : c0;
        uint32_t p0 = cmd[1], p1 = g ? cmd[3] : cmd[2];
        line_job(sext11(p0) + off_x, sext11(p0 >> 16) + off_y, c0, sext11(p1) + off_x, sext11(p1 >> 16) + off_y, c1, g, (op & 2) != 0);
        return;
    }
    if (op >= 0x60 && op < 0x80) { rect_job(); return; }
    // the hardware decodes transfers by the top 3 bits only (0x80-0x9F, 0xA0-0xBF, 0xC0-0xDF)
    if (op >= 0x80 && op < 0xA0) op = 0x80;
    else if (op >= 0xA0 && op < 0xC0) op = 0xA0;
    else if (op >= 0xC0 && op < 0xE0) op = 0xC0;
    switch (op) {
        case 0x02: {   // fill rectangle (ignores mask, draw area, offset)
            uint32_t c = cmd[0];
            uint16_t col = (uint16_t)(((c & 255) >> 3) | (((c >> 8) & 255) >> 3 << 5) | (((c >> 16) & 255) >> 3 << 10));
            int x = cmd[1] & 0x3F0, y = (cmd[1] >> 16) & 0x1FF;
            int w = ((cmd[2] & 0x3FF) + 15) & ~15, h = (cmd[2] >> 16) & 0x1FF;
            if (w == 0 || h == 0) return;
            watch_what = "fill";
            if (watch_on > 0)
                for (int j = 0; j < h; j++)
                    for (int i = 0; i < w; i++) WATCH((x + i) & 1023, (y + j) & 511);
            Job j;
            j.type = J_FILL;
            j.st = snapshot();
            j.u.fill.x = x; j.u.fill.y = y; j.u.fill.w = w; j.u.fill.h = h; j.u.fill.col = col;
            int wraps = x + w > 1024 || y + h > 512;
            submit(&j, wraps ? (Box){0, 0, 1023, 511} : (Box){x, y, x + w - 1, y + h - 1}, NOBOX, NOBOX);
            return;
        }
        case 0x80: {   // VRAM -> VRAM
            sync_why[3]++; gpu_sync();
            int sx = cmd[1] & 1023, sy = (cmd[1] >> 16) & 511, dx = cmd[2] & 1023, dy = (cmd[2] >> 16) & 511;
            int w = cmd[3] & 0xFFFF, h = cmd[3] >> 16;
            if (dbg_env("AC2_AREALOG")) LOGI("vram copy (%d,%d) -> (%d,%d) %dx%d", sx, sy, dx, dy, w, h);
            w = ((w - 1) & 1023) + 1; h = ((h - 1) & 511) + 1;
            watch_what = "copy";
            for (int j = 0; j < (h << SSHIFT); j++)
                for (int i = 0; i < (w << SSHIFT); i++) {
                    uint16_t v = VR((sx << SSHIFT) + i, (sy << SSHIFT) + j);
                    uint16_t* d = &VR((dx << SSHIFT) + i, (dy << SSHIFT) + j);
                    if (mask_check && (*d & 0x8000)) continue;
                    WATCH(((dx << SSHIFT) + i) >> SSHIFT, ((dy << SSHIFT) + j) >> SSHIFT);
                    *d = v | (mask_set ? 0x8000 : 0);
                }
            if (S > 1)   // refresh the mirror of the destination from its top-left samples
                for (int j = 0; j < h; j++)
                    for (int i = 0; i < w; i++) VN(dx + i, dy + j) = VR((dx + i) << SSHIFT, (dy + j) << SSHIFT);
            return;
        }
        case 0xA0: case 0xC0: {
            sync_why[4]++; gpu_sync();   // the transfer words are handled on this thread
            if (op == 0xC0 && g_gpu_skip_draw) {   // the game reads back VRAM it may have drawn (screen effects)
                static int warned;
                if (!warned++) LOGD("state: VRAM read while skipping drawing");
            }
            xf_x = cmd[1] & 1023; xf_y = (cmd[1] >> 16) & 511;
            xf_w = ((cmd[2] & 0xFFFF) - 1) & 1023; xf_w++;
            xf_h = (((cmd[2] >> 16) & 0xFFFF) - 1) & 511; xf_h++;
            xf_i = 0; xf_n = xf_w * xf_h; xf_cx = xf_cy = 0;
            xfer_mode = op == 0xA0 ? 1 : 2;
            if (xf_n > 1024) LOGD("vram %s %dx%d at (%d,%d)", op == 0xA0 ? "upload" : "download", xf_w, xf_h, xf_x, xf_y);
            return;
        }
        case 0xE1: set_drawmode(cmd[0]); return;
        case 0xE2:
            tw_mask_x = cmd[0] & 31; tw_mask_y = (cmd[0] >> 5) & 31;
            tw_off_x = (cmd[0] >> 10) & 31; tw_off_y = (cmd[0] >> 15) & 31;
            return;
        case 0xE3: da_x1 = cmd[0] & 1023; da_y1 = (cmd[0] >> 10) & 511; return;
        case 0xE4: da_x2 = cmd[0] & 1023; da_y2 = (cmd[0] >> 10) & 511; return;
        case 0xE5:
            off_x = sext11(cmd[0]); off_y = sext11(cmd[0] >> 11);
            if (dbg_env("AC2_AREALOG")) LOGI("E5 offset %d,%d (area %d,%d-%d,%d)", off_x, off_y, da_x1, da_y1, da_x2, da_y2);
            return;
        case 0xE6: mask_set = cmd[0] & 1; mask_check = (cmd[0] >> 1) & 1; return;
        case 0x1F: gpu_irq = 1; irq_raise(IRQ_GPU); return;
        default: return;   // nop / cache clear / unknown
    }
}

void gpu_write_gp0(uint32_t w) {
    if (xfer_mode == 1) {
        for (int k = 0; k < 2 && xf_i < xf_n; k++, xf_i++) {
            uint16_t px = (uint16_t)(w >> (16 * k));
            int x = xf_x + xf_cx, y = xf_y + xf_cy;
            if (++xf_cx == xf_w) { xf_cx = 0; xf_cy++; }
            uint16_t* d = &VN(x, y);
            if (mask_check && (*d & 0x8000)) continue;
            if (watch_on > 0 && (x & 1023) >= wx0 && (x & 1023) <= wx1 && (y & 511) >= wy0 && (y & 511) <= wy1 && xf_i == 0)
                LOGI("WATCH upload %dx%d at (%d,%d)", xf_w, xf_h, xf_x, xf_y);
            native_block(x & 1023, y & 511, px | (mask_set ? 0x8000 : 0));
        }
        if (xf_i >= xf_n) xfer_mode = 0;
        return;
    }
    if (poly_line) {
        // polyline state machine: 2 = expecting the first vertex, 1 = expecting [colour,] next vertex
        if (poly_line == 2) {
            pl_xy = w;
            poly_line = 1;
            pl_expect_color = poly_gouraud;
            return;
        }
        if ((w & 0xF000F000) == 0x50005000) { poly_line = 0; return; }   // terminator
        if (pl_expect_color) { pl_next_color = w & 0xFFFFFF; pl_expect_color = 0; return; }
        uint32_t col = poly_gouraud ? pl_next_color : pl_color;
        line_job(sext11(pl_xy) + off_x, sext11(pl_xy >> 16) + off_y, pl_color, sext11(w) + off_x, sext11(w >> 16) + off_y,
                 col, poly_gouraud, poly_semi);
        pl_color = col;
        pl_xy = w;
        pl_expect_color = poly_gouraud;
        return;
    }
    if (ncmd == 0) {
        uint32_t op = w >> 24;
        if (op >= 0x48 && op < 0x60 && (op & 8)) {
            // polyline start: colour (+opcode), then vertices (gouraud: colour/vertex pairs) until 0x5xxx5xxx
            poly_line = 2;
            poly_gouraud = (op & 16) != 0;
            poly_semi = (op & 2) != 0;
            pl_color = w & 0xFFFFFF;
            return;
        }
        need = cmd_length(w);
    }
    cmd[ncmd++] = w;
    if (ncmd >= need) {
        execute();
        ncmd = 0;
    }
}

uint32_t gpu_read(void) {
    if (xfer_mode == 2) {
        uint32_t w = 0;
        for (int k = 0; k < 2 && xf_i < xf_n; k++, xf_i++) {
            int x = xf_x + xf_i % xf_w, y = xf_y + xf_i / xf_w;
            w |= (uint32_t)VN(x, y) << (16 * k);
        }
        if (xf_i >= xf_n) xfer_mode = 0;
        return w;
    }
    return 0;
}

void gpu_write_gp1(uint32_t v) {
    switch (v >> 24) {
        case 0x00:
            set_drawmode(0); disp_enabled = 0; dma_dir = 0; dmode = 0; ncmd = 0; xfer_mode = 0; poly_line = 0;
            da_x1 = da_y1 = 0; da_x2 = da_y2 = 0; off_x = off_y = 0; mask_set = mask_check = 0;
            break;
        case 0x01: ncmd = 0; xfer_mode = 0; poly_line = 0; break;
        case 0x02: gpu_irq = 0; break;
        case 0x03:
            disp_enabled = !(v & 1);
            if (dbg_env("AC2_GP1LOG")) LOGI("GP1 display %s at vblank %llu", disp_enabled ? "on" : "off", (unsigned long long)core_vblank_count());
            break;
        case 0x04: dma_dir = v & 3; break;
        case 0x05:
            disp_x = v & 0x3FE; disp_y = (v >> 10) & 0x1FF;
            if (dbg_env("AC2_GP1LOG")) LOGI("GP1 display area %d,%d at vblank %llu", disp_x, disp_y, (unsigned long long)core_vblank_count());
            break;
        case 0x06: h_range1 = v & 0xFFF; h_range2 = (v >> 12) & 0xFFF; break;
        case 0x07: v_range1 = v & 0x3FF; v_range2 = (v >> 10) & 0x3FF; break;
        case 0x08: dmode = v & 0xFF; break;
        case 0x09: allow_tex_disable = v & 1; break;
        default: break;   // GP1(10h) info queries etc.
    }
}

uint32_t gpu_stat(void) {
    uint32_t s = (tp_x / 64) | ((tp_y / 256) << 4) | (semi_mode << 5) | (tp_depth << 7) | (dither << 9) |
                 (draw_to_disp << 10) | (mask_set << 11) | (mask_check << 12) | (tex_disable << 15);
    s |= (uint32_t)((dmode >> 6) & 1) << 16;
    s |= (uint32_t)(dmode & 3) << 17;
    s |= (uint32_t)((dmode >> 2) & 1) << 19;
    s |= (uint32_t)((dmode >> 3) & 1) << 20;
    s |= (uint32_t)((dmode >> 4) & 1) << 21;
    s |= (uint32_t)((dmode >> 5) & 1) << 22;
    s |= (uint32_t)(!disp_enabled) << 23;
    s |= (uint32_t)gpu_irq << 24;
    s |= (1u << 26) | (1u << 27) | (1u << 28);
    if (dma_dir == 1 || dma_dir == 2) s |= 1u << 25;
    else if (dma_dir == 3) s |= 1u << 25;
    s |= (uint32_t)dma_dir << 29;
    s |= (uint32_t)field << 31;
    s |= (uint32_t)field << 13;
    return s;
}

void gpu_vblank(void) { field ^= 1; static int n; if (dbg_env("AC2_SYNCSTAT") && ++n % 600 == 0) gpu_sync_stat(); }

DisplayInfo gpu_display(void) {
    static const int widths[4] = {256, 320, 512, 640};
    DisplayInfo d;
    d.x = disp_x;
    d.y = disp_y;
    d.w = (dmode & 0x40) ? 368 : widths[dmode & 3];
    int lines = v_range2 - v_range1;
    if (lines <= 0 || lines > 288) lines = 240;
    d.interlaced = (dmode & 0x24) == 0x24;
    d.h = d.interlaced ? lines * 2 : lines;
    d.rgb24 = (dmode >> 4) & 1;
    d.enabled = disp_enabled;
    return d;
}

// Called once at startup, before anything is drawn.
void gpu_set_scale(int s) {
    gpu_sync();
    S = s == 4 ? 4 : s == 2 ? 2 : 1;
    SSHIFT = S == 4 ? 2 : S == 2 ? 1 : 0;
    VW = 1024 << SSHIFT;
    VH = 512 << SSHIFT;
    if (vnat != vram) free(vnat);
    free(vram);
    vram = calloc((size_t)VW * VH, sizeof(uint16_t));
    vnat = S > 1 ? calloc(1024 * 512, sizeof(uint16_t)) : vram;
    start_threads();
}

void gpu_init(void) {
    if (!vram) { gpu_set_scale(1); return; }
    gpu_sync();
    memset(vram, 0, (size_t)VW * VH * sizeof(uint16_t));
    if (vnat != vram) memset(vnat, 0, 1024 * 512 * sizeof(uint16_t));
}
