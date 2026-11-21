.text
.p2align 2
.globl _coop_nxnative_invoke_game_call
_coop_nxnative_invoke_game_call:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    sub sp, sp, #32
    mov x9, x0
    ldr x16, [x9]
    ldp x10, x11, [x9, #72]
    ldp x12, x13, [x9, #88]
    stp x10, x11, [sp]
    stp x12, x13, [sp, #16]
    ldr s0, [x9, #104]
    ldp x0, x1, [x9, #8]
    ldp x2, x3, [x9, #24]
    ldp x4, x5, [x9, #40]
    ldp x6, x7, [x9, #56]
    blr x16
    add sp, sp, #32
    ldp x29, x30, [sp], #16
    ret
