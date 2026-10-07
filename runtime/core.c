// Memory map, I/O dispatch, interrupts, scheduler and the glue the recompiled code calls into.
#include "hw.h"
#include <setjmp.h>
#include <stdlib.h>
#include <time.h>

int g_log_level = 1;
uint8_t* g_ram;
uint8_t g_scratch[1024];
uint32_t g_istat, g_imask;
uint64_t g_cycles, g_next_event;

int dbg_env(const char* name) {
    static struct { const char* n; int v; } cache[16];
    static int count;
    for (int i = 0; i < count; i++)
        if (!strcmp(cache[i].n, name)) return cache[i].v;
    int v = getenv(name) != NULL;
    if (count < 16) cache[count++] = (typeof(cache[0])){name, v};
    return v;
}
uint32_t g_poll_pc;
int g_trace_on;
uint32_t g_trace_ring[1 << 20];
unsigned g_trace_pos;

void trace_dump(const char* why) {
    if (!g_trace_on) return;
    FILE* f = fopen("trace.txt", "w");
    if (!f) return;
    fprintf(f, "# %s\n", why);
    unsigned n = g_trace_pos < (1u << 20) ? g_trace_pos : (1u << 20);
    for (unsigned i = 0; i < n; i++) fprintf(f, "%08x\n", g_trace_ring[(g_trace_pos - n + i) & ((1u << 20) - 1)]);
    fclose(f);
    LOGI("trace written to trace.txt (%s)", why);
}
volatile int g_quit;

static uint32_t memctrl[16], ram_size_reg, cache_ctrl;

// ------------------------------------------------------------------ time
static struct timespec t0;
extern double g_speed;
uint64_t time_cycles(void) { return g_cycles; }

static double real_seconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)(t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) * 1e-9;
}

// ------------------------------------------------------------------ scheduler
typedef struct { uint64_t at; EventFn fn; } Event;
static Event events[64];
static int nevents;

void sched_add(uint64_t at, EventFn fn) {
    if (nevents < 64) events[nevents++] = (Event){at, fn};
    else LOGE("scheduler full");
    if (at < g_next_event) g_next_event = at;
}

static void sched_recalc(void) {
    uint64_t m = UINT64_MAX;
    for (int i = 0; i < nevents; i++)
        if (events[i].at < m) m = events[i].at;
    g_next_event = m;
}

void sched_run(void) {
    uint64_t now = time_cycles();
    for (int pass = 0; pass < 16; pass++) {
        int ran = 0;
        for (int i = 0; i < nevents; i++) {
            if (events[i].at <= now) {
                EventFn fn = events[i].fn;
                events[i] = events[--nevents];
                fn();
                ran = 1;
                break;   // list may have changed
            }
        }
        if (!ran) break;
    }
    sched_recalc();
}

// ------------------------------------------------------------------ interrupts
void irq_raise(int n) {
    g_istat |= 1u << n;
    g_next_event = 0;   // service at the next check
}

static int in_exception;
static CPU* g_cpu;

void app_exit(void);
void cpu_poll(CPU* c) {
    if (g_quit) app_exit();
    sched_run();
    if (in_exception) return;
    uint32_t sr = c->cop0[12];
    if (g_log_level >= 2) {
        static uint64_t last;
        uint64_t now = time_cycles();
        if (now - last > (uint64_t)PSX_CLOCK) {
            last = now;
            void cd_debug(void);
            cd_debug();
            LOGD("irq state: istat=%03x imask=%03x sr=%08x crit=%d pc=%08x ra=%08x", g_istat, g_imask, sr, g_crit, g_poll_pc, c->r[31]);
        }
    }
    if ((g_istat & g_imask) && (sr & 1) && (sr & 0x400) && !g_crit) {
        in_exception = 1;
        bios_exception(c);
        in_exception = 0;
        sched_recalc();
        if (g_istat & g_imask) {
            // nobody acknowledged it: retry a bit later instead of storming
            uint64_t retry = g_cycles + 2000;
            if (retry < g_next_event) g_next_event = retry;
        }
    }
}

// ------------------------------------------------------------------ cop0 / syscall
uint32_t cop0_read(CPU* c, int reg) {
    if (reg == 13) return (c->cop0[13] & ~0x400u) | ((g_istat & g_imask) ? 0x400u : 0);
    return c->cop0[reg];
}
void cop0_write(CPU* c, int reg, uint32_t v) {
    c->cop0[reg] = v;
    if (reg == 12) g_next_event = 0;
}
void cop0_rfe(CPU* c) {
    uint32_t sr = c->cop0[12];
    c->cop0[12] = (sr & ~0x0Fu) | ((sr >> 2) & 0x0F);
}

int g_crit;
void cpu_syscall(CPU* c, uint32_t pc) {
    uint32_t fn = c->r[4];
    switch (fn) {
        // The BIOS edits the SR saved by the syscall exception (IEp) and rfe moves it to IEc.
        // Without a real exception we change IEc (bit 0) and IM2 (bit 10) directly.
        case 1: {   // EnterCriticalSection
            uint32_t sr = c->cop0[12];
            c->r[2] = (sr & 0x401) == 0x401;
            c->cop0[12] = sr & ~0x401u;
            break;
        }
        case 2:     // ExitCriticalSection
            c->cop0[12] |= 0x401;
            g_next_event = 0;
            break;
        case 0:
            break;
        default:
            LOGE("syscall(%u) at %08x not implemented", fn, pc);
    }
}
void cpu_break(CPU* c, uint32_t pc) { LOGE("break at %08x (a0=%08x)", pc, c->r[4]); }
void cpu_unknown(CPU* c, uint32_t pc, uint32_t w) {
    LOGE("unknown instruction %08x at %08x (ra=%08x)", w, pc, c->r[31]);
    exit(3);
}

// ------------------------------------------------------------------ indirect control flow
static RecompFn lookup(uint32_t a) {
    a = (a & 0x1FFFFFFF) | 0x80000000;
    unsigned lo = 0, hi = recomp_table_size;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        uint32_t m = recomp_table[mid].addr;
        if (m == a) return recomp_table[mid].fn;
        if (m < a) lo = mid + 1; else hi = mid;
    }
    return 0;
}

// BIOS table thunks (see bios_init)
void bios_pad_enable(int on);
static int bios_thunk(CPU* c, uint32_t p) {
    // PsyQ's _patch_pad reaches into the kernel relative to B0:5B (ChangeClearPad, thunk 0x156C):
    // +0x884 / +0x894 are the kernel's "enable / disable pad reading on VBlank" routines.
    // +0x7A0 registers secondary 7-byte buffers the kernel leaves untouched for normal pads
    // (verified against a RAM dump of real BIOS behaviour): accept and ignore.
    if (p == 0x156C + 0x884) { bios_pad_enable(1); return 1; }
    if (p == 0x156C + 0x894) { bios_pad_enable(0); return 1; }
    if (p == 0x156C + 0x7A0) { c->r[2] = 1; return 1; }
    if (p >= 0x156C && p < 0x156C + 0x1000 && (p < 0x1000 || p >= 0x1C00)) {
        LOGE("call into the kernel at B0:5B+%#x (BIOS patch) is not emulated", p - 0x156C);
        exit(4);
    }
    if (p < 0x1000 || p >= 0x1C00) return 0;
    uint32_t off = p - 0x1000;
    uint32_t vec = off < 0x400 ? 0xA0 : off < 0x800 ? 0xB0 : 0xC0;
    bios_call(c, vec, (off & 0x3FF) / 4);
    return 1;
}

void call_indirect(CPU* c, uint32_t target) {
    uint32_t p = target & 0x1FFFFFFF;
    if (p == 0xA0 || p == 0xB0 || p == 0xC0) { bios_call(c, p, c->r[9]); return; }
    if (bios_thunk(c, p)) return;
    RecompFn fn = lookup(target);
    if (fn) { fn(c); return; }
    LOGE("call to unknown address %08x (ra=%08x) - add it to recomp.json seeds", target, c->r[31]);
    exit(4);
}

