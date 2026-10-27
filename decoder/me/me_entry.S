/* MIST completion convention: mcidclan, MIT; see dependency LICENSE.md.
 * Unwind our complete C call before returning to the firmware handler. */
.set noreorder
.text
.align 2
.global me_task
.ent me_task
me_task:
    addiu $sp, $sp, -16
    sw $ra, 0($sp)
    jal me_worker
    nop
    lw $ra, 0($sp)
    addiu $sp, $sp, 16
    sync
    li $t0, 0xbfc00700
    li $t1, -567
    li $t9, 0x883004c0
    jr $t9
    sw $t1, 0($t0)
.end me_task
