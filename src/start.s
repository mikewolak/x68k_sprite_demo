################################################################################
#
# Starting point for the executable
#
    global _start
_start:
    #
    # Required '$60' (short branch) header byte.
    # If this isn't here, the IPL ROM will refuse to boot from it.
    #
    dc.b $60
    dc.b ((start_code-_start)-2)

    #
    # Leave some room here for makexdf to mess with the media format
    #
    dc.w   0,0,0,0,0,0,0
    dc.w 0,0,0,0,0,0,0,0
    dc.w 0,0,0,0,0,0,0,0
    dc.w 0,0,0,0,0,0,0
    #
    # Bit 0 = have we already loaded?
    #
loaded_flag:
    dc.b 0
    dc.b 0
    #
    # The next 4 bytes are used by makexdf to create the checksum
    #
    dc.l 0

start_code:
    #
    # Set startup IPL stack pointer for now
    #
    lea ($2000),sp

    #
    # If we've already loaded, skip this next part
    #
    lea (loaded_flag,pc),a0
    btst.b #0,(a0)
    bne skip_loading

    ################################################################################
    #
    # Loader
    #
    # To accommodate different floppy formats, the memtest68k binary is split into
    # 4K chunks, each stored at the beginning of a track, on one side.
    #

    #
    # Get the boot device in d7
    #
    moveq #$FFFFFF8E,d0 /* _BOOTINF */
    trap #15
    moveq #$70,d1
    lsl.w #8,d0
    or.w d0,d1 /* d1 = device number for _B_READ */
    move.l d1,d7

    #
    # Get the executable size in d6
    #
    move.l #((__bss_start-_start)-1),d6
    lsr.l #8,d6
    lsr.l #4,d6 /* d6 = number of 4K sections, minus one */

    #
    # Get the starting location in d5
    # (either 512 or 1024-byte sectors, based on media ID)
    #
    clr.l d5
    move.b (_start+$15,pc),d5 /* $FE=xdf, $F0=2HQ */
    lsr.b #3,d5
    and.b #$3,d5
    ror.l #8,d5
    addq.l #1,d5 /* start at sector #1 */

    #
    # Destination pointer in a6
    #
    move.w #$2000,a6

    #
    # Read the rest of the executable
    #
exe_load_loop:
    move.l a6,a1     /* a1 = destination pointer */
    move.w d7,d1     /* d1 = device */
    move.l d5,d2     /* d2 = disk location */
    move.l #$1000,d3 /* d3 = size, 4K at a time */
    moveq #$46,d0    /* _B_READ */
    trap #15

    lea ($1000,a6),a6 /* advance destination */
    add.l #$10000,d5  /* advance to the next track */

    dbra d6,exe_load_loop

    #
    # Eject the boot device (presumably floppy)
    #
    move.l d7,d1
    moveq #$4f,d0 /* _B_EJECT */
    trap #15

    #
    # Checksum what we just loaded
    #
    lea (_start,pc),a0
    move.l #($3FF+__bss_start-_start),d0
    and.w #$FC00,d0
    lea (a0,d0.l),a1
    clr.l d0
checksum_loop:
    rol.l #1,d0
    add.l (a0)+,d0
    cmp.l a1,a0
    blo checksum_loop
    tst.l d0
    beq checksum_ok
    lea (checksum_bad_str,pc),a1
    moveq.l #$21,d0
    trap #15
checksum_bad:
    bra checksum_bad
checksum_bad_str:
    dc.b 'Checksum failed; program is corrupt or didn',39,'t load properly',0
    align 2
checksum_ok:

    #
    # Clear BSS
    #
    movea.l #__bss_start,a0
    movea.l #_end,a1
    sub.l a0,a1
    pea (a1)
    clr.l -(sp)
    pea (a0)
    bsr (memset,pc)
    lea 12(sp),sp

    ########################################
    #
    # Obtain complete control - disable hardware interrupts
    #
    move.w #$2700,sr

    #
    # Disable CPU caches, if any
    #
    lea (cache_end,pc),a0
    move.l a0,($10) /* illegal instruction */

    dc.w $4E7A,$1002 /* movec cacr,d1 */
    or.w #$0808,d1
    dc.w $4E7B,$1002 /* movec d1,cacr */
    clr.l d1
    dc.w $4E7B,$1002 /* movec d1,cacr */
cache_end:
    movea.l #my_stack_top,sp

    #
    # Set up unhandled exception table
    #
    bsr (uhe_setup,pc)

    #
    # Turn off the floppy LEDs/motors.
    # The IPL ROM will not get a chance to do this after we disable interrupts.
    #
    lea ($E94005),a0
    move.b #$4F,(a0)
    move.b #$40,(a0)
    addq.l #2,a0
    move.b #$03,(a0)
    move.b #$02,(a0)
    move.b #$01,(a0)
    move.b #$00,(a0)

    #
    # Set RTC to bank 0
    #
    bclr.b #0,($e8a01b)

    #
    # Switch to custom stack
    #
    movea.l #my_stack_top,sp

    #
    # We are now done loading
    #
    lea (loaded_flag,pc),a0
    bset.b #0,(a0)

    #
    # Run first-time initialization
    #
    bsr (do_init,pc)

skip_loading:
    #
    # Switch to custom stack
    #
    movea.l #my_stack_top,sp

    #
    # Set up unhandled exception table (again)
    #
    bsr (uhe_setup,pc)

    #
    # Run loader main loop
    #
    bsr (do_loader,pc)

    #
    # If we get here, reboot I guess
    #
    global reboot
reboot:
    move.w #$2700,sr
    move.l ($FF0004),a0
    move.l ($FF0000),sp
    jmp (a0)

    #
    # Restart without reloading
    #
    global restart
restart:
    bra skip_loading

################################################################################

################################################################################
#
# Relocate and restart
#
    align 2
    global relocate
relocate:
    addq.l #4,sp        /* (skip return) */
    lea (_start,pc),a0  /* a0 = old address */
    move.l (sp),a1      /* a1 = new address */
    move.l a1,a2        /* a2 is where we'll jump later */

    move.l #($3+_end-_start),d0
    and.b #$FC,d0       /* d0 = size */

    cmp.l a0,a1
    beq relocate_none
    bhi relocate_backwards

relocate_forwards:
    lsr.l #2,d0
    subq.l #1,d0
relocate_forwards_loop:
    move.l (a0)+,(a1)+
    dbra d0,relocate_forwards_loop
relocate_none:
    jmp (a2)

relocate_backwards:
    add.l d0,a0
    add.l d0,a1
    lsr.l #2,d0
    subq.l #1,d0
relocate_backwards_loop:
    move.l -(a0),-(a1)
    dbra d0,relocate_backwards_loop
    jmp (a2)

################################################################################
#
# Unhandled exception handler
#
    align 2
uhe_setup:
    lea (0),a0
    lea (uhe_redirect,pc),a1
    move.w #255,d0
uhe_setup_loop:
    move.l a1,(a0)+
    addq.l #4,a1
    dbra d0,uhe_setup_loop
    rts

uhe_redirect:

    macro uhetable4
    bsr (uhe_entry,pc)
    bsr (uhe_entry,pc)
    bsr (uhe_entry,pc)
    bsr (uhe_entry,pc)
    endm
    macro uhetable16
    uhetable4
    uhetable4
    uhetable4
    uhetable4
    endm
    macro uhetable64
    uhetable16
    uhetable16
    uhetable16
    uhetable16
    endm
    uhetable64
    uhetable64
    uhetable64
    uhetable64

uhe_entry:
    move.w #$2700,sr
    movem.l d0-d7/a0-a6,-(sp)
    lea (64,sp),a0
    pea (a0)
    move.l usp,a0
    pea (a0)
    #
    # 0  4  8  12 16 20 24 28 32 36 40 44 48 52 56 60 64 68 72
    # us ss d0 d1 d2 d3 d4 d5 d6 d7 a0 a1 a2 a3 a4 a5 a6 V# exc_frame
    #
    # Convert vector number
    #
    lea (uhe_redirect+4,pc),a0
    move.l (68,sp),d0
    sub.l a0,d0
    lsr.l #2,d0
    move.l d0,(68,sp)
    #
    # Go
    #
    bsr (uhe,pc)
uhe_loop:
    bra uhe_loop

################################################################################

    bss
    ds.b 1024
my_stack_top:

################################################################################