void jump_indirect(CPU* c, uint32_t target) {
    uint32_t p = target & 0x1FFFFFFF;
    if (p == 0xA0 || p == 0xB0 || p == 0xC0) { bios_call(c, p, c->r[9]); return; }
    if (bios_thunk(c, p)) return;
    RecompFn fn = lookup(target);
    if (fn) { fn(c); return; }
    if (target == c->r[31]) return;   // "jr $reg" used as a return
    LOGE("jump to unknown address %08x (ra=%08x)", target, c->r[31]);
    exit(4);
}

// ------------------------------------------------------------------ I/O
uint32_t io_read(uint32_t a, int size) {
    uint32_t p = a & 0x1FFFFFFF;
    if (p >= 0x1F801000 && p < 0x1F801024) return memctrl[(p - 0x1F801000) / 4];
    if (p == 0x1F801060) return ram_size_reg;
    if (p == 0x1F801070) return g_istat;
    if (p == 0x1F801074) return g_imask;
    if (p >= 0x1F801080 && p < 0x1F801100) return dma_read(p);
    if (p >= 0x1F801100 && p < 0x1F801130) return timers_read(p);
    if (p >= 0x1F801800 && p < 0x1F801804) {
        if (size == 1) return cd_read(p);
        uint32_t v = 0;
        for (int i = 0; i < size; i++) v |= (uint32_t)cd_read(p + i) << (8 * i);
        return v;
    }
    if (p == 0x1F801810) return gpu_read();
    if (p == 0x1F801814) return gpu_stat();
    if (p == 0x1F801820 || p == 0x1F801824) return mdec_read(p);
    if (p >= 0x1F801C00 && p < 0x1F802000) {
        if (size == 4) return spu_read(p) | ((uint32_t)spu_read(p + 2) << 16);
        return spu_read(p & ~1u);
    }
    if (p >= 0x1F801040 && p < 0x1F801060) return p == 0x1F801044 ? 0x5 : 0;   // SIO: idle, TX ready
    if (p >= 0x1F000000 && p < 0x1F800000) return 0xFFFFFFFF;   // expansion 1 (nothing)
    if (p >= 0x1FC00000 && p < 0x1FC80000) return 0;            // BIOS ROM (not used)
    if (a == 0xFFFE0130) return cache_ctrl;
    LOGD("io_read%d %08x", size * 8, a);
    return 0;
}

void io_write(uint32_t a, uint32_t v, int size) {
    uint32_t p = a & 0x1FFFFFFF;
    if (p >= 0x1F801000 && p < 0x1F801024) { memctrl[(p - 0x1F801000) / 4] = v; return; }
    if (p == 0x1F801060) { ram_size_reg = v; return; }
    if (p == 0x1F801070) { g_istat &= v; return; }
    if (p == 0x1F801074) { g_imask = v & 0x7FF; g_next_event = 0; return; }
    if (p >= 0x1F801080 && p < 0x1F801100) { dma_write(p, v); return; }
    if (p >= 0x1F801100 && p < 0x1F801130) { timers_write(p, v); return; }
    if (p >= 0x1F801800 && p < 0x1F801804) {
        for (int i = 0; i < size; i++) cd_write(p + i, (uint8_t)(v >> (8 * i)));
        return;
    }
    if (p == 0x1F801810) { gpu_write_gp0(v); return; }
    if (p == 0x1F801814) { gpu_write_gp1(v); return; }
    if (p == 0x1F801820 || p == 0x1F801824) { mdec_write(p, v); return; }
    if (p >= 0x1F801C00 && p < 0x1F802000) {
        if (size == 4) { spu_write(p, v & 0xFFFF); spu_write(p + 2, v >> 16); }
        else spu_write(p & ~1u, v & 0xFFFF);
        return;
    }
    if (p >= 0x1F801040 && p < 0x1F801060) return;
    if (p >= 0x1F802000 && p < 0x1F803000) return;   // expansion 2 (debug LEDs etc.)
    if (a == 0xFFFE0130) { cache_ctrl = v; return; }
    LOGD("io_write%d %08x = %08x", size * 8, a, v);
}

