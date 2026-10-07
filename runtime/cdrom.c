// CD-ROM controller: commands, sector reads from the user's .bin image, XA-ADPCM audio.
#include "hw.h"
#include <stdlib.h>
#include <string.h>

static FILE* disc;
static int disc_sectors;

// state left behind by the BIOS after booting the disc: interrupts enabled, motor on
static uint8_t index_reg, irq_enable = 0x1F, irq_flag, mode, stat = 0x02;
static uint8_t params[16], nparams;
static uint8_t resp[16], nresp, resp_pos;
static uint8_t sector[2352];
static uint8_t data_fifo[2352];
static int data_len, data_pos;
static int busy;
static int seek_lba, read_lba, reading, setloc_pending;
static int filter_file, filter_chan;
int g_xa_stats[3];                 // debug: XA sectors played / muted / filtered out
int g_xa_last_file, g_xa_last_chan;
int cd_filter(void) { return filter_file << 8 | filter_chan; }
static int sector_ready;          // a sector waits for INT1 acknowledge + request
static uint64_t int1_ack_time;
static int muted;
double g_cd_speed = 1.0;          // >1 = faster than the original drive

typedef struct { uint8_t type; uint8_t bytes[16]; uint8_t n; uint64_t at; } Pending;
static Pending queue[8];
static int nqueue;

static int bcd(int v) { return ((v / 10) << 4) | (v % 10); }
static int unbcd(int v) { return (v >> 4) * 10 + (v & 15); }

static void try_deliver(void);
static void deliver_event(void) { try_deliver(); }

static void push(uint8_t type, const uint8_t* b, int n, uint64_t delay) {
    if (nqueue >= 8) { LOGE("cd: response queue full"); return; }
    Pending* p = &queue[nqueue++];
    p->type = type;
    p->n = (uint8_t)n;
    memcpy(p->bytes, b, n);
    uint64_t now = time_cycles();
    uint64_t prev = nqueue > 1 ? queue[nqueue - 2].at : now;
    p->at = (prev > now ? prev : now) + delay;
    sched_add(p->at, deliver_event);
}

static void try_deliver(void) {
    if (irq_flag || !nqueue) return;
    if (queue[0].at > time_cycles()) { sched_add(queue[0].at, deliver_event); return; }
    Pending p = queue[0];
    memmove(queue, queue + 1, sizeof(Pending) * (--nqueue));
    memcpy(resp, p.bytes, p.n);
    nresp = p.n;
    resp_pos = 0;
    irq_flag = p.type;
    busy = 0;
    if (irq_flag & irq_enable) irq_raise(IRQ_CDROM);
}

static void stat1(uint8_t type, uint64_t delay) { push(type, &stat, 1, delay); }

// ------------------------------------------------------------------ sectors
static int read_raw(int lba, uint8_t* out) {
    if (lba < 0 || lba >= disc_sectors) return 0;
    fseek(disc, (long)lba * 2352, SEEK_SET);
    return fread(out, 1, 2352, disc) == 2352;
}

static void xa_decode(const uint8_t* s);
static void sector_event(void);

static uint64_t sector_period(void) {
    double p = PSX_CLOCK / ((mode & 0x80) ? 150.0 : 75.0);
    return (uint64_t)(p / g_cd_speed);
}

static int sector_log;
static void start_reading(void) {
    sector_log = 0;
    if (setloc_pending) { read_lba = seek_lba; setloc_pending = 0; }
    reading = 1;
    stat = (stat & ~0xC0) | 0x22;
    sched_add(time_cycles() + sector_period(), sector_event);
}

static void sector_event(void) {
    if (!reading) return;
    // Keep data intact: wait until the program has taken the previous sector. (The real drive would
    // overwrite it; with native timing that can make libcd's CdRead see a missing sector. The movie
    // streaming that relied on overwriting is now played by the native FMV player.)
    // The real drive overwrites unread sectors. We keep an acknowledged-but-unrequested sector for one
    // sector period (libcd's CdRead asks for it shortly after the ack) and then drop it, so streams with
    // data sectors nobody reads (XA music interleaves) keep flowing.
    if (irq_flag == 1 || (sector_ready && time_cycles() - int1_ack_time < sector_period())) {
        sched_add(time_cycles() + sector_period() / 8, sector_event);
        return;
    }
    sector_ready = 0;
    if (!read_raw(read_lba, sector)) {
        reading = 0;
        uint8_t e[2] = {(uint8_t)(stat | 1), 0x04};
        push(5, e, 2, 1000);
        return;
    }
    read_lba++;
    uint8_t submode = sector[18];
    {
        if (g_log_level >= 2 && sector_log < 12) {
            sector_log++;
            LOGD("cd sector lba=%d file=%d chan=%d submode=%02x ci=%02x mode=%02x", read_lba - 1, sector[16], sector[17], submode, sector[19], mode);
        }
    }
    if ((mode & 0x40) && (submode & 0x04)) {
        // XA-ADPCM: goes to the SPU, not to the program
        if (!(mode & 0x08) || (sector[16] == filter_file && sector[17] == filter_chan)) {
            if (!muted) { xa_decode(sector); g_xa_stats[0]++; } else g_xa_stats[1]++;
        } else {
            g_xa_stats[2]++;
            g_xa_last_file = sector[16]; g_xa_last_chan = sector[17];
        }
    } else {
        sector_ready = 1;
        stat1(1, 0);
    }
    sched_add(time_cycles() + sector_period(), sector_event);
}

static void load_data_fifo(void) {
    if (!sector_ready) { data_len = 0; data_pos = 0; return; }
    if (mode & 0x20) { memcpy(data_fifo, sector + 12, 2340); data_len = 2340; }
    else { memcpy(data_fifo, sector + 24, 2048); data_len = 2048; }
    data_pos = 0;
    sector_ready = 0;
}

