////////////////////////////////////////////////////////////////////////////////
// main.c — X68000 Sprite Plex Demo entry point (C99)
// 128 PCG sprites on screen; 64 stable + 64 swapped every ~4 seconds.
////////////////////////////////////////////////////////////////////////////////

#include "types.h"
#include "x68k_hw.h"
#include "x68k_video.h"
#include "x68k_bg.h"

extern void init_sprite_plex(void);
extern void sprite_plex_loop(void);

// Font PCG data (128 bytes each).  Order: '0'-'9' (0-9), 'A'-'F' (10-15).
extern const uint8_t font_16x16_hex_data[];
#define FONT_PCG(n)  (font_16x16_hex_data + (uint32_t)(n) * 128)

// do_init — called exactly once by start.s, immediately after the binary is
// loaded from floppy, BSS is cleared, and hardware interrupts are disabled.
// It is NOT called on soft-restart (the 'restart' symbol in start.s jumps
// directly to do_loader, bypassing this function).
//
// Use this for one-time-only initialisation that must survive a soft-restart:
// e.g. detecting installed RAM, reading hardware ID registers, or seeding
// persistent state that do_loader should not reinitialise on restart.
//
// This demo reinitialises everything in do_loader, so no first-boot-only
// setup is required here.
void do_init(void) {}

void do_loader(void)
{
#if DEMO_RES == 256
    init_video(VIDEO_256x256_16C_15K);
#elif DEMO_RES == 512
    init_video(VIDEO_512x512_16C_31K);
#else
#error "Unknown DEMO_RES. Build with DEMO_RES=256x256 or DEMO_RES=512x512"
#endif
    // Hide GVRAM layer while building background off-screen.
    REG16(VC_R2) = 0x00C0;

    // Background layer — embossed CAFE tile scrolling diagonally.
    // Palette entry 0 is left for the HBlank gradient; 1-15 = grey ramp.
    // The 32×32 tile (4 × 16×16 glyphs) divides 512 exactly (16×16 copies).
    gvram_fill_dma(0);
    bg_grey_ramp(1, 15);
    bg_draw_pcg_embossed( 0,  0, FONT_PCG(12), 15, 8, 1, 0);  // C
    bg_draw_pcg_embossed(16,  0, FONT_PCG(10), 15, 8, 1, 0);  // A
    bg_draw_pcg_embossed( 0, 16, FONT_PCG(15), 15, 8, 1, 0);  // F
    bg_draw_pcg_embossed(16, 16, FONT_PCG(14), 15, 8, 1, 0);  // E
    bg_tile_region_dma(32, 32);

    init_hblank();       // install HBlank ISR for per-scanline blue gradient
    REG16(VC_R2) = 0x00C1;   // reveal GVRAM — background appears instantly
    init_sprite_plex();
    sprite_plex_loop();
    while (1);
}

// Stubs required by start.s / uhe_stub
struct options  { int test_selected, printmode; } options = {-1, 0};
struct progress { int dummy; } progress;
struct tseq     { int a, b, c; char *d; } tseq[] = {{0, 0, 0, ""}};
uint32_t mainmemory = 0x100000;
uint8_t  cputype    = 0;
