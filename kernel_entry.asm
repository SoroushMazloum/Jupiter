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
[extern exception_handler]

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

; ---- CPU exception stubs (vectors 0-19) ----
; Exceptions with an error code already have it on the stack; for the others
; we push a dummy 0 so the frame layout is always the same.
%macro EXC_NOERR 1
exc%1_stub:
    push dword 0
    push dword %1
    jmp exception_common
%endmacro

%macro EXC_ERR 1
exc%1_stub:
    push dword %1
    jmp exception_common
%endmacro

EXC_NOERR 0
EXC_NOERR 1
EXC_NOERR 2
EXC_NOERR 3
EXC_NOERR 4
EXC_NOERR 5
EXC_NOERR 6
EXC_NOERR 7
EXC_ERR   8
EXC_NOERR 9
EXC_ERR   10
EXC_ERR   11
EXC_ERR   12
EXC_ERR   13
EXC_ERR   14
EXC_NOERR 15
EXC_NOERR 16
EXC_ERR   17
EXC_NOERR 18
EXC_NOERR 19

exception_common:
    pusha
    cld
    push esp
    call exception_handler
    add esp, 4
    mov esp, eax        ; always switch to the task the handler picked
    popa
    iretd

section .data
global exception_stub_table
exception_stub_table:
    dd exc0_stub,  exc1_stub,  exc2_stub,  exc3_stub,  exc4_stub
    dd exc5_stub,  exc6_stub,  exc7_stub,  exc8_stub,  exc9_stub
    dd exc10_stub, exc11_stub, exc12_stub, exc13_stub, exc14_stub
    dd exc15_stub, exc16_stub, exc17_stub, exc18_stub, exc19_stub

; Small position-independent Ring 3 program.
; EAX=3 -> sys_yield. It intentionally produces no console output.
section .text.user_program
user_program_start:
    mov eax, 3
.user_loop:
    int 0x80
    jmp .user_loop
user_program_end:
