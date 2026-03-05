////////////////////////////////////////////////////////////////////////////////

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

////////////////////////////////////////////////////////////////////////////////

static uint32_t get32msb(const unsigned char* c) {
    return
        ((((uint32_t)(c[0])) & 0xFF) << 24) |
        ((((uint32_t)(c[1])) & 0xFF) << 16) |
        ((((uint32_t)(c[2])) & 0xFF) <<  8) |
        ((((uint32_t)(c[3])) & 0xFF) <<  0);
}

static void put32msb(unsigned char* c, uint32_t value) {
    c[0] = (unsigned char)(value >> 24);
    c[1] = (unsigned char)(value >> 16);
    c[2] = (unsigned char)(value >>  8);
    c[3] = (unsigned char)(value >>  0);
}

////////////////////////////////////////////////////////////////////////////////

static const unsigned char id_xdf[] = {
0x60,0x3C,0x90,0x58,0x36,0x38,0x49,0x50,0x4C,0x33,0x30,0x00,0x04,0x01,0x01,0x00,
0x02,0xC0,0x00,0xD0,0x04,0xFE,0x02,0x00,0x08,0x00,0x02,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x20,0x20,0x20,0x20,0x20,
0x20,0x20,0x20,0x20,0x20,0x20,0x46,0x41,0x54,0x31,0x32,0x20,0x20,0x20
};

static const unsigned char id_2hq[] = {
0x60,0x3C,0x90,0x58,0x36,0x38,0x49,0x50,0x4C,0x33,0x30,0x00,0x02,0x01,0x01,0x00,
0x02,0xE0,0x00,0x40,0x0B,0xF0,0x09,0x00,0x12,0x00,0x02,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x20,0x20,0x20,0x20,0x20,
0x20,0x20,0x20,0x20,0x20,0x20,0x46,0x41,0x54,0x31,0x32,0x20,0x20,0x20
};

enum { SIZE_XDF = 1261568 };
enum { SIZE_2HQ = 1474560 };

unsigned char image[SIZE_2HQ];

////////////////////////////////////////////////////////////////////////////////

static int extmatch(const char* str, const char* ext) {
    size_t sl = strlen(str);
    size_t el = strlen(ext);

    if(sl < (1 + el)) {
        return 0;
    }

    size_t t = sl - (1 + el);
    if(str[t] != '.') { return 0; }
    if(strcasecmp(str + t + 1, ext)) { return 0; }

    return 1;
}

////////////////////////////////////////////////////////////////////////////////

int main(int argc, char** argv) {
    if(argc != 3) {
        printf("usage: %s infile outfile\n", argv[0]);
        return 1;
    }

    memset(image, 0, sizeof(image));

    uint32_t disksize;
    uint32_t bytes_per_track;

    if(extmatch(argv[2], "xdf")) {
        disksize = SIZE_XDF;
        bytes_per_track = 2 * 1024 * 8;
    } else if(extmatch(argv[2], "2hq")) {
        disksize = SIZE_2HQ;
        bytes_per_track = 2 * 512 * 18;
    } else {
        printf("Unknown format for %s\n", argv[2]);
        return 1;
    }

    FILE* f = fopen(argv[1], "rb");
    if(!f) { perror(argv[1]); return 1; }
    uint32_t binsize = fread(image, 1, sizeof(image), f);
    fclose(f);

    if(binsize > 0x10000) {
        printf("bin size is too large...\n");
        return 1;
    }

    //
    // Patch disk ID and starting location based on size
    //
    if(disksize == SIZE_XDF) {
        memmove(image + 2, id_xdf + 2, 0x1e);
    } else if(disksize == SIZE_2HQ) {
        memmove(image + 2, id_2hq + 2, 0x1e);
    }

    f = fopen(argv[2], "wb");
    if(!f) { perror(argv[2]); return 1; }

    printf("binsize = 0x%08X\n", binsize);

    binsize +=  0xFFF;
    binsize &= ~0xFFF;

    uint32_t i;
    uint32_t sum;

    uint32_t patchpoint = 0x40;
    uint32_t sumatpatch = 0;
    for(i = 0; i < patchpoint; i += 4) {
        sumatpatch += get32msb(image + i);
        sumatpatch = (sumatpatch << 1) | (sumatpatch >> 31);
    }

    sum = 0;
    for(i = binsize;;) {
        i -= 4;
        sum = (sum >> 1) | (sum << 31);
        if(i == patchpoint) {
            put32msb(image + patchpoint, sum - sumatpatch);
            break;
        }
        sum -= get32msb(image + i);
    }

    sum = 0;
    for(i = 0; i < binsize; i += 4) {
        sum += get32msb(image + i);
        sum = (sum << 1) | (sum >> 31);
    }
    printf("sum = 0x%08X\n", sum);

    //
    // Interleave the binary data so it's 4K per track, on one side
    // (this is the only way I could mange to get it loading on both XDF and 2HQ)
    //
    i = binsize / 0x1000;
    while(--i) {
        memmove(image + (bytes_per_track * i), image + (0x1000 * i), 0x1000);
        memset (image + (0x1000 * i), 0, 0x1000);
    }

    fwrite(image, 1, disksize, f);

    fclose(f);

    return 0;
}

////////////////////////////////////////////////////////////////////////////////
