.section .text
.globl _start
.extern main

_start:
    mov (%esp), %eax
    lea 4(%esp), %ecx
    push %ecx
    push %eax
    call main
    add $8, %esp
    mov %eax, %esi
    mov $2, %eax
    int $0x80
1:
    mov $3, %eax
    int $0x80
    jmp 1b
