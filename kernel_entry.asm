[bits 32]

[extern kmain]
[extern scheduler_tick]
[extern keyboard_irq_handler]
[extern syscall_handler]

[global _start]
[global irq0_stub]
[global irq1_stub]
[global default_irq_stub]
[global page_fault_stub]
[global syscall_stub]
[global user_program_start]
[global user_program_end]

section .text

_start:
    call kmain

.hang:
    cli
    hlt
    jmp .hang

irq0_stub:
    pusha
    cld
    push esp
    call scheduler_tick
    add esp, 4
    mov esp, eax
    popa
    iretd

irq1_stub:
    pusha
    cld
    call keyboard_irq_handler
    popa
    iretd

syscall_stub:
    pusha
    cld
    push esp
    call syscall_handler
    add esp, 4
    popa
    iretd

default_irq_stub:
    pusha
    mov al, 0x20
    out 0x20, al
    popa
    iretd

page_fault_stub:
    cli
.hang:
    hlt
    jmp .hang

; Small position-independent Ring 3 program.
; EAX=3 -> sys_yield. It intentionally produces no console output.
section .text.user_program
user_program_start:
    mov eax, 3
.user_loop:
    int 0x80
    jmp .user_loop
user_program_end:
