/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Fiber context switch for the System V x86-64 ABI (docs/adr/0010-fibers.md).
 *
 * Saves the callee-saved registers rbx, rbp, r12-r15 and MXCSR on the
 * current stack, stores the stack pointer through from_sp, loads to_sp, and
 * restores the same from the target stack. The vector registers are all
 * caller-saved in this ABI, so nothing else needs saving. The x87 control
 * word is deliberately not switched: FLDCW is a serializing microcoded
 * instruction that costs more than the rest of the switch together
 * (docs/adr/0010-fibers.md §8), and the engine never uses x87 nor changes
 * its control word. The `ret` continues the target where it last called
 * this function, or enters the trampoline for a fresh fiber.
 *
 * Frame built on the current stack (offsets from the saved rsp):
 *     +0  mxcsr (4)  +4 unused
 *     +8  r15  +16 r14  +24 r13  +32 r12  +40 rbx  +48 rbp
 *     +56 return address
 * fiber.c lays out exactly this for a new fiber with r12 = the NphFiber and
 * the return address = nph_fiber_trampoline_asm.
 */

    .intel_syntax noprefix
    .text

    .globl nph_fiber_switch_asm
    .type nph_fiber_switch_asm, @function
nph_fiber_switch_asm:
    .cfi_startproc
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 8
    stmxcsr dword ptr [rsp]

    mov qword ptr [rdi], rsp
    mov rsp, rsi

    ldmxcsr dword ptr [rsp]
    add rsp, 8
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
    .cfi_endproc
    .size nph_fiber_switch_asm, .-nph_fiber_switch_asm

/*
 * Entered through the `ret` of the first switch into a new fiber, with
 * r12 = NphFiber * and rsp 16-byte aligned as the ABI requires at a call.
 * The .cfi_undefined rip marks this as the outermost frame so unwinders
 * (gdb, the sanitizers' symbolizer) stop here instead of walking into the
 * zero word above.
 */
    .globl nph_fiber_trampoline_asm
    .type nph_fiber_trampoline_asm, @function
nph_fiber_trampoline_asm:
    .cfi_startproc
    .cfi_undefined rip
    mov rdi, r12
    call nph_fiber_entry
    ud2
    .cfi_endproc
    .size nph_fiber_trampoline_asm, .-nph_fiber_trampoline_asm

    .section .note.GNU-stack, "", @progbits
