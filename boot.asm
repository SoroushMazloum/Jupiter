[org 0x7C00]
[bits 16]

KERNEL_OFFSET  equ 0x1000
KERNEL_SECTORS equ 16          ; 16 * 512 = 8 KB for the kernel

start:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    mov [boot_drive], dl       ; BIOS passes the boot drive in DL

    ; Load kernel from disk (sector 2 onward) to 0x0000:0x1000
    mov bx, KERNEL_OFFSET
    mov ah, 0x02               ; BIOS read sectors
    mov al, KERNEL_SECTORS
    mov ch, 0                  ; cylinder 0
    mov cl, 2                  ; start at sector 2 (sector 1 is this bootloader)
    mov dh, 0                  ; head 0
    mov dl, [boot_drive]
    int 0x13
    jc disk_error

    ; Set 80x25 text mode (also clears screen)
    mov ax, 0x0003
    int 0x10

    ; Enter protected mode
    cli
    lgdt [gdt_descriptor]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp CODE_SEG:init_pm

disk_error:
    jmp $

; ---- Global Descriptor Table ----
gdt_start:
    dq 0x0                              ; null descriptor
gdt_code:
    dw 0xFFFF, 0x0000
    db 0x00, 10011010b, 11001111b, 0x00 ; 32-bit code segment
gdt_data:
    dw 0xFFFF, 0x0000
    db 0x00, 10010010b, 11001111b, 0x00 ; 32-bit data segment
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

CODE_SEG equ gdt_code - gdt_start
DATA_SEG equ gdt_data - gdt_start

boot_drive db 0

[bits 32]
init_pm:
    mov ax, DATA_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000
    jmp KERNEL_OFFSET

times 510 - ($ - $$) db 0
dw 0xAA55                               ; boot signature