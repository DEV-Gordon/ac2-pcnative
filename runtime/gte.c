// GTE (Geometry Transformation Engine, cop2): fixed-point vector/matrix unit, bit-exact.
#include "hw.h"

#define D(c, i) ((c)->gte_data[i])
#define C(c, i) ((c)->gte_ctrl[i])

static inline int32_t lo16(uint32_t v) { return (int16_t)(v & 0xFFFF); }
static inline int32_t hi16(uint32_t v) { return (int16_t)(v >> 16); }

// FLAG bits
enum {
    F_MAC1P = 1 << 30, F_MAC2P = 1 << 29, F_MAC3P = 1 << 28,
    F_MAC1N = 1 << 27, F_MAC2N = 1 << 26, F_MAC3N = 1 << 25,
    F_IR1 = 1 << 24, F_IR2 = 1 << 23, F_IR3 = 1 << 22,
    F_R = 1 << 21, F_G = 1 << 20, F_B = 1 << 19,
    F_SZ = 1 << 18, F_DIV = 1 << 17, F_MAC0P = 1 << 16, F_MAC0N = 1 << 15,
    F_SX = 1 << 14, F_SY = 1 << 13, F_IR0 = 1 << 12,
};
static uint32_t flag;
int g_gte_widescreen;   // 16:9 hack: projection X scaled by 3/4 (wider horizontal field of view)

// register views
#define IR0 (lo16(D(c, 8)))
#define IR1 (lo16(D(c, 9)))
#define IR2 (lo16(D(c, 10)))
#define IR3 (lo16(D(c, 11)))
static inline void set_ir(CPU* c, int i, int32_t v) { D(c, 8 + i) = (uint32_t)(int32_t)(int16_t)v; }
static inline int32_t MAC(CPU* c, int i) { return (int32_t)D(c, 24 + i); }

// ------------------------------------------------------------------ saturation helpers
static inline int64_t mac_check(int i, int64_t v) {
    if (v > 0x7FFFFFFFFFFLL) flag |= (uint32_t)F_MAC1P >> (i - 1);
    else if (v < -0x80000000000LL) flag |= (uint32_t)F_MAC1N >> (i - 1);
    return (v << 20) >> 20;     // wrap to 44 bits
}
static inline void mac0_check(int64_t v) {
    if (v > 0x7FFFFFFFLL) flag |= F_MAC0P;
    else if (v < -0x80000000LL) flag |= F_MAC0N;
}
static inline void set_mac(CPU* c, int i, int64_t v, int shift) {
    v = mac_check(i, v);
    D(c, 24 + i) = (uint32_t)(int32_t)(v >> shift);
}
static inline void set_ir_sat(CPU* c, int i, int32_t v, int lm) {
    int32_t lo = lm ? 0 : -0x8000;
    if (v < lo) { v = lo; flag |= (uint32_t)F_IR1 >> (i - 1); }
    else if (v > 0x7FFF) { v = 0x7FFF; flag |= (uint32_t)F_IR1 >> (i - 1); }
    set_ir(c, i, v);
}
static inline void set_mac_ir(CPU* c, int i, int64_t v, int shift, int lm) {
    set_mac(c, i, v, shift);
    set_ir_sat(c, i, MAC(c, i), lm);
}

static inline void push_sz(CPU* c, int32_t z) {
    if (z < 0) { z = 0; flag |= F_SZ; }
    else if (z > 0xFFFF) { z = 0xFFFF; flag |= F_SZ; }
    D(c, 16) = D(c, 17); D(c, 17) = D(c, 18); D(c, 18) = D(c, 19);
    D(c, 19) = (uint32_t)z;
}
static inline void push_sxy(CPU* c, int32_t x, int32_t y) {
    if (x < -0x400) { x = -0x400; flag |= F_SX; } else if (x > 0x3FF) { x = 0x3FF; flag |= F_SX; }
    if (y < -0x400) { y = -0x400; flag |= F_SY; } else if (y > 0x3FF) { y = 0x3FF; flag |= F_SY; }
    D(c, 12) = D(c, 13); D(c, 13) = D(c, 14);
    D(c, 14) = ((uint32_t)x & 0xFFFF) | ((uint32_t)y << 16);
}
static inline uint32_t col_sat(int32_t v, uint32_t f) {
    if (v < 0) { flag |= f; return 0; }
    if (v > 255) { flag |= f; return 255; }
    return (uint32_t)v;
}
static inline void push_rgb_from_mac(CPU* c) {
    uint32_t r = col_sat(MAC(c, 1) >> 4, F_R), g = col_sat(MAC(c, 2) >> 4, F_G), b = col_sat(MAC(c, 3) >> 4, F_B);
    D(c, 20) = D(c, 21); D(c, 21) = D(c, 22);
    D(c, 22) = r | (g << 8) | (b << 16) | (D(c, 6) & 0xFF000000);
}

