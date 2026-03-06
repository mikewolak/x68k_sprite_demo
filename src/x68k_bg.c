////////////////////////////////////////////////////////////////////////////////
// x68k_bg.c — X68000 GVRAM background layer implementation
//
// See x68k_bg.h for full API documentation and usage overview.
////////////////////////////////////////////////////////////////////////////////

#include "types.h"
#include "x68k_hw.h"
#include "x68k_bg.h"

////////////////////////////////////////////////////////////////////////////////
// Internal helpers
////////////////////////////////////////////////////////////////////////////////

// Convenience pointer to the GVRAM as an array of 16-bit pixels.
// Pixel (x, y) = gvram_px[y * 512 + x].
#define GVRAM_PX  ((volatile uint16_t *)GVRAM_BASE)

// GVRAM palette base (16 GRB555 entries, 2 bytes each).
#define BG_PAL    ((volatile uint16_t *)GVRAM_PAL)

// Scroll state — maintained in software to allow signed step arithmetic.
static uint16_t bg_sx;   // current X scroll (0–511)
static uint16_t bg_sy;   // current Y scroll (0–511)

////////////////////////////////////////////////////////////////////////////////
// decode_pcg — unpack one 128-byte PCG pattern into a flat 16×16 byte buffer.
//
// Output: buf[row][col] = source colour index (0 = transparent / off, 1–15 = on).
// This is an internal helper used by the tile-draw functions.
//
// PCG 4-chunk layout reminder:
//   chunk 0 (+0):  TL  rows 0–7,  cols 0–7
//   chunk 1 (+32): BL  rows 8–15, cols 0–7
//   chunk 2 (+64): TR  rows 0–7,  cols 8–15
//   chunk 3 (+96): BR  rows 8–15, cols 8–15
// Each chunk: 8 rows × 4 bytes; each byte = hi_nibble|lo_nibble (left|right).
////////////////////////////////////////////////////////////////////////////////
static void decode_pcg(const uint8_t *pcg, uint8_t buf[16][16])
{
    static const uint8_t chunk_base_row[4] = { 0, 8, 0, 8 };
    static const uint8_t chunk_base_col[4] = { 0, 0, 8, 8 };
    int chunk, row, pair;

    for (chunk = 0; chunk < 4; chunk++) {
        const uint8_t *src   = pcg + chunk * 32;
        int            br    = chunk_base_row[chunk];
        int            bc    = chunk_base_col[chunk];

        for (row = 0; row < 8; row++) {
            for (pair = 0; pair < 4; pair++) {
                uint8_t byte              = src[row * 4 + pair];
                buf[br + row][bc + pair*2    ] = (byte >> 4) & 0xF;
                buf[br + row][bc + pair*2 + 1] =  byte       & 0xF;
            }
        }
    }
}

////////////////////////////////////////////////////////////////////////////////
// Palette management
////////////////////////////////////////////////////////////////////////////////

void bg_set_pal(int idx, uint16_t grb555)
{
    BG_PAL[idx & 0xF] = grb555;
}

void bg_load_pal(const uint16_t *src, int start, int count)
{
    int i;
    for (i = 0; i < count; i++)
        BG_PAL[(start + i) & 0xF] = src[i];
}

// bg_grey_ramp — populate palette entries start..start+count-1 with a linear
// greyscale ramp from dark to bright.
//
// Level formula: level = (i + 1) * 2   for i = 0 .. count-1
// For count=15: levels 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30
// This avoids any division or modulo — only a left-shift and add.
// Grey in GRB555: G == R == B, so grey = (lv<<11)|(lv<<6)|(lv<<1).
void bg_grey_ramp(int start, int count)
{
    int i;
    for (i = 0; i < count; i++) {
        int      lv  = (i + 1) << 1;                        // 2, 4, 6 … 30
        uint16_t grb = (uint16_t)((lv << 11) | (lv << 6) | (lv << 1));
        bg_set_pal(start + i, grb);
    }
}

////////////////////////////////////////////////////////////////////////////////
// Fill
////////////////////////////////////////////////////////////////////////////////

// bg_fill — CPU-fill the entire 512×512 GVRAM (131072 longword writes).
// Each uint32_t write covers two adjacent pixels; the colour index occupies
// the low 4 bits of each 16-bit half: fill = (idx << 16) | idx.
void bg_fill(uint8_t color_idx)
{
    volatile uint32_t *gv  = (volatile uint32_t *)GVRAM_BASE;
    uint32_t           fill = ((uint32_t)color_idx << 16) | color_idx;
    uint32_t           i;
    for (i = 0; i < 131072UL; i++)
        gv[i] = fill;
}

