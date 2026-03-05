////////////////////////////////////////////////////////////////////////////////
// main.c — X68000 Sprite Plex Demo entry point (C99)
// 128 PCG sprites on screen; 64 stable + 64 swapped every ~4 seconds.
////////////////////////////////////////////////////////////////////////////////

#include "types.h"
#include "x68k_hw.h"
#include "x68k_video.h"

extern void init_sprite_plex(void);
extern void sprite_plex_loop(void);

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
    init_video(VIDEO_256x256_16C_31K);
#elif DEMO_RES == 512
    init_video(VIDEO_512x512_16C_31K);
#else
#error "Unknown DEMO_RES. Build with DEMO_RES=256x256 or DEMO_RES=512x512"
#endif
    gvram_fill_dma(0);   // fill background with colour index 0
    init_hblank();       // install HBlank ISR for per-scanline blue gradient
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