// ------------------------------------------------------------------ division (UNR)
static uint8_t unr_table[0x101];
static int unr_ready;
static void unr_init(void) {
    for (int i = 0; i < 0x101; i++) {
        int v = (0x40000 / (i + 0x100) + 1) / 2 - 0x101;
        unr_table[i] = (uint8_t)(v < 0 ? 0 : v);
    }
    unr_ready = 1;
}
static uint32_t unr_divide(uint32_t h, uint32_t sz3) {
    if (!unr_ready) unr_init();
    if (h >= sz3 * 2) { flag |= F_DIV; return 0x1FFFF; }
    int shift = __builtin_clz(sz3 & 0xFFFF) - 16;
    uint32_t n = h << shift, d = (sz3 << shift) & 0xFFFF;
    uint32_t u = unr_table[((d & 0x7FFF) + 0x40) >> 7] + 0x101;
    d = (0x2000080 - (d * u)) >> 8;
    d = (0x0000080 + (d * u)) >> 8;
    uint64_t r = ((uint64_t)n * d + 0x8000) >> 16;
    return r > 0x1FFFF ? 0x1FFFF : (uint32_t)r;
}

// ------------------------------------------------------------------ matrices
typedef struct { int32_t m[3][3]; } Mat;
static Mat mat_rt(CPU* c) {
    Mat r = {{{lo16(C(c, 0)), hi16(C(c, 0)), lo16(C(c, 1))},
              {hi16(C(c, 1)), lo16(C(c, 2)), hi16(C(c, 2))},
              {lo16(C(c, 3)), hi16(C(c, 3)), lo16(C(c, 4))}}};
    return r;
}
static Mat mat_llm(CPU* c) {
    Mat r = {{{lo16(C(c, 8)), hi16(C(c, 8)), lo16(C(c, 9))},
              {hi16(C(c, 9)), lo16(C(c, 10)), hi16(C(c, 10))},
              {lo16(C(c, 11)), hi16(C(c, 11)), lo16(C(c, 12))}}};
    return r;
}
static Mat mat_lcm(CPU* c) {
    Mat r = {{{lo16(C(c, 16)), hi16(C(c, 16)), lo16(C(c, 17))},
              {hi16(C(c, 17)), lo16(C(c, 18)), hi16(C(c, 18))},
              {lo16(C(c, 19)), hi16(C(c, 19)), lo16(C(c, 20))}}};
    return r;
}

static void vec_of(CPU* c, int v, int32_t out[3]) {
    if (v == 3) { out[0] = IR1; out[1] = IR2; out[2] = IR3; return; }
    out[0] = lo16(D(c, v * 2));
    out[1] = hi16(D(c, v * 2));
    out[2] = lo16(D(c, v * 2 + 1));
}

// MAC/IR = T*0x1000 + M*V
static void mul_mat_vec(CPU* c, const Mat* m, const int32_t v[3], const int32_t t[3], int shift, int lm) {
    for (int i = 0; i < 3; i++) {
        int64_t acc = (int64_t)t[i] << 12;
        acc = mac_check(i + 1, acc + (int64_t)m->m[i][0] * v[0]);
        acc = mac_check(i + 1, acc + (int64_t)m->m[i][1] * v[1]);
        acc = acc + (int64_t)m->m[i][2] * v[2];
        set_mac_ir(c, i + 1, acc, shift, lm);
    }
}

