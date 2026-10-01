[bits 32]
[org 0]

; Reference source for the HELLO.JXE program.
; The current build uses tools/make_jexe.py so the project can still
; generate the filesystem image in environments without NASM.

start:
    mov eax, 1
    mov esi, 0x00400020
    mov ecx, message_end - message
    int 0x80
    mov eax, 2
    int 0x80
.hang:
    jmp .hang

times 0x20 - ($ - $$) db 0
message:
    db 'Hello from a JupiterOS userspace program!', 10
message_end:
