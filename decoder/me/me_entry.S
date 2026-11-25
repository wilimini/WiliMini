/* MIST completion convention: mcidclan, MIT; see dependency LICENSE.md.
 * The firmware handler enters here with the MeJob in a1. Unwind the complete
 * C worker call before returning to the firmware handler. */
.set noreorder
.text
.align 2
.global me_mist_entry
.ent me_mist_entry
me_mist_entry:
    addiu $sp, $sp, -16
    sw $ra, 0($sp)
    jal mefp_me_worker
    nop
    lw $ra, 0($sp)
    addiu $sp, $sp, 16
    sync
    li $t0, 0xbfc00700
    li $t1, -567
    li $t9, 0x883004c0
    jr $t9
    sw $t1, 0($t0)
.end me_mist_entry