// ------------------------------------------------------------------ commands
static void command(uint8_t cmd) {
    const uint64_t R1 = 50000;           // first response
    uint8_t b[16];
    busy = 1;
    LOGD("cd cmd %02x params %d [%02x %02x %02x]", cmd, nparams, params[0], params[1], params[2]);
    switch (cmd) {
        case 0x01: stat1(3, R1); break;                                   // Getstat
        case 0x02:                                                        // Setloc
            seek_lba = (unbcd(params[0]) * 60 + unbcd(params[1])) * 75 + unbcd(params[2]) - 150;
            setloc_pending = 1;
            stat1(3, R1);
            break;
        case 0x03: stat1(3, R1); break;                                   // Play (no audio tracks)
        case 0x06: case 0x1B:                                             // ReadN / ReadS
            stat1(3, R1);
            start_reading();
            break;
        case 0x07: stat1(3, R1); stat1(2, 200000); break;                 // MotorOn
        case 0x08:                                                        // Stop
            reading = 0; stat = 0x00;
            stat1(3, R1); stat = 0x02; stat1(2, 300000);
            break;
        case 0x09:                                                        // Pause
            stat1(3, R1);
            reading = 0; sector_ready = 0;
            stat &= ~0xE0;
            stat1(2, (mode & 0x80) ? 70000 : 140000);
            break;
        case 0x0A:                                                        // Init
            mode = 0; reading = 0; sector_ready = 0; stat = 0x02;
            stat1(3, R1); stat1(2, 100000);
            break;
        case 0x0B: muted = 1; stat1(3, R1); break;
        case 0x0C: muted = 0; stat1(3, R1); break;
        case 0x0D: filter_file = params[0]; filter_chan = params[1]; stat1(3, R1); break;
        case 0x0E: mode = params[0]; stat1(3, R1); break;
        case 0x0F: b[0] = stat; b[1] = mode; b[2] = 0; b[3] = (uint8_t)filter_file; b[4] = (uint8_t)filter_chan; push(3, b, 5, R1); break;
        case 0x10: memcpy(b, sector + 12, 8); push(3, b, 8, R1); break;   // GetlocL
        case 0x11: {                                                      // GetlocP
            int lba = read_lba > 0 ? read_lba - 1 : 0, rel = lba, abs_ = lba + 150;
            b[0] = 1; b[1] = 1;
            b[2] = (uint8_t)bcd(rel / 4500); b[3] = (uint8_t)bcd(rel / 75 % 60); b[4] = (uint8_t)bcd(rel % 75);
            b[5] = (uint8_t)bcd(abs_ / 4500); b[6] = (uint8_t)bcd(abs_ / 75 % 60); b[7] = (uint8_t)bcd(abs_ % 75);
            push(3, b, 8, R1);
            break;
        }
        case 0x13: b[0] = stat; b[1] = 1; b[2] = 1; push(3, b, 3, R1); break;   // GetTN
        case 0x14: {                                                      // GetTD
            int t = unbcd(params[0]);
            int lba = t == 0 ? disc_sectors + 150 : 150;
            b[0] = stat; b[1] = (uint8_t)bcd(lba / 4500); b[2] = (uint8_t)bcd(lba / 75 % 60);
            push(3, b, 3, R1);
            break;
        }
        case 0x15: case 0x16:                                             // SeekL / SeekP
            reading = 0; sector_ready = 0;
            read_lba = seek_lba; setloc_pending = 0;
            stat = (stat & ~0xE0) | 0x42;
            stat1(3, R1);
            stat = (stat & ~0xE0) | 0x02;
            stat1(2, 400000);
            break;
        case 0x19:                                                        // Test
            if (params[0] == 0x20) { uint8_t v[4] = {0x94, 0x09, 0x19, 0xC0}; push(3, v, 4, R1); }
            else stat1(3, R1);
            break;
        case 0x1A: {                                                      // GetID
            stat1(3, R1);
            uint8_t id[8] = {0x02, 0x00, 0x20, 0x00, 'S', 'C', 'E', 'A'};
            push(2, id, 8, 300000);
            break;
        }
        case 0x1E: stat1(3, R1); stat1(2, 500000); break;                 // ReadTOC
        default: {
            LOGE("cd: unknown command %02x", cmd);
            uint8_t e[2] = {(uint8_t)(stat | 1), 0x40};
            push(5, e, 2, R1);
        }
    }
    nparams = 0;
}

// ------------------------------------------------------------------ registers
uint8_t cd_read(uint32_t a) {
    switch (a & 3) {
        case 0:
            return index_reg | (nparams == 0 ? 0x08 : 0) | (nparams < 16 ? 0x10 : 0) |
                   (resp_pos < nresp ? 0x20 : 0) | (data_pos < data_len ? 0x40 : 0) | (busy ? 0x80 : 0);
        case 1: return resp_pos < nresp ? resp[resp_pos++] : 0;
        case 2: return data_pos < data_len ? data_fifo[data_pos++] : 0;
        default: return (index_reg & 1) ? (irq_flag | 0xE0) : (irq_enable | 0xE0);
    }
}

void cd_write(uint32_t a, uint8_t v) {
    LOGT("cd write %08x idx%d = %02x", a, index_reg, v);
    switch (((a & 3) << 2) | index_reg) {
        case 0x0: case 0x1: case 0x2: case 0x3: index_reg = v & 3; break;
        case 0x4: command(v); break;
        case 0x8: if (nparams < 16) params[nparams++] = v; break;
        case 0x9: irq_enable = v & 0x1F; break;
        case 0xC:                                                         // request register
            if (v & 0x80) { if (data_pos >= data_len) load_data_fifo(); }
            else { data_len = 0; data_pos = 0; }
            break;
        case 0xD:                                                         // interrupt acknowledge
            if (irq_flag == 1 && (v & 0x1F)) int1_ack_time = time_cycles();
            irq_flag &= ~(v & 0x1F);
            if (v & 0x40) nparams = 0;
            if (!irq_flag) { nresp = resp_pos = 0; try_deliver(); }
            break;
        default: break;                                                   // audio volume registers
    }
}