// ------------------------------------------------------------------ commands
static void rtps(CPU* c, int v, int shift, int lm, int last) {
    Mat m = mat_rt(c);
    int32_t vv[3];
    vec_of(c, v, vv);
    int32_t tr[3] = {(int32_t)C(c, 5), (int32_t)C(c, 6), (int32_t)C(c, 7)};
    int64_t z = 0;
    for (int i = 0; i < 3; i++) {
        int64_t acc = (int64_t)tr[i] << 12;
        acc = mac_check(i + 1, acc + (int64_t)m.m[i][0] * vv[0]);
        acc = mac_check(i + 1, acc + (int64_t)m.m[i][1] * vv[1]);
        acc = mac_check(i + 1, acc + (int64_t)m.m[i][2] * vv[2]);
        D(c, 25 + i) = (uint32_t)(int32_t)(acc >> shift);
        if (i == 2) z = acc;
    }
    set_ir_sat(c, 1, MAC(c, 1), lm);
    set_ir_sat(c, 2, MAC(c, 2), lm);
    // IR3 quirk: saturation flag tested on (z >> 12) when sf = 0
    if (!shift) {
        int32_t t = (int32_t)(z >> 12);
        if (t < -0x8000 || t > 0x7FFF) flag |= F_IR3;
        int32_t v3 = MAC(c, 3), lo = lm ? 0 : -0x8000;
        set_ir(c, 3, v3 < lo ? lo : v3 > 0x7FFF ? 0x7FFF : v3);
    } else {
        set_ir_sat(c, 3, MAC(c, 3), lm);
    }
    push_sz(c, (int32_t)(z >> 12));
    uint32_t div = unr_divide(C(c, 26) & 0xFFFF, D(c, 19));
    int64_t sx = g_gte_widescreen ? (int64_t)div * IR1 * 3 / 4 + (int32_t)C(c, 24) : (int64_t)div * IR1 + (int32_t)C(c, 24);
    int64_t sy = (int64_t)div * IR2 + (int32_t)C(c, 25);
    mac0_check(sx);
    mac0_check(sy);
    push_sxy(c, (int32_t)(sx >> 16), (int32_t)(sy >> 16));
    if (last) {
        int64_t dq = (int64_t)div * (int16_t)C(c, 27) + (int32_t)C(c, 28);
        mac0_check(dq);
        D(c, 24) = (uint32_t)(int32_t)dq;
        int32_t ir0 = (int32_t)(dq >> 12);
        if (ir0 < 0) { ir0 = 0; flag |= F_IR0; } else if (ir0 > 0x1000) { ir0 = 0x1000; flag |= F_IR0; }
        set_ir(c, 0, ir0);
    }
}

static void interpolate(CPU* c, int64_t m1, int64_t m2, int64_t m3, int shift, int lm) {
    int32_t fc[3] = {(int32_t)C(c, 21), (int32_t)C(c, 22), (int32_t)C(c, 23)};
    set_mac_ir(c, 1, ((int64_t)fc[0] << 12) - m1, shift, 0);
    set_mac_ir(c, 2, ((int64_t)fc[1] << 12) - m2, shift, 0);
    set_mac_ir(c, 3, ((int64_t)fc[2] << 12) - m3, shift, 0);
    set_mac_ir(c, 1, (int64_t)IR1 * IR0 + m1, shift, lm);
    set_mac_ir(c, 2, (int64_t)IR2 * IR0 + m2, shift, lm);
    set_mac_ir(c, 3, (int64_t)IR3 * IR0 + m3, shift, lm);
}

static void light(CPU* c, int v, int shift, int lm) {
    Mat llm = mat_llm(c), lcm = mat_lcm(c);
    int32_t vv[3], zero[3] = {0, 0, 0};
    vec_of(c, v, vv);
    mul_mat_vec(c, &llm, vv, zero, shift, lm);
    int32_t ir[3] = {IR1, IR2, IR3}, bk[3] = {(int32_t)C(c, 13), (int32_t)C(c, 14), (int32_t)C(c, 15)};
    mul_mat_vec(c, &lcm, ir, bk, shift, lm);
}

static void color_mul(CPU* c, int shift, int lm) {
    uint32_t rgb = D(c, 6);
    set_mac_ir(c, 1, ((int64_t)(rgb & 0xFF) * IR1) << 4, shift, lm);
    set_mac_ir(c, 2, ((int64_t)((rgb >> 8) & 0xFF) * IR2) << 4, shift, lm);
    set_mac_ir(c, 3, ((int64_t)((rgb >> 16) & 0xFF) * IR3) << 4, shift, lm);
}

static void depth_cue_color(CPU* c, int shift, int lm) {
    uint32_t rgb = D(c, 6);
    interpolate(c, ((int64_t)(rgb & 0xFF) * IR1) << 4, ((int64_t)((rgb >> 8) & 0xFF) * IR2) << 4,
                ((int64_t)((rgb >> 16) & 0xFF) * IR3) << 4, shift, lm);
}

void gte_command(CPU* c, uint32_t cmd) {
    int shift = (cmd & (1 << 19)) ? 12 : 0, lm = (cmd >> 10) & 1;
    flag = 0;
    switch (cmd & 0x3F) {
        case 0x01: rtps(c, 0, shift, lm, 1); break;                              // RTPS
        case 0x30: rtps(c, 0, shift, lm, 0); rtps(c, 1, shift, lm, 0); rtps(c, 2, shift, lm, 1); break;   // RTPT
        case 0x06: {                                                              // NCLIP
            int32_t x0 = lo16(D(c, 12)), y0 = hi16(D(c, 12)), x1 = lo16(D(c, 13)), y1 = hi16(D(c, 13));
            int32_t x2 = lo16(D(c, 14)), y2 = hi16(D(c, 14));
            int64_t v = (int64_t)x0 * y1 + (int64_t)x1 * y2 + (int64_t)x2 * y0 - (int64_t)x0 * y2 - (int64_t)x1 * y0 - (int64_t)x2 * y1;
            mac0_check(v);
            D(c, 24) = (uint32_t)(int32_t)v;
            break;
        }
        case 0x0C: {                                                              // OP
            int32_t d1 = lo16(C(c, 0)), d2 = lo16(C(c, 2)), d3 = lo16(C(c, 4));
            int32_t i1 = IR1, i2 = IR2, i3 = IR3;
            set_mac_ir(c, 1, (int64_t)i3 * d2 - (int64_t)i2 * d3, shift, lm);
            set_mac_ir(c, 2, (int64_t)i1 * d3 - (int64_t)i3 * d1, shift, lm);
            set_mac_ir(c, 3, (int64_t)i2 * d1 - (int64_t)i1 * d2, shift, lm);
            break;
        }
        case 0x10: {                                                              // DPCS
            uint32_t rgb = D(c, 6);
            interpolate(c, (int64_t)(rgb & 0xFF) << 16, (int64_t)((rgb >> 8) & 0xFF) << 16, (int64_t)((rgb >> 16) & 0xFF) << 16, shift, lm);
            push_rgb_from_mac(c);
            break;
        }
        case 0x2A:                                                                // DPCT
            for (int i = 0; i < 3; i++) {
                uint32_t rgb = D(c, 20);
                interpolate(c, (int64_t)(rgb & 0xFF) << 16, (int64_t)((rgb >> 8) & 0xFF) << 16, (int64_t)((rgb >> 16) & 0xFF) << 16, shift, lm);
                push_rgb_from_mac(c);
            }
            break;
        case 0x11:                                                                // INTPL
            interpolate(c, (int64_t)IR1 << 12, (int64_t)IR2 << 12, (int64_t)IR3 << 12, shift, lm);
            push_rgb_from_mac(c);
            break;
        case 0x29: depth_cue_color(c, shift, lm); push_rgb_from_mac(c); break;   // DCPL
        case 0x12: {                                                              // MVMVA
            int mx = (cmd >> 17) & 3, v = (cmd >> 15) & 3, cv = (cmd >> 13) & 3;
            Mat m;
            if (mx == 0) m = mat_rt(c);
            else if (mx == 1) m = mat_llm(c);
            else if (mx == 2) m = mat_lcm(c);
            else {
                int32_t r = (int32_t)((D(c, 6) & 0xFF) << 4);
                Mat g = {{{-r, r, IR0}, {lo16(C(c, 1)), lo16(C(c, 1)), lo16(C(c, 1))}, {lo16(C(c, 2)), lo16(C(c, 2)), lo16(C(c, 2))}}};
                m = g;
            }
            int32_t vv[3], t[3] = {0, 0, 0};
            vec_of(c, v, vv);
            if (cv == 0) { t[0] = (int32_t)C(c, 5); t[1] = (int32_t)C(c, 6); t[2] = (int32_t)C(c, 7); }
            else if (cv == 1) { t[0] = (int32_t)C(c, 13); t[1] = (int32_t)C(c, 14); t[2] = (int32_t)C(c, 15); }
            else if (cv == 2) { t[0] = (int32_t)C(c, 21); t[1] = (int32_t)C(c, 22); t[2] = (int32_t)C(c, 23); }
            if (cv == 2) {
                // hardware bug: the far-colour vector only affects flags, result uses the last two columns
                for (int i = 0; i < 3; i++) {
                    int64_t acc = ((int64_t)t[i] << 12) + (int64_t)m.m[i][0] * vv[0];
                    set_ir_sat(c, i + 1, (int32_t)(mac_check(i + 1, acc) >> shift), 0);
                    acc = mac_check(i + 1, (int64_t)m.m[i][1] * vv[1]);
                    acc = acc + (int64_t)m.m[i][2] * vv[2];
                    set_mac_ir(c, i + 1, acc, shift, lm);
                }
            } else {
                mul_mat_vec(c, &m, vv, t, shift, lm);
            }
            break;
        }
        case 0x13: light(c, 0, shift, lm); depth_cue_color(c, shift, lm); push_rgb_from_mac(c); break;   // NCDS
        case 0x16:                                                                                       // NCDT
            for (int i = 0; i < 3; i++) { light(c, i, shift, lm); depth_cue_color(c, shift, lm); push_rgb_from_mac(c); }
            break;
        case 0x14: {                                                              // CDP
            Mat lcm = mat_lcm(c);
            int32_t ir[3] = {IR1, IR2, IR3}, bk[3] = {(int32_t)C(c, 13), (int32_t)C(c, 14), (int32_t)C(c, 15)};
            mul_mat_vec(c, &lcm, ir, bk, shift, lm);
            depth_cue_color(c, shift, lm);
            push_rgb_from_mac(c);
            break;
        }
        case 0x1B: light(c, 0, shift, lm); color_mul(c, shift, lm); push_rgb_from_mac(c); break;   // NCCS
        case 0x3F:                                                                                   // NCCT
            for (int i = 0; i < 3; i++) { light(c, i, shift, lm); color_mul(c, shift, lm); push_rgb_from_mac(c); }
            break;
        case 0x1C: {                                                              // CC
            Mat lcm = mat_lcm(c);
            int32_t ir[3] = {IR1, IR2, IR3}, bk[3] = {(int32_t)C(c, 13), (int32_t)C(c, 14), (int32_t)C(c, 15)};
            mul_mat_vec(c, &lcm, ir, bk, shift, lm);
            color_mul(c, shift, lm);
            push_rgb_from_mac(c);
            break;
        }
        case 0x1E: light(c, 0, shift, lm); push_rgb_from_mac(c); break;           // NCS
        case 0x20:                                                                // NCT
            for (int i = 0; i < 3; i++) { light(c, i, shift, lm); push_rgb_from_mac(c); }
            break;
        case 0x28:                                                                // SQR
            set_mac_ir(c, 1, (int64_t)IR1 * IR1, shift, lm);
            set_mac_ir(c, 2, (int64_t)IR2 * IR2, shift, lm);
            set_mac_ir(c, 3, (int64_t)IR3 * IR3, shift, lm);
            break;
        case 0x2D: case 0x2E: {                                                   // AVSZ3 / AVSZ4
            int64_t v;
            if ((cmd & 0x3F) == 0x2D) v = (int64_t)(int16_t)C(c, 29) * (D(c, 17) + D(c, 18) + D(c, 19));
            else v = (int64_t)(int16_t)C(c, 30) * (D(c, 16) + D(c, 17) + D(c, 18) + D(c, 19));
            mac0_check(v);
            D(c, 24) = (uint32_t)(int32_t)v;
            int32_t z = (int32_t)(v >> 12);
            if (z < 0) { z = 0; flag |= F_SZ; } else if (z > 0xFFFF) { z = 0xFFFF; flag |= F_SZ; }
            D(c, 7) = (uint32_t)z;
            break;
        }
        case 0x3D:                                                                // GPF
            set_mac_ir(c, 1, (int64_t)IR0 * IR1, shift, lm);
            set_mac_ir(c, 2, (int64_t)IR0 * IR2, shift, lm);
            set_mac_ir(c, 3, (int64_t)IR0 * IR3, shift, lm);
            push_rgb_from_mac(c);
            break;
        case 0x3E:                                                                // GPL
            set_mac_ir(c, 1, ((int64_t)MAC(c, 1) << shift) + (int64_t)IR0 * IR1, shift, lm);
            set_mac_ir(c, 2, ((int64_t)MAC(c, 2) << shift) + (int64_t)IR0 * IR2, shift, lm);
            set_mac_ir(c, 3, ((int64_t)MAC(c, 3) << shift) + (int64_t)IR0 * IR3, shift, lm);
            push_rgb_from_mac(c);
            break;
        default:
            LOGD("GTE: unknown command %08x", cmd);
    }
    if (flag & 0x7F87E000) flag |= 0x80000000u;
    C(c, 31) = flag;
}

