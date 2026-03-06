////////////////////////////////////////////////////////////////////////////////
// x68k_video.c — X68000 video initialisation, VBlank, DMA fill, HBlank ISR
//
// Ports x68k_video.s and font_helpers_16x16.s entirely to C99.
////////////////////////////////////////////////////////////////////////////////

#include "types.h"
#include "x68k_hw.h"
#include "x68k_video.h"

////////////////////////////////////////////////////////////////////////////////
// Video mode table — 4 modes, 14 words (28 bytes) each
//
// CRTC R20 ($E80028) — resolution selector:
//   bit 4   : 0 = 15 kHz (TV/CRT composite sync), 1 = 31 kHz (VGA-style)
//   bits 3-2: vertical pixel count: 00=256, 01=512
//   bits 1-0: horizontal pixel count: 00=256, 01=512
//
// CRTC R00-R08 ($E80000-$E80010) — horizontal/vertical timing:
//   R00: H-total    (in 8-pixel chars; total scanline width)
//   R01: H-sync end (end of horizontal sync pulse)
//   R02: H-disp start (first visible char; = H-back-porch end)
//   R03: H-disp end   (first invisible char; R03-R02 = display width in chars)
//   R04: V-total    (total lines per frame, 0-based)
//   R05: V-sync end (end of vertical sync pulse)
//   R06: V-disp start (first visible line; = V-back-porch end)
//   R07: V-disp end   (first invisible line; R07-R06 = active display lines)
//   R08: V-sync adjustment (fine-tuning for interlace; 0 for progressive)
//
// Sprite timing ($EB080A-$EB0810) — must match CRTC H/V parameters:
//   SP_H_TOTAL ($EB080A): sprite H-total (should equal CRTC R00)
//   SP_H_DISP  ($EB080C): sprite H-disp start (chars before first sprite column)
//   SP_V_DISP  ($EB080E): sprite V-disp start (lines before first sprite row)
//   SP_RES     ($EB0810): sprite resolution flags (bit0=H512, bit1=V512, etc.)
//
// Sources: X68000 Technical Data Book Tables 2-11 (CRTC) and 2-18 (Sprite).
////////////////////////////////////////////////////////////////////////////////
typedef struct {
    uint16_t r20;
    uint16_t r[9];        // CRTC R00-R08 (sequential word registers at $E80000+)
    uint16_t sp_h_total;
    uint16_t sp_h_disp;
    uint16_t sp_v_disp;
    uint16_t sp_res;
} video_mode_t;

