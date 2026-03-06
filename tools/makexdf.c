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

////////////////////////////////////////////////////////////////////////////////
// XDF: 77 cylinders x 2 heads x 8 sectors x 1024 bytes = 1,261,568 bytes.
//
// The loader reads 8192 bytes (one full side) per _B_READ call, alternating
// head 0 and head 1, then advancing the cylinder.  The XDF image is laid out
// as sequential half-tracks:
//
//   [0    .. 8191 ] = cyl 0, head 0  (sectors 1-8)
//   [8192 .. 16383] = cyl 0, head 1  (sectors 1-8)
//   [16384.. 24575] = cyl 1, head 0
//   ...
//
// This means the flat binary maps directly onto the image with no interleaving.
// chunk i of the binary (offset i*8192) lands at image offset i*8192, which is
// exactly where the BIOS delivers it at load time.  No memmove needed.
////////////////////////////////////////////////////////////////////////////////

enum { SIZE_XDF      = 1261568 };   // 77 * 2 * 8 * 1024
enum { XDF_HALF_TRACK = 8192   };   // bytes loaded per _B_READ call

unsigned char image[SIZE_XDF];

////////////////////////////////////////////////////////////////////////////////
// print_disk_map — ASCII track map showing half-track usage across the XDF.
//
// Layout: 4-column grid, each row = one cylinder, each cell = one head (side).
//   # = half-track contains payload data
//   . = half-track is free
//
// Total width: 57 characters.
//   SEP  "+-----+---+---+..." 4 groups of 14 chars + trailing + = 57
//   TOP  "+-------...-------+" = 1 + 55 dashes + 1 = 57
//   Info "|  %-53s|"          = 1 + 2 + 53 + 1 = 57
//   Data "|" + 4 × " %3d | %s | %s |" = 1 + 56 = 57
////////////////////////////////////////////////////////////////////////////////

static void print_disk_map(const char *outfile, uint32_t used_size)
{
    const int TOTAL_CYLS = 77;
    const int TOTAL_HALF = 154;   /* 77 cyls * 2 heads */
    const int NROWS      = 20;    /* ceil(77 / 4) rows in the 4-col grid */

    uint32_t used_half = used_size / XDF_HALF_TRACK;
    uint32_t free_half = (uint32_t)TOTAL_HALF - used_half;
    uint32_t used_kb   = used_size / 1024;
    uint32_t free_kb   = (SIZE_XDF - used_size) / 1024;

    const char *name = strrchr(outfile, '/');
    name = name ? name + 1 : outfile;

    char buf[80];

    /* Is half-track (cyl, head) within the used region? */
#define HT(c,h)  (((uint32_t)((c)*2+(h)) < used_half) ? "#" : ".")

#define SEP "+-----+---+---+-----+---+---+-----+---+---+-----+---+---+"
#define TOP "+-------------------------------------------------------+"

    printf("\n");
    printf(TOP "\n");

    snprintf(buf, sizeof(buf), "XDF DISK MAP  --  %s", name);
    printf("|  %-53s|\n", buf);

    snprintf(buf, sizeof(buf),
             "77 cyls x 2 heads x 8 KB/side = 1,232 KB capacity");
    printf("|  %-53s|\n", buf);

    snprintf(buf, sizeof(buf), "# = 8 KB half-track used    . = free");
    printf("|  %-53s|\n", buf);

    printf(SEP "\n");
    printf("| CYL | 0 | 1 | CYL | 0 | 1 | CYL | 0 | 1 | CYL | 0 | 1 |\n");
    printf(SEP "\n");

    for (int row = 0; row < NROWS; row++) {
        printf("|");
        for (int col = 0; col < 4; col++) {
            int cyl = col * NROWS + row;
            if (cyl < TOTAL_CYLS)
                printf(" %3d | %s | %s |", cyl, HT(cyl, 0), HT(cyl, 1));
            else
                printf("     |   |   |");
        }
        printf("\n");
    }

    printf(SEP "\n");

    snprintf(buf, sizeof(buf), "Used: %3u / %3u half-tracks  %4u KB  %5.1f%%",
             used_half, TOTAL_HALF, used_kb,
             (used_half * 100.0) / TOTAL_HALF);
    printf("|  %-53s|\n", buf);

    snprintf(buf, sizeof(buf), "Free: %3u / %3u half-tracks  %4u KB  %5.1f%%",
             free_half, TOTAL_HALF, free_kb,
             (free_half * 100.0) / TOTAL_HALF);
    printf("|  %-53s|\n", buf);

    printf(TOP "\n");
    printf("\n");

#undef HT
#undef SEP
#undef TOP
}

////////////////////////////////////////////////////////////////////////////////

int main(int argc, char** argv) {
    if(argc != 3) {
        printf("usage: %s infile outfile\n", argv[0]);
        return 1;
    }

    memset(image, 0, sizeof(image));

    FILE* f = fopen(argv[1], "rb");
    if(!f) { perror(argv[1]); return 1; }
    uint32_t binsize = fread(image, 1, sizeof(image), f);
    fclose(f);

    if(binsize > SIZE_XDF) {
        printf("binary (%u bytes) exceeds XDF capacity (%u bytes)\n",
               binsize, SIZE_XDF);
        return 1;
    }

    // Patch the BPB (bytes 2..0x1F) with the XDF disk parameter block.
    // Byte 0x15 = media ID 0xFE (retained for reference; loader no longer
    // reads it — disk geometry is now hardcoded for XDF).
    memmove(image + 2, id_xdf + 2, 0x1e);

    f = fopen(argv[2], "wb");
    if(!f) { perror(argv[2]); return 1; }

    // Pad to 8KB boundary (matches loader's half-track chunk size).
    binsize  = (binsize + (XDF_HALF_TRACK - 1)) & ~(uint32_t)(XDF_HALF_TRACK - 1);

    uint32_t i;
    uint32_t sum;

    // Compute checksum patch at offset 0x40 so that the forward checksum
    // over the padded binary totals zero.  Same algorithm as the boot sector
    // checksum loop in start.s.
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

    // Verify: forward pass should produce zero.
    sum = 0;
    for(i = 0; i < binsize; i += 4) {
        sum += get32msb(image + i);
        sum = (sum << 1) | (sum >> 31);
    }
    if(sum != 0) {
        printf("ERROR: checksum mismatch (got 0x%08X)\n", sum);
        fclose(f);
        return 1;
    }

    // The flat binary is already correctly positioned in the image buffer:
    // sequential half-tracks in the binary match sequential half-tracks in
    // the XDF image layout.  Write the full image.
    fwrite(image, 1, SIZE_XDF, f);
    fclose(f);

    print_disk_map(argv[2], binsize);

    return 0;
}

////////////////////////////////////////////////////////////////////////////////
