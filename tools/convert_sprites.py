#!/usr/bin/env python3
"""
convert_sprites.py - Convert 16x16 sprite sheet PNG to X68000 PCG format

Usage: python3 convert_sprites.py <input.png> <patterns.s> <palette.s>

Input:  RGBA PNG sprite sheet, 16-wide grid of 16x16 sprites
Output:
  sprite_patterns.s - 256 sprites in PCG 4-chunk format (256 * 128 = 32768 bytes)
  sprite_palette.s  - 15-colour GRB555 palette (+ 1 transparent = 16 entries)

PCG 4-chunk layout per 16x16 tile (128 bytes):
  +0:  TL chunk  rows 0-7,  cols 0-7   (32 bytes)
  +32: BL chunk  rows 8-15, cols 0-7   (32 bytes)
  +64: TR chunk  rows 0-7,  cols 8-15  (32 bytes)
  +96: BR chunk  rows 8-15, cols 8-15  (32 bytes)

Each 8x8 chunk (32 bytes): 8 rows x 4 bytes.
Each byte: high nibble = left pixel colour index, low nibble = right pixel colour index.
Colour index 0 = transparent.
"""

import sys
import struct
from PIL import Image

# ---------------------------------------------------------------------------

SPRITE_W = 16
SPRITE_H = 16
GRID_COLS = 16
NUM_SPRITES = 256    # first 256 sprites from the sheet
MAX_COLORS = 15      # colours 1-15; colour 0 = transparent

# ---------------------------------------------------------------------------

def rgb_to_grb555(r, g, b):
    """Convert 8-bit RGB to X68000 GRB555 word: (G<<11)|(R<<6)|(B<<1)"""
    g5 = (g >> 3) & 0x1F
    r5 = (r >> 3) & 0x1F
    b5 = (b >> 3) & 0x1F
    return (g5 << 11) | (r5 << 6) | (b5 << 1)

def color_distance(c1, c2):
    """Euclidean distance in RGB space"""
    return ((c1[0]-c2[0])**2 + (c1[1]-c2[1])**2 + (c1[2]-c2[2])**2)

def kmeans_quantize(colors, k, max_iter=50):
    """Simple k-means quantization, returns list of k centroids as (R,G,B)"""
    import random
    random.seed(42)
    # Seed from evenly-spaced unique colors
    unique = list(dict.fromkeys(colors))
    step = max(1, len(unique) // k)
    centroids = [unique[i * step] for i in range(k)][:k]
    while len(centroids) < k:
        centroids.append(unique[len(centroids)])

    for _ in range(max_iter):
        clusters = [[] for _ in range(k)]
        for c in colors:
            nearest = min(range(k), key=lambda i: color_distance(c, centroids[i]))
            clusters[nearest].append(c)
        new_centroids = []
        for i, cluster in enumerate(clusters):
            if cluster:
                r = round(sum(x[0] for x in cluster) / len(cluster))
                g = round(sum(x[1] for x in cluster) / len(cluster))
                b = round(sum(x[2] for x in cluster) / len(cluster))
                new_centroids.append((r, g, b))
            else:
                new_centroids.append(centroids[i])
        if new_centroids == centroids:
            break
        centroids = new_centroids
    return centroids

# ---------------------------------------------------------------------------

def extract_sprites(img, num_sprites):
    """Extract num_sprites 16x16 tiles as lists of (R,G,B,A) pixels."""
    sprites = []
    for i in range(num_sprites):
        col = i % GRID_COLS
        row = i // GRID_COLS
        x0 = col * SPRITE_W
        y0 = row * SPRITE_H
        tile = img.crop((x0, y0, x0 + SPRITE_W, y0 + SPRITE_H))
        pixels = list(tile.getdata())   # list of (R,G,B,A)
        sprites.append(pixels)
    return sprites

def build_palette(sprites):
    """Collect all opaque colours, quantize to MAX_COLORS, return palette list."""
    opaque_colors = []
    for pixels in sprites:
        for r, g, b, a in pixels:
            if a >= 128:
                opaque_colors.append((r, g, b))

    unique_colors = list(dict.fromkeys(opaque_colors))
    print(f"  Total opaque pixels: {len(opaque_colors)}, unique colours: {len(unique_colors)}")

    if len(unique_colors) <= MAX_COLORS:
        palette = unique_colors[:MAX_COLORS]
        # Pad if fewer than MAX_COLORS
        while len(palette) < MAX_COLORS:
            palette.append((0, 0, 0))
        print(f"  Using all {len(unique_colors)} colours (no quantization needed)")
    else:
        print(f"  Quantizing {len(unique_colors)} colours to {MAX_COLORS}...")
        palette = kmeans_quantize(unique_colors, MAX_COLORS)

    return palette  # list of 15 (R,G,B) tuples, index 0 = palette entry 1

def nearest_palette_index(r, g, b, palette):
    """Return 1-based index into palette (1-15). Index 0 = transparent."""
    best = min(range(len(palette)), key=lambda i: color_distance((r, g, b), palette[i]))
    return best + 1  # 1-based

def sprite_to_pcg(pixels, palette):
    """
    Convert a list of 256 (R,G,B,A) pixels (16x16) to PCG 4-chunk format (128 bytes).
    Returns bytearray of 128 bytes.
    """
    # Map each pixel to a 4-bit colour index
    indices = []
    for r, g, b, a in pixels:
        if a < 128:
            indices.append(0)  # transparent
        else:
            indices.append(nearest_palette_index(r, g, b, palette))

    def encode_chunk(chunk_rows, chunk_cols):
        """Encode an 8x8 chunk from given row/col ranges. Returns 32 bytes."""
        data = bytearray()
        for row in chunk_rows:
            for pair in range(4):  # 4 pairs of pixels per row = 8 pixels
                col_left  = chunk_cols[pair * 2]
                col_right = chunk_cols[pair * 2 + 1]
                left  = indices[row * SPRITE_W + col_left]
                right = indices[row * SPRITE_W + col_right]
                data.append((left << 4) | right)
        return data

    rows_top    = list(range(0, 8))
    rows_bottom = list(range(8, 16))
    cols_left   = list(range(0, 8))
    cols_right  = list(range(8, 16))

    result = bytearray()
    result += encode_chunk(rows_top,    cols_left)   # TL: +0
    result += encode_chunk(rows_bottom, cols_left)   # BL: +32
    result += encode_chunk(rows_top,    cols_right)  # TR: +64
    result += encode_chunk(rows_bottom, cols_right)  # BR: +96
    assert len(result) == 128
    return result

# ---------------------------------------------------------------------------

def write_palette_s(path, palette):
    """Write sprite_palette.s: 16-entry GRB555 table (entry 0 = transparent)."""
    lines = [
        "################################################################################",
        "# sprite_palette.s - Sprite palette for sprite_plex demo",
        "# 16 entries: index 0 = transparent (black), 1-15 = sprite colours",
        "# GRB555 format: (G<<11)|(R<<6)|(B<<1)",
        "################################################################################",
        "",
        "    align 4",
        "    global sprite_palette_grb555",
        "sprite_palette_grb555:",
        "    dc.w $0000  # 0: transparent / black",
    ]
    for i, (r, g, b) in enumerate(palette):
        word = rgb_to_grb555(r, g, b)
        lines.append(f"    dc.w ${word:04X}  # {i+1}: R={r:3d} G={g:3d} B={b:3d}")
    lines.append("")
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"  Wrote {path}")

