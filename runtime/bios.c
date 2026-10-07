// High-level reimplementation of the PlayStation BIOS calls the game uses (A0/B0/C0 tables),
// plus the kernel's interrupt/exception plumbing.
#include "hw.h"
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

uint32_t load_psx_exe(const uint8_t* exe, uint32_t* gp, uint32_t* sp);

#define EXC_STACK 0x8000F000u      // kernel area, free in our HLE world

// ------------------------------------------------------------------ events
enum { EV_FREE = 0, EV_DISABLED = 0x1000, EV_ENABLED = 0x2000, EV_READY = 0x4000 };
enum { EV_MODE_INTR = 0x1000, EV_MODE_NOINTR = 0x2000 };
typedef struct { uint32_t cls, spec, mode, func, status; } Ev;
static Ev evs[32];

static uint32_t open_event(uint32_t cls, uint32_t spec, uint32_t mode, uint32_t func) {
    for (int i = 0; i < 32; i++)
        if (evs[i].status == EV_FREE) {
            evs[i] = (Ev){cls, spec, mode, func, EV_DISABLED};
            LOGD("OpenEvent(%08x,%04x,%04x,%08x) = %d", cls, spec, mode, func, i);
            return 0xF1000000u | i;
        }
    LOGE("out of events");
    return 0xFFFFFFFF;
}
static Ev* ev_get(uint32_t id) { return (id & 0xFFFF) < 32 ? &evs[id & 0xFFFF] : NULL; }

static CPU* g_c;
static void deliver_event(uint32_t cls, uint32_t spec) {
    for (int i = 0; i < 32; i++) {
        Ev* e = &evs[i];
        if (e->status != EV_ENABLED || e->cls != cls || e->spec != spec) continue;
        if (e->mode == EV_MODE_INTR) {
            if (e->func && g_c) {
                CPU save = *g_c;
                g_c->r[29] = EXC_STACK - 0x800;
                call_indirect(g_c, e->func);
                *g_c = save;
            }
        } else {
            e->status = EV_READY;
        }
    }
}

// ------------------------------------------------------------------ interrupts / exceptions
static uint32_t custom_exit;          // jmp_buf in RAM set by SetCustomExitFromException
static uint32_t chains[4];           // SysEnqIntRP lists
static int clear_rcnt[4] = {1, 1, 1, 1};
static jmp_buf exc_return;
static int exc_active;

void bios_exception(CPU* c) {
    CPU saved = *c;
    g_c = c;
    // like the hardware: push the interrupt-enable/kernel-mode stack
    uint32_t sr = c->cop0[12];
    c->cop0[12] = (sr & ~0x3Fu) | ((sr << 2) & 0x3F);
    c->cop0[13] = 0;   // cause: interrupt
    c->r[29] = EXC_STACK;

    for (int prio = 0; prio < 4; prio++) {
        uint32_t e = chains[prio];
        int guard = 0;
        while (e && guard++ < 32) {
            uint32_t next = mem_r32(e), handler = mem_r32(e + 4), verifier = mem_r32(e + 8);
            if (verifier) {
                c->r[29] = EXC_STACK;
                call_indirect(c, verifier);
                uint32_t v = c->r[2];
                if (v && handler) {
                    c->r[4] = v;
                    c->r[29] = EXC_STACK;
                    call_indirect(c, handler);
                }
            }
            e = next;
        }
        if (prio == 0) {
            // BIOS default handlers: root counters 0-2 and VBlank (= root counter 3)
            static const int bits[4] = {IRQ_TMR0, IRQ_TMR1, IRQ_TMR2, IRQ_VBLANK};
            for (int t = 0; t < 4; t++) {
                uint32_t m = 1u << bits[t];
                if (g_istat & g_imask & m) {
                    deliver_event(0xF2000000u | t, 0x0002);
                    if (clear_rcnt[t]) g_istat &= ~m;
                }
            }
        }
    }

    if (custom_exit) {
        // longjmp into the program (libetc's interrupt dispatcher), which ends in ReturnFromException
        exc_active = 1;
        if (!setjmp(exc_return)) {
            uint32_t b = custom_exit;
            c->r[31] = mem_r32(b + 0x00);
            c->r[29] = mem_r32(b + 0x04);
            c->r[30] = mem_r32(b + 0x08);
            for (int i = 0; i < 8; i++) c->r[16 + i] = mem_r32(b + 0x0C + 4 * i);
            c->r[28] = mem_r32(b + 0x2C);
            c->r[2] = 1;
            call_indirect(c, c->r[31]);
            LOGE("custom exception exit returned without ReturnFromException");
        }
        exc_active = 0;
    }
    *c = saved;
}