static const video_mode_t video_mode_table[4] = {

    // -------------------------------------------------------------------------
    // Mode 0: 256x256 16C 15kHz — VIDEO_256x256_16C_15K (index 0)   *** ACTIVE ***
    // Source: X68000 Technical Data Book Table 2-11 (rightmost column) +
    //         Table 2-18 (15kHz 256×256 sprite timing)
    //
    // R20 = 0x0000
    //   bit4=0  → 15 kHz horizontal scan rate (~15.98 kHz actual)
    //   bits1-0=00 → H pixel count = 256
    //   bits3-2=00 → V pixel count = 256
    //
    // Pixel clock: ~4.79 MHz (15.98 kHz × 300 dots/line = 4.794 MHz)
    //
    // CRTC horizontal (all values in 8-pixel char units):
    //   R00=0x0025 (37): H-total = 37 chars = 296 dot-clocks/line
    //   R01=0x0001 ( 1): H-sync ends at char 1 (sync pulse = 1 char = 8px)
    //   R02=0x0000 ( 0): H-disp starts at char 0 (no back-porch — overscan)
    //   R03=0x0020 (32): H-disp ends at char 32 → 32×8 = 256 visible pixels
    //   Note: 15kHz uses OVERSCAN (display begins at char 0); no blanking margin
    //
    // CRTC vertical:
    //   R04=0x0103 (259): V-total = 260 lines (0-based: 0..259)
    //     → Frame rate: 15980 Hz / 260 lines ≈ 61.5 Hz
    //   R05=0x0002 ( 2): V-sync ends at line 2
    //   R06=0x0010 (16): V-disp starts at line 16 (16-line back porch)
    //   R07=0x0100 (256): V-disp ends at line 256 → 256-16 = 240 active lines
    //   R08=0x0024 (36): V-sync adjustment
    //
    // Active display: 256×240 pixels (progressive scan, no doublescan)
    // Sprite coordinate offset: reg = screen + 16 (hardware constant)
    //
    // Sprite timing (Table 2-18, 15kHz 256×256 column):
    //   SP_H_TOTAL=0x0025: matches CRTC R00
    //   SP_H_DISP =0x0004: 4 chars before first sprite column
    //   SP_V_DISP =0x0010: 16 lines before first sprite row (matches R06)
    //   SP_RES    =0x0000: no resolution doubling
    // -------------------------------------------------------------------------
    { 0x0000, {0x0025,0x0001,0x0000,0x0020,0x0103,0x0002,0x0010,0x0100,0x0024},
      0x0025, 0x0004, 0x0010, 0x0000 },

    // -------------------------------------------------------------------------
    // Mode 1: 512x512 16C 31kHz — VIDEO_512x512_16C_31K (index 1)   VERIFIED
    // Source: X68000 Technical Data Book Table 2-11 (512×512 column)
    //
    // R20 = 0x0015
    //   bit4=1  → 31 kHz horizontal scan rate (~31.5 kHz actual)
    //   bits1-0=01 → H pixel count = 512
    //   bits3-2=01 → V pixel count = 512
    //
    // CRTC horizontal (8-pixel char units):
    //   R00=0x004B (75): H-total = 76 chars = 608 dots/line
    //   R01=0x0003 ( 3): H-sync ends at char 3
    //   R02=0x0005 ( 5): H-disp starts at char 5 (underscan back-porch)
    //   R03=0x0045 (69): H-disp ends at char 69 → (69-5)×8 = 512 pixels
    //
    // CRTC vertical:
    //   R04=0x023F (575): V-total = 576 lines → 31500/576 ≈ 54.7 Hz
    //   R05=0x0005 ( 5): V-sync ends at line 5
    //   R06=0x0010 (16): V-disp starts at line 16
    //   R07=0x0210 (528): V-disp ends at line 528 → 528-16 = 512 active lines
    //   R08=0x0005 ( 5): V-sync adjustment
    //
    // Active display: 512×512 pixels (progressive, 31kHz underscan)
    //
    // Sprite timing:
    //   SP_H_TOTAL=0x004B, SP_H_DISP=0x0004, SP_V_DISP=0x0010, SP_RES=0x0005
    //   SP_RES=0x0005: bit0=1 (H512), bit2=1 (V512... TBD confirm bit assignment)
    // -------------------------------------------------------------------------
    { 0x0015, {0x004B,0x0003,0x0005,0x0045,0x023F,0x0005,0x0010,0x0210,0x0005},
      0x004B, 0x0004, 0x0010, 0x0005 },

    // -------------------------------------------------------------------------
    // Mode 2: 256x256 16C 31kHz — VIDEO_256x256_16C_31K (index 2)
    // Source: X68000 Technical Data Book Table 2-11 (31kHz 256×256 column)
    //
    // R20 = 0x0010
    //   bit4=1  → 31 kHz
    //   bits1-0=00 → H pixel count = 256
    //   bits3-2=00 → V pixel count = 256
    //
    // CRTC horizontal:
    //   R00=0x004B (75): H-total = 76 chars (same clock as 512-mode)
    //   R01=0x0003 ( 3): H-sync ends at char 3
    //   R02=0x0015 (21): H-disp starts at char 21 (wide underscan back-porch)
    //   R03=0x0035 (53): H-disp ends at char 53 → (53-21)×8 = 256 pixels
    //
    // CRTC vertical (doublescan — each pixel row displayed twice):
    //   R04=0x023F (575): V-total = 576 lines → ~54.7 Hz
    //   R05=0x0005 ( 5): V-sync ends at line 5
    //   R06=0x0010 (16): V-disp starts at line 16
    //   R07=0x0110 (272): V-disp ends at line 272 → 272-16 = 256 lines
    //     In doublescan: 256 CRTC lines / 2 = 128 unique pixel rows visible
    //   R08=0x0005 ( 5): V-sync adjustment
    //
    // Active display: 256×256 CRTC lines = 128 unique pixel rows (doublescan)
    // WARNING: use 15kHz mode (index 0) for full 240-line progressive display.
    //
    // Sprite timing:
    //   SP_H_TOTAL=0x004B, SP_H_DISP=0x0014, SP_V_DISP=0x0010, SP_RES=0x0000
    // -------------------------------------------------------------------------
    { 0x0010, {0x004B,0x0003,0x0015,0x0035,0x023F,0x0005,0x0010,0x0110,0x0005},
      0x004B, 0x0014, 0x0010, 0x0000 },

    // -------------------------------------------------------------------------
    // Mode 3: 512x256 16C 31kHz — VIDEO_512x256_16C_31K (index 3)
    // Source: X68000 Technical Data Book Table 2-11 (31kHz 512×256 column)
    //
    // R20 = 0x0011
    //   bit4=1  → 31 kHz
    //   bits1-0=01 → H pixel count = 512
    //   bits3-2=00 → V pixel count = 256
    //
    // CRTC horizontal:
    //   R00=0x004B (75): H-total = 76 chars
    //   R01=0x0003 ( 3): H-sync ends at char 3
    //   R02=0x0005 ( 5): H-disp starts at char 5
    //   R03=0x0045 (69): H-disp ends at char 69 → (69-5)×8 = 512 pixels
    //
    // CRTC vertical (doublescan):
    //   R04=0x023F (575): V-total = 576 lines → ~54.7 Hz
    //   R05=0x0005 ( 5): V-sync ends at line 5
    //   R06=0x0010 (16): V-disp starts at line 16
    //   R07=0x0110 (272): V-disp ends at line 272 → 256 CRTC lines (doublescan)
    //   R08=0x0005 ( 5): V-sync adjustment
    //
    // Sprite timing:
    //   SP_H_TOTAL=0x004B, SP_H_DISP=0x0004, SP_V_DISP=0x0010, SP_RES=0x0001
    // -------------------------------------------------------------------------
    { 0x0011, {0x004B,0x0003,0x0005,0x0045,0x023F,0x0005,0x0010,0x0110,0x0005},
      0x004B, 0x0004, 0x0010, 0x0001 },
};

