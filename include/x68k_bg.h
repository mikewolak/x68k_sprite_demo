////////////////////////////////////////////////////////////////////////////////
// x68k_bg.h — X68000 GVRAM background layer API
//
// Provides a self-contained toolkit for managing the GVRAM graphic layer:
// palette control, pixel and tile drawing (including embossed PCG glyphs),
// full-screen tiling, and hardware-accelerated diagonal scroll.
//
// ── GVRAM overview ───────────────────────────────────────────────────────────
//   Address:    0xC00000  (GVRAM_BASE)
//   Size:       512 × 512 pixels, always present regardless of video mode.
//               In 256×256 video mode only the top-left 256×256 region is
//               visible, but the full 512×512 buffer is available for tiles.
//   Pixel fmt:  4-bpp packed — each pixel occupies one 16-bit word.
//               Only bits [3:0] carry the colour index (0–15).
//               Pixel (x, y) lives at: GVRAM_BASE + y*1024 + x*2
//   Colour 0:   Conventional background / transparent.  The HBlank ISR in
//               x68k_video.c may animate palette entry 0 for gradient effects;
//               using 0 as the tile background colour lets that animation show
//               through between glyphs.
//
// ── Palette overview ─────────────────────────────────────────────────────────
//   16 GRB555 entries at 0xE82000 (GVRAM_PAL_BASE).
//   GRB555 word: (G5 << 11) | (R5 << 6) | (B5 << 1), bit 0 unused.
//   Entry 0 is typically animated by the HBlank ISR; entries 1–15 are free
//   for static background colours.
//   bg_grey_ramp(1, 15) fills entries 1–15 with a dark-to-bright grey scale.
//
// ── Scroll overview ──────────────────────────────────────────────────────────
//   CRTC R12 (0xE80018) and R13 (0xE8001A) hardware-scroll the GVRAM graphic
//   layer with toroidal wrap.  (R10/R11 scroll the text layer, not GVRAM.)
//   Range: 0–511 per axis.  Call bg_scroll_step() once
//   per VBlank.  Because the tile region is a power-of-2 fraction of 512,
//   the wrap is seamless with no redraw.
//
// ── Typical usage ────────────────────────────────────────────────────────────
//   bg_fill(0);                          // clear GVRAM to colour 0 (black)
//   bg_grey_ramp(1, 15);                 // palette entries 1-15 = grey ramp
//   bg_draw_pcg_embossed(0,  0, pcg_C, 15, 8, 1, 0);  // C  top-left
//   bg_draw_pcg_embossed(16, 0, pcg_A, 15, 8, 1, 0);  // A  top-right
//   bg_draw_pcg_embossed(0, 16, pcg_F, 15, 8, 1, 0);  // F  bottom-left
//   bg_draw_pcg_embossed(16,16, pcg_E, 15, 8, 1, 0);  // E  bottom-right
//   bg_tile_region(32, 32);              // fill 512×512 with 16×16 repetitions
//   // then each VBlank:
//   bg_scroll_step(1, 1);               // diagonal scroll, hardware-wrapped
////////////////////////////////////////////////////////////////////////////////

#ifndef X68K_BG_H
#define X68K_BG_H

#include "types.h"
#include "x68k_hw.h"   // GVRAM_BASE, GVRAM_PAL, CRTC_R10, CRTC_R11

////////////////////////////////////////////////////////////////////////////////
// Palette management
//
// All palette entries use X68000 GRB555 format:
//   grb555 = (G5 << 11) | (R5 << 6) | (B5 << 1)
// where G5/R5/B5 are 5-bit values (0–31).  Bit 0 is always 0.
////////////////////////////////////////////////////////////////////////////////

// bg_set_pal — write one GRB555 colour to GVRAM palette entry idx (0–15).
void bg_set_pal(int idx, uint16_t grb555);

// bg_load_pal — copy 'count' GRB555 words from src into palette starting at
// entry 'start'.  Caller must ensure start+count <= 16.
void bg_load_pal(const uint16_t *src, int start, int count);

// bg_grey_ramp — fill 'count' consecutive palette entries starting at 'start'
// with a linear dark-to-bright greyscale ramp.  No floating-point or division.
// For count=15, start=1: entries 1–15 receive levels 2,4,6,...,30 (GRB equal).
// Entry 0 is left untouched so the HBlank gradient effect can continue.
void bg_grey_ramp(int start, int count);

////////////////////////////////////////////////////////////////////////////////
// GVRAM fill
////////////////////////////////////////////////////////////////////////////////

// bg_fill — CPU-fill the entire 512×512 GVRAM with colour index 'color_idx'.
// Use this at startup to establish a clean background before tile drawing.
// For the gradient effect to work, fill with 0 (the animated palette entry).
void bg_fill(uint8_t color_idx);

////////////////////////////////////////////////////////////////////////////////
// Low-level pixel access
//
// bg_pset is declared static inline so it can be called from tight tile-
// building loops in the caller's code without function-call overhead.
////////////////////////////////////////////////////////////////////////////////

