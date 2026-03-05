################################################################################
#
# Probe for the amount of main memory
#
################################################################################

    align 2
    global probe_main_memory
probe_main_memory:
    #
    # Save the stack pointer
    #
    move.l sp,d1
    #
    # Install a new bus error handler; save the old one in a1
    #
    move.l (8),a1
    lea (probe_main_memory_buserror,pc),a0
    move.l a0,(8)
    #
    # Read in 4KiB increments until we get a bus error or hit 12MiB,
    # whichever comes first
    #
    lea (0),a0
probe_main_memory_busloop:
    tst.l (a0)
    add.w #$1000,a0
    cmp.l #$C00000,a0
    bne probe_main_memory_busloop
probe_main_memory_buserror:
    move.l d1,sp
    move.l a0,d0

    #
    # Install second bus error handler
    #
    lea (probe_main_memory_buserror2,pc),a0
    move.l a0,(8)
    #
    # Test to see if this memory actually works
    #
probe_main_memory_test:

    pea (_end,pc)
    or.w #$FFC,(2,sp)
    move.l (sp)+,a0
    bra probe_main_memory_loop1t
probe_main_memory_loop1:
    move.l a0,(a0)
    add.w #$1000,a0
probe_main_memory_loop1t:
    cmp.l d0,a0
    blo probe_main_memory_loop1

    pea (_end,pc)
    or.w #$FFC,(2,sp)
    move.l (sp)+,a0
    bra probe_main_memory_loop2t
probe_main_memory_loop2:
    cmp.l (a0),a0
    bne probe_main_memory_testfail
    add.w #$1000,a0
probe_main_memory_loop2t:
    cmp.l d0,a0
    blo probe_main_memory_loop2

    #
    # That all succeeded
    #

#
# Restore the old stack pointer and bus error handler and return
#
probe_main_memory_done:
    move.l a1,(8)
    move.l d1,sp
    rts

#
# Write/read test failed - decrease memory by 4KiB and try again
#
probe_main_memory_testfail:
    sub.l #$1000,d0
    bhi probe_main_memory_test
    clr.l d0
    bra probe_main_memory_done

#
# Whatever was at a0 is the new memory amount
#
probe_main_memory_buserror2:
    move.l a0,d0
    bra probe_main_memory_done

################################################################################
