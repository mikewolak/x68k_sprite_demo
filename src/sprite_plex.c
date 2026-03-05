////////////////////////////////////////////////////////////////////////////////
// sprite_plex.c — X68000 128-sprite PCG multiplexer demo (C99 port)
//
// Compile-time resolution via DEMO_RES (set by Makefile):
//   DEMO_RES=256: 7x7 grid = 49 sprites, 256x256 31kHz
//   DEMO_RES=512: 15x8 grid = 120 sprites, 512x512 31kHz
//
// Sprite slot layout:
//   0 .. PLEX_TOTAL-1  : animated plex grid
//   120 .. 123         : HUD — 4 hex digits showing sprite count
//   all others         : hidden at init
//
// PCG pattern layout:
//   0-127  : sprite plex patterns (0-63 stable, 64-127 swapped every ~4s)
//   128-144: hex font (17 chars: '0'-'9', 'A'-'F', 'x')
//
// Animation (per frame, per sprite):
//   angle = (frame_lo2 + col*13 + row*23) & 0xFF
//   X = BASE_X + col*GRID_STEP + sint[angle]
//   Y = BASE_Y + row*GRID_STEP + sint[(angle+64)&0xFF]
//   ctrl = (slot + pat_counter) & 0x7F  — shifts colour diagonally
////////////////////////////////////////////////////////////////////////////////

#include "types.h"
#include "x68k_hw.h"
#include "x68k_video.h"

////////////////////////////////////////////////////////////////////////////////
// Grid layout — resolved at compile time from DEMO_RES
////////////////////////////////////////////////////////////////////////////////
#define GRID_STEP   32      // pixel spacing between grid positions
#define FONT_STEP   16      // pixel spacing between font character sprites
#define FONT_REG_Y  18      // sprite reg Y for HUD row (screen y=2)

#if DEMO_RES == 256
// 256x256: 7x7 grid = 49 sprites
// Centre: BASE_screen = display/2 - (cols-1)*step/2 = 128-96 = 32 -> reg = 32+16 = 48
#define PLEX_COLS   7
#define PLEX_ROWS   7
#define PLEX_TOTAL  49
#define BASE_X      48
#define BASE_Y      48
#define FONT_REG_X  208     // flush right: screen x=192, reg x=208
#elif DEMO_RES == 512
// 512x512: 15x8 grid = 120 sprites
// X: 256 - 14*32/2 = 32 -> reg = 48; Y: 256 - 7*32/2 = 144 -> reg = 160
#define PLEX_COLS   15
#define PLEX_ROWS   8
#define PLEX_TOTAL  120
#define BASE_X      48
#define BASE_Y      160
#define FONT_REG_X  448     // flush right: screen x=432, reg x=448
#else
#error "Unknown DEMO_RES. Build with DEMO_RES=256x256 or DEMO_RES=512x512"
#endif

#define PCG_FONT        128     // PCG slot for first font pattern ('0')
#define FONT_CHARS      17      // '0'-'9', 'A'-'F', 'x'
#define PAL_PLEX        0
#define PAL_FONT        1
#define PAL_FONT_BITS   0x0100  // palette 1 shifted into ctrl word bits 11:8
#define SWAP_FRAMES     216     // ~4 s at ~54 Hz

////////////////////////////////////////////////////////////////////////////////
// Sprite attribute RAM layout: 8 bytes per slot, 128 slots at SP_ATTR_RAM
////////////////////////////////////////////////////////////////////////////////
typedef struct {
    uint16_t x;
    uint16_t y;
    uint16_t ctrl;   // bits 11:8 = palette, bits 7:0 = PCG pattern index
    uint16_t prio;
} sprite_t;

#define SPRITES ((volatile sprite_t *)SP_ATTR_RAM)

////////////////////////////////////////////////////////////////////////////////
// External data symbols (defined in data/*.s assembled files)
////////////////////////////////////////////////////////////////////////////////
extern const uint16_t sprite_palette_grb555[16];
extern const uint8_t  sprite_patterns[];          // 256 patterns x 128 bytes
extern const uint16_t font_palette_grb555[16];
extern const uint8_t  font_16x16_hex_data[];      // 17 patterns x 128 bytes

// Batch swap source: which 64 patterns load into PCG slots 64-127 each cycle
// Batch 0 -> source patterns 128-191
// Batch 1 -> source patterns 192-255
// Batch 2 -> source patterns  64-127
static const uint8_t * const swap_src_table[3] = {
    sprite_patterns + 128 * 128,
    sprite_patterns + 192 * 128,
    sprite_patterns +  64 * 128,
};

