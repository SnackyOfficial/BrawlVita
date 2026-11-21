@ Entry point of every logging thunk (see make_thunk in elf_loader.c).
@ On entry: r12 = id of the unresolved import, r0-r3 = the caller's arguments, lr = the caller's return address.
@ Calls bv_unresolved(id, lr) in C, then returns 0 to the caller (r0 = result, r1 = 0 so a double result is 0.0).
    .syntax unified
    .arm
    .text
    .align 2
    .global bv_unresolved_entry
    .type bv_unresolved_entry, %function
bv_unresolved_entry:
    push    {r0-r4, lr}
    mov     r0, r12
    mov     r1, lr
    bl      bv_unresolved
    add     sp, sp, #8
    pop     {r2-r4, lr}
    mov     r1, #0
    bx      lr
    .size bv_unresolved_entry, .-bv_unresolved_entry
