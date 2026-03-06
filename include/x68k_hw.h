////////////////////////////////////////////////////////////////////////////////
// x68k_hw.h — X68000 hardware register addresses and access macros
////////////////////////////////////////////////////////////////////////////////
#ifndef X68K_HW_H
#define X68K_HW_H

#include "types.h"

// Volatile register access
#define REG16(addr)  (*(volatile uint16_t *)(uint32_t)(addr))
#define REG8(addr)   (*(volatile uint8_t  *)(uint32_t)(addr))
#define REG32(addr)  (*(volatile uint32_t *)(uint32_t)(addr))

// CRTC ($E80000)
#define CRTC_R00    0xE80000UL
#define CRTC_R01    0xE80002UL
#define CRTC_R02    0xE80004UL
#define CRTC_R03    0xE80006UL
#define CRTC_R04    0xE80008UL
#define CRTC_R05    0xE8000AUL
#define CRTC_R06    0xE8000CUL
#define CRTC_R07    0xE8000EUL
#define CRTC_R08    0xE80010UL
#define CRTC_R10    0xE80014UL  // Text layer horizontal scroll (0-511)
#define CRTC_R11    0xE80016UL  // Text layer vertical scroll   (0-511)
#define CRTC_R12    0xE80018UL  // GVRAM graphic layer horizontal scroll (0-511)
#define CRTC_R13    0xE8001AUL  // GVRAM graphic layer vertical scroll   (0-511)
#define CRTC_R20    0xE80028UL

// Video Controller
#define VC_R0       0xE82400UL
#define VC_R1       0xE82500UL
#define VC_R2       0xE82600UL

// GVRAM
#define GVRAM_PAL       0xE82000UL
#define GVRAM_BASE      0xC00000UL
#define GVRAM_ROW_BYTES 1024

// Sprite hardware ($EB0xxx)
#define SP_ATTR_RAM     0xEB0000UL
#define SP_PAL_RAM      0xE82200UL
#define SP_PAT_RAM      0xEB8000UL
#define SP_DISP_CPU     0xEB0808UL
#define SP_H_TOTAL      0xEB080AUL
#define SP_H_DISP       0xEB080CUL
#define SP_V_DISP       0xEB080EUL
#define SP_RES          0xEB0810UL

// MFP MC68901 ($E88000) — bit4 of GPIP: 0=VBlank, 1=display
#define MFP_GPIP    0xE88000UL
#define MFP_GPIP_B  0xE88001UL
#define MFP_AER     0xE88003UL
#define MFP_DDR     0xE88005UL
#define MFP_IERA    0xE88007UL
#define MFP_IERB    0xE88009UL
#define MFP_IPRA    0xE8800BUL
#define MFP_IPRB    0xE8800DUL
#define MFP_ISRA    0xE8800FUL
#define MFP_ISRB    0xE88011UL
#define MFP_IMRA    0xE88013UL
#define MFP_IMRB    0xE88015UL
#define MFP_TACR    0xE88019UL
#define MFP_TBCR    0xE8801BUL
#define MFP_TCDCR   0xE8801DUL

// HD63450 DMA channel 2 ($E84080) — register byte offsets
#define DMA2_BASE   0xE84080UL
#define DMA_CSR      0    // byte: Control/Status
#define DMA_DCR      4    // byte: Device Control
#define DMA_OCR      5    // byte: Operation Control
#define DMA_SCR      6    // byte: Sequence Control
#define DMA_CCR      7    // byte: Channel Control (bit7 = start)
#define DMA_DAR     20    // long: Source Address (row 0 for cascade)
#define DMA_BTC     26    // word: Block Transfer Count
#define DMA_BAR     28    // long: Base Address (chain table pointer)
#define DMA_MFC     41    // byte: Memory Function Code
#define DMA_CPR     45    // byte: Channel Priority
#define DMA_DFC     49    // byte: Device Function Code
#define DMA_BFC     57    // byte: Base Function Code

// Video mode indices for init_video() — see video_mode_table in x68k_video.c
// for full register-by-register documentation of each mode.
//
// "16C" = 16-colour GVRAM graphic layer (4bpp palette, 16 entries at $E82000).
//         This refers to the GVRAM depth only — the sprite layer always has
//         16 separate palettes × 16 colours = 256 simultaneous sprite colours.
//
// 15kHz modes: progressive scan, overscan, ~60 Hz.  Require a 15kHz CRT or
//              a multi-sync monitor.  GVRAM display is 256×240 active lines.
// 31kHz modes: underscan, ~54 Hz.  256-line 31kHz modes use doublescan
//              (each pixel row shown twice), giving only 128 unique pixel rows.
#define VIDEO_256x256_16C_15K   0   // 256×240 active, 15kHz, ~61.5Hz  *** ACTIVE ***
#define VIDEO_512x512_16C_31K   1   // 512×512 active, 31kHz, ~54.7Hz  VERIFIED
#define VIDEO_256x256_16C_31K   2   // 256×128 unique rows, 31kHz doublescan
#define VIDEO_512x256_16C_31K   3   // 512×128 unique rows, 31kHz doublescan

#endif // X68K_HW_H
