////////////////////////////////////////////////////////////////////////////////
// sprite_plex.c — X68000 128-sprite PCG multiplexer demo (C99 port)
//
// Compile-time resolution via DEMO_RES (set by Makefile):
//   DEMO_RES=256: 10x10 grid = 100 sprites, 256x256 15kHz
//   DEMO_RES=512: 15x8 grid = 120 sprites, 512x512 31kHz
//
// Demo phases:
//   Phase 0 (360 frames): sine-wave ripple grid
//   Phase 1 (360 frames): sprites rotate around a Bresenham circle
//   Phase 2 (until done): decompose — circle keeps rotating while each
//     sprite is launched one-at-a-time, gliding 2px/frame to its grid
//     target; once all 100 sprites arrive, phase 0 resumes.
//
// Visible display calibration (256 mode, 15kHz progressive):
//   Full display exposes screen_y 0-239 (~240 lines visible).
//   Circle: centre screen(128,120) -> reg(144,136), r=64 -> screen_y 56-184.
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
#include "x68k_bg.h"

////////////////////////////////////////////////////////////////////////////////
// Grid layout — resolved at compile time from DEMO_RES
//
// Visible area calibration: screen_y 0-239 visible in 256 mode (15kHz progressive).
// Grid fits 10 rows in 240 lines: GRID_STEP_Y=21, BASE_Y=41 (screen_y 25).
// Grid fits 10 cols in 256 lines: GRID_STEP_X=26, BASE_X=27 (screen_x 11).
////////////////////////////////////////////////////////////////////////////////
#define FONT_STEP   16      // pixel spacing between font character sprites
#define FONT_REG_Y  18      // sprite reg Y for HUD row (screen y=2)

#if DEMO_RES == 256
// 256x256 mode — visible area ~240 lines tall x 256 wide (15kHz progressive)
// X: margin=(256-9*26)/2=11 -> BASE_X=11+16=27; covers screen_x 11-245
// Y: margin=(240-9*21)/2=25 -> BASE_Y=25+16=41; covers screen_y 25-214
#define GRID_STEP_X 26
#define GRID_STEP_Y 21
#define PLEX_COLS   10
#define PLEX_ROWS   10
#define PLEX_TOTAL  100
#define BASE_X      27
#define BASE_Y      41
#define FONT_REG_X  208     // flush right: screen x=192, reg x=208
#elif DEMO_RES == 512
// 512x512: 15x8 grid = 120 sprites (HW max with HUD at 120-123)
// X: 256 - 14*32/2 = 32 -> reg = 48; Y: 256 - 7*32/2 = 144 -> reg = 160
#define GRID_STEP_X 32
#define GRID_STEP_Y 32
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
#define DECOMP_LAUNCH   7       // frames between successive sprite launches
#define DECOMP_SPEED    2       // pixels per frame toward grid target

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
// Circle path — 100 points evenly sampled from a r=64 circle, sorted CCW
// from west.
// Centre: screen(128,120) -> reg(144,136). screen_y range: 56-184.
////////////////////////////////////////////////////////////////////////////////
static const uint8_t circle_lut[100][2] = {  /* {reg_x, reg_y} r=64, centre reg(144,136) */
    { 80,136}, { 80,132}, { 81,128}, { 81,124}, { 82,120},
    { 83,116}, { 84,112}, { 86,109}, { 88,105}, { 90,102},
    { 92, 98}, { 95, 95}, { 97, 92}, {100, 89}, {103, 87},
    {106, 84}, {110, 82}, {113, 80}, {117, 78}, {120, 76},
    {124, 75}, {128, 74}, {132, 73}, {136, 73}, {140, 72},
    {144, 72}, {148, 72}, {152, 73}, {156, 73}, {160, 74},
    {164, 75}, {168, 76}, {171, 78}, {175, 80}, {178, 82},
    {182, 84}, {185, 87}, {188, 89}, {191, 92}, {193, 95},
    {196, 98}, {198,102}, {200,105}, {202,109}, {204,112},
    {205,116}, {206,120}, {207,124}, {207,128}, {208,132},
    {208,136}, {208,140}, {207,144}, {207,148}, {206,152},
    {205,156}, {204,160}, {202,163}, {200,167}, {198,170},
    {196,174}, {193,177}, {191,180}, {188,183}, {185,185},
    {182,188}, {178,190}, {175,192}, {171,194}, {168,196},
    {164,197}, {160,198}, {156,199}, {152,199}, {148,200},
    {144,200}, {140,200}, {136,199}, {132,199}, {128,198},
    {124,197}, {120,196}, {117,194}, {113,192}, {110,190},
    {106,188}, {103,185}, {100,183}, { 97,180}, { 95,177},
    { 92,174}, { 90,170}, { 88,167}, { 86,163}, { 84,160},
    { 83,156}, { 82,152}, { 81,148}, { 81,144}, { 80,140},
};

// PCG load order: 50 unique-shape patterns first, then 78 colour variants.
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
static uint8_t  pat_tick;         // counts 0-2; pat_counter++ every 3 frames
static uint16_t phase_counter;    // counts down; fires grid<->circle switch
static uint8_t  demo_phase;       // 0=grid, 1=circle, 2=decompose
static uint8_t  circle_rot;       // 0-99 rotation index for circle/decompose

// Decompose phase state
static uint8_t  decompose_count;           // sprites launched so far (0-PLEX_TOTAL)
static uint8_t  decompose_arrived;         // sprites that reached their grid target
static uint8_t  decompose_tick;            // frames since last launch (0-6)
static int16_t  decomp_cur_x[PLEX_TOTAL]; // current sprite x during flight
static int16_t  decomp_cur_y[PLEX_TOTAL]; // current sprite y during flight
static uint8_t  decomp_state[PLEX_TOTAL]; // 0=on circle, 1=flying, 2=arrived

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
////////////////////////////////////////////////////////////////////////////////
static void update_all_sprites(void)
{
    volatile sprite_t *sa = SPRITES;
    uint8_t  frame_lo2 = (uint8_t)(frame_counter * 2);
    uint16_t pat_base  = pat_counter & 0x7F;
    uint16_t bx        = BASE_X;
    uint16_t by        = BASE_Y;
    uint16_t col_phase = 0;
    uint16_t row_phase = 0;
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
        bx        = (uint16_t)(bx + GRID_STEP_X);
        col_phase = (uint16_t)(col_phase + 13);

        if (col >= PLEX_COLS) {
            col       = 0;
            bx        = BASE_X;
            col_phase = 0;
            by        = (uint16_t)(by + GRID_STEP_Y);
            row_phase = (uint16_t)(row_phase + 23);
        }
    }
}

////////////////////////////////////////////////////////////////////////////////
// update_circle_sprites — arrange all PLEX_TOTAL sprites around the
// Bresenham circle, offset by circle_rot for rotation each frame.
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
// update_decompose_sprites — decompose phase renderer.
//
// Each sprite is in one of three states:
//   0 (circle):  positioned on rotating circle as normal
//   1 (flying):  glides DECOMP_SPEED px/frame toward its grid target
//   2 (arrived): joins the sine-wave grid animation
//
// Sprites transition 0->1 one at a time every DECOMP_LAUNCH frames.
// When a sprite's current position reaches its grid target, it moves to
// state 2 and decompose_arrived is incremented.
////////////////////////////////////////////////////////////////////////////////
static void update_decompose_sprites(void)
{
    volatile sprite_t *sa = SPRITES;
    uint8_t  frame_lo2 = (uint8_t)(frame_counter * 2);
    uint16_t pat_base  = pat_counter & 0x7F;
    uint8_t  rot       = circle_rot;
    uint16_t bx        = BASE_X;
    uint16_t by        = BASE_Y;
    uint16_t col_phase = 0;
    uint16_t row_phase = 0;
    uint16_t col       = 0;
    int slot;

    for (slot = 0; slot < PLEX_TOTAL; slot++) {
        uint8_t state = decomp_state[slot];

        if (state == 0) {
            // Still on rotating circle
            uint8_t pos = (uint8_t)(slot + rot);
            if (pos >= 100) pos = (uint8_t)(pos - 100);
            sa[slot].x = circle_lut[pos][0];
            sa[slot].y = circle_lut[pos][1];

        } else if (state == 1) {
            // Flying toward grid centre (bx, by) at DECOMP_SPEED px/frame
            int16_t tx = (int16_t)bx;
            int16_t ty = (int16_t)by;
            int16_t cx = decomp_cur_x[slot];
            int16_t cy = decomp_cur_y[slot];
            int16_t ex = tx - cx;
            int16_t ey = ty - cy;

            if      (ex >  DECOMP_SPEED) cx = (int16_t)(cx + DECOMP_SPEED);
            else if (ex < -DECOMP_SPEED) cx = (int16_t)(cx - DECOMP_SPEED);
            else                          cx = tx;

            if      (ey >  DECOMP_SPEED) cy = (int16_t)(cy + DECOMP_SPEED);
            else if (ey < -DECOMP_SPEED) cy = (int16_t)(cy - DECOMP_SPEED);
            else                          cy = ty;

            decomp_cur_x[slot] = cx;
            decomp_cur_y[slot] = cy;
            sa[slot].x = (uint16_t)cx;
            sa[slot].y = (uint16_t)cy;

            if (cx == tx && cy == ty) {
                decomp_state[slot] = 2;
                decompose_arrived++;
            }

        } else {
            // Arrived — full sine-wave grid animation
            uint8_t angle = (uint8_t)(frame_lo2 + col_phase + row_phase);
            int8_t  sdx   = sine_table[angle];
            int8_t  sdy   = sine_table[(uint8_t)(angle + 64)];
            sa[slot].x = (uint16_t)((int16_t)bx + sdx);
            sa[slot].y = (uint16_t)((int16_t)by + sdy);
        }

        sa[slot].ctrl = (uint16_t)((slot + pat_base) & 0x7F);
        sa[slot].prio = 0x0003;

        col++;
        bx        = (uint16_t)(bx + GRID_STEP_X);
        col_phase = (uint16_t)(col_phase + 13);

        if (col >= PLEX_COLS) {
            col       = 0;
            bx        = BASE_X;
            col_phase = 0;
            by        = (uint16_t)(by + GRID_STEP_Y);
            row_phase = (uint16_t)(row_phase + 23);
        }
    }
}