def write_patterns_s(path, all_pcg_bytes):
    """Write sprite_patterns.s: 256 sprites * 128 bytes each = 32768 bytes."""
    lines = [
        "################################################################################",
        "# sprite_patterns.s - PCG sprite patterns for sprite_plex demo",
        f"# {NUM_SPRITES} sprites x 128 bytes = {NUM_SPRITES * 128} bytes",
        "# 4-chunk format: TL(+0) BL(+32) TR(+64) BR(+96), each 8x8 pixels",
        "# Colour index 0 = transparent, 1-15 = palette entries",
        "################################################################################",
        "",
        "    align 4",
        "    global sprite_patterns",
        "sprite_patterns:",
    ]

    for sprite_idx, pcg in enumerate(all_pcg_bytes):
        lines.append(f"    # Sprite {sprite_idx}")
        for i in range(0, 128, 16):
            chunk = pcg[i:i+16]
            hex_vals = ",".join(f"${b:02X}" for b in chunk)
            lines.append(f"    dc.b {hex_vals}")
    lines.append("")
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"  Wrote {path} ({NUM_SPRITES * 128} bytes)")

# ---------------------------------------------------------------------------

def main():
    if len(sys.argv) != 4:
        print(f"Usage: {sys.argv[0]} <input.png> <patterns.s> <palette.s>")
        sys.exit(1)

    png_path      = sys.argv[1]
    patterns_path = sys.argv[2]
    palette_path  = sys.argv[3]

    print(f"Loading {png_path}...")
    img = Image.open(png_path).convert("RGBA")
    w, h = img.size
    print(f"  Image size: {w}x{h}")

    total_available = (w // SPRITE_W) * (h // SPRITE_H)
    print(f"  Available sprites: {total_available} ({w // SPRITE_W} cols x {h // SPRITE_H} rows)")
    if total_available < NUM_SPRITES:
        print(f"  WARNING: only {total_available} sprites available, reducing to {total_available}")

    n = min(NUM_SPRITES, total_available)

    print(f"Extracting {n} sprites...")
    sprites = extract_sprites(img, n)

    print("Building palette...")
    palette = build_palette(sprites)

    print("Converting sprites to PCG format...")
    all_pcg = [sprite_to_pcg(pixels, palette) for pixels in sprites]

    print("Writing output files...")
    write_palette_s(palette_path, palette)
    write_patterns_s(patterns_path, all_pcg)

    print("Done.")

if __name__ == "__main__":
    main()