// bg_pset — write colour index 'c' to GVRAM pixel (x, y).
// No bounds checking; caller must ensure 0 <= x,y <= 511.
static inline void bg_pset(int x, int y, uint8_t c)
{
    ((volatile uint16_t *)GVRAM_BASE)[(uint16_t)y * 512u + (uint16_t)x] = c;
}

////////////////////////////////////////////////////////////////////////////////
// Tile drawing — PCG chunk format
//
// Both functions accept a pointer to a 128-byte PCG pattern in the standard
// X68000 4-chunk layout:
//
//   +0:  TL — top-left  8×8 (rows 0–7,  cols 0–7)   — 32 bytes
//   +32: BL — bot-left  8×8 (rows 8–15, cols 0–7)   — 32 bytes
//   +64: TR — top-right 8×8 (rows 0–7,  cols 8–15)  — 32 bytes
//   +96: BR — bot-right 8×8 (rows 8–15, cols 8–15)  — 32 bytes
//
// Each 8×8 chunk: 8 rows × 4 bytes.  Each byte = two 4-bit colour indices
// (high nibble = left pixel, low nibble = right pixel).  Index 0 = transparent.
//
// Parameters common to both:
//   gx, gy   — top-left GVRAM pixel coordinate to draw into (0–511)
//   pcg      — pointer to 128-byte PCG pattern data
////////////////////////////////////////////////////////////////////////////////

// bg_draw_pcg — draw a 16×16 PCG tile at (gx, gy) with flat colour mapping.
//   fg       — palette index for any non-zero source pixel
//   bg_c     — palette index for zero (transparent) source pixels
void bg_draw_pcg(int gx, int gy, const uint8_t *pcg, uint8_t fg, uint8_t bg_c);

// bg_draw_pcg_embossed — draw a 16×16 PCG tile with a 3-shade emboss effect.
//
// The emboss kernel compares each pixel with its upper-left neighbour:
//   source ON,  neighbour OFF → 'hi'    (highlight — simulated light catch)
//   source OFF, neighbour ON  → 'shadow' (shadow — simulated depth)
//   source ON,  neighbour ON  → 'mid'   (interior of glyph)
//   source OFF, neighbour OFF → 'bg_c'  (background / transparent)
//
// Recommended values for a raised-metal look over the grey ramp (palette 1–15):
//   hi=15  mid=8  shadow=1  bg_c=0
//   hi=14  mid=9  shadow=2  bg_c=0  (slightly less contrast)
void bg_draw_pcg_embossed(int gx, int gy, const uint8_t *pcg,
                           uint8_t hi, uint8_t mid,
                           uint8_t shadow, uint8_t bg_c);

////////////////////////////////////////////////////////////////////////////////
// Tiling
////////////////////////////////////////////////////////////////////////////////

// bg_tile_region — replicate the top-left tile_w × tile_h block of GVRAM
// across the full 512×512 canvas.
//
// Preconditions (not checked at runtime):
//   • The source tile must already be drawn into GVRAM at (0,0).
//   • 512 must be exactly divisible by both tile_w and tile_h.
//     (32×32 tiles: 512/32=16 — 16×16=256 copies, perfectly seamless.)
//
// Algorithm:
//   Pass 1 — horizontal: extend each source row to fill 512 columns using a
//             wrap counter (no division or modulo).
//   Pass 2 — vertical:   copy the tile_h source rows downward to fill 512
//             rows using a wrap counter.
//
// This is an init-time operation; it runs once and need not be fast.
void bg_tile_region(int tile_w, int tile_h);

// bg_tile_region_dma — DMA-accelerated version of bg_tile_region.
//
// Pass 1 (CPU): tile horizontally across tile_h source rows.
// Pass 2 (DMA): HD63450 channel 2 linked-array cascade copies rows downward
//   in a single DMA call — same mechanism as gvram_fill_dma in x68k_video.c.
//
// Preconditions: same as bg_tile_region.
// Call this instead of bg_tile_region for instant off-screen construction.
void bg_tile_region_dma(int tile_w, int tile_h);

////////////////////////////////////////////////////////////////////////////////
// Hardware scroll
//
// The CRTC R10/R11 registers shift the GVRAM viewport with automatic toroidal
// wrap.  Incrementing both registers by 1 per VBlank produces smooth 45°
// diagonal scroll.  Because tile_w and tile_h are both exact divisors of 512,
// the scroll wraps invisibly — the pattern appears to scroll forever with no
// seam and no redraw required.
////////////////////////////////////////////////////////////////////////////////

// bg_set_scroll — write absolute scroll position to CRTC R10 (x) and R11 (y).
// Values are masked to 9 bits (0–511) by the hardware.
void bg_set_scroll(uint16_t x, uint16_t y);

// bg_scroll_step — advance the scroll position by (dx, dy) and wrap within
// 0–511.  Writes the updated values to CRTC R10/R11.
// Call once per VBlank after wait_vblank().
// Handles both positive (forward) and negative (reverse) step values.
void bg_scroll_step(int16_t dx, int16_t dy);

#endif // X68K_BG_H
