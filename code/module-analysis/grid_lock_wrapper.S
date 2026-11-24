.arch armv8.3-a
.section .text.tb375fc_grid_lock,"ax",%progbits
.p2align 2
.global tb375fc_grid_lock_wrapper
.hidden tb375fc_grid_lock_wrapper
.type tb375fc_grid_lock_wrapper,%function
tb375fc_grid_lock_wrapper:
.cfi_startproc
paciasp
.cfi_negate_ra_state
stp x29,x30,[sp,#-128]!
.cfi_def_cfa_offset 128
.cfi_offset 29,-128
.cfi_offset 30,-120
mov x29,sp
stp x19,x20,[sp,#16]
.cfi_offset 19,-112
.cfi_offset 20,-104
stp x0,x1,[sp,#32]
stp x2,x3,[sp,#48]
stp x4,x5,[sp,#64]
stp x6,x7,[sp,#80]
str x8,[sp,#112]
adrp x19,ts
ldr x19,[x19,:lo12:ts]
add x19,x19,#0xf8
mov x0,x19
bl mutex_lock
ldp x0,x1,[sp,#32]
ldp x2,x3,[sp,#48]
ldp x4,x5,[sp,#64]
ldp x6,x7,[sp,#80]
ldr x8,[sp,#112]
bl nvt_edge_grid_zone_set
stp x0,x1,[sp,#96]
mov x0,x19
bl mutex_unlock
ldp x0,x1,[sp,#96]
ldp x19,x20,[sp,#16]
.cfi_restore 19
.cfi_restore 20
ldp x29,x30,[sp],#128
.cfi_restore 29
.cfi_restore 30
.cfi_def_cfa_offset 0
autiasp
.cfi_negate_ra_state
ret
.cfi_endproc
.size tb375fc_grid_lock_wrapper,.-tb375fc_grid_lock_wrapper
.section .note.gnu.property,"a"
.p2align 3
.long 4,16,5
.asciz "GNU"
.long 0xc0000000,4,2,0
.section .note.GNU-stack,"",%progbits
