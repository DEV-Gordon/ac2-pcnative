// DMA controller and root counters (timers).
#include "hw.h"

// ------------------------------------------------------------------ DMA
static uint32_t madr[7], bcr[7], chcr[7];
static uint32_t dpcr = 0x07654321, dicr;

static void dicr_update(void) {
    uint32_t flags = (dicr >> 24) & 0x7F, enables = (dicr >> 16) & 0x7F;
    int master = (dicr & 0x8000) || ((dicr & 0x800000) && (flags & enables));
    int was = (dicr >> 31) & 1;
    dicr = (dicr & 0x7FFFFFFF) | ((uint32_t)master << 31);
    if (master && !was) irq_raise(IRQ_DMA);
}

static void dma_done(int ch) {
    chcr[ch] &= ~0x11000000u;
    if (dicr & (1u << (16 + ch))) dicr |= 1u << (24 + ch);
    dicr_update();
}

static void dma_run(int ch) {
    uint32_t c = chcr[ch];
    if (ch <= 1) LOGT("dma%d madr=%06x bcr=%08x chcr=%08x dicr=%08x", ch, madr[ch], bcr[ch], c, dicr);
    int to_ram = !(c & 1), step = (c & 2) ? -4 : 4, sync = (c >> 9) & 3;
    uint32_t addr = madr[ch] & 0x1FFFFC;
    uint32_t words;
    if (sync == 0) words = bcr[ch] & 0xFFFF ? bcr[ch] & 0xFFFF : 0x10000;
    else words = (bcr[ch] & 0xFFFF) * (bcr[ch] >> 16);

    switch (ch) {
        case 0:   // MDEC in
            for (uint32_t i = 0; i < words; i++, addr += step) {
                uint32_t w = mem_r32(0x80000000 | addr);
                mdec_dma_in(&w, 1);
            }
            break;
        case 1:   // MDEC out
            for (uint32_t i = 0; i < words; i++, addr += step) {
                uint32_t w;
                mdec_dma_out(&w, 1);
                mem_w32(0x80000000 | addr, w);
            }
            break;
        case 2:   // GPU
            if (sync == 2) {
                // linked list (ordering table)
                int guard = 0;
                while (guard++ < 0x40000) {
                    uint32_t hdr = mem_r32(0x80000000 | addr);
                    uint32_t n = hdr >> 24;
                    for (uint32_t i = 1; i <= n; i++) gpu_write_gp0(mem_r32(0x80000000 | ((addr + 4 * i) & 0x1FFFFC)));
                    if ((hdr & 0x800000) || (hdr & 0xFFFFFF) == 0xFFFFFF) break;
                    addr = hdr & 0x1FFFFC;
                }
                madr[ch] = 0xFFFFFF;
            } else if (to_ram) {
                for (uint32_t i = 0; i < words; i++, addr += step) mem_w32(0x80000000 | addr, gpu_read());
            } else {
                for (uint32_t i = 0; i < words; i++, addr += step) gpu_write_gp0(mem_r32(0x80000000 | addr));
            }
            break;
        case 3:   // CD-ROM
            for (uint32_t i = 0; i < words; i++, addr += step) mem_w32(0x80000000 | addr, cd_dma_read_word());
            break;
        case 4: { // SPU
            for (uint32_t i = 0; i < words; i++, addr += step) {
                uint32_t w;
                if (to_ram) { spu_dma_read(&w, 1); mem_w32(0x80000000 | addr, w); }
                else { w = mem_r32(0x80000000 | addr); spu_dma_write(&w, 1); }
            }
            break;
        }
        case 6:   // OTC: reverse linked list of empty entries
            for (uint32_t i = 0; i < words; i++) {
                mem_w32(0x80000000 | addr, i == words - 1 ? 0xFFFFFF : ((addr - 4) & 0x1FFFFF));
                addr -= 4;
            }
            break;
        default:
            LOGE("DMA channel %d not supported", ch);
    }
    if (sync != 2) madr[ch] = (0x80000000 | addr) & 0xFFFFFF;
    dma_done(ch);
}

void dma_write(uint32_t a, uint32_t v) {
    if (a == 0x1F8010F0) { dpcr = v; return; }
    if (a == 0x1F8010F4) {
        LOGT("dicr write %08x (was %08x)", v, dicr);
        // bits 24-30 are write-1-to-clear flags
        uint32_t keep = (dicr & 0x7F000000) & ~(v & 0x7F000000);
        dicr = (v & 0x00FF803F) | keep;
        dicr_update();
        return;
    }
    int ch = (a - 0x1F801080) >> 4, reg = (a >> 2) & 3;
    if (ch > 6) return;
    if (reg == 0) madr[ch] = v & 0xFFFFFF;
    else if (reg == 1) bcr[ch] = v;
    else if (reg == 2) {
        chcr[ch] = v;
        int sync = (v >> 9) & 3;
        if ((v & 0x01000000) && (sync != 0 || (v & 0x10000000) || ch == 3)) dma_run(ch);
    }
}

uint32_t dma_read(uint32_t a) {
    if (a == 0x1F8010F0) return dpcr;
    if (a == 0x1F8010F4) return dicr;
    int ch = (a - 0x1F801080) >> 4, reg = (a >> 2) & 3;
    if (ch > 6) return 0;
    return reg == 0 ? madr[ch] : reg == 1 ? bcr[ch] : chcr[ch];
}

// ------------------------------------------------------------------ root counters
#define HBLANK_HZ 15734.264
typedef struct {
    uint32_t mode, target;
    double base_value;      // counter value at base_time
    uint64_t base_time;
    int irq_scheduled;
} Timer;
static Timer tm[3];

static double timer_rate(int n) {     // counter ticks per CPU cycle
    int src = (tm[n].mode >> 8) & 3;
    if (n == 0) return (src & 1) ? (6.65e6 / PSX_CLOCK) : 1.0;
    if (n == 1) return (src & 1) ? (HBLANK_HZ / PSX_CLOCK) : 1.0;
    return (src & 2) ? 1.0 / 8.0 : 1.0;
}

static uint32_t timer_value(int n) {
    Timer* t = &tm[n];
    double v = t->base_value + (double)(time_cycles() - t->base_time) * timer_rate(n);
    uint32_t period = (t->mode & 8) && t->target ? t->target + 1 : 0x10000;
    return (uint32_t)((uint64_t)v % period);
}

static void timer_irq_0(void);
static void timer_irq_1(void);
static void timer_irq_2(void);
static const EventFn timer_irq_fn[3] = {timer_irq_0, timer_irq_1, timer_irq_2};

static void timer_schedule(int n) {
    Timer* t = &tm[n];
    if (!(t->mode & 0x30) || t->irq_scheduled) return;
    uint32_t v = timer_value(n);
    uint32_t goal = (t->mode & 0x10) ? t->target : 0xFFFF;
    double ticks = goal > v ? goal - v : ((t->mode & 8) && t->target ? t->target + 1 : 0x10000) - v + goal;
    if (ticks < 1) ticks = 1;
    sched_add(time_cycles() + (uint64_t)(ticks / timer_rate(n)), timer_irq_fn[n]);
    t->irq_scheduled = 1;
}

static void timer_fire(int n) {
    Timer* t = &tm[n];
    t->irq_scheduled = 0;
    if (!(t->mode & 0x30)) return;
    t->mode |= (t->mode & 0x10) ? 0x800 : 0x1000;
    irq_raise(IRQ_TMR0 + n);
    if (t->mode & 0x40) timer_schedule(n);   // repeat
}
static void timer_irq_0(void) { timer_fire(0); }
static void timer_irq_1(void) { timer_fire(1); }
static void timer_irq_2(void) { timer_fire(2); }

void timers_write(uint32_t a, uint32_t v) {
    int n = (a >> 4) & 3, reg = (a >> 2) & 3;
    if (n > 2) return;
    Timer* t = &tm[n];
    if (reg == 0) { t->base_value = v & 0xFFFF; t->base_time = time_cycles(); }
    else if (reg == 1) {
        t->mode = (v & 0x3FF) | 0x400;
        t->base_value = 0;
        t->base_time = time_cycles();
        t->irq_scheduled = 0;
        timer_schedule(n);
    } else if (reg == 2) { t->target = v & 0xFFFF; timer_schedule(n); }
}

uint32_t timers_read(uint32_t a) {
    int n = (a >> 4) & 3, reg = (a >> 2) & 3;
    if (n > 2) return 0;
    if (reg == 0) return timer_value(n);
    if (reg == 1) { uint32_t m = tm[n].mode; tm[n].mode &= ~0x1800u; return m; }
    return tm[n].target;
}

void timers_vblank(void) {}
