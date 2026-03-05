# X68000 Sprite Plex Demo

A bare-metal PCG sprite multiplexer for the Sharp X68000, written entirely in
C99 (M68000-targeted). It displays 100 simultaneous 16×16 PCG sprites across
three animated phases and serves as a verified foundation for hardware-accurate
sprite-based game development on this platform.

---

## Table of Contents

1. [What This Is](#what-this-is)
2. [Track Loader Boot Mechanism](#track-loader-boot-mechanism)
3. [Hardware Under Test](#hardware-under-test)
4. [Demo Phases](#demo-phases)
5. [Project Structure](#project-structure)
6. [Build System](#build-system)
7. [Toolchain Setup](#toolchain-setup)
8. [Running the Demo](#running-the-demo)
9. [Technical Architecture](#technical-architecture)
10. [Key Algorithms](#key-algorithms)
11. [Hard-Won Fixes and Gotchas](#hard-won-fixes-and-gotchas)
12. [Using This as a Game Foundation](#using-this-as-a-game-foundation)
13. [Extending the Demo](#extending-the-demo)

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
describes the floppy geometry. `makexdf.c` patches this area with a
pre-computed template (`id_xdf` or `id_2hq`) matching the target format:

| Format | Total size | Sectors/track | Sector size | Tracks |
|--------|-----------|---------------|-------------|--------|
| XDF | 1,261,568 B | 8 per side | 1024 bytes | 77 |
| 2HQ | 1,474,560 B | 18 per side | 512 bytes | 80 |

Critically, byte `$15` of the BPB is the **media ID byte** (`$FE` for XDF,
`$F0` for 2HQ). The bootstrap reads this at runtime to determine the sector
size and therefore how many sectors make up one track — it uses this to
calculate the disk address of each successive 4 KB chunk:

```asm
move.b (_start+$15,pc),d5   ; read media ID from own boot sector
lsr.b  #3,d5                ; shift to extract sector-size class
and.b  #$3,d5               ; mask to 2-bit field
ror.l  #8,d5                ; place into high word as sector address component
addq.l #1,d5                ; start at sector 1 (sector 0 is the boot sector)
```

This single media ID byte is all the bootstrap needs to support both floppy
formats from the same binary.

### Track Layout — One 4 KB Chunk Per Track

The core trick is in `makexdf.c`. After computing the checksum, it
**interleaves** the binary so that each 4 KB piece lands at the start of a
separate track, leaving the rest of that track empty:

```c
// Place 4K chunk i at the beginning of track i, one side only
memmove(image + (bytes_per_track * i), image + (0x1000 * i), 0x1000);
memset (image + (0x1000 * i), 0, 0x1000);   // zero out the old location
```

For XDF (`bytes_per_track = 16,384`): chunk 1 lands at byte 16384 (track 1),
chunk 2 at byte 32768 (track 2), and so on. For 2HQ (`bytes_per_track =
18,432`): the same logic applies with 18,432-byte track strides.

The result is that the binary occupies only the **first sector of each track**,
on one side of the disk. The rest of each track is zero-filled. This is wasteful
of disk space but simple and works identically on both media formats.

### The Bootstrap Load Loop

Once the IPL ROM hands control to the boot sector at `0x2000`, the bootstrap
runs its own loader loop. It already has the first 4 KB (track 0) in memory.
It then reads every remaining track into successive 4 KB windows:

```asm
exe_load_loop:
    move.l a6,a1        ; a1 = destination (starts at 0x2000, advances 4K each)
    move.w d7,d1        ; d1 = boot device number (from _BOOTINF BIOS call)
    move.l d5,d2        ; d2 = disk address (track 1, sector 1, advancing by track)
    move.l #$1000,d3    ; d3 = 4096 bytes
    moveq  #$46,d0      ; _B_READ BIOS call
    trap   #15

    lea    ($1000,a6),a6    ; advance destination pointer by 4K
    add.l  #$10000,d5       ; advance disk address by one full track
    dbra   d6,exe_load_loop ; repeat for all remaining chunks
```

`d6` is initialised to `(binary_size / 4096) - 1` so the loop runs exactly
the right number of times. `d7` holds the boot device number obtained from the
`_BOOTINF` BIOS call earlier, so the loader reads from whichever drive the
machine booted from.

After the last chunk is read, the floppy is ejected (`_B_EJECT`), the
checksum is verified, BSS is zeroed with `memset`, all hardware interrupts are
disabled (`move.w #$2700,sr`), and control passes to `do_init` then
`do_loader`.

### Checksum

`makexdf.c` computes a rotating-left add-accumulate checksum over the entire
padded binary and patches the result into offset `$40` of the boot sector
(within the BPB area) such that the same checksum loop run over the loaded
binary produces zero:

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

`0xC00000` — 512KB planar graphic layer, 512×512 in 4bpp mode. The background
gradient uses GVRAM: it is cleared to all-zero pixels (colour index 0), and the
HBlank ISR changes what colour index 0 resolves to on each scanline. This gives
a per-scanline gradient with no per-pixel cost.

Colour palette for GVRAM lives at `0xE82000`. The sprite palettes begin at
`0xE82200` (after the 16 GVRAM palette entries).

The HD63450 DMA controller fills GVRAM via a linked-array chain: the CPU fills
row 0, then the DMA copies row N → row N+1 for 511 iterations, completing a
full 512KB clear in hardware.

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

## Project Structure

```
x68_sprite_plex_c/
├── src/
│   ├── main.c            Entry point: do_init / do_loader stubs + demo launch
│   ├── start.s           M68K boot loader (floppy load, checksum, BSS clear)
│   ├── sprite_plex.c     All demo logic: phases, PCG init, sprite updates
│   ├── x68k_video.c      Video init, VBlank poll, DMA fill, HBlank ISR
│   ├── uhe_stub.c        Unhandled exception handler (halts with register dump)
│   ├── memset.c          Minimal memset for BSS clear
│   └── memsize.s         Exports __bss_start / _end symbols for linker
├── include/
│   ├── types.h           Standalone int types (uint8_t etc.) — no stdint.h
│   ├── x68k_hw.h         All hardware register addresses and REG8/16/32 macros
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
│   └── makexdf.c               XDF/2HQ floppy image creator
├── cfg/
│   └── x68000.cfg              MAME configuration (floppy path, display)
├── roms/
│   └── x68000/                 MAME ROM files (iplrom.dat, cgrom.dat)
├── Makefile
└── DEMO.md                     This document
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
make            # build sprite_plex.xdf and sprite_plex.2hq (256×256 mode)
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
4. `m68k-elf-objcopy` strips the ELF to a raw binary, padded to a 1 KB boundary
5. `tools/makexdf` wraps the binary into an XDF floppy image with the correct
   media ID byte, sector layout, and checksum so the X68000 IPL ROM accepts it

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
| Boot loader (floppy, checksum, BSS clear) | Complete |
| Video mode initialisation (256 and 512 modes) | Complete |
| Sprite attribute RAM layout and addressing | Verified |
| PCG pattern upload (byte-by-byte) | Verified |
| Palette loading (GRB555, 16 banks) | Verified |
| VBlank synchronisation | Verified |
| Per-scanline HBlank gradient effect | Verified |
| DMA GVRAM fill | Verified |
| 128-sprite slot management | Demonstrated |
| PCG batch swap (background pattern streaming) | Demonstrated |
| Sprite animation with sine displacement | Demonstrated |
| Fixed-point sprite motion (no floats) | Demonstrated |
| 16×16 hex font on PCG sprites | Complete |
| GRB555 colour conversion tool | Complete |
| PNG spritesheet → PCG data converter | Complete |
| XDF/2HQ floppy image builder | Complete |

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

For scrolling levels, use the GVRAM text plane (32×32 character cells,
hardware-scrolled via `0xE80010`–`0xE80014`). The sprite layer sits in front,
so GVRAM functions as a free-scrolling tilemap background with no CPU cost per
frame beyond setting the scroll registers.

Alternatively, use the CRTC raster scroll trick: change the CRTC horizontal
start register inside a mid-screen HBlank handler to scroll different rows at
different speeds (parallax).

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
