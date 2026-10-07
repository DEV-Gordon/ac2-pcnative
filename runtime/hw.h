// Hardware-facing runtime services (C linkage, shared by all runtime modules).
#pragma once
#include <stdint.h>
#include <stdio.h>
#include "cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

// ------------------------------------------------------------------ logging
extern int g_log_level;   // 0 errors, 1 info, 2 debug, 3 trace
#define LOGE(...) do { fprintf(stderr, "[E] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define LOGI(...) do { if (g_log_level >= 1) { fprintf(stderr, "[I] " __VA_ARGS__); fputc('\n', stderr); } } while (0)
#define LOGD(...) do { if (g_log_level >= 2) { fprintf(stderr, "[D] " __VA_ARGS__); fputc('\n', stderr); } } while (0)
#define LOGT(...) do { if (g_log_level >= 3) { fprintf(stderr, "[T] " __VA_ARGS__); fputc('\n', stderr); } } while (0)

// ------------------------------------------------------------------ debug switches (environment, read once)
int dbg_env(const char* name);   // 1 if the variable is set (cached)

// ------------------------------------------------------------------ time
// "PS1 time" in CPU cycles (33.8688 MHz), derived from real time.
#define PSX_CLOCK 33868800.0
uint64_t time_cycles(void);

// ------------------------------------------------------------------ interrupts
enum {
    IRQ_VBLANK = 0, IRQ_GPU = 1, IRQ_CDROM = 2, IRQ_DMA = 3, IRQ_TMR0 = 4, IRQ_TMR1 = 5, IRQ_TMR2 = 6,
    IRQ_SIO = 7, IRQ_SIO2 = 8, IRQ_SPU = 9, IRQ_PIO = 10
};
extern uint32_t g_istat, g_imask;
void irq_raise(int n);

// scheduled events (cycle timestamps)
typedef void (*EventFn)(void);
void sched_add(uint64_t at, EventFn fn);
void sched_run(void);

// ------------------------------------------------------------------ devices
void dma_write(uint32_t addr, uint32_t v);
uint32_t dma_read(uint32_t addr);
void timers_write(uint32_t addr, uint32_t v);
uint32_t timers_read(uint32_t addr);
void timers_vblank(void);

void cd_init(const char* bin_path);
uint8_t cd_read(uint32_t addr);
void cd_write(uint32_t addr, uint8_t v);
uint32_t cd_dma_read_word(void);

void gpu_init(void);
void gpu_write_gp0(uint32_t v);
void gpu_write_gp1(uint32_t v);
uint32_t gpu_read(void);
uint32_t gpu_stat(void);
void gpu_vblank(void);
const uint16_t* gpu_vram(void);
int gpu_scale(void);            // internal resolution multiplier (VRAM stored at 1024*S x 512*S)
void gpu_set_scale(int s);
void gpu_sync(void);
extern int g_gpu_skip_draw;    // drop drawing commands (debug-state fast-forward)            // waits for the render threads (gpu_vram() does it too)
typedef struct DisplayInfo { int x, y, w, h; int rgb24; int enabled; int interlaced; } DisplayInfo;
DisplayInfo gpu_display(void);

void spu_init(void);
uint16_t spu_read(uint32_t addr);
void spu_write(uint32_t addr, uint16_t v);
void spu_dma_write(const uint32_t* src, uint32_t words);
void spu_dma_read(uint32_t* dst, uint32_t words);
void spu_render(int16_t* out, int frames);   // 44.1 kHz stereo
void spu_xa_push(const int16_t* lr, int frames, int rate);

void mdec_write(uint32_t addr, uint32_t v);
uint32_t mdec_read(uint32_t addr);
void mdec_dma_in(const uint32_t* src, uint32_t words);
void mdec_dma_out(uint32_t* dst, uint32_t words);

// ------------------------------------------------------------------ BIOS (HLE)
void bios_init(CPU* c);
void bios_vblank(CPU* c);           // pad buffers, card events
void bios_exception(CPU* c);        // runs the interrupt handlers (called from cpu_poll)
extern int g_crit;                  // interrupts disabled (EnterCriticalSection / SR.IEc == 0)

// ------------------------------------------------------------------ platform (SDL, C++)
void platform_vblank(void);          // present + poll input; may set g_quit
extern volatile int g_quit;
uint16_t platform_pad_buttons(int port);   // live active-low PS1 button mask (movie player)
uint16_t platform_game_buttons(int port);  // what the game sees: latched once per VBlank (recorded / replayed)
void platform_latch_input(uint64_t vblank);
extern int g_replaying;                     // fast-forwarding a debug state (no pacing, drawing, sound)
uint64_t core_vblank_count(void);
void core_pace_reset(void);

#ifdef __cplusplus
}
#endif