////////////////////////////////////////////////////////////////////////////////
// Sine table — 256 entries, signed bytes, amplitude ~20
// Each sprite orbits its grid position in a circle; phase offset by (col,row)
// produces a ripple-wave effect across the grid.
////////////////////////////////////////////////////////////////////////////////
static const int8_t sine_table[256] = {
     0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7,
     8, 8, 9, 9, 9,10,10,11,11,12,12,12,13,13,13,14,
    14,14,15,15,15,16,16,16,17,17,17,17,18,18,18,18,
    18,19,19,19,19,19,19,20,20,20,20,20,20,20,20,20,
    20,20,20,20,20,20,20,20,20,20,19,19,19,19,19,19,
    18,18,18,18,18,17,17,17,17,16,16,16,15,15,15,14,
    14,14,13,13,13,12,12,12,11,11,10,10, 9, 9, 9, 8,
     8, 7, 7, 6, 6, 5, 5, 4, 4, 3, 3, 2, 2, 1, 1, 0,
     0, 0,-1,-1,-2,-2,-3,-3,-4,-4,-5,-5,-6,-6,-7,-7,
    -8,-8,-9,-9,-9,-10,-10,-11,-11,-12,-12,-12,-13,-13,-13,-14,
    -14,-14,-15,-15,-15,-16,-16,-16,-17,-17,-17,-17,-18,-18,-18,-18,
    -18,-19,-19,-19,-19,-19,-19,-20,-20,-20,-20,-20,-20,-20,-20,-20,
    -20,-20,-20,-20,-20,-20,-20,-20,-20,-20,-19,-19,-19,-19,-19,-19,
    -18,-18,-18,-18,-18,-17,-17,-17,-17,-16,-16,-16,-15,-15,-15,-14,
    -14,-14,-13,-13,-13,-12,-12,-12,-11,-11,-10,-10,-9,-9,-9,-8,
    -8,-7,-7,-6,-6,-5,-5,-4,-4,-3,-3,-2,-2,-1,-1, 0,
};

// Main loop state
static uint32_t frame_counter;
static uint16_t swap_countdown;
static uint16_t swap_batch;
static uint16_t pat_counter;
static uint8_t  pat_tick;       // counts 0-2; pat_counter++ every 3 frames

////////////////////////////////////////////////////////////////////////////////
// load_palette — copy 16 GRB555 words to sprite palette bank (0-15)
////////////////////////////////////////////////////////////////////////////////
static void load_palette(const uint16_t *src, int bank)
{
    volatile uint16_t *dst = (volatile uint16_t *)(SP_PAL_RAM + (uint32_t)(bank & 0xF) * 32);
    int i;
    for (i = 0; i < 16; i++)
        dst[i] = src[i];
}

////////////////////////////////////////////////////////////////////////////////
// load_to_pcg — copy count patterns (128 bytes each) from src into PCG RAM
// starting at slot start_pcg. Byte-by-byte to match PCG write requirements.
////////////////////////////////////////////////////////////////////////////////
static void load_to_pcg(const uint8_t *src, int start_pcg, int count)
{
    volatile uint8_t *dst = (volatile uint8_t *)(SP_PAT_RAM + (uint32_t)(start_pcg & 0xFF) * 128);
    int total = count * 128;
    int i;
    for (i = 0; i < total; i++)
        dst[i] = src[i];
}

////////////////////////////////////////////////////////////////////////////////
// update_all_sprites — reposition all PLEX_TOTAL sprites for this frame
//
// For each sprite (slot):
//   angle = (frame_lo2 + col*13 + row*23) & 0xFF
//   X = BASE_X + col*GRID_STEP + sine[angle]
//   Y = BASE_Y + row*GRID_STEP + sine[(angle+64)&0xFF]
//   ctrl = (slot + pat_counter) & 0x7F   (colour wave shift)
////////////////////////////////////////////////////////////////////////////////
static void update_all_sprites(void)
{
    volatile sprite_t *sa = SPRITES;
    uint8_t  frame_lo2 = (uint8_t)(frame_counter * 2);
    uint16_t pat_base  = pat_counter & 0x7F;
    uint16_t bx        = BASE_X;
    uint16_t by        = BASE_Y;
    uint16_t col_phase = 0;   // accumulated col * 13
    uint16_t row_phase = 0;   // accumulated row * 23
    uint16_t col       = 0;
    int slot;

    for (slot = 0; slot < PLEX_TOTAL; slot++) {
        uint8_t angle = (uint8_t)(frame_lo2 + col_phase + row_phase);
        int8_t  dx    = sine_table[angle];
        int8_t  dy    = sine_table[(uint8_t)(angle + 64)];

        sa[slot].x    = (uint16_t)((int16_t)bx + dx);
        sa[slot].y    = (uint16_t)((int16_t)by + dy);
        sa[slot].ctrl = (uint16_t)((slot + pat_base) & 0x7F);
        sa[slot].prio = 0x0003;

        col++;
        bx        = (uint16_t)(bx + GRID_STEP);
        col_phase = (uint16_t)(col_phase + 13);

        if (col >= PLEX_COLS) {
            col       = 0;
            bx        = BASE_X;
            col_phase = 0;
            by        = (uint16_t)(by + GRID_STEP);
            row_phase = (uint16_t)(row_phase + 23);
        }
    }
}

