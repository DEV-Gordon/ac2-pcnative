// Minimal ISO9660 reader on top of the raw MODE2/2352 disc image (root directory only).
#include "hw.h"
#include <stdlib.h>
#include <string.h>

static FILE* disc;

int iso_open(const char* path) {
    disc = fopen(path, "rb");
    return disc != NULL;
}

int iso_read_raw(int lba, uint8_t* out2352) {
    if (!disc) return 0;
    fseek(disc, (long)lba * 2352, SEEK_SET);
    return fread(out2352, 1, 2352, disc) == 2352;
}

static int read2048(int lba, uint8_t* out) {
    uint8_t raw[2352];
    if (!iso_read_raw(lba, raw)) return 0;
    memcpy(out, raw + 24, 2048);
    return 1;
}

// Finds a file in the root directory: returns its LBA (or -1) and size.
int iso_find(const char* name, uint32_t* size) {
    uint8_t s[2048];
    if (!read2048(16, s) || memcmp(s + 1, "CD001", 5)) return -1;
    uint32_t root_lba, root_size;
    memcpy(&root_lba, s + 156 + 2, 4);
    memcpy(&root_size, s + 156 + 10, 4);
    for (uint32_t sec = 0; sec * 2048 < root_size; sec++) {
        if (!read2048((int)(root_lba + sec), s)) return -1;
        for (int off = 0; off < 2048;) {
            int len = s[off];
            if (!len) break;
            int nlen = s[off + 32];
            char fname[64] = {0};
            memcpy(fname, s + off + 33, nlen < 63 ? nlen : 63);
            char* semi = strchr(fname, ';');
            if (semi) *semi = 0;
            if (!strcmp(fname, name)) {
                uint32_t lba;
                memcpy(&lba, s + off + 2, 4);
                if (size) memcpy(size, s + off + 10, 4);
                return (int)lba;
            }
            off += len;
        }
    }
    return -1;
}

// Reads a whole file (malloc'd).
uint8_t* iso_read_file(const char* name, uint32_t* size) {
    uint32_t sz;
    int lba = iso_find(name, &sz);
    if (lba < 0) return NULL;
    uint8_t* data = malloc(sz + 2048);
    for (uint32_t i = 0; i * 2048 < sz; i++) read2048(lba + (int)i, data + i * 2048);
    if (size) *size = sz;
    return data;
}
