////////////////////////////////////////////////////////////////////////////////
// sprite_plex.c — X68000 128-sprite PCG multiplexer demo (C99 port)
//
// Compile-time resolution via DEMO_RES (set by Makefile):
//   DEMO_RES=256: 10x10 grid = 100 sprites, 256x256 31kHz
//   DEMO_RES=512: 15x8 grid = 120 sprites, 512x512 31kHz
//
// Demo phases (each 360 frames, alternating):
//   Phase 0: sine-wave ripple grid
//   Phase 1: sprites rotate around a Bresenham circle (r=90, 100 points)
//
// Sprite slot layout:
//   0 .. PLEX_TOTAL-1  : animated plex grid / circle
//   120 .. 123         : HUD — 4 hex digits showing sprite count
//   all others         : hidden at init
//
// PCG pattern layout:
//   0-127  : sprite plex patterns (loaded in unique-shape order)
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
#define FONT_STEP   16      // pixel spacing between font character sprites
#define FONT_REG_Y  18      // sprite reg Y for HUD row (screen y=2)

#if DEMO_RES == 256
// 256x256: 10x10 grid = 100 sprites, GRID_STEP=26
// margin=(256-9*26)/2=11 -> reg=11+16=27; covers screen_x/y 11-245
#define GRID_STEP   26
#define PLEX_COLS   10
#define PLEX_ROWS   10
#define PLEX_TOTAL  100
#define BASE_X      27
#define BASE_Y      27
#define FONT_REG_X  208     // flush right: screen x=192, reg x=208
#elif DEMO_RES == 512
// 512x512: 15x8 grid = 120 sprites (HW max with HUD at 120-123), GRID_STEP=32
// X: 256 - 14*32/2 = 32 -> reg = 48; Y: 256 - 7*32/2 = 144 -> reg = 160
#define GRID_STEP   32
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
#define PHASE_FRAMES    360     // frames per demo phase

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

////////////////////////////////////////////////////////////////////////////////
// Bresenham circle path — 100 points evenly sampled from a r=90 circle,
// sorted CCW from east. centre=(128,128), reg = screen+16 on each axis.
// Produced by the midpoint-circle algorithm in Python; no float in the demo.
////////////////////////////////////////////////////////////////////////////////
static const uint8_t circle_lut[100][2] = {  /* {reg_x, reg_y} */
    {234,144}, {234,138}, {233,133}, {233,128}, {232,123},
    {230,118}, {228,113}, {226,107}, {224,102}, {221, 97},
    {217, 92}, {214, 87}, {209, 82}, {204, 77}, {199, 73},
    {194, 69}, {189, 66}, {184, 63}, {179, 61}, {173, 59},
    {168, 57}, {163, 56}, {158, 55}, {153, 54}, {148, 54},
    {143, 54}, {138, 54}, {133, 55}, {128, 55}, {123, 56},
    {118, 58}, {113, 60}, {107, 62}, {102, 64}, { 97, 67},
    { 92, 71}, { 87, 74}, { 82, 79}, { 77, 84}, { 73, 89},
    { 69, 94}, { 66, 99}, { 63,104}, { 61,109}, { 59,115},
    { 57,120}, { 56,125}, { 55,130}, { 54,135}, { 54,140},
    { 54,145}, { 54,150}, { 55,155}, { 55,160}, { 56,165},
    { 58,170}, { 60,175}, { 62,181}, { 64,186}, { 67,191},
    { 71,196}, { 74,201}, { 79,206}, { 84,211}, { 89,215},
    { 94,219}, { 99,222}, {104,225}, {109,227}, {115,229},
    {120,231}, {125,232}, {130,233}, {135,234}, {140,234},
    {145,234}, {150,234}, {155,233}, {160,233}, {165,232},
    {170,230}, {175,228}, {181,226}, {186,224}, {191,221},
    {196,217}, {201,214}, {206,209}, {211,204}, {215,199},
    {219,194}, {222,189}, {225,184}, {227,179}, {229,173},
    {231,168}, {232,163}, {233,158}, {234,153}, {234,148},
};

// PCG load order: 50 unique-shape patterns first, then 78 colour variants.
// Analysis of sprite_patterns.s shows only 50 distinct pixel shapes across
// 256 patterns; loading unique shapes into PCG slots 0-49 ensures every
// on-screen sprite at pat_base=0 maps to a unique shape.
static const uint8_t pcg_init_order[128] = {
      0,  5, 10, 15, 20, 21, 27, 63, 68, 73, 78, 83, 84, 85, 92, 99,
    104,109,114,119,124,125,126,136,138,140,145,146,147,148,149,159,
    165,171,176,182,187,192,197,202,207,208,213,218,223,228,229,237,
    245,253,  1,  2,  3,  4,  6,  7,  8,  9, 11, 12, 13, 14, 16, 17,
     18, 19, 22, 23, 24, 25, 26, 28, 29, 30, 31, 32, 33, 34, 35, 36,
     37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52,
     53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 64, 65, 66, 67, 69, 70,
     71, 72, 74, 75, 76, 77, 79, 80, 81, 82, 86, 87, 88, 89, 90, 91,
};

// Main loop state
static uint32_t frame_counter;
static uint16_t swap_countdown;
static uint16_t swap_batch;
static uint16_t pat_counter;
static uint8_t  pat_tick;       // counts 0-2; pat_counter++ every 3 frames
static uint16_t phase_counter;  // counts down; phase switches at 0
static uint8_t  demo_phase;     // 0=grid ripple, 1=circle rotate
static uint8_t  circle_rot;     // 0-99 rotation index for circle mode

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
// update_circle_sprites — arrange all PLEX_TOTAL sprites around the
// Bresenham circle, offset by circle_rot for rotation each frame.
// No modulo: slot+rot <= 198 so one conditional subtract suffices.
////////////////////////////////////////////////////////////////////////////////
static void update_circle_sprites(void)
{
    volatile sprite_t *sa = SPRITES;
    uint16_t pat_base = pat_counter & 0x7F;
    uint8_t  rot      = circle_rot;
    int slot;

    for (slot = 0; slot < PLEX_TOTAL; slot++) {
        uint8_t pos = (uint8_t)(slot + rot);
        if (pos >= 100) pos = (uint8_t)(pos - 100);

        sa[slot].x    = circle_lut[pos][0];
        sa[slot].y    = circle_lut[pos][1];
        sa[slot].ctrl = (uint16_t)((slot + pat_base) & 0x7F);
        sa[slot].prio = 0x0003;
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

    // Load PCG: unique-shape patterns first (slots 0-49), colour variants after.
    // pcg_init_order maps each PCG slot to its source pattern for max diversity.
    for (i = 0; i < 128; i++)
        load_to_pcg(sprite_patterns + (uint32_t)pcg_init_order[i] * 128, i, 1);
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
    phase_counter  = PHASE_FRAMES;
    demo_phase     = 0;
    circle_rot     = 0;

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

        // Phase switch every PHASE_FRAMES frames
        if (--phase_counter == 0) {
            phase_counter = PHASE_FRAMES;
            demo_phase ^= 1;
            circle_rot = 0;
        }

        if (demo_phase == 0) {
            update_all_sprites();
        } else {
            // Advance circle rotation each frame (mod 100, no libgcc modulo)
            if (++circle_rot >= 100)
                circle_rot = 0;
            update_circle_sprites();
        }

        update_font_display();
    }
}
