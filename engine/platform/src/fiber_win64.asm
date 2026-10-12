; SPDX-License-Identifier: Apache-2.0
; Fiber context switch for the Windows x64 ABI (docs/adr/0010-fibers.md).
;
; Saves on the current stack what the ABI makes the callee responsible for:
; rbx, rbp, rdi, rsi, r12-r15, xmm6-xmm15, MXCSR, plus the three stack
; bounds the kernel and the CRT read from the thread information block
; (StackBase, StackLimit, DeallocationStack), then stores the stack pointer
; through from_sp, loads to_sp, and restores the same from the target
; stack. A `ret` then continues the target where it last called this
; function, or, for a fresh fiber, enters the trampoline.
;
; The x87 control word is deliberately not switched: FLDCW is a serializing
; microcoded instruction that costs more than the rest of the switch
; together (docs/adr/0010-fibers.md §8), and x64 code never uses x87.
;
; Frame built by nph_fiber_switch_asm on the current stack (offsets from
; the saved rsp, all 16-byte aligned for movaps):
;     +0   xmm6 ... +144 xmm15
;     +160 mxcsr (4)   +164 unused
;     +168 StackBase   +176 StackLimit   +184 DeallocationStack
;     +200 r15 +208 r14 +216 r13 +224 r12 +232 rsi +240 rdi +248 rbx +256 rbp
;     +264 return address
; fiber.c lays out exactly this for a new fiber with r12 = the NphFiber and
; the return address = nph_fiber_trampoline_asm.

.code

EXTERN nph_fiber_entry:PROC

; void nph_fiber_switch_asm(void **from_sp /* rcx */, void *to_sp /* rdx */)
PUBLIC nph_fiber_switch_asm
nph_fiber_switch_asm PROC
    push rbp
    push rbx
    push rdi
    push rsi
    push r12
    push r13
    push r14
    push r15
    sub rsp, 200
    movaps xmmword ptr [rsp+0], xmm6
    movaps xmmword ptr [rsp+16], xmm7
    movaps xmmword ptr [rsp+32], xmm8
    movaps xmmword ptr [rsp+48], xmm9
    movaps xmmword ptr [rsp+64], xmm10
    movaps xmmword ptr [rsp+80], xmm11
    movaps xmmword ptr [rsp+96], xmm12
    movaps xmmword ptr [rsp+112], xmm13
    movaps xmmword ptr [rsp+128], xmm14
    movaps xmmword ptr [rsp+144], xmm15
    stmxcsr dword ptr [rsp+160]
    mov rax, qword ptr gs:[8]
    mov qword ptr [rsp+168], rax
    mov rax, qword ptr gs:[16]
    mov qword ptr [rsp+176], rax
    mov rax, qword ptr gs:[1478h]
    mov qword ptr [rsp+184], rax

    mov qword ptr [rcx], rsp
    mov rsp, rdx

    mov rax, qword ptr [rsp+168]
    mov qword ptr gs:[8], rax
    mov rax, qword ptr [rsp+176]
    mov qword ptr gs:[16], rax
    mov rax, qword ptr [rsp+184]
    mov qword ptr gs:[1478h], rax
    ldmxcsr dword ptr [rsp+160]
    movaps xmm6, xmmword ptr [rsp+0]
    movaps xmm7, xmmword ptr [rsp+16]
    movaps xmm8, xmmword ptr [rsp+32]
    movaps xmm9, xmmword ptr [rsp+48]
    movaps xmm10, xmmword ptr [rsp+64]
    movaps xmm11, xmmword ptr [rsp+80]
    movaps xmm12, xmmword ptr [rsp+96]
    movaps xmm13, xmmword ptr [rsp+112]
    movaps xmm14, xmmword ptr [rsp+128]
    movaps xmm15, xmmword ptr [rsp+144]
    add rsp, 200
    pop r15
    pop r14
    pop r13
    pop r12
    pop rsi
    pop rdi
    pop rbx
    pop rbp
    ret
nph_fiber_switch_asm ENDP

; Entered through the `ret` of the first switch into a new fiber, with
; r12 = NphFiber * and rsp 16-byte aligned (one slot above the zero "return
; address" that ends stack walks). Allocates the 32-byte shadow space the
; ABI requires and calls nph_fiber_entry(fiber), which never returns.
PUBLIC nph_fiber_trampoline_asm
nph_fiber_trampoline_asm PROC
    sub rsp, 32
    mov rcx, r12
    call nph_fiber_entry
    int 3
nph_fiber_trampoline_asm ENDP

END
