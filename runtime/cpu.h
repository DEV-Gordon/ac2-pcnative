// Execution context shared by the recompiled code and the runtime.
#pragma once
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CPU {
    uint32_t r[32];
    uint32_t hi, lo;
    uint32_t cop0[32];          // SR = 12, CAUSE = 13, EPC = 14
    // GTE (cop2)
    uint32_t gte_data[32];
    uint32_t gte_ctrl[32];
} CPU;

typedef void (*RecompFn)(CPU* c);
typedef struct RecompFunc { uint32_t addr; RecompFn fn; } RecompFunc;

extern const RecompFunc recomp_table[];
extern const unsigned recomp_table_size;

// ------------------------------------------------------------------ memory
extern uint8_t* g_ram;          // 2 MB main RAM
extern uint8_t g_scratch[1024]; // 0x1F800000

uint32_t io_read(uint32_t addr, int size);
void io_write(uint32_t addr, uint32_t value, int size);

// kuseg/kseg0/kseg1 all map the same physical space
static inline uint32_t phys(uint32_t a) { return a & 0x1FFFFFFF; }

#define RAM_FAST(p) ((p) < 0x00800000u)

static inline uint32_t mem_r32(uint32_t a) {
    uint32_t p = phys(a);
    if (RAM_FAST(p)) { uint32_t v; memcpy(&v, g_ram + (p & 0x1FFFFC), 4); return v; }
    if ((p & 0xFFFFFC00u) == 0x1F800000u) { uint32_t v; memcpy(&v, g_scratch + (p & 0x3FC), 4); return v; }
    return io_read(a & ~3u, 4);
}
static inline uint32_t mem_r16(uint32_t a) {
    uint32_t p = phys(a);
    if (RAM_FAST(p)) { uint16_t v; memcpy(&v, g_ram + (p & 0x1FFFFE), 2); return v; }
    if ((p & 0xFFFFFC00u) == 0x1F800000u) { uint16_t v; memcpy(&v, g_scratch + (p & 0x3FE), 2); return v; }
    return io_read(a & ~1u, 2) & 0xFFFF;
}
static inline uint32_t mem_r8(uint32_t a) {
    uint32_t p = phys(a);
    if (RAM_FAST(p)) return g_ram[p & 0x1FFFFF];
    if ((p & 0xFFFFFC00u) == 0x1F800000u) return g_scratch[p & 0x3FF];
    return io_read(a, 1) & 0xFF;
}
static inline void mem_w32(uint32_t a, uint32_t v) {
    uint32_t p = phys(a);
    if (RAM_FAST(p)) { memcpy(g_ram + (p & 0x1FFFFC), &v, 4); return; }
    if ((p & 0xFFFFFC00u) == 0x1F800000u) { memcpy(g_scratch + (p & 0x3FC), &v, 4); return; }
    io_write(a & ~3u, v, 4);
}
static inline void mem_w16(uint32_t a, uint32_t v) {
    uint32_t p = phys(a);
    uint16_t h = (uint16_t)v;
    if (RAM_FAST(p)) { memcpy(g_ram + (p & 0x1FFFFE), &h, 2); return; }
    if ((p & 0xFFFFFC00u) == 0x1F800000u) { memcpy(g_scratch + (p & 0x3FE), &h, 2); return; }
    io_write(a & ~1u, h, 2);
}
static inline void mem_w8(uint32_t a, uint32_t v) {
    uint32_t p = phys(a);
    if (RAM_FAST(p)) { g_ram[p & 0x1FFFFF] = (uint8_t)v; return; }
    if ((p & 0xFFFFFC00u) == 0x1F800000u) { g_scratch[p & 0x3FF] = (uint8_t)v; return; }
    io_write(a, v & 0xFF, 1);
}

// unaligned word access (little endian semantics of the R3000)
static inline uint32_t mem_lwl(uint32_t a, uint32_t rt) {
    uint32_t w = mem_r32(a & ~3u);
    switch (a & 3) {
        case 0: return (rt & 0x00FFFFFF) | (w << 24);
        case 1: return (rt & 0x0000FFFF) | (w << 16);
        case 2: return (rt & 0x000000FF) | (w << 8);
        default: return w;
    }
}
static inline uint32_t mem_lwr(uint32_t a, uint32_t rt) {
    uint32_t w = mem_r32(a & ~3u);
    switch (a & 3) {
        case 0: return w;
        case 1: return (rt & 0xFF000000) | (w >> 8);
        case 2: return (rt & 0xFFFF0000) | (w >> 16);
        default: return (rt & 0xFFFFFF00) | (w >> 24);
    }
}
static inline void mem_swl(uint32_t a, uint32_t rt) {
    uint32_t al = a & ~3u, w = mem_r32(al);
    switch (a & 3) {
        case 0: w = (w & 0xFFFFFF00) | (rt >> 24); break;
        case 1: w = (w & 0xFFFF0000) | (rt >> 16); break;
        case 2: w = (w & 0xFF000000) | (rt >> 8); break;
        default: w = rt; break;
    }
    mem_w32(al, w);
}
static inline void mem_swr(uint32_t a, uint32_t rt) {
    uint32_t al = a & ~3u, w = mem_r32(al);
    switch (a & 3) {
        case 0: w = rt; break;
        case 1: w = (w & 0x000000FF) | (rt << 8); break;
        case 2: w = (w & 0x0000FFFF) | (rt << 16); break;
        default: w = (w & 0x00FFFFFF) | (rt << 24); break;
    }
    mem_w32(al, w);
}

// ------------------------------------------------------------------ CPU helpers
static inline void cpu_div(CPU* c, uint32_t s, uint32_t t) {
    int32_t a = (int32_t)s, b = (int32_t)t;
    if (b == 0) { c->hi = s; c->lo = a >= 0 ? 0xFFFFFFFFu : 1u; }
    else if (a == INT32_MIN && b == -1) { c->hi = 0; c->lo = 0x80000000u; }
    else { c->lo = (uint32_t)(a / b); c->hi = (uint32_t)(a % b); }
}
static inline void cpu_divu(CPU* c, uint32_t s, uint32_t t) {
    if (t == 0) { c->hi = s; c->lo = 0xFFFFFFFFu; }
    else { c->lo = s / t; c->hi = s % t; }
}

uint32_t cop0_read(CPU* c, int reg);
void cop0_write(CPU* c, int reg, uint32_t v);
void cop0_rfe(CPU* c);
void cpu_syscall(CPU* c, uint32_t pc);
void cpu_break(CPU* c, uint32_t pc);
void cpu_unknown(CPU* c, uint32_t pc, uint32_t word);

uint32_t gte_read_data(CPU* c, int reg);
uint32_t gte_read_ctrl(CPU* c, int reg);
void gte_write_data(CPU* c, int reg, uint32_t v);
void gte_write_ctrl(CPU* c, int reg, uint32_t v);
void gte_command(CPU* c, uint32_t cmd);

void call_indirect(CPU* c, uint32_t target);
void jump_indirect(CPU* c, uint32_t target);
void bios_call(CPU* c, uint32_t vector, uint32_t fn);
int hle_hook(CPU* c, uint32_t addr);   // native replacements of game routines (hooks in recomp.json)

// Virtual PS1 clock: every basic block adds its estimated cycle cost. Loop back-edges check
// whether a timed event (VBlank, CD, timers, audio) or an interrupt is due.
extern uint64_t g_cycles, g_next_event;
void cpu_poll(CPU* c);
#define CYC(n) (g_cycles += (n))
extern uint32_t g_poll_pc;
// function-entry trace ring (debugging): enabled with --trace
extern int g_trace_on;
extern uint32_t g_trace_ring[1 << 20];
extern unsigned g_trace_pos;
#define FN_ENTER(a) do { if (g_trace_on) g_trace_ring[g_trace_pos++ & ((1 << 20) - 1)] = (a); } while (0)
void trace_dump(const char* why);      // MIPS address of the last check (debugging)
#define IRQ_CHECK(c, pc) do { if (g_cycles >= g_next_event) { g_poll_pc = (pc); cpu_poll(c); } } while (0)

#ifdef __cplusplus
}
#endif