////////////////////////////////////////////////////////////////////////////////
// swap_sprite_batch — CPU-copy 64 patterns into PCG slots 64-127
// Uses longword copies (64*128/4 = 2048 longwords) for speed.
////////////////////////////////////////////////////////////////////////////////
static void swap_sprite_batch(void)
{
    const uint32_t  *src = (const uint32_t *)swap_src_table[swap_batch];
    volatile uint32_t *dst = (volatile uint32_t *)(SP_PAT_RAM + 64 * 128);
    int i;
    for (i = 0; i < 64 * 128 / 4; i++)
        dst[i] = src[i];
}

////////////////////////////////////////////////////////////////////////////////
// update_font_display — write ctrl words for HUD sprites 120-123
// Displays PLEX_TOTAL as 4 hex digits (e.g. 120 = 0x0078 -> "0078")
////////////////////////////////////////////////////////////////////////////////
static void update_font_display(void)
{
    volatile sprite_t *sa = SPRITES + 120;
    uint16_t val = PLEX_TOTAL;
    sa[0].ctrl = (uint16_t)(PAL_FONT_BITS | (PCG_FONT + ((val >> 12) & 0xF)));
    sa[1].ctrl = (uint16_t)(PAL_FONT_BITS | (PCG_FONT + ((val >>  8) & 0xF)));
    sa[2].ctrl = (uint16_t)(PAL_FONT_BITS | (PCG_FONT + ((val >>  4) & 0xF)));
    sa[3].ctrl = (uint16_t)(PAL_FONT_BITS | (PCG_FONT + ((val      ) & 0xF)));
}

////////////////////////////////////////////////////////////////////////////////
// init_sprite_plex — one-time hardware setup
////////////////////////////////////////////////////////////////////////////////
void init_sprite_plex(void)
{
    volatile sprite_t *sa = SPRITES;
    int i;

    // Clear all 128 sprite slots to off-screen
    for (i = 0; i < 128; i++) {
        sa[i].x    = 0x03FF;
        sa[i].y    = 0x03FF;
        sa[i].ctrl = 0;
        sa[i].prio = 0;
    }

    // Load palettes
    load_palette(sprite_palette_grb555, PAL_PLEX);
    load_palette(font_palette_grb555,   PAL_FONT);

    // Load PCG: sprite patterns 0-127, then font patterns at PCG_FONT
    load_to_pcg(sprite_patterns,     0,        128);
    load_to_pcg(font_16x16_hex_data, PCG_FONT, FONT_CHARS);

    // Position HUD sprites 120-123 at top-right, showing '0' initially
    for (i = 0; i < 4; i++) {
        sa[120 + i].x    = (uint16_t)(FONT_REG_X + i * FONT_STEP);
        sa[120 + i].y    = FONT_REG_Y;
        sa[120 + i].ctrl = (uint16_t)(PAL_FONT_BITS | PCG_FONT);  // '0'
        sa[120 + i].prio = 0x0003;
    }
}

////////////////////////////////////////////////////////////////////////////////
// sprite_plex_loop — main demo loop (never returns)
////////////////////////////////////////////////////////////////////////////////
void sprite_plex_loop(void)
{
    frame_counter  = 0;
    swap_countdown = SWAP_FRAMES;
    swap_batch     = 0;
    pat_counter    = 0;
    pat_tick       = 0;

    for (;;) {
        wait_vblank();
        frame_counter++;

        // Advance colour-wave counter every 3 frames (avoids 32-bit modulo / libgcc BSR.L)
        if (++pat_tick >= 3) {
            pat_tick = 0;
            pat_counter++;
        }

        // Batch swap countdown (~4 s)
        if (--swap_countdown == 0) {
            swap_countdown = SWAP_FRAMES;
            if (++swap_batch >= 3)
                swap_batch = 0;
            swap_sprite_batch();
        }

        update_all_sprites();
        update_font_display();
    }
}
