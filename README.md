# X68000 Sprite Plex Demo

A bare-metal hardware demonstration for the Sharp X68000, written entirely in
C99 (M68000-targeted). It drives 100 simultaneous 16×16 PCG sprites across
three animated phases over a scrolling GVRAM background and serves as a
verified foundation for hardware-accurate sprite-based game development on
this platform.

---

## Table of Contents

1. [What This Is](#what-this-is)
2. [Track Loader Boot Mechanism](#track-loader-boot-mechanism) · [The `makexdf` Tool](#the-makexdf-tool)
3. [Hardware Under Test](#hardware-under-test)
4. [Demo Phases](#demo-phases)
5. [GVRAM Background Layer](#gvram-background-layer)
6. [Project Structure](#project-structure)
7. [Build System](#build-system)
8. [Toolchain Setup](#toolchain-setup)
9. [Running the Demo](#running-the-demo)
10. [Technical Architecture](#technical-architecture)
11. [Key Algorithms](#key-algorithms)
12. [Hard-Won Fixes and Gotchas](#hard-won-fixes-and-gotchas)
13. [Using This as a Game Foundation](#using-this-as-a-game-foundation)
14. [Extending the Demo](#extending-the-demo)

---

## What This Is

The Sharp X68000 (1987) is a Japanese personal computer built around the
Motorola 68000 CPU. It was one of the most capable home computers of its era
for 2D game programming, featuring dedicated sprite hardware, a DMA controller,
and an MFP (Multi-Function Peripheral) that can generate interrupts on every
horizontal sync. Ports of arcade games like Gradius and Space Harrier ran on
this machine with near-perfect accuracy.

This project is a **hardware validation and capability demonstration** of the
X68000's PCG (Pattern Character Generator) sprite system. It runs in bare-metal
supervisor mode — no OS, no BIOS calls after boot, no runtime libraries other
than a tiny `memset` stub — and directly programs every register involved in
sprite display, GVRAM background, CRTC timing, DMA, and MFP interrupts.

The goal was to:

- Prove that the complete sprite pipeline can be driven from pure C99 on a
  stock 68000 (no 68020+ instructions)
- Establish correct register access patterns, coordinate systems, and timing
  constraints verified against MAME emulation
- Create a reusable, well-understood codebase to build a real game from

---

## Track Loader Boot Mechanism

This demo does **not** use the X68000 FAT12 filesystem to load itself. Instead
it uses a classic demoscene technique: a **track loader**. The binary is written
directly to the start of each floppy track with no directory entry, no FAT
chain, and no filesystem metadata beyond a valid boot sector header. Loading is
done by the program's own bootstrap code using raw BIOS sector reads, one track
at a time.

This has two advantages for a bare-metal demo or game: the binary loads as fast
as the drive can spin (no FAT traversal overhead), and there is no dependency on
the filesystem being intact or even present.

### How the X68000 IPL ROM Boots

When the X68000 powers on, the IPL ROM reads the first sector (512 or 1024
bytes, depending on media) of the inserted floppy into memory at `0x2000` and
inspects the very first byte. If it is `$60` (the M68K short branch opcode
`BRA.S`), the IPL ROM treats the sector as a valid boot sector and jumps to
`0x2000` to execute it. Any other first byte causes the IPL ROM to abort and
display an error.

The binary produced by this project starts with exactly this two-byte sequence
in `start.s`:

```asm
_start:
    dc.b $60                          ; BRA.S — IPL ROM boot signature
    dc.b ((start_code - _start) - 2) ; branch displacement over the BPB area
```

The short branch jumps forward over 60 bytes of reserved space (the BPB —
BIOS Parameter Block) directly to `start_code`, which is the real bootstrap
entry point. The BPB area is overwritten by `makexdf` with the correct disk
format parameters after compilation.

### BPB and Media ID

Bytes 2–63 of the boot sector hold the BPB, a standard structure that
describes the floppy geometry. `makexdf.c` patches this area with the XDF
parameter block template (`id_xdf`) after compilation.

| Format | Total size | Geometry | Sector size | Capacity |
|--------|-----------|----------|-------------|---------|
| XDF | 1,261,568 B | 77 cyl × 2 heads × 8 sectors | 1024 bytes | ~1.2 MB |

Byte `$15` of the BPB is the media ID byte (`$FE` for XDF). The bootstrap no
longer reads it at runtime — the disk geometry is hardcoded for XDF. The BPB
is patched by `makexdf` purely for compatibility with any software that
inspects the format block.

### Track Layout — Full Disk, Both Sides

XDF is a 77-cylinder, double-sided format. Each side of each cylinder holds 8
sectors of 1024 bytes = **8 KB per half-track**. The loader reads one full
side per `_B_READ` call, alternating heads across all 77 cylinders. Total
capacity loaded: 77 × 2 × 8 KB = **1,232 KB**.

The image layout maps directly — no interleaving is needed:

```
[0     .. 8191 ] = cyl 0, head 0  (sectors 1–8)
[8192  .. 16383] = cyl 0, head 1  (sectors 1–8)
[16384 .. 24575] = cyl 1, head 0
[24576 .. 32767] = cyl 1, head 1
...
```

The flat binary is written into the image buffer sequentially. Chunk `i` of
the binary (at byte offset `i × 8192`) lands exactly at image offset
`i × 8192`, which is where the BIOS delivers it at load time. No `memmove`
or interleave step is needed.

### The Bootstrap Load Loop

Once the IPL ROM hands control to the boot sector at `0x2000`, the bootstrap
runs its own loader loop. It already has the first 8 KB (cyl 0, head 0) in
memory. It then reads every remaining half-track, alternating heads and
advancing the cylinder after completing both sides:

```asm
exe_load_loop:
    move.l a6,a1            ; a1 = destination pointer
    move.w d7,d1            ; d1 = boot device number
    move.l d5,d2            ; d2 = disk address [size][cyl][head][sector]
    move.l #$2000,d3        ; d3 = 8192 bytes (8 sectors × 1024)
    moveq  #$46,d0          ; _B_READ BIOS call
    trap   #15

    lea    ($2000,a6),a6    ; advance destination by 8KB

    eor.l  #$100,d5         ; toggle head bit: 0→1 or 1→0
    btst   #8,d5            ; test head bit after toggle
    bne    exe_load_loop_next ; head is now 1: stay on same cylinder
    add.l  #$10000,d5       ; head wrapped 1→0: advance to next cylinder
exe_load_loop_next:
    dbra   d6,exe_load_loop
```

The disk address in `d5` is a packed longword:
- `bits [31:24]` = sector-size code (`$03` = 1024 bytes)
- `bits [23:16]` = cylinder number (0–76)
- `bits  [15:8]` = head (0 or 1, toggled each chunk)
- `bits   [7:0]` = sector number (always 1 — the BIOS reads all 8 sequentially)

`d6` is initialised to `(binary_size / 8192) - 1` so the loop runs exactly
the right number of times. After the last half-track is read, the floppy is
ejected (`_B_EJECT`), the checksum is verified, BSS is zeroed, all hardware
interrupts are disabled (`move.w #$2700,sr`), and control passes to `do_init`
then `do_loader`.

### Checksum

`makexdf.c` pads the binary to an 8 KB boundary (matching the half-track chunk
size) and then computes a rotating-left add-accumulate checksum over the entire
padded binary, patching the result into offset `$40` of the boot sector such
that the same checksum loop run over the loaded binary produces zero. The tool
verifies this itself — a non-zero result aborts the build with an error rather
than silently producing a corrupt image.

```c
// Back-calculate the patch value so that the forward checksum totals zero
for (i = binsize; ; ) {
    i -= 4;
    sum = (sum >> 1) | (sum << 31);   // rotate right
    if (i == patchpoint) {
        put32msb(image + patchpoint, sum - sumatpatch);
        break;
    }
    sum -= get32msb(image + i);
}
```

The bootstrap's checksum loop mirrors this exactly. A mismatch halts and prints
`Checksum failed; program is corrupt or didn't load properly` — the only human-
readable string in the entire binary.

### Soft Restart Without Reloading

`start.s` includes a `loaded_flag` byte in the boot sector area. After the
first successful load, bit 0 of this flag is set. If the program ever jumps
back to `_start` (e.g. from the `restart` symbol), the bootstrap detects the
flag and branches directly to `skip_loading`, bypassing the entire floppy load
sequence. The program re-runs from the top of `do_loader` with no disk I/O.

This is why `do_init` and `do_loader` are separate entry points: `do_init` runs
only on the cold first load; `do_loader` runs on every start including restarts.
Any hardware state that must persist across a restart (and therefore must not be
re-initialised) belongs in `do_init`. Everything this demo needs is safe to
reinitialise, so `do_init` is empty.

### The `makexdf` Tool

`tools/makexdf.c` is a host-side C program that converts the flat compiled
binary into a bootable XDF floppy image. It is compiled from source by the
Makefile using the host `gcc` and run as part of every build.

**What it does, in order:**

1. **Reads** the raw binary produced by `m68k-elf-objcopy`
2. **Patches** bytes 2–63 of the image with the XDF BPB template (`id_xdf`),
   giving the disk the correct geometry description
3. **Pads** the binary to the next 8 KB boundary (matching the half-track
   chunk size the loader reads per `_B_READ` call)
4. **Back-calculates** a checksum patch value at offset `$40` such that a
   forward rotating-left add pass over the entire padded binary sums to zero
5. **Verifies** the checksum in-tool — the build fails with a non-zero result
   rather than producing a silently corrupt image
6. **Writes** the full 1,261,568-byte XDF image (the binary occupies the low
   end; the rest is zero-filled)
7. **Prints** an ASCII disk map showing track utilisation across all 154
   half-tracks (77 cylinders × 2 heads)

After every successful build you see a map like this:

```
+-------------------------------------------------------+
|  XDF DISK MAP  --  sprite_plex.xdf                    |
|  77 cyls x 2 heads x 8 KB/side = 1,232 KB capacity    |
|  # = 8 KB half-track used    . = free                 |
+-----+---+---+-----+---+---+-----+---+---+-----+---+---+
| CYL | 0 | 1 | CYL | 0 | 1 | CYL | 0 | 1 | CYL | 0 | 1 |
+-----+---+---+-----+---+---+-----+---+---+-----+---+---+
|   0 | # | # |  20 | . | . |  40 | . | . |  60 | . | . |
|   1 | # | # |  21 | . | . |  41 | . | . |  61 | . | . |
|   2 | # | # |  22 | . | . |  42 | . | . |  62 | . | . |
|   3 | . | . |  23 | . | . |  43 | . | . |  63 | . | . |
|   4 | . | . |  24 | . | . |  44 | . | . |  64 | . | . |
...
+-----+---+---+-----+---+---+-----+---+---+-----+---+---+
|  Used:   6 / 154 half-tracks    48 KB    3.9%         |
|  Free: 148 / 154 half-tracks  1184 KB   96.1%         |
+-------------------------------------------------------+
```

Each `#` represents one 8 KB half-track occupied by the payload binary. Each
`.` is free disk space. The summary line shows exactly how much of the disk's
1,232 KB capacity is consumed — useful at a glance when adding large data
assets like music, sample data, or extra sprite banks.

**Capacity headroom:** The XDF format gives ~1.2 MB of usable payload space.
The demo binary at ~48 KB uses less than 4% of it, leaving over 1.1 MB free
for game data, level maps, music sequences, and additional PCG tiles.

---

## Hardware Under Test

### Motorola 68000 CPU

- 8 MHz, 32-bit internal / 16-bit external bus
- No hardware multiply/divide for 32-bit operands; no barrel shifter
- All 32-bit modulo operations (`__umodsi3` in libgcc) use the `BSR.L`
  instruction (opcode `0x61FF`) which is **68020+ only** and causes an illegal
  instruction exception on a real 68000. All modulo in this codebase is
  replaced with compare-and-subtract or ring-counter patterns.

### PCG Sprite Hardware

The X68000 sprite system supports 128 simultaneous hardware sprites, each
16×16 pixels, 16 colours (4bpp). Hardware registers:

| Register | Address | Purpose |
|---|---|---|
| `SP_ATTR_RAM` | `0xEB0000` | 128 × 8-byte sprite attribute records |
| `SP_PAT_RAM` | `0xEB8000` | 256 × 128-byte PCG pattern tiles |
| `SP_PAL_RAM` | `0xE82200` | 16 palettes × 16 GRB555 colours |
| `SP_DISP_CPU` | `0xEB0808` | Sprite enable / display control |
| `SP_H_TOTAL` | `0xEB080A` | Sprite horizontal timing |
| `SP_H_DISP` | `0xEB080C` | Sprite horizontal display |
| `SP_V_DISP` | `0xEB080E` | Sprite vertical display |
| `SP_RES` | `0xEB0810` | Sprite resolution mode |

Each sprite attribute record (8 bytes at `SP_ATTR_RAM + slot * 8`):

```
uint16_t x;     // reg_x = screen_x + 16
uint16_t y;     // reg_y = screen_y + 16
uint16_t ctrl;  // bits 11:8 = palette index, bits 7:0 = PCG pattern index
uint16_t prio;  // bits 1:0 = display priority
```

**Coordinate system:** sprite registers use a 1-based offset. A sprite with
`reg_x = 16, reg_y = 16` appears at screen position (0, 0). The +16 offset
maps the hardware's 0-based counting onto the visible screen.

### PCG Pattern RAM

Each pattern is 128 bytes in a **4-chunk layout** for a 16×16 tile:

```
+0:  TL — top-left  8×8 quadrant  (32 bytes)
+32: BL — bot-left  8×8 quadrant  (32 bytes)
+64: TR — top-right 8×8 quadrant  (32 bytes)
+96: BR — bot-right 8×8 quadrant  (32 bytes)
```

Each 8×8 chunk encodes 8 rows × 4 byte-pairs. Each byte stores two 4-bit
palette indices (high nibble = left pixel, low nibble = right pixel). Index 0
is transparent.

### GVRAM (Graphic VRAM)

`0xC00000` — 512KB planar graphic layer, 512×512 in 4bpp mode.

This demo uses GVRAM for two overlapping effects:

1. **HBlank gradient:** GVRAM is filled with all-zero pixels (colour index 0).
   The HBlank ISR rewrites palette entry 0 on every scanline, producing a
   per-scanline blue gradient with no per-pixel CPU cost.

2. **Scrolling tile background:** A 32×32 pixel embossed "CAFE" logo tile is
   drawn into the top-left corner of GVRAM, then tiled 16×16 times to fill the
   full 512×512 canvas. CRTC registers R12/R13 (`0xE80018`/`0xE8001A`) scroll
   the layer +1 pixel per frame in both X and Y, producing smooth diagonal
   movement with perfect toroidal wrap and no CPU cost beyond writing two
   registers per frame.

Colour palette for GVRAM lives at `0xE82000` (16 entries). The sprite palettes
begin at `0xE82200` (after the 16 GVRAM palette entries).

**CRTC scroll register note:** R10/R11 (`0xE80014`/`0xE80016`) scroll the
**text** layer, not GVRAM. R12/R13 (`0xE80018`/`0xE8001A`) scroll the GVRAM
graphic layer. Using R10/R11 produces no visible scroll effect on the GVRAM.

### MFP MC68901

The MFP provides timers and GPIO interrupt lines. GPIP bit 4 signals VBlank
(0 = blanking, 1 = display active). GPIP bit 7 / I15 signals HSync (falling
edge). The demo uses:

- **VBlank polling** (`MFP_GPIP_B` at `0xE88001`) for frame synchronisation
  in the main loop. Word reads from `0xE88000` are not handled correctly in
  MAME; only byte reads from the odd address work.
- **HBlank interrupt** (GPIP7 / vector `0x4F` / address `0x13C`) for the
  per-scanline background gradient. The ISR writes one word to `GVRAM_PAL`
  per scanline, updating colour 0's GRB555 value.

### CRTC and Video Controller

The demo runs in **256×256 31 kHz 16-colour mode** (mode index 2). Key
register groups:

- `CRTC_R00`–`CRTC_R08` (`0xE80000`): horizontal and vertical timing
- `CRTC_R10`/`CRTC_R11` (`0xE80014`/`0xE80016`): **text** layer H/V scroll
- `CRTC_R12`/`CRTC_R13` (`0xE80018`/`0xE8001A`): **GVRAM** graphic layer H/V scroll
- `CRTC_R20` (`0xE80028`): resolution and colour depth selector
- `VC_R0`–`VC_R2` (`0xE82400`–`0xE82600`): layer enable and priority

`VC_R2 = 0x00C1` enables GVRAM layer 0 (bit 0), sprite layer (bit 6), and text
layer (bit 7).

---

## Demo Phases

The demo cycles through three phases automatically, looping forever.

### Phase 0 — Sine-Wave Ripple Grid (360 frames)

100 sprites are arranged in a 10×10 grid filling the entire visible screen
area. Every frame, each sprite is displaced by a sine-wave offset computed
from its column, row, and the current frame counter:

```
angle  = (frame * 2 + col * 13 + row * 23) & 0xFF
offset_x = sine_table[angle]
offset_y = sine_table[(angle + 64) & 0xFF]
```

The 64-entry phase shift between X and Y produces circular wobble per sprite.
The `col * 13` and `row * 23` prime-multiplied offsets ensure no two sprites
ever share the same wave phase, creating a fluid rolling ripple across the
grid.

Every 3 frames `pat_counter` increments, shifting which PCG pattern each slot
displays. Because `ctrl = (slot + pat_counter) & 0x7F` offsets each slot
differently, the pattern shift travels diagonally across the grid.

### Phase 1 — Circle Rotation (360 frames)

All 100 sprites are placed on a pre-computed circle path and rotate around it
in unison. One sprite advances per frame (`circle_rot` increments 0–99,
wrapping without modulo), creating smooth CCW rotation.

The circle path is stored as a static 100-entry lookup table generated offline
with Bresenham's midpoint circle algorithm (see [Key Algorithms](#key-algorithms)).
There is no floating-point arithmetic anywhere in the binary.

Circle parameters (256-mode):
- Centre: screen(160, 64) → reg(176, 80)
- Radius: 55 pixels
- Coverage: screen_y 9–119 (fully within visible area)

### Phase 2 — Decompose Transition (completion-driven)

When the circle phase ends, the demo enters a transition rather than snapping
instantly back to the grid. The circle **keeps rotating** while sprites are
progressively launched toward their grid home positions:

- One sprite is "launched" every 7 frames
- At the moment of launch, its starting position is snapshotted from the
  current circle position
- Each launched sprite moves **2 pixels per frame** in both X and Y toward
  its grid centre
- Once it arrives, it immediately joins the sine-wave ripple
- The transition completes when all 100 sprites have arrived (~700+ frames
  depending on path lengths)

This produces the visual effect of the rotating ring dissolving and
crystallising into the grid. The phase is **completion-driven** — it exits
when the last sprite lands, not at a fixed frame count.

### HUD

Sprite slots 120–123 display a 4-digit hex counter (flush top-right) showing
`PLEX_TOTAL` in hex throughout all phases. The font is a custom 16×16 PCG
hex font stored in PCG slots 128–144. The HUD uses palette 1 (white) to
distinguish it from the plex sprites on palette 0.

---

## GVRAM Background Layer

The background layer is implemented as a self-contained, reusable module:
`include/x68k_bg.h` / `src/x68k_bg.c`. It is designed to be dropped into any
X68000 project without modification.

### API Overview

```c
// Palette
void bg_set_pal(int idx, uint16_t grb555);
void bg_load_pal(const uint16_t *src, int start, int count);
void bg_grey_ramp(int start, int count);  // linear dark→bright grey ramp

// Fill (see also gvram_fill_dma() in x68k_video.h for DMA-accelerated clear)
void bg_fill(uint8_t color_idx);          // CPU-fill 512×512 GVRAM

// Pixel (inline, zero-overhead)
static inline void bg_pset(int x, int y, uint8_t c);

// Tile drawing from 128-byte PCG patterns
void bg_draw_pcg(int gx, int gy, const uint8_t *pcg, uint8_t fg, uint8_t bg_c);
void bg_draw_pcg_embossed(int gx, int gy, const uint8_t *pcg,
                           uint8_t hi, uint8_t mid, uint8_t shadow, uint8_t bg_c);

// Tiling (no division/modulo — pure wrap-counter)
void bg_tile_region(int tile_w, int tile_h);

// Tiling — DMA-accelerated (HD63450 linked-array cascade, single DMA call)
void bg_tile_region_dma(int tile_w, int tile_h);

// Hardware scroll (CRTC R12/R13, toroidal wrap 0–511)
void bg_set_scroll(uint16_t x, uint16_t y);
void bg_scroll_step(int16_t dx, int16_t dy);
```

### The CAFE Tile

The demo uses four letters from the built-in 16×16 hex font PCG data to
construct a 32×32 logo tile:

```
C A
F E
```

- **C** = hex digit 12 = `FONT_PCG(12)`, placed at GVRAM (0, 0)
- **A** = hex digit 10 = `FONT_PCG(10)`, placed at GVRAM (16, 0)
- **F** = hex digit 15 = `FONT_PCG(15)`, placed at GVRAM (0, 16)
- **E** = hex digit 14 = `FONT_PCG(14)`, placed at GVRAM (16, 16)

### Emboss Effect

Each letter is drawn with `bg_draw_pcg_embossed()` using a **directional
light-source kernel** — the upper-left diagonal neighbour of each pixel
determines its shade:

| Pixel | Neighbour | Output |
|-------|-----------|--------|
| ON | OFF | `hi` (highlight — edge facing the light) |
| OFF | ON | `shadow` (dark edge turned away from light) |
| ON | ON | `mid` (interior, uniformly lit) |
| OFF | OFF | `bg_c` (background — transparent for gradient show-through) |

Parameters used: `hi=15, mid=8, shadow=1, bg_c=0` over a 15-entry grey ramp
(`bg_grey_ramp(1, 15)`), giving a raised-metal appearance.

Because `bg_c=0` is also the animated HBlank gradient colour, the open areas
between glyphs show the blue gradient through the embossed tile, blending both
effects seamlessly.

### HBlank Gradient

GVRAM is first filled entirely with colour index 0 via `bg_fill(0)`. Palette
entry 0 starts at black. The HBlank ISR (`hblank_handler` in `x68k_video.c`)
fires on every falling edge of HSync (MFP GPIP7, vector `0x4F`). On each
scanline it writes a new GRB555 value to `GVRAM_PAL` (palette entry 0 at
`0xE82000`):

```c
// 24 gradient bands × 16 scanlines each = 384 scanlines covered
if (s < 384)
    REG16(GVRAM_PAL) = sky_colors[s >> 4];
else
    REG16(GVRAM_PAL) = 0x8088;   // twilight green ground (off-screen)
```

The `sky_colors[24]` table is a pure-blue ramp in GRB555 (`B` field only,
`G=R=0`):

```c
static const uint16_t sky_colors[24] = {
    0x0004, 0x0006, 0x000A, 0x000E,  //  0- 3: near-black → dark blue
    0x0014, 0x001C, 0x0026, 0x0032,  //  4- 7: mid → vivid blue  ← visible
    0x003A, 0x003C, 0x003E, 0x003E,  //  8-11: saturated (off-screen)
    // ... 0x003E through band 23
};
```

In the MAME "Disk Drive and Keyboard LEDs" view only approximately 128 lines
are visible (bands 0–7, scanlines 0–127). The gradient therefore runs from
near-black blue at the very top to vivid saturated blue near the bottom of the
visible window.

Because all GVRAM pixels are colour index 0, every pixel on screen takes its
colour from palette entry 0 — which the ISR changes 128 times per frame. The
CPU cost is **one word write per scanline**; no pixel data is ever rewritten.

### Tiling

Once the 32×32 CAFE tile is drawn into GVRAM at (0, 0),
`bg_tile_region_dma(32, 32)` replicates it across the full 512×512 canvas.

- **Pass 1 — horizontal (CPU):** For each of the 32 source rows, the 32
  source pixels are copied rightward to column 511 using a wrap counter.
  Result: 512 ÷ 32 = 16 tile copies per row. No division or modulo.
- **Pass 2 — vertical (DMA):** A single HD63450 channel 2 DMA call using a
  480-entry linked-array chain cascades the 32 completed rows downward across
  the remaining 480 rows in one shot.

The DMA cascade exploits the same mechanism as `gvram_fill_dma`: the DAR
(source pointer) auto-increments through GVRAM as each chain entry completes.
Chain entry `i` reads row `i` (already correct from either the original tile
or a prior cascade copy) and writes it to row `32 + i`. Because the DMA
processes the chain sequentially, each newly written row becomes the correct
source for the chain entry 32 positions later — the tile pattern propagates
naturally to fill all 512 rows.

Because 512 is exactly divisible by 32, the tiling is perfectly seamless with
no partial tile anywhere in the 512×512 canvas.

The background is constructed with **GVRAM hidden** (`VC_R2 = 0x00C0`,
bit 0 off) so nothing is visible on screen during the fill, draw, and tile
sequence. Once complete, `VC_R2 = 0x00C1` reveals the layer — the fully-tiled
background appears in a single frame with no visible construction.

### How the Two Effects Combine

The emboss parameters `bg_c=0` is the key. Any GVRAM pixel that is "off"
(not part of a letter glyph) is written as colour index 0 — the same entry
animated by the HBlank ISR. So the gradient bleeds through all the gaps
between letters and tiles. The lit glyph pixels (hi/mid/shadow) use palette
entries 1–15 (the grey ramp), which are static.

The visual result: grey embossed CAFE lettering, raised-metal appearance,
floating over a continuously shifting blue gradient background. The gradient
changes horizontally (scanline by scanline) while the tile pattern moves
diagonally. The two effects are independent — the gradient is driven by
interrupt, the tile motion by CRTC registers.

### Hardware Scroll

The X68000 CRTC R12/R13 registers (`0xE80018`/`0xE8001A`) define the
top-left corner of the GVRAM viewport into the 512×512 canvas. Incrementing
both by 1 each frame shifts the visible window one pixel right and one pixel
down — the pattern appears to scroll diagonally (45°) toward the bottom-right.

When the scroll position reaches 511, incrementing by 1 wraps it back to 0:
the hardware enforces this 9-bit (0–511) toroidal wrap automatically. Because
the tile is 32 pixels wide and 512 ÷ 32 = 16, the wrap point coincides exactly
with a tile boundary — the scroll is completely seamless, no seam ever appears.

`bg_scroll_step` maintains the scroll position in software (`bg_sx`, `bg_sy`)
using integer arithmetic with compare-and-subtract wrap (no division/modulo):

```c
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
    REG16(CRTC_R12) = bg_sx;  // write to hardware scroll registers
    REG16(CRTC_R13) = bg_sy;
}
```

The signed `int16_t` arithmetic allows negative step values for reverse scroll
in any direction. The per-frame CPU cost is **two CRTC register writes** — no
pixel data is ever moved. `bg_scroll_step(1, 1)` is called once per VBlank at
the end of the main loop.

### Typical Initialisation Sequence

```c
REG16(VC_R2) = 0x00C0;        // hide GVRAM layer during construction

gvram_fill_dma(0);             // DMA-clear 512×512 GVRAM to colour 0
bg_grey_ramp(1, 15);           // palette entries 1–15 = grey ramp
bg_draw_pcg_embossed( 0,  0, FONT_PCG(12), 15, 8, 1, 0);  // C
bg_draw_pcg_embossed(16,  0, FONT_PCG(10), 15, 8, 1, 0);  // A
bg_draw_pcg_embossed( 0, 16, FONT_PCG(15), 15, 8, 1, 0);  // F
bg_draw_pcg_embossed(16, 16, FONT_PCG(14), 15, 8, 1, 0);  // E
bg_tile_region_dma(32, 32);    // DMA cascade tile to full 512×512

init_hblank();                 // install HBlank ISR before reveal
REG16(VC_R2) = 0x00C1;        // reveal — full background appears instantly

// Once per VBlank in the main loop:
bg_scroll_step(1, 1);
```

---

## Project Structure

```
x68_sprite_plex_c/
├── src/
│   ├── main.c            Entry point: do_init / do_loader stubs + demo launch
│   ├── start.s           M68K boot loader (floppy load, checksum, BSS clear)
│   ├── sprite_plex.c     All demo logic: phases, PCG init, sprite updates
│   ├── x68k_video.c      Video init, VBlank poll, DMA fill, HBlank ISR
│   ├── x68k_bg.c         GVRAM background layer implementation
│   ├── uhe_stub.c        Unhandled exception handler (halts with register dump)
│   ├── memset.c          Minimal memset for BSS clear
│   └── memsize.s         Exports __bss_start / _end symbols for linker
├── include/
│   ├── types.h           Standalone int types (uint8_t etc.) — no stdint.h
│   ├── x68k_hw.h         All hardware register addresses and REG8/16/32 macros
│   ├── x68k_bg.h         GVRAM background layer API (palette, fill, PCG, scroll)
│   └── x68k_video.h      Public API: init_video, wait_vblank, gvram_fill_dma,
│                         init_hblank
├── data/
│   ├── spritesheet_16x16.png   Source sprite art (256 tiles, 16×16 each)
│   ├── sprite_patterns.s       Generated: 256 PCG patterns (32 KB)
│   ├── sprite_palette.s        Generated: 15-colour GRB555 palette
│   ├── font_16x16_hex_data.s   Hand-crafted hex font (0–F + 'x', 17 tiles)
│   └── font_palette.s          Font palette (white on transparent)
├── tools/
│   ├── convert_sprites.py      PNG → PCG assembler data converter
│   └── makexdf.c               XDF floppy image builder (BPB patch, checksum, disk map)
├── cfg/
│   └── x68000.cfg              MAME configuration (floppy path, display)
├── roms/
│   └── x68000/                 MAME ROM files (iplrom.dat, cgrom.dat)
├── Makefile
└── README.md                   This document
```

### Generated Files

`sprite_patterns.s` and `sprite_palette.s` are produced by
`tools/convert_sprites.py` from `data/spritesheet_16x16.png`. They are
committed to the repository so the build does not require Python unless the
source art changes. If you modify the PNG, run `make sprites` to regenerate.

---

## Build System

The Makefile handles the complete pipeline from C/asm sources to bootable
floppy image:

```
make            # build sprite_plex.xdf (256×256 mode)
make DEMO_RES=512x512   # build in 512×512 mode (120 sprites, 15×8 grid)
make run        # build + launch in MAME
make sprites    # regenerate PCG data from spritesheet_16x16.png
make clean      # wipe obj/ and build/
```

**Build pipeline:**

1. `m68k-elf-as` assembles `start.s`, `memsize.s`, and all `data/*.s` files
2. `m68k-elf-gcc` compiles all `src/*.c` files to `obj/*.o`
3. `m68k-elf-ld` links everything at `0x2000` (the X68000 load address),
   garbage-collects unused sections, and produces `sprite_plex.elf`
4. `m68k-elf-objcopy` strips the ELF to a raw binary, padded to an 8 KB
   boundary (matching the loader's half-track chunk size)
5. `tools/makexdf` (see [The `makexdf` Tool](#the-makexdf-tool)) patches the
   BPB, computes the checksum, writes the full 1,261,568-byte XDF image, and
   prints the ASCII disk map

**Compiler flags of note:**

```makefile
CFLAGS = -m68000 -march=68000 -mcpu=68000 -mtune=68000
CFLAGS += -std=c99 -Os -fomit-frame-pointer -ffunction-sections
CFLAGS += -fno-strict-aliasing
```

`-ffunction-sections` combined with `--gc-sections` in the linker eliminates
dead code. `-fno-strict-aliasing` is necessary because the hardware register
access macros (`REG16`, `REG8`) cast integer addresses to volatile pointers,
which violates strict aliasing rules.

### types.h — No stdint.h

The cross-compiler's `stdint.h` is not used. `types.h` declares all integer
types manually:

```c
typedef unsigned char  uint8_t;
typedef unsigned short uint16_t;
typedef unsigned long  uint32_t;
```

On M68K with GCC, `long` is always 32 bits. This avoids any dependency on
the newlib headers bundled with the toolchain.

---

## Toolchain Setup

You need a bare-metal M68K ELF cross-compiler. The project was built and tested
with **m68k-elf-gcc 13.3.0** on macOS (Homebrew).

```bash
brew install m68k-elf-gcc    # or build from source for other platforms
```

Verify the toolchain:

```bash
m68k-elf-gcc --version
m68k-elf-as  --version
m68k-elf-ld  --version
```

You also need:

- **Python 3 + Pillow** for sprite conversion: `pip3 install Pillow`
- **MAME** with X68000 support: `brew install mame`
- X68000 ROM files placed in `roms/x68000/`:
  - `iplrom.dat` (IPL ROM, SHA1: `2b3a8d9a`)
  - `cgrom.dat` (CG ROM)

---

## Running the Demo

```bash
make run
```

This builds the XDF disk image and launches MAME with the correct flags:

```
mame x68000 -flop1 build/sprite_plex.xdf \
    -rompath roms -skip_gameinfo -window -nomaximize \
    -resolution 1024x768 -bios ipl10
```

**Important — MAME display mode:** Use the **"Disk Drive and Keyboard LEDs"**
view in MAME (accessed via the in-emulator menu). The default view shows the
full 768×512 output buffer; the artwork view crops to the actual visible game
area (approximately 256×128 pixels in 256-mode) which is what the demo was
calibrated for.

The demo boots directly from floppy. The IPL ROM loads the binary at `0x2000`,
verifies the checksum, clears BSS, disables hardware interrupts, and calls
`do_loader`, which then sets up video, DMA-clears GVRAM, installs the HBlank
ISR, initialises sprites, and enters the main loop. The floppy is ejected after
load.

---

## Technical Architecture

### Coordinate System

The X68000 sprite system uses **register coordinates** that are offset by +16
from screen coordinates:

```
reg_x = screen_x + 16
reg_y = screen_y + 16
```

A sprite at `reg_x = 0, reg_y = 0` is off the top-left corner of the screen.
This offset exists so that sprites partially off the left or top edge can be
represented without signed arithmetic in the hardware.

Throughout the code, `BASE_X`, `BASE_Y`, and the circle LUT all store reg
coordinates directly. Conversions between screen and reg are handled at data
generation time, not at runtime.

### Visible Area Calibration

In 256-mode with the MAME "Disk Drive and Keyboard LEDs" artwork view, the
visible game area is approximately **256 pixels wide × 128 pixels tall**
(screen_y 0–127). The full CRTC output is 256×256, but the case artwork
crops the lower half. All grid and circle positions were calibrated to this
visible window.

The 10×10 sprite grid:
- X: `BASE_X = 27` (screen_x 11), step 26 → covers screen_x 11–245
- Y: `BASE_Y = 22` (screen_y 6),  step 12 → covers screen_y 6–114

### PCG Pattern Management

The hardware provides 256 PCG pattern slots (0–255). This demo uses:

- Slots **0–127**: sprite plex patterns, loaded at startup in a carefully
  chosen order that prioritises visual uniqueness
- Slots **128–144**: 16×16 hex font (17 characters: `0`–`9`, `A`–`F`, `x`)

The source spritesheet contains 256 sprites, but only **50 distinct pixel
shapes** exist across them (the rest are colour variants of the same shapes).
The `pcg_init_order[128]` table loads all 50 unique shapes into the first 50
PCG slots, giving 50 different shapes across 100 on-screen sprites rather than
the ~16 that would appear with sequential loading.

Every 216 frames (~4 seconds), a background batch swap copies 64 new patterns
into PCG slots 64–127, cycling through three batches of source patterns. This
ensures the upper half of the grid continues to evolve visually even within a
single demo phase.

### Sprite Palette Cycling

The `ctrl` word formula `(slot + pat_counter) & 0x7F` means:

- Each sprite slot uses a different PCG pattern (uniqueness)
- As `pat_counter` increments (every 3 frames), the mapping shifts, creating
  a diagonal wave of pattern changes across the grid

The palette index in `ctrl` bits 11:8 is kept at 0 for all plex sprites. The
font sprites use palette 1.

### GRB555 Colour Format

The X68000 uses a non-standard colour encoding where the channel order is
**G–R–B** rather than R–G–B:

```c
grb555 = (G5 << 11) | (R5 << 6) | (B5 << 1)
```

Where G5, R5, B5 are 5-bit values (0–31). Bit 0 is unused (always 0). This
differs from most contemporary systems and is a common source of wrong colours
when porting assets.

### Frame Pacing

The main loop calls `wait_vblank()` once per iteration. This polls
`REG8(MFP_GPIP_B)` (byte read from `0xE88001`, bit 4) in a two-phase spin:
wait until bit 4 is high (active display), then wait until it drops (VBlank
starts). This gives a clean, jitter-free frame sync at the CRTC's native rate
(~54 Hz in 31 kHz mode).

The frame rate is inherently limited to one iteration per VBlank. In practice
the demo runs comfortably at full rate: 100 sprite attribute writes + 100 sine
lookups + PCG batch management all complete well within a single frame budget.

---

## Key Algorithms

### Bresenham Midpoint Circle

The circle path is generated offline using Bresenham's integer midpoint circle
algorithm — no floating-point is involved anywhere in the final binary.

The algorithm generates the full set of integer-coordinate points on a circle
of radius _r_ around a centre, then 100 evenly-spaced points are sampled from
that set (sorted CCW from the east pole) and stored as a static `uint8_t`
lookup table compiled into the ROM image.

At runtime, `update_circle_sprites()` simply indexes into this table:

```c
uint8_t pos = (uint8_t)(slot + circle_rot);
if (pos >= 100) pos -= 100;   // wrap without modulo
sa[slot].x = circle_lut[pos][0];
sa[slot].y = circle_lut[pos][1];
```

The rotation is achieved by offsetting each sprite's LUT index by `circle_rot`,
which increments by 1 per frame. All 100 sprites remain on the circle path;
each simply occupies a different position on it.

### Sine Table

A 256-entry signed-byte sine table with amplitude ±20 pixels is stored in ROM.
Lookups use 8-bit wraparound arithmetic naturally — `(uint8_t)(angle + 64)`
gives the 90° phase shift for the Y axis without any range checking.

```c
static const int8_t sine_table[256] = { /* amplitude ±20 */ };
```

### 68000-Safe Modulo

The M68K `__umodsi3` libgcc function uses `BSR.L` (68020+ only). Every
counter that would naively use `%` is replaced with either:

```c
// Ring counter (for values wrapping at a power of two)
value = (value + 1) & 0x7F;

// Compare-and-subtract (for arbitrary wrap values like 100)
if (++value >= 100) value = 0;
```

No `%` operator with a non-power-of-two operand appears anywhere in the
codebase.

### Sprite Motion in the Decompose Phase

The decompose transition uses a simple chasing algorithm per sprite:

```c
int16_t error_x = target_x - current_x;
if      (error_x >  SPEED) current_x += SPEED;
else if (error_x < -SPEED) current_x -= SPEED;
else                        current_x  = target_x;
```

Applied independently to X and Y (2 pixels/frame each axis). When both axes
reach the target, the sprite transitions to the full sine-wave animation.
No square roots, no floating point, no per-sprite timer — just integer
clamping.

---

## Hard-Won Fixes and Gotchas

These issues were discovered during development and are worth knowing before
writing any X68000 code.

### 1. `BSR.L` in libgcc — Instant Illegal Instruction

Any `%` operation with a non-constant, non-power-of-two divisor on a 32-bit
value will call `__umodsi3` from libgcc. That function contains `BSR.L`
(a 68020 long-displacement branch), which raises an illegal instruction
exception on the 68000.

**Symptom:** The demo halts immediately after boot with no visible output.

**Fix:** Replace all such operations with compare-and-subtract wrappers or
bit-mask wrappers. Audit with `m68k-elf-objdump -d` and `grep 0x61ff` if
you suspect hidden calls.

### 2. MFP Word Read Broken in MAME

Reading the MFP status registers as a 16-bit word from `0xE88000` is not
emulated correctly in MAME. The VBlank bit always returns the wrong value,
causing the frame sync loop to hang forever.

**Fix:** Read as a byte from the **odd** address `0xE88001`:

```c
// WRONG — hangs in MAME
while (REG16(MFP_GPIP) & 0x0010);

// CORRECT
while (REG8(MFP_GPIP_B) & 0x10);
```

### 3. `MOVE #imm,SR` in Inline Assembly

GCC's M68K inline assembler emits a relocation for the symbol `sr` when you
write `__asm__("move.w #0x2500,sr")`, rather than encoding the status register
reference directly. This produces an "undefined reference to `sr`" linker
error — but only when the function is actually linked (previously it was
dead-stripped).

**Fix:** Emit the raw opcode bytes:

```c
// WRONG — produces undefined reference to `sr` when linked
__asm__ volatile("move.w #0x2500,sr");

// CORRECT — MOVE.W #0x2500,SR opcode
__asm__ volatile(".word 0x46FC, 0x2500");
```

### 4. Visible Screen Area in MAME

The X68000 CRTC in 256-mode outputs 256 vertical lines, but the MAME
"Disk Drive and Keyboard LEDs" artwork view (the most faithful to the real
machine with its case) exposes only approximately **128 lines** (screen_y 0–127).
Sprite positions must be calibrated for this window, not the full CRTC height.

### 5. GVRAM DMA Fill Format

`gvram_fill_dma()` fills GVRAM with longword writes. In 4bpp mode, each
longword holds 8 pixels (2 pixels per byte, 4 pixels per word). To fill with
colour index N:

```c
uint32_t fill = (value << 16) | value;  // N in both 16-bit halves
```

Where `value` is the colour index repeated as two adjacent 16-bit pixels
(each 16-bit value holds 4 pixels at 4bpp). The DMA then copies this pattern
across all 512 rows.

---

## Using This as a Game Foundation

This codebase was designed to be the lowest layer of a sprite-based game. Here
is what is already proven and ready to use.

### What You Get Out of the Box

| Component | Status |
|---|---|
| Full-disk track loader (both sides, 77 cyl, ~1.2 MB capacity) | Complete |
| Video mode initialisation (256 and 512 modes) | Complete |
| Sprite attribute RAM layout and addressing | Verified |
| PCG pattern upload (byte-by-byte) | Verified |
| Palette loading (GRB555, 16 banks) | Verified |
| VBlank synchronisation | Verified |
| Per-scanline HBlank gradient effect | Verified |
| GVRAM background layer module (x68k_bg) | Complete |
| Embossed PCG tile drawing | Verified |
| Full-screen GVRAM tiling — CPU+DMA cascade (no modulo) | Verified |
| Hardware diagonal scroll (CRTC R12/R13) | Verified |
| DMA GVRAM fill | Verified |
| 128-sprite slot management | Demonstrated |
| PCG batch swap (background pattern streaming) | Demonstrated |
| Sprite animation with sine displacement | Demonstrated |
| Fixed-point sprite motion (no floats) | Demonstrated |
| 16×16 hex font on PCG sprites | Complete |
| GRB555 colour conversion tool | Complete |
| PNG spritesheet → PCG data converter | Complete |
| XDF floppy image builder with disk map | Complete |

### Recommended Architecture for a Game

#### Sprite Object System

Replace the fixed `slot` assignment with a sprite object table:

```c
typedef struct {
    uint16_t x, y;        // world position (fixed-point: upper 8 bits = pixels)
    uint16_t vx, vy;      // velocity
    uint8_t  pcg;         // current PCG pattern index
    uint8_t  anim_frame;  // animation frame counter
    uint8_t  type;        // object type (player, enemy, bullet, etc.)
    uint8_t  active;      // 0 = free slot
} game_obj_t;

static game_obj_t objects[PLEX_TOTAL];
```

The `update_all_sprites()` pattern maps naturally onto this: iterate over
active objects, compute screen position from world position, write to
`SP_ATTR_RAM`.

#### Input Handling

The X68000 keyboard and joystick are accessed via the IOCS (IPL OS Call System)
or directly via `0xE9A001` (keyboard) and `0xE9C001` (joystick). For a
bare-metal game, poll the joystick register each frame before the sprite
update:

```c
uint8_t joy = REG8(0xE9C001);
// bit 0: up, bit 1: down, bit 2: left, bit 3: right, bit 4: fire
if (!(joy & 0x08)) player.vx = -2;  // left
if (!(joy & 0x04)) player.vx =  2;  // right
```

#### Sound

The X68000 has an OPM (YM2151) FM synthesiser at `0xE90001` / `0xE90003` and
an ADPCM chip (MSM6258V) at `0xE92001`. Both can be triggered from the main
loop or from a timer interrupt. The OPM is the primary tool for music and sound
effects in any serious game on this hardware.

#### Scrolling Background

The `x68k_bg` module provides a complete, reusable GVRAM background layer ready
for game use. Draw any tile pattern into the top-left corner of GVRAM, call
`bg_tile_region(w, h)` once to fill the 512×512 canvas, then call
`bg_scroll_step(dx, dy)` once per VBlank to scroll at any speed and direction
with automatic toroidal wrap. No redraw is ever needed.

For a game tilemap background, replace the 32×32 logo tile with a 64×64 (or
larger, as long as 512 is divisible by it) rendered tilemap chunk. The hardware
scroll handles the rest.

For parallax effects, change the CRTC horizontal start register inside a
mid-screen HBlank handler to scroll different rows at different speeds.

#### Collision Detection

At 100 sprites with simple rectangular hitboxes, brute-force AABB testing
(O(n²)) is feasible: 100×100 = 10,000 comparisons, each just four 16-bit
subtracts and compares. On 8 MHz 68000 that is approximately 80,000 cycles,
which fits comfortably in a 148,000-cycle frame budget at 54 Hz.

For more objects, partition by type: only check player bullets against enemies,
etc. This reduces the active comparison count by 80–90%.

#### PCG Animation

To animate a sprite, increment its `pcg` field every N frames and update
`sa[slot].ctrl = (palette << 8) | pcg`. Store animation sequences as small
arrays of PCG indices:

```c
static const uint8_t explosion_frames[] = { 32, 33, 34, 35, 36, 37 };
```

With 256 PCG slots minus font (17) and minus fixed graphics, you have up to
~220 slots for game art. The batch-swap pattern already in this demo shows how
to stream new patterns in from a larger pool at runtime.

---

## Extending the Demo

### More Sprites

The hardware supports 128 simultaneous sprites. The demo uses 100 for the
plex grid plus 4 for the HUD. The remaining 24 slots are free. In 512×512
mode (`make DEMO_RES=512x512`) the grid expands to 15×8 = 120 sprites.

### Additional Circle Paths

A second Lissajous or spiral path can be swapped in as a new demo phase.
Generate the path offline in Python, embed as a second static LUT, add a new
`demo_phase` value, and add the corresponding update function. The decompose
transition generalises to any target position — it just needs a grid origin
per slot.

### Music Synchronisation

Install an OPM timer interrupt (MFP Timer B, vector `0x4C`) alongside the
HBlank interrupt. Inside the timer ISR, step through a music sequence. Because
both ISRs are short, both can coexist with the VBlank-paced main loop.

### Real Floppy / Real Hardware

The XDF image produced by `make` is compatible with real X68000 hardware using
a USB floppy emulator (e.g. Gotek with FlashFloppy firmware) or by transferring
the `.xdf` file to genuine 2HD media. No BIOS calls remain in the binary after
boot, so it runs identically on real hardware as in MAME — this was an explicit
design goal.

With ~1.2 MB of payload capacity, the loader can hold a complete small game
including code, PCG tiles, music data, and sample banks without compressing
or splitting data across multiple disks.

---

## Commit History Summary

| Commit | Description |
|---|---|
| Initial | Port from assembly: C99 video init, sprite plex grid, HUD |
| Bug fix | VBlank poll: REG16→REG8 on MFP_GPIP (MAME word-read issue) |
| Bug fix | pat_counter: replace `frame % 3` with pat_tick counter (BSR.L) |
| Feature | 10×10 grid (100 sprites), tighter spacing, full screen coverage |
| Feature | pcg_init_order: load 50 unique shapes first for visual variety |
| Feature | Phase 1: Bresenham circle rotation, 360-frame cycle |
| Fix | Circle centre: calibrated to visible area screen_y 0–128 |
| Feature | Phase 2: decompose transition, sprites glide to grid (2px/frame) |
| Feature | HBlank gradient background (pure blue, dark→vivid) |
| Fix | SR inline asm: use .word opcode to avoid GAS relocation bug |
| Tuning | Circle centre shifted +32px right for better visual balance |
| Feature | x68k_bg module: palette, fill, PCG draw (flat + embossed), tiling, scroll |
| Feature | CAFE tile: embossed 32×32 logo tiled 16×16 across 512×512 GVRAM |
| Fix | GVRAM scroll: corrected from CRTC R10/R11 (text) to R12/R13 (GVRAM) |
| Docs | README: full background layer documentation, API reference, tile design |
| Feature | Full-disk track loader: both sides, 8KB/half-track, 77 cyl, ~1.2MB capacity |
| Feature | makexdf: drop 2HQ, full XDF capacity, ASCII disk map with track utilisation |
| Feature | bg_tile_region_dma: HD63450 cascade chain tiles all 512 rows in one DMA call |
| Feature | Background construction off-screen (VC_R2 hide/reveal) for instant appearance |