// ------------------------------------------------------------------ pads (BIOS InitPad/StartPad)
static uint32_t pad_buf[2], pad_size[2];
static int pad_started;
void bios_pad_enable(int on) { pad_started = on; LOGD("kernel pad reading %s", on ? "on" : "off"); }
static uint32_t old_pad_dest;   // OutdatedPadInitAndStart

static void pad_fill(void) {
    for (int port = 0; port < 2; port++) {
        uint32_t b = pad_buf[port];
        if (!b) continue;
        uint16_t buttons = platform_game_buttons(port);
        if (port == 0 && !dbg_env("AC2_NOPAD")) {
            mem_w8(b + 0, 0x00);          // ok
            mem_w8(b + 1, 0x41);          // digital pad, 1 halfword of data
            mem_w8(b + 2, buttons & 0xFF);
            mem_w8(b + 3, buttons >> 8);
        } else {
            mem_w8(b + 0, 0xFF);          // no controller
        }
    }
    if (old_pad_dest) mem_w32(old_pad_dest, ~(uint32_t)platform_game_buttons(0) & 0xFFFF);
}

// ------------------------------------------------------------------ memory card (files on the host)
static char card_dir[2][512];
typedef struct { FILE* f; int used; uint32_t pos; } Fd;
static Fd fds[16];
// File descriptors 0 and 1 are the TTY; card files get 2..17 like on the real BIOS (handing out 0 sent the
// game's save data to the debug log instead of the card).
#define FD_BASE 2
static Fd* fd_get(uint32_t fd) { return fd >= FD_BASE && fd < FD_BASE + 16 && fds[fd - FD_BASE].used ? &fds[fd - FD_BASE] : NULL; }
static int card_pending[2];       // deliver "I/O done" card events on the next vblank

static int card_port(const char* dev) {
    // "bu00:" -> port 0, "bu10:" -> port 1
    if (strncmp(dev, "bu", 2) != 0) return -1;
    return dev[2] == '1' ? 1 : 0;
}

static void read_cstr(uint32_t a, char* out, int max) {
    int i = 0;
    for (; i < max - 1; i++) {
        out[i] = (char)mem_r8(a + i);
        if (!out[i]) break;
    }
    out[i] = 0;
}

static int file_open(uint32_t name_ptr, uint32_t mode) {
    char name[64];
    read_cstr(name_ptr, name, sizeof name);
    int port = card_port(name);
    const char* colon = strchr(name, ':');
    if (port < 0 || !colon) { LOGE("FileOpen(%s) unsupported device", name); return -1; }
    char path[700];
    snprintf(path, sizeof path, "%s/%s", card_dir[port], colon + 1);
    FILE* f = NULL;
    if (mode & 0x200) {                      // FCREAT: blocks in the upper 16 bits
        uint32_t blocks = mode >> 16 ? mode >> 16 : 1;
        f = fopen(path, "w+b");
        if (f) {
            static uint8_t zero[8192];
            for (uint32_t i = 0; i < blocks; i++) fwrite(zero, 1, sizeof zero, f);
            fseek(f, 0, SEEK_SET);
        }
    } else {
        f = fopen(path, (mode & 2) ? "r+b" : "rb");
    }
    LOGI("FileOpen(%s, %x) -> %s", name, mode, f ? "ok" : "not found");
    if (!f) return -1;
    for (int i = 0; i < 16; i++)
        if (!fds[i].used) { fds[i] = (Fd){f, 1, 0}; card_pending[port] = 1; return i + FD_BASE; }
    fclose(f);
    return -1;
}

// direntry: name[20], attr, size, next, head, system[4]  (40 bytes)
static char find_pattern[64];
static int find_index;
static int find_port;