// ------------------------------------------------------------------ register access
static inline int32_t sat5(int32_t v) { v >>= 7; return v < 0 ? 0 : v > 31 ? 31 : v; }

uint32_t gte_read_data(CPU* c, int r) {
    switch (r) {
        case 1: case 3: case 5: case 8: case 9: case 10: case 11:
            return (uint32_t)(int32_t)(int16_t)D(c, r);
        case 7: case 16: case 17: case 18: case 19:
            return D(c, r) & 0xFFFF;
        case 15: return D(c, 14);
        case 28: case 29:
            return (uint32_t)(sat5(IR1) | (sat5(IR2) << 5) | (sat5(IR3) << 10));
        case 31: {   // LZCR: count of leading bits equal to the sign bit of LZCS
            uint32_t v = D(c, 30), x = (int32_t)v < 0 ? ~v : v;
            return x ? (uint32_t)__builtin_clz(x) : 32;
        }
        default: return D(c, r);
    }
}

void gte_write_data(CPU* c, int r, uint32_t v) {
    switch (r) {
        case 15:   // SXYP: push
            D(c, 12) = D(c, 13); D(c, 13) = D(c, 14); D(c, 14) = v;
            return;
        case 28:   // IRGB
            D(c, 28) = v & 0x7FFF;
            set_ir(c, 1, (int32_t)((v & 31) << 7));
            set_ir(c, 2, (int32_t)(((v >> 5) & 31) << 7));
            set_ir(c, 3, (int32_t)(((v >> 10) & 31) << 7));
            return;
        case 29: case 31: return;   // read-only
        default: D(c, r) = v;
    }
}

uint32_t gte_read_ctrl(CPU* c, int r) {
    switch (r) {
        case 4: case 12: case 20: case 26: case 27: case 29: case 30:
            return (uint32_t)(int32_t)(int16_t)C(c, r);
        default: return C(c, r);
    }
}

void gte_write_ctrl(CPU* c, int r, uint32_t v) {
    if (r == 31) {
        v &= 0x7FFFF000;
        if (v & 0x7F87E000) v |= 0x80000000u;
        C(c, 31) = v;
        return;
    }
    C(c, r) = v;
}