uint32_t cd_dma_read_word(void) {
    uint32_t w = 0;
    for (int i = 0; i < 4; i++) w |= (uint32_t)(data_pos < data_len ? data_fifo[data_pos++] : 0) << (8 * i);
    return w;
}

// ------------------------------------------------------------------ XA-ADPCM
static const int K0[4] = {0, 60, 115, 98}, K1[4] = {0, 0, -52, -55};
static int32_t xa_hist[2][2];
int xa_decode_sector(const uint8_t* s, int32_t hist[2][2], int16_t* out, int* rate);

static int16_t clamp16(int v) { return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }

static void xa_decode(const uint8_t* s) {
    int16_t out[4032 * 2];
    int rate;
    int n = xa_decode_sector(s, xa_hist, out, &rate);
    spu_xa_push(out, n, rate);
}

// Decodes one XA-ADPCM sector (raw 2352 bytes) into stereo frames; returns the frame count.
int xa_decode_sector(const uint8_t* s, int32_t xa_hist[2][2], int16_t* out, int* rate_out) {
    uint8_t ci = s[19];
    int stereo = ci & 1, rate = (ci & 4) ? 18900 : 37800, bits8 = (ci & 0x10) != 0;
    int nout = 0;
    *rate_out = rate;
    const uint8_t* data = s + 24;
    int units = bits8 ? 4 : 8;
    int16_t mono[18 * 8 * 28];
    int nmono = 0;
    int16_t lr[2][18 * 4 * 28];
    int nlr[2] = {0, 0};
    for (int g = 0; g < 18; g++) {
        const uint8_t* grp = data + g * 128;
        for (int u = 0; u < units; u++) {
            uint8_t hdr = grp[4 + u];
            int shift = hdr & 15, filt = (hdr >> 4) & 3;
            if (shift > 12) shift = 9;
            int ch = stereo ? (u & 1) : 0;
            for (int i = 0; i < 28; i++) {
                int v;
                if (bits8) v = (int8_t)grp[16 + u + i * 4] << 8;
                else {
                    uint8_t byte = grp[16 + (u >> 1) + i * 4];
                    v = (int16_t)(((u & 1) ? (byte >> 4) : (byte & 15)) << 12);
                }
                v >>= shift;
                v += (xa_hist[ch][0] * K0[filt] + xa_hist[ch][1] * K1[filt] + 32) >> 6;
                int16_t sample = clamp16(v);
                xa_hist[ch][1] = xa_hist[ch][0];
                xa_hist[ch][0] = sample;
                if (stereo) lr[ch][nlr[ch]++] = sample;
                else mono[nmono++] = sample;
            }
        }
    }
    if (stereo) {
        int n = nlr[0] < nlr[1] ? nlr[0] : nlr[1];
        for (int i = 0; i < n; i++) { out[nout * 2] = lr[0][i]; out[nout * 2 + 1] = lr[1][i]; nout++; }
    } else {
        for (int i = 0; i < nmono; i++) { out[nout * 2] = out[nout * 2 + 1] = mono[i]; nout++; }
    }
    return nout;
}

int cd_last_setloc(void) { return seek_lba; }   // start of the stream the program asked for

void cd_init(const char* path) {
    disc = fopen(path, "rb");
    if (!disc) { LOGE("cannot open disc image %s", path); exit(2); }
    fseek(disc, 0, SEEK_END);
    disc_sectors = (int)(ftell(disc) / 2352);
    LOGI("disc: %s (%d sectors)", path, disc_sectors);
}

void cd_debug(void) {
    LOGD("cd state: flag=%d enable=%02x queue=%d busy=%d reading=%d sector_ready=%d mode=%02x stat=%02x next=%llu now=%llu",
         irq_flag, irq_enable, nqueue, busy, reading, sector_ready, mode, stat,
         nqueue ? (unsigned long long)queue[0].at : 0ULL, (unsigned long long)time_cycles());
}