////////////////////////////////////////////////////////////////////////////////
// swap_sprite_batch — CPU-copy 64 patterns into PCG slots 64-127
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

    for (i = 0; i < 128; i++) {
        sa[i].x    = 0x03FF;
        sa[i].y    = 0x03FF;
        sa[i].ctrl = 0;
        sa[i].prio = 0;
    }

    load_palette(sprite_palette_grb555, PAL_PLEX);
    load_palette(font_palette_grb555,   PAL_FONT);

    for (i = 0; i < 128; i++)
        load_to_pcg(sprite_patterns + (uint32_t)pcg_init_order[i] * 128, i, 1);
    load_to_pcg(font_16x16_hex_data, PCG_FONT, FONT_CHARS);

    for (i = 0; i < 4; i++) {
        sa[120 + i].x    = (uint16_t)(FONT_REG_X + i * FONT_STEP);
        sa[120 + i].y    = FONT_REG_Y;
        sa[120 + i].ctrl = (uint16_t)(PAL_FONT_BITS | PCG_FONT);
        sa[120 + i].prio = 0x0003;
    }
}

////////////////////////////////////////////////////////////////////////////////
// sprite_plex_loop — main demo loop (never returns)
//
// Phase sequence: 0 (grid, 360f) -> 1 (circle, 360f) -> 2 (decompose,
// completion-driven) -> 0 -> ...
////////////////////////////////////////////////////////////////////////////////
void sprite_plex_loop(void)
{
    int i;

    frame_counter     = 0;
    swap_countdown    = SWAP_FRAMES;
    swap_batch        = 0;
    pat_counter       = 0;
    pat_tick          = 0;
    phase_counter     = PHASE_FRAMES;
    demo_phase        = 0;
    circle_rot        = 0;
    decompose_count   = 0;
    decompose_arrived = 0;
    decompose_tick    = 0;
    for (i = 0; i < PLEX_TOTAL; i++)
        decomp_state[i] = 0;

    for (;;) {
        wait_vblank();
        frame_counter++;

        if (++pat_tick >= 3) {
            pat_tick = 0;
            pat_counter++;
        }

        if (--swap_countdown == 0) {
            swap_countdown = SWAP_FRAMES;
            if (++swap_batch >= 3)
                swap_batch = 0;
            swap_sprite_batch();
        }

        if (demo_phase == 0) {
            // ---- Grid / sine-wave phase ----
            if (--phase_counter == 0) {
                phase_counter = PHASE_FRAMES;
                demo_phase    = 1;
                circle_rot    = 0;
            }
            update_all_sprites();

        } else if (demo_phase == 1) {
            // ---- Circle rotation phase ----
            if (++circle_rot >= 100) circle_rot = 0;

            if (--phase_counter == 0) {
                // Transition to decompose
                demo_phase        = 2;
                decompose_count   = 0;
                decompose_arrived = 0;
                decompose_tick    = (uint8_t)(DECOMP_LAUNCH - 1); // fire first launch next frame
                for (i = 0; i < PLEX_TOTAL; i++)
                    decomp_state[i] = 0;
            }
            update_circle_sprites();

        } else {
            // ---- Decompose phase ----
            if (++circle_rot >= 100) circle_rot = 0;

            // Launch one sprite per DECOMP_LAUNCH frames
            if (++decompose_tick >= DECOMP_LAUNCH) {
                decompose_tick = 0;
                if (decompose_count < PLEX_TOTAL) {
                    uint8_t s   = decompose_count;
                    uint8_t pos = (uint8_t)(s + circle_rot);
                    if (pos >= 100) pos = (uint8_t)(pos - 100);
                    // Snapshot current circle position as starting point
                    decomp_cur_x[s] = (int16_t)circle_lut[pos][0];
                    decomp_cur_y[s] = (int16_t)circle_lut[pos][1];
                    decomp_state[s] = 1;   // start flying
                    decompose_count++;
                }
            }

            update_decompose_sprites();

            // Phase ends when every sprite has arrived at its grid target
            if (decompose_arrived >= PLEX_TOTAL) {
                demo_phase    = 0;
                phase_counter = PHASE_FRAMES;
            }
        }

        update_font_display();
        bg_scroll_step(1, 1);   // diagonal background scroll, hardware-wrapped
    }
}