static uint32_t find_next(uint32_t de) {
    char dirpath[512];
    snprintf(dirpath, sizeof dirpath, "%s", card_dir[find_port]);
    // list files in the card directory, matching '?'/'*' patterns
    DIR* d = opendir(dirpath);
    if (!d) return 0;
    struct dirent* e;
    int idx = 0;
    uint32_t result = 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        const char* p = find_pattern;
        const char* n = e->d_name;
        int ok = 1;
        while (*p && ok) {
            if (*p == '*') break;
            if (!*n || (*p != '?' && *p != *n)) ok = 0;
            p++; n++;
        }
        if (ok && !*p && *n) ok = 0;
        if (!ok) continue;
        if (idx++ < find_index) continue;
        char full[1100];
        snprintf(full, sizeof full, "%s/%s", dirpath, e->d_name);
        struct stat st;
        stat(full, &st);
        for (int i = 0; i < 20; i++) mem_w8(de + i, i < (int)strlen(e->d_name) ? e->d_name[i] : 0);
        mem_w32(de + 20, 0x51);
        mem_w32(de + 24, (uint32_t)st.st_size);
        mem_w32(de + 28, 0);
        mem_w32(de + 32, 0);
        find_index++;
        result = de;
        break;
    }
    closedir(d);
    return result;
}

// ------------------------------------------------------------------ dispatch
static uint32_t heap_base, heap_size;

static void bios_a0(CPU* c, uint32_t fn) {
    uint32_t a0 = c->r[4], a1 = c->r[5], a2 = c->r[6];
    switch (fn) {
        case 0x39: heap_base = a0; heap_size = a1; c->r[2] = 0; break;           // InitHeap
        case 0x43: {                                                             // Exec(header, sp, size)
            uint32_t pc = mem_r32(a0), gp = mem_r32(a0 + 4);
            uint32_t b_addr = mem_r32(a0 + 0x18), b_size = mem_r32(a0 + 0x1C);
            uint32_t s_addr = mem_r32(a0 + 0x20), s_size = mem_r32(a0 + 0x24);
            for (uint32_t i = 0; i < b_size; i += 4) mem_w32(b_addr + i, 0);
            LOGI("Exec pc=%08x gp=%08x stack=%08x+%x args=(%x,%x)", pc, gp, s_addr, s_size, a1, a2);
            // The program being replaced leaves events open (its libraries' CD/VSync events); the new one
            // initialises its own and nothing can use the old ones any more. Without this the table ran out
            // after ~9 intro<->main cycles of the attract loop ("out of events").
            {
                int freed = 0;
                for (int i = 0; i < 32; i++) if (evs[i].status != EV_FREE) { evs[i].status = EV_FREE; freed++; }
                LOGD("Exec: released %d events of the previous program", freed);
            }
            c->r[28] = gp;
            if (s_addr) c->r[29] = c->r[30] = s_addr + s_size;
            c->r[4] = a1; c->r[5] = a2;   // the program's argc/argv
            call_indirect(c, pc);
            LOGE("executable at %08x returned", pc);
            exit(0);
        }
        case 0x44: break;                                                        // FlushCache
        case 0x49: gpu_write_gp0(a0); break;                                     // GPU_cw
        case 0x70: c->r[2] = 1; break;                                           // _bu_init
        case 0x72: break;                                                        // _96_remove (CdRemove)
        case 0xA1: LOGE("SystemErrorBootOrDiskFailure(%c, %x)", (char)a0, a1); exit(5);
        case 0xAB: card_pending[a0 >> 4 & 1] = 1; c->r[2] = 1; break;            // _card_info
        case 0xAC: card_pending[a0 >> 4 & 1] = 1; c->r[2] = 1; break;            // _card_load
        default: LOGE("A0:%02X not implemented (ra=%08x)", fn, c->r[31]); c->r[2] = 0;
    }
}