////////////////////////////////////////////////////////////////////////////////
// Tile drawing
////////////////////////////////////////////////////////////////////////////////

void bg_draw_pcg(int gx, int gy, const uint8_t *pcg, uint8_t fg, uint8_t bg_c)
{
    uint8_t buf[16][16];
    int x, y;

    decode_pcg(pcg, buf);

    for (y = 0; y < 16; y++)
        for (x = 0; x < 16; x++)
            bg_pset(gx + x, gy + y, buf[y][x] ? fg : bg_c);
}

// bg_draw_pcg_embossed — draw a 16×16 PCG glyph with a directional emboss.
//
// Light source is assumed to come from the upper-left.  The kernel compares
// each pixel against its upper-left diagonal neighbour (x-1, y-1):
//
//   (ON,  neighbour OFF) → highlight  : bright edge facing the light source
//   (OFF, neighbour ON)  → shadow     : dark edge turned away from the light
//   (ON,  neighbour ON)  → mid        : interior of the glyph, lit uniformly
//   (OFF, neighbour OFF) → bg_c       : open background
//
// The effect gives a convincing raised/embossed appearance when the palette
// is set to a greyscale ramp (bg_grey_ramp).
void bg_draw_pcg_embossed(int gx, int gy, const uint8_t *pcg,
                           uint8_t hi, uint8_t mid,
                           uint8_t shadow, uint8_t bg_c)
{
    uint8_t buf[16][16];
    int x, y;
    uint8_t here, ul, out;

    decode_pcg(pcg, buf);

    for (y = 0; y < 16; y++) {
        for (x = 0; x < 16; x++) {
            here = buf[y][x]          ? 1u : 0u;
            ul   = (x > 0 && y > 0) ? (buf[y-1][x-1] ? 1u : 0u) : 0u;

            if      ( here && !ul) out = hi;
            else if (!here &&  ul) out = shadow;
            else if ( here &&  ul) out = mid;
            else                   out = bg_c;

            bg_pset(gx + x, gy + y, out);
        }
    }
}

////////////////////////////////////////////////////////////////////////////////
// Tiling
////////////////////////////////////////////////////////////////////////////////

// bg_tile_region — replicate the top-left tile_w×tile_h block across 512×512.
//
// Pass 1 (horizontal): for each of the tile_h source rows, extend the tile_w
//   pixels rightward to column 511 using a wrap counter (no division/modulo).
//
// Pass 2 (vertical): copy the tile_h completed rows downward to row 511
//   using a wrap counter (no division/modulo).
//
// Precondition: 512 % tile_w == 0 and 512 % tile_h == 0.
void bg_tile_region(int tile_w, int tile_h)
{
    int x, y, src_x, src_y;

    // Pass 1 — tile horizontally within each source row
    for (y = 0; y < tile_h; y++) {
        src_x = 0;
        for (x = tile_w; x < 512; x++) {
            GVRAM_PX[y * 512 + x] = GVRAM_PX[y * 512 + src_x];
            if (++src_x >= tile_w) src_x = 0;
        }
    }

    // Pass 2 — tile vertically, copying each completed row downward
    src_y = 0;
    for (y = tile_h; y < 512; y++) {
        for (x = 0; x < 512; x++)
            GVRAM_PX[y * 512 + x] = GVRAM_PX[src_y * 512 + x];
        if (++src_y >= tile_h) src_y = 0;
    }
}

////////////////////////////////////////////////////////////////////////////////
// Hardware scroll
////////////////////////////////////////////////////////////////////////////////

void bg_set_scroll(uint16_t x, uint16_t y)
{
    bg_sx = x & 0x1FF;   // clamp to 9-bit hardware range
    bg_sy = y & 0x1FF;
    REG16(CRTC_R10) = bg_sx;
    REG16(CRTC_R11) = bg_sy;
}

// bg_scroll_step — advance scroll by (dx, dy) and wrap within 0–511.
//
// Uses int16_t arithmetic to safely handle negative steps (reverse scroll).
// Two compares replace any modulo operation, keeping this 68000-safe.
void bg_scroll_step(int16_t dx, int16_t dy)
{
    int16_t sx = (int16_t)bg_sx + dx;
    int16_t sy = (int16_t)bg_sy + dy;

    if      (sx >= 512) sx -= 512;
    else if (sx <    0) sx += 512;

    if      (sy >= 512) sy -= 512;
    else if (sy <    0) sy += 512;

    bg_sx = (uint16_t)sx;
    bg_sy = (uint16_t)sy;
    REG16(CRTC_R10) = bg_sx;
    REG16(CRTC_R11) = bg_sy;
}