// ------------------------------------------------------------------ boot
static void vblank_event(void);
static uint64_t next_vblank;
#define VBLANK_CYCLES (PSX_CLOCK / 59.94)

static uint64_t vblank_count;
static double real_base;

uint64_t core_vblank_count(void) { return vblank_count; }
// restart real-time pacing from now (after a fast-forward or a speed change)
void core_pace_reset(void) { real_base = real_seconds() - vblank_count / 59.94 / g_speed; }

static void vblank_event(void) {
    next_vblank += (uint64_t)VBLANK_CYCLES;
    sched_add(next_vblank, vblank_event);
    // pace virtual time against the real clock
    vblank_count++;
    double target = real_base + vblank_count / 59.94 / g_speed;
    double now = real_seconds();
    if (g_replaying) {
        // fast-forwarding a debug state: no pacing
    } else if (now < target) {
        struct timespec ts = {0, (long)((target - now) * 1e9)};
        if (g_speed < 100) nanosleep(&ts, NULL);
    } else if (now - target > 0.25) {
        real_base += now - target;   // far behind (debugger, heavy load): resynchronise
    }
    gpu_vblank();
    timers_vblank();
    irq_raise(IRQ_VBLANK);
    platform_latch_input(vblank_count);   // the buttons the game sees until the next VBlank
    if (g_cpu) bios_vblank(g_cpu);
    platform_vblank();
}

CPU* cpu_get(void) { return g_cpu; }

// audio is produced in small blocks on the game thread, in step with PS1 time
#define AUDIO_BLOCK 128
static uint64_t next_audio;
void platform_audio_push(const int16_t* lr, int frames);
static void audio_event(void) {
    static int16_t buf[AUDIO_BLOCK * 2];
    next_audio += (uint64_t)(PSX_CLOCK * AUDIO_BLOCK / 44100.0);
    uint64_t now = time_cycles();
    if (next_audio + (uint64_t)(PSX_CLOCK * 0.1) < now) next_audio = now;   // skip after a stall
    sched_add(next_audio, audio_event);
    spu_render(buf, AUDIO_BLOCK);
    platform_audio_push(buf, AUDIO_BLOCK);
}

// Loads a PS-X EXE image (header + body) into RAM; returns entry point.
uint32_t load_psx_exe(const uint8_t* exe, uint32_t* gp, uint32_t* sp) {
    uint32_t pc, t_addr, t_size, b_addr, b_size, s_addr, s_size;
    memcpy(&pc, exe + 0x10, 4);
    memcpy(gp, exe + 0x14, 4);
    memcpy(&t_addr, exe + 0x18, 4);
    memcpy(&t_size, exe + 0x1C, 4);
    memcpy(&b_addr, exe + 0x28, 4);
    memcpy(&b_size, exe + 0x2C, 4);
    memcpy(&s_addr, exe + 0x30, 4);
    memcpy(&s_size, exe + 0x34, 4);
    memcpy(g_ram + (t_addr & 0x1FFFFF), exe + 0x800, t_size);
    if (b_size) memset(g_ram + (b_addr & 0x1FFFFF), 0, b_size);
    if (s_addr) *sp = s_addr + s_size;
    return pc;
}

void core_start(CPU* c, uint32_t entry) {
    g_cpu = c;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    next_vblank = (uint64_t)VBLANK_CYCLES;
    sched_add(next_vblank, vblank_event);
    next_audio = 0;
    sched_add(next_audio, audio_event);
    LOGI("starting at %08x", entry);
    call_indirect(c, entry);
    LOGE("entry point returned");
}

void core_init(void) {
    g_ram = calloc(1, 2 * 1024 * 1024);
}
