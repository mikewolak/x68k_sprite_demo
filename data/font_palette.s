# 16-color grayscale palette for font rendering
# GRB555 format for X68000

    align 4
    global font_palette_grb555
font_palette_grb555:
    dc.w $0000  # 0: Transparent/Black
    dc.w $1042  # 1: Very dark gray
    dc.w $2084  # 2: Dark gray
    dc.w $30C6  # 3: Dark-medium gray
    dc.w $4108  # 4: Medium-dark gray
    dc.w $514A  # 5: Medium gray
    dc.w $618C  # 6: Medium-light gray
    dc.w $71CE  # 7: Light-medium gray
    dc.w $8210  # 8: Light gray
    dc.w $9252  # 9: Lighter gray
    dc.w $A294  # 10: Very light gray
    dc.w $B2D6  # 11: Near white
    dc.w $C318  # 12: Almost white
    dc.w $D35A  # 13: Brightest gray
    dc.w $E39C  # 14: Nearly pure white
    dc.w $FFFF  # 15: Pure white