static void bios_b0(CPU* c, uint32_t fn) {
    uint32_t a0 = c->r[4], a1 = c->r[5], a2 = c->r[6], a3 = c->r[7];
    Ev* e;
    switch (fn) {
        case 0x07: deliver_event(a0, a1); c->r[2] = 0; break;
        case 0x08: c->r[2] = open_event(a0, a1, a2, a3); break;
        case 0x09: if ((e = ev_get(a0))) e->status = EV_FREE; c->r[2] = 1; break;
        case 0x0B:                                                               // TestEvent
            e = ev_get(a0);
            if (e && e->status == EV_READY) { e->status = EV_ENABLED; c->r[2] = 1; }
            else c->r[2] = 0;
            break;
        case 0x0C: if ((e = ev_get(a0))) e->status = EV_ENABLED; c->r[2] = 1; break;
        case 0x0D: if ((e = ev_get(a0))) e->status = EV_DISABLED; c->r[2] = 1; break;
        case 0x10: LOGE("ChangeThread(%08x) - not supported", a0); c->r[2] = 1; break;
        case 0x12:                                                               // InitPad
            LOGD("InitPad(%08x, %x, %08x, %x)", a0, a1, a2, a3);
            pad_buf[0] = a0; pad_size[0] = a1; pad_buf[1] = a2; pad_size[1] = a3;
            c->r[2] = 2;
            break;
        case 0x13: pad_started = 1; c->r[2] = 1; break;                          // StartPad
        case 0x14: pad_started = 0; break;                                       // StopPad
        case 0x15: old_pad_dest = a1; pad_started = 1; c->r[2] = 2; break;       // OutdatedPadInitAndStart
        case 0x16: c->r[2] = ~(uint32_t)platform_game_buttons(0) & 0xFFFF; break;  // OutdatedPadGetButtons
        case 0x17:                                                               // ReturnFromException
            if (exc_active) longjmp(exc_return, 1);
            LOGE("ReturnFromException outside an exception");
            break;
        case 0x18: custom_exit = 0; break;
        case 0x19: custom_exit = a0; LOGD("SetCustomExitFromException(%08x)", a0); break;
        case 0x32: c->r[2] = (uint32_t)file_open(a0, a1); break;
        case 0x33: {                                                             // FileSeek(fd, offset, whence)
            Fd* fd = fd_get(a0);
            if (!fd || fseek(fd->f, (long)(int32_t)a1, a2 == 1 ? SEEK_CUR : a2 == 2 ? SEEK_END : SEEK_SET)) { c->r[2] = 0xFFFFFFFF; break; }
            c->r[2] = (uint32_t)ftell(fd->f);
            break;
        }
        case 0x34: {                                                             // FileRead(fd, dst, len)
            Fd* fd = fd_get(a0);
            if (!fd) { c->r[2] = 0xFFFFFFFF; break; }
            uint8_t* tmp = malloc(a2);
            size_t n = fread(tmp, 1, a2, fd->f);
            for (size_t i = 0; i < n; i++) mem_w8(a1 + i, tmp[i]);
            free(tmp);
            c->r[2] = (uint32_t)n;
            break;
        }
        case 0x35: {                                                             // FileWrite(fd, src, len)
            if (a0 <= 1) {                                                       // stdout: the game's debug TTY
                static char line[512];
                static int nl;
                for (uint32_t i = 0; i < a2; i++) {
                    char ch = (char)mem_r8(a1 + i);
                    if (ch == '\n' || nl >= 510) {
                        line[nl] = 0;
                        if (g_log_level >= 1) fprintf(stderr, "[tty] %s\n", line);
                        if (strstr(line, "time out") || strstr(line, "rror")) trace_dump(line);
                        nl = 0;
                    }
                    else if (ch != '\r') line[nl++] = ch;
                }
                c->r[2] = a2;
                break;
            }
            Fd* fd = fd_get(a0);
            if (!fd) { c->r[2] = 0xFFFFFFFF; break; }
            uint8_t* tmp = malloc(a2);
            for (uint32_t i = 0; i < a2; i++) tmp[i] = (uint8_t)mem_r8(a1 + i);
            size_t n = fwrite(tmp, 1, a2, fd->f);
            fflush(fd->f);
            LOGI("FileWrite(fd %u, %u bytes) -> %zu", a0, a2, n);
            free(tmp);
            c->r[2] = (uint32_t)n;
            break;
        }
        case 0x36: { Fd* fd = fd_get(a0); if (fd) { fclose(fd->f); fd->used = 0; } c->r[2] = a0; break; }   // FileClose
        case 0x41: c->r[2] = 1; break;                                           // FormatDevice
        case 0x42: {                                                             // firstfile(pattern, de)
            char pat[64];
            read_cstr(a0, pat, sizeof pat);
            find_port = card_port(pat) == 1;
            const char* colon = strchr(pat, ':');
            snprintf(find_pattern, sizeof find_pattern, "%s", colon ? colon + 1 : pat);
            find_index = 0;
            c->r[2] = find_next(a1);
            break;
        }
        case 0x43: c->r[2] = find_next(a0); break;                               // nextfile
        case 0x45: {                                                             // FileDelete
            char name[64], path[700];
            read_cstr(a0, name, sizeof name);
            const char* colon = strchr(name, ':');
            snprintf(path, sizeof path, "%s/%s", card_dir[card_port(name) == 1], colon ? colon + 1 : name);
            c->r[2] = remove(path) == 0;
            break;
        }
        case 0x4A: case 0x4B: case 0x4C: case 0x50: case 0x5B: c->r[2] = 1; break;
        case 0x4E: card_pending[a0 >> 4 & 1] = 1; c->r[2] = 1; break;            // write_card_sector
        case 0x51: c->r[2] = 0xFFFFFFFF; break;                                  // Krom2RawAdd (no font ROM)
        case 0x56: c->r[2] = 0x80000674; break;                                  // GetC0Table
        case 0x57: c->r[2] = 0x80000874; break;                                  // GetB0Table
        default: LOGE("B0:%02X not implemented (ra=%08x)", fn, c->r[31]); c->r[2] = 0;
    }
}

static void bios_c0(CPU* c, uint32_t fn) {
    uint32_t a0 = c->r[4], a1 = c->r[5];
    switch (fn) {
        case 0x02: {                                                             // SysEnqIntRP(prio, entry)
            uint32_t p = a0 & 3;
            mem_w32(a1, chains[p]);
            chains[p] = a1;
            LOGD("SysEnqIntRP(%u, %08x) handler=%08x verifier=%08x", p, a1, mem_r32(a1 + 4), mem_r32(a1 + 8));
            c->r[2] = 0;
            break;
        }
        case 0x03: {                                                             // SysDeqIntRP
            uint32_t p = a0 & 3, prev = 0, e = chains[p];
            while (e) {
                if (e == a1) {
                    if (prev) mem_w32(prev, mem_r32(e)); else chains[p] = mem_r32(e);
                    break;
                }
                prev = e;
                e = mem_r32(e);
            }
            c->r[2] = 0;
            break;
        }
        case 0x0A: {                                                             // ChangeClearRCnt(t, flag)
            int t = a0 & 3;
            c->r[2] = (uint32_t)clear_rcnt[t];
            clear_rcnt[t] = (int)a1;
            break;
        }
        default: LOGE("C0:%02X not implemented (ra=%08x)", fn, c->r[31]); c->r[2] = 0;
    }
}

void bios_call(CPU* c, uint32_t vector, uint32_t fn) {
    g_c = c;
    LOGT("BIOS %02X:%02X a0=%08x a1=%08x ra=%08x", vector, fn, c->r[4], c->r[5], c->r[31]);
    if (vector == 0xA0) bios_a0(c, fn);
    else if (vector == 0xB0) bios_b0(c, fn);
    else bios_c0(c, fn);
}

void bios_vblank(CPU* c) {
    g_c = c;
    if (pad_started) pad_fill();
    for (int port = 0; port < 2; port++) {
        if (card_pending[port]) {
            card_pending[port] = 0;
            // software card events (SwCARD): "end of I/O"
            deliver_event(0xF0000011u, 0x0004);
            deliver_event(0xF4000001u, 0x0004);
        }
    }
}

void bios_set_card_dir(const char* dir) {
    snprintf(card_dir[0], sizeof card_dir[0], "%s", dir);
    snprintf(card_dir[1], sizeof card_dir[1], "%s_2", dir);
}

// Kernel function tables (A0 at 0x200, C0 at 0x674, B0 at 0x874 like the real BIOS). Each entry points
// to a "thunk" address the runtime turns back into a native BIOS call; programs may read or patch them.
#define THUNK_BASE 0x80001000u
void bios_init(CPU* c) {
    g_c = c;
    for (uint32_t n = 0; n < 0xC0; n++) mem_w32(0x80000200 + n * 4, THUNK_BASE + 0x000 + n * 4);
    for (uint32_t n = 0; n < 0x20; n++) mem_w32(0x80000674 + n * 4, THUNK_BASE + 0x800 + n * 4);
    for (uint32_t n = 0; n < 0x60; n++) mem_w32(0x80000874 + n * 4, THUNK_BASE + 0x400 + n * 4);
    c->cop0[12] = 0;   // interrupts off until the program enables them
    for (int p = 0; p < 2; p++) {
        if (!card_dir[p][0]) {
            const char* home = getenv("HOME");
            snprintf(card_dir[p], sizeof card_dir[p], "%s/.local/share/ac2native/card%d", home ? home : ".", p);
        }
        char cmd[600];
        snprintf(cmd, sizeof cmd, "mkdir -p '%s'", card_dir[p]);
        if (system(cmd) != 0) LOGE("cannot create %s", card_dir[p]);
    }
}
