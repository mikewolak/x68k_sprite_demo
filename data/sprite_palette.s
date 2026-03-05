################################################################################
# sprite_palette.s - Sprite palette for sprite_plex demo
# 16 entries: index 0 = transparent (black), 1-15 = sprite colours
# GRB555 format: (G<<11)|(R<<6)|(B<<1)
################################################################################

    align 4
    global sprite_palette_grb555
sprite_palette_grb555:
    dc.w $0000  # 0: transparent / black
    dc.w $0000  # 1: R=  7 G=  7 B=  7
    dc.w $CE6C  # 2: R=205 G=204 B=178
    dc.w $81F4  # 3: R= 58 G=130 B=211
    dc.w $E690  # 4: R=210 G=231 B= 66
    dc.w $2984  # 5: R= 50 G= 41 B= 19
    dc.w $5350  # 6: R=108 G= 87 B= 64
    dc.w $6E8E  # 7: R=214 G=111 B= 56
    dc.w $BD7E  # 8: R=168 G=187 B=254
    dc.w $214E  # 9: R= 40 G= 38 B= 58
    dc.w $2D66  # 10: R=168 G= 41 B=154
    dc.w $A7EE  # 11: R=255 G=166 B=190
    dc.w $EF7A  # 12: R=237 G=237 B=237
    dc.w $8464  # 13: R=140 G=130 B=150
    dc.w $3404  # 14: R=128 G= 55 B= 16
    dc.w $57AE  # 15: R=245 G= 83 B=186