////////////////////////////////////////////////////////////////////////////////
// init_video — program CRTC, VC, and sprite timing for the given mode
////////////////////////////////////////////////////////////////////////////////
void init_video(int mode)
{
    const video_mode_t *m = &video_mode_table[mode];
    volatile uint16_t *crtc = (volatile uint16_t *)CRTC_R00;
    int i;

    REG16(VC_R0) = 0x0000;
    REG16(VC_R1) = 0x02E4;
    REG16(VC_R2) = 0x00C1;

    REG16(CRTC_R20) = m->r20;
    for (i = 0; i < 9; i++)
        crtc[i] = m->r[i];   // R00-R08 are sequential word registers

    REG16(SP_H_TOTAL)  = m->sp_h_total;
    REG16(SP_H_DISP)   = m->sp_h_disp;
    REG16(SP_V_DISP)   = m->sp_v_disp;
    REG16(SP_RES)      = m->sp_res;
    REG16(SP_DISP_CPU) = 0x0200;  // sprites on — constant for all modes
}

////////////////////////////////////////////////////////////////////////////////
// wait_vblank — spin until the start of VBlank
// MFP GPIP bit 4: 0 = VBlank active, 1 = display active
////////////////////////////////////////////////////////////////////////////////
void wait_vblank(void)
{
    while (!(REG8(MFP_GPIP_B) & 0x10));  // if in VBlank, wait for display
    while (  REG8(MFP_GPIP_B) & 0x10);   // wait for VBlank to start
}

////////////////////////////////////////////////////////////////////////////////
// gvram_fill_dma — fill GVRAM page 0 with a 4-bit colour index (blocking)
//
// Strategy: cascade copy, HD63450 channel 2 linked-array chain, burst/longword.
// CPU fills row 0 (256 longwords), DMA copies row N → row N+1 for 511 entries.
////////////////////////////////////////////////////////////////////////////////
typedef struct {
    uint32_t mar;    // destination address
    uint16_t mtcr;   // longword count (256 per row)
} __attribute__((packed)) dma_entry_t;

static dma_entry_t gvram_clear_chain[512];  // 511 entries used (rows 1-511)

void gvram_fill_dma(uint32_t value)
{
    volatile uint8_t  *dma   = (volatile uint8_t  *)DMA2_BASE;
    volatile uint32_t *gvram = (volatile uint32_t *)GVRAM_BASE;
    uint32_t fill;
    int i;

    value &= 0xF;
    fill = (value << 16) | value;   // two 16-bit pixels per longword

    // Step 1: CPU-fill row 0 (256 longwords = 512 pixels)
    for (i = 0; i < 256; i++)
        gvram[i] = fill;

    // Step 2: build chain table for rows 1-511
    for (i = 0; i < 511; i++) {
        gvram_clear_chain[i].mar  = (uint32_t)(GVRAM_BASE + (uint32_t)(i + 1) * GVRAM_ROW_BYTES);
        gvram_clear_chain[i].mtcr = 256;
    }

    // Step 3: program DMA channel 2
    dma[DMA_CSR] = 0xFF;     // clear all status bits
    dma[DMA_DCR] = 0x08;     // burst mode
    dma[DMA_OCR] = 0xA9;     // linked-array chain, longword operand
    dma[DMA_SCR] = 0x05;     // DAIN=01 (src increments), MAIN=01 (dest increments)
    dma[DMA_CCR] = 0x00;
    dma[DMA_CPR] = 0x03;
    dma[DMA_MFC] = 0x05;
    dma[DMA_DFC] = 0x05;
    dma[DMA_BFC] = 0x05;

    *(volatile uint16_t *)(DMA2_BASE + DMA_BTC) = 511;
    *(volatile uint32_t *)(DMA2_BASE + DMA_DAR) = (uint32_t)GVRAM_BASE;
    *(volatile uint32_t *)(DMA2_BASE + DMA_BAR) = (uint32_t)gvram_clear_chain;

    dma[DMA_CCR] |= 0x80;   // start DMA

    // Wait for completion (COC or error)
    while (!(dma[DMA_CSR] & 0x90));
    dma[DMA_CSR] = 0xFF;
}

////////////////////////////////////////////////////////////////////////////////
// Blue gradient — 24 entries x 16 lines = 384 scanlines
// GRB555: (G<<11)|(R<<6)|(B<<1). Pure blue, dark at top to vivid at bottom.
// Only the first ~8 bands (lines 0-127) are visible in 256-mode MAME view.
////////////////////////////////////////////////////////////////////////////////
static const uint16_t sky_colors[24] = {
    0x0004, 0x0006, 0x000A, 0x000E,  //  0- 3: near-black to dark blue
    0x0014, 0x001C, 0x0026, 0x0032,  //  4- 7: mid to vivid blue (visible)
    0x003A, 0x003C, 0x003E, 0x003E,  //  8-11: saturated blue (off-screen)
    0x003E, 0x003E, 0x003E, 0x003E,  // 12-15: max blue
    0x003E, 0x003E, 0x003E, 0x003E,  // 16-19: max blue
    0x003E, 0x003E, 0x003E, 0x003E,  // 20-23: max blue
};

static volatile uint16_t hblank_scanline;
static volatile uint32_t hblank_lo;

////////////////////////////////////////////////////////////////////////////////
// hblank_handler — GPIP7/HSync ISR (installed at vector $4F, address $13C)
// Twilight sky gradient: scanlines 0-383 = sky, 384+ = solid ground.
////////////////////////////////////////////////////////////////////////////////
void __attribute__((interrupt)) hblank_handler(void)
{
    uint16_t s;
    hblank_lo++;

    // During VBlank (bit4=0): reset scanline counter and return
    if (!(REG8(MFP_GPIP_B) & 0x10)) {
        hblank_scanline = 0;
        return;
    }

    s = hblank_scanline++;
    if (s < 384)
        REG16(GVRAM_PAL) = sky_colors[s >> 4];   // sky: 24 bands x 16 lines
    else
        REG16(GVRAM_PAL) = 0x8088;               // twilight green ground
}

////////////////////////////////////////////////////////////////////////////////
// init_hblank — hook GPIP7/HSync interrupt for sky gradient effect
////////////////////////////////////////////////////////////////////////////////
void init_hblank(void)
{
    // Install handler at vector $4F (address $13C)
    // Suppress array-bounds: GCC doesn't know address 0x13C is a valid HW location
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
    *(volatile uint32_t *)0x13CUL = (uint32_t)hblank_handler;
#pragma GCC diagnostic pop

    // Stop all MFP timers
    REG8(MFP_TACR)  = 0;
    REG8(MFP_TBCR)  = 0;
    REG8(MFP_TCDCR) = 0;

    // Disable and clear all MFP interrupt state
    REG8(MFP_IERA) = 0;
    REG8(MFP_IERB) = 0;
    REG8(MFP_IMRA) = 0;
    REG8(MFP_IMRB) = 0;
    REG8(MFP_IPRA) = 0;
    REG8(MFP_IPRB) = 0;
    REG8(MFP_ISRA) = 0;
    REG8(MFP_ISRB) = 0;

    // Configure GPIP7 as input, falling edge trigger
    REG8(MFP_DDR) &= ~0x80;   // DDR bit7 = 0 (input)
    REG8(MFP_AER) &= ~0x80;   // AER bit7 = 0 (falling edge)

    // Enable GPIP7/I15 only
    REG8(MFP_IERA) = 0x80;
    REG8(MFP_IMRA) = 0x80;

    // Lower IPL to 5 to allow level-6 MFP interrupts
    // Use raw opcode: MOVE #0x2500,SR = 0x46FC 0x2500
    __asm__ volatile(".word 0x46FC, 0x2500");
}
